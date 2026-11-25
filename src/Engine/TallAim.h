#pragma once

// T5.2 tall pose (REFERENCE 22.17): the aim at a tall cover. The engine's own aim input does nothing in the standing cover pose (22.10), and the standing cover component has only the side
// modes, which lean the actor out past the end of the wall: so while the aim is held the plugin sends SightedStart (the side aim) when an Open end of the wall is within fEdgePeekRange on
// the side the actor is turned to, SightedEnd on the release (never before the Into clip is over), and when only the other end is open and in reach it turns the actor round first (the Enter
// event of the other side). With no open end in reach the aim does nothing and the pose stays. Blind fire at tall cover uses the same rule (BlindFire.cpp asks Decide and StartTurn here).
// Hook side: OnAim; tick side: Tick (called from the cover tick with the tracked candidate's ends). Behind [Cover] bPoseTallAim.

#include "Core/TallVerb.h"

namespace Engine::TallAim
{
	struct Plan
	{
		bool swallow{ false };
		bool resetHeld{ false };
	};

	enum class State
	{
		Idle,
		Turning,  // the Enter event of the other side was sent, waiting for iSyncRightLeft to flip
		Into,     // SightedStart sent, the Into clip plays (1.50 s)
		Aiming,
		Out,      // SightedEnd sent, the Out clip plays (1.67 s)
	};
	[[nodiscard]] const char* ToString(State a_state) noexcept;

	struct Info
	{
		State         state{ State::Idle };
		std::uint32_t aimStarts{ 0 }, aimEnds{ 0 }, turns{ 0 }, refused{ 0 };
		double        tStart{ -1.0 }, tIntoOver{ -1.0 }, tEnd{ -1.0 }, tTurn{ -1.0 }, tTurnDone{ -1.0 };
		bool          lastTurnToLeft{ false };
	};
	[[nodiscard]] Info GetInfo();

	// the tall pose is up, settled and the weapon is drawn, bPoseTall and bPoseTallAim on
	[[nodiscard]] bool Active();
	// the end rule for the pose's present side with the ends of the latest tick
	[[nodiscard]] Core::TallVerb Decide();
	// the actor turns round to the other side (the Enter event of that side); a_aimWanted: start the aim when the turn is over and the aim is still held
	void StartTurn(const char* a_why, bool a_aimWanted);
	// the turn or the aim clip is running: events of the fire control are held back
	[[nodiscard]] bool Busy();

	[[nodiscard]] Plan OnAim(bool a_down, bool a_pressEdge);
	void Tick(double a_now, const Core::TallEnds& a_ends, double a_stillFor = 0.0);
	void Reset(const char* a_why);
	// W4: end the aim the plugin started (SightedEnd by the tick, never before the Into clip is over); nothing when no aim clip is running
	void RequestEnd(const char* a_why);
}
