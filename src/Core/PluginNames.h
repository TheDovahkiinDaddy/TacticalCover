#pragma once

#include <string>
#include <string_view>

// The plugin's file names, derived from the DLL's own name (release 1.0.0, REFERENCE 24). Engine-free and unit-tested.
//
// The development build is CoverSF.dll and keeps CoverSF.ini, CoverSF.log and CoverSF.reloadsound.txt; the release is TacticalCover.dll with
// TacticalCover.ini, TacticalCover.log and TacticalCover.reloadsound.txt. Nothing else decides a name, so the rig, the deploy folder and every
// script that reads CoverSF.log are untouched by the release build.

namespace Core
{
	// "C:\...\Plugins\TacticalCover.dll" -> "TacticalCover". An empty or unusable path gives a_fallback.
	[[nodiscard]] std::string ModuleBaseName(std::string_view a_modulePath, std::string_view a_fallback);

	[[nodiscard]] inline std::string IniFileName(const std::string_view a_base) { return std::string(a_base) + ".ini"; }
	[[nodiscard]] inline std::string LogBaseName(const std::string_view a_base) { return std::string(a_base); }  // SFSE adds ".log"
	[[nodiscard]] inline std::string ReloadSoundFileName(const std::string_view a_base) { return std::string(a_base) + ".reloadsound.txt"; }
}
