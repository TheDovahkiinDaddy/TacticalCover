#pragma once

// The p3 test script (PLAN.md Phase 3, rig tests R1 to R7). [General] sTestScript=p3 switches the cover feature on for the run and plays one
// of three sets, chosen by [General] iP3Set, once a save has loaded and the player has stood still for 3 s:
//
//   set 1 (Save29, the wall 1.23 m east of the spawn): the direction check of T3.0 (W, A, S against the computed input direction), R1 enter
//         high with S held and a Sneak tap, leaving by Sneak, Jump, Sprint and by forcing first person (R3), the Sneak press with no key
//         held (R1's second half) and in open ground (R6, first half)
//   set 2 (Save30, the low wall along x = -615.3): R4 enter low, stance 1 within 0.3 s, leave by Sneak, entering while already sneaking
//   set 3 (the exit save in the Well): R7, the doorway of card P2: the three recorded spots, a Find at each with the user's view as intent,
//         the scan line, and for a rejection the fit points
//
// One RESULT line per question, then TEST COMPLETE. Synthetic keys only while the game window is in front (Engine/Rig).

namespace Engine::Frame
{
	struct Snapshot;
}

namespace Engine::P3
{
	void OnTick(const Frame::Snapshot& a_snap);

	// Stops the script, releases the rig's keys, logs "TEST COMPLETE (aborted)" and quits if so configured.
	void Abort(const char* a_why);

	[[nodiscard]] bool Finished() noexcept;
}
