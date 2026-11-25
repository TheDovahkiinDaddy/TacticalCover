#pragma once

// The g2 test script (PLAN.md Phase 1, S4 + S5 + S6a/c, evidence for gate G2): one scripted run that measures
// the camera hook (S4), the player-control questions (S5) and sends the cover animation events (S6), logging one
// RESULT line per question and "SCREENSHOT NOW <name>" lines at the moments a picture is worth taking.
// Started by [General] sTestScript=g2, driven from Engine/Rig.

namespace Engine::Frame
{
	struct Snapshot;
}

namespace Engine::G2
{
	void OnTick(const Frame::Snapshot& a_snap);

	// Stops the script, releases the rig's keys, logs "TEST COMPLETE (aborted)" and quits if so configured.
	void Abort(const char* a_why);

	[[nodiscard]] bool Finished() noexcept;
}
