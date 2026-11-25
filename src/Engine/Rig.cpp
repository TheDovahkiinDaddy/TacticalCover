#include "Engine/Rig.h"

#include "Engine/G2Script.h"
#include "Engine/P3Script.h"
#include "Engine/P5Script.h"
#include "Engine/T2Script.h"

#include "Config.h"
#include "Engine/Frame.h"
#include "Engine/Guard.h"
#include "Engine/Input.h"
#include "Engine/Offsets.h"
#include "Engine/RewriteTest.h"

// Three user32 / kernel32 functions, declared by hand: <Windows.h> is not included in this project because its
// ERROR macro breaks REX::ERROR, and CommonLibSF's REX/W32/USER32.h has no SendInput.
extern "C"
{
	__declspec(dllimport) std::uint32_t __stdcall SendInput(std::uint32_t a_count, void* a_inputs, int a_size);
	__declspec(dllimport) void* __stdcall GetForegroundWindow();
	__declspec(dllimport) std::uint32_t __stdcall GetWindowThreadProcessId(void* a_wnd, std::uint32_t* a_processId);
	__declspec(dllimport) std::uint32_t __stdcall GetCurrentProcessId();
}

namespace Engine::Rig
{
	namespace
	{
		// The Win32 INPUT structure for a keyboard event: type, then the union (sized by its largest member,
		// MOUSEINPUT). 40 bytes on x64.
		struct KeyInput
		{
			std::uint32_t type;   // INPUT_KEYBOARD = 1
			std::uint32_t pad0;
			std::uint16_t vk;
			std::uint16_t scan;
			std::uint32_t flags;  // KEYEVENTF_KEYUP 0x2, KEYEVENTF_SCANCODE 0x8
			std::uint32_t time;
			std::uint32_t pad1;
			std::uint64_t extra;
			std::uint64_t pad2;
		};
		static_assert(sizeof(KeyInput) == 40, "the INPUT structure must be 40 bytes on x64");
		static_assert(offsetof(KeyInput, flags) == 12 && offsetof(KeyInput, extra) == 24);

		constexpr std::uint32_t kInputKeyboard = 1;
		constexpr std::uint32_t kKeyUp = 0x2;
		constexpr std::uint32_t kScanCode = 0x8;

		constexpr Key kSpace{ 0x20, 0x39 };
		constexpr Key kW{ 0x57, 0x11 };
		constexpr Key kS{ 0x53, 0x1F };

		constexpr double kMaxHoldSeconds = 5.0;
		constexpr double kStillSeconds = 3.0;
		constexpr double kTapSeconds = 0.06;
		constexpr double kFirstTapWait = 0.5;
		constexpr double kJumpLoopMaxSeconds = 40.0;
		constexpr std::uint32_t kJumpPressTarget = 10;
		// Displacement test (planning session, 2026-11-16): four short holds, one second of standing still before each.
		constexpr double kMoveStillSeconds = 1.0;
		constexpr double kMoveHoldSeconds = 0.5;
		constexpr double kMoveSettleSeconds = 0.3;

		struct MoveStep
		{
			int         stage;  // 0 none, 1 zero, 2 scale
			Key         key;
			const char* label;
		};
		constexpr std::array<MoveStep, 4> kMoves{ {
			{ 1, kW, "W with movement zeroed" },
			{ 0, kW, "W baseline" },
			{ 2, kS, "S with movement scaled" },
			{ 0, kS, "S baseline" },
		} };
		enum class MoveSub
		{
			Wait,
			Hold,
			Settle,
		};
		std::size_t  s_moveIdx{ 0 };
		MoveSub      s_moveSub{ MoveSub::Wait };
		double       s_moveSubStart{ 0.0 };
		RE::NiPoint3 s_moveStartPos{};
		std::array<float, 4> s_disp{};

		enum class Step
		{
			WaitReady,
			FirstTap,
			JumpLoop,
			Move,
			Done,
		};

