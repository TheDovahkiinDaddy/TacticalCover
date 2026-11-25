// Engine-free tests for Core/CoverSensor (T2.2), against a fake probe that models boxes and answers a frame late.

#include "Core/CoverSensor.h"

#include <cmath>
#include <cstdio>

#include "fake_world.h"

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

	bool Near(const float a, const float b, const float eps) { return std::fabs(a - b) <= eps; }

	// Ticks the probe and updates the sensor until it has a result (at most 100 frames).
	SensorResult Run(CoverSensor& a_sensor, Fake::Probe& a_probe, int* a_frames = nullptr)
	{
		int frames = 0;
		while (!a_sensor.HasResult() && frames < 100) {
			a_probe.Tick();
			a_sensor.Update();
			++frames;
		}
		if (a_frames) {
			*a_frames = frames;
		}
		CHECK(a_sensor.HasResult());
		return a_sensor.TakeResult();
	}

	SensorResult RunFind(Fake::World& a_world, const Vec3& a_feet, const Vec2& a_intent, const SensorSettings& a_cfg = {})
	{
		Fake::Probe  probe(a_world);
		CoverSensor sensor(probe, a_cfg);
		CHECK(sensor.Find(a_feet, a_intent));
		return Run(sensor, probe);
	}

	// ---- worlds ---------------------------------------------------------------------------------------------------
	Fake::World TallWall(const float a_halfLength = 20.0f)
	{
		Fake::World w;
		w.AddFloor();
		w.boxes.push_back(Fake::MakeBox(10.0f, 10.3f, -a_halfLength, a_halfLength, 0.0f, 3.0f));
		return w;
	}

	Fake::World Crate(const float a_height)
	{
		Fake::World w;
		w.AddFloor();
		w.boxes.push_back(Fake::MakeBox(10.0f, 11.0f, -1.0f, 1.0f, 0.0f, a_height));
		return w;
	}

	// The wall's near face is at x = 10. The player at x = 8.5 faces +x. Facing the wall, the normal is (-1, 0), the tangent (0, -1)
	// (the player's right is -y), so "left" is +y.
	void TestTallWall()
	{
		auto        w = TallWall();
		int         frames = 0;
		Fake::Probe probe(w);
		CoverSensor sensor(probe);
		CHECK(sensor.Find({ 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }));
		CHECK(sensor.Busy() && !sensor.HasResult());
		CHECK(!sensor.Find({ 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }));  // a second query is refused while one runs
		const auto r = Run(sensor, probe, &frames);
		const auto& c = r.candidate;
		CHECK(!sensor.Busy() && !sensor.HasResult());
		CHECK(c.kind == CoverKind::High && c.why == Reject::None);
		CHECK(c.heightCapped && c.height > 1.6f);
		CHECK(Near(c.normal.x, -1.0f, 0.01f) && Near(c.normal.y, 0.0f, 0.01f));
		CHECK(Near(c.tangent.x, 0.0f, 0.01f) && Near(c.tangent.y, -1.0f, 0.01f));
		CHECK(Near(c.distance, 1.5f, 0.03f));
		CHECK(Near(c.anchor.x, 10.0f, 0.03f) && Near(c.anchor.z, 0.0f, 0.02f));
		CHECK(Near(c.standPoint.x, 10.0f - 0.35f, 0.03f) && Near(c.standPoint.y, 0.0f, 0.05f) && Near(c.standPoint.z, 0.0f, 0.02f));
		CHECK(c.edgeLeftKind == EdgeKind::None && c.edgeRightKind == EdgeKind::None);
		CHECK(Near(c.edgeLeft, 4.0f, 0.2f) && Near(c.edgeRight, 4.0f, 0.2f));
		CHECK(r.casts > 40 && r.casts < 200);
		CHECK(frames >= 4 && frames <= 8);  // Search, Fit, Profile, Edges: one frame each (the fake answers a frame late)
	}

	void TestLowCrate()
	{
		auto w = Crate(0.9f);
		const auto r = RunFind(w, { 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f });
		const auto& c = r.candidate;
		CHECK(c.kind == CoverKind::Low);
		CHECK(!c.heightCapped && Near(c.height, 0.9f, 0.09f));
		CHECK(Near(c.normal.x, -1.0f, 0.01f));
		// the crate is 2 m wide with the player in the middle: both ends about 1 m away, open
		CHECK(c.edgeLeftKind == EdgeKind::Open && c.edgeRightKind == EdgeKind::Open);
		CHECK(Near(c.edgeLeft, 1.0f, 0.15f) && Near(c.edgeRight, 1.0f, 0.15f));
		CHECK(Near(c.standPoint.x, 10.0f - 0.35f, 0.03f));
	}

	void TestHeightBoundaries()
	{
		{
			auto w = Crate(1.2f);
			CHECK(RunFind(w, { 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }).candidate.kind == CoverKind::Low);
		}
		{
			// T2.4: fHighCoverMinHeight is 1.70 now (it was 1.40, and this 1.6 m crate was High): the standing player's head is above 1.6 m
			auto w = Crate(1.6f);
			const auto c = RunFind(w, { 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }).candidate;
			CHECK(c.kind == CoverKind::Low && !c.heightCapped && Near(c.height, 1.6f, 0.09f));
		}
		{
			auto w = Crate(1.8f);
			CHECK(RunFind(w, { 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }).candidate.kind == CoverKind::High);
		}
	}

	void TestRejections()
	{
		// too low: 0.55 m. The search ray at 0.5 m still hits it, the height says no.
		{
			auto w = Crate(0.55f);
			const auto c = RunFind(w, { 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }).candidate;
			CHECK(c.kind == CoverKind::None && c.why == Reject::TooLow);
			CHECK(c.height > 0.45f && c.height < 0.65f);  // what was learned stays in the candidate
		}
		// a curb of 0.3 m is below the search ray: nothing there
		{
			auto w = Crate(0.3f);
			CHECK(RunFind(w, { 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }).candidate.why == Reject::NoSurface);
		}
		// open ground
		{
			Fake::World w;
			w.AddFloor();
			const auto c = RunFind(w, { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f }).candidate;
			CHECK(c.kind == CoverKind::None && c.why == Reject::NoSurface);
		}
		// beyond the snap distance (the wall is 1.5 m away, the limit 1.0 m)
		{
			auto w = TallWall();
			SensorSettings cfg;
			cfg.maxSnapDistance = 1.0f;
			CHECK(RunFind(w, { 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }, cfg).candidate.why == Reject::NoSurface);
		}
		// facing away from the wall: the fan does not reach it
		{
			auto w = TallWall();
			CHECK(RunFind(w, { 8.5f, 0.0f, 0.0f }, { -1.0f, 0.0f }).candidate.why == Reject::NoSurface);
		}
		// a thin pole, 0.2 m square and 2 m tall
		{
			Fake::World w;
			w.AddFloor();
			w.boxes.push_back(Fake::MakeBox(10.0f, 10.2f, -0.1f, 0.1f, 0.0f, 2.0f));
			const auto c = RunFind(w, { 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }).candidate;
			CHECK(c.kind == CoverKind::None && (c.why == Reject::TooNarrow || c.why == Reject::TooSmallToFit));
		}
		// a sign hanging over the player with nothing at the bottom: the lowest height cast misses
		{
			Fake::World w;
			w.AddFloor();
			w.boxes.push_back(Fake::MakeBox(10.0f, 10.3f, -20.0f, 20.0f, 0.55f, 3.0f));
			CHECK(RunFind(w, { 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }).candidate.why == Reject::NoBase);
		}
		// the floor ends before the stand point (a ledge)
		{
			auto w = TallWall();
			w.boxes[0] = Fake::MakeBox(-200.0f, 9.5f, -200.0f, 200.0f, -1.0f, 0.0f);
			CHECK(RunFind(w, { 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }).candidate.why == Reject::NoFloor);
		}
		// a floor 0.6 m higher at the stand point than under the player
		{
			auto w = TallWall();
			w.boxes.push_back(Fake::MakeBox(9.0f, 12.0f, -50.0f, 50.0f, 0.0f, 0.6f));
			const auto c = RunFind(w, { 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }).candidate;
			CHECK(c.kind == CoverKind::None);  // the raised floor shortens every height cast or moves the floor: either way no cover
		}
		// something hangs in the way at chest height
		{
			auto w = TallWall();
			w.boxes.push_back(Fake::MakeBox(8.6f, 8.8f, -0.3f, 0.3f, 0.9f, 1.2f));
			const auto c = RunFind({ w }, { 8.0f, 0.0f, 0.0f }, { 1.0f, 0.0f }).candidate;
			CHECK(c.kind == CoverKind::None && c.why == Reject::PathBlocked);
		}
		// no intent
		{
			auto w = TallWall();
			Fake::Probe  probe(w);
			CoverSensor sensor(probe);
			CHECK(sensor.Find({ 8.5f, 0.0f, 0.0f }, { 0.0f, 0.0f }));
			CHECK(sensor.HasResult());
			CHECK(sensor.TakeResult().candidate.why == Reject::BadInput);
		}
	}

	void TestProbeUnavailable()
	{
		auto        w = TallWall();
		Fake::Probe probe(w);
		probe.available = false;
		CoverSensor sensor(probe);
		CHECK(sensor.Find({ 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }));
		CHECK(Run(sensor, probe).candidate.why == Reject::ProbeUnavailable);
		probe.available = true;
		probe.refuseSubmit = true;
		CHECK(sensor.Find({ 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }));
		CHECK(Run(sensor, probe).candidate.why == Reject::ProbeUnavailable);
		// a Track on an unavailable probe says so
		probe.refuseSubmit = false;
		const auto good = RunFind(w, { 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }).candidate;
		probe.available = false;
		CHECK(sensor.Track(good, { 8.5f, 1.0f, 0.0f }));
		const auto t = Run(sensor, probe);
		CHECK(t.isTrack && t.status == TrackStatus::Unavailable);
		CHECK(Near(t.candidate.standPoint.x, good.standPoint.x, 1e-4f));  // the old candidate comes back unchanged
	}

	void TestCancelAndSlowProbe()
	{
		auto        w = TallWall();
		Fake::Probe probe(w);
		probe.latencyFrames = 3;
		CoverSensor sensor(probe);
		CHECK(sensor.Find({ 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }));
		int frames = 0;
		const auto r = Run(sensor, probe, &frames);
		CHECK(r.candidate.kind == CoverKind::High);
		CHECK(frames >= 12);  // four phases, three frames of latency each
		CHECK(sensor.Find({ 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }));
		sensor.Update();
		sensor.Cancel();
		CHECK(!sensor.Busy() && !sensor.HasResult());
		CHECK(sensor.Find({ 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }));  // usable again
		CHECK(Run(sensor, probe).candidate.kind == CoverKind::High);
	}

	// A wall turned by 20 degrees: the normal follows it.
	void TestAngledWall()
	{
		Fake::World w;
		w.AddFloor();
		Fake::Box b = Fake::MakeBox(0.0f, 0.3f, -20.0f, 20.0f, 0.0f, 3.0f);
		b.cx = 10.0f;
		b.cy = 0.0f;
		b.hx = 0.15f;
		b.hy = 20.0f;
		b.angleDeg = 20.0f;  // the box's x axis (its thickness) points 20 degrees counter-clockwise from +x
		w.boxes.push_back(b);
		const auto c = RunFind(w, { 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }).candidate;
		CHECK(c.kind == CoverKind::High);
		const Vec2 expected{ -std::cos(20.0f * 0.017453292f), -std::sin(20.0f * 0.017453292f) };
		CHECK(AngleBetweenDeg(c.normal, expected) < 1.5f);
		CHECK(Near(c.fitRms, 0.0f, 0.01f));
	}

	// An inside corner at the left end of the player's span: a second wall runs toward the player at y = 1.5.
	void TestInsideCorner()
	{
		auto w = TallWall();
		w.boxes.push_back(Fake::MakeBox(4.0f, 10.3f, 1.5f, 1.8f, 0.0f, 3.0f));
		const auto c = RunFind(w, { 8.0f, 0.0f, 0.0f }, { 1.0f, 0.0f }).candidate;
		CHECK(c.kind == CoverKind::High);
		CHECK(c.edgeLeftKind == EdgeKind::Blocked && c.edgeLeft > 1.1f && c.edgeLeft < 1.6f);  // left is +y here
		CHECK(c.edgeRightKind == EdgeKind::None);
	}

	// A wall piece that ends: the edge is found on that side only.
	void TestWallEnd()
	{
		Fake::World w;
		w.AddFloor();
		w.boxes.push_back(Fake::MakeBox(10.0f, 10.3f, -3.0f, 1.0f, 0.0f, 3.0f));  // y from -3 to 1
		const auto c = RunFind(w, { 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }).candidate;
		CHECK(c.kind == CoverKind::High);
		CHECK(c.edgeLeftKind == EdgeKind::Open && Near(c.edgeLeft, 1.0f, 0.15f));  // left (+y) ends 1 m away
		CHECK(c.edgeRightKind == EdgeKind::Open && Near(c.edgeRight, 3.0f, 0.15f));  // right (-y) ends 3 m away: within the 4 m range, so found
	}

	void TestTrack()
	{
		Fake::World w;
		w.AddFloor();
		w.boxes.push_back(Fake::MakeBox(10.0f, 10.3f, -3.0f, 3.0f, 0.0f, 3.0f));
		Fake::Probe  probe(w);
		CoverSensor sensor(probe);
		CHECK(sensor.Find({ 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }));
		const auto found = Run(sensor, probe).candidate;
		CHECK(found.kind == CoverKind::High);
		CHECK(found.edgeLeftKind == EdgeKind::Open && found.edgeRightKind == EdgeKind::Open);

		// the player stands at the stand point and slides 2 m to the left (+y): Track keeps the candidate and moves the edges
		CHECK(sensor.Track(found, { found.standPoint.x, 2.0f, 0.0f }));
		CHECK(sensor.Busy());
		auto t = Run(sensor, probe);
		CHECK(t.isTrack && t.status == TrackStatus::Ok);
		CHECK(Near(t.candidate.normal.x, -1.0f, 0.01f) && Near(t.candidate.standPoint.y, 2.0f, 0.05f));
		CHECK(t.candidate.edgeLeftKind == EdgeKind::Open && Near(t.candidate.edgeLeft, 1.0f, 0.15f));  // the +y end is 1 m away
		CHECK(t.candidate.edgeRightKind == EdgeKind::None && Near(t.candidate.edgeRight, 2.0f, 0.2f));  // the other is beyond the track range
		CHECK(t.candidate.kind == CoverKind::High);
		CHECK(t.casts < 80);
		CHECK(Near(t.turnDeg, 0.0f, 1.0f));

		// past the end of the wall: the cover is lost
		CHECK(sensor.Track(found, { found.standPoint.x, 4.5f, 0.0f }));
		t = Run(sensor, probe);
		CHECK(t.status == TrackStatus::Lost);
		CHECK(Near(t.candidate.standPoint.y, found.standPoint.y, 1e-4f));  // the old candidate is returned

		// the wall disappeared altogether
		Fake::World empty;
		empty.AddFloor();
		probe.SetWorld(empty);
		CHECK(sensor.Track(found, { found.standPoint.x, 0.0f, 0.0f }));
		CHECK(Run(sensor, probe).status == TrackStatus::Lost);
		probe.SetWorld(w);

		// the floor ends under the stand point: lost
		Fake::World ledge = w;
		ledge.boxes[0] = Fake::MakeBox(-200.0f, 9.5f, -200.0f, 200.0f, -1.0f, 0.0f);
		probe.SetWorld(ledge);
		CHECK(sensor.Track(found, { found.standPoint.x, 0.0f, 0.0f }));
		CHECK(Run(sensor, probe).status == TrackStatus::Lost);
		probe.SetWorld(w);

		// a Track on a rejected candidate is lost at once
		CoverCandidate none;
		CHECK(sensor.Track(none, { 0, 0, 0 }));
		CHECK(sensor.HasResult() && sensor.TakeResult().status == TrackStatus::Lost);
	}

	// The wall turns: the same place, but the wall now stands at 50 degrees: a corner.
	// Phase 3 (R5): the player walks along a low wall toward its open end, Track after Track from the previous result, as the engine does
	void TestTrackToTheEnd()
	{
		for (const float height : { 0.74f, 2.0f }) {
			Fake::World w;
			w.AddFloor();
			w.boxes.push_back(Fake::MakeBox(10.0f, 10.3f, -3.0f, 3.0f, 0.0f, height));  // the +y end is at y = 3.0
			Fake::Probe  probe(w);
			CoverSensor sensor(probe);
			CHECK(sensor.Find({ 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }));
			auto current = Run(sensor, probe).candidate;
			CHECK(current.kind != CoverKind::None);
			bool  lostEarly = false;
			float lastY = 0.0f;
			for (float y = 0.25f; y <= 2.8f; y += 0.25f) {
				CHECK(sensor.Track(current, { current.standPoint.x, y, 0.0f }));
				const auto t = Run(sensor, probe);
				if (t.status != TrackStatus::Ok) {
					std::printf("  track at y=%.2f (end at 3.00, height %.2f): %s\n", y, height, ToString(t.status));
					lostEarly = true;
					break;
				}
				lastY = y;
				current = t.candidate;
			}
			CHECK(!lostEarly);  // the end is 0.2 m or more away from every position tried: the cover must still be tracked
			CHECK(lastY >= 2.7f);
			// the edge toward the end is found and its distance is right
			CHECK(current.edgeLeftKind == EdgeKind::Open);
		}
	}

	void TestTrackCorner()
	{
		auto w = TallWall();
		Fake::Probe  probe(w);
		CoverSensor sensor(probe);
		CHECK(sensor.Find({ 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }));
		const auto found = Run(sensor, probe).candidate;
		CHECK(found.kind == CoverKind::High);

		Fake::World turned;
		turned.AddFloor();
		// a wall whose near face still passes through (10, 0), turned by the angle: the box centre sits half a thickness behind the face
		const auto turnedBox = [](const float a_deg) {
			Fake::Box b;
			const float rad = a_deg * 0.017453292f;
			b.cx = 10.0f + 0.15f * std::cos(rad);
			b.cy = 0.15f * std::sin(rad);
			b.hx = 0.15f;
			b.hy = 20.0f;
			b.z0 = 0.0f;
			b.z1 = 3.0f;
			b.angleDeg = a_deg;
			return b;
		};
		turned.boxes.push_back(turnedBox(50.0f));  // much sharper and the casts no longer find "the wall" where it was: Lost, which also stops movement
		probe.SetWorld(turned);
		CHECK(sensor.Track(found, { found.standPoint.x, found.standPoint.y, 0.0f }));
		const auto t = Run(sensor, probe);
		CHECK(t.status == TrackStatus::Corner);
		CHECK(t.turnDeg > 45.0f);
		CHECK(Near(t.candidate.normal.x, found.normal.x, 1e-4f));  // the candidate stays as it was

		// a turn of 20 degrees is followed
		turned.boxes[1] = turnedBox(20.0f);
		CHECK(sensor.Track(found, { found.standPoint.x, found.standPoint.y, 0.0f }));
		const auto t2 = Run(sensor, probe);
		CHECK(t2.status == TrackStatus::Ok);
		CHECK(Near(t2.turnDeg, 20.0f, 2.0f));
	}

	// ---- T2.4 item 2: the wall is not lost at its end ---------------------------------------------------------------------
	// The wall's near face is x = 10 and it runs from y = -5 up to a_endY (the end is on the +y side, which is the player's left when facing +x).
	Fake::World WallEndingAt(const float a_endY)
	{
		Fake::World w;
		w.AddFloor();
		w.boxes.push_back(Fake::MakeBox(10.0f, 10.3f, -5.0f, a_endY, 0.0f, 3.0f));
		return w;
	}

	// case #86 of the play check: the player 0.35 m from the wall, the view 30 degrees off the wall's normal, the wall's end 0.2 m beyond where the
	// centre ray hits it
	void TestWallEndBesideTheHit()
	{
		for (const float side : { 1.0f, -1.0f }) {
			// mirror: for side -1 the wall runs the other way and the view turns the other way
			Fake::World w;
			w.AddFloor();
			const float y0 = 0.144f;  // where a ray 30 degrees off hits the line of centres from 0.25 m away
			if (side > 0) {
				w.boxes.push_back(Fake::MakeBox(10.0f, 10.3f, -5.0f, y0 + 0.2f, 0.0f, 3.0f));
			} else {
				w.boxes.push_back(Fake::MakeBox(10.0f, 10.3f, -y0 - 0.2f, 5.0f, 0.0f, 3.0f));
			}
			const Vec2 intent{ std::cos(30.0f * 0.017453292f), side * std::sin(30.0f * 0.017453292f) };
			const auto r = RunFind(w, { 9.65f, 0.0f, 0.0f }, intent);
			const auto& c = r.candidate;
			CHECK(c.kind == CoverKind::High);
			CHECK(Near(c.normal.x, -1.0f, 0.02f));
			// the end is on the side of +y (left when facing +x) for side +1, on -y (right) for side -1; the foot is 0.344 m from it
			const auto  nearKind = side > 0 ? c.edgeLeftKind : c.edgeRightKind;
			const float nearDist = side > 0 ? c.edgeLeft : c.edgeRight;
			CHECK(nearKind == EdgeKind::Open);
			CHECK(Near(nearDist, 0.344f, 0.15f));
		}
	}

	// the player 0.35 m from the wall (surface), a_beyond metres beyond the wall's end, the view straight at the line
	SensorResult FindBeyondTheEnd(const float a_beyond, const SensorSettings& a_cfg = {})
	{
		auto w = WallEndingAt(-a_beyond);
		return RunFind(w, { 9.65f, 0.0f, 0.0f }, { 1.0f, 0.0f }, a_cfg);
	}

	void TestBeyondTheEnd()
	{
		const SensorSettings cfg;
		// 0.1 m beyond the end: a candidate, the stand point fEdgeMargin inside the end
		{
			const auto c = FindBeyondTheEnd(0.1f).candidate;
			CHECK(c.kind == CoverKind::High);
			CHECK(Near(c.standPoint.y, -0.1f - cfg.edgeMargin, 0.15f));
			CHECK(c.edgeLeftKind == EdgeKind::Open && Near(c.edgeLeft, cfg.edgeMargin, 0.15f));
		}
		// 0.35 m beyond: the farthest the +-50 degree fan reaches from 0.35 m away (0.25 m from the line of centres: 0.25 x tan 50 = 0.30 m, and
		// the line of centres ends 0.1 m inside the surface end)
		{
			const auto c = FindBeyondTheEnd(0.35f).candidate;
			CHECK(c.kind == CoverKind::High);
			CHECK(Near(c.standPoint.y, -0.35f - cfg.edgeMargin, 0.15f));
		}
		// 0.5 m beyond, as the plan has it: not a candidate. The fan's outer ray reaches only 0.40 m beyond the end from 0.35 m away (0.25 x tan 50,
		// plus the 0.1 m the line of centres ends inside the surface); a wider fan (60 degrees) reaches the wall's end face instead of its front
		// and finds a 0.5 m wide thing: too narrow. Either way nothing is offered at 0.83 m from the player.
		{
			CHECK(FindBeyondTheEnd(0.5f).candidate.kind == CoverKind::None);
			SensorSettings wide;
			wide.searchHalfAngleDeg = 60.0f;
			CHECK(FindBeyondTheEnd(0.5f, wide).candidate.kind == CoverKind::None);
		}
		// 1.5 m beyond: nothing
		CHECK(FindBeyondTheEnd(1.5f).candidate.kind == CoverKind::None);
	}

	// ---- T2.4 item 3: a wall to the side is taken only when it is near -----------------------------------------------------------
	void TestSidePicks()
	{
		const auto longWall = [](const float a_y) {
			Fake::World w;
			w.AddFloor();
			w.boxes.push_back(Fake::MakeBox(-5.0f, 5.0f, a_y, a_y + 0.3f, 0.0f, 3.0f));  // parallel to the view (+x)
			return w;
		};
		auto near = longWall(0.8f);
		const auto n = RunFind(near, { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f }).candidate;
		CHECK(n.kind == CoverKind::High && Near(n.wallPointAngleDeg, 90.0f, 5.0f) && Near(n.wallPointDistance, 0.8f, 0.05f));
		auto far = longWall(1.5f);
		const auto f = RunFind(far, { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f }).candidate;
		CHECK(f.kind == CoverKind::None && f.why == Reject::TooFarToTheSide);
		// a wall straight ahead at 2.0 m is taken
		Fake::World ahead;
		ahead.AddFloor();
		ahead.boxes.push_back(Fake::MakeBox(2.0f, 2.3f, -5.0f, 5.0f, 0.0f, 3.0f));
		const auto a = RunFind(ahead, { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f }).candidate;
		CHECK(a.kind == CoverKind::High && a.wallPointAngleDeg < 5.0f && Near(a.wallPointDistance, 2.0f, 0.05f));
		// the limits are settings
		SensorSettings cfg;
		cfg.sideSnapDistance = 2.0f;
		CHECK(RunFind(far, { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f }, cfg).candidate.kind == CoverKind::High);
		cfg = {};
		cfg.sideSnapAngleDeg = 95.0f;
		CHECK(RunFind(far, { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f }, cfg).candidate.kind == CoverKind::High);
	}

	// ---- T3.0: ahead first, one fallback --------------------------------------------------------------------------------------------
	void TestAheadFirst()
	{
		// the view runs along a wall 0.8 m to the side; a wall stands 2 m straight ahead. The old scoring took the side wall (1.04 m at the edge
		// of the fan, scored 1.57, against 2.0); the wall the player pushes toward must win
		Fake::World w;
		w.AddFloor();
		w.boxes.push_back(Fake::MakeBox(-5.0f, 1.9f, 0.8f, 1.1f, 0.0f, 3.0f));  // beside the view (+x), 0.8 m to the left
		w.boxes.push_back(Fake::MakeBox(2.0f, 2.3f, -5.0f, 5.0f, 0.0f, 3.0f));  // straight ahead
		const auto r = RunFind(w, { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f });
		const auto& c = r.candidate;
		CHECK(c.kind == CoverKind::High);
		CHECK(Near(c.normal.x, -1.0f, 0.05f) && Near(c.normal.y, 0.0f, 0.05f));
		CHECK(c.wallPointAngleDeg < 10.0f && Near(c.wallPointDistance, 2.0f, 0.1f));
		CHECK(!c.usedFallback);
		// the same world with the setting at 0 degrees (only the ray straight ahead counts as ahead) still takes the wall ahead; with the old
		// reading (everything is "ahead") the side wall wins again
		SensorSettings cfg;
		cfg.aheadHalfAngleDeg = 60.0f;
		const auto old = RunFind(w, { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f }, cfg).candidate;
		CHECK(old.kind == CoverKind::High && Near(old.wallPointAngleDeg, 90.0f, 8.0f));
		// with nothing ahead the outer rays decide, as before
		Fake::World side;
		side.AddFloor();
		side.boxes.push_back(Fake::MakeBox(-5.0f, 5.0f, 0.8f, 1.1f, 0.0f, 3.0f));
		const auto s = RunFind(side, { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f }).candidate;
		CHECK(s.kind == CoverKind::High && Near(s.wallPointAngleDeg, 90.0f, 5.0f) && !s.usedFallback);
	}

	void TestFallback()
	{
		const auto pole = [](Fake::World& a_w, const float a_x, const float a_y) { a_w.boxes.push_back(Fake::MakeBox(a_x - 0.02f, a_x + 0.02f, a_y - 0.02f, a_y + 0.02f, 0.0f, 3.0f)); };
		// a thin pole straight ahead is rejected (too small to fit); the wall beside the view is taken by the one fallback
		{
			Fake::World w;
			w.AddFloor();
			pole(w, 2.0f, 0.0f);
			w.boxes.push_back(Fake::MakeBox(-5.0f, 5.0f, 0.8f, 1.1f, 0.0f, 3.0f));
			const auto r = RunFind(w, { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f });
			const auto& c = r.candidate;
			CHECK(c.kind == CoverKind::High && c.usedFallback && c.otherWhy == Reject::TooSmallToFit);
			CHECK(Near(c.wallPointAngleDeg, 90.0f, 8.0f) && Near(c.wallPointDistance, 0.8f, 0.1f));
			CHECK(!r.fitPoints.empty());
		}
		// both rejected: the first attempt's rejection is reported, with the second one's reason next to it
		{
			Fake::World w;
			w.AddFloor();
			pole(w, 2.0f, 0.0f);
			pole(w, 1.36f, 0.634f);
			pole(w, 1.36f, -0.634f);
			const auto r = RunFind(w, { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f });
			CHECK(r.candidate.kind == CoverKind::None && r.candidate.why == Reject::TooSmallToFit);
			CHECK(r.candidate.usedFallback && r.candidate.otherWhy == Reject::TooSmallToFit);
		}
		// a rejected surface ahead with nothing beside it: no fallback, the plain rejection
		{
			Fake::World w;
			w.AddFloor();
			pole(w, 2.0f, 0.0f);
			const auto r = RunFind(w, { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f });
			CHECK(r.candidate.kind == CoverKind::None && r.candidate.why == Reject::TooSmallToFit && !r.candidate.usedFallback);
		}
		// a rejection that is not one of the four (too low) does not fall back: a 0.55 m kerb ahead, a tall wall beside the view
		{
			Fake::World w;
			w.AddFloor();
			w.boxes.push_back(Fake::MakeBox(2.0f, 2.4f, -5.0f, 5.0f, 0.0f, 0.55f));
			w.boxes.push_back(Fake::MakeBox(-5.0f, 5.0f, 0.8f, 1.1f, 0.0f, 3.0f));
			const auto r = RunFind(w, { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f });
			CHECK(r.candidate.kind == CoverKind::None && !r.candidate.usedFallback);
		}
	}

	// ---- T2.4 item 1: heights ---------------------------------------------------------------------------------------------------
	void TestOverPeek()
	{
		const auto kindAt = [](const float a_top) {
			auto w = Crate(a_top);
			return RunFind(w, { 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }).candidate;
		};
		const auto c110 = kindAt(1.1f);
		CHECK(c110.kind == CoverKind::Low && c110.canPeekOver);
		const auto c145 = kindAt(1.45f);
		CHECK(c145.kind == CoverKind::Low && !c145.canPeekOver);
		const auto c160 = kindAt(1.6f);
		CHECK(c160.kind == CoverKind::Low && !c160.canPeekOver);
		const auto c180 = kindAt(1.8f);
		CHECK(c180.kind == CoverKind::High && !c180.canPeekOver);
		// a top above the highest cast is a lower bound measured from the feet; with the stand floor 0.17 m higher it reads 1.6 m: still High (play check P2 #69)
		{
			auto w = TallWall();
			w.boxes.push_back(Fake::MakeBox(9.0f, 12.0f, -50.0f, 50.0f, 0.0f, 0.17f));
			const auto c = RunFind(w, { 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }).candidate;
			CHECK(c.kind == CoverKind::High && c.heightCapped && c.height < 1.7f && !c.canPeekOver);
		}
	}

	// The sensor reads its thresholds from the settings.
	void TestSettings()
	{
		auto w = Crate(0.9f);
		SensorSettings cfg;
		cfg.lowCoverMinHeight = 1.0f;  // a 0.9 m crate is now too low
		CHECK(RunFind(w, { 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }, cfg).candidate.why == Reject::TooLow);
		cfg = {};
		cfg.highCoverMinHeight = 0.8f;  // and now it is high cover
		CHECK(RunFind(w, { 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }, cfg).candidate.kind == CoverKind::High);
		cfg = {};
		cfg.minCoverWidth = 2.5f;  // the crate is 2 m wide
		CHECK(RunFind(w, { 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }, cfg).candidate.why == Reject::TooNarrow);
		// the angle weight: a wall close to the side of the fan against a crate straight ahead
		{
			Fake::World w2;
			w2.AddFloor();
			w2.boxes.push_back(Fake::MakeBox(2.0f, 2.6f, -1.0f, 1.0f, 0.0f, 0.9f));   // straight ahead (+x), 2.0 m
			w2.boxes.push_back(Fake::MakeBox(-5.0f, 5.0f, 0.8f, 1.1f, 0.0f, 3.0f));   // a long wall to the left, 0.8 m from the player
			SensorSettings c2;
			// T3.0 "ahead first": with the ahead group at its default (20 degrees) the crate the player points at wins; the old contest by weight
			// only exists when every ray counts as ahead (aheadHalfAngleDeg wide)
			const auto crate = RunFind(w2, { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f }, c2).candidate;
			CHECK(crate.kind == CoverKind::Low && crate.normal.x < -0.9f);
			c2.aheadHalfAngleDeg = 60.0f;
			const auto near = RunFind(w2, { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f }, c2).candidate;
			CHECK(near.kind == CoverKind::High && near.normal.y < -0.9f);  // the wall wins with the default weight
			c2.searchAngleWeight = 3.0f;
			const auto ahead = RunFind(w2, { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f }, c2).candidate;
			CHECK(ahead.kind == CoverKind::Low && ahead.normal.x < -0.9f);  // the crate wins when the angle counts more
		}
		cfg = {};
		cfg.capsuleRadius = 0.5f;
		CHECK(Near(RunFind(w, { 8.5f, 0.0f, 0.0f }, { 1.0f, 0.0f }, cfg).candidate.standPoint.x, 10.0f - 0.55f, 0.03f));
	}
}

int RunSensorTests()
{
	g_failures = 0;
	TestTallWall();
	TestLowCrate();
	TestHeightBoundaries();
	TestRejections();
	TestProbeUnavailable();
	TestCancelAndSlowProbe();
	TestAngledWall();
	TestInsideCorner();
	TestWallEnd();
	TestTrack();
	TestTrackToTheEnd();
	TestTrackCorner();
	TestWallEndBesideTheHit();
	TestBeyondTheEnd();
	TestSidePicks();
	TestAheadFirst();
	TestFallback();
	TestOverPeek();
	TestSettings();
	std::printf("sensor tests: %s\n", g_failures == 0 ? "ok" : "FAILED");
	return g_failures;
}
