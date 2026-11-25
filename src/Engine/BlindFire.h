#pragma once

// T5.4: blind fire in the pose. The fire control pressed while the aim is not held and no aim mode is on sends BlindFireOverStart (low cover, stance 3) or BlindFireStart (tall cover,
// stance 5) and the press is swallowed: a shot before the Into clip has played would go into the cover from the idle pose, which has no fire clip (REFERENCE 22.2). When Into is over
// and the control is still held, the next event of the real control is passed to the game with its held time set to 0 (so the game sees a fresh press, from the real device and id of
// the control); the release passes. After the release the mode stays for fBlindFireLinger (a new press inside that time goes straight to the game), then BlindFireEnd. The aim pressed
// during blind fire: BlindFireEnd first, the aim press is swallowed until the Out clip is over and then passes with its held time set to 0. Never in the first 0.5 s of a pose and
// never while the weapon is not drawn. Hook side: OnFire and OnAim (the controls hook), tick side: Tick.

namespace Engine::BlindFire
{
	struct Plan
	{
		bool swallow{ false };    // hide this event from the game
		bool resetHeld{ false };  // pass it with its held time set to 0 (the game sees a press)
	};

	enum class State
	{
		Idle,
		Into,     // the start event was asked for, the control's events are swallowed
		Firing,   // the control passes to the game
		Linger,   // released: the mode stays for fBlindFireLinger
	};
	[[nodiscard]] const char* ToString(State a_state) noexcept;

	struct Info
	{
		State         state{ State::Idle };
		std::uint32_t starts{ 0 };         // start events sent
		std::uint32_t ends{ 0 };           // BlindFireEnd events sent
		std::uint32_t swallowed{ 0 };      // events of the fire control swallowed
		std::uint32_t freed{ 0 };          // presses passed with the held time reset (Into over)
		std::uint32_t passedInLinger{ 0 }; // presses passed straight inside the linger time
		std::uint32_t madeShots{ 0 };      // the lost tap: fire presses made by the plugin when Into ended
		double        tPress{ -1.0 }, tStartSent{ -1.0 }, tFreed{ -1.0 }, tRelease{ -1.0 }, tEndSent{ -1.0 };
		bool          aimDeferred{ false };
	};
	[[nodiscard]] Info GetInfo();

	// hook side, the fire control's events (down = value above 0, pressEdge = down with held time 0)
	[[nodiscard]] Plan OnFire(bool a_down, bool a_pressEdge, int a_device, int a_id);
	// hook side, every aim event (a_down = value above 0.3): the plan for this event
	[[nodiscard]] Plan OnAim(bool a_down, bool a_pressEdge);
	// tick side: sends the events, resets when the pose is gone
	void Tick(double a_now);
	// everything idle (a leave, a hard reset); no event is sent
	void Reset(const char* a_why);
}
