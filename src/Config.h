#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "Core/CoverSensor.h"
#include "Core/CoverStateMachine.h"

// Settings read from <dll name>.ini next to the DLL (CoverSF.ini for the development build, TacticalCover.ini for the release). A missing file or key means the default
// below; the release build first applies its compiled-in defaults (ConfigParse.h, ApplyReleaseDefaults: the accepted play state m4b13), so a key left out of
// TacticalCover.ini has m4b13's value there.
// Every Phase 1 spike has its own switch (PLAN.md rule 3): set it to 0 to keep that spike out of the game.
//
// Key codes are the idCode of the game's keyboard button events. Test run 1 (2026-11-16) showed they are
// Windows virtual-key codes (W = 0x57, Numpad 0 = 0x60, NumLock = 0x90), not DirectInput scancodes; the log
// names the idCode of every key that is pressed, so a wrong one is easy to correct in the INI.

namespace Config
{
	struct Settings
	{
		// [General]
		bool enabled{ true };
		bool fullLog{ true };  // bFullLog (release 1.0.0): 1 = every log line as before (the development default, the user's own copy); 0 = the quiet log: the load and version, the hook decisions, faults, one line per cover entry and exit
		// S0 test rig. Name of a script the plugin runs by itself once a save has loaded ("s2b"), empty for none.
		// Synthetic key presses are only ever sent while the game window is in the foreground.
		std::string testScript{};
		int         g2EndStage{ 999 };  // g2 script: stop (successfully) before this stage index
		int         g2StartStage{ 1 };  // g2 script: first stage after "ready" (1 = everything; 9 = the S4 camera stages only)
		std::string camBlackBox{};  // sCamBlackBox: a file the camera hook writes one line per frame to, flushed after every line (empty = off): what the last frames were when the game stops (REFERENCE 23.2)
		std::string shotAckFile{};  // g2 script: file whose appearance means "the screenshot is taken" (empty: g2_shot_ack.txt next to the DLL)
		int         t2StartStage{ 1 };  // t2 script: first stage after "ready" (1 = everything; 5 = the sensor stages only)
		int         reloadKey{ 0x69 };   // iReloadKey: Numpad 9 re-reads the INI (T3.5)
		float       coverHoldTime{ 0.25f };  // fCoverHoldTime: iCoverTrigger 2: the Sneak control held this long takes cover
		std::string rigArmFile{};            // sRigArmFile: a rig script (sTestScript) and the quit run only when this file exists and is younger than 3 minutes at load; else "rig state found, not armed: ignored"
		bool        quitAfterTest{ false };  // set Main::quitGame after TEST COMPLETE (unverified, see Engine/Offsets.h)
		int         p3Set{ 1 };          // p3 script (Phase 3 rig): which set of tests runs (1 = Save29 wall, 2 = Save30 low wall, 3 = the Well doorway)

		// [FrameTick] S1
		bool          frameTick{ true };
		std::uint32_t logIntervalMs{ 1000 };

		// [Input] S2
		bool inputHook{ true };
		int  coverKey{ 0x65 };  // Numpad 5. Stand-in for the real cover key, which is chosen in Phase 3
		int  maxInputLogLines{ 1500 };

		// [InputRewrite] S2b. Debug keys: Numpad 1 to 4 and 7, NumLock on
		bool inputRewrite{ true };
		int  autoRewriteKey{ 0x61 };    // Numpad 1: start/stop the automatic S2b test cycle (see Engine/Input.cpp)
		std::string jumpPlans{ "0,2,3,4,1" };  // order of the rotation: 0 pass, 1 unlink, 2 disabled flag, 3 status stop, 4 value zero
		bool rewriteAtControls{ true };  // apply the rewrites in PlayerControls::Manager's receiver (false: the PlayerCamera hook, which is too late)
		bool jumpRotation{ false };       // event-driven Jump test: each counted press takes the next plan of the rotation
		bool moveSequence{ false };      // the speed-based movement test (replaced by the rig's displacement test, which is simpler to satisfy)
		bool autoRewriteStart{ false };  // the old timed cycle; off by default since the event-driven tests replaced it
		int  autoRewriteDelaySec{ 45 };
		int  swallowJumpKey{ 0x67 };    // Numpad 7: toggle swallowing of Jump presses by hand
		int  zeroMoveKey{ 0x62 };       // Numpad 2: toggle "movement value forced to 0"
		int  halfMoveKey{ 0x63 };       // Numpad 3: toggle "movement value scaled by moveScale"
		int  swallowMethodKey{ 0x64 };  // Numpad 4: cycle the method used to swallow Jump
		float moveScale{ 0.5f };

