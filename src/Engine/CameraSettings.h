#pragma once

// S4, route 2 (planning session's lead, 2026-11-16): the engine's own camera INI settings. Starfield.exe registers
// fOverShoulderPosX:Camera and friends; if the third-person camera reads them, changing a value moves the camera through the
// engine's own smoothing and collision. These helpers look the settings up with RE::GetINISetting, log what is there and read
// or write one value, all guarded. [Camera] bCameraSettings=0 keeps them out of the game.

namespace Engine::CameraSettings
{
	// The settings named by the planning session plus a few neighbours, as "name:Camera".
	[[nodiscard]] std::span<const char* const> Names();

	// Logs, once, for each name: found or not, type, current and default value. Safe to call repeatedly (logs only the first time).
	void LogAllOnce();

	// Current value of a float or bool setting. False when not found, of another type, or after a fault.
	[[nodiscard]] bool GetFloat(const char* a_name, float& a_out);
	[[nodiscard]] bool GetBool(const char* a_name, bool& a_out);

	// Writes a float or bool setting in place (the engine's own copy). False when not found, of another type, or after a fault.
	[[nodiscard]] bool SetFloat(const char* a_name, float a_value);
	[[nodiscard]] bool SetBool(const char* a_name, bool a_value);
}
