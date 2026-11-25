// Engine-free tests for Core/SwapChord: the shoulder swap on LT + A (release 1.0.0, REFERENCE 24).

#include "Core/SwapChord.h"

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

	SwapChord Make(const bool a_on = true)
	{
		SwapChord c;
		c.SetSettings({ a_on, 0.5f, 0.25 });
		return c;
	}
}

int RunSwapChordTests()
{
	g_failures = 0;

	// off: everything passes, the trigger is never "held"
	{
		auto c = Make(false);
		c.OnTrigger(1.0f);
		CHECK(!c.TriggerHeld());
		CHECK(c.OnAButton(true, true, 1.0) == ChordVerdict::Pass);
		CHECK(c.AllowKeySwap(1.0));
	}

	// A alone passes, press, repeat and release
	{
		auto c = Make();
		CHECK(c.OnAButton(true, true, 1.0) == ChordVerdict::Pass);
		CHECK(c.OnAButton(true, false, 1.1) == ChordVerdict::Pass);
		CHECK(c.OnAButton(false, false, 1.2) == ChordVerdict::Pass);
	}

	// LT held, A pressed: one swap, the repeats and the release swallowed, the next A alone passes again
	{
		auto c = Make();
		c.OnTrigger(0.8f);
		CHECK(c.TriggerHeld());
		CHECK(c.OnAButton(true, true, 2.0) == ChordVerdict::SwapAndSwallow);
		CHECK(c.Captured());
		CHECK(c.OnAButton(true, false, 2.1) == ChordVerdict::Swallow);
		CHECK(c.OnAButton(false, false, 2.2) == ChordVerdict::Swallow);
		CHECK(!c.Captured());
		c.OnTrigger(0.0f);
		CHECK(c.OnAButton(true, true, 3.0) == ChordVerdict::Pass);
		CHECK(c.OnAButton(false, false, 3.1) == ChordVerdict::Pass);
	}

	// the trigger let go before A: the release of A is still the chord's (the game never saw its press)
	{
		auto c = Make();
		c.OnTrigger(1.0f);
		CHECK(c.OnAButton(true, true, 4.0) == ChordVerdict::SwapAndSwallow);
		c.OnTrigger(0.0f);
		CHECK(c.OnAButton(false, false, 4.2) == ChordVerdict::Swallow);
		CHECK(!c.Captured());
	}

	// A pressed first, then the trigger: not a chord, the A press passes to its release
	{
		auto c = Make();
		CHECK(c.OnAButton(true, true, 5.0) == ChordVerdict::Pass);
		c.OnTrigger(1.0f);
		CHECK(c.OnAButton(true, false, 5.1) == ChordVerdict::Pass);
		CHECK(c.OnAButton(false, false, 5.2) == ChordVerdict::Pass);
		// a second press with the trigger still held is a chord
		CHECK(c.OnAButton(true, true, 5.5) == ChordVerdict::SwapAndSwallow);
	}

	// the threshold: a light touch of the trigger is not "held"
	{
		auto c = Make();
		c.OnTrigger(0.3f);
		CHECK(!c.TriggerHeld());
		CHECK(c.OnAButton(true, true, 6.0) == ChordVerdict::Pass);
		c.OnTrigger(0.5f);
		CHECK(c.TriggerHeld());
	}

	// two quick chords both swap (0.4 s apart)
	{
		auto c = Make();
		c.OnTrigger(1.0f);
		CHECK(c.OnAButton(true, true, 7.0) == ChordVerdict::SwapAndSwallow);
		CHECK(c.OnAButton(false, false, 7.1) == ChordVerdict::Swallow);
		CHECK(c.OnAButton(true, true, 7.4) == ChordVerdict::SwapAndSwallow);
	}

	// the keyboard path and the pad path for one press: only one swap
	{
		auto c = Make();
		c.OnTrigger(1.0f);
		CHECK(c.OnAButton(true, true, 8.0) == ChordVerdict::SwapAndSwallow);
		CHECK(!c.AllowKeySwap(8.05));  // a layout that sends both: the key does not swap back
		CHECK(c.AllowKeySwap(8.5));    // a later key press is the player's own
		CHECK(c.OnAButton(false, false, 8.55) == ChordVerdict::Swallow);
		CHECK(c.OnAButton(true, true, 8.6) == ChordVerdict::Swallow);  // the key swapped 0.1 s ago: the pad press is swallowed but does not swap again
		CHECK(c.OnAButton(false, false, 8.7) == ChordVerdict::Swallow);
		CHECK(c.OnAButton(true, true, 9.0) == ChordVerdict::SwapAndSwallow);
	}

	// the user's Steam layout: the game sees the trigger and the key X, never A; every key press swaps
	{
		auto c = Make();
		c.OnTrigger(1.0f);
		CHECK(c.AllowKeySwap(10.0));
		CHECK(c.AllowKeySwap(10.3));
		CHECK(c.AllowKeySwap(10.35));
	}

	// a reset forgets a held trigger and a captured press
	{
		auto c = Make();
		c.OnTrigger(1.0f);
		CHECK(c.OnAButton(true, true, 11.0) == ChordVerdict::SwapAndSwallow);
		c.Reset();
		CHECK(!c.TriggerHeld());
		CHECK(!c.Captured());
		CHECK(c.OnAButton(false, false, 11.1) == ChordVerdict::Pass);
	}

	// switched off while a chord press is held: the release passes (the game is not left with a swallowed press half way)
	{
		auto c = Make();
		c.OnTrigger(1.0f);
		CHECK(c.OnAButton(true, true, 12.0) == ChordVerdict::SwapAndSwallow);
		c.SetSettings({ false, 0.5f, 0.25 });
		CHECK(c.OnAButton(false, false, 12.1) == ChordVerdict::Pass);
	}

	std::printf("SwapChord: %s (%d failure(s))\n", g_failures == 0 ? "ok" : "FAILED", g_failures);
	return g_failures;
}
