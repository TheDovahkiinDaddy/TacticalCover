#pragma once

// S2b, event-driven test protocol (PLAN.md section 5, "Test protocol", 2026-11-16). No timer for the user to
// follow: the plugin reacts to what the player (or the S0 test rig) does and logs one RESULT line per question.
//
//  * Jump: every counted Jump press, from any device, takes the next plan of a fixed rotation
//    (pass through, unlink, disabled flag, status stop, value zero). The plan holds for every event of that press
//    until its release; the release always passes. The plugin measures whether the player left the ground and,
//    after every fifth counted press, logs "RESULT S2b jump" with a verdict per plan.
//  * Movement: once the player has moved steadily for about a second, the movement input is zeroed for 0.7 s,
//    scaled for 0.7 s and restored for 0.7 s; the speed in each stage is logged as "RESULT S2b move #n".
//    At most three valid attempts per launch.

namespace Engine::Frame
{
	struct Snapshot;
}

namespace Engine::RewriteTest
{
	// What the Input hook does to one Jump event.
	enum class JumpPlan : int
	{
		Pass = 0,
		Unlink = 1,
		DisabledFlag = 2,
		StatusStop = 3,
		ValueZero = 4,
	};
	inline constexpr int kJumpPlanCount = 5;

	[[nodiscard]] const char* JumpPlanName(int a_plan) noexcept;

	// Hook thread. a_down: value > 0. a_pressEdge: first event of a press. Returns the plan to apply to this event.
	// a_eventId identifies the event (its address and time code) so that the same press seen twice, if a receiver is
	// called more than once per frame, gets the same plan and is counted once; 0 disables that.
	[[nodiscard]] JumpPlan OnJumpEvent(bool a_down, bool a_pressEdge, bool a_keyboard, std::uint64_t a_eventId = 0);

	// Hook thread. 0 = leave movement alone, 1 = zero it, 2 = scale it.
	[[nodiscard]] int MoveStage() noexcept;

	// Hook thread: movement input was seen (original values, before any rewrite). a_keyboard false means gamepad.
	void NoteMoveDevice(bool a_keyboard) noexcept;

	// Tick thread.
	void OnTick(const Frame::Snapshot& a_snap);

	// Counters for the S0 rig (any thread).
	[[nodiscard]] std::uint32_t KeyboardJumpPresses() noexcept;  // keyboard Jump press edges seen, counted or not
	[[nodiscard]] std::uint32_t CountedJumpPresses() noexcept;
	[[nodiscard]] std::uint32_t FinishedJumpChecks() noexcept;
	[[nodiscard]] std::uint32_t ValidMoveAttempts() noexcept;
	[[nodiscard]] std::uint32_t MoveAttempts() noexcept;

	// Human-readable one-line summary for the end of a scripted run.
	[[nodiscard]] std::string Summary();
}
