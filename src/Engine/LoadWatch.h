#pragma once

namespace Engine::LoadWatch
{
	// Registered as a permanent SFSE task, so it runs on the main thread once per frame.
	//
	// Detects "the player entered a cell" (a save was loaded, or fast travel, or a door) by watching the
	// value of the player's parentCell pointer, which is never dereferenced. Logs the player's position
	// at the change and again about a second later, once the load has settled. SFSE 0.2.21 does not send
	// PostLoadGame (see Engine/Offsets.h), so this is the load signal for Phase 0.
	//
	// The body is wrapped in __try/__except. After a fault the watcher logs it and disables itself for
	// the rest of the session (PLAN.md rule 3).
	void Tick();
}