		// Tick thread only.
		Step          s_step{ Step::WaitReady };
		bool          s_scanMode{ true };   // scan codes first; virtual-key codes only as the fallback
		bool          s_triedVk{ false };
		double        s_stepStart{ 0.0 };
		bool          s_announced{ false };

		// held keys
		struct Held
		{
			Key    key{};
			bool   down{ false };
			double since{ 0.0 };
		};
		std::array<Held, 8> s_held{};
		double              s_now{ 0.0 };

		// stillness
		bool         s_haveAnchor{ false };
		RE::NiPoint3 s_anchor{};
		double       s_stillSince{ 0.0 };

		// tapping
		bool          s_tapping{ false };
		double        s_tapEnd{ 0.0 };
		std::uint32_t s_kbBefore{ 0 };
		double        s_lastTap{ 0.0 };
		std::uint32_t s_countedAtTap{ 0 };
		bool          s_decided{ false };
		bool          s_waitFinish{ false };
		double        s_nextTap{ -1.0 };
		double        s_loopStart{ 0.0 };


		bool Send(const Key a_key, const bool a_up)
		{
			KeyInput in{};
			in.type = kInputKeyboard;
			in.vk = a_key.vk;
			in.scan = a_key.scan;
			in.flags = (s_scanMode ? kScanCode : 0u) | (a_up ? kKeyUp : 0u);
			const auto sent = ::SendInput(1, &in, static_cast<int>(sizeof(KeyInput)));
			if (sent != 1) {
				REX::WARN("S0 rig: SendInput({} 0x{:02X}) reported {} events sent", a_up ? "up" : "down", a_key.vk, sent);
			}
			return sent == 1;
		}

		[[nodiscard]] Held* Find(const Key a_key)
		{
			for (auto& h : s_held) {
				if (h.key.vk == a_key.vk) {
					return &h;
				}
			}
			for (auto& h : s_held) {
				if (h.key.vk == 0) {
					h.key = a_key;
					return &h;
				}
			}
			return nullptr;
		}

		void KeyDown(const Key a_key)
		{
			if (auto* h = Find(a_key); h && !h->down) {
				if (Send(a_key, false)) {
					h->down = true;
					h->since = s_now;
				}
			}
		}

		void KeyUp(const Key a_key)
		{
			if (auto* h = Find(a_key); h && h->down) {
				Send(a_key, true);
				h->down = false;
			}
		}

		void ReleaseAll(const char* a_why)
		{
			for (auto& h : s_held) {
				if (h.down) {
					Send(h.key, true);
					h.down = false;
					REX::INFO("S0 rig: released key 0x{:02X} ({})", h.key.vk, a_why);
				}
			}
			s_tapping = false;
			Input::SetRigMoveStage(0);
		}

		[[nodiscard]] std::string MoveVerdicts()
		{
			const float d1 = s_disp[0];
			const float d2 = s_disp[1];
			const float d3 = s_disp[2];
			const float d4 = s_disp[3];
			const char* zero = d2 < 0.2f ? "inconclusive (baseline under 0.2 m)" : (d1 < 0.1f * d2 ? "WORKS" : "NO EFFECT");
			const char* scale = d4 < 0.2f ? "inconclusive (baseline under 0.2 m)" : (d3 < 0.8f * d4 ? "changed" : "NOT changed");
			return std::format("RESULT S2b move: zeroed d1 {:.2f} m vs baseline d2 {:.2f} m -> {} (WORKS if d1 < 0.1 x d2) | scaled d3 {:.2f} m vs baseline d4 {:.2f} m -> {} (changed if d3 < 0.8 x d4)",
				d1, d2, zero, d3, d4, scale);
		}

