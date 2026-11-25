#pragma once

// T3.1: the cover state machine (PLAN.md section 3.2). Engine-free and unit-tested: it holds the state, takes events (a trigger accepted
// by the input hook, a jump or sprint press) and a per-tick bundle of facts (is cover allowed, how the player stands against the wall,
// whether the aim is held ...) and says what changed. It does nothing in the engine itself: Engine/Cover reads the outputs and moves the
// player, filters the input and logs.
//
//   None -> Entering          RequestEnter: the trigger, with cover allowed and a Low or High candidate
//   Entering -> InCover       the player reached the stand point, or fEnterMaxTime passed (then the owner snaps the player there)
//   Entering -> None          the path to the stand point is blocked
//   InCover -> Peeking        aim held and a peek spot is free (the owner says which)
//   Peeking -> InCover        aim released, or the peek spot is no longer valid
//   InCover, Peeking -> Exiting -> None
//                             the trigger again; the input held away from the wall for fExitHoldTime; a jump; a sprint; the cover lost for
//                             several updates; the player displaced more than fMaxDeviation. Exiting lasts fExitTime
//   any -> None (hard reset)  the game loaded, a cell change or teleport, death (the owner reports these as `worldReset`), the camera
//                             left third person or any other "not allowed" condition (`allowed` false)
// Time is in seconds on any steady clock; every call carries its own `now`.

#include "Core/CoverSensor.h"

namespace Core
{
	enum class CoverState
	{
		None,
		Entering,
		InCover,
		Peeking,
		Exiting,
	};
	[[nodiscard]] const char* ToString(CoverState a_state) noexcept;

	enum class PeekKind
	{
		None,
		Left,
		Right,
		Over,
	};
	[[nodiscard]] const char* ToString(PeekKind a_kind) noexcept;

	enum class ExitReason
	{
		None,
		Trigger,    // the cover trigger pressed again
		HoldAway,   // the input held away from the wall for fExitHoldTime
		Jump,
		Sprint,
		Lost,       // the cover could not be tracked for several updates in a row
		Displaced,  // the player is more than fMaxDeviation off the cover line
	};
	[[nodiscard]] const char* ToString(ExitReason a_reason) noexcept;

	// What Update() says happened.
	enum class MachineEvent
	{
		None,
		Reached,       // Entering -> InCover (MachineResult::snap tells whether the owner must put the player on the stand point)
		EnterBlocked,  // Entering -> None
		PeekStarted,   // InCover -> Peeking
		PeekEnded,     // Peeking -> InCover
		ExitStarted,   // InCover or Peeking -> Exiting (MachineResult::exitReason)
		ExitDone,      // Exiting -> None
		HardReset,     // any -> None
	};
	[[nodiscard]] const char* ToString(MachineEvent a_event) noexcept;

	struct StateMachineSettings
	{
		float enterTime{ 0.25f };           // [Movement] fEnterTime: the eased move to the stand point (used by the owner)
		float enterMaxTime{ 0.40f };        // Entering ends after this long: snap to the stand point (PLAN.md 3.2)
		float exitTime{ 0.15f };            // how long Exiting lasts before None
		float exitHoldTime{ 0.25f };        // [Movement] fExitHoldTime
		float exitInputThreshold{ 0.6f };   // [Movement] fExitInputThreshold: input away from the wall beyond this counts
		float maxDeviation{ 0.35f };        // [Movement] fMaxDeviation (0.6 until T3.3; the drift correction is not a leash, so beyond this the cover is left)
		int   lostLeaveCount{ 3 };          // consecutive Lost updates that leave the cover (PLAN.md T3.3)
	};

	// The facts of one tick.
	struct MachineInputs
	{
		double   now{ 0.0 };
		bool     allowed{ true };       // false: a "not allowed" condition holds (first person, zero gravity, airborne, menu ...): hard reset
		bool     worldReset{ false };   // the game loaded, the cell changed, the player teleported or died: hard reset
		float    awayInput{ 0.0f };     // the part of the movement input that points away from the wall, 0..1 (InCover and Peeking)
		float    deviation{ 0.0f };     // how far the player is off the cover line, metres (InCover and Peeking)
		int      lostCount{ 0 };        // consecutive Track results of Lost or Corner (InCover)
		bool     shoved{ false };       // something other than our own write moved the player more than 0.15 m in one tick (hit reaction, explosion ...): leave at once
		bool     aimHeld{ false };
		PeekKind peekSpot{ PeekKind::None };  // which peek is possible now (None: no free spot); InCover starts it when aim is held, Peeking ends when it is None
		bool     reachedStand{ false };  // Entering: the player is on the stand point
		bool     pathBlocked{ false };   // Entering: the move to the stand point is blocked
	};

	struct MachineResult
	{
		MachineEvent event{ MachineEvent::None };
		ExitReason   exitReason{ ExitReason::None };  // ExitStarted
		bool         snap{ false };                   // Reached because fEnterMaxTime passed: put the player on the stand point now
		const char*  why{ "" };                       // HardReset: what caused it (static text)
		PeekKind     peek{ PeekKind::None };          // PeekStarted
	};

	class CoverStateMachine
	{
	public:
		CoverStateMachine() = default;
		explicit CoverStateMachine(const StateMachineSettings& a_settings) :
			m_cfg(a_settings) {}

		void SetSettings(const StateMachineSettings& a_settings) { m_cfg = a_settings; }
		[[nodiscard]] const StateMachineSettings& Settings() const noexcept { return m_cfg; }

		[[nodiscard]] CoverState State() const noexcept { return m_state; }
		[[nodiscard]] PeekKind Peek() const noexcept { return m_peek; }
		[[nodiscard]] ExitReason LastExit() const noexcept { return m_exit; }
		[[nodiscard]] const CoverCandidate& Candidate() const noexcept { return m_candidate; }
		[[nodiscard]] double StateSince() const noexcept { return m_since; }
		// The stance the player had when the cover began (-1 unknown), and the one the cover needs: Low cover wants sneaking (1), High standing (0).
		[[nodiscard]] int StanceBefore() const noexcept { return m_stanceBefore; }
		[[nodiscard]] int WantedStance() const noexcept { return m_candidate.kind == CoverKind::Low ? 1 : 0; }
		[[nodiscard]] bool Active() const noexcept { return m_state != CoverState::None; }

		// None -> Entering. False (nothing changes) unless the state is None and a_candidate is Low or High.
		bool RequestEnter(double a_now, const CoverCandidate& a_candidate, int a_stanceBefore);

		// Entering, InCover or Peeking -> Exiting. False in every other state.
		bool RequestLeave(double a_now, ExitReason a_reason);

		// The tracked candidate of the cover the player is in (Track moves the line, the edges and the stand point as the player moves along it).
		void SetCandidate(const CoverCandidate& a_candidate) { m_candidate = a_candidate; }

		// Any state -> None at once. Returns whether the state changed.
		bool Reset() noexcept;

		// Advances the state with the facts of this tick.
		MachineResult Update(const MachineInputs& a_inputs);

	private:
		void ToState(CoverState a_state, double a_now) noexcept;

		StateMachineSettings m_cfg;
		CoverState           m_state{ CoverState::None };
		PeekKind             m_peek{ PeekKind::None };
		ExitReason           m_exit{ ExitReason::None };
		CoverCandidate       m_candidate{};
		int                  m_stanceBefore{ -1 };
		double               m_since{ 0.0 };
		double               m_awaySince{ -1.0 };
	};
}
