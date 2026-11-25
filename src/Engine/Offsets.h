#pragma once

// Everything version-specific that CoverSF relies on: addresses, vtable indices, struct offsets and
// protocol constants. Each entry names its source and the runtime it was verified on (PLAN.md rule 2).
// Re-verify every entry after a game, SFSE or CommonLibSF update.
//
// Phase 0 uses no raw engine addresses. IDs and struct layouts (PlayerCharacter singleton,
// TESObjectREFR::parentCell, OBJ_REFR::location) come from CommonLibSF at the commit recorded in
// docs/REFERENCE.md. Test card M0 (2026-11-16, runtime 1.16.244) exercised them: the singleton
// resolved and the position values were plausible and changed with the cell.
//
// Phase 1 S1 (Engine/Frame) reads more of CommonLibSF's layouts. Each is "from CommonLibSF d7a15706,
// resolved in the 1.16.244 Address Library on 2026-11-16 (tools/addrlib.py); correct on the running game
// only once the test run says so" (the S1 log lines are the evidence, see docs/REFERENCE.md section 12):
//   RE::Main singleton                ID 883591, offset 0x448 isGameMenuPaused
//   RE::UI singleton                  ID 937580, offset 0x4FA menusVisible
//   RE::PlayerCamera singleton        ID 937788, cameraStates[] at 0x188 (index = RE::CameraStates), currentState at 0x10
//   Actor::actorState1 / actorState2  ActorState at Actor+0xE8, raw flags, meaning unknown (logged raw)
//   IAnimationGraphManagerHolder      virtuals 0x12 / 0x13 / 0x14 (GetGraphVariableImplFloat / Int / Bool)

namespace Engine
{
	// The one game runtime this build supports. On any other runtime the plugin logs one error line
	// and installs nothing (PLAN.md rule 4).
	inline constexpr REL::Version kPinnedRuntime = SFSE::RUNTIME_SF_1_16_244;

	// The commonlibsf.plugin rule writes SFSE::RUNTIME_LATEST into the plugin's compatibility list, and
	// SFSE refuses to load a plugin on any runtime that is not in that list. If updating the CommonLibSF
	// submodule moves RUNTIME_LATEST, this build must fail until the pin is reviewed on purpose, instead
	// of producing a DLL that no longer loads on the pinned runtime.
	static_assert(SFSE::RUNTIME_LATEST == kPinnedRuntime,
		"CommonLibSF's RUNTIME_LATEST no longer matches kPinnedRuntime: review the runtime pin before building");

	// ---- S2: PlayerCamera's input receiver -------------------------------------------------------------
	// PlayerCamera inherits BSInputEventReceiver at +0x48 (CommonLibSF include/RE/P/PlayerCamera.h), whose
	// vtable has the destructor at slot 0 and PerformInputProcessing(const InputEvent*) at slot 1.
	// Which of the ten PlayerCamera vtable IDs in CommonLibSF (RE::VTABLE::PlayerCamera) belongs to that
	// sub-object is NOT given by CommonLibSF. Evidence for 459729, gathered 2026-11-16:
	//   - NativeAnimationFrameworkSF Plugin/src/Tasks/Input.cpp hooks vtable ID 459729 slot 1 on an earlier runtime.
	//   - tools/addrlib.py on 1.16.244: ID 459729 is RVA 0x4D04FB8 in .rdata, a two-entry vtable
	//     [0x1E942C8 (destructor thunk), 0x1E8BC00]; the other nine candidates are the primary vtable
	//     (five entries) and event-sink vtables whose slot 1 sits in a different code cluster (0x1E8C4F0..0x1E8C6D0).
	//   - The hook installer compares the live vptr at PlayerCamera+0x48 with this ID's address and refuses to
	//     patch on a mismatch, logging which of the ten IDs does match. Runtime proof: the S2 test run.
	inline constexpr REL::ID        kPlayerCameraInputReceiverVtable{ 459729 };
	inline constexpr std::ptrdiff_t kPlayerCameraInputReceiverOffset = 0x48;
	inline constexpr std::size_t    kPerformInputProcessingSlot = 1;
	inline constexpr std::uintptr_t kPerformInputProcessingRva = 0x1E8BC00;  // slot 1 of that vtable as read from Starfield.exe on disk (1.16.244)

	// W5 (REFERENCE 23.23): the player's animation graph holder. PlayerCharacter has IAnimationGraphManagerHolder as a base at +0x60 (the lib's class) and `python -I tools/rtti_bases.py
	// IAnimationGraphManagerHolder --slots 3` (2026-11-19, 1.16.244) says its vtable is Address Library ID 452463, RVA 0x4CBC2E8; `python -I tools/addrlib.py 452463 --vtable 3`: slot 1 (the lib's
	// NotifyAnimationGraphImpl, the function AnimRecon::SendEvent calls) is RVA 0xF09F60. The installer compares the live vptr at PlayerCharacter+0x60 with that vtable and the slot with that RVA
	// and installs nothing on a difference.
	inline constexpr REL::ID        kPlayerGraphHolderVtable{ 452463 };
	inline constexpr std::ptrdiff_t kPlayerGraphHolderOffset = 0x60;
	inline constexpr std::size_t    kNotifyAnimationGraphSlot = 1;
	inline constexpr std::uintptr_t kNotifyAnimationGraphRva = 0xF09F60;

