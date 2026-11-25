#pragma once

// Engine-free input model (PLAN.md section 3.1). The engine layer (Engine/Input) turns raw button events
// into calls on this class; Core code and tests never see an engine type.

#include <string>
#include <string_view>

namespace Core
{
	// What a game "user event" means to CoverSF. The names come from the game's own strings (checked in
	// Starfield.exe on 2026-11-16); which of them the keyboard, mouse and gamepad actually deliver is
	// recorded by the first test run (docs/REFERENCE.md section 12).
	enum class Action
	{
		None,
		MoveForward,
		MoveBack,
		MoveLeft,
		MoveRight,
		Sprint,
		Jump,
		Aim,
		Fire,
		Sneak,
	};

	// Exact, case-sensitive match on the game's user-event name. Unknown names give Action::None.
	[[nodiscard]] Action ClassifyUserEvent(std::string_view a_name) noexcept;

	[[nodiscard]] std::string_view ActionName(Action a_action) noexcept;

	// True for the actions that make the player walk (the ones S2b scales or zeroes).
	[[nodiscard]] bool IsMovementAction(Action a_action) noexcept;

	class InputState
	{
	public:
		// Digital button state. a_down is "value > 0" in the engine event.
		void ApplyAction(Action a_action, bool a_down) noexcept;

		// The cover key is configured by key code, not by user event, so it has its own entry point.
		void ApplyCoverKey(bool a_down) noexcept;

		// Analogue stick, each axis in -1..1, x to the right, y forward. Filled once the stick event
		// layout is known (S2 logs raw dumps first).
		void SetStick(float a_x, float a_y) noexcept;

		// The same with the time of the event (seconds on any steady clock). A stick at rest may send no event at all, so a value stays
		// behind when the stick is let go fast; ExpireStick centres it when no stick event has arrived for a_maxAge seconds (keys keep their
		// own down and up state). Phase 3 safety fix: a later Sneak press must not count as "pushing toward cover" on a stale value.
		void SetStickAt(float a_x, float a_y, double a_now) noexcept;
		void ExpireStick(double a_now, double a_maxAge) noexcept;
		[[nodiscard]] double StickTime() const noexcept { return m_stickTime; }

		// Movement intent from keys and stick, clamped to the unit disc. x to the right, y forward.
		[[nodiscard]] float MoveX() const noexcept;
		[[nodiscard]] float MoveY() const noexcept;

		// Length of the movement intent (0 when nothing is pushed, up to 1).
		[[nodiscard]] float MoveMagnitude() const noexcept;

		[[nodiscard]] float StickX() const noexcept { return m_stickX; }
		[[nodiscard]] float StickY() const noexcept { return m_stickY; }

		[[nodiscard]] bool SprintHeld() const noexcept { return m_sprint; }
		[[nodiscard]] bool AimHeld() const noexcept { return m_aim; }
		[[nodiscard]] bool FireHeld() const noexcept { return m_fire; }
		[[nodiscard]] bool SneakHeld() const noexcept { return m_sneak; }
		[[nodiscard]] bool CoverHeld() const noexcept { return m_cover; }

		// Edges: true once per press until consumed.
		[[nodiscard]] bool ConsumeJumpPressed() noexcept;
		[[nodiscard]] bool ConsumeCoverPressed() noexcept;

		// For the log: the held keys, e.g. "Forward+Sprint".
		[[nodiscard]] std::string Describe() const;

		bool operator==(const InputState&) const noexcept = default;

	private:
		bool  m_forward{ false };
		bool  m_back{ false };
		bool  m_left{ false };
		bool  m_right{ false };
		bool  m_sprint{ false };
		bool  m_jump{ false };
		bool  m_aim{ false };
		bool  m_fire{ false };
		bool  m_sneak{ false };
		bool  m_cover{ false };
		bool  m_jumpEdge{ false };
		bool  m_coverEdge{ false };
		float  m_stickX{ 0.0f };
		float  m_stickY{ 0.0f };
		double m_stickTime{ 0.0 };
	};
}
