#include "Core/Verdicts.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Core
{
	std::string JumpVerdict(const std::span<const float> a_rises)
	{
		if (a_rises.empty()) {
			return "no data";
		}
		const bool allBlocked = std::ranges::all_of(a_rises, [](const float r) { return r < kJumpBlockedBelow; });
		const bool allFree = std::ranges::all_of(a_rises, [](const float r) { return r > kJumpNotBlockedAbove; });
		return allBlocked ? "BLOCKED" : (allFree ? "not blocked" : "MIXED");
	}

	std::string ZeroVerdict(const float a_ratio)
	{
		return a_ratio < 0.25f ? "STOPPED the player" : (a_ratio < 0.8f ? "SLOWED the player" : "NO EFFECT");
	}

	std::string ScaleVerdict(const float a_ratio)
	{
		if (std::fabs(a_ratio - 1.0f) < 0.15f) {
			return "NO CHANGE";
		}
		char buffer[64];
		std::snprintf(buffer, sizeof(buffer), "CHANGED the speed to %.0f%% of baseline", static_cast<double>(a_ratio) * 100.0);
		return buffer;
	}
}
