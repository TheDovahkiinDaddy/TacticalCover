#pragma once

// Who may share slot 12 of the ThirdPersonState vtable (M4b11, REFERENCE 23.27). Engine-free and unit-tested.
//
// The plugin's own camera is a hook on that slot. Another mod may already own it: Dynamic Dialogue Camera (DDC.dll) does, and its function
// calls the game's own Update, so a chain (our hook calls whatever the slot held) keeps both working. Stride (DyMC.dll) owns the whole
// third-person camera and is never chained. Anything not on the allow-list keeps the old behaviour: the plugin stands down (compatibility mode).

#include <string>
#include <vector>

namespace Core
{
	enum class CameraSlotDecision
	{
		Own,    // the slot holds the game's own Update: install the hook as always
		Chain,  // the slot holds an allow-listed module's function: install the hook and call that function as the original
		Stand,  // compatibility mode: no hook on the vtable, no camera of our own
	};

	struct CameraSlotInput
	{
		bool        slotHoldsGameUpdate{ false };  // the slot's value is the game's own ThirdPersonState::Update
		bool        strideLoaded{ false };         // DyMC.dll is a loaded module
		int         chainMode{ 1 };                // [Camera] iCameraChain: 0 = never chain (the old behaviour), 1 = chain with the allow-listed owners
		std::string owner{};                       // the file name of the module whose code the slot points into ("" when it cannot be told)
		std::string allowList{};                   // [Camera] sCameraChainModules: file names separated by commas, semicolons or spaces
	};

	struct CameraSlotResult
	{
		CameraSlotDecision decision{ CameraSlotDecision::Stand };
		std::string        reason{};
	};

	// The names in a list, lower-cased, without the path part, empty entries dropped.
	[[nodiscard]] std::vector<std::string> ParseModuleList(const std::string& a_text);
	// True when a_name (a file name, any case, a path is cut to its file name) is in the list.
	[[nodiscard]] bool ModuleInList(const std::string& a_name, const std::string& a_list);
	// Modules that are never chained, whatever the INI says.
	[[nodiscard]] bool NeverChained(const std::string& a_name);

	[[nodiscard]] CameraSlotResult DecideCameraSlot(const CameraSlotInput& a_in);
}
