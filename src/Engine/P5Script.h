#pragma once

// The p5 test script (PLAN.md Phase 5, T5.0): the engine's own cover stance on the player, on the rig only. [General] sTestScript=p5 with [Anim] bStanceSpike=1 plays the
// stages chosen by [Anim] iP5Mode once a save has loaded and the player has stood still for 3 s (docs/REFERENCE.md sections 20 and 21):
//
//   mode 1 (launch A): the eased move to the stand point of the low wall of Save30, stage 1 (probe, no writes: the process fields against the graph variables) and stage 2
//          (the engine's stance function on the player's own graph)
//   mode 2 (launch B): the same with a short probe, then stage 3 (the borrowed NPC graph: ChangeAnimArchetype(null), enter, move, aim, shoot, leave, side true), stage 4
//          (restore, and the test that counts: the call again) and stage 5 (cleanup)
//   mode 3 (launch C): the tall wall of Save29: stages 3 to 5 with stance 5 and an explicit heading toward the wall
//
// Every engine call is guarded; a fault ends the script and switches the spike off for the session. Synthetic keys only while the game window is in front (Engine/Rig).

namespace Engine::Frame
{
	struct Snapshot;
}

namespace Engine::P5
{
	void OnTick(const Frame::Snapshot& a_snap);

	// Stops the script: restores the archetype, checks the stance field, releases the keys, logs "TEST COMPLETE (aborted)" and quits if so configured.
	// a_fault: an engine call faulted; the stance function is not called again.
	void Abort(const char* a_why, bool a_fault = false);

	[[nodiscard]] bool Finished() noexcept;
}