	// Input event facts from test run 1 (2026-11-16, 1.16.244, CoverSF.log, copy in the session notes):
	//   - Keyboard idCode = Windows virtual-key code (W 0x57, Numpad0..5 0x60..0x65, NumLock 0x90, '0' 0x30).
	//   - Gamepad button idCode = XInput button mask (0x40 left-stick click = Sprint, 0x2000 B = Sneak, 0x4000 X =
	//     WeaponReadyReload, 0x8000 Y = Jump, 0x10 Start = DataMenu); triggers are 0x09 (left, SecondaryAttack = aim)
	//     and 0x0A (right, PrimaryAttack = fire) with analogue `value`.
	//   - Thumbstick events derive from IDEvent (user event string at +0x28, idCode at +0x30); two floats at +0x38 and
	//     +0x3C are x and y (idCode 12 = the look stick was dumped; values in -1..1 with a small dead-zone noise).
	//     The "Move" stick's idCode and that x is right / y is forward are confirmed by the next run.
	//   - MouseMove events: two ints at +0x38 / +0x3C (dx, dy), idCode 0xA00.
	inline constexpr std::ptrdiff_t kThumbstickX = 0x38;
	// Release 1.0.0, the shoulder swap on LT + A (Core/SwapChord, REFERENCE 24). Both are idCodes of gamepad ButtonEvents as the input hooks see them, not engine
	// addresses. The left trigger: 0x09 with an analogue value, user event SecondaryAttack (test run 1, REFERENCE 12.2; every play log since). The A button:
	// 0x1000 = XINPUT_GAMEPAD_A, the same mask family as B 0x2000, X 0x4000, Y 0x8000 above; in the user's plays of 2026-11-19 (buildeal4_play_CoverSF.log,
	// 232 events) and 2026-11-21 (buildeal5_play_CoverSF.log) every A event carries 0x1000 with one user event per press: 'Activate' in the world,
	// 'QCAButton' with a container prompt, 'Accept' in a menu, 'SelectTarget' in the ship. Read on 1.16.244.
	inline constexpr std::int32_t   kPadLeftTriggerId = 0x09;
	inline constexpr std::int32_t   kPadButtonAId = 0x1000;
	inline constexpr std::ptrdiff_t kThumbstickY = 0x3C;

	// ---- S2b: PlayerControls::Manager's input receiver (where the rewrites go) -------------------------------
	// The PlayerCamera receiver is called AFTER PlayerControls::Manager::PerformInputProcessing has walked the event
	// list and dispatched every event to the jump and movement handlers, so rewriting there cannot reach them (S2b
	// runs on 2026-11-16: all four Jump swallow methods and movement zeroing had no effect there).
	// Evidence for the hook point, 2026-11-16, runtime 1.16.244:
	//   - planning session: `python -I tools/rtti_bases.py BSInputEventReceiver --slots 2` (reads the compiler's RTTI
	//     records, so the vtable-to-base-class mapping is evidence): PlayerControls::Manager has its BSInputEventReceiver
	//     part at offset 0, vtable Address Library ID 433847, slot 0 = destructor RVA 0x12C4DA0, slot 1 =
	//     PerformInputProcessing RVA 0x12C5730 (Address Library ID 82442).
	//   - this session, `python -I tools/addrlib.py 433847 82442 --vtable 3`: ID 433847 is RVA 0x4C43F70 in .rdata with
	//     [0x12C4DA0, 0x12C5730]; ID 82442 is RVA 0x12C5730 in .text.
	//   - planning session, executable read: that function loops over the events (`next` at +0x18), dispatching each at
	//     RVA 0x12C5C50, and only afterwards registers PlayerCamera's receiver with the input manager.
	// No object instance is needed: the installer reads slot 1 and refuses to patch unless it is RVA 0x12C5730.
	inline constexpr REL::ID        kControlsManagerReceiverVtable{ 433847 };
	inline constexpr std::uintptr_t kControlsManagerPerformInputRva = 0x12C5730;

	// ---- T3.3b: the movement vector inside PlayerControls::Manager ----------------------------------------------------------------
	// Planning session, 2026-11-17, Starfield.exe 1.16.244 (docs/REFERENCE.md 18.11, read with dumpbin /disasm; confirmed or refuted by the probe of T3.3b, whose
	// log lines are the evidence). PerformInputProcessing (ID 82442, RVA 0x12C5730) zeroes the vector at its start (0x1412C5746 to 0x1412C575E: the zero constant at
	// RVA 0x4AE80F0 copied into [rcx+58h] and [rcx+60h]), the handlers write it while the events are walked (MovementHandler::OnButtonEvent RVA 0x12BCF40 writes a
	// constant +-1.0 into x or y for a key whose value is not 0, OnThumbstickEvent RVA 0x12BD080 copies the stick floats), and it is copied to +0x7C at the end
	// (0x1412C596D to 0x1412C5985). The movement system reads +0x58 and +0x5C in IMovementPlayerControls slot 1 (RVA 0x12C5E60, 0x1412C6060 to 0x1412C6087).
	// The manager is the `this` the rewrite hook receives (its BSInputEventReceiver part is at +0x0).
	inline constexpr std::ptrdiff_t kManagerMoveX = 0x58;      // float, strafe, right positive
	inline constexpr std::ptrdiff_t kManagerMoveY = 0x5C;      // float, forward positive
	inline constexpr std::ptrdiff_t kManagerMoveZ = 0x60;      // float, up and down (zero gravity)
	inline constexpr std::ptrdiff_t kManagerPrevMoveX = 0x7C;  // float, the copy of the last call (the unfiltered game values when our write comes after the copy)
	inline constexpr std::ptrdiff_t kManagerPrevMoveY = 0x80;  // float

