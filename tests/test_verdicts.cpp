// Engine-free tests for Core/Verdicts.

#include "Core/Verdicts.h"

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

	void TestJump()
	{
		const auto verdict = [](const std::vector<float>& v) { return Core::JumpVerdict(v); };
		CHECK(verdict({}) == "no data");
		CHECK(verdict({ 0.0f, 0.02f }) == "BLOCKED");
		CHECK(verdict({ 1.33f, 1.32f }) == "not blocked");
		CHECK(verdict({ 0.0f, 1.33f }) == "MIXED");
		CHECK(verdict({ 0.2f }) == "MIXED");   // between the two thresholds is neither
		CHECK(verdict({ 0.15f }) == "MIXED");  // exactly on a threshold is not blocked
		CHECK(verdict({ 0.30f }) == "MIXED");
	}

	void TestMove()
	{
		CHECK(Core::ZeroVerdict(0.0f) == "STOPPED the player");
		CHECK(Core::ZeroVerdict(0.24f) == "STOPPED the player");
		CHECK(Core::ZeroVerdict(0.5f) == "SLOWED the player");
		CHECK(Core::ZeroVerdict(0.8f) == "NO EFFECT");
		CHECK(Core::ZeroVerdict(1.0f) == "NO EFFECT");
		CHECK(Core::ScaleVerdict(1.0f) == "NO CHANGE");
		CHECK(Core::ScaleVerdict(0.9f) == "NO CHANGE");
		CHECK(Core::ScaleVerdict(0.5f) == "CHANGED the speed to 50% of baseline");
		CHECK(Core::ScaleVerdict(0.0f) == "CHANGED the speed to 0% of baseline");
	}
}

int RunVerdictTests()
{
	TestJump();
	TestMove();
	if (g_failures == 0) {
		std::printf("all Verdicts tests passed\n");
		return 0;
	}
	std::printf("%d check(s) failed\n", g_failures);
	return 1;
}
