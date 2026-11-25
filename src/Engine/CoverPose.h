#pragma once

// T5.2: the cover poses. On top of Phase 3, which keeps finding the cover, taking it, tracking it and leaving it, the player's animation graph is switched to the NPC graph (the
// AnimArchetypePlayer keyword cleared, REFERENCE 21) while the machine is InCover, and the engine's own stance function puts the character into the cover state at the stand point,
// facing the wall: the pose the enemy AI uses (REFERENCE 22). The camera rig is held at the own graph's values (T5.0b) with the T5.3a cover offset on top. Every exit of Phase 3
// calls the engine's exit function at once and gives the own graph back when the exit has cleared; a hard reset puts everything back in the same tick.
//
// Behind [Cover] bCoverPoses (repo default 0). Every engine call is guarded; a fault puts everything back and switches the feature off for the session.

#include "Core/CoverSensor.h"
#include "Core/CoverStateMachine.h"

namespace Engine::CoverPose
{
	enum class State
	{
		Off,
		Switching,  // the archetype is cleared, waiting for the NPC graph to be live
		Entering,   // the stance call was made, waiting for bIsInCover
		Posed,
		Leaving,    // the exit function was called, waiting for bIsExitingCover to clear
		Restoring,  // the own archetype was asked for, waiting for the own graph
	};
	[[nodiscard]] const char* ToString(State a_state) noexcept;

	struct Info
	{
		State  state{ State::Off };
		bool   faulted{ false };
		int    sessions{ 0 };      // poses begun
		int    restored{ 0 };      // poses ended with the own graph confirmed
		int    unconfirmed{ 0 };   // poses ended without the own graph confirmed
		// the latest pose (Frame::Now() seconds, -1 = not reached)
		double tBegin{ -1.0 }, tGraphReady{ -1.0 }, tCall{ -1.0 }, tPosed{ -1.0 }, tLeave{ -1.0 }, tRestoreCall{ -1.0 }, tOwnBack{ -1.0 };
		int    stanceCalls{ 0 };
		bool   usedHolsterFallback{ false };
		bool   lastCallReturned{ false };
		int    lastStance{ -1 };
		bool   lastSide{ false };
		float  lastHeading{ 0.0f };
		RE::NiPoint3 lastLocation{};
	};
	[[nodiscard]] Info GetInfo();

	// bCoverPoses is on and nothing faulted
	[[nodiscard]] bool Enabled() noexcept;
	// a pose is in progress and the machine's peeks and stance service must stay out (Switching, Entering, Posed)
	[[nodiscard]] bool Active() noexcept;
	// anything but Off, including the way out
	[[nodiscard]] bool Busy() noexcept;

	struct Inputs
	{
		double                       now{ 0.0 };
		Core::CoverState             machine{ Core::CoverState::None };
		const Core::CoverCandidate*  candidate{ nullptr };
		bool                         canStart{ false };  // the caller's conditions for beginning (allowed, settled, not shoved)
		float                        playerZ{ 0.0f };
	};
	enum class Event
	{
		None,
		Restored,  // the pose is over and the own graph is back
	};
	// Called from the cover tick, every logic step.
	[[nodiscard]] Event Tick(const Inputs& a_in);

	// Everything back in this call (the exit function, the archetype, the hold, the camera offset). Safe to call in any state.
	void HardReset(const char* a_why);

