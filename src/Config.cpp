#include "Config.h"

#include "ConfigParse.h"
#include "Core/PluginNames.h"

#include "REX/W32/KERNEL32.h"

namespace Config
{
	namespace
	{
		Settings s_settings;

		[[nodiscard]] std::filesystem::path ModulePath()
		{
			wchar_t buffer[REX::W32::MAX_PATH]{};
			REX::W32::GetModuleFileNameW(REX::W32::GetCurrentModule(), buffer, REX::W32::MAX_PATH);
			return std::filesystem::path(buffer);
		}

		[[nodiscard]] std::filesystem::path IniPath()
		{
			return ModulePath().parent_path() / Core::IniFileName(BaseName());
		}

		// The settings before the INI: the code defaults, and in the release m4b13's values on top (ConfigParse.h).
		[[nodiscard]] Settings StartingSettings()
		{
			Settings s;
			if (IsRelease()) {
				ApplyReleaseDefaults(s);
			}
			return s;
		}

		// Reads the INI into a_settings; false when the file is missing.
		bool ReadIni(const std::filesystem::path& a_path, Settings& a_settings)
		{
			std::ifstream file(a_path);
			if (!file.is_open()) {
				return false;
			}
			const auto report = ParseIni(file, a_settings, IsRelease());
			for (const auto& key : report.unknown) {
				REX::WARN("{}: unknown key '{}' ignored", a_path.filename().string(), key);
			}
			for (const auto& key : report.ignored) {
				REX::WARN("{}: '{}' is a key of the test rig or a debug key; the release ignores it", a_path.filename().string(), key);
			}
			return true;
		}
	}

	const std::string& BaseName()
	{
		static const std::string s_base = Core::ModuleBaseName(ModulePath().string(), IsRelease() ? "TacticalCover" : "CoverSF");
		return s_base;
	}

	const char* DisplayName() noexcept
	{
		return IsRelease() ? "Tactical Cover" : "CoverSF";
	}

	std::filesystem::path Directory()
	{
		return IniPath().parent_path();
	}

	void Reload()
	{
		// T3.5: re-reads the INI into a scratch copy and swaps it in in one assignment, so a reader never sees defaults in between. The three strings
		// (test script, shot file, jump plans) keep their values: they are read from other threads and a script is not changed in mid-run.
		Settings   scratch = StartingSettings();
		const bool ok = ReadIni(IniPath(), scratch);
		if (!ok) {
			REX::WARN("Config: reload: {} not found; nothing changed", IniPath().string());
			return;
		}
		scratch.testScript = s_settings.testScript;
		scratch.shotAckFile = s_settings.shotAckFile;
		scratch.jumpPlans = s_settings.jumpPlans;
		if (scratch.testScript == "p3") {
			scratch.cover = true;
			scratch.jumpRotation = false;
			scratch.moveSequence = false;
		}
		s_settings = scratch;
		Log::SetFull(s_settings.fullLog);
		const auto& s = s_settings;
		COVER_DETAIL("Config: RELOADED {} | COVER bCover={} trigger={} stickMin={:.2f} candidateMaxAge={:.2f} senseRate={:.0f} enterTime={:.2f}/{:.2f} exitHold={:.2f} maxDeviation={:.2f} holdTolerance={:.2f} peek={} peekOver={} peekTime={:.2f} sidePeek={:.2f} edgePeekRange={:.2f} | edgeMargin={:.2f} trackHz={:.0f}",
			IniPath().string(), s.cover, s.coverTrigger, s.coverStickMin, s.candidateMaxAge, s.senseRateHz, s.machineCfg.enterTime, s.machineCfg.enterMaxTime, s.machineCfg.exitHoldTime, s.machineCfg.maxDeviation, s.holdTolerance, s.peek, s.peekOver, s.peekTime,
			s.sidePeekDistance, s.edgePeekRange, s.sensorCfg.edgeMargin, s.sensorCfg.trackRateHz);
	}

	Settings& Mutable() noexcept
	{
		return s_settings;
	}

