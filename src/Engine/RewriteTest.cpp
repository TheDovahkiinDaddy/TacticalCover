#include "Engine/RewriteTest.h"

#include "Config.h"
#include "Engine/Frame.h"
#include "Core/Verdicts.h"
#include "Engine/Input.h"
#include "Engine/WorldProbe.h"

#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

namespace Engine::RewriteTest
{
	namespace
	{
		constexpr const char* kPlanNames[kJumpPlanCount] = { "pass through", "unlink from queue", "set disabled flag", "set status=Stop", "set value=0" };

		constexpr double kJumpWatchSeconds = 1.2;
		constexpr float  kBlockedBelow = Core::kJumpBlockedBelow;
		constexpr float  kNotBlockedAbove = Core::kJumpNotBlockedAbove;
		constexpr int    kPressesPerResult = 5;

		constexpr double kStageSeconds = 0.7;
		constexpr float  kMinBaselineSpeed = 0.8f;   // u/s
		constexpr double kSteadySeconds = 1.0;
		constexpr float  kMinForwardClear = 2.0f;    // m
		constexpr int    kMaxValidAttempts = 3;
		constexpr int    kMaxAttempts = 8;           // discarded ones count too, so a bad spot cannot loop forever

		// ---- position history (tick thread) ----------------------------------------------------------
		struct Sample
		{
			double t;
			float  x;
			float  y;
			float  z;
		};
		std::deque<Sample> s_history;

		std::atomic<bool> s_vertStill{ false };  // published for the hook thread

		// ---- Jump (hook thread writes press state, tick thread runs the check) ------------------------
		std::mutex                s_jumpMutex;
		int                       s_activePlan{ -1 };   // plan of the press in progress, -1 when none
		bool                      s_busy{ false };      // a counted press whose check has not finished
		bool                      s_startPending{ false };
		int                       s_pendingPlan{ 0 };
		std::uint32_t             s_pendingSeq{ 0 };
		std::atomic<std::uint32_t> s_keyboardPresses{ 0 };
		std::atomic<std::uint32_t> s_counted{ 0 };
		std::atomic<std::uint32_t> s_finished{ 0 };
		std::atomic<std::uint32_t> s_notCounted{ 0 };
		std::uint64_t              s_lastPressId{ 0 };   // the press that was decided last, to recognise it when seen again
		int                        s_lastPressPlan{ 0 };

		std::array<std::vector<float>, kJumpPlanCount> s_rises;  // tick thread

		// The rotation order from [InputRewrite] sJumpPlans (default 0,1,2,3,4); parsed once.
		std::vector<int> s_order;
		[[nodiscard]] const std::vector<int>& Order()
		{
			if (s_order.empty()) {
				const std::string& text = Config::Get().jumpPlans;
				std::size_t        pos = 0;
				while (pos < text.size()) {
					const auto comma = text.find(',', pos);
					const auto token = text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
					const int  v = std::atoi(token.c_str());
					if (!token.empty() && v >= 0 && v < kJumpPlanCount) {
						s_order.push_back(v);
					}
					pos = comma == std::string::npos ? text.size() : comma + 1;
				}
				if (s_order.empty()) {
					s_order = { 0, 1, 2, 3, 4 };
				}
				std::string joined;
				for (const int v : s_order) {
					joined += (joined.empty() ? "" : ",") + std::to_string(v);
				}
				REX::INFO("S2b jump rotation order: {}", joined);
			}
			return s_order;
		}

		bool          s_checkRunning{ false };
		double        s_checkT0{ 0.0 };
		float         s_checkZ0{ 0.0f };
		float         s_checkMax{ 0.0f };
		int           s_checkPlan{ 0 };
		std::uint32_t s_checkSeq{ 0 };

		// ---- Movement (tick thread; the hook reads s_stage and s_moveDevice) --------------------------
		enum class Seq
		{
			Idle,
			Zero,
			Scale,
			Restore,
		};
		Seq               s_seq{ Seq::Idle };
		double            s_seqT0{ 0.0 };
		double            s_presentSince{ -1.0 };
		double            s_lostSince{ -1.0 };
		bool              s_watchBack{ false };  // this attempt walks backward: watch the ray behind instead of ahead
		float             s_baseline{ 0.0f };
		float             s_zeroed{ 0.0f };
		float             s_scaled{ 0.0f };
		std::uint32_t     s_attempts{ 0 };
		std::uint32_t     s_valid{ 0 };
		std::atomic<int>  s_stage{ 0 };        // 0 pass, 1 zero, 2 scale
		std::atomic<int>  s_moveDevice{ 0 };   // 1 keyboard, 2 gamepad: the latest device that pushed movement