		void Finish(const char* a_summary, const bool a_ok)
		{
			ReleaseAll(a_ok ? "script finished" : "script aborted");
			REX::INFO("RESULT rig: {} | {}", a_summary, RewriteTest::Summary());
			REX::INFO("TEST COMPLETE{}", a_ok ? "" : " (aborted)");
			s_step = Step::Done;

			const auto& cfg = Config::Get();
			if (cfg.quitAfterTest) {
				if (const auto main = RE::Main::GetSingleton()) {
					main->quitGame = true;
					REX::INFO("S0 rig: Main::quitGame set; the game should close now");
				}
			} else {
				REX::INFO("S0 rig: leaving the game running (bQuitAfterTest=0); close it by hand or from the launcher");
			}
		}

		std::uint32_t s_tapNumber{ 0 };

		void Tap(const double a_now)
		{
			++s_tapNumber;
			REX::INFO("S0 rig: tap #{} (keyboard Jump presses seen so far {}, counted {}, finished checks {})", s_tapNumber, RewriteTest::KeyboardJumpPresses(),
				RewriteTest::CountedJumpPresses(), RewriteTest::FinishedJumpChecks());
			s_kbBefore = RewriteTest::KeyboardJumpPresses();
			s_countedAtTap = RewriteTest::CountedJumpPresses();
			s_lastTap = a_now;
			s_decided = false;
			s_waitFinish = false;
			s_nextTap = -1.0;
			s_tapping = true;
			s_tapEnd = a_now + kTapSeconds;
			KeyDown(kSpace);
		}

