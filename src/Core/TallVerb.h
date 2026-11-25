#pragma once

// T5.2 tall pose: what the aim or blind fire verbs may do at a tall cover (REFERENCE 22.17). The standing cover component has only the side modes (`sighted`, `blindfire`), which lean the
// actor out past the end of the wall on the side it is turned to, so they make sense only with an Open end of the wall in reach on that side. A pure decision, tested without the game.
//   Here: the actor's own side has an Open end within the range: start the verb now.
//   Turn: only the other end is open and in reach: turn round first (the Enter event of the other side), the verb after it.
//   None: no open end in reach: the verb does nothing and the player stays in the pose.

#include "Core/CoverSensor.h"

namespace Core
{
	enum class TallVerb
	{
		None,
		Here,
		Turn,
	};

	struct TallEnds
	{
		float    left{ 1e9f };    // distance from the foot to the left end seen facing the wall
		float    right{ 1e9f };
		EdgeKind leftKind{ EdgeKind::None };
		EdgeKind rightKind{ EdgeKind::None };
	};

	// a_sideLeft: the actor is turned to the Left (the graph's iSyncRightLeft 1), a_range: fEdgePeekRange
	[[nodiscard]] TallVerb DecideTallVerb(const TallEnds& a_ends, bool a_sideLeft, float a_range) noexcept;
	// card M4b: idle in the tall pose: true when the actor should turn to the other side by itself: only the other end is open and in range, or both are and the other is nearer by more than a_margin
	[[nodiscard]] bool DecideAutoTurn(const TallEnds& a_ends, bool a_sideLeft, float a_range, float a_margin) noexcept;
	[[nodiscard]] const char* ToString(TallVerb a_verb) noexcept;
}
