#include "Engine/CoverPose.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cfloat>
#include <mutex>
#include <optional>

#include "Config.h"
#include "Core/EntryWait.h"
#include "Engine/AnimRecon.h"
#include "Engine/Camera.h"
#include "Engine/Frame.h"
#include "Engine/Guard.h"
#include "Engine/Input.h"
#include "RE/B/BSInputEnableLayer.h"
#include "Engine/Offsets.h"
#include "Engine/PlayerControl.h"

namespace Engine::CoverPose
{
	namespace
	{
		constexpr float kPi = 3.14159265358979f;

		std::mutex            s_mutex;  // the info is read by the rig from the same tick thread family; kept simple
		Info                  s_info;
		State&                s_state = s_info.state;
		bool                  s_faulted{ false };
		Core::CoverCandidate  s_cand;
		RE::NiPoint3          s_boom{}, s_pivot{};
		double                s_tSwitch{ 0.0 };
		double                s_tCall{ 0.0 };
		double                s_tLeave{ 0.0 };
		double                s_tRestore{ 0.0 };
		double                s_tFallback{ 0.0 };
		int                   s_fallbackStage{ 0 };  // 0 none, 1 holstered, 2 drawn again
		double                s_nextStart{ 0.0 };
		bool                  s_holdOn{ false };
		bool                  s_immediate{ false };
		int                   s_restoreAttempt{ 0 };
		double                s_tHoldOff{ -1.0 };  // when the hold went off during the restore (the rig node check follows)
		bool                  s_holstered{ false };
		bool                  s_drawn{ false };
		bool                  s_weaponWarned{ false };
		bool                  s_a2Phase{ false };      // modes 1 and 2: waiting for the boom node to read the own graph after the exit clip
		double                s_tA2{ 0.0 };
		std::optional<float>  s_exitY0;         // card M4b: the graph's boom value just after the leave (the NPC graph's -0.5187); a graph that does not update leaves it alone
		bool                  s_remedyDone{ false };
		double                s_nextTrace{ 0.0 };
		bool                  s_archEarly{ false };     // mode 2: the own archetype was asked for right after the stance call
		// T5.3: the four own-graph rig value sets and the pose's easing between them
		struct RigSet
		{
			bool         seen{ false };
			RE::NiPoint3 boom{}, pivot{};
		};
		RigSet       s_sets[4];
		bool         s_setsInit{ false };
		int          s_sampleSit{ -1 };
		RE::NiPoint3 s_sampleBoom{}, s_samplePivot{};
		double       s_sampleSince{ 0.0 };
		double       s_nextSample{ 0.0 };
		int          s_curSit{ -1 };
		int          s_fromSit{ -1 };
		RE::NiPoint3 s_curBoom{}, s_curPivot{}, s_fromBoom{}, s_fromPivot{};
		double       s_tSitChange{ 0.0 };
		bool         s_drawAsked{ false };
		double                s_drawAskedAt{ 0.0 };
		double                s_lastDrawAt{ -100.0 };  // when the pose last asked for the weapon to be drawn
		bool                  s_haveSample{ false };  // the own camera rig read while waiting to begin: the values must be steady (a crouch or a wall can still be moving the camera)
		float                 s_lastRig[3]{};
		double                s_stableSince{ 0.0 };
		double                s_waitSince{ -1.0 };
		// T5.6: the weapon controls
		std::atomic<bool>     s_swapReq{ false };
		std::atomic<bool>     s_wheelOpen{ false };
		std::atomic<double>   s_wheelT{ 0.0 };
		std::atomic<bool>     s_swapHoldA{ false };
		std::atomic<double>   s_reloadUntil{ 0.0 };
		double                s_reloadTapAt{ -1.0 };  // after a reload of the engine's own that ended the pose: the reload tap is made when the weapon is steady
		double                s_reloadTapWs3{ -1.0 };
		char                  s_swapWhy[48]{};
		double                s_ws3Since{ -1.0 };
		double                s_wsBadSince{ -1.0 };

		[[nodiscard]] double Now() { return Frame::Now(); }

		// ---- the process data's stance field ---------------------------------------------------------------------------------------------------
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

		[[nodiscard]] double Var(const char* a_name, const double a_missing)
		{
			const auto v = PlayerControl::ReadVar(a_name);
			return v ? *v : a_missing;
		}

		[[nodiscard]] bool VarResolves(const char* a_name) { return PlayerControl::ReadVar(a_name).has_value(); }

		// the process data's gun-down field (REFERENCE 21: 0 with the weapon drawn)
		[[nodiscard]] bool WeaponUp()
		{
			const auto b = MiddleHigh();
			std::int32_t v{ -1 };
			return b && SafeRead(b + kMhIsGunDown, v) && v == 0;
		}

		// the boom node's local y: about -1.9 on the own graph, -0.52 on the NPC graph (REFERENCE 21.2)
		[[nodiscard]] std::optional<float> BoomY()
		{
			if (s_holdOn) {
				if (const auto g = Camera::GraphBoomY()) {
					return g;  // the hold overwrites the node: what the graph itself wrote this frame is the answer
				}
			}
			const auto r = Camera::ReadRig();
			if (!r.ok || !r.n1F8.present) {
				return std::nullopt;
			}
			return r.n1F8.t.y;
		}

		void InitSets()
		{
			if (s_setsInit) {
				return;
			}
			s_setsInit = true;
			const RE::NiPoint3 boom{ 0.0194f, -1.94f, -0.0262f };
			const RE::NiPoint3 standing{ 0.457f, 0.056f, 1.454f };
			const RE::NiPoint3 sneaking{ 0.457f, 0.0125f, 0.94f };
			s_sets[0] = { false, boom, standing };
			s_sets[2] = { false, boom, sneaking };
			s_sets[1] = s_sets[0];  // aiming sets fall back to their not-aiming ones until seen
			s_sets[3] = s_sets[2];
		}

		[[nodiscard]] const RigSet& SetFor(const int a_sit)
		{
			InitSets();
			if ((a_sit == 1 || a_sit == 3) && !s_sets[a_sit].seen) {
				return s_sets[a_sit - 1];
			}
			return s_sets[a_sit];
		}

		// the pose's situation: low cover idle, walk and blind fire sneaking, low cover while aiming (the over aim stands the character up) standing and aiming, tall cover idle and blind fire standing, tall aiming standing and aiming
		[[nodiscard]] int PoseSituation()
		{
			const bool aiming = Var("iIsSighted", 0.0) > 0.5;
			if (s_cand.kind == Core::CoverKind::Low) {
				return aiming ? 1 : 2;
			}
			return aiming ? 1 : 0;
		}

		// moves the hold toward the set of the current situation, eased over 0.25 s
		void StepHold(const double a_now, const bool a_reset)
		{
			if (!s_holdOn) {
				return;
			}
			const int  sit = PoseSituation();
			const auto& target = SetFor(sit);
			if (a_reset || s_curSit < 0) {
				s_curSit = sit;
				s_fromSit = sit;
				s_fromBoom = s_curBoom = s_boom;
				s_fromPivot = s_curPivot = s_pivot;
				s_tSitChange = a_now;
			} else if (sit != s_curSit) {
				s_fromBoom = s_curBoom;
				s_fromPivot = s_curPivot;
				s_fromSit = s_curSit;
				s_curSit = sit;
				s_tSitChange = a_now;
				COVER_DETAIL("COVER POSE #{}: the camera hold moves to the {} set (boom y {:+.3f}, pivot z {:+.3f}, {})", s_info.sessions, sit == 0 ? "standing" : (sit == 1 ? "standing and aiming" : (sit == 2 ? "sneaking" : "sneaking and aiming")), target.boom.y, target.pivot.z, target.seen ? "seen on the own graph" : "default");
			}
			const float u = std::clamp(static_cast<float>((a_now - s_tSitChange) / 0.25), 0.0f, 1.0f);
			const float e = u * u * (3.0f - 2.0f * u);
			auto        lerp = [e](const RE::NiPoint3& a, const RE::NiPoint3& b) { return RE::NiPoint3{ a.x + (b.x - a.x) * e, a.y + (b.y - a.y) * e, a.z + (b.z - a.z) * e }; };
			s_curBoom = lerp(s_fromBoom, target.boom);
			s_curPivot = lerp(s_fromPivot, target.pivot);
			Camera::SetRigHoldValues(s_curBoom, s_curPivot);
		}