		[[nodiscard]] const char* StageName(const Seq a_seq) noexcept
		{
			switch (a_seq) {
			case Seq::Zero: return "zero stage";
			case Seq::Scale: return "scale stage";
			case Seq::Restore: return "restore stage";
			default: return "idle";
			}
		}

		// Average horizontal speed between two times, from the history. Returns a negative number when the history
		// does not cover enough of the interval.
		[[nodiscard]] float Speed(const double a_t1, const double a_t2)
		{
			const Sample* first = nullptr;
			const Sample* last = nullptr;
			for (const auto& s : s_history) {
				if (s.t >= a_t1 && !first) {
					first = &s;
				}
				if (s.t <= a_t2) {
					last = &s;
				}
			}
			if (!first || !last || last->t - first->t < 0.6 * (a_t2 - a_t1)) {
				return -1.0f;
			}
			const float dx = last->x - first->x;
			const float dy = last->y - first->y;
			return std::sqrt(dx * dx + dy * dy) / static_cast<float>(last->t - first->t);
		}

		void RecordHistory(const Frame::Snapshot& a_snap)
		{
			if (!a_snap.valid) {
				s_history.clear();
				s_vertStill = false;
				return;
			}
			if (s_history.empty() || a_snap.time - s_history.back().t >= 0.01) {
				s_history.push_back({ a_snap.time, a_snap.position.x, a_snap.position.y, a_snap.position.z });
			}
			while (!s_history.empty() && a_snap.time - s_history.front().t > 4.0) {
				s_history.pop_front();
			}

			// Vertically still: z moved less than 0.05 over the last 0.3 s, with samples covering most of it.
			float  zmin = 1e9f;
			float  zmax = -1e9f;
			double tmin = 1e9;
			for (auto it = s_history.rbegin(); it != s_history.rend() && a_snap.time - it->t <= 0.3; ++it) {
				zmin = (std::min)(zmin, it->z);
				zmax = (std::max)(zmax, it->z);
				tmin = (std::min)(tmin, it->t);
			}
			s_vertStill = !a_snap.paused && zmax - zmin < 0.05f && a_snap.time - tmin >= 0.2;
		}

		void LogJumpResult()
		{
			std::string line = std::format("RESULT S2b jump (after {} counted presses):", s_finished.load());
			for (int plan = 0; plan < kJumpPlanCount; ++plan) {
				std::string rises;
				for (const float r : s_rises[plan]) {
					rises += (rises.empty() ? "" : ",") + std::format("{:.2f}", r);
				}
				line += std::format(" [{}: rises {} -> {}]", kPlanNames[plan], rises.empty() ? "-" : rises, Core::JumpVerdict(s_rises[plan]));
			}
			REX::INFO("{}", line);
		}

		void UpdateJump(const Frame::Snapshot& a_snap)
		{
			bool start = false;
			int  plan = 0;
			std::uint32_t seq = 0;
			{
				std::lock_guard lock(s_jumpMutex);
				if (s_startPending) {
					s_startPending = false;
					start = true;
					plan = s_pendingPlan;
					seq = s_pendingSeq;
				}
			}
			if (start && a_snap.valid) {
				s_checkRunning = true;
				s_checkT0 = a_snap.time;
				s_checkZ0 = a_snap.position.z;
				s_checkMax = 0.0f;
				s_checkPlan = plan;
				s_checkSeq = seq;
			} else if (start) {
				// no player to measure: release the busy flag and forget the press
				std::lock_guard lock(s_jumpMutex);
				s_busy = false;
			}

			if (!s_checkRunning) {
				return;
			}
			s_checkMax = (std::max)(s_checkMax, a_snap.position.z - s_checkZ0);
			if (a_snap.time - s_checkT0 < kJumpWatchSeconds) {
				return;
			}

			s_checkRunning = false;
			s_rises[s_checkPlan].push_back(s_checkMax);
			REX::INFO("S2b jump check #{}: plan '{}': the player rose {:.2f} m within {:.1f} s ({})", s_checkSeq, kPlanNames[s_checkPlan], s_checkMax,
				kJumpWatchSeconds, s_checkMax < kBlockedBelow ? "no jump happened" : (s_checkMax > kNotBlockedAbove ? "the game saw the jump" : "unclear"));
			{
				std::lock_guard lock(s_jumpMutex);
				s_busy = false;
			}
			const auto finished = s_finished.fetch_add(1) + 1;
			if (finished % kPressesPerResult == 0) {
				LogJumpResult();
			}
		}