		void TickImpl(const Frame::Snapshot& a_snap)
		{
			s_now = a_snap.time;
			const double now = a_snap.time;

			// ---- safety first: applies to every step that may hold a key ----
			if (s_step != Step::WaitReady && s_step != Step::Done) {
				const char* abort = nullptr;
				if (!a_snap.valid || a_snap.paused) {
					abort = "the game paused or left the world";
				} else if (!InForeground()) {
					abort = "the game window lost the foreground";
				}
				for (const auto& h : s_held) {
					if (h.down && now - h.since > kMaxHoldSeconds) {
						abort = "a key was held longer than 5 s";
					}
				}
				if (abort) {
					Finish(std::format("aborted: {}", abort).c_str(), false);
					return;
				}
			}

			// Space taps end after ~60 ms.
			if (s_tapping && now >= s_tapEnd) {
				KeyUp(kSpace);
				s_tapping = false;
			}

			switch (s_step) {
			case Step::WaitReady: {
				if (!a_snap.valid || a_snap.paused || !Input::HookInstalled()) {
					s_haveAnchor = false;
					return;
				}
				if (!s_haveAnchor || std::fabs(a_snap.position.x - s_anchor.x) > 0.02f || std::fabs(a_snap.position.y - s_anchor.y) > 0.02f ||
					std::fabs(a_snap.position.z - s_anchor.z) > 0.02f) {
					s_anchor = a_snap.position;
					s_haveAnchor = true;
					s_stillSince = now;
					return;
				}
				if (now - s_stillSince < kStillSeconds) {
					return;
				}
				if (!InForeground()) {
					if (!s_announced) {
						s_announced = true;
						REX::INFO("S0 rig: ready, but the game window is not in the foreground; waiting (no synthetic key is sent while it is not)");
					}
					return;
				}
				REX::INFO("S0 rig: script 's2b' starting (save loaded, player still for {:.0f} s, game window in the foreground)", kStillSeconds);
				s_step = Step::FirstTap;
				s_stepStart = now;
				Tap(now);
				return;
			}

			case Step::FirstTap: {
				// The first tap proves that synthetic keys reach the game at all: the hook must see a keyboard Jump press.
				if (RewriteTest::KeyboardJumpPresses() > s_kbBefore) {
					REX::INFO("S0 rig: the hook saw the synthetic Space press ({} mode); continuing with the jump rotation", s_scanMode ? "scan-code" : "virtual-key");
					s_step = Step::JumpLoop;
					s_loopStart = s_stepStart;
					return;
				}
				if (now - s_lastTap >= kFirstTapWait) {
					if (!s_triedVk) {
						s_triedVk = true;
						s_scanMode = false;
						REX::INFO("S0 rig: no Jump press seen after the scan-code tap; retrying once with virtual-key codes");
						Tap(now);
						return;
					}
					Finish("synthetic keyboard input did not reach the game", false);
				}
				return;
			}

			case Step::JumpLoop: {
				const auto counted = RewriteTest::CountedJumpPresses();
				const auto finished = RewriteTest::FinishedJumpChecks();

				if (!s_tapping && !s_decided && now >= s_lastTap + 0.3) {
					s_decided = true;
					if (RewriteTest::KeyboardJumpPresses() <= s_kbBefore) {
						REX::WARN("S0 rig: tap #{} was NOT seen by the input hook (synthetic key lost, or the keyboard stopped delivering events)", s_tapNumber);
					}
					if (counted > s_countedAtTap) {
						s_waitFinish = true;  // counted: wait for its check, then 1 s
					} else {
						s_nextTap = s_lastTap + 1.5;  // not counted (player not still, check busy): try again
					}
				}
				if (s_waitFinish && finished >= counted) {
					s_waitFinish = false;
					s_nextTap = now + 1.0;
				}

				const bool enough = counted >= kJumpPressTarget && finished >= counted;
				if (enough || now - s_loopStart > kJumpLoopMaxSeconds) {
					REX::INFO("S0 rig: jump phase over ({} counted presses, {} finished checks, {:.0f} s); starting the displacement test", counted, finished, now - s_loopStart);
					s_step = Step::Move;
					s_stepStart = now;
					s_moveIdx = 0;
					s_moveSub = MoveSub::Wait;
					s_moveSubStart = now;
					return;
				}
				if (!s_tapping && s_nextTap >= 0.0 && now >= s_nextTap) {
					Tap(now);
				}
				return;
			}

			case Step::Move: {
				const auto& m = kMoves[s_moveIdx];
				switch (s_moveSub) {
				case MoveSub::Wait:
					if (now - s_moveSubStart >= kMoveStillSeconds) {
						s_moveStartPos = a_snap.position;
						Input::SetRigMoveStage(m.stage);
						KeyDown(m.key);
						s_moveSub = MoveSub::Hold;
						s_moveSubStart = now;
					}
					break;
				case MoveSub::Hold:
					if (now - s_moveSubStart >= kMoveHoldSeconds) {
						KeyUp(m.key);
						s_moveSub = MoveSub::Settle;
						s_moveSubStart = now;
					}
					break;
				case MoveSub::Settle:
					if (now - s_moveSubStart >= kMoveSettleSeconds) {
						Input::SetRigMoveStage(0);
						const float dx = a_snap.position.x - s_moveStartPos.x;
						const float dy = a_snap.position.y - s_moveStartPos.y;
						s_disp[s_moveIdx] = std::sqrt(dx * dx + dy * dy);
						REX::INFO("S0 rig: move step {} ({}): horizontal displacement {:.2f} m", s_moveIdx + 1, m.label, s_disp[s_moveIdx]);
						if (++s_moveIdx >= kMoves.size()) {
							REX::INFO("{}", MoveVerdicts());
							Finish("script s2b finished", true);
						} else {
							s_moveSub = MoveSub::Wait;
							s_moveSubStart = now;
						}
					}
					break;
				}
				return;
			}

			case Step::Done:
				return;
			}
		}
	}

	// ---- public plumbing for the scripts ------------------------------------------------------------------------
	namespace
	{
		struct MouseInput
		{
			std::uint32_t type;  // INPUT_MOUSE = 0
			std::uint32_t pad0;
			std::int32_t  dx;
			std::int32_t  dy;
			std::uint32_t mouseData;
			std::uint32_t flags;  // MOUSEEVENTF_RIGHTDOWN 0x8, MOUSEEVENTF_RIGHTUP 0x10
			std::uint32_t time;
			std::uint32_t pad1;
			std::uint64_t extra;
		};
		static_assert(sizeof(MouseInput) == 40, "the INPUT structure must be 40 bytes on x64");

