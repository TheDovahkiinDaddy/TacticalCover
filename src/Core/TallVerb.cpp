#include "Core/TallVerb.h"

namespace Core
{
	TallVerb DecideTallVerb(const TallEnds& a_ends, const bool a_sideLeft, const float a_range) noexcept
	{
		const bool leftOk = a_ends.leftKind == EdgeKind::Open && a_ends.left <= a_range;
		const bool rightOk = a_ends.rightKind == EdgeKind::Open && a_ends.right <= a_range;
		const bool here = a_sideLeft ? leftOk : rightOk;
		const bool other = a_sideLeft ? rightOk : leftOk;
		if (here) {
			return TallVerb::Here;
		}
		return other ? TallVerb::Turn : TallVerb::None;
	}

	bool DecideAutoTurn(const TallEnds& a_ends, const bool a_sideLeft, const float a_range, const float a_margin) noexcept
	{
		const bool leftOk = a_ends.leftKind == EdgeKind::Open && a_ends.left <= a_range;
		const bool rightOk = a_ends.rightKind == EdgeKind::Open && a_ends.right <= a_range;
		const bool  here = a_sideLeft ? leftOk : rightOk;
		const bool  other = a_sideLeft ? rightOk : leftOk;
		if (!other) {
			return false;
		}
		if (!here) {
			return true;
		}
		const float dHere = a_sideLeft ? a_ends.left : a_ends.right;
		const float dOther = a_sideLeft ? a_ends.right : a_ends.left;
		return dOther + a_margin < dHere;
	}

	const char* ToString(const TallVerb a_verb) noexcept
	{
		switch (a_verb) {
		case TallVerb::None: return "none";
		case TallVerb::Here: return "here";
		case TallVerb::Turn: return "turn first";
		}
		return "?";
	}
}