	// ---- S4: ThirdPersonState::Update ---------------------------------------------------------------------------
	// Vtable: Address Library ID 459751 = RE::VTABLE::ThirdPersonState[0] in CommonLibSF d7a15706. Evidence that it is the
	// class's own vtable, 2026-11-16, runtime 1.16.244: `python -I tools/rtti_bases.py ThirdPersonState --self --slots 24`
	// reads the compiler's RTTI records: one vtable, sub-object +0x0 (TESCameraState), RVA 0x4D05BC8, ID 459751.
	// Slot 12 = TESCameraState::Update, sources:
	//   - chosti-factor/cinematic-conversation-camera-sf (GPL-3.0) src/Plugin.cpp hooks vtable RVA 0x4C27500 slot 12 as
	//     the dialogue camera's Update, written for 1.16.244. rtti_bases.py says that vtable is DialogueCameraState (ID
	//     431020, RE::VTABLE::DialogueCameraState), derived from ThirdPersonState, so slot 12 of a ThirdPersonState-derived
	//     camera state is Update on this runtime.
	//   - the same slot is overridden by every camera state class (FirstPersonState 0x1E834C0, PlayerCameraTransitionState
	//     0x1E94420, DialogueCameraState 0xF9BEF0, ThirdPersonState 0x1E954A0) and holds a small default (RVA 0xF14490)
	//     in the TESCameraState base vtable (ID 431091), which is what a virtual "Update" looks like; slots 2 to 7 are
	//     shared no-ops.
	//   - Skyrim's TESCameraState puts Update(BSTSmartPointer<TESCameraState>&) at index 3 after the input-receiver base
	//     slots; Starfield's layout differs, so this is not used as evidence.
	//   - Runtime proof: the S4 log lines "S4 hook: first call ... equal=true" and "S4 hook rate" (about once per frame
	//     while the camera is third person, none otherwise).
	// The hook patches the vtable, not an object, so it also runs for any other object using this vtable; the hook
	// compares `this` with PlayerCamera::cameraStates[kThirdPerson] before doing anything. DialogueCameraState has its own
	// vtable, so a conversation camera is not touched.
	inline constexpr REL::ID        kThirdPersonStateVtable{ 459751 };
	inline constexpr std::size_t    kCameraStateUpdateSlot = 12;
	inline constexpr std::uintptr_t kThirdPersonUpdateRva = 0x1E954A0;  // slot 12 as read from Starfield.exe on disk (rtti_bases.py, 1.16.244)
	// PlayerCamera::ForceFirstPerson (ID 113397, RVA 0x1E8C7B0) and ForceThirdPerson (ID 113398, RVA 0x1E8C840): IDs from
	// CommonLibSF d7a15706, both resolve in the 1.16.244 Address Library (tools/addrlib.py, 2026-11-16). Used by the rig
	// only; first runtime use is the S4 test run.

	// S4 scan result (run 12, 2026-11-16, runtime 1.16.244, `S4 scan` log lines): the ThirdPersonState object holds the camera
	// position as a float triple at +0x190 and again at +0x19C (both equal to the root node's translate right after Update).
	// Which one is the position and which a target is NOT known; nothing uses these yet.
	// Slots 13 and 14 of the same vtable are the getters the engine reads the pose through after Update (planning session,
	// disassembly of Starfield.exe 1.16.244, re-checked by this session with tools/addrlib.py --bytes):
	//   slot 13 RVA 0x1E99B20 (ID 113500): c5 f8 10 81 70 01 00 00 / c5 f8 11 02 / c3  = vmovups xmm0,[rcx+0x170]; vmovups [rdx],xmm0; ret
	//                                      -> GetRotation, a 16-byte quaternion at this+0x170
	//   slot 14 RVA 0x1E99B30 (ID 113501): c5 fb 10 81 90 01 00 00 / c5 fb 11 02 / 8b 81 98 01 00 00 / 89 42 08 / c3
	//                                      = copy 12 bytes from this+0x190 to [rdx] -> GetTranslation
	// The hook on slot 14 calls the original and adds the offset to the returned point (this+0x190 itself is never written).
	inline constexpr std::size_t    kCameraStateGetTranslationSlot = 14;
	inline constexpr std::uintptr_t kThirdPersonGetTranslationRva = 0x1E99B30;
	inline constexpr std::ptrdiff_t kThirdPersonStatePosA = 0x190;
	inline constexpr std::ptrdiff_t kThirdPersonStatePosB = 0x19C;

