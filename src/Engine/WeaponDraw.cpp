#include "Engine/WeaponDraw.h"

#include "Config.h"
#include "Engine/AnimRecon.h"
#include "Engine/AudioProbe.h"
#include "Engine/Camera.h"
#include "Engine/CoverPose.h"
#include "Engine/Frame.h"
#include "Engine/Guard.h"
#include "Engine/Offsets.h"
#include "Engine/PlayerControl.h"

namespace Engine::WeaponDraw
{
	namespace
	{
		using Work_t = void (*)(void*);

		Work_t s_work{ nullptr };
		bool   s_checked{ false };
		bool   s_available{ false };
		bool   s_faulted{ false };
		int    s_calls{ 0 };
		int    s_giveUps{ 0 };

		std::uint32_t s_lastKey{ 0 };
		double        s_nextPoll{ 0.0 };
		bool          s_pending{ false };
		double        s_tKeyChange{ 0.0 };
		std::uint32_t s_pendingKey{ 0 };
		double        s_tReady{ -1.0 };      // when the Weapon node had lost its model and a holster node held one
		int           s_callsThisSwitch{ 0 };
		double        s_tFirstCall{ -1.0 };
		double        s_tLastCall{ -1.0 };
		bool          s_reevalDone{ false };
		int           s_switches{ 0 };
		double        s_blockedAt{ -1.0 };      // an Equip event was blocked at this time (W5)
		int           s_stateCalls{ 0 };
		double        s_lastStateCall{ -10.0 };

		// the snapshots after a call: due times and the label
		struct Due
		{
			double      t;
			const char* label;
			bool        done;
		};
		std::array<Due, 4> s_due{};
		double             s_tCall{ -1.0 };

		[[nodiscard]] std::uintptr_t ModuleBase() { return REX::FModule::GetExecutingModule().GetBaseAddress(); }

		// the skeleton root: the ancestor of the camera pivot named HumanExportRoot (as the rig does)
		[[nodiscard]] const RE::NiAVObject* SkeletonRoot()
		{
			const auto rig = Camera::ReadRig();
			if (!rig.ok || !rig.n200.ptr) {
				return nullptr;
			}
			const RE::NiAVObject* cur = static_cast<const RE::NiAVObject*>(rig.n200.ptr);
			for (int i = 0; i < 40 && cur; ++i) {
				const char* s = cur->name.c_str();
				if (s && std::strcmp(s, "HumanExportRoot") == 0) {
					return cur;
				}
				cur = cur->parent;
			}
			return nullptr;
		}

		// the children of a node: the array of objects whose parent field (+0x38) is the node, found in the node's memory from +0x130 (the way the rig lists them)
		[[nodiscard]] int ChildCount(const void* a_node)
		{
			if (!a_node) {
				return -1;
			}
			int           count = -1;
			unsigned long code{ 0 };
			(void)Guarded([&] {
				const auto base = reinterpret_cast<const char*>(a_node);
				for (std::ptrdiff_t off = 0x130; off < 0x1F8; off += 8) {
					const void* v{ nullptr };
					if (!SafeRead(base + off, v) || !v || (reinterpret_cast<std::uintptr_t>(v) & 7) != 0) {
						continue;
					}
					int run = 0;
					for (int k = 0; k < 96; ++k) {
						const void* c{ nullptr };
						const void* par{ nullptr };
						if (!SafeRead(reinterpret_cast<const char*>(v) + k * 8, c) || !c || (reinterpret_cast<std::uintptr_t>(c) & 7) != 0 || !SafeRead(reinterpret_cast<const char*>(c) + 0x38, par) || par != a_node) {
							break;
						}
						++run;
					}
					if (run > 0) {
						count = run;
						return;
					}
				}
				count = 0;
			}, code);
			return count;
		}

		[[nodiscard]] int NodeChildren(const char* a_name)
		{
			int           n = -2;
			unsigned long code{ 0 };
			(void)Guarded([&] {
				const auto root = SkeletonRoot();
				if (!root) {
					return;
				}
				const RE::BSFixedString fs{ a_name };
				auto*                   node = const_cast<RE::NiAVObject*>(root)->GetObjectByName(fs);
				n = node ? ChildCount(node) : -3;
			}, code);
			return n;
		}

