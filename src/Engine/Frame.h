#pragma once

// S1: the frame tick and the once-a-second player-state log.
//
// Frame::Tick runs from the permanent SFSE task registered in main.cpp. It measures how often and on which
// OS threads the task runs, reads the player's state, and hands a Snapshot to the other spikes (Input, WorldProbe).

namespace Engine::Frame
{
	struct Snapshot
	{
		bool         valid{ false };   // the player exists and has a parent cell
		bool         paused{ false };  // game paused by a menu (RE::Main::isGameMenuPaused)
		double       time{ 0.0 };      // seconds since the first tick
		float        dt{ 0.0f };       // seconds since the previous tick that ran
		RE::NiPoint3 position{};       // OBJ_REFR::location (the player's origin, at the feet)
		RE::NiPoint3 angle{};          // OBJ_REFR::angle; z is the heading
		int          cameraIndex{ -1 };  // index of PlayerCamera::currentState in cameraStates, -1 when none matches
	};

	// Called from the SFSE task. Never throws; faults inside the engine reads disable S1 for the session.
	void Tick();

	// The snapshot of the latest tick that ran. Only valid on the thread that runs the SFSE tasks.
	[[nodiscard]] const Snapshot& Latest() noexcept;

	// Seconds since the first tick (steady clock).
	[[nodiscard]] double Now() noexcept;
}
