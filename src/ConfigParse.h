#pragma once

#include <istream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Config.h"

// The INI parser and the key table, engine-free (release 1.0.0, REFERENCE 24): the plugin, the Core tests and the configuration check
// (tools/config_check.cpp, tools/compare_config.py) all use this one code.

namespace Config
{
	enum class KeyResult
	{
		Applied,
		Unknown,
		IgnoredInRelease  // a key of the test rig or a debug key: the release keeps its own value
	};

	// Applies one key (any case; the section is ignored, key names are unique). a_release: the rig and debug keys are refused.
	KeyResult ApplyKey(Settings& a_settings, const std::string& a_key, const std::string& a_value, bool a_release);

	struct ParseReport
	{
		std::vector<std::string> unknown;
		std::vector<std::string> ignored;
	};

	// Reads INI text (key=value lines; ';' or '#' starts a comment line, a ';' after a value starts a comment) into a_settings.
	ParseReport ParseIni(std::istream& a_in, Settings& a_settings, bool a_release);

	// True for the keys the release refuses (lower case).
	[[nodiscard]] bool IsReleaseIgnoredKey(std::string_view a_lowerKey);

	// The release build's compiled-in defaults: the accepted play state m4b13 (build\m4b13_candidate\CoverSF.ini, every key) and then the
	// release's own values (the quiet log, the controller chord, the debug keys off). Applied before TacticalCover.ini is read.
	void ApplyReleaseDefaults(Settings& a_settings);

	// Every key with the value it has in a_settings, sorted by key, for the configuration check.
	[[nodiscard]] std::vector<std::pair<std::string, std::string>> Dump(const Settings& a_settings);
}
