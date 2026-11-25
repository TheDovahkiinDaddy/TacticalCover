#pragma once

// Card M4b / 23.16: the game's foot IK in the mirrored cover walk. The two settings bHumanoidFootIKEnable:Animation and
// bEnableFootIK:Animation are looked up through the library's typed access (RE::GetINISetting, the same route as CameraSettings) and
// written in the engine's own copy. The switch is global, so every change is one log line, the value found first is the value restored,
// and a fault switches the whole thing off for the session. iPoseFootIK 0 (the default) never touches them.

namespace Engine::FootIK
{
	struct Info
	{
		bool found{ false };        // both settings exist and are bool
		bool broken{ false };       // a fault: off for the session
		bool changed{ false };      // the settings are not at their original values now
		bool humanoid{ true };      // current bHumanoidFootIKEnable
		bool feet{ true };          // current bEnableFootIK
		bool origHumanoid{ true };  // as first seen
		bool origFeet{ true };
		int  changes{ 0 };
	};

	// Looks the two settings up (once; logs what it finds). True when both are usable.
	[[nodiscard]] bool Probe();

	// Writes both settings (true = on, false = off; one log line when something changes). The first Probe records the original values. False when the settings are unusable.
	[[nodiscard]] bool Set(bool a_on, const char* a_why);

	// The two switches separately (the rig: which of them matters). Same rules as Set.
	[[nodiscard]] bool SetEach(bool a_humanoid, bool a_feet, const char* a_why);

	// Writes the original values back when they were changed. Always safe to call.
	void Restore(const char* a_why);

	[[nodiscard]] Info GetInfo();
}
