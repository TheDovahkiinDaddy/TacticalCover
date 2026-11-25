#include "Core/CandidateCache.h"

namespace Core
{
	void CandidateCache::Store(const double a_time, const CoverCandidate& a_candidate, const Vec2 a_intent, const Vec3 a_feet)
	{
		m_latest = CachedCandidate{ a_candidate, a_time, Normalized(a_intent), a_feet };
	}

	std::optional<CachedCandidate> CandidateCache::Fresh(const double a_now, const float a_maxAge, const Vec2 a_intentNow, const float a_maxTurnDeg) const
	{
		if (!m_latest) {
			return std::nullopt;
		}
		const double age = a_now - m_latest->time;
		if (age < -0.05 || age > static_cast<double>(a_maxAge)) {
			return std::nullopt;
		}
		const Vec2 now = Normalized(a_intentNow);
		if (a_maxTurnDeg > 0.0f && Length(now) > 0.5f && Length(m_latest->intent) > 0.5f && AngleBetweenDeg(now, m_latest->intent) > a_maxTurnDeg) {
			return std::nullopt;
		}
		return m_latest;
	}
}
