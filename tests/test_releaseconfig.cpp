// Engine-free tests for the release build's names and settings (release 1.0.0, REFERENCE 24): Core/PluginNames and the parser's release defaults.

#include "ConfigParse.h"
#include "Core/PluginNames.h"

#include <cstdio>
#include <sstream>

namespace
{
	int g_failures = 0;

#define CHECK(cond)                                                     \
	do {                                                                \
		if (!(cond)) {                                                  \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++g_failures;                                               \
		}                                                               \
	} while (false)

	Config::Settings Release(const char* a_ini)
	{
		Config::Settings s;
		Config::ApplyReleaseDefaults(s);
		std::istringstream in(a_ini);
		(void)Config::ParseIni(in, s, true);
		return s;
	}
}

int RunReleaseConfigTests()
{
	g_failures = 0;

	// the names follow the DLL
	CHECK(Core::ModuleBaseName(R"(D:\Mods\CoverSF\SFSE\Plugins\CoverSF.dll)", "x") == "CoverSF");
	CHECK(Core::ModuleBaseName(R"(C:\Game\Data\SFSE\Plugins\TacticalCover.dll)", "x") == "TacticalCover");
	CHECK(Core::ModuleBaseName("TacticalCover.DLL", "x") == "TacticalCover");
	CHECK(Core::ModuleBaseName("D:/a.b/Plugins/My.Mod.dll", "x") == "My.Mod");
	CHECK(Core::ModuleBaseName("", "CoverSF") == "CoverSF");
	CHECK(Core::ModuleBaseName(R"(C:\Plugins\)", "CoverSF") == "CoverSF");
	CHECK(Core::IniFileName("TacticalCover") == "TacticalCover.ini");
	CHECK(Core::LogBaseName("TacticalCover") == "TacticalCover");
	CHECK(Core::ReloadSoundFileName("TacticalCover") == "TacticalCover.reloadsound.txt");
	CHECK(Core::IniFileName("CoverSF") == "CoverSF.ini");
	CHECK(Core::ReloadSoundFileName("CoverSF") == "CoverSF.reloadsound.txt");

	// the release with no INI at all plays as m4b13
	{
		const auto s = Release("");
		CHECK(s.enabled && s.cover);
		CHECK(s.coverTrigger == 3);
		CHECK(s.coverPoses && s.poseTall && s.coverCamera);
		CHECK(s.autoCoverTall == 2);
		CHECK(s.autoCoverTime == 0.30f && s.autoCoverTimeDrawn == 0.05f);
		CHECK(s.poseWaitByte == 2 && s.poseWaitByteMax == 1.5f && s.poseNoRepeat == 0);
		CHECK(s.poseWeaponSwap == 3 && s.poseScopeAim == 2 && s.scopeSource == 3);
		CHECK(s.poseFootIK == 1 && s.poseFootIKLow == 1 && s.turnLock);
		CHECK(s.poseReloadSound == 1 && s.poseReloadSoundFile == 1);
		CHECK(s.cameraChain == 1 && s.cameraChainModules == "DDC.dll");
		CHECK(s.frameShoulderDrawn == 0.45f && s.shoulderLeftExtra == 0.06f && s.frameDistAim == 0.95f);
		CHECK(!s.coverNotify);
		CHECK(s.enterSettleTime == 1.0f);  // m4b13's INI value, not the code default 0
		// the release's own values
		CHECK(!s.fullLog);
		CHECK(s.shoulderPadChord);
		CHECK(s.shoulderKey == 0x58);
		CHECK(s.reloadKey == 0 && s.probeKey == 0 && s.senseKey == 0 && s.cameraFlipKey == 0);
		CHECK(s.autoRewriteKey == 0 && s.swallowJumpKey == 0 && s.zeroMoveKey == 0 && s.halfMoveKey == 0 && s.swallowMethodKey == 0);
		// nothing of the rig
		CHECK(s.testScript.empty() && !s.quitAfterTest && s.rigArmFile.empty());
		CHECK(s.graphEventProbe == 0 && s.poseEntryProbe == 0 && s.equipFieldProbe == 0 && s.audioProbe == 0);
		CHECK(!s.autoProbe && !s.senseOnStop);  // m4b13's 0 for bAutoProbe, whose code default is 1 (found by the configuration check)
	}

	// a player's INI changes what it names; the rig keys and the debug keys are refused
	{
		std::istringstream in("[General]\nbFullLog=1\nsTestScript=p3\nbQuitAfterTest=1\nsRigArmFile=C:\\x.txt\niProbeKey=0x60\n[Cover]\nbCover=0\nfAutoCoverTime=0.5 ; slower\n");
		Config::Settings s;
		Config::ApplyReleaseDefaults(s);
		const auto report = Config::ParseIni(in, s, true);
		CHECK(s.fullLog);
		CHECK(!s.cover);
		CHECK(s.autoCoverTime == 0.5f);
		CHECK(s.testScript.empty() && !s.quitAfterTest && s.rigArmFile.empty());
		CHECK(s.probeKey == 0);
		CHECK(report.ignored.size() == 4);
		CHECK(report.unknown.empty());
	}

	// the development parser takes the rig keys as before
	{
		std::istringstream in("sTestScript=P3\nbQuitAfterTest=1\nbBogusKey=1\n");
		Config::Settings s;
		const auto report = Config::ParseIni(in, s, false);
		CHECK(s.testScript == "p3" && s.quitAfterTest);
		CHECK(report.unknown.size() == 1 && report.ignored.empty());
		CHECK(s.fullLog);              // the development default: the full log
		CHECK(!s.shoulderPadChord);    // the development default: the chord off (CoverSF.ini decides)
	}

	// the dump names every key once and reads the field it should
	{
		const auto s = Release("fShoulderLeftExtra=0.10\n");
		const auto d = Config::Dump(s);
		CHECK(d.size() > 200);
		int hits = 0;
		for (std::size_t i = 0; i < d.size(); ++i) {
			if (i > 0) {
				CHECK(d[i - 1].first < d[i].first);
			}
			if (d[i].first == "fshoulderleftextra") {
				CHECK(d[i].second == "0.1");
				++hits;
			}
			if (d[i].first == "bshoulderpadchord") {
				CHECK(d[i].second == "1");
				++hits;
			}
		}
		CHECK(hits == 2);
	}

	std::printf("ReleaseConfig: %s (%d failure(s))\n", g_failures == 0 ? "ok" : "FAILED", g_failures);
	return g_failures;
}