		struct PendingTap
		{
			Key    key{};
			double releaseAt{ 0.0 };
			bool   active{ false };
		};
		std::array<PendingTap, 8> s_taps{};
		bool                      s_mouseRightDown{ false };
		bool                      s_mouseLeftDown{ false };

		void PumpTaps()
		{
			for (auto& t : s_taps) {
				if (t.active && s_now >= t.releaseAt) {
					KeyUp(t.key);
					t.active = false;
				}
			}
		}
	}

	bool KeyForChar(const char a_char, Key& a_key, bool& a_shift)
	{
		static constexpr std::string_view kRowQ = "qwertyuiop";  // scan 0x10..
		static constexpr std::string_view kRowA = "asdfghjkl";   // scan 0x1E..
		static constexpr std::string_view kRowZ = "zxcvbnm";     // scan 0x2C..
		a_shift = false;
		char c = a_char;
		if (c >= 'A' && c <= 'Z') {
			a_shift = true;
			c = static_cast<char>(c - 'A' + 'a');
		}
		const auto letter = [&](const std::string_view a_row, const std::uint16_t a_first) {
			const auto pos = a_row.find(c);
			if (pos == std::string_view::npos) {
				return false;
			}
			a_key = { static_cast<std::uint16_t>(c - 'a' + 'A'), static_cast<std::uint16_t>(a_first + pos) };
			return true;
		};
		if (letter(kRowQ, 0x10) || letter(kRowA, 0x1E) || letter(kRowZ, 0x2C)) {
			return true;
		}
		a_shift = false;
		if (c >= '1' && c <= '9') {
			a_key = { static_cast<std::uint16_t>(c), static_cast<std::uint16_t>(0x02 + (c - '1')) };
			return true;
		}
		switch (c) {
		case '0': a_key = { '0', 0x0B }; return true;
		case ' ': a_key = { 0x20, 0x39 }; return true;
		case '.': a_key = { 0xBE, 0x34 }; return true;
		case '-': a_key = { 0xBD, 0x0C }; return true;
		case '_': a_key = { 0xBD, 0x0C }; a_shift = true; return true;
		case '"': a_key = { 0xDE, 0x28 }; a_shift = true; return true;
		default: return false;
		}
	}

	bool InForeground()
	{
		void* wnd = ::GetForegroundWindow();
		if (!wnd) {
			return false;
		}
		std::uint32_t pid{ 0 };
		::GetWindowThreadProcessId(wnd, &pid);
		return pid == ::GetCurrentProcessId();
	}

	void Tap(const Key a_key, const double a_seconds)
	{
		if (!InForeground()) {
			REX::WARN("S0 rig: Tap(0x{:02X}) skipped, the game window is not in the foreground", a_key.vk);
			return;
		}
		for (auto& t : s_taps) {
			if (!t.active) {
				KeyDown(a_key);
				t = { a_key, s_now + a_seconds, true };
				return;
			}
		}
		REX::WARN("S0 rig: Tap(0x{:02X}) skipped, too many taps pending", a_key.vk);
	}

	void MouseRight(const bool a_down)
	{
		if (a_down && !InForeground()) {
			REX::WARN("S0 rig: right mouse button skipped, the game window is not in the foreground");
			return;
		}
		if (a_down == s_mouseRightDown) {
			return;
		}
		MouseInput in{};
		in.type = 0;
		in.flags = a_down ? 0x8u : 0x10u;
		if (::SendInput(1, &in, static_cast<int>(sizeof(MouseInput))) == 1) {
			s_mouseRightDown = a_down;
		}
	}

