#include "Engine/Input.h"

#include "Config.h"
#include "Engine/Camera.h"
#include "Engine/BlindFire.h"
#include "Engine/CoverPose.h"
#include "Engine/Cover.h"
#include "Engine/Frame.h"
#include "Engine/Guard.h"
#include "Engine/Offsets.h"
#include "Engine/RewriteTest.h"

#include "Core/SwapChord.h"

#include "REX/W32/KERNEL32.h"

#include <atomic>
#include <mutex>
#include <map>
#include <set>

namespace Engine::Input
{
	namespace
	{
		using Orig_t = void (*)(RE::BSInputEventReceiver*, const RE::InputEvent*);

		constexpr std::size_t kMaxChain = 512;  // a queue longer than this is treated as corrupt

		// ---- state shared between the hook (game input path) and the tick (SFSE task) ----------------
		std::atomic<bool> s_installed{ false };
		std::atomic<bool> s_gaveUp{ false };   // install refused or failed: stop retrying
		std::atomic<bool> s_disabled{ false };  // a fault inside the hook: pass everything through untouched
		Orig_t            s_orig{ nullptr };

		std::mutex         s_stateMutex;
		Core::InputState   s_state;

		std::atomic<std::uint64_t> s_hookCalls{ 0 };
		std::atomic<std::uint64_t> s_eventCounts[8]{};  // by InputEvent::EventType
		std::atomic<std::uint32_t> s_hookThread{ 0 };

		// S2b modes. Toggled by the debug keys, read by the hook.
		std::atomic<bool> s_swallowJump{ false };
		// The Move stick before the rewrite hook changed it (S2b stick check): magnitude and steady-clock time of the latest event.
		std::atomic<float>         s_rawMoveMag{ 0.0f };
		std::atomic<std::int64_t>  s_rawMoveTicks{ 0 };
		std::atomic<bool> s_zeroMove{ false };
		std::atomic<bool> s_scaleMove{ false };
		std::atomic<int>  s_method{ 0 };
		std::atomic<int>  s_rigMoveStage{ 0 };  // set by the S0 rig for its displacement test

		// The rewrite hook on PlayerControls::Manager's receiver.
		Orig_t                     s_origControls{ nullptr };
		std::atomic<bool>          s_controlsInstalled{ false };
		std::atomic<bool>          s_controlsGaveUp{ false };
		std::atomic<bool>          s_controlsDisabled{ false };
		std::atomic<std::uint64_t> s_controlsCalls{ 0 };

		// Release 1.0.0: the shoulder swap on LT + A, read here (REFERENCE 24). The chord's state is shared by the controls hook (the pad) and the camera
		// receiver's hook (the keyboard key), so it sits behind its own mutex. A fault inside it switches the chord off for the session (rule 3).
		std::mutex             s_chordMutex;
		Core::SwapChord        s_chord;
		std::atomic<bool>      s_chordFaulted{ false };
		std::atomic<bool>      s_chordLogged{ false };