	void Load()
	{
		const auto path = IniPath();
		s_settings = StartingSettings();
		if (!ReadIni(path, s_settings)) {
			REX::INFO("Config: {} not found; using built-in defaults", path.string());
		} else {
			REX::INFO("Config: read {}", path.string());
		}
		if (IsRelease()) {
			// nothing in the release can start a test: the keys are refused by the parser, and this is the second lock
			s_settings.testScript.clear();
			s_settings.quitAfterTest = false;
			s_settings.rigArmFile.clear();
		}
		Log::SetFull(s_settings.fullLog);

		// the arming guard (23.16): a rig script and the quit are honoured only when the helper wrote the arming file in the last 3 minutes
		if (!s_settings.testScript.empty() || s_settings.quitAfterTest) {
			bool        armed = false;
			std::string why = "no sRigArmFile";
			if (!s_settings.rigArmFile.empty()) {
				std::error_code ec;
				const auto      ft = std::filesystem::last_write_time(s_settings.rigArmFile, ec);
				if (ec) {
					why = std::format("the arming file {} does not exist", s_settings.rigArmFile);
				} else {
					const auto age = std::chrono::duration_cast<std::chrono::seconds>(std::filesystem::file_time_type::clock::now() - ft).count();
					armed = age >= -5 && age < 180;
					why = std::format("the arming file {} is {} s old", s_settings.rigArmFile, age);
				}
			}
			if (armed) {
				REX::INFO("Config: rig state found (sTestScript '{}', quit {}) and ARMED ({})", s_settings.testScript, s_settings.quitAfterTest, why);
			} else {
				REX::WARN("Config: rig state found (sTestScript '{}', quit {}), not armed: ignored ({}); this session behaves as play", s_settings.testScript, s_settings.quitAfterTest, why);
				s_settings.testScript.clear();
				s_settings.quitAfterTest = false;
			}
		}

		// The p3 rig script plays the cover feature itself: it switches it on for its own run and the S2b jump rotation off (that rotation swallows
		// Jump presses, which would fake a failure of "leave by Jump").
		if (s_settings.testScript == "p3") {
			s_settings.cover = true;
			s_settings.jumpRotation = false;
			s_settings.moveSequence = false;
			REX::INFO("Config: sTestScript=p3: the cover feature is switched on for this run, the jump rotation and the move sequence off");
		}
		// the p5 script (T5.0) meets only the engine: the Phase 3 cover feature, the jump rotation and the move sequence are off
		if (s_settings.testScript == "p5") {
			s_settings.cover = false;
			s_settings.jumpRotation = false;
			s_settings.moveSequence = false;
			REX::INFO("Config: sTestScript=p5: the cover feature, the jump rotation and the move sequence are switched off for this run (stance spike {}, mode {})", s_settings.stanceSpike, s_settings.p5Mode);
		}
		const auto& s = s_settings;
		REX::INFO("Config: {} ({} build): cover {}, poses {}, own camera {}, shoulder swap: key 0x{:02X}, controller LT + A {}; full log {}", DisplayName(), IsRelease() ? "release" : "development",
			s.enabled && s.cover ? "on" : "off", s.coverPoses ? "on" : "off", s.camera && s.framing ? "on" : "off", s.shoulderKey, s.shoulderPadChord ? "on" : "off", s.fullLog ? "on" : "off (quiet)");
		COVER_DETAIL("Config: testScript='{}' quitAfterTest={} jumpRotation={} moveSequence={} autoRewriteStart={}", s_settings.testScript,
			s_settings.quitAfterTest, s_settings.jumpRotation, s_settings.moveSequence, s_settings.autoRewriteStart);
		COVER_DETAIL("Config: camera={} shoulderOffset={:.2f} smooth={:.2f} flipKey=0x{:02X} collision={} clearance={:.2f} | playerControl={} animRecon={}", s.camera, s.shoulderOffset,
			s.cameraSmoothTime, s.cameraFlipKey, s.cameraCollision, s.cameraClearance, s.playerControl, s.animRecon);
		COVER_DETAIL("Config: FRAMING (M4a) {} | shoulder key 0x{:02X}, left extra {:.2f} m, swap {:.2f} s, ease {:.2f} s | distance behind: explore {:.2f}, drawn {:.2f}, aim {:.2f}, sprint {:.2f} | shoulder: explore {:.2f}, drawn {:.2f}, aim {:.2f}, sprint {:.2f} | height: drawn {:+.2f}, aim {:+.2f}",
			s.framing ? "ON" : "off", s.shoulderKey, s.shoulderLeftExtra, s.shoulderSwapEase, s.frameEase, s.frameDistExplore, s.frameDistDrawn, s.frameDistAim, s.frameDistSprint, s.frameShoulderExplore, s.frameShoulderDrawn, s.frameShoulderAim,
			s.frameShoulderSprint, s.frameHeightDrawn, s.frameHeightAim);
		{
			const auto& c = s.sensorCfg;
			COVER_DETAIL("Config: sensor={} senseKey=0x{:02X} senseOnStop={} (still {:.2f} s, move {:.2f} m, turn {:.0f} deg, interval {:.1f} s) | snap {:.2f} radius {:.2f} low {:.2f} high {:.2f} overPeek {:.2f} sideSnap {:.0f} deg / {:.2f} m width {:.2f} edgeStep {:.2f} edgeMargin {:.2f} trackHz {:.0f} | searchHeight {:.2f} rays {} halfAngle {:.0f} fitStep {:.2f} fitRms {:.3f} standoff {:.2f} near {:.2f} heightStep {:.2f} capsule {:.2f} gap {:.2f} floorStep {:.2f} edgeRange {:.1f}/{:.1f} edgeMisses {}",
				s.sensor, s.senseKey, s.senseOnStop, s.senseStillTime, s.senseMinMove, s.senseTurnDeg, s.senseMinInterval, c.maxSnapDistance, c.probeRadius, c.lowCoverMinHeight, c.highCoverMinHeight, c.overPeekMaxHeight, c.sideSnapAngleDeg, c.sideSnapDistance, c.minCoverWidth, c.edgeProbeStep, c.edgeMargin, c.trackRateHz, c.searchHeight, c.searchRays, c.searchHalfAngleDeg,
				c.wallFitStep, c.wallFitMaxRms, c.standoff, c.nearTolerance, c.heightStep, c.capsuleRadius, c.standGap, c.maxFloorStep, c.edgeSearchRange, c.trackEdgeRange, c.edgeMissCount);
		}
		COVER_DETAIL("Config: COVER bCover={} trigger={} stickMin={:.2f} candidateMaxAge={:.2f} maxTurn={:.0f} senseRate={:.0f} enterTime={:.2f}/{:.2f} exitTime={:.2f} exitHold={:.2f} exitInput={:.2f} maxDeviation={:.2f} lostLeave={} aheadHalfAngle={:.0f} filterInput={} move={}",
			s.cover, s.coverTrigger, s.coverStickMin, s.candidateMaxAge, s.candidateMaxTurnDeg, s.senseRateHz, s.machineCfg.enterTime, s.machineCfg.enterMaxTime, s.machineCfg.exitTime, s.machineCfg.exitHoldTime,
			s.machineCfg.exitInputThreshold, s.machineCfg.maxDeviation, s.machineCfg.lostLeaveCount, s.sensorCfg.aheadHalfAngleDeg, s.coverFilterInput, s.coverMove);
		COVER_DETAIL("Config: COVER POSES {} | archetype mode {} | switch wait {:.2f} s, holster at {:.2f} s, draw at {:.2f} s, confirm at {:.2f} s", s.coverPoses ? "ON" : "off", s.poseArchetypeMode, s.poseSwitchWait, s.poseHolsterAt, s.poseDrawAt, s.poseConfirmAt);
		COVER_DETAIL("Config: pose entry (23.32, 23.36): wait for the archetype-pending byte {} (at most {:.2f} s), no repeat call {}, entry probe {}", s.poseWaitByte == 2 ? "ON, and the equip byte too" : (s.poseWaitByte != 0 ? "ON" : "off"), s.poseWaitByteMax, s.poseNoRepeat != 0 ? "ON" : "off", s.poseEntryProbe != 0 ? "ON" : "off");
		COVER_DETAIL("Config: COVER CAMERA {} | low back {:.2f} up {:.2f}, tall back {:.2f} up {:.2f}, aim share {:.2f}, ease {:.2f} s", s.coverCamera ? "ON" : "off", s.coverCamBackLow, s.coverCamUpLow, s.coverCamBackTall, s.coverCamUpTall, s.coverCamAimShare, s.coverCamEase);
		COVER_DETAIL("Config: AUTOCOVER (mode 3) time={:.2f} rearm={:.2f} angle={:.0f} deg distance={:.2f} tallEdge={:.2f} | tall cover mode {} (0 never, 1 near an open end, 2 anywhere with the weapon drawn), weapon drawn hold {:.2f} s | notify={}", s.autoCoverTime, s.autoCoverRearm, s.autoCoverAngleDeg, s.autoCoverDistance, s.autoCoverTallEdge, s.autoCoverTall, s.autoCoverTimeDrawn, s.coverNotify);
		COVER_DETAIL("Config: enabled={} frameTick={} inputHook={} inputRewrite={} worldProbe={} | keys cover=0x{:02X} swallowJump=0x{:02X} zeroMove=0x{:02X} halfMove=0x{:02X} method=0x{:02X} probe=0x{:02X}",
			s.enabled, s.frameTick, s.inputHook, s.inputRewrite, s.worldProbe,
			s.coverKey, s.swallowJumpKey, s.zeroMoveKey, s.halfMoveKey, s.swallowMethodKey, s.probeKey);
	}

	const Settings& Get() noexcept
	{
		return s_settings;
	}
}