		[[nodiscard]] int Mode() { return Config::Get().poseArchetypeMode; }

		// the archetype-pending byte (A2)
		[[nodiscard]] int ReadEquipByte()
		{
			const auto   b = MiddleHigh();
			std::uint8_t v{ 0 };
			return b && SafeRead(b + kMhEquipPendingByte, v) ? static_cast<int>(v) : -1;
		}
		[[nodiscard]] int ReadByte()
		{
			const auto   b = MiddleHigh();
			std::uint8_t v{ 0 };
			return b && SafeRead(b + kMhArchetypePending, v) ? static_cast<int>(v) : -1;
		}

		void LogByte(const char* a_step)
		{
			COVER_DETAIL("COVER POSE #{}: archetype pending byte = {} ({})", s_info.sessions, ReadByte(), a_step);
		}

		// writes 0 into the byte, logging the value before and after; false when the write faulted
		// T5.8: the aim's pitch clamps of the process data written to -1.0 / +1.0 rad (the heading clamps are left as they are); logged the first time in each pose
		// T5.9: the favourites lock (the engine's own input-enable layer)
		RE::BSInputEnableLayer* s_layer{ nullptr };
		bool                    s_favOff{ false };
		bool                    s_layerFault{ false };
		void SetFavoritesLock(const bool a_lock, const char* a_why)
		{
			if (s_layerFault || a_lock == s_favOff) {
				return;
			}
			unsigned long code{ 0 };
			bool          failed{ false };
			const bool    ok = Guarded([&] {
                const auto mgr = RE::BSInputEnableManager::GetSingleton();
                if (!mgr) {
                    failed = true;
                    return;
                }
                if (!s_layer && !mgr->AllocateNewLayer(&s_layer, "CoverSF") ) {
                    failed = true;
                    return;
                }
                if (!s_layer) {
                    failed = true;
                    return;
                }
                const auto before = static_cast<std::uint32_t>(mgr->m_cachedOtherEventFlags);
                s_layer->EnableOtherEvent(RE::OTHER_EVENT_FLAG::Favorites, !a_lock);
                COVER_DETAIL("COVER POSE: the favourites {} ({}): layer {}, the manager's cached other-event flags 0x{:X} -> 0x{:X} (Favorites is bit 5)", a_lock ? "SWITCHED OFF" : "switched on again", a_why, s_layer->GetLayerID(), before,
                    static_cast<std::uint32_t>(mgr->m_cachedOtherEventFlags));
            }, code);
			if (!ok || failed) {
				s_layerFault = true;
				REX::ERROR("COVER POSE: the input-enable layer failed ({}, exception 0x{:08X}): iPoseWeaponSwap 0 has no effect this session", failed ? "no manager or layer" : "fault", code);
				return;
			}
			s_favOff = a_lock;
		}

		bool s_pitchLogged{ false };
		bool WritePitchClamps(const char* a_why)
		{
			const auto b = MiddleHigh();
			if (!b) {
				return false;
			}
			float before[2]{ 0.0f, 0.0f };
			(void)SafeRead(b + kMhDirectAtPitchMin, before[0]);
			(void)SafeRead(b + kMhDirectAtPitchMax, before[1]);
			unsigned long code{ 0 };
			const bool    ok = Guarded([&] {
                *reinterpret_cast<float*>(const_cast<std::byte*>(b) + kMhDirectAtPitchMin) = -1.0f;
                *reinterpret_cast<float*>(const_cast<std::byte*>(b) + kMhDirectAtPitchMax) = 1.0f;
            }, code);
			if (!ok) {
				REX::ERROR("COVER POSE: exception 0x{:08X} writing the pitch clamps", code);
				return false;
			}
			if (!s_pitchLogged) {
				s_pitchLogged = true;
				COVER_DETAIL("COVER POSE #{}: the pitch clamps written ({}): min {:+.3f} -> -1.000, max {:+.3f} -> +1.000", s_info.sessions, a_why, before[0], before[1]);
			}
			return true;
		}

		bool WriteStanceField(const std::int32_t a_value)
		{
			const auto b = MiddleHigh();
			if (!b) {
				return false;
			}
			unsigned long code{ 0 };
			const bool    ok = Guarded([&] { *reinterpret_cast<std::int32_t*>(const_cast<std::byte*>(b) + kMhActorStance) = a_value; }, code);
			if (!ok) {
				REX::ERROR("COVER POSE: exception 0x{:08X} writing the stance field", code);
			}
			return ok;
		}

		bool ClearByte(const char* a_step)
		{
			const auto b = MiddleHigh();
			if (!b) {
				return false;
			}
			const int     before = ReadByte();
			unsigned long code{ 0 };
			const bool    ok = Guarded([&] { *reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(b) + kMhArchetypePending) = 0; }, code);
			if (!ok) {
				REX::ERROR("COVER POSE: exception 0x{:08X} writing the archetype pending byte", code);
				return false;
			}
			COVER_DETAIL("COVER POSE #{}: archetype pending byte {} -> {} ({})", s_info.sessions, before, ReadByte(), a_step);
			return true;
		}

		// 23.32 (rig only, iPoseEntryProbe 1): one line a frame while the pose is entering, so that a log shows when the graph takes the archetype change
		// and what the second call changes. Reads only; every read is guarded.
		void EntryProbe(const double a_now, const char* a_tag)
		{
			if (Config::Get().poseEntryProbe == 0) {
				return;
			}
			const auto    b = MiddleHigh();
			std::uint8_t  b585{ 0 }, b586{ 0 };
			const bool    r585 = b && SafeRead(b + kMhArchetypePending, b585);
			const bool    r586 = b && SafeRead(b + kMhEquipPendingByte, b586);
			const auto    player = RE::PlayerCharacter::GetSingleton();
			std::uint32_t as2{ 0 };
			const bool    ras2 = player && SafeRead(&player->actorState2, as2);
			const auto    rig = Camera::ReadRig();
			COVER_DETAIL("POSE PROBE #{} +{:.3f} s after the clear [{}]: state {}, byte585 {}, byte586 {}, bIsExitingCover {:g}, bIsInCover {:g}, boom y {}, actorState2 {}, weapon state {}, bIsEquipping {:g}, stance field {}, calls {}", s_info.sessions, a_now - s_tSwitch, a_tag, ToString(s_state),
				r585 ? static_cast<int>(b585) : -1, r586 ? static_cast<int>(b586) : -1, Var("bIsExitingCover", -1.0), Var("bIsInCover", -1.0), rig.ok && rig.n1F8.present ? std::format("{:+.4f}", rig.n1F8.t.y) : std::string("unreadable"),
				ras2 ? std::format("0x{:08X}", as2) : std::string("unreadable"), ras2 ? static_cast<int>(as2 & 7u) : -1, Var("bIsEquipping", -1.0), StanceField(), s_info.stanceCalls);
		}

