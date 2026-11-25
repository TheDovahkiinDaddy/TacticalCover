#include "Core/SenseTrigger.h"

#include <cmath>

namespace Core
{
	namespace
	{
		constexpr float kPi = 3.14159265358979f;

		// The smallest absolute difference between two angles in degrees (0 to 180).
		float YawDiffDeg(const float a_a, const float a_b) noexcept
		{
			float d = std::fmod(a_a - a_b, 2.0f * kPi);
			if (d > kPi) {
				d -= 2.0f * kPi;
			} else if (d < -kPi) {
				d += 2.0f * kPi;
			}
			return std::fabs(d) * 180.0f / kPi;
		}
	}

	const char* ToString(const SenseReason a_reason) noexcept
	{
		switch (a_reason) {
		case SenseReason::None: return "none";
		case SenseReason::Stopped: return "stopped";
		case SenseReason::Turned: return "turned";
		}
		return "?";
	}

	void SenseTrigger::Sensed(const double a_now, const Vec2 a_position, const float a_yawRadians) noexcept
	{
		m_lastPos = a_position;
		m_lastYaw = a_yawRadians;
		m_lastTime = a_now;
	}

	SenseReason SenseTrigger::Update(const double a_now, const Vec2 a_position, const float a_yawRadians, const bool a_busy)
	{
		if (!m_started) {
			m_started = true;
			m_stillAnchor = a_position;
			m_stillSince = a_now;
			m_yawAnchor = a_yawRadians;
			m_yawSince = a_now;
			m_lastPos = a_position;
			m_lastYaw = a_yawRadians;
			m_lastTime = -1e9;
			return SenseReason::None;
		}
		if (Length(a_position - m_stillAnchor) > m_cfg.moveEps) {
			m_stillAnchor = a_position;
			m_stillSince = a_now;
		}
		if (YawDiffDeg(a_yawRadians, m_yawAnchor) > m_cfg.yawEps) {
			m_yawAnchor = a_yawRadians;
			m_yawSince = a_now;
		}
		if (a_busy || a_now - m_lastTime < static_cast<double>(m_cfg.minInterval)) {
			return SenseReason::None;
		}
		const bool still = a_now - m_stillSince >= static_cast<double>(m_cfg.stillSeconds);
		const bool steady = a_now - m_yawSince >= static_cast<double>(m_cfg.stillSeconds);
		if (still && Length(a_position - m_lastPos) >= m_cfg.minMove) {
			return SenseReason::Stopped;
		}
		if (steady && YawDiffDeg(a_yawRadians, m_lastYaw) > m_cfg.turnDeg) {
			return SenseReason::Turned;
		}
		return SenseReason::None;
	}
}