		// [WorldProbe] S3
		bool  worldProbe{ true };
		int   probeKey{ 0x60 };  // Numpad 0: one full probe now (forward, a nine-ray fan, floor calibration)
		bool  autoProbe{ true };        // compact probe (forward, down, left, right) logged on its own while you play
		int   autoProbeMs{ 500 };
		float probeRadius{ 0.10f };
		float probeRange{ 5.0f };
		float chestHeight{ 1.2f };  // above the player's origin; metres if S1 confirms the units
		float fanHalfAngleDeg{ 40.0f };
		int   maxCastsPerFrame{ 32 };  // T2.0: casts run per ThirdPersonState::Update call (the queue holds the rest for the next frame)

		// [Sensor] T2.2 / T2.3. The thresholds live in sensorCfg (Core::SensorSettings); fProbeRadius above feeds both.
		bool                 sensor{ true };    // the debug key and the t2 rig script may run the sensor
		int                  senseKey{ 0x68 };  // Numpad 8: log the cover candidate in front of the camera
		bool                 senseOnStop{ false };  // sense by itself (no key): when the player stops after moving 0.5 m, or the view has turned and settled
		float                senseStillTime{ 0.4f };    // seconds of standing still (or a steady view) before it senses
		float                senseMinMove{ 0.5f };      // metres moved since the last sense for the stop trigger
		float                senseTurnDeg{ 30.0f };     // degrees turned since the last sense for the turn trigger
		float                senseMinInterval{ 1.0f };  // seconds between two senses at the least
		Core::SensorSettings sensorCfg{};

		// [Camera] S4
		bool  camera{ true };
		float shoulderOffset{ 0.0f };     // metres to the right of the view, negative = left. 0 = the hook leaves the camera alone
		float cameraSmoothTime{ 0.20f };  // seconds for the offset to cover about 63 % of a change
		int   cameraFlipKey{ 0x66 };      // Numpad 6: flip the side
		int   cameraMode{ 4 };               // 0 = offset off, 3 = added in the call-site hook (turns the view), 4 = pivot shift (S4b, default)
		bool  cameraFlipOnDraw{ false };     // test aid: swap the side each time the weapon is drawn (a look-and-feel test without keyboard presses)
	int   cameraRightSource{ 0 };        // call-site hook: 0 = the right vector from row 0 of the returned rotation, 1 = camera-to-player geometry
		int         cameraChain{ 1 };                // [Camera] iCameraChain (M4b11, REFERENCE 23.27): 0 = stand down when slot 12 of the ThirdPersonState vtable is held by another mod (the old behaviour), 1 = chain in front of an allow-listed owner
		std::string cameraChainModules{ "DDC.dll" };  // [Camera] sCameraChainModules: file names of the plugins whose slot-12 function is called as the original (DyMC.dll is never chained)
		bool  cameraSettings{ true };        // the engine's own camera INI settings (Engine/CameraSettings): logged at load, written only by the g2 script
		bool  cameraShiftChildren_unused{ false };  // also move the root's descendants (the NiCamera), see Engine/Camera.cpp
		bool  cameraCollision{ true };    // shorten the offset when a sphere cast from the camera hits something
		float cameraClearance{ 0.15f };   // metres kept between the camera and the thing it would hit
		// M4a (Phase 4): the own camera. The framing per situation is written into the rig nodes (boom +0x1F8: local y = -distance behind; pivot +0x200: local x = the shoulder, local z = the game's own height + the height offset)
		// before the pose call and put back after it; every change eases over fFrameEase seconds. Off in compatibility mode (another camera mod owns the camera).
		bool  framing{ true };              // bFraming
		int   shoulderKey{ 0x58 };          // iShoulderKey: the keyboard key that swaps the shoulder (the user's controller layout sends X for LT + A); 0 = none
		bool  shoulderPadChord{ false };   // bShoulderPadChord (release 1.0.0, Core/SwapChord): LT + A on a controller swaps the shoulder, read by the plugin; the A press is kept from the game (1 in the release)
		float shoulderPadTrigger{ 0.5f };  // fShoulderPadTrigger: the left trigger counts as held from this value on
		float shoulderLeftExtra{ 0.06f };   // fShoulderLeftExtra: the left shoulder is the mirror of the right plus this (metres)
		float shoulderSwapEase{ 0.15f };    // fShoulderSwapEase: seconds for a swap
		float frameEase{ 0.25f };           // fFrameEase: seconds for a change of situation
		float frameDistExplore{ 2.1f }, frameDistDrawn{ 1.7f }, frameDistAim{ 0.95f }, frameDistSprint{ 2.4f };                       // metres behind the pivot
		float frameShoulderExplore{ 0.45f }, frameShoulderDrawn{ 0.45f }, frameShoulderAim{ 0.40f }, frameShoulderSprint{ 0.45f };    // metres to the side of the pivot (the right shoulder; the left adds fShoulderLeftExtra)
		float frameHeightDrawn{ -0.05f }, frameHeightAim{ -0.05f };                                                                  // metres added to the pivot's own height
		bool  frameLog{ true };             // bFrameLog: one line when the situation changes

