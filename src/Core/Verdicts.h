#pragma once

// Engine-free verdict rules for the S2b input-rewriting tests (Engine/RewriteTest). Kept here so the thresholds
// are tested without the game.

#include <span>
#include <string>

namespace Core
{
	inline constexpr float kJumpBlockedBelow = 0.15f;     // metres: every rise under this means the jump never happened
	inline constexpr float kJumpNotBlockedAbove = 0.30f;  // metres: every rise above this means the game saw the jump

	// "no data", "BLOCKED" (every rise < 0.15), "not blocked" (every rise > 0.3) or "MIXED".
	[[nodiscard]] std::string JumpVerdict(std::span<const float> a_rises);

	// ratio = speed in the zeroed stage / baseline speed.
	[[nodiscard]] std::string ZeroVerdict(float a_ratio);

	// ratio = speed in the scaled stage / baseline speed. "NO CHANGE" within 15% of the baseline.
	[[nodiscard]] std::string ScaleVerdict(float a_ratio);
}
