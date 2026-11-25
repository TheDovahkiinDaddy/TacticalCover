// Engine-free tests for Core/CoverMovement (T3.3, T3.3b): the filter on the game movement vector (the part along the wall, a cap, a braking zone before the ends),
// the "away" amount, the edge limits and the drift correction. The rig cannot move a stick, so these are the evidence for the stick path.

#include "Core/CoverMovement.h"

#include <cmath>
#include <cstdio>

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

	using namespace Core;

	constexpr float kPi = 3.14159265358979f;

	bool Near(const float a, const float b, const float e = 1e-3f) { return std::fabs(a - b) <= e; }

	// A wall along the world y axis at x = 10; the player stands at x = 9.65 on its west side, so the normal (wall to player) is (-1, 0) and the
	// tangent Perp(normal) = (0, -1). The camera yaw is the heading the player looks along: yaw = pi/2 looks east, at the wall.
	struct Scene
	{
		Vec2 normal{ -1.0f, 0.0f };
		Vec2 tangent{ 0.0f, -1.0f };
	};

	// no cap, a 0.5 m braking zone, ends far away
	MoveLimits Free()
	{
		MoveLimits l;
		l.maxInput = 1.0f;
		return l;
	}

	void TestConventions()
	{
		// yaw 0: forward = +y, right = +x
		auto w = InputToWorld(0.0f, 0.0f, 1.0f);
		CHECK(Near(w.x, 0.0f) && Near(w.y, 1.0f));
		w = InputToWorld(0.0f, 1.0f, 0.0f);
		CHECK(Near(w.x, 1.0f) && Near(w.y, 0.0f));
		// yaw 90 degrees: forward = +x, right = -y (the right-hand side of east is south)
		w = InputToWorld(kPi / 2, 0.0f, 1.0f);
		CHECK(Near(w.x, 1.0f) && Near(w.y, 0.0f));
		w = InputToWorld(kPi / 2, 1.0f, 0.0f);
		CHECK(Near(w.x, 0.0f) && Near(w.y, -1.0f));
		// the round trip
		const auto in = WorldToInput(1.1f, InputToWorld(1.1f, 0.3f, -0.7f));
		CHECK(Near(in.x, 0.3f) && Near(in.y, -0.7f));
	}

	void TestStickAlongTheWall()
	{
		Scene s;
		const MoveLimits free = Free();
		const float yaw = kPi / 2;  // looking at the wall: forward is toward it, right is south
		// the stick pushed to the right: along the wall (tangent is south), kept whole
		auto r = RewriteStick(1.0f, 0.0f, yaw, s.tangent, s.normal, free, 1.0f);
		CHECK(Near(r.x, 1.0f) && Near(r.y, 0.0f) && Near(r.away, 0.0f) && !r.atEnd);
		// pushed to the left: the other way along the wall
		r = RewriteStick(-1.0f, 0.0f, yaw, s.tangent, s.normal, free, 1.0f);
		CHECK(Near(r.x, -1.0f) && Near(r.y, 0.0f));
		// half a stick stays half a stick (not scaled down by default)
		r = RewriteStick(0.5f, 0.0f, yaw, s.tangent, s.normal, free, 1.0f);
		CHECK(Near(r.x, 0.5f) && Near(r.y, 0.0f));
		// pushed at the wall: nothing kept, nothing away
		r = RewriteStick(0.0f, 1.0f, yaw, s.tangent, s.normal, free, 1.0f);
		CHECK(Near(r.x, 0.0f) && Near(r.y, 0.0f) && Near(r.away, 0.0f));
		// pulled away from the wall: nothing kept, the away amount is reported
		r = RewriteStick(0.0f, -1.0f, yaw, s.tangent, s.normal, free, 1.0f);
		CHECK(Near(r.x, 0.0f) && Near(r.y, 0.0f) && Near(r.away, 1.0f));
		// a diagonal: forward-right (toward the wall and along it): only the along part is kept
		r = RewriteStick(0.6f, 0.8f, yaw, s.tangent, s.normal, free, 1.0f);
		CHECK(Near(r.x, 0.6f) && Near(r.y, 0.0f));
		// a diagonal away from the wall: right and back
		r = RewriteStick(0.6f, -0.8f, yaw, s.tangent, s.normal, free, 1.0f);
		CHECK(Near(r.x, 0.6f) && Near(r.y, 0.0f) && Near(r.away, 0.8f));
		// the scale setting
		r = RewriteStick(1.0f, 0.0f, yaw, s.tangent, s.normal, free, 0.5f);
		CHECK(Near(r.x, 0.5f));
	}

	// a key is a full vector: the game writes a constant 1.0, the cap turns it into the walk
	void TestCap()
	{
		Scene s;
		MoveLimits l;  // default: cap 0.6
		const float yaw = kPi / 2;
		auto r = RewriteStick(1.0f, 0.0f, yaw, s.tangent, s.normal, l, 1.0f);  // D held: the game vector (1, 0)
		CHECK(Near(r.x, 0.6f) && Near(r.y, 0.0f));
		r = RewriteStick(-1.0f, 0.0f, yaw, s.tangent, s.normal, l, 1.0f);      // A held
		CHECK(Near(r.x, -0.6f));
		r = RewriteStick(0.4f, 0.0f, yaw, s.tangent, s.normal, l, 1.0f);       // a stick under the cap is not touched
		CHECK(Near(r.x, 0.4f));
		l.maxInput = 0.3f;
		r = RewriteStick(1.0f, 0.0f, yaw, s.tangent, s.normal, l, 1.0f);
		CHECK(Near(r.x, 0.3f));
		// a diagonal key pair (W and D together, the game vector (1, 1) before it is limited to the unit disc, here (0.707, 0.707)): the along part is 0.707, capped
		l.maxInput = 0.6f;
		r = RewriteStick(0.7071f, 0.7071f, yaw, s.tangent, s.normal, l, 1.0f);
		CHECK(Near(r.x, 0.6f, 2e-3f) && Near(r.y, 0.0f, 2e-3f));
	}

	void TestStickCameraNotSquare()
	{
		Scene s;
		const MoveLimits free = Free();
		// the camera looks along the wall (south): forward is along the tangent
		const float yaw = kPi;  // forward = (sin pi, cos pi) = (0, -1) = the tangent
		auto r = RewriteStick(0.0f, 1.0f, yaw, s.tangent, s.normal, free, 1.0f);
		CHECK(Near(r.x, 0.0f) && Near(r.y, 1.0f) && Near(r.away, 0.0f));
		// right is (cos pi, -sin pi) = (-1, 0) = away from the wall
		r = RewriteStick(1.0f, 0.0f, yaw, s.tangent, s.normal, free, 1.0f);
		CHECK(Near(r.x, 0.0f) && Near(r.y, 0.0f) && Near(r.away, 1.0f));
		// a camera 45 degrees off: forward-left stick is along the wall again
		const float yaw45 = kPi / 2 + kPi / 4;  // looks south-east: forward = (0.707, -0.707)
		r = RewriteStick(0.0f, 1.0f, yaw45, s.tangent, s.normal, free, 1.0f);
		CHECK(Near(r.x, 0.5f, 2e-3f) && Near(r.y, 0.5f, 2e-3f) && Near(r.away, 0.0f));  // the tangent part 0.707 expressed in this camera's right and forward
		// whatever the camera, the rewritten input moves the player along the tangent: check the result in the world
		for (float h = 0.0f; h < 6.28f; h += 0.37f) {
			const auto out = RewriteStick(0.3f, 0.9f, h, s.tangent, s.normal, free, 1.0f);
			const auto world = InputToWorld(h, out.x, out.y);
			CHECK(Near(Dot(world, s.normal), 0.0f, 2e-3f));
		}
	}

	void TestBrake()
	{
		Scene s;
		const float yaw = kPi / 2;
		MoveLimits  l = Free();
		l.brakeDistance = 0.5f;
		l.remainPlus = 5.0f;
		l.remainMinus = 5.0f;
		// far from the end: untouched
		auto r = RewriteStick(1.0f, 0.0f, yaw, s.tangent, s.normal, l, 1.0f);
		CHECK(Near(r.x, 1.0f) && !r.atEnd);
		// the braking zone: 1 at brakeDistance before the stop point, falling linearly to 0 at it. D / a stick to the right is +tangent (south)
		l.remainPlus = 0.5f;
		r = RewriteStick(1.0f, 0.0f, yaw, s.tangent, s.normal, l, 1.0f);
		CHECK(Near(r.x, 1.0f));
		l.remainPlus = 0.25f;
		r = RewriteStick(1.0f, 0.0f, yaw, s.tangent, s.normal, l, 1.0f);
		CHECK(Near(r.x, 0.5f, 1e-3f) && !r.atEnd);
		l.remainPlus = 0.05f;
		r = RewriteStick(1.0f, 0.0f, yaw, s.tangent, s.normal, l, 1.0f);
		CHECK(Near(r.x, 0.1f, 1e-3f));
		l.remainPlus = 0.0f;  // at the stop point: nothing
		r = RewriteStick(1.0f, 0.0f, yaw, s.tangent, s.normal, l, 1.0f);
		CHECK(Near(r.x, 0.0f) && r.atEnd);
		l.remainPlus = -0.2f;  // past it: nothing, and the other way still works at full value
		r = RewriteStick(1.0f, 0.0f, yaw, s.tangent, s.normal, l, 1.0f);
		CHECK(Near(r.x, 0.0f) && r.atEnd);
		r = RewriteStick(-1.0f, 0.0f, yaw, s.tangent, s.normal, l, 1.0f);
		CHECK(Near(r.x, -1.0f) && !r.atEnd);
		// the other end brakes the other direction
		l.remainPlus = 5.0f;
		l.remainMinus = 0.1f;
		r = RewriteStick(-1.0f, 0.0f, yaw, s.tangent, s.normal, l, 1.0f);
		CHECK(Near(r.x, -0.2f, 1e-3f));
		r = RewriteStick(1.0f, 0.0f, yaw, s.tangent, s.normal, l, 1.0f);
		CHECK(Near(r.x, 1.0f));
		// the cap and the brake together: the brake multiplies the capped value (cap 0.6, 0.25 m before the stop point of a 0.5 m zone: 0.3)
		l = MoveLimits{};
		l.remainPlus = 0.25f;
		r = RewriteStick(1.0f, 0.0f, yaw, s.tangent, s.normal, l, 1.0f);
		CHECK(Near(r.x, 0.3f, 1e-3f));
		// the brake setting is a setting
		l.brakeDistance = 1.0f;
		r = RewriteStick(1.0f, 0.0f, yaw, s.tangent, s.normal, l, 1.0f);
		CHECK(Near(r.x, 0.15f, 1e-3f));
		// a sequence at 60 Hz: the player moves at the written input times a speed of 5 m/s per unit of input, never reaches the stop point and gets very close to it
		MoveLimits seq;
		seq.remainPlus = 2.0f;
		float remain = 2.0f;
		for (int i = 0; i < 600 && remain > 0.0f; ++i) {
			seq.remainPlus = remain;
			const auto o = RewriteStick(1.0f, 0.0f, yaw, s.tangent, s.normal, seq, 1.0f);
			remain -= o.x * 5.0f / 60.0f;
		}
		CHECK(remain > 0.0f && remain < 0.05f);  // an exponential approach: it stops short of the point and never passes it
	}

	// T3.3e: the input is 0 from stopDistance before the stop point on; the brake counts from there
	void TestStopDistance()
	{
		Scene s;
		const float yaw = kPi / 2;
		MoveLimits  l = Free();
		l.brakeDistance = 0.15f;
		l.stopDistance = 0.35f;
		l.remainPlus = 5.0f;
		l.remainMinus = 5.0f;
		auto r = RewriteStick(1.0f, 0.0f, yaw, s.tangent, s.normal, l, 1.0f);  // far: full
		CHECK(Near(r.x, 1.0f) && !r.atEnd);
		l.remainPlus = 0.50f;  // 0.15 past the stop distance: the brake starts, still 1
		r = RewriteStick(1.0f, 0.0f, yaw, s.tangent, s.normal, l, 1.0f);
		CHECK(Near(r.x, 1.0f) && !r.atEnd);
		l.remainPlus = 0.425f;  // half way through the brake
		r = RewriteStick(1.0f, 0.0f, yaw, s.tangent, s.normal, l, 1.0f);
		CHECK(Near(r.x, 0.5f) && !r.atEnd);
		l.remainPlus = 0.35f;  // the stop distance: nothing, at the end
		r = RewriteStick(1.0f, 0.0f, yaw, s.tangent, s.normal, l, 1.0f);
		CHECK(Near(r.x, 0.0f) && r.atEnd);
		l.remainPlus = 0.1f;  // inside the stop distance: nothing; the other way is free
		r = RewriteStick(1.0f, 0.0f, yaw, s.tangent, s.normal, l, 1.0f);
		CHECK(Near(r.x, 0.0f) && r.atEnd);
		r = RewriteStick(-1.0f, 0.0f, yaw, s.tangent, s.normal, l, 1.0f);
		CHECK(Near(r.x, -1.0f) && !r.atEnd);
		// the cap and the stop distance together (the low pair: cap 1.0 here, brake 0.15)
		l.maxInput = 0.5f;
		l.remainPlus = 5.0f;
		r = RewriteStick(1.0f, 0.0f, yaw, s.tangent, s.normal, l, 1.0f);
		CHECK(Near(r.x, 0.5f));
	}

	// T3.3f: the along dead zone
	void TestAlongMin()
	{
		Scene s;
		const float yaw = kPi / 2;  // looks east, at the wall
		MoveLimits  l = Free();
		l.alongMin = 0.30f;
		// S held (straight back from the view, here away from... the wall is ahead, so W is toward it): along part 0, nothing written
		auto r = RewriteStick(0.0f, 1.0f, yaw, s.tangent, s.normal, l, 1.0f);
		CHECK(r.x == 0.0f && r.y == 0.0f);
		// 1 degree off the wall normal: along part 0.017: exactly zero
		r = RewriteStick(std::sin(1.0f * kPi / 180.0f), std::cos(1.0f * kPi / 180.0f), yaw, s.tangent, s.normal, l, 1.0f);
		CHECK(r.x == 0.0f && r.y == 0.0f);
		// 17 degrees (the along part 0.29): zero; 18 degrees (0.309): passes
		r = RewriteStick(std::sin(17.0f * kPi / 180.0f), std::cos(17.0f * kPi / 180.0f), yaw, s.tangent, s.normal, l, 1.0f);
		CHECK(r.x == 0.0f && r.y == 0.0f);
		r = RewriteStick(std::sin(18.0f * kPi / 180.0f), std::cos(18.0f * kPi / 180.0f), yaw, s.tangent, s.normal, l, 1.0f);
		CHECK(std::fabs(r.x) > 0.3f && Near(r.y, 0.0f));
		// a key along the wall (full 1.0) is far above it
		r = RewriteStick(1.0f, 0.0f, yaw, s.tangent, s.normal, l, 1.0f);
		CHECK(Near(r.x, 1.0f));
		// the dead zone does not report an end
		MoveLimits e = l;
		e.remainPlus = 0.0f;
		e.remainMinus = 0.0f;
		r = RewriteStick(0.0f, 1.0f, yaw, s.tangent, s.normal, e, 1.0f);
		CHECK(!r.atEnd && r.x == 0.0f && r.y == 0.0f);
		// default (alongMin 0): a tiny input passes as before
		MoveLimits d = Free();
		r = RewriteStick(std::sin(1.0f * kPi / 180.0f), std::cos(1.0f * kPi / 180.0f), yaw, s.tangent, s.normal, d, 1.0f);
		CHECK(std::fabs(r.x) > 0.01f);
	}

	// T3.3d: what a Track answer does
	void TestTrackDecision()
	{
		TrackFilter f;
		// Ok: the candidate is used and everything is reset
		auto d = DecideTrack(TrackStatus::Ok, f, 1);
		CHECK(d.useCandidate && d.next.lostCount == 0 && !d.next.stopAll && d.next.blockedSide == 0);
		// Corner while moving plus: the candidate is kept (not used), nothing counted, no stop of everything, the plus side blocked
		d = DecideTrack(TrackStatus::Corner, f, 1);
		CHECK(!d.useCandidate && d.next.lostCount == 0 && !d.next.stopAll && d.next.blockedSide == 1);
		MoveLimits l;
		ApplyBlockedSide(l, d.next.blockedSide);
		CHECK(l.remainPlus <= 0.0f && l.remainMinus > 5.0f);
		// Corner while moving minus: the minus side
		d = DecideTrack(TrackStatus::Corner, f, -1);
		CHECK(d.next.blockedSide == -1 && d.next.lostCount == 0 && !d.next.stopAll);
		MoveLimits m;
		ApplyBlockedSide(m, d.next.blockedSide);
		CHECK(m.remainMinus <= 0.0f && m.remainPlus > 5.0f);
		// Corner again, no movement known: the blocked side stays
		auto d2 = DecideTrack(TrackStatus::Corner, d.next, 0);
		CHECK(d2.next.blockedSide == -1 && d2.next.lostCount == 0);
		// many Corners in a row never count
		TrackFilter g;
		for (int k = 0; k < 10; ++k) {
			g = DecideTrack(TrackStatus::Corner, g, 1).next;
		}
		CHECK(g.lostCount == 0 && !g.stopAll && g.blockedSide == 1);
		// Corner then Ok: free again
		g = DecideTrack(TrackStatus::Ok, g, 1).next;
		CHECK(g.blockedSide == 0 && g.lostCount == 0 && !g.stopAll);
		// Lost three times: counted, everything stops, the blocked side is kept, no candidate
		TrackFilter h;
		for (int k = 1; k <= 3; ++k) {
			const auto e = DecideTrack(TrackStatus::Lost, h, 1);
			h = e.next;
			CHECK(!e.useCandidate && h.lostCount == k && h.stopAll);
		}
		const auto u = DecideTrack(TrackStatus::Unavailable, h, 0);
		CHECK(u.next.lostCount == 4 && u.next.stopAll && !u.useCandidate);
		// a Corner between Lost answers does not reset the count
		TrackFilter c2;
		c2 = DecideTrack(TrackStatus::Lost, c2, 1).next;
		c2 = DecideTrack(TrackStatus::Corner, c2, 1).next;
		c2 = DecideTrack(TrackStatus::Lost, c2, 1).next;
		CHECK(c2.lostCount == 2);
	}

	void TestAway()
	{
		Scene s;
		const float yaw = kPi / 2;
		CHECK(Near(AwayInput(0.0f, -1.0f, yaw, s.normal), 1.0f));  // S from the wall
		CHECK(Near(AwayInput(0.0f, 1.0f, yaw, s.normal), 0.0f));   // W at it
		CHECK(Near(AwayInput(1.0f, 0.0f, yaw, s.normal), 0.0f));   // along it
		CHECK(Near(AwayInput(0.6f, -0.8f, yaw, s.normal), 0.8f));
		// the exit threshold 0.6 is crossed by a pull of 0.6 and not by a diagonal key pair (0.707 each way: 0.707 away)
		CHECK(AwayInput(1.0f, -1.0f, yaw, s.normal) < 1.0f + 1e-5f);
	}

	void TestLimitsFor()
	{
		CoverCandidate c;
		c.kind = CoverKind::High;
		c.normal = { -1.0f, 0.0f };
		c.tangent = { 0.0f, -1.0f };
		c.linePoint = { 9.9f, 0.0f };
		c.edgeLeft = 1.0f;
		c.edgeLeftKind = EdgeKind::Open;
		c.edgeRight = 2.0f;
		c.edgeRightKind = EdgeKind::Blocked;
		// at the foot: the room is the edge minus the margin
		auto l = LimitsFor(c, { 9.9f, 0.0f }, 0.25f);
		CHECK(Near(l.remainPlus, 1.75f) && Near(l.remainMinus, 0.75f));
		// 0.5 m along +tangent (south, y = -0.5): the room that way shrinks, the other grows
		l = LimitsFor(c, { 9.9f, -0.5f }, 0.25f);
		CHECK(Near(l.remainPlus, 1.25f) && Near(l.remainMinus, 1.25f));
		// an edge not found within the range leaves 10 m
		c.edgeRightKind = EdgeKind::None;
		l = LimitsFor(c, { 9.9f, 0.0f }, 0.25f);
		CHECK(Near(l.remainPlus, 10.0f) && Near(l.remainMinus, 0.75f));
		// past the end: negative
		l = LimitsFor(c, { 9.9f, 0.9f }, 0.25f);
		CHECK(l.remainMinus < 0.0f);
	}

	void TestCorrection()
	{
		CoverCandidate c;
		c.kind = CoverKind::High;
		c.normal = { -1.0f, 0.0f };
		c.tangent = { 0.0f, -1.0f };
		c.standPoint = { 9.65f, 0.0f, 0.0f };
		const MoveLimits free;
		// on the line: nothing
		auto k = DriftCorrection(c, { 9.65f, 0.3f }, free, 0.08f, 0.04f);
		CHECK(!k.needed && Near(k.perp, 0.0f));
		// within the tolerance: nothing
		k = DriftCorrection(c, { 9.58f, 0.0f }, free, 0.08f, 0.04f);
		CHECK(!k.needed);
		// 0.2 m too far from the wall (x = 9.45): moved toward it, capped to the step
		k = DriftCorrection(c, { 9.45f, 0.0f }, free, 0.08f, 0.04f);
		CHECK(k.needed && Near(k.perp, 0.2f) && Near(k.delta.x, 0.04f) && Near(k.delta.y, 0.0f));
		// a smaller drift than the step: the whole of it
		k = DriftCorrection(c, { 9.55f, 0.0f }, free, 0.08f, 0.2f);
		CHECK(k.needed && Near(k.delta.x, 0.10f));
		// into the wall: moved back out
		k = DriftCorrection(c, { 9.85f, 0.0f }, free, 0.08f, 0.5f);
		CHECK(k.needed && Near(k.delta.x, -0.20f));
		// past an end: reported, not corrected (the braking zone keeps the player inside; nothing is pushed along the wall)
		MoveLimits l;
		l.remainPlus = -0.2f;
		k = DriftCorrection(c, { 9.65f, -2.0f }, l, 0.08f, 0.5f);
		CHECK(!k.needed && Near(k.overshoot, 0.2f));
		k = DriftCorrection(c, { 9.45f, -2.0f }, l, 0.08f, 0.05f);
		CHECK(k.needed && Near(k.delta.x, 0.05f) && Near(k.delta.y, 0.0f));
	}
}

int RunMovementTests()
{
	g_failures = 0;
	TestConventions();
	TestStickAlongTheWall();
	TestCap();
	TestStickCameraNotSquare();
	TestBrake();
	TestStopDistance();
	TestTrackDecision();
	TestAlongMin();
	TestAway();
	TestLimitsFor();
	TestCorrection();
	std::printf("movement tests: %s\n", g_failures == 0 ? "ok" : "FAILED");
	return g_failures;
}
