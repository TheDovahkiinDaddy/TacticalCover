// Engine-free tests for the Phase 3 core: the candidate cache and the press decision (T3.0). The state machine has its own file.

#include "Core/CandidateCache.h"
#include "Core/CoverDecision.h"
#include "Core/CoverStateMachine.h"

#include <array>
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

	CoverCandidate MakeCandidate(const CoverKind a_kind)
	{
		CoverCandidate c;
		c.kind = a_kind;
		c.normal = { -1.0f, 0.0f };
		c.tangent = Perp(c.normal);
		c.standPoint = { 5.0f, 0.0f, 0.0f };
		c.height = a_kind == CoverKind::Low ? 0.9f : 2.0f;
		return c;
	}

	// ---- the cache ----------------------------------------------------------------------------------------------------------------
	void TestCache()
	{
		CandidateCache cache;
		CHECK(cache.Empty());
		CHECK(!cache.Fresh(0.0, 0.35f, { 1.0f, 0.0f }, 45.0f));

		cache.Store(10.0, MakeCandidate(CoverKind::High), { 1.0f, 0.0f }, { 0.0f, 0.0f, 0.0f });
		CHECK(!cache.Empty());
		CHECK(cache.Fresh(10.0, 0.35f, { 1.0f, 0.0f }, 45.0f).has_value());
		CHECK(cache.Fresh(10.34, 0.35f, { 1.0f, 0.0f }, 45.0f).has_value());
		CHECK(!cache.Fresh(10.36, 0.35f, { 1.0f, 0.0f }, 45.0f));  // too old
		CHECK(!cache.Fresh(9.0, 0.35f, { 1.0f, 0.0f }, 45.0f));    // from the future: the clock went back

		// the input turned away from the direction the answer was found for
		CHECK(cache.Fresh(10.1, 0.35f, { 1.0f, 0.5f }, 45.0f).has_value());   // 27 degrees
		CHECK(!cache.Fresh(10.1, 0.35f, { 1.0f, 1.5f }, 45.0f));              // 56 degrees
		CHECK(!cache.Fresh(10.1, 0.35f, { -1.0f, 0.0f }, 45.0f));             // the other way
		CHECK(cache.Fresh(10.1, 0.35f, { -1.0f, 0.0f }, 0.0f).has_value());   // the check is off
		CHECK(cache.Fresh(10.1, 0.35f, { 0.0f, 0.0f }, 45.0f).has_value());   // no intent now: nothing to compare

		// the stored intent is normalised, and the answer keeps its feet and time
		const auto got = cache.Fresh(10.1, 0.35f, { 1.0f, 0.0f }, 45.0f);
		CHECK(got && std::fabs(Length(got->intent) - 1.0f) < 1e-5f && std::fabs(got->time - 10.0) < 1e-9);

		// a rejection is an answer too
		CoverCandidate none;
		none.why = Reject::NoSurface;
		cache.Store(11.0, none, { 0.0f, 1.0f }, {});
		const auto n = cache.Fresh(11.1, 0.35f, { 0.0f, 1.0f }, 45.0f);
		CHECK(n && n->candidate.kind == CoverKind::None && n->candidate.why == Reject::NoSurface);

		cache.Clear();
		CHECK(cache.Empty() && !cache.Fresh(11.1, 0.35f, { 0.0f, 1.0f }, 45.0f));
	}

	// ---- the decision on a Sneak press -----------------------------------------------------------------------------------------------
	PressContext Ctx(const CoverKind a_kind, const int a_stance)
	{
		PressContext c;
		c.mode = 1;
		c.state = CoverState::None;
		c.allowed = true;
		c.moveMagnitude = 1.0f;
		c.stickMin = 0.4f;
		c.candidateFresh = true;
		c.candidateKind = a_kind;
		c.stance = a_stance;
		return c;
	}

	void TestDecisionEnter()
	{
		// M1: an Enter is always swallowed, whatever the stance (the gamepad's B toggles sneak only for a press held under about 0.4 s); the plugin makes the stance
		// itself. High cover wants standing, low cover wants sneaking.
		for (const int stance : { 0, 1 }) {
			auto d = DecideSneakPress(Ctx(CoverKind::High, stance));
			CHECK(d.action == PressAction::Enter && d.swallow && d.wantedStance == 0);
			d = DecideSneakPress(Ctx(CoverKind::Low, stance));
			CHECK(d.action == PressAction::Enter && d.swallow && d.wantedStance == 1);
		}
	}

	void TestDecisionVanilla()
	{
		const auto vanilla = [](const PressContext& a_c) {
			const auto d = DecideSneakPress(a_c);
			return d.action == PressAction::Nothing && !d.swallow;
		};
		auto c = Ctx(CoverKind::High, 0);
		c.moveMagnitude = 0.0f;  // nothing pushed
		CHECK(vanilla(c));
		c.moveMagnitude = 0.39f;
		CHECK(vanilla(c));
		c.moveMagnitude = 0.4f;
		CHECK(DecideSneakPress(c).action == PressAction::Enter);
		c = Ctx(CoverKind::High, 0);
		c.candidateFresh = false;  // stale or no answer
		CHECK(vanilla(c));
		c = Ctx(CoverKind::None, 0);  // the sensor looked and found nothing
		CHECK(vanilla(c));
		c = Ctx(CoverKind::High, 0);
		c.allowed = false;  // first person, airborne, ...
		CHECK(vanilla(c));
		c = Ctx(CoverKind::High, -1);  // stance unreadable
		CHECK(vanilla(c));
		c = Ctx(CoverKind::High, 0);
		c.mode = 0;  // the other trigger modes do not use the Sneak control (yet)
		CHECK(vanilla(c));
		c.mode = 2;
		CHECK(vanilla(c));
		c.mode = 3;  // automatic mode: a press pushing toward cover still enters at once
		CHECK(DecideSneakPress(c).action == PressAction::Enter && DecideSneakPress(c).swallow);
	}

	void TestDecisionLeave()
	{
		auto c = Ctx(CoverKind::High, 0);
		c.state = CoverState::InCover;
		c.moveMagnitude = 0.0f;  // the stick does not matter in cover
		c.candidateFresh = false;
		c.stanceBefore = 0;
		auto d = DecideSneakPress(c);
		CHECK(d.action == PressAction::Leave && d.swallow && d.wantedStance == 0);  // already what it was before
		c.stance = 1;  // low cover made the player sneak: the plugin's own press restores it, the real one is swallowed all the same (M1)
		d = DecideSneakPress(c);
		CHECK(d.action == PressAction::Leave && d.swallow && d.wantedStance == 0);
		c.stanceBefore = 1;  // the player was sneaking before and still is
		d = DecideSneakPress(c);
		CHECK(d.action == PressAction::Leave && d.swallow && d.wantedStance == 1);
		c.state = CoverState::Peeking;
		CHECK(DecideSneakPress(c).action == PressAction::Leave);
		c.stanceBefore = -1;  // unknown: keep what the player has
		d = DecideSneakPress(c);
		CHECK(d.action == PressAction::Leave && d.swallow);
		c.stanceBefore = 0;
		c.stance = -1;
		d = DecideSneakPress(c);
		CHECK(d.action == PressAction::Leave && d.swallow);
		c.mode = 2;  // hold mode: in cover the press leaves like mode 1
		CHECK(DecideSneakPress(c).action == PressAction::Leave);
		c.mode = 3;  // automatic mode: B in cover leaves too (M1b)
		d = DecideSneakPress(c);
		CHECK(d.action == PressAction::Leave && d.swallow);
		c.mode = 0;  // the dedicated key mode never touches the Sneak control
		CHECK(DecideSneakPress(c).action == PressAction::Nothing && !DecideSneakPress(c).swallow);
	}

	// T3.5: the dedicated key (mode 0)
	void TestCoverKey()
	{
		auto c = Ctx(CoverKind::High, 1);
		c.mode = 0;
		c.moveMagnitude = 0.0f;  // no movement input needed
		auto d = DecideCoverKeyPress(c);
		CHECK(d.action == PressAction::Enter && !d.swallow && d.wantedStance == 0);
		c = Ctx(CoverKind::Low, 0);
		c.mode = 0;
		d = DecideCoverKeyPress(c);
		CHECK(d.action == PressAction::Enter && d.wantedStance == 1);
		c.candidateFresh = false;
		CHECK(DecideCoverKeyPress(c).action == PressAction::Nothing);
		c = Ctx(CoverKind::None, 0);
		c.mode = 0;
		CHECK(DecideCoverKeyPress(c).action == PressAction::Nothing);
		c = Ctx(CoverKind::High, 0);
		c.mode = 0;
		c.allowed = false;
		CHECK(DecideCoverKeyPress(c).action == PressAction::Nothing);
		c = Ctx(CoverKind::High, -1);
		c.mode = 0;
		CHECK(DecideCoverKeyPress(c).action == PressAction::Nothing);
		c = Ctx(CoverKind::High, 0);
		c.mode = 1;  // another mode: the key does nothing
		CHECK(DecideCoverKeyPress(c).action == PressAction::Nothing);
		// in cover it leaves, restoring the stance held before
		c = Ctx(CoverKind::High, 1);
		c.mode = 0;
		c.state = CoverState::InCover;
		c.stanceBefore = 0;
		d = DecideCoverKeyPress(c);
		CHECK(d.action == PressAction::Leave && d.wantedStance == 0);
		c.state = CoverState::Entering;
		CHECK(DecideCoverKeyPress(c).action == PressAction::Nothing);
	}

	void TestDecisionBusy()
	{
		for (const auto s : { CoverState::Entering, CoverState::Exiting }) {
			auto c = Ctx(CoverKind::High, 0);
			c.state = s;
			const auto d = DecideSneakPress(c);
			CHECK(d.action == PressAction::Nothing && d.swallow);
		}
	}

}

int RunCoverTests()
{
	g_failures = 0;
	TestCache();
	TestDecisionEnter();
	TestDecisionVanilla();
	TestDecisionLeave();
	TestDecisionBusy();
	TestCoverKey();
	std::printf("cover tests: %s\n", g_failures == 0 ? "ok" : "FAILED");
	return g_failures;
}
