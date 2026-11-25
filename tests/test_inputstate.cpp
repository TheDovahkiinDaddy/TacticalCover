// Engine-free tests for Core/InputState. Built by the CoverSF_tests target; no game needed.
// Run: xmake build CoverSF_tests, then xmake run CoverSF_tests

#include "Core/InputState.h"

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

	bool Near(const float a, const float b) { return std::fabs(a - b) < 1e-5f; }

	void TestClassify()
	{
		using Core::Action;
		CHECK(Core::ClassifyUserEvent("Forward") == Action::MoveForward);
		CHECK(Core::ClassifyUserEvent("Back") == Action::MoveBack);
		CHECK(Core::ClassifyUserEvent("StrafeLeft") == Action::MoveLeft);
		CHECK(Core::ClassifyUserEvent("StrafeRight") == Action::MoveRight);
		CHECK(Core::ClassifyUserEvent("Sprint") == Action::Sprint);
		CHECK(Core::ClassifyUserEvent("Jump") == Action::Jump);
		CHECK(Core::ClassifyUserEvent("PrimaryAttack") == Action::Fire);
		CHECK(Core::ClassifyUserEvent("forward") == Action::None);  // exact match only
		CHECK(Core::ClassifyUserEvent("") == Action::None);
		CHECK(Core::IsMovementAction(Action::MoveLeft));
		CHECK(!Core::IsMovementAction(Action::Jump));
	}

	// Phase 3 safety fix: a stick value left behind by a fast release expires when no stick event arrives for a while; keys are not touched
	void TestStickExpiry()
	{
		Core::InputState s;
		s.SetStickAt(0.0f, 1.0f, 10.0);
		s.ExpireStick(10.2, 0.25);
		CHECK(Near(s.StickY(), 1.0f) && Near(s.MoveY(), 1.0f));  // still fresh
		s.ExpireStick(10.26, 0.25);
		CHECK(Near(s.StickY(), 0.0f) && Near(s.MoveMagnitude(), 0.0f));  // centred
		s.SetStickAt(0.5f, 0.0f, 11.0);  // a new event brings it back
		CHECK(Near(s.StickX(), 0.5f));
		s.ExpireStick(11.1, 0.25);
		CHECK(Near(s.StickX(), 0.5f));
		s.SetStickAt(0.0f, 0.0f, 11.2);  // an event that centres it
		s.ExpireStick(20.0, 0.25);
		CHECK(Near(s.StickX(), 0.0f));
		// keys keep their own state
		Core::InputState k;
		k.ApplyAction(Core::Action::MoveForward, true);
		k.SetStickAt(0.0f, -1.0f, 1.0);
		k.ExpireStick(5.0, 0.25);
		CHECK(Near(k.MoveY(), 1.0f));
	}

	void TestMove()
	{
		using Core::Action;
		Core::InputState s;
		CHECK(Near(s.MoveX(), 0.0f) && Near(s.MoveY(), 0.0f));
		s.ApplyAction(Action::MoveForward, true);
		CHECK(Near(s.MoveY(), 1.0f));
		s.ApplyAction(Action::MoveBack, true);  // opposite keys cancel
		CHECK(Near(s.MoveY(), 0.0f));
		s.ApplyAction(Action::MoveBack, false);
		s.ApplyAction(Action::MoveRight, true);  // diagonal is clamped to the unit disc
		CHECK(Near(s.MoveX() * s.MoveX() + s.MoveY() * s.MoveY(), 1.0f));
		s.ApplyAction(Action::MoveForward, false);
		s.ApplyAction(Action::MoveRight, false);
		s.SetStick(0.5f, -2.0f);  // stick axes clamp to -1..1
		// (0.5, -1) has length 1.118, so it is scaled onto the unit disc
		CHECK(Near(s.MoveX() * s.MoveX() + s.MoveY() * s.MoveY(), 1.0f));
		CHECK(Near(s.MoveX(), 0.5f / std::sqrt(1.25f)));
		CHECK(s.MoveY() < 0.0f);
		CHECK(Near(s.MoveMagnitude(), 1.0f));  // clamped onto the unit disc

		Core::InputState t;
		CHECK(Near(t.MoveMagnitude(), 0.0f));
		t.SetStick(0.03f, 0.04f);
		CHECK(Near(t.MoveMagnitude(), 0.05f));
		t.ApplyAction(Core::Action::MoveForward, true);
		CHECK(t.MoveMagnitude() > 0.9f);
	}

	void TestEdges()
	{
		using Core::Action;
		Core::InputState s;
		CHECK(!s.ConsumeJumpPressed());
		s.ApplyAction(Action::Jump, true);
		s.ApplyAction(Action::Jump, true);  // key repeat: still one edge
		CHECK(s.ConsumeJumpPressed());
		CHECK(!s.ConsumeJumpPressed());
		s.ApplyAction(Action::Jump, false);
		s.ApplyAction(Action::Jump, true);
		CHECK(s.ConsumeJumpPressed());

		s.ApplyCoverKey(true);
		CHECK(s.CoverHeld());
		CHECK(s.ConsumeCoverPressed());
		CHECK(!s.ConsumeCoverPressed());
		s.ApplyCoverKey(false);
		CHECK(!s.CoverHeld());
	}

	void TestHeldFlagsAndDescribe()
	{
		using Core::Action;
		Core::InputState s;
		CHECK(s.Describe() == "-");
		s.ApplyAction(Action::MoveForward, true);
		s.ApplyAction(Action::Sprint, true);
		s.ApplyAction(Action::Aim, true);
		s.ApplyAction(Action::Fire, true);
		CHECK(s.SprintHeld() && s.AimHeld() && s.FireHeld() && !s.SneakHeld());
		CHECK(s.Describe() == "Forward+Sprint+Aim+Fire");
		s.ApplyAction(Action::None, true);  // unknown events change nothing
		CHECK(s.Describe() == "Forward+Sprint+Aim+Fire");
	}
}

int RunInputStateTests()
{
	TestClassify();
	TestMove();
	TestStickExpiry();
	TestEdges();
	TestHeldFlagsAndDescribe();
	if (g_failures == 0) {
		std::printf("all InputState tests passed\n");
		return 0;
	}
	std::printf("%d check(s) failed\n", g_failures);
	return 1;
}
