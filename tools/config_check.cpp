// The configuration check (release 1.0.0, REFERENCE 24): prints the effective settings, one "key=value" line per key, sorted, exactly as the plugin would
// hold them after reading an INI. Built from the plugin's own parser (src/ConfigParse.cpp).
//
//   CoverSF_configcheck dev     <ini>   the development build: the code defaults, then the INI
//   CoverSF_configcheck release <ini>   the release build: the code defaults, m4b13's compiled-in values, then the INI (rig and debug keys refused)
//   CoverSF_configcheck release -       the release build with no INI at all
//
// Unknown and refused keys are listed on stderr as "unknown: <key>" and "ignored: <key>".

#include "ConfigParse.h"

#include <cstdio>
#include <cstring>
#include <fstream>

int main(int argc, char** argv)
{
	if (argc != 3 || (std::strcmp(argv[1], "dev") != 0 && std::strcmp(argv[1], "release") != 0)) {
		std::fprintf(stderr, "usage: %s dev|release <ini or ->\n", argv[0]);
		return 2;
	}
	const bool       release = std::strcmp(argv[1], "release") == 0;
	Config::Settings s;
	if (release) {
		Config::ApplyReleaseDefaults(s);
	}
	if (std::strcmp(argv[2], "-") != 0) {
		std::ifstream in(argv[2]);
		if (!in.is_open()) {
			std::fprintf(stderr, "cannot open %s\n", argv[2]);
			return 2;
		}
		const auto report = Config::ParseIni(in, s, release);
		for (const auto& k : report.unknown) {
			std::fprintf(stderr, "unknown: %s\n", k.c_str());
		}
		for (const auto& k : report.ignored) {
			std::fprintf(stderr, "ignored: %s\n", k.c_str());
		}
	}
	for (const auto& [key, value] : Config::Dump(s)) {
		std::printf("%s=%s\n", key.c_str(), value.c_str());
	}
	return 0;
}
