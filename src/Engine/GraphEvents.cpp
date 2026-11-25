#include "Engine/GraphEvents.h"

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "Config.h"
#include "Engine/AudioProbe.h"
#include "Engine/CoverPose.h"
#include "Engine/Frame.h"
#include "Engine/Guard.h"
#include "Engine/Offsets.h"
#include "Engine/WeaponDraw.h"

namespace Engine::GraphEvents
{
	namespace
	{
		using Notify_t = bool (*)(void*, const RE::BSFixedString&);

		[[nodiscard]] std::uintptr_t ModuleBase() { return REX::FModule::GetExecutingModule().GetBaseAddress(); }

		struct Ev
		{
			double        t{ 0.0 };
			std::array<char, 56> name{};
			int           ret{ -1 };
			bool          own{ false };
			bool          blocked{ false };
			std::uint32_t tid{ 0 };
		};

		constexpr std::size_t kRing = 2048;

		std::mutex            s_mutex;
		std::array<Ev, kRing> s_ring{};
		std::size_t           s_head{ 0 };   // next slot to write
		std::size_t           s_count{ 0 };
		Notify_t              s_orig{ nullptr };
		bool                  s_tried{ false };
		bool                  s_installed{ false };
		std::atomic<bool>     s_faulted{ false };
		std::atomic<bool>     s_blockOn{ false };  // the pose is Posed and iPoseEquipBlock is 1
		std::atomic<std::uint32_t> s_seen{ 0 }, s_blocked{ 0 };
		std::atomic<double>   s_winFrom{ 1e18 }, s_winUntil{ -1.0 };
		std::atomic<double>   s_lastBlockT{ -1.0 };
		double                s_forwardedBlockT{ -1.0 };
		std::uint32_t         s_windows{ 0 };
		std::uint32_t         s_lastKey{ 0 };
		double                s_nextPoll{ 0.0 };
		std::vector<std::string> s_blockNames;
		std::string           s_blockNamesFrom;
		thread_local bool     t_own{ false };

		using Capture_t = unsigned short (*)(unsigned long, unsigned long, void**, unsigned long*);
		Capture_t s_capture{ nullptr };

		// a return address as text: an RVA when it lies in Starfield.exe's image, else the module's file name and the offset in it
		[[nodiscard]] std::string AddrText(void* a_addr)
		{
			REX::W32::MEMORY_BASIC_INFORMATION mbi{};
			if (REX::W32::VirtualQuery(a_addr, &mbi, sizeof(mbi)) == 0 || !mbi.allocationBase) {
				return std::format("?{:X}", reinterpret_cast<std::uintptr_t>(a_addr));
			}
			const auto base = reinterpret_cast<std::uintptr_t>(mbi.allocationBase);
			const auto addr = reinterpret_cast<std::uintptr_t>(a_addr);
			if (base == ModuleBase()) {
				return std::format("0x{:X}", addr - base);
			}
			char name[260]{};
			const auto n = REX::W32::GetModuleFileNameA(reinterpret_cast<REX::W32::HMODULE>(mbi.allocationBase), name, 259);
			std::string file = n ? name : "?";
			if (const auto slash = file.find_last_of("\\/"); slash != std::string::npos) {
				file = file.substr(slash + 1);
			}
			return std::format("{}+0x{:X}", file, addr - base);
		}

		// the call stack of an engine's Equip / InitializeGunGraph (23.24): read only, in __try; one log line, the first 32 frames (frame 0 is the hook)
		void LogStack(const char* a_event, const double a_now)
		{
			if (!s_capture) {
				return;
			}
			void*         frames[32]{};
			unsigned long hash{ 0 };
			unsigned short n{ 0 };
			unsigned long code{ 0 };
			if (!Guarded([&] { n = s_capture(0, 32, frames, &hash); }, code)) {
				REX::ERROR("GRAPHSTACK: exception 0x{:08X} while capturing the stack", code);
				return;
			}
			std::string out;
			for (unsigned short i = 0; i < n; ++i) {
				out += (i ? " " : "") + AddrText(frames[i]);
			}
			COVER_DETAIL("GRAPHSTACK {} t={:.3f} thread {} ({} frames, hash 0x{:X}): {}", a_event, a_now, REX::W32::GetCurrentThreadId(), n, hash, out);
		}

