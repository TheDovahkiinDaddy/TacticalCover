// Engine-free tests for the automatic cover entry (M1b, iCoverTrigger 3): the timer, the re-arm, the angle, the tall-edge rule.

#include "Core/AutoCover.h"

#include <cstdio>
#include <string>

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

	AutoCoverInput Into(const double a_now, const CoverKind a_kind = CoverKind::Low)
	{
		AutoCoverInput i;
		i.now = a_now;
		i.state = CoverState::None;
		i.allowed = true;
		i.moveMagnitude = 1.0f;
		i.candidateFresh = true;
		i.kind = a_kind;
		i.angleDeg = 5.0f;
		i.standDistance = 0.4f;
		i.openEndDistance = 0.8f;
		i.weaponDrawn = false;  // the tests of the old times; the drawn weapon has its own below
		return i;
	}

	void TestAngle()
	{
		// the wall normal points from the wall to the player: moving along -normal goes into the wall
		CHECK(AngleToWallDeg({ 1.0f, 0.0f }, { -1.0f, 0.0f }) < 0.01f);
		CHECK(AngleToWallDeg({ -1.0f, 0.0f }, { -1.0f, 0.0f }) > 179.9f);
		const float a = AngleToWallDeg({ 0.0f, 1.0f }, { -1.0f, 0.0f });  // along the wall
		CHECK(a > 89.9f && a < 90.1f);
		const float b = AngleToWallDeg({ 0.8192f, 0.5736f }, { -1.0f, 0.0f });  // 35 degrees off
		CHECK(b > 34.9f && b < 35.1f);
		CHECK(AngleToWallDeg({ 0.0f, 0.0f }, { -1.0f, 0.0f }) > 179.9f);  // no input

		CoverCandidate c;
		CHECK(NearestOpenEnd(c) > 1e8f);
		c.edgeLeft = 3.0f;
		c.edgeLeftKind = EdgeKind::Open;
		c.edgeRight = 1.0f;
		c.edgeRightKind = EdgeKind::Blocked;  // a closed end does not count
		CHECK(NearestOpenEnd(c) == 3.0f);
		c.edgeRightKind = EdgeKind::Open;
		CHECK(NearestOpenEnd(c) == 1.0f);
	}

	void TestTimer()
	{
		AutoCover        a;
		const AutoCoverSettings s;  // time 0.30
		auto r = a.Update(s, Into(10.0));
		CHECK(r.into && !r.enter);
		r = a.Update(s, Into(10.2));
		CHECK(r.into && !r.enter);
		r = a.Update(s, Into(10.29));
		CHECK(!r.enter);
		r = a.Update(s, Into(10.31));
		CHECK(r.enter && r.heldFor >= 0.30f && r.heldFor < 0.32f);
		// the state leaves None: no second entry
		auto i = Into(10.4);
		i.state = CoverState::Entering;
		CHECK(!a.Update(s, i).enter);
	}

	void TestBreakResetsTimer()
	{
		AutoCover        a;
		const AutoCoverSettings s;
		(void)a.Update(s, Into(1.0));
		(void)a.Update(s, Into(1.2));
		auto i = Into(1.25);
		i.moveMagnitude = 0.1f;  // the stick let go for a moment
		CHECK(!a.Update(s, i).into);
		auto r = a.Update(s, Into(1.3));
		CHECK(r.into && !r.enter);
		r = a.Update(s, Into(1.55));
		CHECK(!r.enter);  // 0.25 s since the break
		r = a.Update(s, Into(1.61));
		CHECK(r.enter);
	}

	void TestEachCondition()
	{
		const AutoCoverSettings s;
		const auto holds = [&](AutoCoverInput a_bad) {
			AutoCover a;
			(void)a.Update(s, a_bad);
			a_bad.now += 0.5;
			return !a.Update(s, a_bad).enter;
		};
		auto i = Into(0.0);
		i.allowed = false;
		CHECK(holds(i));
		i = Into(0.0);
		i.sprinting = true;
		CHECK(holds(i));
		i = Into(0.0);
		i.moveMagnitude = 0.39f;
		CHECK(holds(i));
		i = Into(0.0);
		i.candidateFresh = false;
		CHECK(holds(i));
		i = Into(0.0);
		i.kind = CoverKind::None;
		CHECK(holds(i));
		i = Into(0.0);
		i.angleDeg = 36.0f;  // along the wall at a flat angle
		CHECK(holds(i));
		i = Into(0.0);
		i.standDistance = 0.61f;
		CHECK(holds(i));
		// the same input with every value just inside passes
		AutoCover a;
		i = Into(0.0);
		i.angleDeg = 35.0f;
		i.standDistance = 0.60f;
		i.moveMagnitude = 0.4f;
		(void)a.Update(s, i);
		i.now = 0.31;
		CHECK(a.Update(s, i).enter);
	}

	void TestTall()
	{
		AutoCoverSettings s;  // tallEdge 1.2
		AutoCover         a;
		auto              i = Into(0.0, CoverKind::High);
		i.openEndDistance = 5.0f;  // a wall in the middle of a corridor
		(void)a.Update(s, i);
		i.now = 1.0;
		CHECK(!a.Update(s, i).enter);
		a.Reset();
		i = Into(2.0, CoverKind::High);
		i.openEndDistance = 1.2f;  // near an open end
		(void)a.Update(s, i);
		i.now = 2.31;
		CHECK(a.Update(s, i).enter);
		// 0 = tall cover never by itself
		s.tallEdge = 0.0f;
		a.Reset();
		i = Into(3.0, CoverKind::High);
		i.openEndDistance = 0.1f;
		(void)a.Update(s, i);
		i.now = 4.0;
		CHECK(!a.Update(s, i).enter);
		// iAutoCoverTall 0: never, whatever the end
		s.tallEdge = 1.2f;
		s.tallMode = 0;
		a.Reset();
		i = Into(2.5, CoverKind::High);
		i.openEndDistance = 0.1f;
		(void)a.Update(s, i);
		i.now = 4.0;
		CHECK(!a.Update(s, i).enter);
		// iAutoCoverTall 2: anywhere with the weapon drawn, after fAutoCoverTimeDrawn (0.05 s since card M4b)
		s.tallMode = 2;
		a.Reset();
		i = Into(6.0, CoverKind::High);
		i.openEndDistance = 1e9f;
		i.weaponDrawn = true;
		(void)a.Update(s, i);
		i.now = 6.03;
		CHECK(!a.Update(s, i).enter);
		i.now = 6.06;
		CHECK(a.Update(s, i).enter);
		// ... not with the weapon sheathed, however long the input is held
		a.Reset();
		i = Into(8.0, CoverKind::High);
		i.openEndDistance = 1e9f;
		i.weaponDrawn = false;
		(void)a.Update(s, i);
		i.now = 9.5;
		CHECK(!a.Update(s, i).enter);
		CHECK(std::string(a.Update(s, i).why).find("weapon") != std::string::npos);
		// ... and a low wall still takes the short time with the weapon sheathed
		a.Reset();
		i = Into(10.0, CoverKind::Low);
		i.weaponDrawn = false;
		(void)a.Update(s, i);
		i.now = 10.31;
		CHECK(a.Update(s, i).enter);
		s.tallMode = 1;
		// low cover does not care about the ends
		a.Reset();
		i = Into(5.0, CoverKind::Low);
		i.openEndDistance = 1e9f;
		(void)a.Update(s, i);
		i.now = 5.31;
		CHECK(a.Update(s, i).enter);
	}

	void TestDrawn()
	{
		// card M4b: with the weapon drawn low and tall cover are taken after fAutoCoverTimeDrawn; with it away the old 0.30 s stays
		AutoCoverSettings s;
		for (const auto kind : { CoverKind::Low, CoverKind::High }) {
			AutoCover a;
			auto      i = Into(0.0, kind);
			i.weaponDrawn = true;
			i.openEndDistance = 1.0f;
			auto r = a.Update(s, i);
			CHECK(r.into && !r.enter);  // the first tick only starts the clock
			i.now = 0.04;
			CHECK(!a.Update(s, i).enter);
			i.now = 0.06;
			r = a.Update(s, i);
			CHECK(r.enter && r.heldFor >= 0.05f && r.heldFor < 0.07f);
		}
		// zero: the first tick that meets the conditions enters (the conditions include the fresh sensor answer)
		s.timeDrawn = 0.0f;
		{
			AutoCover a;
			auto      i = Into(0.0);
			i.weaponDrawn = true;
			CHECK(a.Update(s, i).enter);
		}
		s.timeDrawn = 0.05f;
		// the weapon away: low cover 0.30 s as before (the tall cover of mode 1 too)
		{
			AutoCover a;
			auto      i = Into(1.0);
			i.weaponDrawn = false;
			(void)a.Update(s, i);
			i.now = 1.2;
			CHECK(!a.Update(s, i).enter);
			i.now = 1.31;
			CHECK(a.Update(s, i).enter);
			s.tallMode = 1;
			a.Reset();
			i = Into(2.0, CoverKind::High);
			i.openEndDistance = 1.0f;
			(void)a.Update(s, i);
			i.now = 2.2;
			CHECK(!a.Update(s, i).enter);
			i.now = 2.31;
			CHECK(a.Update(s, i).enter);
			s.tallMode = 2;
		}
		// a short entry is no false entry: every other condition still holds the entry back, and the re-arm is unchanged
		{
			AutoCover a;
			auto      i = Into(0.0);
			i.weaponDrawn = true;
			i.angleDeg = 60.0f;  // walking along the wall, not into it
			(void)a.Update(s, i);
			i.now = 3.0;
			CHECK(!a.Update(s, i).enter);
			i = Into(4.0);
			i.weaponDrawn = true;
			i.standDistance = 0.9f;  // the stand point is too far
			(void)a.Update(s, i);
			i.now = 6.0;
			CHECK(!a.Update(s, i).enter);
			i = Into(7.0);
			i.weaponDrawn = true;
			i.moveMagnitude = 0.2f;
			(void)a.Update(s, i);
			i.now = 8.0;
			CHECK(!a.Update(s, i).enter);
			i = Into(9.0);
			i.weaponDrawn = true;
			i.sprinting = true;
			(void)a.Update(s, i);
			i.now = 10.0;
			CHECK(!a.Update(s, i).enter);
			// after an exit: no re-entry while the pull is still on, then the re-arm of 0.30 s, then the short time
			i = Into(11.0);
			i.weaponDrawn = true;
			i.state = CoverState::Exiting;
			(void)a.Update(s, i);
			i = Into(11.1);
			i.weaponDrawn = true;
			(void)a.Update(s, i);
			i.now = 13.0;
			CHECK(!a.Update(s, i).enter);
			auto off = Into(14.0);
			off.weaponDrawn = true;
			off.moveMagnitude = 0.0f;
			(void)a.Update(s, off);
			off.now = 14.31;
			(void)a.Update(s, off);
			CHECK(a.Armed());
			i = Into(14.4);
			i.weaponDrawn = true;
			(void)a.Update(s, i);
			i.now = 14.46;
			CHECK(a.Update(s, i).enter);
		}
	}

	void TestRearm()
	{
		AutoCoverSettings s;  // rearm 0.30
		AutoCover         a;
		(void)a.Update(s, Into(0.0));
		CHECK(a.Update(s, Into(0.31)).enter);
		// in cover, then out again (B pressed while still pushing toward the cover)
		auto st = Into(1.0);
		st.state = CoverState::InCover;
		(void)a.Update(s, st);
		st.state = CoverState::Exiting;
		st.now = 2.0;
		(void)a.Update(s, st);
		CHECK(!a.Armed());
		// the state is None again with the stick still pushing: nothing, however long
		auto r = a.Update(s, Into(2.1));
		CHECK(r.into && !r.enter);
		r = a.Update(s, Into(5.0));
		CHECK(r.into && !r.enter && !a.Armed());
		// the pull is off for 0.29 s: still not armed
		auto off = Into(5.1);
		off.moveMagnitude = 0.0f;
		(void)a.Update(s, off);
		off.now = 5.39;
		(void)a.Update(s, off);
		CHECK(!a.Armed());
		off.now = 5.41;
		(void)a.Update(s, off);
		CHECK(a.Armed());
		// pushing again: a new automatic entry after the usual time
		r = a.Update(s, Into(5.5));
		CHECK(r.into && !r.enter);
		r = a.Update(s, Into(5.81));
		CHECK(r.enter);
		// a pull that comes back before the re-arm time restarts the off-clock
		a.Reset();
		st = Into(10.0);
		st.state = CoverState::Exiting;
		(void)a.Update(s, st);
		off = Into(10.1);
		off.moveMagnitude = 0.0f;
		(void)a.Update(s, off);
		(void)a.Update(s, Into(10.3));  // pushing again after 0.2 s
		off.now = 10.5;
		(void)a.Update(s, off);
		off.now = 10.7;
		(void)a.Update(s, off);
		CHECK(!a.Armed());  // only 0.2 s off since 10.5
		off.now = 10.81;
		(void)a.Update(s, off);
		CHECK(a.Armed());
	}
}

int RunAutoCoverTests()
{
	g_failures = 0;
	TestAngle();
	TestTimer();
	TestBreakResetsTimer();
	TestEachCondition();
	TestTall();
	TestDrawn();
	TestRearm();
	std::printf("autocover tests: %s\n", g_failures == 0 ? "ok" : "FAILED");
	return g_failures;
}
