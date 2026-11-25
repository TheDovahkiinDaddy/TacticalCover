#pragma once

// S4: the third-person camera hook.
//
// Replaces slot 12 of the ThirdPersonState vtable (TESCameraState::Update, see Engine/Offsets.h for the evidence),
// calls the original, and afterwards moves the camera root node sideways by a smoothed offset. A zero offset
// touches nothing. [Camera] bCamera=0 keeps the whole spike out of the game.
//
// The offset is "metres to the right of the camera's view direction", positive to the right. The view direction is
// taken from the camera-to-player vector (horizontal), so no knowledge of the camera node's axis layout is needed.

namespace Engine::Frame
{
	struct Snapshot;
}

namespace Engine::Camera
{
	struct Stats
	{
		std::uint64_t calls{ 0 };         // hook calls, any state object
		std::uint64_t callsThird{ 0 };    // calls whose `this` is PlayerCamera::cameraStates[kThirdPerson]
		std::uint64_t callsOther{ 0 };    // calls on any other object (the dialogue camera derives from ThirdPersonState but has its own vtable)
		std::uint64_t appliedCalls{ 0 };  // calls that actually moved the root node
		double        lastThirdTime{ 0.0 };  // Frame::Now() of the latest third-person call
		float         applied{ 0.0f };    // the smoothed offset that the latest call used
		float         allowed{ 0.0f };    // collision limit the tick computed (|offset| is clamped to it)
		bool          siteInstalled{ false };
		std::uint64_t siteCalls{ 0 };
		std::uint64_t siteApplied{ 0 };
		bool          installed{ false };
		bool          chained{ false };  // M4b11: slot 12 was held by an allow-listed plugin (DDC.dll); our hook is in front of it and calls its function as the original
		bool          compat{ false };   // T4.0: another mod owns the ThirdPersonState vtable; CoverSF pumps its casts from the call-site hook and has no camera of its own
		bool          faulted{ false };
		std::uint64_t pivotWrites{ 0 };    // mode 4: Updates in which the pivot node's local x was shifted
		std::uint64_t pivotRestored{ 0 };  // ... and written back
		std::uint64_t pivotChanged{ 0 };   // ... but the float had changed meanwhile, so it was left alone
		std::uint64_t pivotNull{ 0 };      // Updates in which the pivot node could not be read
	};

	// S4b probe: one reading, taken in the Update hook after the original ran (see Camera.cpp DoProbe).
	struct ProbeResult
	{
		bool        ok{ false };
		std::string label;
		char        convention{ '?' };  // yaw convention that matched the call-site matrix ('A' forward (sin, cos), 'B' (-sin, cos))
		bool        convOk{ false };    // both dot products above 0.9
		float       applied{ 0.0f };
		float       side{ 0.0f };       // camera root minus player, along the yaw right
		float       back{ 0.0f };       // ... behind the yaw forward
		float       up{ 0.0f };
		float       pSide{ 0.0f };      // the look-at point P relative to the player
		float       pForward{ 0.0f };
		float       horizDistToP{ 0.0f };
		float       angleDeg{ 0.0f };      // final forward row (row 1) against the yaw forward, horizontal, counter-clockwise positive
		float       predictedDeg{ 0.0f };  // atan(applied / horizontal distance camera to P)
		float       distLine[3]{ 0, 0, 0 };  // distance of P from the line through the camera along each row of its final rotation
		int         bestRow{ -1 };
		float       blend{ 0.0f };
		float       pivotX{ 0.0f };
		float       factor{ 0.0f };      // state+0x250 (engine collision factor, 1 = camera not pulled in)
		float       timer{ 0.0f };       // state+0x254
		float       wantedToFinal{ 0.0f };  // distance between +0x19C (wanted) and +0x190 (after collision)
	};

	// The engine's collision state of the third-person camera, read on demand (any thread, guarded).
	struct CollisionState
	{
		bool  ok{ false };
		float factor{ 0.0f };
		float timer{ 0.0f };
		float wantedToFinal{ 0.0f };
	};
	[[nodiscard]] CollisionState ReadCollision();

	// The view direction of the third-person camera as a heading in radians (forward = (sin h, cos h)): the yaw float at
	// ThirdPersonState + 0x230 (Offsets.h kTpsYaw, convention confirmed by S4b). False when it cannot be read. Any thread, guarded.
	[[nodiscard]] bool ReadViewYaw(float& a_yaw);

	// What the SFSE-task side sees of the camera root right now.
	struct Sample
	{
		bool         ok{ false };
		RE::NiPoint3 rootWorld{};
		RE::NiPoint3 rootLocal{};
		bool         hasParent{ false };
		RE::NiPoint3 player{};
		float        distance{ 0.0f };  // horizontal camera-to-player
	};

	// Tick thread: installs the hook (once the PlayerCamera exists), recomputes the collision limit at about 20 Hz,
	// runs the pending state scan.
	void OnTick(const Frame::Snapshot& a_snap);