		[[nodiscard]] const std::byte* MiddleHigh()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return nullptr;
			}
			RE::AIProcess* proc{ nullptr };
			if (!SafeRead(&player->currentProcess, proc) || !proc) {
				return nullptr;
			}
			RE::MiddleHighProcessData* mh{ nullptr };
			if (!SafeRead(&proc->middleHigh, mh) || !mh) {
				return nullptr;
			}
			return reinterpret_cast<const std::byte*>(mh);
		}

		void Snapshot(const char* a_label)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			std::uint32_t s1{ 0 }, s2{ 0 };
			if (player) {
				unsigned long code{ 0 };
				(void)Guarded([&] { s1 = static_cast<std::uint32_t>(player->actorState1); s2 = static_cast<std::uint32_t>(player->actorState2); }, code);
			}
			std::uint8_t b584{ 0xFF }, b585{ 0xFF };
			if (const auto mh = MiddleHigh()) {
				(void)SafeRead(mh + kMhDrawFlagsByte, b584);
				(void)SafeRead(mh + kMhDrawFlagsByte + 1, b585);
			}
			// the object behind the global at RVA 0x5F43230 and its dword at +0x10EC (what the handler tests): a safe read, to learn what it is
			std::uint64_t obj{ 0 };
			std::uint32_t d10EC{ 0xFFFFFFFF };
			(void)SafeRead(reinterpret_cast<const void*>(ModuleBase() + kPlayerObjectGlobalRva), obj);
			if (obj > 0x10000) {
				(void)SafeRead(reinterpret_cast<const void*>(obj + 0x10EC), d10EC);
			}
			COVER_DETAIL("WEAPONDRAW [{}] t={:+.3f} key=0x{:08X} aState=0x{:08X}/0x{:08X} ws={} mh+0x584=0x{:02X} mh+0x585=0x{:02X} cov={:g} stance={} pose={} eq={:g} uneq={:g} wds={:g} camera={} global={:#x} [+0x10EC]=0x{:08X} | Weapon:c{} WeaponLeft:c{} R_BackPackHolster:c{} _LowGrip:c{} _Aim:c{}", a_label, s_tCall > 0.0 ? Frame::Now() - s_tCall : 0.0,
				Engine::AudioProbe::CurrentWeapon(), s1, s2, CoverPose::WeaponState(), b584, b585, PlayerControl::ReadVar("bIsInCover").value_or(-9.0), CoverPose::StanceField(), CoverPose::ToString(CoverPose::GetInfo().state), PlayerControl::ReadVar("bIsEquipping").value_or(-9.0),
				PlayerControl::ReadVar("bIsUnequipping").value_or(-9.0), PlayerControl::ReadVar("iSyncWeaponDrawState").value_or(-9.0), Frame::Latest().cameraIndex, obj, d10EC, NodeChildren("Weapon"), NodeChildren("WeaponLeft"), NodeChildren("R_BackPackHolster"),
				NodeChildren("R_BackPackHolster_LowGrip"), NodeChildren("R_BackPackHolster_Aim"));
		}

		void CheckAvailable()
		{
			s_checked = true;
			const auto  addr = kWeaponDrawWorkId.address();
			std::array<std::uint8_t, kWeaponDrawWorkPrologue.size()> got{};
			if (SafeCopy(got.data(), reinterpret_cast<const void*>(addr), got.size()) != got.size()) {
				REX::ERROR("WEAPONDRAW: cannot read the first bytes of ID {}: not installed", kWeaponDrawWorkId.id());
				return;
			}
			std::string have;
			for (const auto b : got) {
				have += std::format("{:02X} ", b);
			}
			const bool same = got == kWeaponDrawWorkPrologue;
			REX::INFO("WEAPONDRAW: Address Library ID {} at RVA 0x{:X}: first bytes {}: {}", kWeaponDrawWorkId.id(), addr - ModuleBase(), have, same ? "the verified ones" : "NOT the verified bytes: nothing is installed");
			if (same) {
				s_work = reinterpret_cast<Work_t>(addr);
				s_available = true;
			}
		}

		// the Weapon node has lost its model AND a holster node holds one (a model that is not loaded yet may come late)
		[[nodiscard]] bool ModelOnHolster()
		{
			return NodeChildren("R_BackPackHolster") >= 1 || NodeChildren("R_BackPackHolster_LowGrip") >= 1 || NodeChildren("R_BackPackHolster_Alternate") >= 1;
		}

		void DoCall(const double a_now)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player || !s_work) {
				return;
			}
			if (s_callsThisSwitch == 0) {
				s_tFirstCall = a_now;
				s_tCall = a_now;
				Snapshot("just before the call");
				s_due = { { { a_now + 0.001, "next tick", false }, { a_now + 0.1, "0.1 s after", false }, { a_now + 0.5, "0.5 s after", false }, { a_now + 2.0, "2 s after", false } } };
			}
			unsigned long code{ 0 };
			if (!Guarded([&] { s_work(player); }, code)) {
				s_faulted = true;
				REX::ERROR("WEAPONDRAW: exception 0x{:08X} in the game's weapon-draw work: the draw call is off for this session", code);
				return;
			}
			++s_calls;
			++s_callsThisSwitch;
			s_tLastCall = a_now;
			COVER_DETAIL("WEAPONDRAW: the game's weapon-draw work (ID {}) was called with the player: call {} of this switch, {:.3f} s after the weapon key changed", kWeaponDrawWorkId.id(), s_callsThisSwitch, a_now - s_tKeyChange);
		}

		void Reevaluate(const double a_now)
		{
			s_reevalDone = true;
			bool          acc{ false };
			unsigned long code{ 0 };
			bool          sent{ false };
			if (!Guarded([&] { sent = AnimRecon::SendEvent("ReevaluateGraphState", acc); }, code)) {
				REX::ERROR("WEAPONDRAW: exception 0x{:08X} sending ReevaluateGraphState", code);
				return;
			}
			COVER_DETAIL("WEAPONDRAW: the event ReevaluateGraphState was sent to the player's graph {:.3f} s after the first call (sent {}, accepted {})", a_now - s_tFirstCall, sent, acc);
			Snapshot("after ReevaluateGraphState");
		}
	}

	void Tick(const double a_now, const bool a_posed)
	{
		const auto& cfg = Config::Get();
		if ((cfg.poseWeaponDraw == 0 && cfg.poseWeaponSwap != 3) || s_faulted) {
			return;
		}
		if (!s_checked) {
			CheckAvailable();
		}
		if (!s_available) {
			return;
		}
		// the snapshots after a call
		if (s_tCall > 0.0) {
			bool any = false;
			for (auto& d : s_due) {
				if (!d.done) {
					any = true;
					if (a_now >= d.t) {
						d.done = true;
						Snapshot(d.label);
					}
				}
			}
			if (!any) {
				s_tCall = -1.0;
			}
		}
		if (!a_posed) {
			if (s_lastKey != 0 || s_pending) {
				s_lastKey = 0;
				s_pending = false;
			}
			return;
		}
		if (s_blockedAt > 0.0 && !s_pending && a_now - s_blockedAt >= 0.05) {
			const int ws = CoverPose::WeaponState();
			if (ws == 3) {
				COVER_DETAIL("WEAPONDRAW: {:.3f} s after the blocked Equip the weapon state is 3: nothing to do ({} state call(s))", a_now - s_blockedAt, s_stateCalls);
				s_blockedAt = -1.0;
			} else if (a_now - s_blockedAt > 1.2 || s_stateCalls >= 3) {
				COVER_DETAIL("WEAPONDRAW: {:.3f} s after the blocked Equip the weapon state is still {} after {} state call(s): given up", a_now - s_blockedAt, ws, s_stateCalls);
				s_blockedAt = -1.0;
			} else if (a_now - s_lastStateCall >= 0.15) {
				const auto player = RE::PlayerCharacter::GetSingleton();
				unsigned long scode{ 0 };
				if (player && s_work && Guarded([&] { s_work(player); }, scode)) {
					++s_stateCalls;
					++s_calls;
					s_lastStateCall = a_now;
					COVER_DETAIL("WEAPONDRAW: the weapon state is {} {:.3f} s after the blocked Equip: the draw work is called (state call {})", ws, a_now - s_blockedAt, s_stateCalls);
				} else if (scode) {
					s_faulted = true;
					REX::ERROR("WEAPONDRAW: exception 0x{:08X} in the draw work (state call): off for this session", scode);
				}
			}
		}
		if (a_now < s_nextPoll) {
			return;
		}
		s_nextPoll = a_now + 0.02;
		unsigned long code{ 0 };
		bool          ok = Guarded([&] {
			if (!s_pending) {
				const auto key = Engine::AudioProbe::CurrentWeapon();
				if (key == 0) {
					return;
				}
				if (s_lastKey == 0) {
					s_lastKey = key;
					return;
				}
				if (key != s_lastKey) {
					++s_switches;
					COVER_DETAIL("WEAPONDRAW: switch {}: the weapon in the hands changed in the pose: 0x{:08X} -> 0x{:08X}; waiting for the Weapon node to lose its model and a holster node to hold one, then {:.2f} s", s_switches, s_lastKey, key, cfg.poseWeaponDrawDelay);
					s_lastKey = key;
					s_pendingKey = key;
					s_pending = true;
					s_tKeyChange = a_now;
					s_tReady = -1.0;
					s_callsThisSwitch = 0;
					s_tFirstCall = -1.0;
					s_tLastCall = -1.0;
					s_reevalDone = false;
					Snapshot("key changed");
				}
				return;
			}
			const double since = a_now - s_tKeyChange;
			if (s_callsThisSwitch == 0) {
				if (s_tReady < 0.0) {
					if (NodeChildren("Weapon") == 2 && ModelOnHolster()) {
						s_tReady = a_now;
					} else if (since > 1.5) {
						++s_giveUps;
						s_pending = false;
						COVER_DETAIL("WEAPONDRAW: switch {} gave up: {:.2f} s after the key the Weapon node has {} children and the holster nodes hold {}", s_switches, since, NodeChildren("Weapon"), ModelOnHolster() ? "a model" : "no model");
						Snapshot("gave up");
					}
				} else if (a_now - s_tReady >= static_cast<double>(cfg.poseWeaponDrawDelay)) {
					DoCall(a_now);
				}
				return;
			}
			// after the first call: the optional graph event, the look 0.2 s after each call, a new call when the model is not in the hands, at most five calls in 1.5 s
			if (cfg.poseDrawReevaluate == 1 && !s_reevalDone && a_now - s_tFirstCall >= 0.1) {
				Reevaluate(a_now);
			}
			const int weaponKids = NodeChildren("Weapon");
			if (weaponKids == 3) {
				COVER_DETAIL("WEAPONDRAW: switch {}: the model is in the hands (Weapon has three children) {:.3f} s after the weapon key changed, {:.3f} s after the first call; {} call(s) for this switch", s_switches, a_now - s_tKeyChange, a_now - s_tFirstCall, s_callsThisSwitch);
				s_pending = false;
			} else if (a_now - s_tLastCall >= 0.2) {
				if (s_callsThisSwitch < 5 && a_now - s_tFirstCall < 1.5) {
					COVER_DETAIL("WEAPONDRAW: switch {}: 0.2 s after call {} the Weapon node still has {} children: calling again", s_switches, s_callsThisSwitch, weaponKids);
					DoCall(a_now);
				} else {
					++s_giveUps;
					s_pending = false;
					COVER_DETAIL("WEAPONDRAW: switch {} gave up after {} call(s) in {:.2f} s: the Weapon node has {} children", s_switches, s_callsThisSwitch, a_now - s_tFirstCall, weaponKids);
					Snapshot("gave up after the calls");
				}
			}
		}, code);
		if (!ok) {
			s_faulted = true;
			REX::ERROR("WEAPONDRAW: exception 0x{:08X} in the tick: the draw call is off for this session", code);
		}
	}

	Info GetInfo()
	{
		return { s_available, s_faulted, s_calls, s_giveUps };
	}

	void NoteEquipBlocked(const double a_now)
	{
		s_blockedAt = a_now;
		s_stateCalls = 0;
		s_lastStateCall = -10.0;
	}

	void LogState(const char* a_label)
	{
		unsigned long code{ 0 };
		(void)Guarded([&] { Snapshot(a_label); }, code);
	}
}