		// 23.32: whether the cell the player stands in is an interior (the first pose of the user's play in a different cell read byte 2 from then on); logged at the begin when the probe or the wait is on
		void LogCellKind()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}
			unsigned long code{ 0 };
			int           interior{ -1 };
			(void)Guarded([&] {
				const auto cell = player->parentCell;
				interior = cell ? (cell->IsInterior() ? 1 : 0) : -1;
			}, code);
			COVER_DETAIL("COVER POSE #{}: the cell is {} ({})", s_info.sessions, interior == 1 ? "an INTERIOR" : (interior == 0 ? "an exterior" : "unreadable"), code ? "faulted" : "read");
		}

		// the boom node and the byte every 0.1 s for 3 s after the exit function (modes 1 and 2)
		void Trace(const double a_now)
		{
			if (Mode() == 0 || s_tLeave <= 0.0 || a_now > s_tLeave + 3.0 || a_now < s_nextTrace) {
				return;
			}
			s_nextTrace = a_now + 0.1;
			const auto y = BoomY();
			COVER_DETAIL("COVER POSE #{} trace +{:.2f} s after the exit function: boom y {}, pending byte {}, stance field {}, bIsInCover {:g}, bIsExitingCover {:g}, state {}", s_info.sessions, a_now - s_tLeave, y ? std::format("{:+.4f}", *y) : std::string("unreadable"), ReadByte(), StanceField(),
				Var("bIsInCover", -1.0), Var("bIsExitingCover", -1.0), ToString(s_state));
		}

		// ---- the engine's functions, each called only after its ID resolved to the verified RVA and its first bytes matched (REFERENCE 20, 21) ------
		[[nodiscard]] bool StanceFnVerified()
		{
			static int s_checked = 0;
			if (s_checked == 0) {
				const auto base = REX::FModule::GetExecutingModule().GetBaseAddress();
				const auto addr = kActorChangeStance.address();
				std::array<std::uint8_t, 24> bytes{};
				const bool readable = SafeCopy(bytes.data(), reinterpret_cast<const void*>(addr), bytes.size()) == bytes.size();
				const bool ok = (addr - base) == kActorChangeStanceRva && readable && bytes == kActorChangeStanceBytes;
				s_checked = ok ? 1 : -1;
				COVER_DETAIL("COVER POSE: the stance function, ID {} resolves to RVA 0x{:X} (expected 0x{:X}), bytes {} -> {}", kActorChangeStance.id(), addr - base, kActorChangeStanceRva, readable ? (bytes == kActorChangeStanceBytes ? "match" : "DIFFER") : "UNREADABLE",
					ok ? "OK" : "REFUSED, the poses stay off");
			}
			return s_checked == 1;
		}

		[[nodiscard]] bool ExitFnVerified()
		{
			static int s_checked = 0;
			if (s_checked == 0) {
				const auto base = REX::FModule::GetExecutingModule().GetBaseAddress();
				const auto addr = kActorExitCover.address();
				std::array<std::uint8_t, 26> bytes{};
				const bool readable = SafeCopy(bytes.data(), reinterpret_cast<const void*>(addr), bytes.size()) == bytes.size();
				const bool ok = (addr - base) == kActorExitCoverRva && readable && bytes == kActorExitCoverBytes;
				s_checked = ok ? 1 : -1;
				COVER_DETAIL("COVER POSE: the exit function, ID {} resolves to RVA 0x{:X} (expected 0x{:X}), bytes {} -> {}", kActorExitCover.id(), addr - base, kActorExitCoverRva, readable ? (bytes == kActorExitCoverBytes ? "match" : "DIFFER") : "UNREADABLE",
					ok ? "OK" : "REFUSED, the poses stay off");
			}
			return s_checked == 1;
		}

		// a_loc null: "here". False when the function did not run (refused or faulted); a fault is reported to the caller by a_faulted.
		bool CallStance(const int a_stance, const bool a_side, const RE::NiPoint3* a_loc, const float a_heading, bool& a_returned, bool& a_faulted)
		{
			a_returned = false;
			a_faulted = false;
			if (!StanceFnVerified()) {
				return false;
			}
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return false;
			}
			static const RE::NiPoint3 here{ FLT_MAX, FLT_MAX, FLT_MAX };  // the engine compares the values, not the pointer
			unsigned long             code{ 0 };
			bool                      ret{ false };
			const bool                ok = Guarded(
                [&] {
                    using func_t = bool (*)(RE::Actor*, std::int32_t, bool, bool, const RE::NiPoint3*, float);
                    const auto fn = reinterpret_cast<func_t>(kActorChangeStance.address());
                    ret = fn(player, a_stance, a_side, false, a_loc ? a_loc : &here, a_heading);
                },
                code);
			if (!ok) {
				a_faulted = true;
				REX::ERROR("COVER POSE: exception 0x{:08X} in the stance function", code);
				return false;
			}
			a_returned = ret;
			return true;
		}

		bool CallExit(bool& a_returned, bool& a_faulted)
		{
			a_returned = false;
			a_faulted = false;
			if (!ExitFnVerified()) {
				return false;
			}
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return false;
			}
			unsigned long code{ 0 };
			bool          ret{ false };
			const bool    ok = Guarded(
				[&] {
					using func_t = bool (*)(RE::Actor*);
					const auto fn = reinterpret_cast<func_t>(kActorExitCover.address());
					ret = fn(player);
				},
				code);
			if (!ok) {
				a_faulted = true;
				REX::ERROR("COVER POSE: exception 0x{:08X} in the exit function", code);
				return false;
			}
			a_returned = ret;
			return true;
		}

		void HoldOff()
		{
			if (s_holdOn) {
				Camera::SetRigHold(false, {}, {});
				s_holdOn = false;
			}
		}

		// everything back, now, whatever state it is in. Used by a hard reset and by a fault.
		void RestoreAll(const char* a_why, const bool a_fault)
		{
			const double now = Now();
			SetFavoritesLock(false, a_why);
			COVER_DETAIL("COVER POSE: {} -> the exit function, the own archetype, the hold and the camera offset back now (state was {})", a_why, ToString(s_state));
			bool ret{ false }, faulted{ false };
			if (StanceField() >= 2) {
				(void)CallExit(ret, faulted);
				COVER_DETAIL("COVER POSE: the exit function returned {} (ran {}), the stance field reads {}", ret, !faulted, StanceField());
			}
			bool restored{ false };
			if (AnimRecon::ArchetypeChanged()) {
				const bool ran = AnimRecon::RestoreArchetype(restored);
				COVER_DETAIL("COVER POSE: the own archetype asked for: ran {}, returned {}", ran, restored);
			}
			s_holstered = s_drawn = false;
			if (Mode() == 0 && Config::Get().poseHolsterAt <= 0.0f) {
				(void)AnimRecon::DrawWeapon(false);
				s_holstered = true;
			}
			if (Mode() != 0 && ReadByte() > 0) {
				(void)ClearByte("hard reset or fault");
			}
			HoldOff();
			s_tHoldOff = now;
			Camera::CoverCamReset();
			s_info.tLeave = s_info.tLeave < 0.0 ? now : s_info.tLeave;
			s_info.tRestoreCall = now;
			if (a_fault) {
				s_faulted = true;
				Config::Mutable().coverPoses = false;
				s_info.faulted = true;
				s_state = State::Off;
				REX::ERROR("COVER POSE: switched off for this session after a fault: {}", a_why);
				return;
			}
			s_state = State::Restoring;
			s_tRestore = now;
			s_fallbackStage = 0;
			s_restoreAttempt = 0;
			s_a2Phase = Mode() != 0;
			s_tA2 = now;
			if (s_info.tLeave > 0.0) {
				s_tLeave = s_info.tLeave;
			}
		}

		void Begin(const Inputs& a_in)
		{
			const auto rig = Camera::ReadRig();
			if (!rig.ok || !rig.n1F8.present || !rig.n200.present) {
				REX::WARN("COVER POSE: the camera rig could not be read; no pose now");
				s_nextStart = a_in.now + 1.0;
				return;
			}
			if ((Config::Get().poseDiagMask & 16) == 0 && WeaponState() != 3) {
				if (!s_weaponWarned) {
					s_weaponWarned = true;
					COVER_DETAIL("COVER POSE: the weapon is not drawn: no pose (the plain Phase 3 cover stays)");
				}
				s_nextStart = a_in.now + 1.0;
				return;
			}
			s_weaponWarned = false;
			s_swapHoldA.store(false);
			if (!StanceFnVerified() || !ExitFnVerified()) {
				s_faulted = true;
				Config::Mutable().coverPoses = false;
				s_info.faulted = true;
				REX::ERROR("COVER POSE: an engine function is refused; the poses are off for this session");
				return;
			}
			s_cand = *a_in.candidate;
			s_boom = rig.n1F8.t;
			s_pivot = rig.n200.t;
			{
				// what the graph wrote, without the cover camera's offset (it is added on top of the hold: y gets -back, z gets +up)
				const auto cc = Camera::GetCoverCam();
				s_boom.y += cc.backNow;
				s_pivot.z -= cc.upNow;
			}
			s_info = Info{ .state = State::Switching, .faulted = s_info.faulted, .sessions = s_info.sessions + 1, .restored = s_info.restored, .unconfirmed = s_info.unconfirmed, .stanceCalls = 0 };
			s_info.tBegin = a_in.now;
			s_tSwitch = a_in.now;
			s_fallbackStage = 0;
			s_a2Phase = false;
			s_archEarly = false;
			s_tLeave = 0.0;
			COVER_DETAIL("COVER POSE #{}: begin at {:.3f}: the own rig boom ({:+.4f},{:+.4f},{:+.4f}) pivot ({:+.4f},{:+.4f},{:+.4f}) is held, the archetype is cleared; {} cover, stand point ({:.3f},{:.3f}), wall normal ({:+.2f},{:+.2f}), ends {:.2f} m ({}) left and {:.2f} m ({}) right",
				s_info.sessions, a_in.now, s_boom.x, s_boom.y, s_boom.z, s_pivot.x, s_pivot.y, s_pivot.z, s_cand.kind == Core::CoverKind::Low ? "LOW" : "HIGH", s_cand.standPoint.x, s_cand.standPoint.y, s_cand.normal.x, s_cand.normal.y,
				s_cand.edgeLeft, Core::ToString(s_cand.edgeLeftKind), s_cand.edgeRight, Core::ToString(s_cand.edgeRightKind));
			COVER_DETAIL("{}", Camera::DescribeRig("pose begin (rig as read)", rig));
			Camera::SetRigHold(true, s_boom, s_pivot);
			s_holdOn = true;
			s_curSit = -1;
			Camera::BeginEntryBlend(Config::Get().poseEntryBlend);
			bool       ret{ false };
			LogByte("before ChangeAnimArchetype(player, null)");
			const bool ran = AnimRecon::ChangeArchetype(0, ret);
			COVER_DETAIL("COVER POSE #{}: ChangeAnimArchetype(player, null): ran {}, returned {}", s_info.sessions, ran, ret);
			LogByte("after ChangeAnimArchetype(player, null)");
			if (Config::Get().poseEntryProbe != 0 || Config::Get().poseWaitByte != 0) {
				LogCellKind();
			}
			EntryProbe(a_in.now, "begin");
			if (!ran) {
				RestoreAll("ChangeAnimArchetype did not run", true);
			}
		}

		// side false = the graph's Right: the pose for the right end seen facing the wall; the nearer open end decides, none open = Right
		[[nodiscard]] bool SideOf(const Core::CoverCandidate& a_c)
		{
			const bool rightOpen = a_c.edgeRightKind == Core::EdgeKind::Open;
			const bool leftOpen = a_c.edgeLeftKind == Core::EdgeKind::Open;
			if (rightOpen && (!leftOpen || a_c.edgeRight <= a_c.edgeLeft)) {
				return false;
			}
			if (leftOpen) {
				return true;
			}
			return false;
		}

		void DoStanceCall(const Inputs& a_in)
		{
			const int   stance = s_cand.kind == Core::CoverKind::Low ? 3 : 5;
			const bool  side = SideOf(s_cand);
			float       heading = std::atan2(-s_cand.normal.x, -s_cand.normal.y);  // actor angle z: 0 = +y, growing toward +x; the direction toward the wall
			if (heading < 0.0f) {
				heading += 2.0f * kPi;
			}
			const RE::NiPoint3 loc{ s_cand.standPoint.x, s_cand.standPoint.y, a_in.playerZ };
			bool               ret{ false }, faulted{ false };
			const bool         ran = CallStance(stance, side, &loc, heading, ret, faulted);
			++s_info.stanceCalls;
			s_info.lastStance = stance;
			s_info.lastSide = side;
			s_info.lastHeading = heading;
			s_info.lastLocation = loc;
			s_info.lastCallReturned = ret;
			if (s_info.tCall < 0.0) {
				s_info.tCall = a_in.now;
			}
			s_tCall = a_in.now;
			COVER_DETAIL("COVER POSE #{}: the stance call {} (stance {}, side {} = {}, location ({:.3f},{:.3f},{:.3f}), heading {:.4f} rad): ran {}, returned {}, {:.3f} s after the archetype was cleared", s_info.sessions, s_info.stanceCalls, stance, side, side ? "Left" : "Right",
				loc.x, loc.y, loc.z, heading, ran, ret, a_in.now - s_tSwitch);
			LogByte("after the stance call");
			if (faulted) {
				RestoreAll("exception in the stance function", true);
				return;
			}
			s_state = State::Entering;
		}

		void CallRestoreA2(double a_now);

		void StartLeave(const double a_now, const char* a_why, const bool a_immediate)
		{
			SetFavoritesLock(false, "the pose is leaving");
			COVER_DETAIL("COVER POSE #{}: leaving ({}), state was {}, the stance field reads {}", s_info.sessions, a_why, ToString(s_state), StanceField());
			s_info.tLeave = a_now;
			s_tLeave = a_now;
			s_nextTrace = a_now;
			s_exitY0.reset();
			s_remedyDone = false;
			if (Mode() != 0) {
				LogByte("before the exit function");
				if (!Config::Get().poseExitHold) {
					HoldOff();  // so that the camera boom node shows the graph itself during the exit (modes 1 and 2)
				} else {
					COVER_DETAIL("COVER POSE #{}: the camera hold stays on through the exit (bPoseExitHold 1); the graph's own boom value is read in the hook", s_info.sessions);
				}
			}
			if (Config::Get().poseExitTurn == 2) {
				float vyaw{ 0.0f };
				if (Camera::ReadViewYaw(vyaw)) {
					if (const auto pl = RE::PlayerCharacter::GetSingleton()) {
						unsigned long wc{ 0 };
						const float   before = pl->data.angle.z;
						(void)Guarded([&] { pl->data.angle.z = vyaw; }, wc);
						COVER_DETAIL("COVER POSE #{}: iPoseExitTurn 2: the body's angle z {:.1f} deg written to the view yaw {:.1f} deg before the exit function", s_info.sessions, before * 57.29578f, vyaw * 57.29578f);
					}
				}
			}
			bool ret{ false }, faulted{ false };
			if (Config::Get().poseExitClip == 1 && StanceField() >= 2) {
				// (d): no exit clip. The cover branch of the graph is left on CoverEnd, the stance field is what the exit function writes (0), the own archetype and the pending byte in the same tick
				bool       accepted{ false };
				const int  variant = Config::Get().poseExitVariant;
				bool       a2{ false }, a3{ false };
				if (variant == 2) {
					(void)AnimRecon::SendEvent("ExitCover", a2);
				}
				if (variant == 3) {
					bool r3{ false }, f3{ false };
					(void)CallExit(r3, f3);
					a2 = r3;
				}
				const bool sent = AnimRecon::SendEvent("CoverEnd", accepted);
				if (variant == 1 || variant == 5) {
					(void)AnimRecon::SendEvent("ExitCover", a2);
				}
				if (variant == 4 || variant == 5) {
					(void)AnimRecon::SendEvent("CoverEnd", a3);
				}
				if (variant != 0) {
					COVER_DETAIL("COVER POSE #{}: iPoseExitVariant {}: the extra events were accepted {} / {}", s_info.sessions, variant, a2, a3);
				}
				const bool wrote = WriteStanceField(0);
				COVER_DETAIL("COVER POSE #{}: iPoseExitClip 1: CoverEnd sent {} accepted {}, the stance field written 0 ({}), now reads {}; bIsInCover {:g}", s_info.sessions, sent, accepted, wrote, StanceField(), Var("bIsInCover", -1.0));
				s_state = State::Leaving;
				s_immediate = a_immediate;
				CallRestoreA2(a_now);
				return;
			}
			if (StanceField() >= 2) {
				const bool ran = CallExit(ret, faulted);
				COVER_DETAIL("COVER POSE #{}: the exit function: ran {}, returned {}, the stance field reads {} right after", s_info.sessions, ran, ret, StanceField());
				if (faulted) {
					RestoreAll("exception in the exit function", true);
					return;
				}
			}
			s_state = State::Leaving;
			s_immediate = a_immediate;
			if (Config::Get().poseExitTurn == 1 && Mode() != 0 && !a_immediate) {
				COVER_DETAIL("COVER POSE #{}: iPoseExitTurn 1: the own archetype is asked for in the same tick as the exit function", s_info.sessions);
				CallRestoreA2(a_now);
			}
		}

		void CallRestore(const double a_now)
		{
			bool       ret{ false };
			const bool ran = AnimRecon::RestoreArchetype(ret);
			s_info.tRestoreCall = a_now;
			s_tRestore = a_now;
			s_fallbackStage = 0;
			s_holstered = s_drawn = false;
			s_tHoldOff = -1.0;
			s_state = State::Restoring;
			COVER_DETAIL("COVER POSE #{}: the own archetype asked for {:.3f} s after the exit was called: ran {}, returned {}", s_info.sessions, a_now - s_tLeave, ran, ret);
			if (!ran) {
				RestoreAll("RestoreArchetype did not run", true);
				return;
			}
			if (Config::Get().poseHolsterAt <= 0.0f) {
				(void)AnimRecon::DrawWeapon(false);
				s_holstered = true;
				s_info.usedHolsterFallback = true;
			}
		}

		// modes 1 and 2: the exit clip is over. Mode 1 asks for the own archetype and clears the byte; mode 2 only checks the byte (the archetype has been back since the pose began)
		void CallRestoreA2(const double a_now)
		{
			s_info.tRestoreCall = a_now;
			s_tRestore = a_now;
			s_tA2 = a_now;
			s_a2Phase = true;
			s_state = State::Restoring;
			if (Mode() == 1) {
				bool       ret{ false };
				const bool ran = AnimRecon::RestoreArchetype(ret);
				COVER_DETAIL("COVER POSE #{}: the own archetype asked for {:.3f} s after the exit function: ran {}, returned {}", s_info.sessions, a_now - s_tLeave, ran, ret);
				LogByte("after the own archetype was asked for");
				if (!ran) {
					RestoreAll("RestoreArchetype did not run", true);
					return;
				}
				(void)ClearByte("mode 1, after the own archetype was asked for");
			} else {
				LogByte("mode 2, the exit clip is over");
				if (ReadByte() > 0) {
					(void)ClearByte("mode 2, found not 0 after the exit clip");
				}
			}
		}

		void Finish(const double a_now, const bool a_confirmed)
		{
			if (s_holdOn && Config::Get().poseExitHold && Config::Get().poseExitEase > 0.0f) {
				Camera::ReleaseRigHold(Config::Get().poseExitEase);  // eased to what the own graph gives, then off (REFERENCE 22.19)
				s_holdOn = false;
			}
			HoldOff();
			s_info.tOwnBack = a_now;
			s_state = State::Off;
			s_nextStart = a_now + (s_swapHoldA.load() ? 1.0 : 0.5);
			if (a_confirmed) {
				++s_info.restored;
			} else {
				++s_info.unconfirmed;
				REX::ERROR("COVER POSE #{}: the own graph was not confirmed (the camera boom node still reads the NPC graph's value); the hold is off anyway", s_info.sessions);
			}
			COVER_DETAIL("COVER POSE #{}: over: the own graph is {} {:.3f} s after the exit was called, {:.3f} s after the pose began, {} attempt(s); the stance field reads {}", s_info.sessions, a_confirmed ? "confirmed" : "NOT confirmed", a_now - s_tLeave, a_now - s_info.tBegin, s_restoreAttempt + 1, StanceField());
		}
	}

	const char* ToString(const State a_state) noexcept
	{
		switch (a_state) {
		case State::Off: return "Off";
		case State::Switching: return "Switching";
		case State::Entering: return "Entering";
		case State::Posed: return "Posed";
		case State::Leaving: return "Leaving";
		case State::Restoring: return "Restoring";
		}
		return "?";
	}

	Info GetInfo()
	{
		std::lock_guard lock(s_mutex);
		return s_info;
	}

	bool Enabled() noexcept
	{
		return Config::Get().coverPoses && !s_faulted;
	}

	bool Active() noexcept
	{
		return s_state == State::Switching || s_state == State::Entering || s_state == State::Posed;
	}

	bool Busy() noexcept
	{
		return s_state != State::Off;
	}

	double VarValue(const char* a_name)
	{
		return Var(a_name, -1.0);
	}

	RigSetInfo GetRigSets()
	{
		std::lock_guard lock(s_mutex);
		RigSetInfo      i;
		InitSets();
		for (int k = 0; k < 4; ++k) {
			i.seen[k] = s_sets[k].seen;
			i.boom[k] = s_sets[k].boom;
			i.pivot[k] = s_sets[k].pivot;
		}
		i.situation = Busy() ? s_curSit : -1;
		return i;
	}

	void SampleOwnRig(const double a_now, const bool a_eligible, const bool a_aiming, const bool a_sneaking)
	{
		if (a_now < s_nextSample) {
			return;
		}
		s_nextSample = a_now + 0.1;
		std::lock_guard lock(s_mutex);
		if (!a_eligible || Busy()) {
			s_sampleSit = -1;
			return;
		}
		const int sit = (a_sneaking ? 2 : 0) + (a_aiming ? 1 : 0);
		const auto r = Camera::ReadRig();
		if (!r.ok || !r.n1F8.present || !r.n200.present) {
			s_sampleSit = -1;
			return;
		}
		const bool same = sit == s_sampleSit && std::fabs(r.n1F8.t.y - s_sampleBoom.y) < 0.004f && std::fabs(r.n200.t.y - s_samplePivot.y) < 0.004f && std::fabs(r.n200.t.z - s_samplePivot.z) < 0.004f;
		if (!same) {
			s_sampleSit = sit;
			s_sampleSince = a_now;
		}
		s_sampleBoom = r.n1F8.t;
		s_samplePivot = r.n200.t;
		if (same && a_now - s_sampleSince >= 0.3) {
			InitSets();
			auto& set = s_sets[sit];
			const bool first = !set.seen;
			if (first || std::fabs(set.pivot.z - r.n200.t.z) > 0.01f || std::fabs(set.boom.y - r.n1F8.t.y) > 0.01f) {
				COVER_DETAIL("COVER POSE: own-graph rig set {} ({}) {}: boom ({:+.4f},{:+.4f},{:+.4f}) pivot ({:+.4f},{:+.4f},{:+.4f})", sit, sit == 0 ? "standing" : (sit == 1 ? "standing and aiming" : (sit == 2 ? "sneaking" : "sneaking and aiming")), first ? "seen" : "updated", r.n1F8.t.x, r.n1F8.t.y, r.n1F8.t.z,
					r.n200.t.x, r.n200.t.y, r.n200.t.z);
			}
			set.seen = true;
			set.boom = r.n1F8.t;
			set.pivot = r.n200.t;
		}
	}

	int WeaponState()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		std::uint32_t v{ 0 };
		return player && SafeRead(&player->actorState2, v) ? static_cast<int>(v & 7u) : -1;
	}

	int PendingByte()
	{
		return ReadByte();
	}

	bool ReadMiddleHighFloats(const std::ptrdiff_t a_offset, const int a_count, float* a_out)
	{
		const auto b = MiddleHigh();
		if (!b) {
			return false;
		}
		bool ok = true;
		for (int i = 0; i < a_count; ++i) {
			ok = SafeRead(b + a_offset + 4 * i, a_out[i]) && ok;
		}
		return ok;
	}

	bool ReadDirectAt(float (&a_out)[6], int& a_aimActive)
	{
		const auto b = MiddleHigh();
		if (!b) {
			return false;
		}
		const std::ptrdiff_t offs[6] = { kMhDirectAtHeadingVar, kMhDirectAtPitchVar, kMhDirectAtHeadingMin, kMhDirectAtHeadingMax, kMhDirectAtPitchMin, kMhDirectAtPitchMax };
		for (int i = 0; i < 6; ++i) {
			if (!SafeRead(b + offs[i], a_out[i])) {
				return false;
			}
		}
		std::uint8_t ab{ 0 };
		a_aimActive = SafeRead(b + kMhAimActive, ab) ? static_cast<int>(ab) : -1;
		return true;
	}

	int StanceField() {
		const auto b = MiddleHigh();
		std::int32_t v{ -1 };
		if (!b || !SafeRead(b + kMhActorStance, v)) {
			return -1;
		}
		return v;
	}

	bool ProbeStanceCall(bool& a_returned)
	{
		bool faulted{ false };
		const bool ran = CallStance(3, false, nullptr, -FLT_MAX, a_returned, faulted);
		if (ran && a_returned) {
			bool r2{ false }, f2{ false };
			(void)CallExit(r2, f2);  // on the NPC graph the call put the player into the cover state: undone at once
		}
		return ran;
	}

	Event Tick(const Inputs& a_in)
	{
		std::lock_guard lock(s_mutex);
		const double now = a_in.now;
		if (!Enabled()) {
			SetFavoritesLock(false, "the poses are off");
			if (s_state != State::Off) {
				RestoreAll(s_faulted ? "the poses are off after a fault" : "bCoverPoses was switched off", false);
			}
			if (s_state != State::Restoring) {
				return Event::None;
			}
		}
		if (s_state == State::Switching || s_state == State::Entering || s_state == State::Posed) {
			StepHold(now, false);
		}
		{
			// T5.6: the weapon controls and the weapon state
			const int ws = WeaponState();
			if (ws == 3) {
				if (s_ws3Since < 0.0) {
					s_ws3Since = now;
				}
				s_wsBadSince = -1.0;
			} else {
				s_ws3Since = -1.0;
				if (s_wsBadSince < 0.0) {
					s_wsBadSince = now;
				}
			}
			if (s_wheelOpen.load() && now - s_wheelT.load() > 8.0) {
				s_wheelOpen.store(false);
				COVER_DETAIL("COVER POSE: the favourites wheel: no event for 8 s, taken as closed");
			}
			if (a_in.machine == Core::CoverState::None && s_state == State::Off && s_swapHoldA.load()) {
				s_swapHoldA.store(false);  // out of cover: the next cover starts from scratch (draw first, then the pose)
			}
			// T5.7: the engine's own reload (the magazine ran empty, the control pressed before the hook saw it): bIsReloading (graph variable, found by the probe of run m3c-1) ends the pose at once
			if (Config::Get().poseReload && (s_state == State::Entering || s_state == State::Posed) && VarValue("bIsReloading") > 0.5) {
				s_swapHoldA.store(true);
				s_reloadUntil.store(now + static_cast<double>(Config::Get().poseReloadTime));
				s_reloadTapAt = now;
				s_reloadTapWs3 = -1.0;
				StartLeave(now, "the game's own reload began (bIsReloading 1)", false);
			}
			if (s_reloadTapAt > 0.0 && s_state == State::Off) {
				if (ws == 3) {
					if (s_reloadTapWs3 < 0.0) {
						s_reloadTapWs3 = now;
					}
				} else {
					s_reloadTapWs3 = -1.0;
				}
				if (s_reloadTapWs3 > 0.0 && now - s_reloadTapWs3 >= 0.25) {
					const double rl = VarValue("bIsReloading");
					COVER_DETAIL("COVER POSE: the pose ended for a reload {:.2f} s ago; bIsReloading reads {:g} on the own graph: {}", now - s_reloadTapAt, rl, rl > 0.5 ? "the reload goes on, no tap" : "a reload tap of the plugin's own is made (on a full magazine it does nothing)");
					if (rl < 0.5) {
						Input::RequestReloadTap();
					}
					s_reloadTapAt = -1.0;
				}
			} else if (s_reloadTapAt > 0.0 && now - s_reloadTapAt > 6.0) {
				s_reloadTapAt = -1.0;
			}
			const bool req = s_swapReq.exchange(false) && Config::Get().poseWeaponSwap != 2;
			const bool inPose = s_state == State::Switching || s_state == State::Entering || s_state == State::Posed;
			// the weapon state dips to 2 for about 0.3 s when the NPC graph takes over (run m3b-1): not looked at in the first 0.8 s of a pose, and the dip must last 0.35 s
			const bool wsLost = s_state == State::Posed && now - s_info.tPosed >= 0.8 && s_wsBadSince >= 0.0 && now - s_wsBadSince >= 0.35;
			if (req || (wsLost && Config::Get().poseWeaponSwap < 2)) {
				s_swapHoldA.store(true);
				if (inPose) {
					StartLeave(now, req ? s_swapWhy : "the weapon state left 3", false);
				}
			}
		}
		SetFavoritesLock(Config::Get().poseWeaponSwap == 0 && (s_state == State::Switching || s_state == State::Entering || s_state == State::Posed), "the pose state");
		switch (s_state) {
		case State::Off:
			if (a_in.machine == Core::CoverState::InCover && a_in.canStart && a_in.candidate && a_in.candidate->kind != Core::CoverKind::Low && !Config::Get().poseTall) {
				break;  // bPoseTall 0: tall cover stays Phase 3 Basic
			}
			if (Config::Get().poseReload && a_in.machine == Core::CoverState::InCover && VarValue("bIsReloading") > 0.5) {
				s_reloadUntil.store((std::max)(s_reloadUntil.load(), now + 0.5));  // the loop of card M3b: the pose came back 3 s after the reload began, while its clip still played
			}
			if (a_in.machine == Core::CoverState::InCover && now < s_reloadUntil.load()) {
				break;  // T5.7: a reload is playing
			}
			if (s_swapHoldA.load() && a_in.machine == Core::CoverState::InCover) {
				if (s_wheelOpen.load() || s_ws3Since < 0.0 || now - s_ws3Since < 0.5) {
					break;  // T5.6: a weapon control was used: the favourites wheel closed, the weapon drawn for 0.5 s without a break
				}
			}
			if (a_in.machine == Core::CoverState::InCover && a_in.canStart && a_in.candidate && now >= s_nextStart) {
				if ((Config::Get().poseDiagMask & 16) == 0 && WeaponState() != 3) {
					// the weapon is not drawn (REFERENCE 22.9): the first aim press would make the engine draw it and drop the cover
					if (Config::Get().poseDrawWeapon) {
						if (!s_drawAsked || now - s_drawAskedAt > 3.0) {
							s_drawAsked = true;
							s_drawAskedAt = now;
							s_lastDrawAt = now;
							(void)AnimRecon::DrawWeapon(true);
							COVER_DETAIL("COVER POSE: the weapon state is {} (not drawn): the weapon is drawn first", WeaponState());
						}
					} else if (!s_weaponWarned) {
						s_weaponWarned = true;
						COVER_DETAIL("COVER POSE: the weapon state is {} (not drawn): no pose (the plain Phase 3 cover stays)", WeaponState());
					}
					s_haveSample = false;
					s_waitSince = -1.0;
					break;
				}
				if (s_drawAsked) {
					COVER_DETAIL("COVER POSE: the weapon is drawn ({:.2f} s after it was asked for)", now - s_drawAskedAt);
					s_drawAsked = false;
				}
				// T5.2b: no waiting for a steady camera rig (a live player never holds it still: card M3e's poses began 1.5 to 3.3 s late): the hold starts from the rig as it is, without the cover
				// camera's offset, and eases to the set of the situation. The weapon must have read 3 for 0.1 s (the dip to 2 comes after the NPC graph takes over).
				// (run m3h-3: a margin of 0.3 s after a weapon the pose drew itself did not remove the camera step of the sheathed entries and cost 0.2 s: 0.1 s again)
				if (s_ws3Since < 0.0 || now - s_ws3Since < 0.1) {
					break;
				}
				Begin(a_in);
			} else {
				s_haveSample = false;
				s_waitSince = -1.0;
			}
			break;

		case State::Switching: {
			if (a_in.machine != Core::CoverState::InCover) {
				StartLeave(now, "the cover ended before the stance call", false);
				break;
			}
			// (iSyncStandingCrouching resolves only once the cover state is on, so it is no sign of the switch: the call is made after fPoseSwitchWait and repeated when it brought nothing)
			EntryProbe(now, "switching");
			const auto& cfg = Config::Get();
			const auto  d = Core::DecideStanceCall({ .callsMade = 0, .sinceSwitch = now - s_tSwitch, .sinceLastCall = 0.0, .switchWait = static_cast<double>(cfg.poseSwitchWait), .waitForByte = cfg.poseWaitByte != 0,
				.pendingByte = ReadByte(), .waitForEquipByte = cfg.poseWaitByte == 2, .equipByte = ReadEquipByte(), .noRepeat = cfg.poseNoRepeat != 0, .maxWait = static_cast<double>(cfg.poseWaitByteMax) });
			if (d.call) {
				s_info.tGraphReady = now;
				if (cfg.poseWaitByte != 0 || cfg.poseEntryProbe != 0) {
					COVER_DETAIL("COVER POSE #{}: the first stance call: {} ({:.3f} s after the archetype was cleared, byte {}, equip byte {})", s_info.sessions, d.reason, now - s_tSwitch, ReadByte(), ReadEquipByte());
				}
				DoStanceCall(a_in);
			}
			break;
		}

		case State::Entering: {
			if (a_in.machine != Core::CoverState::InCover) {
				StartLeave(now, "the cover ended during the stance call", false);
				break;
			}
			const int stance = StanceField();
			EntryProbe(now, "entering");
			if (Var("bIsInCover", 0.0) > 0.5 && stance >= 2 && stance <= 5) {
				s_info.tPosed = now;
				s_state = State::Posed;
				s_pitchLogged = false;
				// (the aim clamp writes of card M3d are gone: they squeezed the weapon's aim pitch to one degree, run m3g-2)
				if (Mode() != 0) {
					LogByte("in the pose, before the A2 step");
					if (Mode() == 2) {
						bool       r2{ false };
						const bool ran = AnimRecon::RestoreArchetype(r2);
						s_archEarly = true;
						COVER_DETAIL("COVER POSE #{}: mode 2: the own archetype asked for at once: ran {}, returned {}", s_info.sessions, ran, r2);
						LogByte("after the own archetype was asked for");
					}
					(void)ClearByte(Mode() == 2 ? "mode 2, in the pose" : "mode 1, in the pose");
				}
				COVER_DETAIL("COVER POSE #{}: in the pose: bIsInCover 1, the stance field {}, {:.3f} s after the stance call, {:.3f} s after the archetype was cleared", s_info.sessions, stance, now - s_tCall, now - s_tSwitch);
			} else if (Core::DecideStanceCall({ .callsMade = s_info.stanceCalls, .sinceSwitch = now - s_tSwitch, .sinceLastCall = now - s_tCall, .waitForByte = Config::Get().poseWaitByte != 0, .pendingByte = ReadByte(),
							   .waitForEquipByte = Config::Get().poseWaitByte == 2, .equipByte = ReadEquipByte(), .noRepeat = Config::Get().poseNoRepeat != 0, .maxWait = static_cast<double>(Config::Get().poseWaitByteMax) }).call) {
				REX::WARN("COVER POSE #{}: no cover state {:.2f} s after the stance call (bIsInCover {:g}, stance field {}): the call is made again", s_info.sessions, now - s_tCall, Var("bIsInCover", -1.0), stance);
				DoStanceCall(a_in);
			} else if (now - s_tCall > (Config::Get().poseNoRepeat != 0 ? 0.5 : 1.5)) {
				StartLeave(now, Config::Get().poseNoRepeat != 0 ? "the stance call did not bring the cover state (no repeat)" : "the stance calls did not bring the cover state", false);
			}
			break;
		}

		case State::Posed:
			if (a_in.machine != Core::CoverState::InCover) {
				StartLeave(now, a_in.machine == Core::CoverState::Exiting ? "the machine is leaving the cover" : "the machine is no longer in cover", false);
			} else if (now - s_info.tPosed > 0.5 && Var("bIsInCover", 1.0) < 0.5 && Var("bIsExitingCover", 0.0) < 0.5) {
				REX::WARN("COVER POSE #{}: the engine left the cover state by itself (bIsInCover 0, stance field {})", s_info.sessions, StanceField());
				StartLeave(now, "the engine left the cover state", false);
			}
			break;

		case State::Leaving: {
			const bool   cleared = Var("bIsExitingCover", 0.0) < 0.5 && Var("bIsInCover", 0.0) < 0.5;
			const double waited = now - s_tLeave;
			if (s_immediate || (cleared && waited >= 0.3) || waited > 1.5) {
				if (!s_immediate && !cleared) {
					REX::WARN("COVER POSE #{}: the exit did not clear within 1.5 s (bIsExitingCover {:g}, bIsInCover {:g}); the own archetype is asked for anyway", s_info.sessions, Var("bIsExitingCover", -1.0), Var("bIsInCover", -1.0));
				}
				if (Mode() == 0) {
					CallRestore(now);
				} else {
					CallRestoreA2(now);
				}
			}
			Trace(now);
			break;
		}

		case State::Restoring: {
			if (s_a2Phase) {
				Trace(now);
				// card M4b: after a leave without the exit clip the graph sometimes does not update at all (the boom reads exactly -0.5188 for the whole second, the player cannot move,
				// bIsInCover stays 1) until the weapon cycle. A graph that works moves the boom value up by 0.0005 within 0.13 s. At 0.27 s a frozen one gets the remedy of iPoseExitRemedy.
				if (!s_remedyDone && Mode() == 1 && Config::Get().poseExitRemedy > 0) {
					const auto yy = BoomY();
					if (!s_exitY0 && yy && now - s_tLeave < 0.1) {
						s_exitY0 = yy;
					}
					if (s_exitY0 && yy && now - s_tLeave >= 0.27 && *yy > -0.75f) {
						s_remedyDone = true;
						const bool frozen = (*yy - *s_exitY0) < 0.00025f;
						COVER_DETAIL("COVER POSE #{}: exit check at +{:.2f} s: the graph's boom value {:+.4f} (just after the leave {:+.4f}): {}", s_info.sessions, now - s_tLeave, *yy, *s_exitY0, frozen ? "FROZEN: the remedy is applied" : "the graph is moving");
						if (frozen) {
							const int  rem = Config::Get().poseExitRemedy;
							bool       acc{ false }, ret{ false }, flt{ false };
							switch (rem) {
							case 1: (void)AnimRecon::SendEvent("ExitCover", acc); break;
							case 2: (void)AnimRecon::SendEvent("CoverEnd", acc); break;
							case 3: (void)CallExit(ret, flt); acc = ret; break;
							case 4: (void)AnimRecon::RestoreArchetype(ret); acc = ret; break;
							case 5:
								s_a2Phase = false;
								s_info.usedHolsterFallback = true;
								Camera::SetRigHold(true, s_boom, s_pivot);
								s_holdOn = true;
								CallRestore(now);
								break;
							default: break;
							}
							COVER_DETAIL("COVER POSE #{}: iPoseExitRemedy {}: applied (accepted/returned {})", s_info.sessions, rem, acc);
							if (!s_a2Phase) {
								break;
							}
						}
					}
				}
				if (Frame::Latest().cameraIndex != RE::CameraStates::kThirdPerson) {
					COVER_DETAIL("COVER POSE #{}: the camera is not third person: the own graph cannot be checked by the boom node now; the pose is over", s_info.sessions);
					Finish(now, true);
					return Event::Restored;
				}
				const auto y = BoomY();
				if (y && *y < -0.75f) {
					COVER_DETAIL("COVER POSE #{}: the camera boom node reads {:+.4f}: the own graph, {:.3f} s after the exit function, {:.3f} s after the exit clip was over (mode {}, no weapon cycle); pending byte {}", s_info.sessions, *y, now - s_tLeave, now - s_tA2, Mode(), ReadByte());
					Finish(now, true);
					return Event::Restored;
				}
				if (now - s_tA2 >= 1.0) {
					REX::WARN("COVER POSE #{}: the boom node still reads {} {:.2f} s after the exit clip was over (mode {}): the weapon cycle is made", s_info.sessions, y ? std::format("{:+.4f}", *y) : std::string("unreadable"), now - s_tA2, Mode());
					s_a2Phase = false;
					s_info.usedHolsterFallback = true;
					Camera::SetRigHold(true, s_boom, s_pivot);
					s_holdOn = true;
					CallRestore(now);
				}
				break;
			}
			const auto&  cfg = Config::Get();
			const double waited = now - s_tRestore;
			if (!s_holstered && waited >= static_cast<double>(cfg.poseHolsterAt)) {
				(void)AnimRecon::DrawWeapon(false);
				s_holstered = true;
				s_info.usedHolsterFallback = true;
			}
			if (s_holstered && !s_drawn && waited >= static_cast<double>(cfg.poseDrawAt)) {
				(void)AnimRecon::DrawWeapon(true);
				s_drawn = true;
			}
			if (s_drawn && s_tHoldOff < 0.0 && waited >= static_cast<double>(cfg.poseConfirmAt)) {
				HoldOff();
				s_tHoldOff = now;
			}
			if (s_tHoldOff >= 0.0 && s_drawn && waited >= static_cast<double>(cfg.poseConfirmAt) && now - s_tHoldOff >= 0.15) {
				if (Frame::Latest().cameraIndex != RE::CameraStates::kThirdPerson) {
					// the third-person camera state is not live: its nodes say nothing (a hard reset from first person). The rig checks again in third person.
					COVER_DETAIL("COVER POSE #{}: the camera is not third person: the own graph cannot be checked by the boom node now; the pose is over", s_info.sessions);
					Finish(now, true);
					return Event::Restored;
				}
				const auto y = BoomY();
				const bool own = y && *y < -0.75f;  // own: about -1.9 standing or crouched, -0.9 to -1.2 aiming; the NPC graph: -0.52
				COVER_DETAIL("COVER POSE #{}: the camera boom node reads {} {:.3f} s after the hold went off: {}", s_info.sessions, y ? std::format("{:+.4f}", *y) : std::string("unreadable"), now - s_tHoldOff, own ? "the own graph" : "NOT the own graph");
				if (own) {
					Finish(now, true);
					return Event::Restored;
				}
				if (s_restoreAttempt < 1) {
					++s_restoreAttempt;
					REX::WARN("COVER POSE #{}: the own graph is not back: the archetype is asked for once more, with the weapon holstered and drawn", s_info.sessions);
					Camera::SetRigHold(true, s_boom, s_pivot);
					s_holdOn = true;
					CallRestore(now);
					s_info.usedHolsterFallback = true;
				} else {
					Finish(now, false);
					return Event::Restored;
				}
			}
			if (waited > 8.0) {
				Finish(now, false);
				return Event::Restored;
			}
			break;
		}
		}
		return Event::None;
	}

	namespace
	{
		std::atomic<bool> s_cornerActive{ false };
		std::atomic<int>  s_slideSide{ 0 };
	}

	bool CornerActive() noexcept
	{
		return s_cornerActive.load();
	}

	bool TakeCornerSlide(int& a_side) noexcept
	{
		const int s = s_slideSide.exchange(0);
		a_side = s;
		return s != 0;
	}

	void CornerHoldTick(const double a_now)
	{
		static bool   s_wasHolding{ false };
		static int    s_writes{ 0 };
		static double s_t0{ 0.0 };
		const auto&   cfg = Config::Get();
		const bool    want = cfg.poseCornerHold > 0 && s_state == State::Posed && s_cand.kind == Core::CoverKind::High && (VarValue("iIsSighted") > 0.5 || VarValue("iSyncSighted") > 0.5);
		const int sideDir = VarValue("iSyncRightLeft") > 0.5 ? -1 : 1;
		Camera::SetCornerFollow(want && cfg.poseCornerCamera, want && cfg.poseCornerShoulder ? sideDir : 0);
		s_cornerActive.store(want);
		static float s_holdHeading{ 0.0f };
		if (want && !s_wasHolding) {
			// the heading the body has now; when the engine has already turned it (the aim press and this tick are not the same moment) the cover's own heading
			float cover = std::atan2(-s_cand.normal.x, -s_cand.normal.y);
			if (cover < 0.0f) {
				cover += 6.2831853f;
			}
			float cur = cover;
			if (const auto pl = RE::PlayerCharacter::GetSingleton()) {
				cur = pl->data.angle.z;
			}
			float d = cur - cover;
			while (d > 3.14159265f) {
				d -= 6.2831853f;
			}
			while (d < -3.14159265f) {
				d += 6.2831853f;
			}
			s_holdHeading = std::fabs(d) < 0.2618f ? cur : cover;
			Camera::ResetCornerStats();
			s_slideSide.store(sideDir);
		}
		Camera::SetCornerHold(want, s_holdHeading, cfg.poseCornerCone * 0.0174533f);
		if (!want) {
			if (s_wasHolding) {
				COVER_DETAIL("COVER POSE #{}: the corner hold ended after {:.2f} s, {} writes of the heading", s_info.sessions, a_now - s_t0, s_writes);
				s_wasHolding = false;
			}
			return;
		}
		const auto pl = RE::PlayerCharacter::GetSingleton();
		if (!pl) {
			return;
		}
		const float heading = s_holdHeading;
		unsigned long code{ 0 };
		const float   before = pl->data.angle.z;
		if (!Guarded([&] { pl->data.angle.z = heading; }, code)) {
			REX::ERROR("COVER POSE: exception 0x{:08X} writing the heading in the corner hold", code);
			return;
		}
		if (!s_wasHolding) {
			s_wasHolding = true;
			s_writes = 0;
			s_t0 = a_now;
			COVER_DETAIL("COVER POSE #{}: the corner hold begins: the heading {:.1f} deg (it was {:.1f}) is written every tick while the aim is held; the camera's pivot follows the body {}", s_info.sessions, heading * 57.29578f, before * 57.29578f, cfg.poseCornerCamera ? "yes" : "no");
		}
		++s_writes;
	}

	void NoteWeaponControl(const char* a_event, const bool a_wheel)
	{
		if (Config::Get().poseWeaponSwap == 2 || (Config::Get().poseWeaponSwap == 3 && std::strncmp(a_event, "WeaponReadyReload", 17) != 0)) {
			return;  // spike W1 / the play candidate: the plugin does nothing because of the favourites and the quick-keys (the holster hold stays as it is)
		}
		std::snprintf(s_swapWhy, sizeof(s_swapWhy), "weapon control %s", a_event);
		if (a_wheel) {
			s_wheelOpen.store(true);
			s_wheelT.store(Now());
		}
		s_swapReq.store(true);
	}

	void NoteWheelEvent(const bool a_close)
	{
		if (!s_wheelOpen.load()) {
			return;
		}
		if (a_close) {
			s_wheelOpen.store(false);
		} else {
			s_wheelT.store(Now());
		}
	}

	void UnlockInput(const char* a_why)
	{
		std::lock_guard lock(s_mutex);
		SetFavoritesLock(false, a_why);
	}

	bool FavoritesLocked() noexcept
	{
		return s_favOff;
	}

	bool WillPose(const Core::CoverKind a_kind)
	{
		if (!Enabled() || (Config::Get().poseDiagMask & 8) != 0) {
			return false;
		}
		if (a_kind != Core::CoverKind::Low && !Config::Get().poseTall) {
			return false;
		}
		if (s_swapHoldA.load() || Now() < s_reloadUntil.load()) {
			return false;
		}
		return WeaponState() == 3 || Config::Get().poseDrawWeapon;
	}

	void NoteReload(const float a_seconds)
	{
		s_reloadUntil.store(Now() + static_cast<double>(a_seconds));
		s_swapHoldA.store(true);
		std::snprintf(s_swapWhy, sizeof(s_swapWhy), "the reload control");
		s_swapReq.store(true);
	}

	bool SwapHold()
	{
		return s_swapHoldA.load();
	}

	void HardReset(const char* a_why)
	{
		std::lock_guard lock(s_mutex);
		if (s_state == State::Off || s_state == State::Restoring) {
			if (s_state == State::Restoring) {
				HoldOff();
				Camera::CoverCamReset();
			}
			return;
		}
		RestoreAll(a_why, false);
	}
}