	// Target offset in metres (positive = right of the view). Thread-safe.
	void SetOffsetTarget(float a_metres) noexcept;
	[[nodiscard]] float OffsetTarget() noexcept;
	// Flip the side of the target. Thread-safe (the flip key calls this from the input hook).
	void Flip() noexcept;
	// 0 = the offset is not applied, 3 = applied in the call-site hook (default, [Camera] iCameraMode). Thread-safe.
	void SetMode(int a_mode) noexcept;
	[[nodiscard]] int Mode() noexcept;

	// 4 = pivot shift (S4b): the eased offset is added to the local x of the node at ThirdPersonState + 0x200 for the duration of the
	// original Update. The plugin's own collision limit is not applied in that mode (the engine's collision reads the shifted node).
	// S4b probe: ask for one reading; the next Update call on the hook thread logs a RESULT line and stores it for LastProbe().
	void ProbeRequest(const std::string& a_label);
	[[nodiscard]] ProbeResult LastProbe();

	[[nodiscard]] Stats GetStats() noexcept;
	// True in compatibility mode (T4.0).
	[[nodiscard]] bool Compat() noexcept;
	[[nodiscard]] Sample SampleRoot();
	// T5.8 probe: the nine entries of the camera root's world rotation (rows of NiMatrix3), false when unreadable. Any thread, guarded.
	[[nodiscard]] bool ReadRootRot(float (&a_out)[9]);
	// T5.8 probe: a_count floats of the ThirdPersonState object from a_offset on (raw reads, guarded); false when unreadable.
	// T5.8 probe at the call-site hook (once per frame, before and after the original pose call): between ProbeBegin and ProbeEnd the minimum and maximum of every value of the list below
	// are kept; ProbeEnd logs the values that moved (range above 1e-4) in one line. The list: the rotation handed to the pose call (before and after), the position after, the ThirdPersonState floats
	// at +0x160 to +0x25C, the local and world rotations of the nodes at +0x1F8, +0x200, +0x208 and +0x210, the camera root's world rotation (the previous frame's result).
	// T5.2c probe at the call-site hook (card M3h): one line per frame between YawProbeBegin and YawProbeEnd (the body's angle z at player + 0x88, the offset at player + 0xE2C, their sum, the
	// state's yaw +0x230, the camera root's world position and its distance from the head), the numbers summarised and the lines logged at the end.
	// the boom node's local y as the animation graph wrote it this frame, read in the hook before the rig hold overwrote it (nullopt when the hold is off or the value is older than 0.25 s)
	[[nodiscard]] std::optional<float> GraphBoomY();
	void YawProbeBegin(const char* a_label);
	struct YawSummary
	{
		std::size_t frames{ 0 };
		float       minHead{ 0.0f }, maxJump{ 0.0f };          // all rows
		float       poseMinHead{ 0.0f }, poseMaxJump{ 0.0f };  // from the first row with the pose on (all rows when the pose was on from the start)
	};
	YawSummary YawProbeEnd();
	void ProbeBegin(const char* a_label);
	void ProbeEnd();
	[[nodiscard]] bool ReadTpsFloats(std::ptrdiff_t a_offset, int a_count, float* a_out);

	// T5.0b: the camera rig as the engine reads it: the nodes at ThirdPersonState + 0x1F8, +0x200, +0x208, +0x210, +0x218, the flag at +0x291, the collision factor, the yaw, the
	// blend, the two positions at +0x190 and +0x19C, the camera root and the player. Any thread, guarded.
	struct RigNode
	{
		bool         present{ false };
		const void*  ptr{ nullptr };
		std::string  name;
		RE::NiPoint3 t{};
		float        rot[9]{};
	};
	struct RigState
	{
		bool         ok{ false };
		RigNode      n1F8, n200, n208, n210, n218;
		bool         haveFlag{ false };
		std::uint8_t flag291{ 0 };
		float        factor{ 0.0f }, yaw{ 0.0f }, blend{ 0.0f };
		RE::NiPoint3 p190{}, p19c{}, rootWorld{}, player{};
		float        rootToPlayer{ 0.0f }, rootToPlayerHoriz{ 0.0f };
	};
	[[nodiscard]] RigState    ReadRig();
	[[nodiscard]] std::string DescribeRig(const char* a_label, const RigState& a_state);
	// T5.0b spike: while on (and [Anim] bRigHold is 1), the hook site writes these two points into the local translates of the nodes at +0x1F8 and +0x200 before its original call,
	// for the live third-person state only. Nothing is written back afterwards. RigHoldWrites() counts the writes.
	void SetRigHold(bool a_on, const RE::NiPoint3& a_boom, const RE::NiPoint3& a_pivot);
	// card M4b spike: while on, the framing's pivot follows the body (the COM bone's offset from the Root bone, turned into the view's frame)
	void SetCornerFollow(bool a_on, int a_sideDir = 0) noexcept;  // a_sideDir +1 / -1: the shoulder the camera is on while the corner hold is on (0 = the player's own choice)
	struct CornerStats
	{
		int   frames{ 0 };       // frames the hook held the heading
		int   corrections{ 0 };  // frames in which the engine had moved the heading by more than 0.5 degree since the hook's last write
		float maxDevDeg{ 0.0f };
		int   lateFrames{ 0 };   // frames in which the heading was off by more than 0.5 degree again right after the camera's own update returned
		float lateMaxDeg{ 0.0f };
		int   clamps{ 0 };
	};
	[[nodiscard]] CornerStats GetCornerStats() noexcept;
	void                      ResetCornerStats() noexcept;
	// card M4b spike: every frame, in the call-site hook, the actor's angle z is written to a_heading and the view's yaw is kept within a_cone radians of it (a_cone 0 = no limit)
	void SetCornerHold(bool a_on, float a_heading, float a_cone) noexcept;
	[[nodiscard]] std::uint64_t RigHoldWrites() noexcept;
	// T5.3: new values for a hold that is on (no log line); the hook site writes them from the next frame on.
	void SetRigHoldValues(const RE::NiPoint3& a_boom, const RE::NiPoint3& a_pivot) noexcept;
	// T5.2c: the hold is not dropped at once but eased from the held values to what the graph writes (read in the hook before the overwrite) over a_seconds, then it goes off
	void ReleaseRigHold(float a_seconds);
	// T5.2c: the entry blend: from the next frame the camera position is blended from its value at that frame to the engine's over a_seconds (smoothstep); the position is kept in the yaw frame so that looking round does not rotate it
	void BeginEntryBlend(float a_seconds);