		void EndSequence(const Frame::Snapshot& a_snap, const char* a_discardReason, const float a_after)
		{
			s_stage = 0;
			const auto stage = s_seq;
			s_seq = Seq::Idle;
			s_presentSince = -1.0;
			s_lostSince = -1.0;
			const char* device = s_moveDevice.load() == 1 ? "keyboard" : (s_moveDevice.load() == 2 ? "gamepad" : "unknown");

			if (a_discardReason) {
				REX::INFO("RESULT S2b move #{} DISCARDED during the {}: {} | device {}", s_attempts, StageName(stage), a_discardReason, device);
				return;
			}

			const float fwdMin = WorldProbe::ForwardWatchMin(s_watchBack);
			const char* where = s_watchBack ? "behind" : "ahead";
			if (fwdMin < 0.0f) {
				REX::INFO("RESULT S2b move #{} DISCARDED: no probe reading {} during the attempt (is [WorldProbe] bAutoProbe on?) | device {}", s_attempts, where, device);
				return;
			}
			if (fwdMin < kMinForwardClear) {
				REX::INFO("RESULT S2b move #{} DISCARDED: something was {:.2f} m {} (a wall in the way would stop the player by itself) | device {}", s_attempts, fwdMin, where, device);
				return;
			}
			if (a_after < 0.8f * s_baseline) {
				REX::INFO("RESULT S2b move #{} DISCARDED: the speed did not recover at the end ({:.2f} after against {:.2f} baseline u/s) | device {}", s_attempts, a_after,
					s_baseline, device);
				return;
			}

			++s_valid;
			const float zr = s_zeroed / s_baseline;
			const float sr = s_scaled / s_baseline;
			const std::string zeroVerdict = Core::ZeroVerdict(zr);
			const std::string scaleVerdict = Core::ScaleVerdict(sr);
			REX::INFO("RESULT S2b move #{}: baseline {:.2f} | zeroed {:.2f} | scaled {:.2f} | after {:.2f} u/s | device {} | zeroing: {} ({:.0f}%); scaling: {} ({:.0f}%); restore: recovered ({:.0f}%) | clear {} min {:.1f} m | valid {} of {}",
				s_attempts, s_baseline, s_zeroed, s_scaled, a_after, device, zeroVerdict, zr * 100.0f, scaleVerdict, sr * 100.0f, a_after / s_baseline * 100.0f, where, fwdMin,
				s_valid, kMaxValidAttempts);
			(void)a_snap;
		}

		void UpdateMove(const Frame::Snapshot& a_snap)
		{
			const auto& cfg = Config::Get();
			const double t = a_snap.time;
			// "Present" must not be decided from InputState alone: while the rewrite zeroes the stick the value read after the rewrite is 0 and every
			// attempt was thrown away 0.15 s after it began (play check 2026-11-17, 8 of 8). The stick as the hook saw it before the rewrite counts too.
			const bool present = a_snap.valid && !a_snap.paused && (Input::State().MoveMagnitude() > 0.05f || Input::RawMoveRecent(0.25));

			if (present) {
				if (s_presentSince < 0.0) {
					s_presentSince = t;
				}
				s_lostSince = -1.0;
			} else {
				if (s_lostSince < 0.0) {
					s_lostSince = t;
				}
				if (t - s_lostSince > 0.15) {
					s_presentSince = -1.0;
				}
			}

			if (s_seq == Seq::Idle) {
				if (!cfg.moveSequence || s_valid >= kMaxValidAttempts || s_attempts >= kMaxAttempts) {
					return;
				}
				if (!present || s_presentSince < 0.0 || t - s_presentSince < kSteadySeconds) {
					return;
				}
				const float v = Speed(t - kSteadySeconds, t);
				if (v < kMinBaselineSpeed) {
					return;
				}
				++s_attempts;
				s_baseline = v;
				s_seq = Seq::Zero;
				s_seqT0 = t;
				s_watchBack = Input::State().MoveY() < -0.05f;
				WorldProbe::BeginForwardWatch(s_watchBack);
				s_stage = 1;
				REX::INFO("S2b move #{} starting: steady movement at {:.2f} u/s ({})", s_attempts, v, s_moveDevice.load() == 1 ? "keyboard" : "gamepad");
				return;
			}

			// An attempt is running. It is thrown away if the input is let go, the game pauses or the player vanishes.
			if (!a_snap.valid || a_snap.paused) {
				EndSequence(a_snap, "the game paused or the player went away", 0.0f);
				return;
			}
			if (s_lostSince >= 0.0 && t - s_lostSince > 0.15) {
				EndSequence(a_snap, "the movement input was released", 0.0f);
				return;
			}

			switch (s_seq) {
			case Seq::Zero:
				if (t >= s_seqT0 + kStageSeconds) {
					s_zeroed = (std::max)(Speed(s_seqT0 + 0.5 * kStageSeconds, s_seqT0 + kStageSeconds), 0.0f);
					s_seq = Seq::Scale;
					s_stage = 2;
				}
				break;
			case Seq::Scale:
				if (t >= s_seqT0 + 2.0 * kStageSeconds) {
					s_scaled = (std::max)(Speed(s_seqT0 + 1.5 * kStageSeconds, s_seqT0 + 2.0 * kStageSeconds), 0.0f);
					s_seq = Seq::Restore;
					s_stage = 0;
				}
				break;
			case Seq::Restore:
				if (t >= s_seqT0 + 3.0 * kStageSeconds) {
					const float after = (std::max)(Speed(s_seqT0 + 2.5 * kStageSeconds, s_seqT0 + 3.0 * kStageSeconds), 0.0f);
					EndSequence(a_snap, nullptr, after);
				}
				break;
			default:
				break;
			}
		}
	}