	// ---- S5 / S6: what the g2 run used (2026-11-16, runtime 1.16.244, run 12) ---------------------------------------
	//   Actor::SetPosition(const NiPoint3&, bool)      vfunc 0x137 per CommonLibSF; works, pass a_updateCharController = true
	//   Actor::DrawWeaponMagicHands(bool)              vfunc 0x136 per CommonLibSF; holstered and drew the weapon
	//   IAnimationGraphManagerHolder::NotifyAnimationGraphImpl  vfunc 01 per CommonLibSF; returned true for every event, even
	//                                                  ones the player's graph does not know: the return value says nothing
	//   BGSKeywordForm::AddKeyword / RemoveKeyword     data-only CommonLibSF code on the TESNPC keyword array; worked
	//   AnimArchetypePlayer keyword                    TESFormID 0x0006B508 in Starfield.esm, from tools/esm_subgraphs.py; found by
	//                                                  TESForm::LookupByID, present on the player's TESNPC, not on HumanRace
	// Keyboard and mouse events observed (run 12): Left Ctrl idCode 0xA2 = Sneak, R 0x52 = WeaponReadyReload, `~` 0xC0 = Console,
	// mouse button 1 (right) = SecondaryAttack. Graph variables that resolve on the player: iSyncActorStance, iSyncRelaxedReadyAlert,
	// iSyncWeaponDrawState, iSyncGunDown, iSyncSighted, iIsSighted, iSyncSprintState, iSyncJumpState, iSyncSwimState, iSyncGravity,
	// bIsMantling, bIsInCover, iIsPlayer, iSyncRightLeft, iPartialCover, iIsBlindFiring, Direction, fDesiredHeading; not:
	// iSyncBleedout, bIsDialogueOpen, bIsSneaking, iIsInSneak, iSyncStandingCrouching, iSyncCoverStates, bIsEnteringCover,
	// bIsExitingCover, bIsShuffling.
	// ActorUtils::ChangeAnimArchetype has ID 0 in CommonLibSF (comment: 150497, which resolves to RVA 0x2CDDE70 but is unverified): NOT used.

	// ---- S4: the call site that computes the third-person camera position ---------------------------------------------
	// Planning session, 2026-11-16, Starfield.exe 1.16.244 (disassembly of ThirdPersonState::Update, Address Library ID 113492,
	// RVA 0x1E954A0): at Update + 0x431 (RVA 0x1E958D1) it calls RVA 0x1E9B560 (ID 113514) with rcx = this, rdx = this+0x190
	// (position out), r8 = a rotation matrix (out), then converts the matrix to the quaternion at this+0x170. This session
	// re-read the bytes with tools/addrlib.py: at Update + 0x431 they are E8 8A 5C 00 00, and E8 + rel32 0x5C8A from the end of the
	// call (RVA 0x1E958D6) resolves to RVA 0x1E9B560. A second call to the same function sits at RVA 0x1E95C41 (another branch,
	// temporary output): not hooked. The installer checks the five bytes at runtime and refuses on any difference.
	inline constexpr REL::ID        kThirdPersonUpdateId{ 113492 };
	inline constexpr std::ptrdiff_t kThirdPersonPoseCallOffset = 0x431;
	inline constexpr std::uintptr_t kThirdPersonPoseCallTargetRva = 0x1E9B560;

	// ---- spike 23.13: the engine's Wwise post calls (the reload sound in the cover pose) -------------------------------------
	// Sources: CommonLibSF's IDs.h (`AkSoundEngine`: "Statically-linked Wwise 2021.1 AK::SoundEngine entry points"): ID 150391 =
	// RVA 0x2CD4E10 and ID 150393 = RVA 0x2CD5050 on 1.16.244 (tools/addrlib.py, this session). The prologues fit the Wwise SDK's
	// AK::SoundEngine::PostEvent overloads (ecx/rcx = event id / name, rdx = game object, r8d = flags, r9 = callback): 150391
	// copies ecx to r15d and rdx to r14, 150393 copies rcx to rbx and rdx to r12. The whole executable calls them from five places
	// (tools/xrefs.py id:150391 / id:150393 --calls, 2026-11-19): in the game's wrapper ID 73418 (RVA 0xF1BF20) at +0x236 and +0x2CE
	// (by name) and +0x2FD (by id), and in ID 73419 (RVA 0xF1C2E0) at +0x1A4 (by name) and +0x1CB (by id). The installer checks the
	// five bytes (E8 + rel32) of every site against the two targets and refuses on any difference.
	inline constexpr std::uintptr_t kAkPostEventRva = 0x2CD4E10;
	inline constexpr std::uintptr_t kAkPostEventByNameRva = 0x2CD5050;
	inline constexpr REL::ID        kAudioWrapperAId{ 73418 };
	inline constexpr REL::ID        kAudioWrapperBId{ 73419 };

