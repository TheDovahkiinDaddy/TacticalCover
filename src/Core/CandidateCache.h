#pragma once

// T3.0: the candidate cache. The background sensor (Engine/Cover) asks for cover in the direction of the movement input at a few
// Hz and keeps the latest answer here with the time it was found. A Sneak press is decided inside the input hook in that same call, so
// it can only use what is already known: Fresh() says whether the latest answer is recent enough and still points the way the
// player is pushing. Engine-free; the caller does its own locking.

#include <optional>

#include "Core/CoverSensor.h"

namespace Core
{
	struct CachedCandidate
	{
		CoverCandidate candidate{};  // kind None: the sensor looked and found nothing (the reason is in candidate.why)
		double         time{ 0.0 };  // when the answer arrived
		Vec2           intent{};     // the direction the query was started with (unit)
		Vec3           feet{};       // where the player stood when it started
	};

	class CandidateCache
	{
	public:
		void Store(double a_time, const CoverCandidate& a_candidate, Vec2 a_intent, Vec3 a_feet);
		void Clear() noexcept { m_latest.reset(); }
		[[nodiscard]] bool Empty() const noexcept { return !m_latest.has_value(); }
		[[nodiscard]] const std::optional<CachedCandidate>& Latest() const noexcept { return m_latest; }

		// The latest answer when it is no older than a_maxAge and the movement input has not turned more than a_maxTurnDeg away from
		// the direction it was found for (a_maxTurnDeg <= 0 or a zero a_intentNow skip that check). An answer from the future (a clock
		// that went back) is not fresh either.
		[[nodiscard]] std::optional<CachedCandidate> Fresh(double a_now, float a_maxAge, Vec2 a_intentNow, float a_maxTurnDeg) const;

	private:
		std::optional<CachedCandidate> m_latest;
	};
}
