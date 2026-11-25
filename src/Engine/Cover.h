#pragma once

// Phase 3: the cover feature in the engine (T3.0 trigger and background sensing, T3.2 enter and leave). Everything that is engine-free lives
// in Core/ (CoverStateMachine, CoverDecision, CandidateCache, CoverSensor); this file is the glue:
//
//  * the HOOK side, called from the PlayerControls::Manager rewrite hook in Engine/Input.cpp for every event of a queue. It tracks the
//    movement input as the player gave it, decides what a press of the Sneak control means (Core::DecideSneakPress, from what the
//    tick already cached: no waiting for a sensor query), tells the state machine about jump and sprint presses, and may swallow a
//    press or zero the movement while the player slides to the cover. It returns at once and holds one mutex briefly;
//  * the TICK side (Frame::Tick, a steady 60 Hz logic timer inside the SFSE task, rule 5): reads the facts the hook cannot (stance,
//    "not allowed" conditions, the camera yaw), runs the background sensor while the movement input points somewhere, advances the state
//    machine, moves the player to the stand point with SetPosition, checks the stance 0.3 s after a press and logs every change.
//
// [Cover] bCover is the master switch (0 in the repository INI): with it off nothing here runs and the hook passes everything. Any doubt
// about the player's state (load, cell change, teleport, first person, a "not allowed" reading, a fault) resets to None and removes every
// input filter.

#include <string>
#include <vector>

#include "Core/CoverDecision.h"
#include "Core/CoverSensor.h"
#include "Core/CoverStateMachine.h"
#include "Core/InputState.h"

namespace Engine::Frame
{
	struct Snapshot;
}

namespace Engine::Cover
{
	// True while the feature is live: [Cover] bCover, no fault, the rewrite hook installed and working.
	[[nodiscard]] bool Enabled() noexcept;

	// ---- hook side (any thread; the controls hook calls these for each event of its queue) ------------------------------------------
	struct ButtonPlan
	{
		bool swallow{ false };    // hide this event from the game (the caller sets the `disabled` flag with undo)
		bool resetHeld{ false };  // pass it with its held time set to 0 (blind fire: the game sees a fresh press)
	};
	// a_deviceType is the event's device (keyboard, mouse, gamepad) and a_idCode its code: a swallowed press is swallowed to its release.
	// a_eventId identifies the event (address and time code) so that a press seen twice gets the same decision. Only Sneak, Jump and
	// Sprint matter here; the movement actions go through OnMoveKey.
	[[nodiscard]] ButtonPlan OnButton(Core::Action a_action, bool a_down, bool a_pressEdge, int a_deviceType, std::int32_t a_idCode, std::uint64_t a_eventId);
	// A movement key event: only recorded (T3.3b: the events are no longer rewritten, the filter works on the game vector, see FilterMovementVector). a_value is not changed.
	void OnMoveKey(Core::Action a_action, bool a_down, float& a_value);
	// The Move stick: only recorded; a_x and a_y are not changed.
	void OnMoveStick(float& a_x, float& a_y);
	// how many movement inputs the pose zeroed for the game so far (T5.2c, the rig reads it)
	[[nodiscard]] int ZeroInputCount() noexcept;

	// T3.3b: called by the rewrite hook right after the game own PerformInputProcessing returned, with the movement vector the game computed (a_gameX strafe, a_gameY
	// forward). The game vector is the player real input (kept for the pull-away exit); the answer says what to write back, if anything: zero while entering, peeking or
	// after Lost or Corner, in cover the part along the wall capped and braked before an end. Nothing is written while the tick facts are stale (vanilla input).
	struct VectorFilter
	{
		bool  write{ false };
		float x{ 0.0f };
		float y{ 0.0f };
	};
	[[nodiscard]] VectorFilter FilterMovementVector(float a_gameX, float a_gameY);
	// T3.5, iCoverTrigger 0: a press of the dedicated cover key (a keyboard event with the code of [Input] iCoverKey).
	void OnCoverKey(bool a_down, bool a_pressEdge, std::uint64_t a_eventId);

	// ---- tick side ------------------------------------------------------------------------------------------------------------
	void OnTick(const Frame::Snapshot& a_snap);

	// ---- for the rig scripts and the log --------------------------------------------------------------------------------------------
	struct Transition
	{
		double           time{ 0.0 };  // Frame::Now()
		Core::CoverState from{ Core::CoverState::None };
		Core::CoverState to{ Core::CoverState::None };
		std::string      why;
	};

	struct Info
	{
		bool                 enabled{ false };
		bool                 faulted{ false };
		Core::CoverState     state{ Core::CoverState::None };
		Core::PeekKind       peek{ Core::PeekKind::None };
		Core::ExitReason     lastExit{ Core::ExitReason::None };
		Core::CoverCandidate candidate{};  // of the current or last cover
		int                  stance{ -1 };
		int                  stanceBefore{ -1 };
		bool                 haveCached{ false };
		Core::CoverKind      cachedKind{ Core::CoverKind::None };
		double               cachedAge{ 0.0 };
		std::uint32_t        presses{ 0 };     // Sneak presses decided
		std::uint32_t        entered{ 0 };
		std::uint32_t        swallowed{ 0 };
		std::uint32_t        senses{ 0 };       // background Finds answered
		std::uint32_t        tracks{ 0 };       // Track answers applied in cover
		std::uint32_t        trackLost{ 0 };    // Track answers of Lost, Corner or Unavailable
		int                  lostCount{ 0 };    // in a row now
		float                perp{ 0.0f };      // signed distance from the cover line (positive away from the wall), at the last tick in cover
		float                remainPlus{ 0.0f };   // room left along +tangent / -tangent, at the last tick in cover
		float                remainMinus{ 0.0f };
		int                  filterMode{ 0 };         // 0 none, 1 zero, 2 wall (the movement filter as the hook applies it now)
		std::uint32_t        positionWrites{ 0 };     // SetPosition calls made by Phase 3 (A3 diagnostics)
		std::uint32_t        positionWritesBlocked{ 0 };
		bool                 aimHeldSeen{ false };    // s_aimHeld, what the machine is told
	};
	[[nodiscard]] Info GetInfo();

	// The movement input as the hook last saw it, turned into a world direction with the camera yaw: magnitude 0..1 and a unit direction.
	struct InputDir
	{
		bool       valid{ false };  // the yaw could be read
		float      magnitude{ 0.0f };
		Core::Vec2 dir{};
		float      yaw{ 0.0f };
	};
	[[nodiscard]] InputDir GetInputDir();

	// Every state change since the process started, oldest first (capped). a_from is an index into that list.
	[[nodiscard]] std::vector<Transition> Transitions(std::size_t a_from = 0);
	[[nodiscard]] std::size_t             TransitionCount();

	// Clears the cache and the state at once (the rig does this between tests). Tick thread.
	void ForceReset(const char* a_why);
}