	// ---- spike W2 (REFERENCE 23.19, 23.20): the game's own weapon-draw work ------------------------------------------------------------
	// Source: the planning session's reading of the executable (Starfield.exe 1.16.244, 2026-11-19; listings build\dis\weapondraw_18D7050.txt, drawhelper_1966090.txt, weapondrawhandler_103947.txt).
	// The handler of the animation mark WeaponDraw (WeaponDrawHandler, vtable ID 453127, slot 1 = ID 103947, RVA 0x1AE5870) does one thing: it calls the function at Address Library ID 100439
	// (RVA 0x18D7050 on 1.16.244) with the actor in rcx. First bytes on disk (tools\addrlib.py, 2026-11-19): 40 55 53 56 57 41 54 41 56 41 57 48 8B EC 48 81 EC 80 00 00 00 48 8B F1. The module
	// compares the first 24 bytes at load and installs nothing on any difference. One argument, the actor; the handler uses no result. For the player the handler skips the call when the dword at
	// +0x10EC of the object that the global at RVA 0x5F43230 points to is not zero and the weapon state is not 2: that dword is only read (logged) by the spike.
	inline constexpr REL::ID                       kWeaponDrawWorkId{ 100439 };
	inline constexpr std::array<std::uint8_t, 24> kWeaponDrawWorkPrologue{ 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xF1 };
	inline constexpr std::uintptr_t                kPlayerObjectGlobalRva = 0x5F43230;
	inline constexpr std::ptrdiff_t                kMhDrawFlagsByte = 0x584;  // middleHigh + 0x584 (bit 1 and bit 2 are cleared by the work), + 0x585 is the archetype pending byte (kMhArchetypePending)

	// ---- W7 (REFERENCE 23.25): the pending weapon animation --------------------------------------------------------------------
	// Source: the planning session's listings of Starfield.exe 1.16.244 (2026-11-19): build\dis\equipsend_101325.txt (Address Library ID 101325, RVA 0x1960230: reads rax = [actor + 0x228] (the
	// lib's currentProcess), middleHigh = [rax + 8], high = [rax + 0x10]; it reads the byte at middleHigh + 0x586 (1 calls ID 101326, the sender of the Equip event; 2 calls ID 101327; after a
	// success it writes 0 there), passes middleHigh + 0x3B0, + 0x2D0, + 0x3D0 and + 0x2F0 to ID 102324, and ID 101326 clears the byte at high + 0x57E, tests the byte at high + 0x566 and sets the
	// byte at high + 0x583 to 1), equipgate_104383.txt, equipqueue_102324.txt, equipjob_100690.txt. The offsets are only read (and the one byte at middleHigh + 0x586 written) by Engine/EquipPending,
	// which logs what it reads before it writes anything; the runtime proof is the W7 run (docs/REFERENCE.md 23.25).
	inline constexpr std::ptrdiff_t kMhEquipPendingByte = 0x586;
	inline constexpr std::ptrdiff_t kMhEquipField2D0 = 0x2D0;
	inline constexpr std::ptrdiff_t kMhEquipField2F0 = 0x2F0;
	inline constexpr std::ptrdiff_t kMhEquipField3B0 = 0x3B0;
	inline constexpr std::ptrdiff_t kMhEquipField3D0 = 0x3D0;
	inline constexpr std::ptrdiff_t kHighEquipByte566 = 0x566;
	inline constexpr std::ptrdiff_t kHighEquipByte57E = 0x57E;
	inline constexpr std::ptrdiff_t kHighEquipByte583 = 0x583;

	// ---- 23.16: the foot IK settings ---------------------------------------------------------------------------------------------
	// No address is used: bHumanoidFootIKEnable:Animation and bEnableFootIK:Animation are names that Starfield.exe registers (tools\exe_strings.py,
	// planning session 2026-11-19) and are reached with the library's typed access RE::GetINISetting(name) (the route CameraSettings uses for the
	// camera names). Engine/FootIK.cpp logs what it finds (found, type, value) before it writes anything and refuses on any other type than bool.
	// ---- spike 23.14: the weapon in the player's hands ---------------------------------------------------------------------------
	// Found by the weapon scan of rig set 56 (docs/REFERENCE.md 23.14, run build\m4d_run2.log, 1.16.244, 2026-11-19): the player's
	// AIProcess (player->currentProcess) holds middleHigh at +0x8 (CommonLibSF); middleHigh + 0x278 holds a pointer to an array whose
	// first entry's first qword is the TESObjectWEAP* of the weapon in the hands. Checked on three weapons: 0x002BF65B (the one the
	// save starts with), 0x002773C8 after number key 2, 0x00016758 after number key 3, each equal to the form id of the ActorItemEquipped
	// event of the same swap. The pointer is only trusted when the game's own TESForm::LookupByID(formID at +0x28) returns it and the
	// form type at +0x2E is WEAP; the first 0x100 bytes of the array are searched for the first such pointer.
	inline constexpr std::ptrdiff_t kMhEquippedItemsData = 0x278;
	inline constexpr std::size_t    kEquippedItemsScanBytes = 0x100;