		[[nodiscard]] std::string Lower(std::string a_s)
		{
			for (auto& c : a_s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return a_s;
		}

		void ParseBlockNames()
		{
			const auto& cur = Config::Get().equipBlockEvents;
			if (cur == s_blockNamesFrom) {
				return;
			}
			s_blockNamesFrom = cur;
			s_blockNames.clear();
			std::string item;
			for (const char c : cur + ",") {
				if (c == ',' || c == ';' || c == ' ') {
					if (!item.empty()) {
						s_blockNames.push_back(Lower(item));
						item.clear();
					}
				} else {
					item.push_back(c);
				}
			}
		}

		void LogEv(const char* a_why, const Ev& a_e, const double a_tKey)
		{
			COVER_DETAIL("GRAPHEVT {} [{:+.3f}] {} -> {}{}{} (thread {})", a_why, a_e.t - a_tKey, a_e.name.data(), a_e.blocked ? "BLOCKED (false returned)" : (a_e.ret < 0 ? "?" : (a_e.ret ? "accepted" : "refused")), a_e.own ? " [plugin's own]" : "", "", a_e.tid);
		}

		double s_tKey{ -1.0 };

		bool Hook(void* a_this, const RE::BSFixedString& a_event)
		{
			const double now = Frame::Now();
			Ev           e;
			e.t = now;
			e.own = t_own;
			e.tid = static_cast<std::uint32_t>(REX::W32::GetCurrentThreadId());
			unsigned long code{ 0 };
			(void)Guarded([&] {
				const char* s = a_event.c_str();
				if (s) {
					std::size_t i = 0;
					for (; i + 1 < e.name.size() && s[i]; ++i) {
						e.name[i] = s[i];
					}
					e.name[i] = 0;
				}
			}, code);
			if (Config::Get().graphEventProbe == 2 && !e.own) {
				const char* nm = e.name.data();
				if (std::strcmp(nm, "Equip") == 0 || std::strcmp(nm, "InitializeGunGraph") == 0 || std::strcmp(nm, "InstantInitializeGunGraph") == 0) {
					LogStack(nm, now);
				}
			}
			bool block = false;
			if (s_blockOn.load() && !s_blockNames.empty()) {
				const auto lower = Lower(e.name.data());
				for (const auto& n : s_blockNames) {
					if (lower == n) {
						block = true;
						break;
					}
				}
			}
			bool ret = false;
			if (block) {
				e.blocked = true;
				e.ret = 0;
				++s_blocked;
				s_lastBlockT = now;
			} else {
				ret = s_orig(a_this, a_event);
				e.ret = ret ? 1 : 0;
			}
			++s_seen;
			if (Config::Get().graphEventProbe != 0 || block) {
				std::lock_guard lock(s_mutex);
				s_ring[s_head] = e;
				s_head = (s_head + 1) % kRing;
				if (s_count < kRing) {
					++s_count;
				}
				if (now >= s_winFrom.load() && now <= s_winUntil.load()) {
					LogEv("live", e, s_tKey);
				} else if (block) {
					LogEv("blocked", e, s_tKey);
				}
			}
			return ret;
		}

		void Install()
		{
			s_tried = true;
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				s_tried = false;  // try again when the player exists
				return;
			}
			const auto vtblAddr = kPlayerGraphHolderVtable.address();
			std::uintptr_t live{ 0 };
			if (!SafeRead(reinterpret_cast<const std::byte*>(player) + kPlayerGraphHolderOffset, live)) {
				REX::ERROR("GRAPHEVT: cannot read the vtable pointer at PlayerCharacter+0x{:X}: hook NOT installed", static_cast<std::uint32_t>(kPlayerGraphHolderOffset));
				s_faulted = true;
				return;
			}
			if (live != vtblAddr) {
				REX::ERROR("GRAPHEVT: the live vtable pointer of the player's graph holder is RVA 0x{:X}, expected ID {} (RVA 0x{:X}): hook NOT installed", live - ModuleBase(), kPlayerGraphHolderVtable.id(), vtblAddr - ModuleBase());
				s_faulted = true;
				return;
			}
			std::uintptr_t original{ 0 };
			if (!SafeRead(reinterpret_cast<const void*>(vtblAddr + sizeof(void*) * kNotifyAnimationGraphSlot), original)) {
				REX::ERROR("GRAPHEVT: cannot read slot {}: hook NOT installed", kNotifyAnimationGraphSlot);
				s_faulted = true;
				return;
			}
			REX::INFO("GRAPHEVT: the player's graph holder vtable is RVA 0x{:X} (ID {}); slot {} points to RVA 0x{:X} (offline expectation 0x{:X}){}", vtblAddr - ModuleBase(), kPlayerGraphHolderVtable.id(), kNotifyAnimationGraphSlot, original - ModuleBase(),
				kNotifyAnimationGraphRva, original - ModuleBase() == kNotifyAnimationGraphRva ? "" : "  <== differs: nothing installed");
			if (original - ModuleBase() != kNotifyAnimationGraphRva) {
				s_faulted = true;
				return;
			}
			s_orig = reinterpret_cast<Notify_t>(original);
			if (const auto nt = REX::W32::GetModuleHandleA("ntdll.dll")) {
				s_capture = reinterpret_cast<Capture_t>(REX::W32::GetProcAddress(nt, "RtlCaptureStackBackTrace"));
			}
			COVER_DETAIL("GRAPHEVT: the stack capture function is {}", s_capture ? "found" : "NOT found");
			REL::Relocation<std::uintptr_t> vtbl{ vtblAddr };
			vtbl.write_vfunc(kNotifyAnimationGraphSlot, reinterpret_cast<std::uintptr_t>(&Hook));
			s_installed = true;
			REX::INFO("GRAPHEVT: hook installed on slot {} (iGraphEventProbe {}, iPoseEquipBlock {})", kNotifyAnimationGraphSlot, Config::Get().graphEventProbe, Config::Get().poseEquipBlock);
		}
	}

