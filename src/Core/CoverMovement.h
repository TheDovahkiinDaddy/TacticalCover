#pragma once

// T3.3 / T3.3b: moving along the cover, as pure functions (engine-free, tested). The input hook calls these after every call of the game's own
// PerformInputProcessing, on the movement vector the game computed (x strafe, y forward: a stick copies its two floats, a key writes a constant, docs/REFERENCE.md
// 18.11): the vector is replaced by the part along the wall, capped, and braked before an end of the cover, so a key and a stick are treated the same way, like a stick
// held part of the way. The rig cannot move a stick, so these functions and their tests are the evidence for the stick path.
//
// Conventions (docs/REFERENCE.md 18.1): a camera yaw h gives forward = (sin h, cos h) and right = (cos h, -sin h); an input (x, y) is x to the
// right and y forward; the wall normal points from the wall to the player, the tangent is Perp(normal).

#include "Core/CoverSensor.h"

namespace Core
{
	// The room left along the wall in each direction, from the player position to the stop point (fEdgeMargin inside the end; negative: already past it), and the
	// settings of the filter.
	struct MoveLimits
	{
		float remainPlus{ 10.0f };     // along +tangent
		float remainMinus{ 10.0f };    // along -tangent
		float brakeDistance{ 0.5f };   // [Movement] fEdgeBrakeDistance: toward an end the input falls linearly from its full value this far before the stop point to 0 at it
		float maxInput{ 0.6f };        // [Movement] fCoverMaxInput: the most input (0..1, the game walk is about half) along the wall
		// T3.3e: the input is 0 from this distance before the stop point on (the player coasts the rest: the game speed does not fall with the input between about 0.1 and
		// 0.5, so a long linear brake only ends the movement at the game dead zone, REFERENCE 18.14); the brake distance counts from that point.
		float stopDistance{ 0.0f };    // [Movement] fEdgeStopDistance / fEdgeStopDistanceLow
		// T3.3f: an along-the-wall part of the input smaller than this in size is written as exactly (0, 0): the game speed is a step (any input above its dead zone walks
		// at the full gear), so a stick held 6 to 17 degrees off the wall normal would already walk along the wall.
		float alongMin{ 0.0f };        // [Movement] fCoverAlongMin
	};

	// T3.3d: what a Track answer does to the cover. Ok: the candidate is replaced, everything is reset. Corner (the wall fit near an inside corner goes bad): the last good
	// candidate stays, the count of lost answers is not touched and nothing stops except the movement toward the side the player was last moving along the wall (until the next
	// Ok answer); the other way stays free. Lost and Unavailable: counted, all movement stops (three in a row leave, the state machine decides).
	struct TrackFilter
	{
		int  lostCount{ 0 };      // consecutive Lost / Unavailable answers
		bool stopAll{ false };    // all movement stops until the wall is found again
		int  blockedSide{ 0 };    // +1: movement along +tangent is stopped, -1: along -tangent, 0: none
	};

	struct TrackDecision
	{
		TrackFilter next{};
		bool        useCandidate{ false };  // the answer's candidate replaces the cover's candidate (Ok only)
	};

	// a_lastSide: +1 or -1, the side along the wall the player was last moving toward (0: not known).
	[[nodiscard]] TrackDecision DecideTrack(TrackStatus a_status, const TrackFilter& a_prev, int a_lastSide) noexcept;

	// The limits with the blocked side's room taken to 0 (min(room, 0)).
	void ApplyBlockedSide(MoveLimits& a_limits, int a_blockedSide) noexcept;

	[[nodiscard]] Vec2 InputToWorld(float a_yaw, float a_x, float a_y) noexcept;
	[[nodiscard]] Vec2 WorldToInput(float a_yaw, Vec2 a_world) noexcept;

	// The limits at a_player for a candidate whose edges are measured from candidate.linePoint (Find and Track both set it): an edge that was not found
	// within the search range (EdgeKind::None) leaves 10 m; every found edge, Open or Blocked, ends the cover a_edgeMargin before it. brakeDistance and maxInput keep their defaults.
	[[nodiscard]] MoveLimits LimitsFor(const CoverCandidate& a_candidate, Vec2 a_player, float a_edgeMargin) noexcept;

	struct StickResult
	{
		float x{ 0.0f };        // the rewritten input
		float y{ 0.0f };
		float along{ 0.0f };    // signed part of the input along the tangent (before scaling and the cap)
		float away{ 0.0f };     // the part of the input pointing away from the wall, 0..1; 0 when pointing at the wall
		bool  atEnd{ false };   // the kept part was stopped by an end of the cover
	};

	// The input kept to the tangent part times a_moveScale (default 1.0), capped to +-a_limits.maxInput. The part along the normal (toward or away from the wall) is
	// dropped. Toward an end the kept part is multiplied by remain / brakeDistance (0 at the stop point or past it, 1 from brakeDistance before it).
	// The input is the one the game computed: a_x right and a_y forward in the camera frame of a_yaw. Works for a stick and for keys alike.
	[[nodiscard]] StickResult RewriteStick(float a_x, float a_y, float a_yaw, Vec2 a_tangent, Vec2 a_normal, const MoveLimits& a_limits, float a_moveScale) noexcept;

	// The part of the whole movement input (keys and stick together, a_x right and a_y forward) that points away from the wall, 0..1.
	[[nodiscard]] float AwayInput(float a_x, float a_y, float a_yaw, Vec2 a_normal) noexcept;

	struct Correction
	{
		bool  needed{ false };
		Vec2  delta{};            // horizontal move to make this tick (at most a_maxStep long)
		float perp{ 0.0f };       // signed distance from the cover line, positive away from the wall
		float overshoot{ 0.0f };  // how far past an end of the cover, 0 when inside (reported, not corrected: the braking zone keeps the player inside)
	};

	// How far the player has drifted off the cover line (the line through candidate.standPoint along the tangent). A correction is only needed beyond a_tolerance
	// (fHoldTolerance) and is capped to a_maxStep per call: it removes small drift that movement input caused, it is not a leash (the owner leaves cover instead
	// when the drift is larger than fMaxDeviation or the player was moved by something else). Nothing along the wall: the filter above keeps the ends.
	[[nodiscard]] Correction DriftCorrection(const CoverCandidate& a_candidate, Vec2 a_player, const MoveLimits& a_limits, float a_tolerance, float a_maxStep) noexcept;
}