	// ---- S4b: fields of ThirdPersonState read and written by the pivot shift -----------------------------------------
	// Source for every entry: planning session, disassembly of ThirdPersonState::Update (ID 113492, RVA 0x1E954A0) and the pose
	// function (ID 113514, RVA 0x1E9B560), Starfield.exe 1.16.244, docs/REFERENCE.md section 14.6. Instruction addresses (all
	// RVA + 0x140000000 as `dumpbin` prints them): Update reads the pivot node `mov r15,[rsi+200h]` at 0x141E957F0 and its local
	// translate `[r15+70h]` / `[r15+78h]` at 0x141E9599C / 0x141E959A8, the yaw `[rsi+230h]` at 0x141E95981; the pose function reads
	// the boom node `[rdi+1F8h]` at 0x141E9B92F (translate +70h / +78h at 0x141E9B966 / 0x141E9B971, rotation +40h at 0x141E9BD79),
	// the pivot node `[rdi+200h]` at 0x141E9BCD2 (translate +70h / +74h), the blend `[rdi+264h]` at 0x141E9BBCB and the yaw
	// `[rdi+230h]` at 0x141E9BCEB. NOT yet confirmed by a run: the S4b probe logs all of them (the first runtime use is run 1 of
	// S4b). The node pointers are NiAVObject* (CommonLibSF layout: local at +0x40, so the local translate is at +0x70).
	inline constexpr std::ptrdiff_t kTpsBoomNode = 0x1F8;   // NiAVObject*: local translate = boom (x sideways, y along the view, z up)
	inline constexpr std::ptrdiff_t kTpsPivotNode = 0x200;  // NiAVObject*: local translate = the look-at pivot offset beside the player
	inline constexpr std::ptrdiff_t kTpsFovNode = 0x208;    // NiAVObject*: field of view / zoom transitions
	inline constexpr std::ptrdiff_t kTpsBoneNode = 0x210;   // NiAVObject*: animated camera bone (when set, the camera takes its transform)
	inline constexpr std::ptrdiff_t kTpsYaw = 0x230;        // float, radians
	inline constexpr std::ptrdiff_t kTpsBlend = 0x264;      // float b, 0 to 1 (free-look blend)
	// Collision factor and its timer (second round of S4b). Source: planning session, disassembly of the engine's camera collision
	// routine RVA 0x1E98380 (Address Library ID 113497), docs/REFERENCE.md section 14.8: `vmovss xmm3,[r14+250h]` at 0x141E988B1,
	// `vmulss xmm1,xmm1,[r14+250h]` at 0x141E98DBE, `vmovss xmm4,[r14+254h]` at 0x141E988E0 (r14 = the ThirdPersonState). The engine
	// scales the camera distance and its own shoulder offset by the float at +0x250 (1 = free) and lets it recover while it sees the
	// player move or the camera turn; +0x254 is its timer. Only read here, never written. Runtime check: the S4b probe logs both.
	inline constexpr std::ptrdiff_t kTpsCollisionFactor = 0x250;  // float, 1.0 = camera not pulled in
	inline constexpr std::ptrdiff_t kTpsCollisionTimer = 0x254;   // float
	// The float C of the look-at pivot (P.z = z - b*C): RVA 0x5F55E80, Address Library ID 926178 (tools/addrlib.py 926178 on
	// 1.16.244.0 prints RVA 0x5F55E80, section .data, 2026-11-17).
	inline constexpr REL::ID        kPivotConstId{ 926178 };
	inline constexpr std::uintptr_t kPivotConstRva = 0x5F55E80;

	// ---- S6 route A: ActorUtils::ChangeAnimArchetype(Actor*, BGSKeyword*) -> bool -------------------------------------
	// Found and verified by the planning session in Starfield.exe 1.16.244 (2026-11-16); CommonLibSF has it with the stub ID 0
	// and the unrelated comment number 150497 (RVA 0x2CDDE70, an unrelated transform function: not used).
	//   - `python -I tools/rtti_bases.py ChangeAnimArchetypeHandler --self`: vtable ID 453317 (RVA 0x4CC5518), slot 1 at RVA
	//     0x1AEAEC0 (ID 104048) = ExecuteHandler(Actor&, const BSFixedString&). With a non-empty name it looks the keyword up
	//     by name, checks it is keyword type 7 (kAnimArchetype) and tail-jumps to RVA 0x17976C0 with rcx = actor, rdx = keyword;
	//     with an empty name it clears the type-7 keywords inline and runs the same refresh calls.
	//   - RVA 0x17976C0 = Address Library ID 97226 (tools/addrlib.py 97226): with a non-null keyword the actor already has
	//     (virtual call through actor+0x68) it returns false; otherwise it removes every type-7 keyword of the actor, adds the
	//     new one (call 0xB42710) when non-null, and runs the graph refresh. A null keyword means "clear".
	//   - this session re-checked the prologue bytes: 48 89 5c 24 08 55 56 57 48 83 ec 20 48 8b ea 48 8b d9 48 85 d2 74 5f ...
	//     (rbp = rdx = keyword, rbx = rcx = actor, null test on rdx, then add rcx,0x68).
	// AnimRecon refuses to call it unless the ID resolves to exactly this RVA.
	inline constexpr REL::ID        kChangeAnimArchetype{ 97226 };
	inline constexpr std::uintptr_t kChangeAnimArchetypeRva = 0x17976C0;