	Info GetInfo()
	{
		return { s_installed, s_faulted.load(), s_seen.load(), s_blocked.load(), s_windows };
	}

	void SetOwn(const bool a_own) noexcept
	{
		t_own = a_own;
	}

	void Tick(const double a_now, const bool a_posed)
	{
		const auto& cfg = Config::Get();
		if ((cfg.graphEventProbe == 0 && cfg.poseEquipBlock == 0) || s_faulted.load() || !cfg.enabled) {
			s_blockOn = false;
			return;
		}
		if (!s_tried) {
			unsigned long code{ 0 };
			if (!Guarded([] { Install(); }, code)) {
				s_faulted = true;
				REX::ERROR("GRAPHEVT: exception 0x{:08X} while installing the hook", code);
			}
			return;
		}
		if (!s_installed) {
			return;
		}
		ParseBlockNames();
		s_blockOn = cfg.poseEquipBlock != 0 && a_posed;
		if (const double bt = s_lastBlockT.load(); bt > s_forwardedBlockT) {
			s_forwardedBlockT = bt;
			WeaponDraw::NoteEquipBlocked(bt);
		}
		if (cfg.graphEventProbe == 0 || a_now < s_nextPoll) {
			return;
		}
		s_nextPoll = a_now + 0.02;
		const auto key = Engine::AudioProbe::CurrentWeapon();
		if (key == 0) {
			return;
		}
		if (s_lastKey == 0) {
			s_lastKey = key;
			return;
		}
		if (key != s_lastKey) {
			++s_windows;
			COVER_DETAIL("GRAPHEVT: window {}: the weapon key changed 0x{:08X} -> 0x{:08X}; pose {}; the events of the last second follow, then the next 1.5 s as they come", s_windows, s_lastKey, key, CoverPose::ToString(CoverPose::GetInfo().state));
			s_lastKey = key;
			std::lock_guard lock(s_mutex);
			s_tKey = a_now;
			const std::size_t n = s_count;
			for (std::size_t i = 0; i < n; ++i) {
				const auto& e = s_ring[(s_head + kRing - n + i) % kRing];
				if (e.t >= a_now - 1.0 && e.t <= a_now) {
					LogEv("ring", e, s_tKey);
				}
			}
			s_winFrom = a_now;
			s_winUntil = a_now + 1.5;
		}
	}
}
