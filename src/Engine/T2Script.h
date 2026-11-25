#pragma once

// The t2 test script (PLAN.md Phase 2): measurements for T2.0 (the cast queue: the engine's camera collision factor must not
// move while the plugin casts 200 times a second; the floor check), and for T2.3 (CoverSensor::Find at several spots, next to
// a scan of the surroundings). One RESULT line per question, ending with TEST COMPLETE. Started by [General] sTestScript=t2,
// driven from Engine/Rig. Synthetic keys only in the stages that walk, and only while the game window is in front.

namespace Engine::Frame
{
	struct Snapshot;
}

namespace Engine::T2
{
	void OnTick(const Frame::Snapshot& a_snap);

	// Stops the script, releases the rig's keys, logs "TEST COMPLETE (aborted)" and quits if so configured.
	void Abort(const char* a_why);

	[[nodiscard]] bool Finished() noexcept;
}