	const char* JumpPlanName(const int a_plan) noexcept
	{
		return (a_plan >= 0 && a_plan < kJumpPlanCount) ? kPlanNames[a_plan] : "?";
	}

	JumpPlan OnJumpEvent(const bool a_down, const bool a_pressEdge, const bool a_keyboard, const std::uint64_t a_eventId)
	{
		const auto& cfg = Config::Get();
		if (!cfg.inputRewrite || !cfg.jumpRotation) {
			if (a_keyboard && a_pressEdge) {
				s_keyboardPresses.fetch_add(1);
			}
			return JumpPlan::Pass;
		}

		std::lock_guard lock(s_jumpMutex);
		if (a_pressEdge && a_eventId != 0 && a_eventId == s_lastPressId) {
			s_activePlan = s_lastPressPlan;  // the same press again: same plan, not counted twice
			return static_cast<JumpPlan>(s_lastPressPlan);
		}
		if (a_keyboard && a_pressEdge) {
			s_keyboardPresses.fetch_add(1);
		}
		if (!a_down) {
			s_activePlan = -1;  // the release always passes
			return JumpPlan::Pass;
		}
		if (a_pressEdge) {
			s_activePlan = 0;
			s_lastPressId = a_eventId;
			s_lastPressPlan = 0;
			if (s_busy || s_startPending) {
				if (s_notCounted.fetch_add(1) < 20) {
					REX::INFO("S2b jump press not counted: the previous jump check is still running (passed through)");
				}
				return JumpPlan::Pass;
			}
			if (!s_vertStill.load()) {
				if (s_notCounted.fetch_add(1) < 20) {
					REX::INFO("S2b jump press not counted: the player is not vertically still (airborne, boosting or no data); passed through");
				}
				return JumpPlan::Pass;
			}
			const auto counted = s_counted.fetch_add(1);
			const auto& order = Order();
			const int   plan = order[counted % order.size()];
			s_activePlan = plan;
			s_lastPressPlan = plan;
			s_busy = true;
			s_startPending = true;
			s_pendingPlan = plan;
			s_pendingSeq = counted + 1;
			REX::INFO("S2b jump press #{} counted: plan '{}' ({})", counted + 1, kPlanNames[plan], a_keyboard ? "keyboard" : "other device");
			return static_cast<JumpPlan>(plan);
		}
		// key repeat or an analogue value change inside the same press
		return s_activePlan > 0 ? static_cast<JumpPlan>(s_activePlan) : JumpPlan::Pass;
	}

	int MoveStage() noexcept
	{
		return s_stage.load();
	}

	void NoteMoveDevice(const bool a_keyboard) noexcept
	{
		s_moveDevice = a_keyboard ? 1 : 2;
	}

	void OnTick(const Frame::Snapshot& a_snap)
	{
		const auto& cfg = Config::Get();
		if (!cfg.enabled || !cfg.inputRewrite) {
			return;
		}
		RecordHistory(a_snap);
		UpdateJump(a_snap);
		UpdateMove(a_snap);
	}

	std::uint32_t KeyboardJumpPresses() noexcept
	{
		return s_keyboardPresses.load();
	}

	std::uint32_t CountedJumpPresses() noexcept
	{
		return s_counted.load();
	}

	std::uint32_t FinishedJumpChecks() noexcept
	{
		return s_finished.load();
	}

	std::uint32_t ValidMoveAttempts() noexcept
	{
		return s_valid;
	}

	std::uint32_t MoveAttempts() noexcept
	{
		return s_attempts;
	}

	std::string Summary()
	{
		return std::format("jump presses counted {}, checks finished {}; move attempts {} ({} valid)", s_counted.load(), s_finished.load(), s_attempts, s_valid);
	}
}
