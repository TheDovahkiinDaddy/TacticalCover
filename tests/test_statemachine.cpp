// Engine-free tests for Core/CoverStateMachine (T3.1): every row of PLAN.md section 3.2.

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

	// ---- the state machine, every row of section 3.2 ------------------------------------------------------------------------
	MachineInputs In(const double a_now)
	{
		MachineInputs i;
		i.now = a_now;
		return i;
	}

	// a machine that has entered and reached the stand point at t = 0.3
	CoverStateMachine InCoverMachine(const CoverKind a_kind = CoverKind::High)
	{
		CoverStateMachine m;
		CHECK(m.RequestEnter(0.0, MakeCandidate(a_kind), 0));
		auto in = In(0.1);
		in.reachedStand = true;
		CHECK(m.Update(in).event == MachineEvent::Reached);
		CHECK(m.State() == CoverState::InCover);
		return m;
	}

	void TestEnter()
	{
		// None -> Entering
		CoverStateMachine m;
		CHECK(m.State() == CoverState::None && !m.Active());
		CHECK(!m.RequestEnter(0.0, MakeCandidate(CoverKind::None), 0));  // no candidate
		CHECK(m.State() == CoverState::None);
		CHECK(m.RequestEnter(1.0, MakeCandidate(CoverKind::Low), 0));
		CHECK(m.State() == CoverState::Entering && m.Active());
		CHECK(m.StanceBefore() == 0 && m.WantedStance() == 1 && m.Candidate().kind == CoverKind::Low);
		CHECK(!m.RequestEnter(1.1, MakeCandidate(CoverKind::High), 1));  // not from Entering
		CHECK(m.Candidate().kind == CoverKind::Low && m.StanceBefore() == 0);

		CoverStateMachine h;
		CHECK(h.RequestEnter(0.0, MakeCandidate(CoverKind::High), 1));
		CHECK(h.WantedStance() == 0 && h.StanceBefore() == 1);
	}

	void TestEnteringEnds()
	{
		// Entering -> InCover: reached
		{
			CoverStateMachine m;
			CHECK(m.RequestEnter(0.0, MakeCandidate(CoverKind::High), 0));
			auto r = m.Update(In(0.1));
			CHECK(r.event == MachineEvent::None && m.State() == CoverState::Entering);
			auto in = In(0.2);
			in.reachedStand = true;
			r = m.Update(in);
			CHECK(r.event == MachineEvent::Reached && !r.snap && m.State() == CoverState::InCover);
		}
		// Entering -> InCover: 0.4 s passed without reaching it: snap
		{
			CoverStateMachine m;
			CHECK(m.RequestEnter(0.0, MakeCandidate(CoverKind::High), 0));
			CHECK(m.Update(In(0.39)).event == MachineEvent::None);
			const auto r = m.Update(In(0.40));
			CHECK(r.event == MachineEvent::Reached && r.snap && m.State() == CoverState::InCover);
		}
		// Entering -> None: the path is blocked
		{
			CoverStateMachine m;
			CHECK(m.RequestEnter(0.0, MakeCandidate(CoverKind::High), 0));
			auto in = In(0.1);
			in.pathBlocked = true;
			const auto r = m.Update(in);
			CHECK(r.event == MachineEvent::EnterBlocked && m.State() == CoverState::None);
		}
		// blocked wins over reached in the same tick
		{
			CoverStateMachine m;
			CHECK(m.RequestEnter(0.0, MakeCandidate(CoverKind::High), 0));
			auto in = In(0.1);
			in.pathBlocked = true;
			in.reachedStand = true;
			CHECK(m.Update(in).event == MachineEvent::EnterBlocked);
		}
	}

	void TestPeek()
	{
		for (const auto kind : { PeekKind::Left, PeekKind::Right, PeekKind::Over }) {
			auto m = InCoverMachine();
			// aim held, no free spot: nothing
			auto in = In(1.0);
			in.aimHeld = true;
			CHECK(m.Update(in).event == MachineEvent::None && m.State() == CoverState::InCover);
			// a free spot but no aim: nothing
			in = In(1.1);
			in.peekSpot = kind;
			CHECK(m.Update(in).event == MachineEvent::None && m.State() == CoverState::InCover);
			// both: Peeking
			in = In(1.2);
			in.aimHeld = true;
			in.peekSpot = kind;
			auto r = m.Update(in);
			CHECK(r.event == MachineEvent::PeekStarted && r.peek == kind && m.State() == CoverState::Peeking && m.Peek() == kind);
			// still aiming, spot still free: stays
			in.now = 1.4;
			CHECK(m.Update(in).event == MachineEvent::None && m.State() == CoverState::Peeking);
			// aim released: back to InCover
			in = In(1.6);
			in.peekSpot = kind;
			r = m.Update(in);
			CHECK(r.event == MachineEvent::PeekEnded && m.State() == CoverState::InCover && m.Peek() == PeekKind::None);

			// Peeking -> InCover: the spot became invalid while aim is held
			in = In(2.0);
			in.aimHeld = true;
			in.peekSpot = kind;
			CHECK(m.Update(in).event == MachineEvent::PeekStarted);
			in = In(2.2);
			in.aimHeld = true;
			in.peekSpot = PeekKind::None;
			CHECK(m.Update(in).event == MachineEvent::PeekEnded && m.State() == CoverState::InCover);
		}
	}

	void CheckExits(const bool a_fromPeeking)
	{
		const auto fresh = [&] {
			auto m = InCoverMachine(CoverKind::Low);
			if (a_fromPeeking) {
				auto in = In(1.0);
				in.aimHeld = true;
				in.peekSpot = PeekKind::Right;
				CHECK(m.Update(in).event == MachineEvent::PeekStarted);
			}
			return m;
		};
		const auto base = [&](const double a_now) {
			auto in = In(a_now);
			in.aimHeld = a_fromPeeking;
			in.peekSpot = a_fromPeeking ? PeekKind::Right : PeekKind::None;
			return in;
		};

		// the trigger again
		{
			auto m = fresh();
			CHECK(m.RequestLeave(2.0, ExitReason::Trigger));
			CHECK(m.State() == CoverState::Exiting && m.LastExit() == ExitReason::Trigger);
			CHECK(m.Update(base(2.1)).event == MachineEvent::None && m.State() == CoverState::Exiting);  // 0.1 s: not yet
			const auto r = m.Update(base(2.16));
			CHECK(r.event == MachineEvent::ExitDone && m.State() == CoverState::None);
		}
		// jump and sprint are the same call with another reason
		for (const auto reason : { ExitReason::Jump, ExitReason::Sprint }) {
			auto m = fresh();
			CHECK(m.RequestLeave(2.0, reason));
			CHECK(m.State() == CoverState::Exiting && m.LastExit() == reason);
			CHECK(!m.RequestLeave(2.01, ExitReason::Trigger));  // already leaving
			CHECK(m.LastExit() == reason);
		}
		// the input held away from the wall for fExitHoldTime
		{
			auto m = fresh();
			auto in = base(2.0);
			in.awayInput = 0.7f;
			CHECK(m.Update(in).event == MachineEvent::None);
			in = base(2.2);
			in.awayInput = 0.7f;
			CHECK(m.Update(in).event == MachineEvent::None && m.State() != CoverState::Exiting);  // 0.2 s
			in = base(2.26);
			in.awayInput = 0.7f;
			const auto r = m.Update(in);
			CHECK(r.event == MachineEvent::ExitStarted && r.exitReason == ExitReason::HoldAway && m.State() == CoverState::Exiting);
		}
		// ... but not when it lets up in between (the timer starts again), and not below the threshold
		{
			auto m = fresh();
			auto in = base(2.0);
			in.awayInput = 0.7f;
			(void)m.Update(in);
			in = base(2.2);
			in.awayInput = 0.5f;  // below fExitInputThreshold (0.6)
			CHECK(m.Update(in).event == MachineEvent::None);
			in = base(2.3);
			in.awayInput = 0.7f;
			CHECK(m.Update(in).event == MachineEvent::None);
			in = base(2.5);
			in.awayInput = 0.7f;
			CHECK(m.Update(in).event == MachineEvent::None && m.State() != CoverState::Exiting);  // 0.2 s since 2.3
			in = base(2.56);
			in.awayInput = 0.7f;
			CHECK(m.Update(in).event == MachineEvent::ExitStarted);
		}
		// the cover lost for several updates in a row (3 by default): two do not leave, three do
		{
			auto m = fresh();
			auto in = base(2.0);
			in.lostCount = 2;
			CHECK(m.Update(in).event == MachineEvent::None);
			in = base(2.05);
			in.lostCount = 3;
			const auto r = m.Update(in);
			CHECK(r.event == MachineEvent::ExitStarted && r.exitReason == ExitReason::Lost);
		}
		// displaced more than fMaxDeviation (0.35), or shoved by something else
		{
			auto m = fresh();
			auto in = base(2.0);
			in.deviation = 0.35f;
			CHECK(m.Update(in).event == MachineEvent::None);
			in = base(2.05);
			in.deviation = 0.36f;
			const auto r = m.Update(in);
			CHECK(r.event == MachineEvent::ExitStarted && r.exitReason == ExitReason::Displaced);
		}
		// shoved (a hit reaction, an explosion): leaves at once even with no deviation measured
		{
			auto m = fresh();
			auto in = base(2.0);
			in.shoved = true;
			const auto r = m.Update(in);
			CHECK(r.event == MachineEvent::ExitStarted && r.exitReason == ExitReason::Displaced && m.State() == CoverState::Exiting);
		}
		// the tracked candidate replaces the stored one without changing the state
		{
			auto m = fresh();
			auto c = MakeCandidate(CoverKind::Low);
			c.standPoint = { 7.0f, 1.0f, 0.0f };
			m.SetCandidate(c);
			CHECK(m.Candidate().standPoint.x == 7.0f && m.State() != CoverState::Exiting && m.State() != CoverState::None);
		}
	}

	void TestExits()
	{
		CheckExits(false);
		CheckExits(true);
		// a leave request before Reached also works (a jump while the player is still sliding in); in None it does nothing
		CoverStateMachine m;
		CHECK(!m.RequestLeave(0.0, ExitReason::Jump));
		CHECK(m.RequestEnter(0.0, MakeCandidate(CoverKind::High), 0));
		CHECK(m.RequestLeave(0.1, ExitReason::Jump) && m.State() == CoverState::Exiting);
	}

	void TestHardReset()
	{
		const auto check = [](CoverStateMachine& a_m, const MachineInputs& a_in, const char* a_what) {
			const auto before = a_m.State();
			const auto r = a_m.Update(a_in);
			if (before == CoverState::None) {
				CHECK(r.event == MachineEvent::None);
			} else if (r.event != MachineEvent::HardReset || a_m.State() != CoverState::None) {
				std::printf("FAIL hard reset (%s) from %s\n", a_what, ToString(before));
				++g_failures;
			}
		};

		const auto states = [] {
			std::array<CoverStateMachine, 5> v{};
			CHECK(v[1].RequestEnter(0.0, MakeCandidate(CoverKind::High), 0));  // Entering
			v[2] = InCoverMachine();                                            // InCover
			v[3] = InCoverMachine();                                            // Peeking
			auto in = In(1.0);
			in.aimHeld = true;
			in.peekSpot = PeekKind::Left;
			CHECK(v[3].Update(in).event == MachineEvent::PeekStarted);
			v[4] = InCoverMachine();                                            // Exiting
			CHECK(v[4].RequestLeave(1.0, ExitReason::Trigger));
			return v;
		};

		{  // the game loaded, a cell change, a teleport or death: the owner sets worldReset
			auto v = states();
			for (std::size_t i = 0; i < v.size(); ++i) {
				auto in = In(1.1);
				in.worldReset = true;
				check(v[i], in, "world");
			}
		}
		{  // the camera left third person, first person, airborne ...: allowed is false
			auto v = states();
			for (std::size_t i = 0; i < v.size(); ++i) {
				auto in = In(1.1);
				in.allowed = false;
				check(v[i], in, "not allowed");
			}
		}
		// the reset names its cause and leaves the machine reusable
		CoverStateMachine m = InCoverMachine();
		auto in = In(1.0);
		in.allowed = false;
		auto r = m.Update(in);
		CHECK(r.event == MachineEvent::HardReset && r.why && r.why[0] != '\0');
		CHECK(m.RequestEnter(2.0, MakeCandidate(CoverKind::Low), 1) && m.State() == CoverState::Entering);
		// Reset() is the direct form
		CHECK(m.Reset() && m.State() == CoverState::None && !m.Reset());
		// a machine in None ignores the facts of the world
		CoverStateMachine idle;
		in = In(1.0);
		in.allowed = false;
		in.worldReset = true;
		CHECK(idle.Update(in).event == MachineEvent::None);
	}

	void TestSettings()
	{
		StateMachineSettings s;
		s.enterMaxTime = 0.8f;
		s.exitTime = 0.5f;
		s.exitHoldTime = 0.1f;
		s.exitInputThreshold = 0.3f;
		s.maxDeviation = 0.2f;
		s.lostLeaveCount = 1;
		CoverStateMachine m(s);
		CHECK(m.RequestEnter(0.0, MakeCandidate(CoverKind::High), 0));
		CHECK(m.Update(In(0.5)).event == MachineEvent::None);
		CHECK(m.Update(In(0.8)).snap);
		auto in = In(1.0);
		in.lostCount = 1;
		CHECK(m.Update(in).exitReason == ExitReason::Lost);
		CHECK(m.Update(In(1.4)).event == MachineEvent::None);
		CHECK(m.Update(In(1.5)).event == MachineEvent::ExitDone);
	}
}

int RunStateMachineTests()
{
	g_failures = 0;
	TestEnter();
	TestEnteringEnds();
	TestPeek();
	TestExits();
	TestHardReset();
	TestSettings();
	std::printf("state machine tests: %s\n", g_failures == 0 ? "ok" : "FAILED");
	return g_failures;
}