		[[nodiscard]] double ChordNow()
		{
			return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		[[nodiscard]] bool ChordWanted()
		{
			const auto& cfg = Config::Get();
			return cfg.shoulderPadChord && cfg.camera && cfg.framing && !Camera::Compat() && !s_chordFaulted.load();
		}

		// the keyboard path: false when the pad path swapped for the same press a moment ago
		[[nodiscard]] bool ChordAllowKeySwap()
		{
			std::lock_guard lock(s_chordMutex);
			s_chord.SetSettings({ ChordWanted(), Config::Get().shoulderPadTrigger, 0.25 });
			return s_chord.AllowKeySwap(ChordNow());
		}

		// S3: the probe key sets this; the tick (a safe place for engine calls) consumes it.
		std::atomic<bool> s_probeRequested{ false };
		// T2.3: the sense key sets this; Engine/CoverSense consumes it on the tick.
		std::atomic<bool> s_senseRequested{ false };
		// T3.5: the reload key sets this; Frame::Tick re-reads the INI.
		std::atomic<bool> s_reloadRequested{ false };

		// S2b automatic cycle: the key sets this, the tick starts or stops the cycle.
		std::atomic<bool> s_autoToggle{ false };

		// Install attempts (tick thread only).
		double s_nextInstallAttempt{ 0.0 };
		bool   s_waitingLogged{ false };

		// Logging budgets (hook thread).
		std::atomic<int>         s_logLines{ 0 };
		std::set<std::uint64_t>  s_seenKeys;  // hook thread only
		std::map<std::uint64_t, int> s_rawDumps;  // hook thread only: dumps so far per (eventType, deviceType, idCode)

		constexpr const char* kMethodNames[] = { "unlink from queue", "set disabled flag", "set status=Stop", "set value=0" };
		constexpr int         kMethodCount = 4;

		[[nodiscard]] std::string_view DeviceName(const RE::InputEvent::DeviceType a_type) noexcept
		{
			switch (a_type) {
			case RE::InputEvent::DeviceType::kKeyboard: return "Keyboard";
			case RE::InputEvent::DeviceType::kMouse: return "Mouse";
			case RE::InputEvent::DeviceType::kGamepad: return "Gamepad";
			case RE::InputEvent::DeviceType::kKinect: return "Kinect";
			default: return "None";
			}
		}

		[[nodiscard]] std::string_view EventTypeName(const RE::InputEvent::EventType a_type) noexcept
		{
			switch (a_type) {
			case RE::InputEvent::EventType::kButton: return "Button";
			case RE::InputEvent::EventType::kMouseMove: return "MouseMove";
			case RE::InputEvent::EventType::kCursorMove: return "CursorMove";
			case RE::InputEvent::EventType::kChar: return "Char";
			case RE::InputEvent::EventType::kThumbstick: return "Thumbstick";
			case RE::InputEvent::EventType::kDeviceConnect: return "DeviceConnect";
			case RE::InputEvent::EventType::kKinect: return "Kinect";
			default: return "None";
			}
		}

		[[nodiscard]] bool CanLog()
		{
			return s_logLines.fetch_add(1) < Config::Get().maxInputLogLines;
		}

		// Raw words of an event that CommonLibSF has no layout for (thumbstick, mouse move), so that the layout
		// can be read off the log: the first few events of each (type, device, idCode), as hex and as float.
		void DumpRaw(const RE::InputEvent& a_event, const std::int32_t a_idCode)
		{
			const auto key = (static_cast<std::uint64_t>(a_event.eventType) << 48) ^ (static_cast<std::uint64_t>(a_event.deviceType) << 40) ^
			                 static_cast<std::uint32_t>(a_idCode);
			auto& count = s_rawDumps[key];
			if (count >= 3) {
				return;
			}
			++count;

			std::array<std::uint32_t, 24> words{};  // 0x60 bytes
			if (SafeCopy(words.data(), &a_event, sizeof(words)) != sizeof(words)) {
				COVER_DETAIL("S2 raw {} event: unreadable", EventTypeName(a_event.eventType));
				return;
			}
			std::string line;
			for (std::size_t i = 0x38 / 4; i < 0x50 / 4; ++i) {
				float f{};
				std::memcpy(&f, &words[i], sizeof(f));
				line += std::format(" +{:02X}=0x{:08X}({:.3f})", i * 4, words[i], std::isfinite(f) && std::fabs(f) < 1e6f ? f : 0.0f);
			}
			COVER_DETAIL("S2 raw {} event dev={} id@30={} :{}", EventTypeName(a_event.eventType), DeviceName(a_event.deviceType),
				static_cast<std::int32_t>(words[0x30 / 4]), line);
		}

		// The user event name of a button event, or "" when it has none.
		[[nodiscard]] std::string UserEventOf(const RE::ButtonEvent& a_event)
		{
			const char* text = a_event.strUserEvent.c_str();
			return text ? std::string(text) : std::string();
		}

		void LogFirstSeen(const RE::ButtonEvent& a_event, const std::string& a_name)
		{
			const auto key = (static_cast<std::uint64_t>(a_event.deviceType) << 40) ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a_event.idCode)) << 8) ^
			                 std::hash<std::string>{}(a_name);
			if (!s_seenKeys.insert(key).second) {
				return;
			}
			if (s_seenKeys.size() <= 250) {
				COVER_DETAIL("S2 new input: device={} idCode=0x{:02X} ({}) userEvent='{}' value={:.2f}", DeviceName(a_event.deviceType),
					static_cast<std::uint32_t>(a_event.idCode), a_event.idCode, a_name, a_event.value);
			}
		}

		void ToggleMode(std::atomic<bool>& a_mode, const char* a_label)
		{
			const bool now = !a_mode.load();
			a_mode.store(now);
			COVER_DETAIL("S2b {} {}", a_label, now ? "ON" : "off");
		}

		// Debug keys (S2b). Only presses: key repeat and release do nothing.
		void HandleDebugKey(const int a_idCode)
		{
			const auto& cfg = Config::Get();
			if (a_idCode == cfg.reloadKey) {
				s_reloadRequested = true;
			}
			if (cfg.worldProbe && a_idCode == cfg.probeKey) {
				s_probeRequested = true;
			}
			if (cfg.sensor && cfg.worldProbe && a_idCode == cfg.senseKey) {
				s_senseRequested = true;
			}
			if (cfg.camera && !Camera::Compat() && a_idCode == cfg.cameraFlipKey) {
				Camera::Flip();
			}
			if (cfg.camera && cfg.framing && cfg.shoulderKey != 0 && !Camera::Compat() && a_idCode == cfg.shoulderKey && ChordAllowKeySwap()) {
				Camera::SwapShoulder();
			}
			if (!cfg.inputRewrite) {
				return;
			}
			if (a_idCode == cfg.autoRewriteKey) {
				s_autoToggle = true;
			} else if (a_idCode == cfg.swallowJumpKey) {
				ToggleMode(s_swallowJump, std::format("swallow Jump presses (method {} = {})", s_method.load(), kMethodNames[s_method.load()]).c_str());
			} else if (a_idCode == cfg.zeroMoveKey) {
				ToggleMode(s_zeroMove, "movement value forced to 0");
			} else if (a_idCode == cfg.halfMoveKey) {
				ToggleMode(s_scaleMove, std::format("movement value scaled by {:.2f}", cfg.moveScale).c_str());
			} else if (a_idCode == cfg.swallowMethodKey) {
				const int next = (s_method.load() + 1) % kMethodCount;
				s_method.store(next);
				COVER_DETAIL("S2b swallow method is now {} = {}", next, kMethodNames[next]);
			}
		}

		// The PlayerCamera hook applies rewrites only when the controls hook is off or could not be installed.
		[[nodiscard]] bool RewriteAtCamera()
		{
			return !Config::Get().rewriteAtControls || !s_controlsInstalled.load() || s_controlsDisabled.load();
		}

		// 0 = leave movement alone, 1 = zero it, 2 = scale it. The manual debug toggles win over the event-driven test.
		[[nodiscard]] int MoveStage()
		{
			if (s_zeroMove.load()) {
				return 1;
			}
			if (s_scaleMove.load()) {
				return 2;
			}
			if (const int rig = s_rigMoveStage.load(); rig != 0) {
				return rig;
			}
			return RewriteTest::MoveStage();
		}

		// Applies a Jump plan to one event. Returns true when the event was unlinked from the queue.
		bool ApplyJumpPlan(const int a_plan, RE::ButtonEvent& a_event, RE::InputEvent* a_prev, const RE::InputEvent*& a_head, bool& a_unlinkedHead)
		{
			switch (static_cast<RewriteTest::JumpPlan>(a_plan)) {
			case RewriteTest::JumpPlan::Unlink:
				if (a_prev) {
					a_prev->next = a_event.next;
				} else {
					a_head = a_event.next;
					a_unlinkedHead = true;
				}
				return true;
			case RewriteTest::JumpPlan::DisabledFlag:
				a_event.disabled = true;
				break;
			case RewriteTest::JumpPlan::StatusStop:
				a_event.status = RE::InputEvent::Status::kStop;
				break;
			case RewriteTest::JumpPlan::ValueZero:
				a_event.value = 0.0f;
				break;
			default:
				break;
			}
			return false;
		}

		// Thumbstick events are IDEvents (user event name at +0x28, idCode at +0x30) whose two floats at
		// +0x38 / +0x3C are the stick's x and y. The layout comes from test run 1 (see Engine/Offsets.h); the S1
		// log line shows the stick values next to the player's speed so the next run can confirm it.
		void HandleThumbstick(RE::IDEvent& a_event)
		{
			const auto& cfg = Config::Get();
			const char* text = a_event.strUserEvent.c_str();
			const std::string name = text ? text : "";

			const auto key = (static_cast<std::uint64_t>(0x7000) << 40) ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a_event.idCode)) << 8) ^
			                 std::hash<std::string>{}(name);
			if (s_seenKeys.insert(key).second && s_seenKeys.size() <= 250) {
				COVER_DETAIL("S2 new input: device={} thumbstick idCode=0x{:02X} ({}) userEvent='{}'", DeviceName(a_event.deviceType),
					static_cast<std::uint32_t>(a_event.idCode), a_event.idCode, name);
			}
			DumpRaw(a_event, a_event.idCode);

			auto* xy = reinterpret_cast<float*>(reinterpret_cast<std::byte*>(&a_event) + kThumbstickX);
			static_assert(kThumbstickY == kThumbstickX + 4);
			const float x = xy[0];
			const float y = xy[1];

			if (name == "Move") {
				{
					std::lock_guard lock(s_stateMutex);
					s_state.SetStick(x, y);  // InputState sees the original values
				}
				if (x * x + y * y > 0.05f * 0.05f) {
					RewriteTest::NoteMoveDevice(false);
				}

				if (cfg.inputRewrite && RewriteAtCamera()) {
					const int stage = MoveStage();
					if (stage != 0) {
						const float k = stage == 1 ? 0.0f : cfg.moveScale;
						xy[0] = x * k;
						xy[1] = y * k;
					}
				}
			}
		}

		// Returns true when the event was unlinked from the queue (the caller then skips it in its own walk).
		bool HandleButton(RE::ButtonEvent& a_event, RE::InputEvent* a_prev, const RE::InputEvent*& a_head, bool& a_unlinkedHead)
		{
			const auto& cfg = Config::Get();
			const auto  name = UserEventOf(a_event);
			const bool  down = a_event.value > 0.0f;
			const bool  pressEdge = down && a_event.heldDownSecs <= 0.0f;
			const bool  releaseEdge = !down;
			const bool  keyboard = a_event.deviceType == RE::InputEvent::DeviceType::kKeyboard;

			LogFirstSeen(a_event, name);
			if ((pressEdge || releaseEdge) && CanLog()) {
				COVER_DETAIL("S2 button {} id=0x{:02X} '{}' {} value={:.2f} held={:.2f}s", DeviceName(a_event.deviceType),
					static_cast<std::uint32_t>(a_event.idCode), name, down ? "DOWN" : "up", a_event.value, a_event.heldDownSecs);
			}

			// InputState sees the original values, before any rewrite below.
			const auto action = Core::ClassifyUserEvent(name);
			{
				std::lock_guard lock(s_stateMutex);
				s_state.ApplyAction(action, down);
				if (keyboard && a_event.idCode == cfg.coverKey) {
					s_state.ApplyCoverKey(down);
				}
			}

			if (keyboard && pressEdge) {
				HandleDebugKey(a_event.idCode);
			}

			if (!cfg.inputRewrite || !RewriteAtCamera()) {
				return false;
			}

			// S2b: Jump. The release always passes, so the game cannot be left believing the key is still held.
			if (action == Core::Action::Jump) {
				int plan = 0;
				if (s_swallowJump.load() && down) {
					plan = s_method.load() + 1;  // manual toggle (Numpad 7 / 4): not counted by the test
				} else {
					plan = static_cast<int>(RewriteTest::OnJumpEvent(down, pressEdge, keyboard));
				}
				if (plan != 0) {
					return ApplyJumpPlan(plan, a_event, a_prev, a_head, a_unlinkedHead);
				}
			}

			// S2b: movement. Rewriting `value` (never removing the event) keeps press and release in step.
			if (Core::IsMovementAction(action)) {
				if (down) {
					RewriteTest::NoteMoveDevice(keyboard);
				}
				const int stage = MoveStage();
				if (stage != 0) {
					a_event.value = stage == 1 ? 0.0f : a_event.value * cfg.moveScale;
				}
			}
			return false;
		}

		// Walks the queue, updates InputState, applies the S2b rewrites. Returns the head to hand to the game.
		const RE::InputEvent* Process(const RE::InputEvent* a_head)
		{
			const RE::InputEvent* head = a_head;
			RE::InputEvent*       prev = nullptr;
			bool                  unlinkedHead = false;

			std::size_t n = 0;
			for (auto* ev = const_cast<RE::InputEvent*>(a_head); ev && n < kMaxChain; ++n) {
				auto*      next = ev->next;  // read first: the handler may unlink ev
				const auto type = static_cast<std::size_t>(ev->eventType);
				if (type < 8) {
					s_eventCounts[type].fetch_add(1);
				}

				bool removed = false;
				if (ev->eventType == RE::InputEvent::EventType::kButton) {
					removed = HandleButton(*static_cast<RE::ButtonEvent*>(ev), prev, head, unlinkedHead);
				} else if (ev->eventType == RE::InputEvent::EventType::kThumbstick) {
					HandleThumbstick(*static_cast<RE::IDEvent*>(ev));
				} else if (ev->eventType == RE::InputEvent::EventType::kMouseMove) {
					// Layout from run 1: two ints at +0x38 / +0x3C (dx, dy). Not used yet (look is not ours to rewrite).
					DumpRaw(*ev, 0);
				}

				if (unlinkedHead) {
					// head was advanced past ev; later events are unaffected
					unlinkedHead = false;
				}
				if (!removed) {
					prev = ev;
				}
				ev = next;
			}
			return head;
		}

		void Disable(const unsigned long a_code)
		{
			s_disabled = true;
			REX::ERROR("S2: exception 0x{:08X} in the input hook; the hook now passes events through untouched for this session", a_code);
		}

		void Hook(RE::BSInputEventReceiver* a_this, const RE::InputEvent* a_head)
		{
			const auto orig = s_orig;
			if (!orig) {
				return;
			}

			if (s_hookCalls.fetch_add(1) == 0) {
				s_hookThread = REX::W32::GetCurrentThreadId();
				COVER_DETAIL("S2 hook: first PerformInputProcessing call, receiver={} thread={}", static_cast<void*>(a_this), REX::W32::GetCurrentThreadId());
			}

			const RE::InputEvent* head = a_head;
			if (!s_disabled && a_head) {
				unsigned long code{ 0 };
				if (!Guarded([&] { head = Process(a_head); }, code)) {
					Disable(code);
					head = a_head;
				}
			}
			orig(a_this, head);
		}

		// ---- the rewrite hook: PlayerControls::Manager's receiver -----------------------------------------
		// Every change made to the event queue is recorded and undone after the original function returns, so nothing
		// outside that one call ever sees a modified queue (the first S2b run left an unlinked event behind and keyboard
		// events stopped arriving; this removes that possibility).
		struct Undo
		{
			struct Relink
			{
				RE::InputEvent* prev;
				RE::InputEvent* next;
			};
			struct FloatField
			{
				float* p;
				float  old;
			};
			struct BoolField
			{
				bool* p;
				bool  old;
			};
			struct StatusField
			{
				RE::InputEvent*           ev;
				RE::InputEvent::Status    old;
			};

			std::array<Relink, 16>      relinks{};
			std::size_t                 nRelinks{ 0 };
			std::array<FloatField, 32>  floats{};
			std::size_t                 nFloats{ 0 };
			std::array<BoolField, 16>   bools{};
			std::size_t                 nBools{ 0 };
			std::array<StatusField, 16> statuses{};
			std::size_t                 nStatuses{ 0 };
		};

		// Each setter records the old value first and does nothing when the log is full (an unrecorded change could not be undone).
		bool SetFloat(Undo& a_undo, float* a_field, const float a_value)
		{
			if (a_undo.nFloats >= a_undo.floats.size()) {
				return false;
			}
			a_undo.floats[a_undo.nFloats++] = { a_field, *a_field };
			*a_field = a_value;
			return true;
		}

		void UndoAll(Undo& a_undo)
		{
			while (a_undo.nStatuses > 0) {
				const auto& s = a_undo.statuses[--a_undo.nStatuses];
				s.ev->status = s.old;
			}
			while (a_undo.nBools > 0) {
				const auto& b = a_undo.bools[--a_undo.nBools];
				*b.p = b.old;
			}
			while (a_undo.nFloats > 0) {
				const auto& f = a_undo.floats[--a_undo.nFloats];
				*f.p = f.old;
			}
			while (a_undo.nRelinks > 0) {
				const auto& r = a_undo.relinks[--a_undo.nRelinks];
				r.prev->next = r.next;
			}
		}

		// Applies a Jump plan to one event with undo. Returns true when the event was unlinked.
		bool ApplyJumpPlanUndo(const int a_plan, RE::ButtonEvent& a_event, RE::InputEvent* a_prev, const RE::InputEvent*& a_head, Undo& a_undo)
		{
			switch (static_cast<RewriteTest::JumpPlan>(a_plan)) {
			case RewriteTest::JumpPlan::Unlink:
				if (a_prev) {
					if (a_undo.nRelinks >= a_undo.relinks.size()) {
						return false;
					}
					a_undo.relinks[a_undo.nRelinks++] = { a_prev, a_prev->next };
					a_prev->next = a_event.next;
				} else {
					a_head = a_event.next;  // the head is passed by value: nothing to restore
				}
				return true;
			case RewriteTest::JumpPlan::DisabledFlag:
				if (a_undo.nBools < a_undo.bools.size()) {
					a_undo.bools[a_undo.nBools++] = { &a_event.disabled, a_event.disabled };
					a_event.disabled = true;
				}
				break;
			case RewriteTest::JumpPlan::StatusStop:
				if (a_undo.nStatuses < a_undo.statuses.size()) {
					a_undo.statuses[a_undo.nStatuses++] = { &a_event, a_event.status };
					a_event.status = RE::InputEvent::Status::kStop;
				}
				break;
			case RewriteTest::JumpPlan::ValueZero:
				SetFloat(a_undo, &a_event.value, 0.0f);
				break;
			default:
				break;
			}
			return false;
		}

		// ---- T3.4 spike: a Sneak press made by the plugin ------------------------------------------------------------------------------
		// Design 1 (PLAN.md T3.4 item 1): a copy of a real ButtonEvent (96 bytes: consecutive events of one queue lie exactly 96 bytes apart, run p3-6, which
		// is also sizeof(RE::ButtonEvent) in CommonLibSF) with the user event name 'Sneak', the keyboard device and Left Ctrl as id code, value 1 and held time 0,
		// linked at the tail of the list for the one original call and unlinked afterwards (the existing relink undo). The release follows in the next call.
		constexpr std::size_t kButtonEventSize = 96;
		static_assert(sizeof(RE::ButtonEvent) == kButtonEventSize);
		static_assert(sizeof(RE::BSFixedString) == sizeof(void*));
		alignas(16) std::byte s_template[kButtonEventSize];  // a real button event, copied when one passes the hook (hook thread)
		bool                  s_haveTemplate{ false };
		alignas(16) std::byte s_madeBuf[kButtonEventSize];
		std::atomic<int>      s_pressRequests{ 0 };
		std::atomic<std::int64_t> s_pressRequestTicks{ 0 };  // steady-clock time of the latest request: an old one is dropped (a press that arrives late would undo a later one)
		std::atomic<std::uint32_t> s_madeExpired{ 0 };
		std::atomic<std::uint64_t> s_nullHeadCalls{ 0 };
		int                   s_madeStage{ 0 };  // 0 idle, 1 the release is due in the next call (hook thread)
		std::atomic<std::uint32_t> s_madeDelivered{ 0 };
		std::atomic<std::uint32_t> s_madeNoTemplate{ 0 };
		std::atomic<std::uint32_t> s_madeRequested{ 0 };

		[[nodiscard]] const RE::InputEvent* InjectMadeUpSneak(const RE::InputEvent* a_head, Undo& a_undo)
		{
			const bool release = s_madeStage == 1;
			if (!release && s_pressRequests.load() <= 0) {
				return a_head;
			}
			if (!release) {
				const auto age = std::chrono::duration<double>(std::chrono::steady_clock::duration(std::chrono::steady_clock::now().time_since_epoch().count() - s_pressRequestTicks.load())).count();
				if (age > 1.0) {
					s_madeExpired.fetch_add(static_cast<std::uint32_t>(s_pressRequests.exchange(0)));
					return a_head;
				}
			}
			if (!s_haveTemplate) {
				if (!release) {
					s_pressRequests.fetch_sub(1);
					s_madeNoTemplate.fetch_add(1);
				}
				return a_head;
			}
			static RE::BSFixedString s_sneakName{ "Sneak" };  // keeps the pooled string alive
			std::memcpy(s_madeBuf, s_template, kButtonEventSize);
			auto& ev = *reinterpret_cast<RE::ButtonEvent*>(s_madeBuf);
			ev.next = nullptr;
			std::memcpy(static_cast<void*>(&ev.strUserEvent), static_cast<const void*>(&s_sneakName), sizeof(void*));  // the pooled pointer, no reference counting
			ev.deviceType = RE::InputEvent::DeviceType::kKeyboard;
			ev.idCode = 0xA2;
			ev.value = release ? 0.0f : 1.0f;
			ev.heldDownSecs = release ? 0.05f : 0.0f;
			ev.disabled = false;
			ev.status = RE::InputEvent::Status::kUnhandled;
			RE::InputEvent* last = const_cast<RE::InputEvent*>(a_head);
			if (!last) {
				// nothing in the queue: the made-up event is the whole list
				a_head = &ev;
			} else {
				std::size_t k = 0;
				while (last->next && k++ < kMaxChain) {
					last = last->next;
				}
				if (a_undo.nRelinks >= a_undo.relinks.size()) {
					return a_head;
				}
				a_undo.relinks[a_undo.nRelinks++] = { last, last->next };
				last->next = &ev;
			}
			if (release) {
				s_madeStage = 0;
			} else {
				s_pressRequests.fetch_sub(1);
				s_madeStage = 1;
				s_madeDelivered.fetch_add(1);
			}
			return a_head;
		}

		// T5.4: a made-up fire press (the lost tap of blind fire): the template of a real button event with the user event PrimaryAttack, the device and id of the real control, linked at the tail
		// of the queue in every call while it is held, the last one with value 0.
		std::atomic<bool>  s_fireReq{ false };
		std::atomic<int>   s_fireDev{ 0 };
		std::atomic<int>   s_fireId{ 0 };
		std::atomic<const void*> s_fireNamePtr{ nullptr };  // T5.6: the pooled name of the real control when the press is not PrimaryAttack
		std::atomic<float> s_fireSecs{ 0.15f };
		std::atomic<std::uint32_t> s_fireDelivered{ 0 };
		bool               s_fireActive{ false };
		bool               s_fireFirst{ false };
		std::chrono::steady_clock::time_point s_fireT0;
		alignas(16) std::byte s_madeFireBuf[kButtonEventSize];

		[[nodiscard]] const RE::InputEvent* InjectMadeFire(const RE::InputEvent* a_head, Undo& a_undo)
		{
			if (!s_fireActive) {
				if (!s_fireReq.exchange(false) || !s_haveTemplate) {
					return a_head;
				}
				s_fireActive = true;
				s_fireFirst = true;
				s_fireT0 = std::chrono::steady_clock::now();
			}
			const float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - s_fireT0).count();
			const bool  release = !s_fireFirst && elapsed >= s_fireSecs.load();
			static RE::BSFixedString s_fireName{ "PrimaryAttack" };
			std::memcpy(s_madeFireBuf, s_template, kButtonEventSize);
			auto& ev = *reinterpret_cast<RE::ButtonEvent*>(s_madeFireBuf);
			ev.next = nullptr;
			const void* namePtr = s_fireNamePtr.load();
			std::memcpy(static_cast<void*>(&ev.strUserEvent), namePtr ? &namePtr : static_cast<const void*>(&s_fireName), sizeof(void*));
			ev.deviceType = static_cast<RE::InputEvent::DeviceType>(s_fireDev.load());
			ev.idCode = static_cast<decltype(ev.idCode)>(s_fireId.load());
			ev.value = release ? 0.0f : 1.0f;
			ev.heldDownSecs = s_fireFirst ? 0.0f : elapsed;
			ev.disabled = false;
			ev.status = RE::InputEvent::Status::kUnhandled;
			RE::InputEvent* last = const_cast<RE::InputEvent*>(a_head);
			if (!last) {
				a_head = &ev;
			} else {
				std::size_t k = 0;
				while (last->next && k++ < kMaxChain) {
					last = last->next;
				}
				if (a_undo.nRelinks >= a_undo.relinks.size()) {
					return a_head;
				}
				a_undo.relinks[a_undo.nRelinks++] = { last, last->next };
				last->next = &ev;
			}
			if (s_fireFirst) {
				s_fireFirst = false;
				s_fireDelivered.fetch_add(1);
			}
			if (release) {
				s_fireActive = false;
			}
			return a_head;
		}

		// T5.6: weapon controls (REFERENCE 22.12). The names are the game's own user events (Starfield.exe strings: Quickkey1..12, QuickkeyUp/Down/Left/Right, Favorites, and the menu's own
		// Accept/Cancel/Left/Right/Up/Down that the log of the play of 2026-11-17 21:04 shows between the wheel's opening and the choice).
		bool               s_qkPending{ false };
		const void*        s_qkName{ nullptr };
		int                s_qkDev{ 0 }, s_qkId{ 0 };
		std::chrono::steady_clock::time_point s_qkT0;
		// T5.7: the reload control (a tap reloads, a hold holsters: the same user event WeaponReadyReload)
		bool               s_rlActive{ false };   // the press was held back, the control is still down (tap or hold not known yet)
		bool               s_rlReplay{ false };   // the control was let go as a tap: made again when the pose is over
		float              s_rlWs3{ -1.0f };
		std::atomic<float> s_lookA{ 0.0f }, s_lookB{ 0.0f };   // T5.8: sums of the mouse move ints since the camera hook last took them
		std::atomic<float> s_lookSX{ 0.0f }, s_lookSY{ 0.0f }; // the latest right stick
		bool               s_hsEat{ false };      // the lock: the rest of a holster hold is swallowed
		int                s_hsDev{ 0 }, s_hsId{ 0 };
		bool               s_rlPassNext{ false }; // the control is held long enough to be a holster: the next repeat passes as a fresh press
		const void*        s_rlName{ nullptr };
		int                s_rlDev{ 0 }, s_rlId{ 0 };
		std::chrono::steady_clock::time_point s_rlT0;

		[[nodiscard]] bool IsNumberQuickkey(const std::string& a_name)
		{
			if (a_name.size() <= 8 || a_name.compare(0, 8, "Quickkey") != 0) {
				return false;
			}
			for (std::size_t i = 8; i < a_name.size(); ++i) {
				if (a_name[i] < '0' || a_name[i] > '9') {
					return false;
				}
			}
			return true;
		}

		void WeaponControl(RE::ButtonEvent& a_be, const std::string& a_name, const bool a_down, const bool a_pressEdge, Undo& a_undo)
		{
			const bool wheelKey = a_name == "QuickkeyUp" || a_name == "QuickkeyDown" || a_name == "QuickkeyLeft" || a_name == "QuickkeyRight" || a_name == "Favorites";
			const bool numKey = IsNumberQuickkey(a_name);
			const auto& cfgw = Config::Get();
			bool        lockedHide = false;
			if ((wheelKey || numKey) && cfgw.poseWeaponSwap == 0 && CoverPose::Active()) {
				// the user's decision (card M3b): no weapon swap in the pose: the press never reaches the game, the wheel does not open
				lockedHide = true;
				if (a_pressEdge) {
					COVER_DETAIL("WEAPON CONTROL: {} pressed in the pose: swallowed (iPoseWeaponSwap 0, the weapon is locked while the pose is on)", a_name);
				}
			}
			if (lockedHide) {
				if (a_undo.nBools < a_undo.bools.size()) {
					a_undo.bools[a_undo.nBools++] = { &a_be.disabled, a_be.disabled };
					a_be.disabled = true;
				}
				return;
			}
			if (a_down && (a_name == "Accept" || a_name == "Cancel")) {
				CoverPose::NoteWheelEvent(true);
			} else if (a_down && (a_name == "Left" || a_name == "Right" || a_name == "Up" || a_name == "Down")) {
				CoverPose::NoteWheelEvent(false);
			}
			if (wheelKey && a_pressEdge) {
				CoverPose::NoteWeaponControl(a_name.c_str(), true);
			} else if (numKey && a_pressEdge) {
				CoverPose::NoteWeaponControl(a_name.c_str(), false);
				if (CoverPose::Busy() && !s_qkPending && cfgw.poseWeaponSwap < 2) {
					s_qkPending = true;
					std::memcpy(&s_qkName, static_cast<const void*>(&a_be.strUserEvent), sizeof(void*));
					s_qkDev = static_cast<int>(a_be.deviceType);
					s_qkId = static_cast<int>(a_be.idCode);
					s_qkT0 = std::chrono::steady_clock::now();
					COVER_DETAIL("WEAPON CONTROL: {} pressed while the pose is {}: held back, made again when the own graph is back", a_name, CoverPose::ToString(CoverPose::GetInfo().state));
				}
			} else if (a_name == "WeaponReadyReload" && a_down && a_be.heldDownSecs >= Config::Get().poseHolsterHold) {
				CoverPose::NoteWeaponControl("WeaponReadyReload held", false);
			}
			// T5.7: the reload control in the pose
			if (a_name == "WeaponReadyReload") {
				const auto& cfg = Config::Get();
				const bool  sameKey = static_cast<int>(a_be.deviceType) == s_rlDev && static_cast<int>(a_be.idCode) == s_rlId;
				bool        hide = false;
				if (a_pressEdge && cfg.poseReload && CoverPose::WeaponState() == 3) {
					CoverPose::NoteReload(cfg.poseReloadTime);
					if (CoverPose::Busy() && !s_rlActive && !s_rlReplay) {
						s_rlActive = true;
						s_rlPassNext = false;
						std::memcpy(&s_rlName, static_cast<const void*>(&a_be.strUserEvent), sizeof(void*));
						s_rlDev = static_cast<int>(a_be.deviceType);
						s_rlId = static_cast<int>(a_be.idCode);
						s_rlT0 = std::chrono::steady_clock::now();
						hide = true;
						COVER_DETAIL("WEAPON CONTROL: the reload control pressed while the pose is {}: held back until it is known whether it is a tap (made again when the own graph is back) or a hold (holster)", CoverPose::ToString(CoverPose::GetInfo().state));
					}
				} else if (s_rlActive && sameKey) {
					if (!a_down) {
						s_rlActive = false;
						if (a_be.heldDownSecs < cfg.poseHolsterHold) {
							s_rlReplay = true;
							s_rlWs3 = -1.0f;
							s_rlT0 = std::chrono::steady_clock::now();
							hide = true;
						} else {
							COVER_DETAIL("WEAPON CONTROL: the reload control was let go after {:.2f} s: a holster, the release passes", a_be.heldDownSecs);
						}
					} else if (a_be.heldDownSecs >= cfg.poseHolsterHold) {
						s_rlActive = false;
						s_rlPassNext = true;
						COVER_DETAIL("WEAPON CONTROL: the reload control is held {:.2f} s: a holster, the events pass from now on (the first as a fresh press)", a_be.heldDownSecs);
					} else {
						hide = true;
					}
				} else if (s_rlReplay && sameKey) {
					hide = true;
				}
				if (s_rlPassNext && sameKey && a_down) {
					s_rlPassNext = false;
					SetFloat(a_undo, &a_be.heldDownSecs, 0.0f);
				}
				if (hide && a_undo.nBools < a_undo.bools.size()) {
					a_undo.bools[a_undo.nBools++] = { &a_be.disabled, a_be.disabled };
					a_be.disabled = true;
				}
			}
			if (s_qkPending && numKey && static_cast<int>(a_be.deviceType) == s_qkDev && static_cast<int>(a_be.idCode) == s_qkId && a_undo.nBools < a_undo.bools.size()) {
				a_undo.bools[a_undo.nBools++] = { &a_be.disabled, a_be.disabled };
				a_be.disabled = true;
			}
		}

		// the held back number key: made again once the pose is gone (or after 3 s, then dropped)
		void ServiceHeldKey()
		{
			if (s_rlReplay) {
				const float waited = std::chrono::duration<float>(std::chrono::steady_clock::now() - s_rlT0).count();
				// the weapon state dips to 2 for about 0.3 s when the own graph is back (a press then is not a reload): wait for 3, steady for 0.25 s
				if (!CoverPose::Busy() && CoverPose::WeaponState() == 3) {  // counted only once the pose is gone (run m3c-3: the replay came inside the dip and the reload did not start)
					if (s_rlWs3 < 0.0f) {
						s_rlWs3 = waited;
					}
				} else {
					s_rlWs3 = -1.0f;
				}
				if (!CoverPose::Busy() && waited >= 0.1f && s_rlWs3 >= 0.0f && waited - s_rlWs3 >= 0.25f) {
					s_rlReplay = false;
					RequestMadeButton(s_rlName, s_rlDev, s_rlId, 0.08f);
					CoverPose::NoteReload(Config::Get().poseReloadTime);  // the time counts from the reload itself
					COVER_DETAIL("WEAPON CONTROL: the held back reload control is made again as a tap (the pose is over)");
				} else if (waited > 3.0f) {
					s_rlReplay = false;
					REX::WARN("WEAPON CONTROL: the held back reload control is dropped after 3 s (the pose did not end)");
				}
			}
			if (!s_qkPending) {
				return;
			}
			const float waited = std::chrono::duration<float>(std::chrono::steady_clock::now() - s_qkT0).count();
			if (!CoverPose::Busy() && waited >= 0.1f) {
				s_qkPending = false;
				RequestMadeButton(s_qkName, s_qkDev, s_qkId, 0.15f);
				COVER_DETAIL("WEAPON CONTROL: the held back quick-key is made again {:.2f} s after the press (the pose is over)", waited);
			} else if (waited > 3.0f) {
				s_qkPending = false;
				REX::WARN("WEAPON CONTROL: the held back quick-key is dropped after 3 s (the pose did not end)");
			}
		}

		// T3.4 spike, evidence for the size of a button event: the addresses of the events of one queue (hold two keys and the aim button together) and the
		// distance between neighbours. Logged for the first 16 queues that hold two or more events.
		std::atomic<int> s_queueLogs{ 0 };

		void LogQueueLayout(const RE::InputEvent* a_head)
		{
			if (s_queueLogs.load() >= 16 || !a_head) {
				return;
			}
			std::string   line;
			std::intptr_t prevAddr = 0;
			std::size_t   k = 0;
			for (const RE::InputEvent* e = a_head; e && k < 16; e = e->next, ++k) {
				const auto addr = reinterpret_cast<std::intptr_t>(e);
				line += std::format(" [{} 0x{:X} type={} dev={}", k, static_cast<std::uint64_t>(addr), static_cast<int>(e->eventType), static_cast<int>(e->deviceType));
				if (prevAddr != 0) {
					line += std::format(" +{} from the previous", addr - prevAddr);
				}
				if (e->eventType == RE::InputEvent::EventType::kButton) {
					const auto& b = *static_cast<const RE::ButtonEvent*>(e);
					line += std::format(" '{}' id=0x{:X} value={:.2f} held={:.2f}", UserEventOf(b), static_cast<std::uint32_t>(b.idCode), b.value, b.heldDownSecs);
				}
				line += "]";
				prevAddr = addr;
			}
			if (k >= 2) {
				s_queueLogs.fetch_add(1);
				COVER_DETAIL("S3 queue layout ({} events, CommonLibSF sizeof(ButtonEvent)={}, sizeof(IDEvent)={}):{}", k, sizeof(RE::ButtonEvent), sizeof(RE::IDEvent), line);
			}
		}

		const RE::InputEvent* ProcessControls(const RE::InputEvent* a_head, Undo& a_undo)
		{
			const auto&           cfg = Config::Get();
			const RE::InputEvent* head = a_head;
			RE::InputEvent*       prev = nullptr;

			LogQueueLayout(a_head);

			std::size_t n = 0;
			for (auto* ev = const_cast<RE::InputEvent*>(a_head); ev && n < kMaxChain; ++n) {
				auto* next = ev->next;
				bool  removed = false;

				if (ev->eventType == RE::InputEvent::EventType::kButton) {
					auto&      be = *static_cast<RE::ButtonEvent*>(ev);
					if (!s_haveTemplate && SafeCopy(s_template, &be, kButtonEventSize) == kButtonEventSize) {
						s_haveTemplate = true;
					}
					const auto uname = UserEventOf(be);
					const auto action = Core::ClassifyUserEvent(uname);
					const bool down = be.value > 0.0f;
					const bool pressEdge = down && be.heldDownSecs <= 0.0f;
					const bool keyboard = be.deviceType == RE::InputEvent::DeviceType::kKeyboard;

					// Release 1.0.0: LT + A swaps the shoulder; the A press of the chord, its repeats and its release never reach the game
					if (be.deviceType == RE::InputEvent::DeviceType::kGamepad && (be.idCode == kPadLeftTriggerId || be.idCode == kPadButtonAId) && !s_chordFaulted.load()) {
						Core::ChordVerdict verdict = Core::ChordVerdict::Pass;
						unsigned long      chordCode{ 0 };
						const bool         isTrigger = be.idCode == kPadLeftTriggerId;
						const float        value = be.value;
						if (!Guarded([&] {
								std::lock_guard lock(s_chordMutex);
								s_chord.SetSettings({ ChordWanted(), cfg.shoulderPadTrigger, 0.25 });
								if (isTrigger) {
									s_chord.OnTrigger(value);
								} else {
									verdict = s_chord.OnAButton(down, pressEdge, ChordNow());
								}
							}, chordCode)) {
							s_chordFaulted = true;
							verdict = Core::ChordVerdict::Pass;
							REX::ERROR("SHOULDER CHORD: exception 0x{:08X}; LT + A is off for this session (the keyboard key still swaps)", chordCode);
						}
						if (verdict == Core::ChordVerdict::SwapAndSwallow) {
							Camera::SwapShoulder();
							COVER_DETAIL("SHOULDER CHORD: LT + A: the shoulder swaps; the A press is kept from the game ('{}')", uname);
						}
						if (verdict != Core::ChordVerdict::Pass && a_undo.nBools < a_undo.bools.size()) {
							a_undo.bools[a_undo.nBools++] = { &be.disabled, be.disabled };
							be.disabled = true;
						}
					}

					// T5.6: the weapon controls end the pose (REFERENCE 22.12). A number quick-key press that comes while a pose is on or being given back is held back and made again
					// when the own graph is back, so that the equip happens on the own graph.
					if (cfg.coverPoses && !uname.empty()) {
						WeaponControl(be, uname, down, pressEdge, a_undo);
					}

					// Phase 3: the cover logic sees the Sneak, Jump and Sprint presses first. A press it swallows is hidden with the `disabled` flag
					// (undone after the original returns, like every other change here); deciding inside this call is the whole point.
					if (action == Core::Action::Aim) {
						const auto aimPlan = Cover::OnButton(action, be.value > 0.3f, pressEdge, static_cast<int>(be.deviceType), static_cast<std::int32_t>(be.idCode), 0);  // T3.4: aim held (SecondaryAttack beyond 0.3)
						if (aimPlan.swallow && a_undo.nBools < a_undo.bools.size()) {
							a_undo.bools[a_undo.nBools++] = { &be.disabled, be.disabled };  // T5.4: an aim press during blind fire waits for the Out clip
							be.disabled = true;
						}
						if (aimPlan.resetHeld) {
							SetFloat(a_undo, &be.heldDownSecs, 0.0f);
						}
					}
					if (action == Core::Action::Fire) {
						const auto firePlan = BlindFire::OnFire(down, pressEdge, static_cast<int>(be.deviceType), static_cast<int>(be.idCode));  // T5.4: blind fire in the pose
						if (firePlan.swallow && a_undo.nBools < a_undo.bools.size()) {
							a_undo.bools[a_undo.nBools++] = { &be.disabled, be.disabled };
							be.disabled = true;
						}
						if (firePlan.resetHeld) {
							SetFloat(a_undo, &be.heldDownSecs, 0.0f);
						}
					}
					if (action == Core::Action::Sneak || action == Core::Action::Jump || action == Core::Action::Sprint) {
						const std::uint64_t coverId = reinterpret_cast<std::uintptr_t>(&be) ^ (static_cast<std::uint64_t>(be.timeCode) << 40) ^ 0x2;
						const auto          coverPlan = Cover::OnButton(action, down, pressEdge, static_cast<int>(be.deviceType), static_cast<std::int32_t>(be.idCode), coverId);
						if (coverPlan.swallow && a_undo.nBools < a_undo.bools.size()) {
							a_undo.bools[a_undo.nBools++] = { &be.disabled, be.disabled };
							be.disabled = true;
						}
					}

					if (keyboard && cfg.coverTrigger == 0 && be.idCode == cfg.coverKey) {
						Cover::OnCoverKey(down, pressEdge, reinterpret_cast<std::uintptr_t>(&be) ^ (static_cast<std::uint64_t>(be.timeCode) << 40) ^ 0x3);  // T3.5: the dedicated key
					}

					if (action == Core::Action::Jump) {
						int plan = 0;
						if (s_swallowJump.load() && down) {
							plan = s_method.load() + 1;  // manual toggle: not counted by the test
						} else {
							const std::uint64_t id = reinterpret_cast<std::uintptr_t>(&be) ^ (static_cast<std::uint64_t>(be.timeCode) << 40) ^ 0x1;
							plan = static_cast<int>(RewriteTest::OnJumpEvent(down, pressEdge, keyboard, id));
						}
						if (plan != 0) {
							removed = ApplyJumpPlanUndo(plan, be, prev, head, a_undo);
						}
					} else if (Core::IsMovementAction(action)) {
						if (down) {
							RewriteTest::NoteMoveDevice(keyboard);
						}
						float coverValue = be.value;  // Phase 3: zero while the player slides to the cover (InputState and the cover logic see the original)
						Cover::OnMoveKey(action, down, coverValue);
						if (coverValue != be.value) {
							SetFloat(a_undo, &be.value, coverValue);
						}
						const int stage = MoveStage();
						if (stage != 0) {
							SetFloat(a_undo, &be.value, stage == 1 ? 0.0f : be.value * cfg.moveScale);
						}
					}
				} else if (ev->eventType == RE::InputEvent::EventType::kMouseMove) {
					std::int32_t mx{ 0 }, my{ 0 };
					if (SafeRead(reinterpret_cast<const std::byte*>(ev) + 0x38, mx) && SafeRead(reinterpret_cast<const std::byte*>(ev) + 0x3C, my)) {
						s_lookA.store(s_lookA.load() + static_cast<float>(mx));
						s_lookB.store(s_lookB.load() + static_cast<float>(my));
					}
				} else if (ev->eventType == RE::InputEvent::EventType::kThumbstick) {
					auto&       ide = *static_cast<RE::IDEvent*>(ev);
					const char* text = ide.strUserEvent.c_str();
					if (text && std::string_view(text) == "Look") {
						auto* xy = reinterpret_cast<float*>(reinterpret_cast<std::byte*>(&ide) + kThumbstickX);
						s_lookSX.store(xy[0]);
						s_lookSY.store(xy[1]);
					}
					if (text && std::string_view(text) == "Move") {
						auto*       xy = reinterpret_cast<float*>(reinterpret_cast<std::byte*>(&ide) + kThumbstickX);
						s_rawMoveMag.store(std::sqrt(xy[0] * xy[0] + xy[1] * xy[1]));  // read before any rewrite below
						s_rawMoveTicks.store(std::chrono::steady_clock::now().time_since_epoch().count());
						float coverX = xy[0];
						float coverY = xy[1];
						Cover::OnMoveStick(coverX, coverY);  // Phase 3: zero while the player slides to the cover
						if (coverX != xy[0] || coverY != xy[1]) {
							SetFloat(a_undo, &xy[0], coverX);
							SetFloat(a_undo, &xy[1], coverY);
						}
						const int   stage = MoveStage();
						if (stage != 0) {
							const float k = stage == 1 ? 0.0f : cfg.moveScale;
							const float x = xy[0];
							const float y = xy[1];
							SetFloat(a_undo, &xy[0], x * k);
							SetFloat(a_undo, &xy[1], y * k);
						}
					}
				}

				if (!removed) {
					prev = ev;
				}
				ev = next;
			}
			return head;
		}

		// ---- T3.3b: the game movement vector, read and rewritten after the original returned ---------------------------------------------------
		// PlayerControls::Manager holds the movement input as two floats (offsets in Offsets.h). The original zeroes them at its start and the handlers set them while the
		// events are walked, so a write after it returns is the whole answer for that call; the next call starts from zero again and nothing needs undoing.
		std::atomic<bool>          s_overrideOn{ false };  // the rig write test: write this vector instead of the game one
		std::atomic<float>         s_overrideX{ 0.0f };
		std::atomic<float>         s_overrideY{ 0.0f };
		std::atomic<bool>          s_probeLog{ false };    // the rig probe: log every change of the vector
		std::atomic<int>           s_probeLines{ 0 };
		std::atomic<float>         s_vecX{ 0.0f }, s_vecY{ 0.0f }, s_vecZ{ 0.0f }, s_vecPrevX{ 0.0f }, s_vecPrevY{ 0.0f };
		std::atomic<std::uint64_t> s_vecCalls{ 0 };
		std::atomic<std::uint64_t> s_vecWrites{ 0 };
		std::atomic<float>         s_wroteX{ 0.0f }, s_wroteY{ 0.0f };
		std::atomic<bool>          s_wroteLast{ false };
		std::atomic<bool>          s_vecFaulted{ false };
		std::atomic<std::uint64_t> s_lastVecKey{ ~0ull };

		void PostOriginal(RE::BSInputEventReceiver* a_this)
		{
			if (s_vecFaulted.load()) {
				return;
			}
			auto*  base = reinterpret_cast<std::byte*>(a_this);
			float x{ 0 }, y{ 0 }, z{ 0 }, px{ 0 }, py{ 0 };
			if (!SafeRead(base + kManagerMoveX, x) || !SafeRead(base + kManagerMoveY, y) || !SafeRead(base + kManagerMoveZ, z) || !SafeRead(base + kManagerPrevMoveX, px) || !SafeRead(base + kManagerPrevMoveY, py)) {
				s_vecFaulted = true;
				REX::ERROR("T3.3b: the movement vector of PlayerControls::Manager cannot be read; the movement filter is off for this session");
				return;
			}
			if (!std::isfinite(x) || !std::isfinite(y)) {
				return;
			}
			s_vecX = x;
			s_vecY = y;
			s_vecZ = z;
			s_vecPrevX = px;
			s_vecPrevY = py;
			s_vecCalls.fetch_add(1);
			if (s_probeLog.load() && s_probeLines.load() < 160) {
				std::uint32_t kx{ 0 }, ky{ 0 }, kz{ 0 }, kpx{ 0 }, kpy{ 0 };
				std::memcpy(&kx, &x, 4);
				std::memcpy(&ky, &y, 4);
				std::memcpy(&kz, &z, 4);
				std::memcpy(&kpx, &px, 4);
				std::memcpy(&kpy, &py, 4);
				const std::uint64_t key = (static_cast<std::uint64_t>(kx) * 1000003ull) ^ (static_cast<std::uint64_t>(ky) * 998244353ull) ^ (static_cast<std::uint64_t>(kz) * 7919ull) ^ (static_cast<std::uint64_t>(kpx) << 17) ^ (static_cast<std::uint64_t>(kpy) << 29);
				if (s_lastVecKey.exchange(key) != key) {
					s_probeLines.fetch_add(1);
					COVER_DETAIL("S3 manager vector after the original returned: +0x58 x={:+.3f}, +0x5C y={:+.3f}, +0x60 z={:+.3f} | the copy at +0x7C {:+.3f}, +0x80 {:+.3f}", x, y, z, px, py);
				}
			}
			float wx = x;
			float wy = y;
			bool  write = false;
			const auto f = Cover::FilterMovementVector(x, y);
			if (f.write) {
				wx = f.x;
				wy = f.y;
				write = true;
			}
			if (s_overrideOn.load()) {
				wx = s_overrideX.load();
				wy = s_overrideY.load();
				write = true;
			}
			s_wroteX = write ? wx : x;
			s_wroteY = write ? wy : y;
			s_wroteLast = write && (wx != x || wy != y);
			if (write && (wx != x || wy != y)) {
				unsigned long code{ 0 };
				if (!Guarded([&] {
						*reinterpret_cast<float*>(base + kManagerMoveX) = wx;
						*reinterpret_cast<float*>(base + kManagerMoveY) = wy;
					}, code)) {
					s_vecFaulted = true;
					REX::ERROR("T3.3b: exception 0x{:08X} writing the movement vector; the movement filter is off for this session", code);
				}
				s_vecWrites.fetch_add(1);
			}
		}

		void HookControls(RE::BSInputEventReceiver* a_this, const RE::InputEvent* a_head)
		{
			const auto orig = s_origControls;
			if (!orig) {
				return;
			}
			if (s_controlsCalls.fetch_add(1) == 0) {
				COVER_DETAIL("S2b rewrite hook: first PlayerControls::Manager::PerformInputProcessing call, receiver={} thread={}", static_cast<void*>(a_this),
					REX::W32::GetCurrentThreadId());
			}

			const auto&           cfg = Config::Get();
			const RE::InputEvent* head = a_head;
			Undo                  undo{};
			if (!a_head) {
			s_nullHeadCalls.fetch_add(1);
		}
		if (!s_controlsDisabled && cfg.inputRewrite && cfg.rewriteAtControls) {
				unsigned long code{ 0 };
				if (!Guarded([&] {
						if (a_head) {
					head = ProcessControls(a_head, undo);
				}
						head = InjectMadeUpSneak(head, undo);  // after the walk: the cover logic never sees its own press
						ServiceHeldKey();
						head = InjectMadeFire(head, undo);
					}, code)) {
					// Undo first, so that the original never sees a partly modified queue, then call it with the untouched head.
					s_controlsDisabled = true;
					head = a_head;
					unsigned long undoCode{ 0 };
					(void)Guarded([&] { UndoAll(undo); }, undoCode);
					REX::ERROR("S2b: exception 0x{:08X} in the rewrite hook; queue changes undone, events pass through untouched for this session", code);
				}
			}

			orig(a_this, head);

			// T3.3b: the game vector, after the original (the queue changes above are undone below, the vector needs no undo)
			if (!s_controlsDisabled && cfg.inputRewrite && cfg.rewriteAtControls) {
				unsigned long vcode{ 0 };
				if (!Guarded([&] { PostOriginal(a_this); }, vcode)) {
					s_vecFaulted = true;
					REX::ERROR("T3.3b: exception 0x{:08X} in the movement vector filter; it is off for this session", vcode);
				}
			}

			// Always undone, also after a fault above.
			unsigned long code{ 0 };
			if (!Guarded([&] { UndoAll(undo); }, code)) {
				s_controlsDisabled = true;
				REX::ERROR("S2b: exception 0x{:08X} while undoing queue changes; the rewrite hook is off for this session", code);
			}
		}

		void TryInstallControlsImpl()
		{
			const auto vtbl = kControlsManagerReceiverVtable.address();
			const auto slotAddr = vtbl + sizeof(void*) * kPerformInputProcessingSlot;
			std::uintptr_t slot{ 0 };
			if (!SafeRead(reinterpret_cast<const void*>(slotAddr), slot)) {
				REX::ERROR("S2b: cannot read slot {} of the PlayerControls::Manager receiver vtable (ID {}); rewrite hook NOT installed", kPerformInputProcessingSlot,
					kControlsManagerReceiverVtable.id());
				s_controlsGaveUp = true;
				return;
			}
			const auto rva = slot - REX::FModule::GetExecutingModule().GetBaseAddress();
			REX::INFO("S2b: PlayerControls::Manager receiver vtable ID {} at RVA 0x{:X}; slot {} points to RVA 0x{:X} (expected 0x{:X})",
				kControlsManagerReceiverVtable.id(), vtbl - REX::FModule::GetExecutingModule().GetBaseAddress(), kPerformInputProcessingSlot, rva,
				kControlsManagerPerformInputRva);
			if (rva != kControlsManagerPerformInputRva) {
				REX::ERROR("S2b: slot {} is not the expected function; rewrite hook NOT installed (another plugin may have hooked it, or the runtime differs)", kPerformInputProcessingSlot);
				s_controlsGaveUp = true;
				return;
			}
			s_origControls = reinterpret_cast<Orig_t>(slot);
			REL::Relocation<std::uintptr_t> r{ vtbl };
			r.write_vfunc(kPerformInputProcessingSlot, reinterpret_cast<std::uintptr_t>(&HookControls));
			s_controlsInstalled = true;
			REX::INFO("S2b: rewrite hook installed on PlayerControls::Manager's receiver, slot {}", kPerformInputProcessingSlot);
			if (Config::Get().shoulderPadChord && !s_chordLogged.exchange(true)) {
				REX::INFO("SHOULDER CHORD: installed: LT + A swaps the shoulder (left trigger from {:.2f}), the A press is kept from the game; keyboard key 0x{:02X} too", Config::Get().shoulderPadTrigger, Config::Get().shoulderKey);
			}
		}

		[[nodiscard]] std::uintptr_t ModuleBase()
		{
			return REX::FModule::GetExecutingModule().GetBaseAddress();
		}

		void TryInstallImpl()
		{
			const auto camera = RE::PlayerCamera::GetSingleton();
			if (!camera) {
				if (!s_waitingLogged) {
					s_waitingLogged = true;
					COVER_DETAIL("S2: PlayerCamera does not exist yet; will retry");
				}
				return;
			}

			std::uintptr_t livePtr{ 0 };
			const auto     receiver = reinterpret_cast<const std::byte*>(camera) + kPlayerCameraInputReceiverOffset;
			if (!SafeRead(receiver, livePtr)) {
				REX::ERROR("S2: cannot read the vtable pointer at PlayerCamera+0x{:X}; hook not installed", static_cast<std::uint32_t>(kPlayerCameraInputReceiverOffset));
				s_gaveUp = true;
				return;
			}

			// Which of the ten PlayerCamera vtables does the live sub-object use?
			int matchIndex{ -1 };
			for (std::size_t i = 0; i < RE::VTABLE::PlayerCamera.size(); ++i) {
				if (RE::VTABLE::PlayerCamera[i].address() == livePtr) {
					matchIndex = static_cast<int>(i);
				}
			}

			const auto expected = kPlayerCameraInputReceiverVtable.address();
			COVER_DETAIL("S2: PlayerCamera={} receiver vptr=0x{:X} (RVA 0x{:X}); expected ID {} at 0x{:X} (RVA 0x{:X}); matches RE::VTABLE::PlayerCamera[{}]",
				static_cast<const void*>(camera), livePtr, livePtr - ModuleBase(), kPlayerCameraInputReceiverVtable.id(), expected,
				expected - ModuleBase(), matchIndex);

			if (livePtr != expected) {
				REX::ERROR("S2: the live receiver vtable is not the expected one; hook NOT installed (see matches= above for the right ID)");
				s_gaveUp = true;
				return;
			}

			// Slot 1 must still hold the function we saw offline, otherwise somebody else already hooked it.
			std::uintptr_t original{ 0 };
			if (!SafeRead(reinterpret_cast<const void*>(expected + sizeof(void*) * kPerformInputProcessingSlot), original)) {
				REX::ERROR("S2: cannot read vtable slot {}; hook not installed", kPerformInputProcessingSlot);
				s_gaveUp = true;
				return;
			}
			REX::INFO("S2: slot {} currently points to RVA 0x{:X} (offline expectation 0x{:X}){}", kPerformInputProcessingSlot, original - ModuleBase(),
				kPerformInputProcessingRva, original - ModuleBase() == kPerformInputProcessingRva ? "" : "  <== differs: another plugin may have hooked it; installing anyway on top");

			s_orig = reinterpret_cast<Orig_t>(original);
			REL::Relocation<std::uintptr_t> vtbl{ expected };
			vtbl.write_vfunc(kPerformInputProcessingSlot, reinterpret_cast<std::uintptr_t>(&Hook));
			s_installed = true;
			REX::INFO("S2: input hook installed on PlayerCamera's BSInputEventReceiver vtable, slot {}", kPerformInputProcessingSlot);
		}

		// ---- S2b automatic cycle ---------------------------------------------------------------------
		// One press of the auto key starts a cycle that repeats every 20 s while the player simply runs and taps
		// Jump: 0 to 5 s normal, 5 to 10 s Jump presses swallowed (the method changes every cycle: unlink, disabled
		// flag, status stop, value zero), 10 to 15 s the movement value forced to 0, 15 to 20 s movement scaled.
		// The S1 line (speed next to the held stick) and the jump checks show what the game did in each phase.
		// It stops by itself after 3 minutes, or at the next press of the key.
		constexpr double kPhaseSeconds = 5.0;
		constexpr double kCycleSeconds = 20.0;
		constexpr double kAutoMaxSeconds = 180.0;

		bool   s_autoRunning{ false };
		double s_autoStart{ 0.0 };
		int    s_autoLastPhase{ -1 };
		int    s_autoLastCycle{ -1 };

		void StopAuto(const char* a_why)
		{
			s_autoRunning = false;
			s_swallowJump = false;
			s_zeroMove = false;
			s_scaleMove = false;
			COVER_DETAIL("S2b auto cycle stopped ({}); all rewrite modes off", a_why);
		}

		double s_playSeconds{ 0.0 };  // unpaused time with a player in a cell, for the automatic start
		bool   s_autoStartedOnce{ false };

		void UpdateAuto(const Frame::Snapshot& a_snap)
		{
			const auto& cfg = Config::Get();
			if (cfg.autoRewriteStart && !s_autoStartedOnce && !s_autoRunning && a_snap.valid && !a_snap.paused) {
				s_playSeconds += a_snap.dt;
				if (s_playSeconds >= cfg.autoRewriteDelaySec) {
					s_autoStartedOnce = true;
					s_autoToggle = true;  // handled just below, exactly like a key press
					COVER_DETAIL("S2b auto cycle: starting by itself after {} s of play", cfg.autoRewriteDelaySec);
				}
			}
			if (s_autoToggle.exchange(false)) {
				s_autoStartedOnce = true;
				if (s_autoRunning) {
					StopAuto("key pressed");
				} else {
					s_autoRunning = true;
					s_autoStart = a_snap.time;
					s_autoLastPhase = -1;
					s_autoLastCycle = -1;
					COVER_DETAIL("S2b auto cycle STARTED: run with the left stick or W and tap Jump about once a second; 20 s cycles of normal / Jump swallowed / movement zeroed / movement halved");
				}
			}
			if (!s_autoRunning) {
				return;
			}

			const double elapsed = a_snap.time - s_autoStart;
			if (elapsed >= kAutoMaxSeconds) {
				StopAuto("3 minutes elapsed");
				return;
			}
			const int cycle = static_cast<int>(elapsed / kCycleSeconds);
			const int phase = static_cast<int>(std::fmod(elapsed, kCycleSeconds) / kPhaseSeconds);
			if (cycle == s_autoLastCycle && phase == s_autoLastPhase) {
				return;
			}
			s_autoLastCycle = cycle;
			s_autoLastPhase = phase;

			const int method = cycle % kMethodCount;
			s_method = method;
			s_swallowJump = phase == 1;
			s_zeroMove = phase == 2;
			s_scaleMove = phase == 3;
			static constexpr const char* kPhaseNames[] = { "NORMAL (nothing changed)", "JUMP SWALLOWED", "MOVEMENT ZEROED", "MOVEMENT HALVED" };
			COVER_DETAIL("S2b auto cycle {} phase {}: {}{}", cycle, phase, kPhaseNames[phase],
				phase == 1 ? std::format(" with method {} = {}", method, kMethodNames[method]) : std::string());
		}
	}

	void OnTick(const Frame::Snapshot& a_snap)
	{
		const auto& cfg = Config::Get();
		if (!cfg.enabled || !cfg.inputHook) {
			return;
		}

		if (!s_installed && !s_gaveUp && a_snap.time >= s_nextInstallAttempt) {
			s_nextInstallAttempt = a_snap.time + 1.0;
			unsigned long code{ 0 };
			if (!Guarded([] { TryInstallImpl(); }, code)) {
				s_gaveUp = true;
				REX::ERROR("S2: exception 0x{:08X} while installing the input hook; not installed", code);
			}
		}

		if (cfg.inputRewrite && cfg.rewriteAtControls && !s_controlsInstalled && !s_controlsGaveUp) {
			unsigned long code{ 0 };
			if (!Guarded([] { TryInstallControlsImpl(); }, code)) {
				s_controlsGaveUp = true;
				REX::ERROR("S2b: exception 0x{:08X} while installing the rewrite hook; not installed", code);
			}
		}

		if (cfg.inputRewrite) {
			UpdateAuto(a_snap);
		}
	}

	void SetRigMoveStage(const int a_stage) noexcept
	{
		s_rigMoveStage = a_stage;
	}

	bool HookInstalled() noexcept
	{
		return s_installed.load() && !s_disabled.load();
	}

	void SetMovementOverride(const bool a_on, const float a_x, const float a_y) noexcept
	{
		s_overrideX = a_x;
		s_overrideY = a_y;
		s_overrideOn = a_on;
	}

	void SetMovementProbeLog(const bool a_on) noexcept
	{
		s_probeLog = a_on;
	}

	MovementVector LastMovementVector() noexcept
	{
		return { s_vecX.load(), s_vecY.load(), s_vecZ.load(), s_vecPrevX.load(), s_vecPrevY.load(), s_vecCalls.load(), s_vecWrites.load(), s_wroteX.load(), s_wroteY.load(), s_wroteLast.load(), s_vecFaulted.load() };
	}

	void TakeLook(float& a_mouseA, float& a_mouseB, float& a_stickX, float& a_stickY) noexcept
	{
		a_mouseA = s_lookA.exchange(0.0f);
		a_mouseB = s_lookB.exchange(0.0f);
		a_stickX = s_lookSX.load();
		a_stickY = s_lookSY.load();
	}

	void RequestReloadTap() noexcept
	{
		static RE::BSFixedString s_name{ "WeaponReadyReload" };  // keeps the pooled string alive
		const void*              p{ nullptr };
		std::memcpy(&p, static_cast<const void*>(&s_name), sizeof(void*));
		RequestMadeButton(p, static_cast<int>(RE::InputEvent::DeviceType::kKeyboard), 0x52, 0.08f);
	}

	void RequestMadeButton(const void* a_namePtr, const int a_device, const int a_id, const float a_seconds) noexcept
	{
		s_fireNamePtr.store(a_namePtr);
		s_fireDev.store(a_device);
		s_fireId.store(a_id);
		s_fireSecs.store(a_seconds);
		s_fireReq.store(true);
	}

	void RequestMadeFire(const int a_device, const int a_id, const float a_seconds) noexcept
	{
		s_fireNamePtr.store(nullptr);
		s_fireDev.store(a_device);
		s_fireId.store(a_id);
		s_fireSecs.store(a_seconds);
		s_fireReq.store(true);
	}

	void RequestSneakPress() noexcept
	{
		s_madeRequested.fetch_add(1);
		s_pressRequestTicks.store(std::chrono::steady_clock::now().time_since_epoch().count());
		s_pressRequests.fetch_add(1);
	}

	void ClearMadeUpRequests() noexcept
	{
		s_pressRequests.store(0);
	}

	MadeUpStats MadeUpPressStats() noexcept
	{
		return { s_madeRequested.load(), s_madeDelivered.load(), s_madeNoTemplate.load(), s_haveTemplate, s_madeExpired.load(), s_nullHeadCalls.load() };
	}

	bool ControlsHookWorking() noexcept
	{
		return s_controlsInstalled.load() && !s_controlsDisabled.load();
	}

	bool RawMoveRecent(const double a_seconds) noexcept
	{
		if (s_rawMoveMag.load() <= 0.05f) {
			return false;
		}
		const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
		const auto age = std::chrono::duration<double>(std::chrono::steady_clock::duration(now - s_rawMoveTicks.load())).count();
		return age <= a_seconds;
	}

	bool ConsumeProbeRequest()
	{
		return s_probeRequested.exchange(false);
	}

	bool ConsumeReloadRequest()
	{
		return s_reloadRequested.exchange(false);
	}

	bool ConsumeSenseRequest()
	{
		return s_senseRequested.exchange(false);
	}

	Core::InputState State()
	{
		std::lock_guard lock(s_stateMutex);
		return s_state;
	}

	std::string StatusLine()
	{
		const auto& cfg = Config::Get();
		if (!cfg.inputHook) {
			return "input hook off";
		}
		std::string held;
		{
			std::lock_guard lock(s_stateMutex);
			held = std::format("{} Move=({:.2f},{:.2f})", s_state.Describe(), s_state.StickX(), s_state.StickY());
		}
		return std::format("keys={} hook={}{} calls={} ev(btn={} mouse={} cursor={} char={} stick={}) rewrite@{}({}, calls={}) modes[swallowJump={}(m{}) zero={} scale={}]",
			held, s_installed ? "on" : (s_gaveUp ? "REFUSED" : "waiting"), s_disabled ? "(FAULTED)" : "", s_hookCalls.load(),
			s_eventCounts[0].load(), s_eventCounts[1].load(), s_eventCounts[2].load(), s_eventCounts[3].load(), s_eventCounts[4].load(),
			RewriteAtCamera() ? "camera" : "controls", s_controlsInstalled ? (s_controlsDisabled ? "FAULTED" : "on") : (s_controlsGaveUp ? "REFUSED" : "waiting"), s_controlsCalls.load(),
			s_swallowJump.load() ? "ON" : "off", s_method.load(), s_zeroMove.load() ? "ON" : "off", s_scaleMove.load() ? "ON" : "off");
	}
}
