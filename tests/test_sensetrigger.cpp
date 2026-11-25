// Engine-free tests for Core/SenseTrigger: when the plugin senses cover by itself.

#include "Core/SenseTrigger.h"

#include <cmath>
#include <cstdio>
#include <vector>

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

	constexpr float kDeg = 0.017453292f;

	// Drives the trigger at 100 ticks a second along a scripted path and counts the triggers.
	struct Driver
	{
		SenseTrigger trigger;
		double       now{ 0.0 };
		Vec2         pos{};
		float        yaw{ 0.0f };
		int          stopped{ 0 }, turned{ 0 };
		bool         busy{ false };

		void Step(const double a_dt = 0.01)
		{
			now += a_dt;
			const auto r = trigger.Update(now, pos, yaw, busy);
			if (r == SenseReason::Stopped) {
				++stopped;
			} else if (r == SenseReason::Turned) {
				++turned;
			}
			if (r != SenseReason::None) {
				trigger.Sensed(now, pos, yaw);  // the caller starts the query
			}
		}
		void Stand(const double a_seconds)
		{
			for (double t = 0; t < a_seconds; t += 0.01) {
				Step();
			}
		}
		// walks a_metres along +x at 2 m/s
		void Walk(const float a_metres)
		{
			const double seconds = static_cast<double>(a_metres) / 2.0;
			for (double t = 0; t < seconds; t += 0.01) {
				pos.x += 0.02f;
				Step();
			}
		}
		void Turn(const float a_deg, const double a_seconds)
		{
			const int n = static_cast<int>(a_seconds / 0.01);
			for (int i = 0; i < n; ++i) {
				yaw += a_deg * kDeg / static_cast<float>(n);
				Step();
			}
		}
	};

	void TestStopped()
	{
		Driver d;
		d.Stand(3.0);  // nothing moved: nothing to sense
		CHECK(d.stopped == 0 && d.turned == 0);
		d.Walk(2.0f);
		CHECK(d.stopped == 0);  // still moving: not before the stop
		d.Stand(0.3);
		CHECK(d.stopped == 0);  // 0.3 s of standing is not enough
		d.Stand(0.2);
		CHECK(d.stopped == 1);  // 0.4 s after the stop
		d.Stand(5.0);
		CHECK(d.stopped == 1 && d.turned == 0);  // standing longer does not repeat it
		d.Walk(1.0f);
		d.Stand(1.0);
		CHECK(d.stopped == 2);  // the next stop
	}

	void TestShortMovesAndPauses()
	{
		Driver d;
		d.Stand(1.0);
		d.Walk(0.3f);  // less than 0.5 m since the last sense (the start)
		d.Stand(2.0);
		CHECK(d.stopped == 0);
		d.Walk(0.3f);  // 0.6 m in all, with a stop in between: the second stop counts
		d.Stand(1.0);
		CHECK(d.stopped == 1);
		// a pause while walking shorter than 0.4 s does not trigger
		Driver e;
		e.Stand(1.0);
		e.Walk(1.0f);
		e.Stand(0.3);
		e.Walk(1.0f);
		e.Stand(0.1);
		CHECK(e.stopped == 0);
		e.Stand(0.5);
		CHECK(e.stopped == 1);
	}

	void TestTurned()
	{
		Driver d;
		d.Stand(1.0);
		d.Turn(20.0f, 0.5);  // under 30 degrees: nothing
		d.Stand(2.0);
		CHECK(d.turned == 0);
		d.Turn(25.0f, 0.5);  // 45 degrees from the start in all, but still turning at the end of the turn
		CHECK(d.turned == 0);
		d.Stand(0.3);
		CHECK(d.turned == 0);
		d.Stand(0.2);
		CHECK(d.turned == 1);
		d.Stand(3.0);
		CHECK(d.turned == 1 && d.stopped == 0);
		// a continuous sweep through 180 degrees triggers once, when it ends
		Driver e;
		e.Stand(1.0);
		e.Turn(180.0f, 2.0);
		CHECK(e.turned == 0);
		e.Stand(0.6);
		CHECK(e.turned == 1);
		// the yaw wraps around: from just under +180 to just over -180 is a small turn
		Driver w;
		w.yaw = 179.0f * kDeg;
		w.Stand(1.0);
		w.Turn(10.0f, 0.3);
		w.Stand(2.0);
		CHECK(w.turned == 0);
	}

	void TestIntervalAndBusy()
	{
		// two walks, each followed by a stop, with a stop that comes less than the minimum interval after the last sense
		SenseTriggerSettings cfg;
		cfg.minInterval = 1.0f;
		Driver d{ SenseTrigger(cfg) };
		d.Stand(0.5);
		d.Walk(0.6f);
		d.Stand(0.45);  // senses at the stop
		CHECK(d.stopped == 1);
		d.Walk(0.6f);   // 0.3 s walking
		d.Stand(0.45);  // 0.4 s after this stop is still under 1 s after the last sense
		CHECK(d.stopped == 1);
		d.Stand(0.5);   // the interval is over: it fires
		CHECK(d.stopped == 2);

		// a trigger that comes while a query runs is not lost
		Driver e;
		e.Stand(0.5);
		e.Walk(1.0f);
		e.busy = true;
		e.Stand(2.0);
		CHECK(e.stopped == 0);
		e.busy = false;
		e.Stand(0.05);
		CHECK(e.stopped == 1);
	}

	void TestResetAndSensed()
	{
		Driver d;
		d.Stand(1.0);
		d.Walk(1.0f);
		d.trigger.Reset();  // e.g. a menu opened: the stop is forgotten
		d.Stand(2.0);
		CHECK(d.stopped == 0);  // the first frame after the reset starts from here
		d.Walk(1.0f);
		d.Stand(1.0);
		CHECK(d.stopped == 1);
		// Sensed() from outside (the key) moves the references: a stop right after it, without new movement, does nothing
		Driver e;
		e.Stand(0.5);
		e.Walk(1.0f);
		e.trigger.Sensed(e.now, e.pos, e.yaw);  // the key was pressed while still walking
		e.Stand(2.0);
		CHECK(e.stopped == 0);
	}
}

int RunSenseTriggerTests()
{
	g_failures = 0;
	TestStopped();
	TestShortMovesAndPauses();
	TestTurned();
	TestIntervalAndBusy();
	TestResetAndSensed();
	std::printf("sense trigger tests: %s\n", g_failures == 0 ? "ok" : "FAILED");
	return g_failures;
}