		// [Cover] Phase 3. The master switch is off in the repository INI: nothing of the feature runs unless it is 1 (rig scripts switch it on for their own run).
		bool  cover{ false };               // bCover
		int   coverTrigger{ 1 };            // iCoverTrigger: 1 = the game's Sneak control while pushing toward cover (T3.5: 0 dedicated key, 2 hold)
		// M1b, iCoverTrigger 3: everything of mode 1 plus entering without a press (Core/AutoCover)
		float autoCoverTime{ 0.30f };       // fAutoCoverTime: the "into cover" condition must hold this long
		float autoCoverRearm{ 0.30f };      // fAutoCoverRearm: after an exit it must be false this long before the next automatic entry
		float autoCoverAngleDeg{ 35.0f };   // fAutoCoverAngleDeg: the input at most this far off the direction to the wall
		float autoCoverDistance{ 0.60f };   // fAutoCoverDistance: the stand point at most this far away
		float autoCoverTallEdge{ 1.2f };    // fAutoCoverTallEdge: tall cover needs an open end this near (0 = tall cover never by itself); iAutoCoverTall 1
		int   autoCoverTall{ 2 };           // iAutoCoverTall: 0 tall cover never by itself, 1 only with an open end within fAutoCoverTallEdge, 2 anywhere when the weapon is drawn
		float autoCoverTimeTall{ 0.45f };   // fAutoCoverTimeTall: no longer read since card M4b (kept so that old INIs load)
		float autoCoverTimeDrawn{ 0.05f };  // fAutoCoverTimeDrawn (card M4b): the hold time, low and tall, with the weapon drawn; the weapon away keeps fAutoCoverTime
		bool  coverNotify{ false };         // bCoverNotify: a HUD line on entering and leaving cover (test aid)
		// T5.2: the cover poses (the NPC graph and the engine's stance function, Engine/CoverPose)
		bool  coverPoses{ false };          // bCoverPoses: 0 = off (repo default)
		float poseHolsterAt{ 0.0f };        // fPoseHolsterAt: seconds after the own archetype was asked for at which the weapon is holstered (the NPC graph does not go by the archetype alone: REFERENCE 22.7)
		float poseDrawAt{ 0.6f };           // fPoseDrawAt: ... and drawn again
		float poseConfirmAt{ 1.2f };        // fPoseConfirmAt: ... and the camera hold taken off; 0.15 s later the camera boom node says whether the own graph is back
		int   poseArchetypeMode{ 1 };       // iPoseArchetypeMode: 0 = stage A (the keyword stays away for the whole pose, the weapon cycle brings the own graph back), 1 = the pending flag is cleared during the pose, 2 = the keyword is back after the stance call and the flag cleared
		int   poseDiagMask{ 0 };            // iPoseDiagMask (stage A3 bisect, rig only): 1 = the aim is not given to Phase 3 while a pose is on, 2 = no position writes, 4 = the movement filter off, 8 = no crouch under the pose (low cover), 16 = the weapon-drawn requirement is skipped
		bool  poseDrawWeapon{ true };      // bPoseDrawWeapon: when the weapon is sheathed at the start of a pose, draw it first (1 = default; 0 = no pose then: the first aim press with a sheathed weapon makes the engine draw it and drop the cover)
		float poseEntryBlend{ 0.0f };       // fPoseEntryBlend: from the pose's begin the camera position (the pose function's output, relative to the player) is blended from its value at the begin to what the engine gives over this many seconds (smoothstep): the NPC graph's own pivot and boom values pull the camera in to 0.58 m from the head 0.4 to 0.9 s after the begin (REFERENCE 22.18); 0 = off
		bool  poseHoldWorld{ false };        // bPoseHoldWorld: the hold (and the cover camera) also write the boom and pivot nodes' WORLD translate (parent's world * the local value) after the writes of the local one: the camera follows the pivot's world translate, which the engine computes before the hook (REFERENCE 22.18)
		float poseExitEase{ 0.4f };        // fPoseExitEase: with bPoseExitHold 1 the hold is not dropped at the end of the exit but eased from the held values to what the own graph gives over this many seconds (0 = at once)
		int   poseCornerHold{ 0 };          // iPoseCornerHold (card M4b, a spike): 1 = in the tall pose with the aim held the actor's heading (angle z) is written to the cover's every tick, so the body does not swing round the 0.8 m arm of the side aim
		int   poseReverse{ 1 };             // iPoseReverse (card M4b, REFERENCE 23.12): 0 off, 1 tall cover only (default), 2 tall and low: the input along the wall goes against the pose's side for fPoseReverseHold seconds: the movement vector is written as zero, the Enter event of the needed side is sent and the input goes through again when the side has turned
	float poseReverseHold{ 0.10f };     // fPoseReverseHold
	float poseReverseRest{ 2.50f };     // fPoseReverseRest: at most this long the input waits for the side to turn
	bool  tallAutoTurn{ true };          // bTallAutoTurn (card M4b): idle in the tall pose within range of an open end that is on the other side, the actor turns to it by itself
	float tallAutoTurnStill{ 0.30f };   // fTallAutoTurnStill: after the movement input has been still this long
	float tallAutoTurnMargin{ 0.25f };  // fTallAutoTurnMargin: with both ends in range the other end must be nearer by this much
	float poseCornerEdge{ 0.50f };      // fPoseCornerEdge: at the start of the corner aim the actor slides along the wall to this distance from the open end (the body then ends 0.31 m beyond the end); 0 = no slide
	bool  poseCornerShoulder{ true };   // bPoseCornerShoulder: while the corner aim is on the camera is on the open side's shoulder; the player's own choice comes back after it
	float poseCornerCone{ 55.0f };      // fPoseCornerCone (the same spike): degrees either side of the held heading the view may turn while the aim is held (the weapon follows the crosshair within about 60; 0 = no limit)
	bool  poseCornerCamera{ true };     // bPoseCornerCamera (the same spike): the camera's pivot goes with the body (COM bone minus Root bone) while the heading is held
		int   poseExitRemedy{ 1 };          // iPoseExitRemedy (card M4b): what a leave without the exit clip does when the graph is frozen 0.27 s later (0 nothing, 1 ExitCover, 2 CoverEnd again, 3 the engine's exit function, 4 the own archetype again, 5 the weapon cycle at once)
		int   poseExitVariant{ 0 };         // iPoseExitVariant (card M4b, a diagnostic): events sent with CoverEnd on the leave (0 CoverEnd only, 1 CoverEnd then ExitCover, 2 ExitCover then CoverEnd, 3 the engine's exit function then CoverEnd, 4 CoverEnd twice, 5 CoverEnd, ExitCover, CoverEnd)
		int   poseExitClip{ 1 };            // iPoseExitClip: 0 = the engine's exit function (the exit clip, a quarter turn); 1 = no exit clip: CoverEnd is sent to the graph, the stance field is written 0, the own archetype is asked for in the same tick (REFERENCE 22.19)
		bool  poseStickLog{ true };         // bPoseStickLog: while Posed, one line at 5 Hz with the raw stick, what the game was given and the graph's walk variables (the legs at the wall, card M3h)
		bool  poseExitHold{ true };        // bPoseExitHold: the camera hold stays on through the exit clip (it was released at the exit function, the camera then showed the NPC graph's boom for 0.5 s: the swing); off when the own graph is confirmed. The graph's own boom value is read in the hook before the hold overwrites it
		int   poseExitTurn{ 0 };            // iPoseExitTurn: 0 = as before; 1 = the own archetype is asked for in the same tick as the exit function; 2 = the body's angle z is written to the view yaw before the exit function; 3 = the movement input (keys and stick) is written as zero for the game from the exit function until the own graph is back, so the exit clip does not turn the actor toward the input first
		bool  poseZeroInput{ true };        // bPoseZeroInput: in the pose a movement input whose part along the wall is below fCoverAlongMin (or that points into the wall) is written as zero at the controls hook, so the walk clip does not step on the spot (card M3g play: the legs stepped at the wall)
		bool  poseTallAim{ true };          // bPoseTallAim: at tall cover the aim sends SightedStart / SightedEnd (and turns the actor first) under the open-end rule (T5.2 tall pose)
		bool  poseTallBlindFire{ true };    // bPoseTallBlindFire: blind fire at tall cover (BlindFireStart) under the same rule
		int   poseTallAimSwallow{ 0 };      // iPoseTallAimSwallow: 1 = the real aim press is held back from the game while the plugin sends its own aim events (an experiment)
		bool  poseTall{ true };            // bPoseTall: 0 = tall cover never takes the pose and stays Phase 3 Basic (card M3a); 1 = the pose at tall cover too (T5.2, unfinished: the engine's aim does nothing there)
		bool  poseReload{ false };          // bPoseReload: the reload control in the pose ends the pose first, the reload plays on the own graph (it has its sound there), the pose comes back after fPoseReloadTime (T5.7)
		int   poseWeaponSwap{ 0 };          // iPoseWeaponSwap: 0 = the favourites are switched off by the engine's input-enable layer while the pose is on (T5.9, REFERENCE 22.14); 1 = a quick-key press ends the pose first (M3b, T5.6); 3 = the play candidate (23.21): as 2, and after an equip in the pose the game's own weapon-draw work is called (the holster hold stays as it is); 2 = RIG ONLY (spike W1, 23.18): the favourites are not locked, the quick-key events pass untouched and the plugin does nothing to the pose because of them or because the weapon state changes
		bool  poseFreePitch{ true };        // bPoseFreePitch: while the pose is on the view pitches with the look input (T5.8, proved on the rig: REFERENCE 22.15)
		int   poseFreePitchMode{ 1 };       // iPoseFreePitchMode: 1 = the pitch clamps are written every frame while aiming in the pose, 2 = once after the stance call
		float poseReloadTime{ 3.0f };       // fPoseReloadTime: no pose for this long after a reload began
		float poseHolsterHold{ 0.30f };     // fPoseHolsterHold: the ready/holster control held this long ends the pose (T5.6)
		bool  poseBlindFire{ true };        // bPoseBlindFire: in the pose the fire control without the aim plays blind fire (T5.4)
		float blindFireIntoLow{ 0.80f };    // fBlindFireIntoLow: the Into clip of BlindFireOverStart (low cover, stance 3), seconds
		float blindFireIntoTall{ 0.90f };   // fBlindFireIntoTall: BlindFireStart (tall cover, stance 5)
		float blindFireLinger{ 0.80f };     // fBlindFireLinger: the mode stays this long after the release
		float blindFireOutLow{ 1.17f };     // fBlindFireOutLow: the Out clip, an aim press during blind fire waits for it
		float blindFireOutTall{ 1.07f };    // fBlindFireOutTall
		float poseSwitchWait{ 0.0f };       // fPoseSwitchWait: the least wait between clearing the archetype and the stance call, seconds (the call also waits for the NPC graph to show)
		// T5.3a: the cover camera for Basic (the camera pulls back and up while the machine is InCover or Peeking)
		bool  coverCamera{ false };         // bCoverCamera: 0 = off (repo default)
		float coverCamBackLow{ 0.90f };     // fCoverCamBackLow: metres added to the boom's distance in low cover
		float coverCamUpLow{ 0.35f };       // fCoverCamUpLow: metres added to the pivot's height in low cover (R11 of 2026-11-17: with 0.90 back the camera root is 0.82 m back and 0.58 m up)
		float coverCamBackTall{ 0.60f };    // fCoverCamBackTall
		float coverCamUpTall{ 0.10f };      // fCoverCamUpTall
		float coverCamAimShare{ 0.35f };    // fCoverCamAimShare: the offset is multiplied by this while the aim is held
		float coverCamEase{ 0.35f };        // fCoverCamEase: seconds for the offset to ease in or out (a change of the full size)
		float coverStickMin{ 0.4f };        // fCoverStickMin: the movement input must be at least this far pushed for a Sneak press to take cover
		float candidateMaxAge{ 0.35f };     // fCandidateMaxAge: the sensor's answer must be this recent
		float candidateMaxTurnDeg{ 45.0f }; // fCandidateMaxTurnDeg: ... and the input must not have turned more than this since it was found
		float senseRateHz{ 10.0f };         // fSenseRateHz: background Finds per second while pushing toward something
		float stanceCheckTime{ 0.3f };      // fStanceCheckTime: the stance is checked this long after a press that should change it
		float enterMaxDistance{ 3.0f };     // fEnterMaxDistance: a candidate whose stand point is farther than this from the player is not taken
		float pathBlockedError{ 0.25f };    // fPathBlockedError: during the enter move, the player this far behind where we put them 4 steps in a row = blocked
		Core::StateMachineSettings machineCfg{};  // fEnterTime, fEnterMaxTime, fExitTime, fExitHoldTime, fExitInputThreshold, fMaxDeviation, iLostLeaveCount
		float stickExpireTime{ 0.25f };     // fStickExpireTime: no Move stick event for this long = the stick is centred (a stick at rest sends nothing)
		float coverMoveScale{ 1.0f };       // fCoverMoveScale: the kept (along the wall) part of a stick is multiplied by this (1.0: half a stick is the game walk)
		float holdTolerance{ 0.08f };       // fHoldTolerance: drift off the cover line beyond this is corrected with SetPosition
		float coverMaxInput{ 0.5f };        // fCoverMaxInput: the most movement input (0..1) along the wall in cover; the game input is a full 1.0 for a key
		float edgeBrakeDistance{ 0.15f };    // fEdgeBrakeDistance: toward an end of the cover the input falls linearly to 0 over this distance before the stop point
		float coverMaxInputLow{ 1.0f };      // fCoverMaxInputLow: the same cap while the cover is LOW and the player is crouched (planning session 2026-11-17, batch 2)
		float edgeBrakeDistanceLow{ 0.15f }; // fEdgeBrakeDistanceLow: the same brake distance for low cover and a crouched player
		float edgeStopDistance{ 0.35f };     // fEdgeStopDistance: the input is 0 this far before the stop point (the player coasts the rest; T3.3e)
		float edgeStopDistanceLow{ 0.50f };  // fEdgeStopDistanceLow: the same for low cover and a crouched player (the crouched run coasts 0.27 m)
		float coverAlongMin{ 0.30f };        // fCoverAlongMin: an along-the-wall input part below this is written as exactly (0, 0) (T3.3f)
		float enterSettleTime{ 0.0f };       // fEnterSettleTime: after the stand point is reached, for this long and while the along part of the input is below fCoverAlongMin, a player more than 0.03 m from where it was reached is brought back (0: off)
		float peekSpeed{ 4.0f };             // fPeekSpeed: the eased peek move takes at least distance / this (m/s)
		float holdMaxStep{ 0.04f };         // fHoldMaxStep: the most a correction moves the player in one logic tick (metres)
		float shoveGrace{ 0.60f };          // fShoveGrace (23.18): a move in the first seconds after the plugin's own stance call (the cover entry clip sets the actor's position) is not a shove; 0 = off
	float shoveDistance{ 0.15f };       // fShoveDistance: moved more than this in one logic tick by something other than our own write: leave cover at once
		bool  peek{ true };                 // bPeek: side peek (kill switch)
		bool  peekOver{ true };             // bPeekOver: peek over low cover by standing up (needs the made-up Sneak press; kill switch)
		float peekTime{ 0.15f };            // fPeekTime: the eased move out to the peek spot and back
		float sidePeekDistance{ 0.50f };    // fSidePeekDistance: the peek spot lies this far past the open end, on the wall line
		float edgePeekRange{ 0.95f };       // fEdgePeekRange: aim within this distance of an Open end starts a side peek (else, for low cover, a peek over)
		bool  coverFilterInput{ true };     // bCoverFilterInput: the cover logic may zero the movement input while entering (kill switch for the input side)
		bool  coverMove{ true };            // bCoverMove: the cover logic may move the player with SetPosition (kill switch for the engine side)

