#include "ConfigParse.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <type_traits>

// The INI key table (moved here from Config.cpp for release 1.0.0 so that it runs without the game). Every key the plugin knows is one line of
// ApplyPartA / ApplyPartB; tools/gen_config_dump.py reads those lines and writes src/ConfigKeys.inc, which Dump() uses.

namespace Config
{
	namespace
	{
		[[nodiscard]] std::string Trim(std::string_view a_text)
		{
			const auto isSpace = [](const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
			while (!a_text.empty() && isSpace(a_text.front())) {
				a_text.remove_prefix(1);
			}
			while (!a_text.empty() && isSpace(a_text.back())) {
				a_text.remove_suffix(1);
			}
			return std::string(a_text);
		}

		[[nodiscard]] std::string Lower(std::string a_text)
		{
			std::ranges::transform(a_text, a_text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_text;
		}

		[[nodiscard]] bool ApplyPartA(Settings& s, const std::string& key, const std::string& a_value)
		{
			const auto asBool = [&] { return a_value != "0" && Lower(a_value) != "false"; };
			const auto asInt = [&] { return static_cast<int>(std::strtol(a_value.c_str(), nullptr, 0)); };
			const auto asFloat = [&] { return std::strtof(a_value.c_str(), nullptr); };
			if (key == "benabled") { s.enabled = asBool(); return true; }
			if (key == "stestscript") { s.testScript = Lower(a_value); return true; }
			if (key == "srigarmfile") { s.rigArmFile = a_value; return true; }
			if (key == "fshovegrace") { s.shoveGrace = std::clamp(asFloat(), 0.0f, 3.0f); return true; }
			if (key == "bturnlock") { s.turnLock = asBool(); return true; }
			if (key == "iposescopeaim") { s.poseScopeAim = std::clamp(asInt(), 0, 2); return true; }
			if (key == "iscopesource") { s.scopeSource = std::clamp(asInt(), 1, 3); return true; }
			if (key == "iposedrawreevaluate") { s.poseDrawReevaluate = std::clamp(asInt(), 0, 1); return true; }
			if (key == "iequipfieldprobe") { s.equipFieldProbe = std::clamp(asInt(), 0, 1); return true; }
			if (key == "iposewaitbyte") { s.poseWaitByte = std::clamp(asInt(), 0, 2); return true; }
			if (key == "iposenorepeat") { s.poseNoRepeat = std::clamp(asInt(), 0, 1); return true; }
			if (key == "fposewaitbytemax") { s.poseWaitByteMax = std::clamp(asFloat(), 0.1f, 5.0f); return true; }
			if (key == "iposeentryprobe") { s.poseEntryProbe = std::clamp(asInt(), 0, 1); return true; }
			if (key == "iposeequippending") { s.poseEquipPending = std::clamp(asInt(), 0, 1); return true; }
			if (key == "igrapheventprobe") { s.graphEventProbe = std::clamp(asInt(), 0, 2); return true; }
			if (key == "iposeequipblock") { s.poseEquipBlock = std::clamp(asInt(), 0, 1); return true; }
			if (key == "sposeequipblockevents") { s.equipBlockEvents = Lower(a_value); return true; }
			if (key == "iposeweapondraw") { s.poseWeaponDraw = std::clamp(asInt(), 0, 1); return true; }
			if (key == "fposeweapondrawdelay") { s.poseWeaponDrawDelay = std::clamp(asFloat(), 0.0f, 1.0f); return true; }
			if (key == "iposefootiklow") { s.poseFootIKLow = std::clamp(asInt(), 0, 2); return true; }
			if (key == "iposefootik") { s.poseFootIK = std::clamp(asInt(), 0, 2); return true; }
			if (key == "bquitaftertest") { s.quitAfterTest = asBool(); return true; }
			if (key == "sshotackfile") { s.shotAckFile = a_value; return true; }
			if (key == "scamblackbox") { s.camBlackBox = a_value; return true; }
			if (key == "ig2endstage") { s.g2EndStage = asInt(); return true; }
			if (key == "it2startstage") { s.t2StartStage = (std::max)(asInt(), 1); return true; }
			if (key == "ip3set") { s.p3Set = (std::max)(asInt(), 1); return true; }
			if (key == "bcover") { s.cover = asBool(); return true; }
			if (key == "icovertrigger") { s.coverTrigger = asInt(); return true; }
			if (key == "fautocovertime") { s.autoCoverTime = (std::max)(asFloat(), 0.0f); return true; }
			if (key == "fautocoverrearm") { s.autoCoverRearm = (std::max)(asFloat(), 0.0f); return true; }
			if (key == "fautocoverangledeg") { s.autoCoverAngleDeg = std::clamp(asFloat(), 0.0f, 90.0f); return true; }
			if (key == "fautocoverdistance") { s.autoCoverDistance = (std::max)(asFloat(), 0.0f); return true; }
			if (key == "iautocovertall") { s.autoCoverTall = std::clamp(asInt(), 0, 2); return true; }
			if (key == "fautocovertimedrawn") { s.autoCoverTimeDrawn = (std::max)(asFloat(), 0.0f); return true; }
			if (key == "fautocovertimetall") { s.autoCoverTimeTall = (std::max)(asFloat(), 0.0f); return true; }
			if (key == "fautocovertalledge") { s.autoCoverTallEdge = (std::max)(asFloat(), 0.0f); return true; }
			if (key == "bcovernotify") { s.coverNotify = asBool(); return true; }
			if (key == "bcovercamera") { s.coverCamera = asBool(); return true; }
			if (key == "bcoverposes") { s.coverPoses = asBool(); return true; }
			if (key == "fposeholsterat") { s.poseHolsterAt = std::clamp(asFloat(), 0.0f, 5.0f); return true; }
			if (key == "fposedrawat") { s.poseDrawAt = std::clamp(asFloat(), 0.0f, 5.0f); return true; }
			if (key == "fposeconfirmat") { s.poseConfirmAt = std::clamp(asFloat(), 0.0f, 8.0f); return true; }
			if (key == "iposearchetypemode") { s.poseArchetypeMode = std::clamp(asInt(), 0, 2); return true; }
			if (key == "iposediagmask") { s.poseDiagMask = std::clamp(asInt(), 0, 31); return true; }
			if (key == "bposedrawweapon") { s.poseDrawWeapon = asBool(); return true; }
			if (key == "iposeweaponswap") { s.poseWeaponSwap = std::clamp(asInt(), 0, 3); return true; }
			if (key == "bposefreepitch") { s.poseFreePitch = asBool(); return true; }
			if (key == "iposefreepitchmode") { s.poseFreePitchMode = std::clamp(asInt(), 1, 2); return true; }
			if (key == "bposereload") { s.poseReload = asBool(); return true; }
			if (key == "fposereloadtime") { s.poseReloadTime = (std::max)(asFloat(), 0.0f); return true; }
			if (key == "fposeholsterhold") { s.poseHolsterHold = (std::max)(asFloat(), 0.0f); return true; }
			if (key == "fposeentryblend") { s.poseEntryBlend = std::clamp(asFloat(), 0.0f, 5.0f); return true; }
			if (key == "bposeholdworld") { s.poseHoldWorld = asBool(); return true; }
			if (key == "fposeexitease") { s.poseExitEase = std::clamp(asFloat(), 0.0f, 2.0f); return true; }
			if (key == "iposecornerhold") { s.poseCornerHold = std::clamp(asInt(), 0, 1); return true; }
			if (key == "iposereverse") { s.poseReverse = std::clamp(asInt(), 0, 2); return true; }
			if (key == "fposereversehold") { s.poseReverseHold = std::clamp(asFloat(), 0.0f, 1.0f); return true; }
			if (key == "fposereverserest") { s.poseReverseRest = std::clamp(asFloat(), 0.05f, 4.0f); return true; }
			if (key == "btallautoturn") { s.tallAutoTurn = asBool(); return true; }
			if (key == "ftallautoturnstill") { s.tallAutoTurnStill = (std::max)(asFloat(), 0.0f); return true; }
			if (key == "ftallautoturnmargin") { s.tallAutoTurnMargin = (std::max)(asFloat(), 0.0f); return true; }
			if (key == "fposecorneredge") { s.poseCornerEdge = std::clamp(asFloat(), 0.0f, 2.0f); return true; }
			if (key == "bposecornershoulder") { s.poseCornerShoulder = asBool(); return true; }
			if (key == "fposecornercone") { s.poseCornerCone = std::clamp(asFloat(), 0.0f, 180.0f); return true; }
			if (key == "bposecornercamera") { s.poseCornerCamera = asBool(); return true; }
			if (key == "iposeexitremedy") { s.poseExitRemedy = std::clamp(asInt(), 0, 5); return true; }
			if (key == "iposeexitvariant") { s.poseExitVariant = std::clamp(asInt(), 0, 5); return true; }
			if (key == "iposeexitclip") { s.poseExitClip = std::clamp(asInt(), 0, 1); return true; }
			if (key == "bposesticklog") { s.poseStickLog = asBool(); return true; }
			if (key == "bposeexithold") { s.poseExitHold = asBool(); return true; }
			if (key == "iposeexitturn") { s.poseExitTurn = std::clamp(asInt(), 0, 3); return true; }
			if (key == "bposezeroinput") { s.poseZeroInput = asBool(); return true; }
			if (key == "bposetallaim") { s.poseTallAim = asBool(); return true; }
			if (key == "bposetallblindfire") { s.poseTallBlindFire = asBool(); return true; }
			if (key == "iposetallaimswallow") { s.poseTallAimSwallow = std::clamp(asInt(), 0, 1); return true; }
			if (key == "bposetall") { s.poseTall = asBool(); return true; }
			if (key == "bposeblindfire") { s.poseBlindFire = asBool(); return true; }
			if (key == "fblindfireintolow") { s.blindFireIntoLow = std::clamp(asFloat(), 0.0f, 5.0f); return true; }
			if (key == "fblindfireintotall") { s.blindFireIntoTall = std::clamp(asFloat(), 0.0f, 5.0f); return true; }
			if (key == "fblindfirelinger") { s.blindFireLinger = std::clamp(asFloat(), 0.0f, 5.0f); return true; }
			if (key == "fblindfireoutlow") { s.blindFireOutLow = std::clamp(asFloat(), 0.0f, 5.0f); return true; }
			if (key == "fblindfireouttall") { s.blindFireOutTall = std::clamp(asFloat(), 0.0f, 5.0f); return true; }
			if (key == "fposeswitchwait") { s.poseSwitchWait = std::clamp(asFloat(), 0.0f, 2.0f); return true; }
			if (key == "fcovercambacklow") { s.coverCamBackLow = std::clamp(asFloat(), 0.0f, 3.0f); return true; }
			if (key == "fcovercamuplow") { s.coverCamUpLow = std::clamp(asFloat(), -1.0f, 2.0f); return true; }
			if (key == "fcovercambacktall") { s.coverCamBackTall = std::clamp(asFloat(), 0.0f, 3.0f); return true; }
			if (key == "fcovercamuptall") { s.coverCamUpTall = std::clamp(asFloat(), -1.0f, 2.0f); return true; }
			if (key == "fcovercamaimshare") { s.coverCamAimShare = std::clamp(asFloat(), 0.0f, 1.0f); return true; }
			if (key == "fcovercamease") { s.coverCamEase = std::clamp(asFloat(), 0.05f, 5.0f); return true; }
			if (key == "fcoverstickmin") { s.coverStickMin = asFloat(); return true; }
			if (key == "fcandidatemaxage") { s.candidateMaxAge = asFloat(); return true; }
			if (key == "fcandidatemaxturndeg") { s.candidateMaxTurnDeg = asFloat(); return true; }
			if (key == "fsenseratehz") { s.senseRateHz = (std::max)(asFloat(), 0.5f); return true; }
			if (key == "fstancechecktime") { s.stanceCheckTime = asFloat(); return true; }
			if (key == "fentermaxdistance") { s.enterMaxDistance = asFloat(); return true; }
			if (key == "fpathblockederror") { s.pathBlockedError = asFloat(); return true; }
			if (key == "fentertime") { s.machineCfg.enterTime = asFloat(); return true; }
			if (key == "fentermaxtime") { s.machineCfg.enterMaxTime = asFloat(); return true; }
			if (key == "fexittime") { s.machineCfg.exitTime = asFloat(); return true; }
			if (key == "fexitholdtime") { s.machineCfg.exitHoldTime = asFloat(); return true; }
			if (key == "fexitinputthreshold") { s.machineCfg.exitInputThreshold = asFloat(); return true; }
			if (key == "fmaxdeviation") { s.machineCfg.maxDeviation = asFloat(); return true; }
			if (key == "ilostleavecount") { s.machineCfg.lostLeaveCount = (std::max)(asInt(), 1); return true; }
			if (key == "bcoverfilterinput") { s.coverFilterInput = asBool(); return true; }
			if (key == "bcovermove") { s.coverMove = asBool(); return true; }
			if (key == "fcovermaxinput") { s.coverMaxInput = std::clamp(asFloat(), 0.05f, 1.0f); return true; }
			if (key == "fedgebrakedistance") { s.edgeBrakeDistance = (std::max)(asFloat(), 0.05f); return true; }
			if (key == "fedgestopdistance") { s.edgeStopDistance = (std::max)(asFloat(), 0.0f); return true; }
			if (key == "fedgestopdistancelow") { s.edgeStopDistanceLow = (std::max)(asFloat(), 0.0f); return true; }
			if (key == "fcoveralongmin") { s.coverAlongMin = std::clamp(asFloat(), 0.0f, 1.0f); return true; }
			if (key == "fentersettletime") { s.enterSettleTime = std::clamp(asFloat(), 0.0f, 3.0f); return true; }
			if (key == "fpeekspeed") { s.peekSpeed = (std::max)(asFloat(), 0.5f); return true; }
			if (key == "fcovermaxinputlow") { s.coverMaxInputLow = std::clamp(asFloat(), 0.05f, 1.0f); return true; }
			if (key == "fedgebrakedistancelow") { s.edgeBrakeDistanceLow = (std::max)(asFloat(), 0.05f); return true; }
			if (key == "ireloadkey") { s.reloadKey = asInt(); return true; }
			if (key == "fcoverholdtime") { s.coverHoldTime = asFloat(); return true; }
			if (key == "bpeek") { s.peek = asBool(); return true; }
			if (key == "bpeekover") { s.peekOver = asBool(); return true; }
			if (key == "fpeektime") { s.peekTime = asFloat(); return true; }
			if (key == "fsidepeekdistance") { s.sidePeekDistance = asFloat(); return true; }
			if (key == "fedgepeekrange") { s.edgePeekRange = asFloat(); return true; }
			if (key == "fstickexpiretime") { s.stickExpireTime = asFloat(); return true; }
			if (key == "fcovermovescale") { s.coverMoveScale = asFloat(); return true; }
			if (key == "fholdtolerance") { s.holdTolerance = asFloat(); return true; }
			if (key == "fholdmaxstep") { s.holdMaxStep = asFloat(); return true; }
			if (key == "fshovedistance") { s.shoveDistance = asFloat(); return true; }
			if (key == "faheadhalfangledeg") { s.sensorCfg.aheadHalfAngleDeg = asFloat(); return true; }
			if (key == "ig2startstage") { s.g2StartStage = (std::max)(asInt(), 1); return true; }
			if (key == "brewriteatcontrols") { s.rewriteAtControls = asBool(); return true; }
			if (key == "sjumpplans") { s.jumpPlans = a_value; return true; }
			if (key == "bjumprotation") { s.jumpRotation = asBool(); return true; }
			if (key == "bmovesequence") { s.moveSequence = asBool(); return true; }
			if (key == "bframetick") { s.frameTick = asBool(); return true; }
			if (key == "ilogintervalms") { s.logIntervalMs = static_cast<std::uint32_t>((std::max)(asInt(), 100)); return true; }
			if (key == "binputhook") { s.inputHook = asBool(); return true; }
			if (key == "icoverkey") { s.coverKey = asInt(); return true; }
			if (key == "imaxinputloglines") { s.maxInputLogLines = asInt(); return true; }
			if (key == "binputrewrite") { s.inputRewrite = asBool(); return true; }
			if (key == "iautorewritekey") { s.autoRewriteKey = asInt(); return true; }
			if (key == "bautorewritestart") { s.autoRewriteStart = asBool(); return true; }
			if (key == "iautorewritedelaysec") { s.autoRewriteDelaySec = (std::max)(asInt(), 5); return true; }
			if (key == "iswallowjumpkey") { s.swallowJumpKey = asInt(); return true; }
			return false;
		}

		[[nodiscard]] bool ApplyPartB(Settings& s, const std::string& key, const std::string& a_value)
		{
			const auto asBool = [&] { return a_value != "0" && Lower(a_value) != "false"; };
			const auto asInt = [&] { return static_cast<int>(std::strtol(a_value.c_str(), nullptr, 0)); };
			const auto asFloat = [&] { return std::strtof(a_value.c_str(), nullptr); };
			if (key == "bfulllog") { s.fullLog = asBool(); return true; }
			if (key == "bshoulderpadchord") { s.shoulderPadChord = asBool(); return true; }
			if (key == "fshoulderpadtrigger") { s.shoulderPadTrigger = std::clamp(asFloat(), 0.05f, 1.0f); return true; }
			if (key == "izeromovekey") { s.zeroMoveKey = asInt(); return true; }
			if (key == "ihalfmovekey") { s.halfMoveKey = asInt(); return true; }
			if (key == "iswallowmethodkey") { s.swallowMethodKey = asInt(); return true; }
			if (key == "fmovescale") { s.moveScale = asFloat(); return true; }
			if (key == "bworldprobe") { s.worldProbe = asBool(); return true; }
			if (key == "iprobekey") { s.probeKey = asInt(); return true; }
			if (key == "bautoprobe") { s.autoProbe = asBool(); return true; }
			if (key == "iautoprobems") { s.autoProbeMs = (std::max)(asInt(), 100); return true; }
			if (key == "fproberadius") { s.probeRadius = s.sensorCfg.probeRadius = asFloat(); return true; }
			if (key == "bsensor") { s.sensor = asBool(); return true; }
			if (key == "isensekey") { s.senseKey = asInt(); return true; }
			if (key == "bsenseonstop") { s.senseOnStop = asBool(); return true; }
			if (key == "fsensestilltime") { s.senseStillTime = (std::max)(asFloat(), 0.05f); return true; }
			if (key == "fsenseminmove") { s.senseMinMove = asFloat(); return true; }
			if (key == "fsenseturndeg") { s.senseTurnDeg = asFloat(); return true; }
			if (key == "fsensemininterval") { s.senseMinInterval = asFloat(); return true; }
			if (key == "fmaxsnapdistance") { s.sensorCfg.maxSnapDistance = asFloat(); return true; }
			if (key == "flowcoverminheight") { s.sensorCfg.lowCoverMinHeight = asFloat(); return true; }
			if (key == "fhighcoverminheight") { s.sensorCfg.highCoverMinHeight = asFloat(); return true; }
			if (key == "foverpeekmaxheight") { s.sensorCfg.overPeekMaxHeight = asFloat(); return true; }
			if (key == "fsidesnapangledeg") { s.sensorCfg.sideSnapAngleDeg = asFloat(); return true; }
			if (key == "fsidesnapdistance") { s.sensorCfg.sideSnapDistance = asFloat(); return true; }
			if (key == "fmincoverwidth") { s.sensorCfg.minCoverWidth = asFloat(); return true; }
			if (key == "fedgeprobestep") { s.sensorCfg.edgeProbeStep = asFloat(); return true; }
			if (key == "fedgemargin") { s.sensorCfg.edgeMargin = asFloat(); return true; }
			if (key == "ftrackratehz") { s.sensorCfg.trackRateHz = asFloat(); return true; }
			if (key == "fsearchheight") { s.sensorCfg.searchHeight = asFloat(); return true; }
			if (key == "isearchrays") { s.sensorCfg.searchRays = (std::max)(asInt(), 1); return true; }
			if (key == "fsearchhalfangledeg") { s.sensorCfg.searchHalfAngleDeg = asFloat(); return true; }
			if (key == "fsearchangleweight") { s.sensorCfg.searchAngleWeight = asFloat(); return true; }
			if (key == "fwallfitstep") { s.sensorCfg.wallFitStep = asFloat(); return true; }
			if (key == "fwallfitmaxrms") { s.sensorCfg.wallFitMaxRms = asFloat(); return true; }
			if (key == "fstandoff") { s.sensorCfg.standoff = asFloat(); return true; }
			if (key == "fneartolerance") { s.sensorCfg.nearTolerance = asFloat(); return true; }
			if (key == "fheightstep") { s.sensorCfg.heightStep = (std::max)(asFloat(), 0.05f); return true; }
			if (key == "fheightstart") { s.sensorCfg.heightStart = asFloat(); return true; }
			if (key == "fmaxmeasuredheight") { s.sensorCfg.maxMeasuredHeight = asFloat(); return true; }
			if (key == "fcapsuleradius") { s.sensorCfg.capsuleRadius = asFloat(); return true; }
			if (key == "fstandgap") { s.sensorCfg.standGap = asFloat(); return true; }
			if (key == "fmaxfloorstep") { s.sensorCfg.maxFloorStep = asFloat(); return true; }
			if (key == "fedgesearchrange") { s.sensorCfg.edgeSearchRange = asFloat(); return true; }
			if (key == "ftrackedgerange") { s.sensorCfg.trackEdgeRange = asFloat(); return true; }
			if (key == "iedgemisscount") { s.sensorCfg.edgeMissCount = (std::max)(asInt(), 1); return true; }
			if (key == "fmaxnormalturndeg") { s.sensorCfg.maxNormalTurnDeg = asFloat(); return true; }
			if (key == "ftracktolerance") { s.sensorCfg.trackTolerance = asFloat(); return true; }
			if (key == "fproberange") { s.probeRange = asFloat(); return true; }
			if (key == "fchestheight") { s.chestHeight = asFloat(); return true; }
			if (key == "ffanhalfangledeg") { s.fanHalfAngleDeg = asFloat(); return true; }
			if (key == "imaxcastsperframe") { s.maxCastsPerFrame = (std::max)(asInt(), 1); return true; }
			if (key == "bcamera") { s.camera = asBool(); return true; }
			if (key == "fshoulderoffset") { s.shoulderOffset = asFloat(); return true; }
			if (key == "fcamerasmoothtime") { s.cameraSmoothTime = asFloat(); return true; }
			if (key == "icameraflipkey") { s.cameraFlipKey = asInt(); return true; }
			if (key == "bcameracollision") { s.cameraCollision = asBool(); return true; }
			if (key == "icameramode") { s.cameraMode = asInt(); return true; }
			if (key == "bcameraflipondraw") { s.cameraFlipOnDraw = asBool(); return true; }
			if (key == "icamerarightsource") { s.cameraRightSource = asInt(); return true; }
			if (key == "icamerachain") { s.cameraChain = std::clamp(asInt(), 0, 1); return true; }
			if (key == "scamerachainmodules") { s.cameraChainModules = a_value; return true; }
			if (key == "bcamerasettings") { s.cameraSettings = asBool(); return true; }
			if (key == "fcameraclearance") { s.cameraClearance = asFloat(); return true; }
			if (key == "bframing") { s.framing = asBool(); return true; }
			if (key == "ishoulderkey") { s.shoulderKey = asInt(); return true; }
			if (key == "fshoulderleftextra") { s.shoulderLeftExtra = std::clamp(asFloat(), -0.5f, 0.5f); return true; }
			if (key == "fshoulderswapease") { s.shoulderSwapEase = std::clamp(asFloat(), 0.0f, 3.0f); return true; }
			if (key == "fframeease") { s.frameEase = std::clamp(asFloat(), 0.0f, 3.0f); return true; }
			if (key == "fframedistexplore") { s.frameDistExplore = std::clamp(asFloat(), 0.2f, 6.0f); return true; }
			if (key == "fframedistdrawn") { s.frameDistDrawn = std::clamp(asFloat(), 0.2f, 6.0f); return true; }
			if (key == "fframedistaim") { s.frameDistAim = std::clamp(asFloat(), 0.2f, 6.0f); return true; }
			if (key == "fframedistsprint") { s.frameDistSprint = std::clamp(asFloat(), 0.2f, 6.0f); return true; }
			if (key == "fframeshoulderexplore") { s.frameShoulderExplore = std::clamp(asFloat(), -1.0f, 1.5f); return true; }
			if (key == "fframeshoulderdrawn") { s.frameShoulderDrawn = std::clamp(asFloat(), -1.0f, 1.5f); return true; }
			if (key == "fframeshoulderaim") { s.frameShoulderAim = std::clamp(asFloat(), -1.0f, 1.5f); return true; }
			if (key == "fframeshouldersprint") { s.frameShoulderSprint = std::clamp(asFloat(), -1.0f, 1.5f); return true; }
			if (key == "fframeheightdrawn") { s.frameHeightDrawn = std::clamp(asFloat(), -1.0f, 1.0f); return true; }
			if (key == "fframeheightaim") { s.frameHeightAim = std::clamp(asFloat(), -1.0f, 1.0f); return true; }
			if (key == "bframelog") { s.frameLog = asBool(); return true; }
			if (key == "bplayercontrol") { s.playerControl = asBool(); return true; }
			if (key == "banimrecon") { s.animRecon = asBool(); return true; }
			if (key == "iaudioprobe") { s.audioProbe = std::clamp(asInt(), 0, 2); return true; }
			if (key == "iposereloadsoundfile") { s.poseReloadSoundFile = std::clamp(asInt(), 0, 1); return true; }
			if (key == "iposereloadsound") { s.poseReloadSound = std::clamp(asInt(), 0, 2); return true; }
			if (key == "bstancespike") { s.stanceSpike = asBool(); return true; }
			if (key == "ip5mode") { s.p5Mode = std::clamp(asInt(), 1, 7); return true; }
			if (key == "brighold") { s.rigHold = asBool(); return true; }
			return false;
		}

		// The keys the release refuses: the test rig's (a script, the quit, the arming file, the stage and set numbers, the probes and spikes that only the rig
		// switches on) and the debug keys (Numpad presses that toggled S2b tests or logged probes). In the release they keep the values of ApplyReleaseDefaults.
		constexpr std::string_view kReleaseIgnored[] = {
			"stestscript", "bquitaftertest", "srigarmfile", "sshotackfile", "scamblackbox", "ig2endstage", "ig2startstage", "it2startstage", "ip3set", "ip5mode",
			"bstancespike", "brighold", "iposediagmask", "iequipfieldprobe", "iposeentryprobe", "igrapheventprobe", "iposeequipblock", "sposeequipblockevents",
			"iposeweapondraw", "iposedrawreevaluate", "iaudioprobe", "iposeequippending", "bcameraflipondraw", "sjumpplans", "bjumprotation", "bmovesequence",
			"bautorewritestart", "iautorewritedelaysec", "bautoprobe", "bsenseonstop",
			"ireloadkey", "iprobekey", "isensekey", "icameraflipkey", "iautorewritekey", "iswallowjumpkey", "izeromovekey", "ihalfmovekey", "iswallowmethodkey",
		};

		struct DefaultPair
		{
			std::string_view key;
			std::string_view value;
			bool             own;  // a value of the release's own (after m4b13's); for the reader of ReleaseDefaults.inc
		};

		constexpr DefaultPair kReleaseDefaults[] = {
#include "ReleaseDefaults.inc"
		};

		// the release's own debug-key values are written past the refusal (ApplyKey would refuse them)
		[[nodiscard]] bool ApplyAny(Settings& a_settings, const std::string& a_key, const std::string& a_value)
		{
			return ApplyPartA(a_settings, a_key, a_value) || ApplyPartB(a_settings, a_key, a_value);
		}

		[[nodiscard]] std::string Str(const bool a_v) { return a_v ? "1" : "0"; }
		[[nodiscard]] std::string Str(const int a_v) { return std::to_string(a_v); }
		[[nodiscard]] std::string Str(const std::uint32_t a_v) { return std::to_string(a_v); }
		[[nodiscard]] std::string Str(const float a_v) { return std::format("{}", a_v); }
		[[nodiscard]] std::string Str(const double a_v) { return std::format("{}", a_v); }
		[[nodiscard]] std::string Str(const std::string& a_v) { return a_v; }
	}

	bool IsReleaseIgnoredKey(const std::string_view a_lowerKey)
	{
		return std::ranges::find(kReleaseIgnored, a_lowerKey) != std::end(kReleaseIgnored);
	}

	KeyResult ApplyKey(Settings& a_settings, const std::string& a_key, const std::string& a_value, const bool a_release)
	{
		const auto key = Lower(a_key);
		if (a_release && IsReleaseIgnoredKey(key)) {
			return KeyResult::IgnoredInRelease;
		}
		return ApplyAny(a_settings, key, a_value) ? KeyResult::Applied : KeyResult::Unknown;
	}

	ParseReport ParseIni(std::istream& a_in, Settings& a_settings, const bool a_release)
	{
		ParseReport report;
		std::string line;
		while (std::getline(a_in, line)) {
			const auto trimmed = Trim(line);
			if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#' || trimmed[0] == '[') {
				continue;
			}
			const auto eq = trimmed.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			// Anything after a ';' on a value line is a comment.
			auto value = trimmed.substr(eq + 1);
			if (const auto semi = value.find(';'); semi != std::string::npos) {
				value.resize(semi);
			}
			const auto key = Trim(trimmed.substr(0, eq));
			switch (ApplyKey(a_settings, key, Trim(value), a_release)) {
			case KeyResult::Unknown:
				report.unknown.push_back(key);
				break;
			case KeyResult::IgnoredInRelease:
				report.ignored.push_back(key);
				break;
			default:
				break;
			}
		}
		return report;
	}

	void ApplyReleaseDefaults(Settings& a_settings)
	{
		for (const auto& d : kReleaseDefaults) {
			const std::string key(d.key);
			const std::string value(d.value);
			// every row, the rig and debug keys too: m4b13's values for them are the play values (an empty script, no quit, bAutoProbe 0 where the code default is 1);
			// only a player's INI is refused those keys
			(void)ApplyAny(a_settings, key, value);
		}
	}

	std::vector<std::pair<std::string, std::string>> Dump(const Settings& a_settings)
	{
		const auto& s = a_settings;
		std::vector<std::pair<std::string, std::string>> out;
#define COVER_KEY(a_name, a_field) out.emplace_back(a_name, Str(s.a_field));
#include "ConfigKeys.inc"
#undef COVER_KEY
		std::ranges::sort(out);
		return out;
	}
}