	// T5.3a, the cover camera for Basic: the call-site hook adds an eased offset to what the animation graph wrote into the boom node (+0x1F8: local y gets -back) and the pivot node
	// (+0x200: local z gets +up) this frame, before the pose function reads them. Added to this frame's values; a value the graph did not rewrite is recognised (it equals what was
	// written last) and the offset is taken out of it first, so nothing accumulates. Only with [Cover] bCoverCamera 1 and only for the live third-person state; off for the session after a
	// fault. SetCoverCam gives the target (metres, already multiplied by the aim share), tick side; CoverCamReset zeroes the applied offset at once (a hard reset).
	void SetCoverCam(float a_back, float a_up) noexcept;
	void CoverCamReset() noexcept;
	struct CoverCamState
	{
		float         backNow{ 0.0f }, upNow{ 0.0f };        // the offset the latest call used
		float         backTarget{ 0.0f }, upTarget{ 0.0f };
		std::uint64_t writes{ 0 };                           // calls that wrote the nodes
		std::uint64_t restored{ 0 };                         // calls that took the offset out of a value the graph had not rewritten
		std::uint64_t leftAlone{ 0 };                        // restores skipped because the node no longer held what was written
		bool          faulted{ false };
	};
	[[nodiscard]] CoverCamState GetCoverCam() noexcept;

	// M4a (Phase 4): the own camera's framing (REFERENCE 23). Situations: 0 exploring (weapon away), 1 weapon drawn, 2 aiming, 3 sprinting, 4 low cover, 5 tall cover, 6 low cover aiming, 7 tall cover aiming.
	// The hook site writes the framing into the boom (y = -distance) and pivot (x = shoulder, z = the game's height + an offset) before the pose call and puts the two nodes back after it.
	struct FramingState
	{
		bool          on{ false };             // the framing is wanted and the hook is installed (not in compatibility mode)
		int           situation{ 0 };
		float         dist{ 0.0f }, shoulder{ 0.0f }, height{ 0.0f };  // the eased values of the latest frame (metres)
		float         side{ 1.0f };            // eased: +1 right shoulder, -1 left
		int           sideTarget{ 1 };
		float         pivotX{ 0.0f };          // the x written into the pivot node
		float         boomY{ 0.0f };           // the y written into the boom node
		std::uint64_t writes{ 0 };
		bool          faulted{ false };
	};
	[[nodiscard]] FramingState GetFraming() noexcept;
	[[nodiscard]] const char*  SituationName(int a_situation) noexcept;
	// swaps the shoulder (the key and the rig call this); thread-safe
	void SwapShoulder() noexcept;
	void SetShoulder(int a_dir) noexcept;  // +1 right, -1 left
	// the cover kind the framing is in: 0 none, 1 low, 2 tall (the cover tick sets it every tick); a_aimHeld = the aim control is held
	void SetCoverKind(int a_kind, bool a_aimHeld) noexcept;
	// rig only: the situation is this one (0 to 7) whatever the game says; -1 = the game decides
	void ForceSituation(int a_situation) noexcept;

	// Guarded wrappers around PlayerCamera::ForceFirstPerson / ForceThirdPerson. Return false on a fault.
	[[nodiscard]] bool ForceFirstPerson();
	[[nodiscard]] bool ForceThirdPerson();
}