		// [PlayerControl] S5
		bool  playerControl{ true };

		// [AnimRecon] S6
		bool  animRecon{ true };
		int   audioProbe{ 0 };       // [Audio] iAudioProbe (spike 23.13): 0 off (default), 1 the five Wwise post calls of the exe are hooked and logged while the rig arms them, 2 logged always (rate limited)
		bool  turnLock{ true };        // [Cover] bTurnLock (23.16): no automatic turn after a turn the player asked for (until 0.5 m walked, aim, fire, leave), the reversal rule's hold counts as input; kill switch
	int   equipFieldProbe{ 0 };    // [Cover] iEquipFieldProbe (23.25, rig only): 1 = every change of the equip fields of the process data is logged (EQUIPFIELDS)
	int   poseWaitByte{ 0 };       // [Cover] iPoseWaitByte (23.32): 1 = the first stance call waits until the archetype-pending byte (middleHigh + 0x585) reads 0, 2 = until the equip byte (+ 0x586) reads 0 too (23.35), at most fPoseWaitByteMax seconds; a repeat call only with the byte clear; 0 (default, the play state) = the call is made after fPoseSwitchWait as before
	int   poseNoRepeat{ 0 };       // [Cover] iPoseNoRepeat (23.36): 1 = after the first stance call no repeat call is made; when 0.5 s later the cover state is not there the session ends as a failed one does (the next begins by itself if the player still pushes into the wall); 0 (default) = up to three calls
	float poseWaitByteMax{ 1.5f }; // [Cover] fPoseWaitByteMax (23.32): the longest the byte is waited for, seconds
	int   poseEntryProbe{ 0 };     // [Cover] iPoseEntryProbe (23.32, rig only): 1 = every frame from the pose's begin to the pose or the leave the bytes at middleHigh + 0x585 and + 0x586, bIsExitingCover, the boom node's y and the weapon bits are logged (POSE PROBE)
	int   poseEquipPending{ 0 };   // [Cover] iPoseEquipPending (23.25): 1 = while the pose is on a byte at middleHigh + 0x586 that reads 1 is written 0
	int   graphEventProbe{ 0 };    // [Cover] iGraphEventProbe (23.23, rig only): 2 = as 1 and the call stack of every Equip, InitializeGunGraph and InstantInitializeGunGraph that is not the plugin's own (23.24); 1 = hook the player's graph holder and log the events around every weapon key change
	int   poseEquipBlock{ 0 };     // [Cover] iPoseEquipBlock (23.23): 1 = while the pose is Posed the events of sPoseEquipBlockEvents are not passed to the player's graph
	std::string equipBlockEvents;  // [Cover] sPoseEquipBlockEvents: comma separated event names
	int   poseWeaponDraw{ 0 };     // [Cover] iPoseWeaponDraw (spike W2, rig only): 1 = after the weapon in the hands changed in the pose the game's own weapon-draw work is called once with the player; 0 (default, the play state) = nothing
	int   scopeSource{ 3 };        // [Cover] iScopeSource (23.22): how a weapon is known to have a scope for iPoseScopeAim 2: bit 1 the graph variable bHasScope (cached per weapon outside the pose), bit 2 learning (the camera left third person while the aim control was held)
	int   poseScopeAim{ 0 };       // [Cover] iPoseScopeAim (23.21, 23.22): 2 = the aim is locked in the pose while the weapon in the hands has a scope (the aim control does not reach the game, no aim of the plugin's own); 1 = while the pose is Posed and the aim control is held, the camera leaving third person (a scope view) does not end the cover; if the aim control is up and the camera is still not third person after 1.0 s, the guard applies again
	int   poseDrawReevaluate{ 0 }; // [Cover] iPoseDrawReevaluate (23.21, rig only): 0.1 s after the draw call the player's graph gets the event ReevaluateGraphState once
	float poseWeaponDrawDelay{ 0.10f };  // [Cover] fPoseWeaponDrawDelay: seconds after the key change before the call (the Weapon node must have lost its model; give up after 1.0 s)
	int   poseFootIKLow{ 0 };     // [Cover] iPoseFootIKLow (23.16): the same for the low pose
	int   poseFootIK{ 0 };        // [Cover] iPoseFootIK (23.16): 0 the game's foot IK is left alone (default), 1 it is off while the pose stands on the mirrored side (iSyncRightLeft 1), 2 off for the whole tall pose; put back on every way out
		int   poseReloadSoundFile{ 1 };  // [Audio] iPoseReloadSoundFile: 1 (default; the user said yes, 2026-11-19) the learned sequences are kept in CoverSF.reloadsound.txt beside the INI (read at start, written when learned; a weapon key of 0 is never written or read); 0 memory only, no file
		int   poseReloadSound{ 0 };  // [Audio] iPoseReloadSound (spike 23.13): 0 off (default); the cure, when built
		bool  stanceSpike{ false };  // [Anim] bStanceSpike: the p5 script may call the engine's stance function on the player (kill switch, default off)
		bool  rigHold{ false };      // [Anim] bRigHold: the camera hook may write stored local translates into the camera's boom and pivot nodes (T5.0b spike, default off)
		int   p5Mode{ 1 };           // [Anim] iP5Mode: 1 stages 1 and 2 (launch A), 2 stages 1 to 5 (B), 3 the tall wall of Save29, stages 3 to 5 with stance 5 (C)
	};

	// The folder that holds the DLL and its INI.
	[[nodiscard]] std::filesystem::path Directory();

	// The DLL's own name without ".dll" ("CoverSF" or "TacticalCover"): the INI, the log and the learned file are named after it (Core/PluginNames).
	[[nodiscard]] const std::string& BaseName();

	// The name a player sees in a HUD line: "Tactical Cover" in the release, "CoverSF" in the development build.
	[[nodiscard]] const char* DisplayName() noexcept;

	// True in the release build (COVER_RELEASE): its defaults are m4b13's, the rig and debug keys are refused.
	[[nodiscard]] constexpr bool IsRelease() noexcept
	{
#ifdef COVER_RELEASE
		return true;
#else
		return false;
#endif
	}

	// Reads the INI once at load and logs what was read.
	void Load();

	// T3.5: re-reads the INI while the game runs (the debug key [General] iReloadKey, Numpad 9). Tick thread only.
	void Reload();

	// For the rig scripts only: the live settings, writable. A reload puts the INI values back.
	[[nodiscard]] Settings& Mutable() noexcept;

	[[nodiscard]] const Settings& Get() noexcept;
}