	// Rig: the stance call (stance 3, side false, here) made on whatever graph the player has; true when the function ran, a_returned is its result. The own graph answers false.
	[[nodiscard]] bool ProbeStanceCall(bool& a_returned);
	// The engine's stance field of the player's process data (iSyncActorStance written by the engine), -1 when unreadable.
	[[nodiscard]] int StanceField();
	// The "archetype change pending" byte (Offsets.h kMhArchetypePending), -1 when unreadable.
	[[nodiscard]] int PendingByte();
	// The player's weapon state, the low three bits of the actor state word (0 sheathed, 1 wanted to draw, 2 drawing, 3 drawn: read from the rig traces of stage A3), -1 when unreadable.
	[[nodiscard]] int WeaponState();
	// T5.6, the weapon controls (REFERENCE 22.12): nothing may be equipped or unequipped while the archetype keyword is away (the new weapon is not shown). The pose ends at once on a press
	// of a quick-key or favourites control (hook side, any thread: only atomics are touched), on the weapon state leaving 3 for 0.2 s and on the ready/holster control held for
	// fPoseHolsterHold seconds; a new pose waits until the weapon state has been 3 for 0.5 s, the favourites wheel is closed (its own events: Accept/Cancel close it, 8 s without one too)
	// and 1.0 s have passed since the pose ended. a_wheel: the control opens the favourites wheel (the D-pad quick-keys, the favourites control), false for a number key that equips at once.
	void NoteWeaponControl(const char* a_event, bool a_wheel);
	// hook side: an event of the wheel's own controls (Accept, Cancel, the D-pad directions): a_close for Accept/Cancel.
	void NoteWheelEvent(bool a_close);
	// T5.7: a reload began (or is about to): no pose for a_seconds. Hook side, atomics only.
	void NoteReload(float a_seconds);
	// T5.9, iPoseWeaponSwap 0: the engine's own input-enable layer (CommonLibSF BSInputEnableManager, the one Papyrus knows as InputEnableLayer): one layer of ours with the favourites switched off while
	// the pose is on (the quick-keys and the wheel), switched on again at every end of the pose, every hard reset and fault, when the poses are switched off, and after a save was loaded.
	void UnlockInput(const char* a_why);
	[[nodiscard]] bool FavoritesLocked() noexcept;
	// T5.2b: the pose will begin for this cover by itself (poses on, the cover kind allowed, nothing holds it back, a weapon that is or will be drawn): Phase 3 then makes no Sneak press for low cover
	[[nodiscard]] bool WillPose(Core::CoverKind a_kind);
	// the swap guard holds the next pose back
	[[nodiscard]] bool SwapHold();
	// T5.8 probe: the six aim floats of the process data (heading var, pitch var, heading min, heading max, pitch min, pitch max, REFERENCE 20.1) and the aim-active byte; false when unreadable.
	[[nodiscard]] bool ReadDirectAt(float (&a_out)[6], int& a_aimActive);
	// T5.8 probe: a_count raw floats of the process data from a_offset on (guarded reads); false when unreadable.
	[[nodiscard]] bool ReadMiddleHighFloats(std::ptrdiff_t a_offset, int a_count, float* a_out);
	// A player graph variable by name, -1 when it does not resolve.
	[[nodiscard]] double VarValue(const char* a_name);
	// card M4b spike: called every cover tick; holds the actor's heading at the cover's while the tall side aim is on
	void CornerHoldTick(double a_now);
	// the corner hold is on now (the input along the wall is swallowed, the camera is on the open side's shoulder)
	[[nodiscard]] bool CornerActive() noexcept;
	// true once per hold, at its start: the side of the aim (+1 right, -1 left); Cover.cpp then slides the actor to fPoseCornerEdge from that end
	[[nodiscard]] bool TakeCornerSlide(int& a_side) noexcept;

	// T5.3: the own graph's camera rig values for four situations, kept while the player is on its own graph (third person, no cover, the offset out, the values steady for 0.3 s):
	// 0 standing, 1 standing and aiming, 2 sneaking, 3 sneaking and aiming. The pose holds the set of the situation it is in (eased over 0.25 s).
	// Called from the cover tick every logic step with what the tick knows; does the sampling itself at 10 Hz.
	void SampleOwnRig(double a_now, bool a_eligible, bool a_aiming, bool a_sneaking);
	struct RigSetInfo
	{
		bool         seen[4]{};
		RE::NiPoint3 boom[4]{};
		RE::NiPoint3 pivot[4]{};
		int          situation{ -1 };  // the pose's current situation, -1 when no pose
	};
	[[nodiscard]] RigSetInfo GetRigSets();
}