	void MouseLeft(const bool a_down)
	{
		if (a_down && !InForeground()) {
			REX::WARN("S0 rig: left mouse button skipped, the game window is not in the foreground");
			return;
		}
		if (a_down == s_mouseLeftDown) {
			return;
		}
		MouseInput in{};
		in.type = 0;
		in.flags = a_down ? 0x2u : 0x4u;
		if (::SendInput(1, &in, static_cast<int>(sizeof(MouseInput))) == 1) {
			s_mouseLeftDown = a_down;
		}
	}

	void MouseMove(const int a_dx, const int a_dy)
	{
		if (!InForeground()) {
			return;
		}
		MouseInput in{};
		in.type = 0;
		in.dx = a_dx;
		in.dy = a_dy;
		in.flags = 0x1u;  // MOUSEEVENTF_MOVE, relative
		(void)::SendInput(1, &in, static_cast<int>(sizeof(MouseInput)));
	}

	void ReleaseEverything(const char* a_why)
	{
		for (auto& t : s_taps) {
			t.active = false;
		}
		MouseRight(false);
		MouseLeft(false);
		ReleaseAll(a_why);
	}

	void OnTick(const Frame::Snapshot& a_snap)
	{
		const auto& cfg = Config::Get();
		if (!cfg.enabled) {
			return;
		}
		if (cfg.testScript == "g2") {
			s_now = a_snap.time;
			unsigned long code{ 0 };
			if (!Guarded(
					[&] {
						PumpTaps();
						G2::OnTick(a_snap);
					},
					code)) {
				unsigned long ignored{ 0 };
				(void)Guarded([] { ReleaseEverything("fault"); }, ignored);
				G2::Abort("exception in the g2 script");
				REX::ERROR("S0 rig: exception 0x{:08X} in the g2 script; keys released and the script stopped for this session", code);
			}
			return;
		}
		if (cfg.testScript == "p3") {
			s_now = a_snap.time;
			unsigned long code{ 0 };
			if (!Guarded(
					[&] {
						PumpTaps();
						P3::OnTick(a_snap);
					},
					code)) {
				unsigned long ignored{ 0 };
				(void)Guarded([] { ReleaseEverything("fault"); }, ignored);
				P3::Abort("exception in the p3 script");
				REX::ERROR("S0 rig: exception 0x{:08X} in the p3 script; keys released and the script stopped for this session", code);
			}
			return;
		}
		if (cfg.testScript == "p5") {
			s_now = a_snap.time;
			unsigned long code{ 0 };
			if (!Guarded(
					[&] {
						PumpTaps();
						P5::OnTick(a_snap);
					},
					code)) {
				unsigned long ignored{ 0 };
				(void)Guarded([] { ReleaseEverything("fault"); }, ignored);
				P5::Abort("exception in the p5 script", true);
				REX::ERROR("S0 rig: exception 0x{:08X} in the p5 script; keys released and the script stopped for this session", code);
			}
			return;
		}
		if (cfg.testScript == "t2") {
			s_now = a_snap.time;
			unsigned long code{ 0 };
			if (!Guarded(
					[&] {
						PumpTaps();
						T2::OnTick(a_snap);
					},
					code)) {
				unsigned long ignored{ 0 };
				(void)Guarded([] { ReleaseEverything("fault"); }, ignored);
				T2::Abort("exception in the t2 script");
				REX::ERROR("S0 rig: exception 0x{:08X} in the t2 script; keys released and the script stopped for this session", code);
			}
			return;
		}
		if (cfg.testScript != "s2b" || s_step == Step::Done) {
			return;
		}

		unsigned long code{ 0 };
		if (!Guarded([&] { TickImpl(a_snap); }, code)) {
			// release whatever is held, even though the fault may have come from there
			unsigned long ignored{ 0 };
			(void)Guarded([] { ReleaseAll("fault"); }, ignored);
			s_step = Step::Done;
			REX::ERROR("S0 rig: exception 0x{:08X}; keys released and the script stopped for this session", code);
			REX::INFO("TEST COMPLETE (aborted)");
		}
	}

	bool Finished() noexcept
	{
		return s_step == Step::Done;
	}
}