	// ---- T5.0: the engine's own stance function and the process fields it writes (planning session, docs/REFERENCE.md section 20, read from the Starfield.exe 1.16.244 listing;
	// none of it was used at runtime before the p5 script, whose probe stage compares the fields with the graph variables before anything is called).
	//   RVA 0x198D880 = Address Library ID 101772: bool f(Actor*, int32 stance, bool side, bool partial, const NiPoint3* location, float heading). A location of (FLT_MAX x3)
	//   means "where the actor stands", a heading of -FLT_MAX "as the actor faces". The p5 script refuses to call it unless the ID resolves to this RVA and these 24 bytes match.
	inline constexpr REL::ID                       kActorChangeStance{ 101772 };
	inline constexpr std::uintptr_t                kActorChangeStanceRva = 0x198D880;
	inline constexpr std::array<std::uint8_t, 24> kActorChangeStanceBytes{ 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
	// fields of `player->currentProcess->middleHigh` (CommonLibSF Actor::currentProcess at 0x228, AIProcess::middleHigh at 0x08); the graph reads these by address (20.1)
	inline constexpr std::ptrdiff_t kMhActorStance = 0x510;    // int, iSyncActorStance
	inline constexpr std::ptrdiff_t kMhRelaxedReady = 0x514;   // int, iSyncRelaxedReadyAlert
	inline constexpr std::ptrdiff_t kMhIsSighted = 0x518;      // int
	inline constexpr std::ptrdiff_t kMhIsBlindFiring = 0x51C;  // int
	inline constexpr std::ptrdiff_t kMhIsGunDown = 0x520;      // int
	inline constexpr std::ptrdiff_t kMhRightLeft = 0x524;      // int, iSyncRightLeft
	inline constexpr std::ptrdiff_t kMhPartialCover = 0x528;   // int
	// T5.8 CORRECTION (planning session, 2026-11-18, listings `build\dis\update.txt`, `pose.txt`, `setpitch_193CDD0.txt`, Starfield.exe 1.16.244): virtual slot 243 (+0x798) of the player is the
	// HEADING (fmod(player[+0xE2C] + player[+0x88], two pi), RVA 0x1A44700), not the pitch (run m3e-4 fitted it against the view's pitch: r -0.6). The view's pitch is the player's own angle x,
	// `player + 0x80`, plus the high process's float at `process + 0x10 -> +0x3F4`: ThirdPersonState::Update (RVA 0x1E9557F to 0x1E95667) adds `g[+0x68] * s1 * s2` into the state's float +0x1E8
	// (g = *(RVA 0x61E23E0), flag byte g + 0xB2 must be set, s1 the float at RVA 0x5F55FA0, s2 the float at RVA 0x5FBF9AC), clamps it against angle x +- the float at RVA 0x4F04654, calls
	// RVA 0x193CDD0(player, -value) (adds the delta to angle x, zero when the camera state's id, the dword at state + 0x50, is 0x11) and zeroes +0x1E8. The pose function reads `[actor + 0x80]`
	// at RVA 0x1E9BA2F and 0x1E9BF88 and writes it at RVA 0x1E9C0EB.
	inline constexpr std::ptrdiff_t kRefrAngleX = 0x80;           // float, the player's angle x
	inline constexpr std::ptrdiff_t kProcessHigh = 0x10;          // AIProcess -> high process
	inline constexpr std::ptrdiff_t kHighPitchOffset = 0x3F4;     // float of the high process added to angle x
	inline constexpr std::uintptr_t kLookDataGlobalRva = 0x61E23E0;  // pointer to the look data g: +0xB2 byte (free look), +0x64 and +0x68 floats (the look delta x and y)
	inline constexpr std::ptrdiff_t kLookDataFlag = 0xB2;
	inline constexpr std::ptrdiff_t kLookDataX = 0x64;
	inline constexpr std::ptrdiff_t kLookDataY = 0x68;
	inline constexpr std::uintptr_t kLookScale1Rva = 0x5F55FA0;      // float s1
	inline constexpr std::uintptr_t kLookScale2Rva = 0x5FBF9AC;      // float s2
	inline constexpr std::uintptr_t kPitchLimitRva = 0x4F04654;      // float, the clamp around angle x
	inline constexpr std::ptrdiff_t kTpsLookAccumX = 0x1E4;          // ThirdPersonState floats the look delta is added into
	inline constexpr std::ptrdiff_t kTpsLookAccumY = 0x1E8;
	// the aim's own clamps (REFERENCE 20.1, read from Starfield.exe 1.16.244 by the planning session, first used at runtime by the pitch probe of card M3c, run m3c-1; floats):
	inline constexpr std::ptrdiff_t kMhDirectAtHeadingVar = 0x52C;
	inline constexpr std::ptrdiff_t kMhDirectAtPitchVar = 0x530;
	inline constexpr std::ptrdiff_t kMhDirectAtHeadingMin = 0x534;
	inline constexpr std::ptrdiff_t kMhDirectAtHeadingMax = 0x538;
	inline constexpr std::ptrdiff_t kMhDirectAtPitchMin = 0x53C;
	inline constexpr std::ptrdiff_t kMhDirectAtPitchMax = 0x540;
	inline constexpr std::ptrdiff_t kMhDesiredLocation = 0x550;  // three floats
	inline constexpr std::ptrdiff_t kMhDesiredHeading = 0x560; // float
	inline constexpr std::ptrdiff_t kMhSightedRequested = 0x568;  // int
	inline constexpr std::ptrdiff_t kMhJumpState = 0x56C;      // int, iSyncJumpState
	inline constexpr std::ptrdiff_t kMhAimActive = 0x587;      // bool
	// A2 (planning session, 2026-11-17, read from the Starfield.exe 1.16.244 listings `build\dis\changearch_97226.txt` and `build\dis\consumer_101324.txt`, scan `build\find_byte_field.py`):
	// one byte, "animation archetype change pending": ChangeAnimArchetype (ID 97226) removes the keywords, adds the new one and only marks the change pending (`mov byte ptr [middleHigh + 0x585], 2`
	// at function + 0x131); 0 = none, 1 = in progress (ID 97227 sets it), 2 = asked for. The consumer (ID 101324, RVA 0x195FE40) runs a default action when it is not 0 and clears it at
	// function + 0x3C9; 13 functions touch it. Checked on 1.16.244 by the rig of stage A2 (the values logged before and after every step of the pose).
	inline constexpr std::ptrdiff_t kMhArchetypePending = 0x585;  // uint8

	// ---- T5.0b: the engine's own way out of a cover stance (planning session, docs/REFERENCE.md 21.1, read from the Starfield.exe 1.16.244 listing `build\dis\exitcover_101773.txt`)
	//   RVA 0x198DC00 = Address Library ID 101773: bool g(Actor*), the function right after the stance function. Returns true at once when the actor has no process or its stance
	//   field is not 2 to 5; otherwise runs the exit action (the form at +0x408 of the object ID 44335 returns), writes 0 into iSyncActorStance and returns the action's answer.
	//   The p5 script refuses to call it unless the ID resolves to this RVA and these 26 bytes match.
	inline constexpr REL::ID                       kActorExitCover{ 101773 };
	inline constexpr std::uintptr_t                kActorExitCoverRva = 0x198DC00;
	inline constexpr std::array<std::uint8_t, 26> kActorExitCoverBytes{ 0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8d, 0x6c, 0x24, 0xb9, 0x48, 0x81, 0xec, 0x90, 0x00, 0x00, 0x00 };
	// the node and flag slots of ThirdPersonState read by the camera rig sample (REFERENCE 14.6, 21.1)
	inline constexpr std::ptrdiff_t kTpsNode218 = 0x218;     // NiAVObject* (sampled, meaning not known)
	inline constexpr std::ptrdiff_t kTpsBoneFlag = 0x291;    // byte, the flag of the animated camera bone at +0x210 (REFERENCE 14.6)

	// ---- S0: quitting after a scripted run ---------------------------------------------------------------
	// RE::Main::quitGame, byte at Main + 0x28 (CommonLibSF include/RE/M/Main.h, static_assert on the offset). The
	// neighbouring field isGameMenuPaused at +0x448 from the same struct was confirmed on 1.16.244 by test run 1
	// (it reads true in menus and false in play), so the layout of Main can be trusted; what setting quitGame does
	// has NOT been tried on this runtime. The rig therefore writes it only when [General] bQuitAfterTest=1
	// (default 0), and records the result of the first use in docs/REFERENCE.md.

	// ---- S3: the borrowed sphere cast ------------------------------------------------------------------
	// Source: chosti-factor/cinematic-conversation-camera-sf (GPL-3.0), src/Plugin.cpp around line 1166, written
	// by its author for 1.16.244 and used by its dialogue camera:
	//   bool cast(void* query, const NiPoint3* from, const NiPoint3* to, float radius, void** outHit, void* null, float 0)
	//   Starfield.exe + 0x1E8DC20; `query` is the pointer stored at dialogue camera state + 0x48; the hit
	//   fraction along the line is the float at query + 0x2B4; the call writes into *to, so pass copies.
	// Address Library: ID 113412 has RVA 0x1E8DC20 exactly (tools/addrlib.py, reverse lookup, 2026-11-16), so the
	// raw address is replaced by the ID (PLAN.md S3 step 4). WorldProbe checks the resolved RVA against
	// kSphereCastRva at first use and refuses to cast on a mismatch.
	// The dialogue camera state is PlayerCamera::cameraStates[RE::CameraStates::kDialogue] (CommonLibSF layout,
	// proved for the running game by the "S1 camera layout" log line).
	// Whether `query` is valid outside a dialogue is exactly what test card G1 decides.
	inline constexpr REL::ID        kSphereCast{ 113412 };
	inline constexpr std::uintptr_t kSphereCastRva = 0x1E8DC20;
	inline constexpr std::ptrdiff_t kCameraStateCollisionQuery = 0x48;
	inline constexpr std::ptrdiff_t kCollisionQueryHitFraction = 0x2B4;

	// SFSE messages. Source: ianpatt/sfse tag v0.2.21 (commit 48535cc4): sfse/PluginAPI.h, PluginManager.cpp
	// and Hooks_Data.cpp; read 2026-11-16. Test card M0 confirmed it on 1.16.244: 0 to 3 arrive, 7 never does.
	//   Sent:     0 PostLoad, 1 PostPostLoad (all plugins loaded); 2 PostDataLoad, 3 PostPostDataLoad (game data loaded).
	//   Not sent: 4 PreSaveGame, 5 PostSaveGame, 6 PreLoadGame, 7 PostLoadGame. PluginAPI.h declares them, but
	//             the hooks that dispatch them are compiled out (Hooks_Serialization.cpp, `#if 0`; sfse_whatsnew.txt
	//             0.2.12: "disabled preliminary serialization API"). CommonLibSF's enum stops at 3 for the same reason.
	// Game loads are therefore detected by Engine/LoadWatch, not by a message.
	namespace SfseMessage
	{
		inline constexpr std::uint32_t kPostLoadGame = 7;  // declared only; never delivered by SFSE 0.2.21
	}
}
