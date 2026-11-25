// Engine-free tests for the reload sound's learning rule (REFERENCE 23.18). The three learns of the user's play of 2026-11-19 (weapon 0x00028A02) are the data.

#include "Core/ReloadLearn.h"

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

	// the reload's own six
	std::vector<ReloadEvent> Six()
	{
		return { { 3618642577u, 0.04 }, { 3684033017u, 0.04 }, { 1556442835u, 1.99 }, { 2372205863u, 1.99 }, { 2973908415u, 3.81 }, { 404726203u, 3.81 } };
	}

	ReloadLearn A() { return { 5.10, Six() }; }
	ReloadLearn B()
	{
		auto e = Six();
		e.push_back({ 4118201240u, 1.51 });
		e.push_back({ 467692361u, 2.27 });
		e.push_back({ 467692361u, 3.31 });
		return { 3.91, e };  // ended early: a reload that was cut
	}
	ReloadLearn C()
	{
		auto e = Six();
		e.push_back({ 281068570u, 4.95 });
		return { 5.11, e };
	}

	bool IsSix(const ReloadSequence& s)
	{
		const auto six = Six();
		if (s.events.size() != six.size()) {
			return false;
		}
		for (const auto& e : s.events) {
			bool found = false;
			for (const auto& x : six) {
				if (x.id == e.id && std::fabs(x.dt - e.dt) < 0.03) {
					found = true;
				}
			}
			if (!found) {
				return false;
			}
		}
		return true;
	}

	void TestOrderABC()
	{
		std::vector<ReloadLearn> h;
		AddReloadLearn(h, A());
		auto s = ResolveReload(h);
		CHECK(IsSix(s));
		CHECK(std::fabs(s.plainDur - 5.10) < 0.01);
		AddReloadLearn(h, B());
		s = ResolveReload(h);
		CHECK(IsSix(s));                             // the strays of the cut reload are gone at once
		CHECK(std::fabs(s.plainDur - 5.10) < 0.01);  // and the length is not 3.91 s
		AddReloadLearn(h, C());
		s = ResolveReload(h);
		CHECK(IsSix(s));
		CHECK(s.plainDur >= 5.10 && s.plainDur <= 5.11);
		// sorted by time
		for (std::size_t i = 1; i < s.events.size(); ++i) {
			CHECK(s.events[i - 1].dt <= s.events[i].dt);
		}
	}

	void TestBFirst()
	{
		std::vector<ReloadLearn> h;
		AddReloadLearn(h, B());
		auto s = ResolveReload(h);
		CHECK(s.events.size() == 9);  // one learn: all of it (it may be all there is)
		AddReloadLearn(h, A());
		s = ResolveReload(h);
		CHECK(IsSix(s));  // a second learn takes the strays out
		CHECK(std::fabs(s.plainDur - 5.10) < 0.01);
		AddReloadLearn(h, C());
		s = ResolveReload(h);
		CHECK(IsSix(s));
	}

	void TestCutNeverRemovesTheTail()
	{
		std::vector<ReloadLearn> h;
		AddReloadLearn(h, A());
		// a reload cut at 2.5 s that has the first four events only
		AddReloadLearn(h, { 2.50, { { 3618642577u, 0.04 }, { 3684033017u, 0.04 }, { 1556442835u, 1.99 }, { 2372205863u, 1.99 } } });
		auto s = ResolveReload(h);
		CHECK(IsSix(s));
		CHECK(std::fabs(s.plainDur - 5.10) < 0.01);
		AddReloadLearn(h, { 1.00, { { 3618642577u, 0.04 }, { 3684033017u, 0.04 } } });
		s = ResolveReload(h);
		CHECK(IsSix(s));
		AddReloadLearn(h, C());
		s = ResolveReload(h);
		CHECK(IsSix(s));
		// a whole reload that lacks the tail (the engine posted nothing for it) is evidence against it, once there are two more that have it
		std::vector<ReloadLearn> g;
		AddReloadLearn(g, A());
		AddReloadLearn(g, A());
		AddReloadLearn(g, { 5.10, { { 3618642577u, 0.04 }, { 3684033017u, 0.04 }, { 1556442835u, 1.99 }, { 2372205863u, 1.99 } } });
		CHECK(IsSix(ResolveReload(g)));  // present 2 against absent 1
	}

	void TestPlainLengthNeverFromACutReload()
	{
		std::vector<ReloadLearn> h;
		AddReloadLearn(h, A());
		AddReloadLearn(h, { 0.80, {} });
		AddReloadLearn(h, { 3.91, Six() });
		AddReloadLearn(h, C());
		const auto s = ResolveReload(h);
		CHECK(s.plainDur >= 5.10 && s.plainDur <= 5.11);
		// the history is bounded
		for (int i = 0; i < 20; ++i) {
			AddReloadLearn(h, A());
		}
		CHECK(h.size() == kReloadLearnWindow);
	}

	void TestOldFileEntry()
	{
		// the user's file holds C's seven as one entry (an old-format line): one learn; the next plain reload (six events) takes the stray out
		std::vector<ReloadLearn> h;
		AddReloadLearn(h, C());
		CHECK(ResolveReload(h).events.size() == 7);
		AddReloadLearn(h, A());
		CHECK(IsSix(ResolveReload(h)));
	}

	void TestEmpty()
	{
		const auto s = ResolveReload({});
		CHECK(s.events.empty() && s.plainDur == 0.0);
	}
}

int RunReloadLearnTests()
{
	g_failures = 0;
	TestOrderABC();
	TestBFirst();
	TestCutNeverRemovesTheTail();
	TestPlainLengthNeverFromACutReload();
	TestOldFileEntry();
	TestEmpty();
	std::printf("reload learn tests: %s\n", g_failures == 0 ? "ok" : "FAILED");
	return g_failures;
}
