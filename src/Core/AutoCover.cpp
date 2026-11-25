#include "Core/AutoCover.h"

#include <algorithm>
#include <cmath>

namespace Core
{
	float AngleToWallDeg(const Vec2 a_inputDir, const Vec2 a_wallNormal) noexcept
	{
		const float li = std::sqrt(a_inputDir.x * a_inputDir.x + a_inputDir.y * a_inputDir.y);
		const float ln = std::sqrt(a_wallNormal.x * a_wallNormal.x + a_wallNormal.y * a_wallNormal.y);
		if (li < 1e-4f || ln < 1e-4f) {
			return 180.0f;
		}
		const float c = std::clamp(-(a_inputDir.x * a_wallNormal.x + a_inputDir.y * a_wallNormal.y) / (li * ln), -1.0f, 1.0f);
		return std::acos(c) * 57.29578f;
	}

	float NearestOpenEnd(const CoverCandidate& a_c) noexcept
	{
		float d = 1e9f;
		if (a_c.edgeRightKind == EdgeKind::Open) {
			d = (std::min)(d, a_c.edgeRight);
		}
		if (a_c.edgeLeftKind == EdgeKind::Open) {
			d = (std::min)(d, a_c.edgeLeft);
		}
		return d;
	}

	void AutoCover::Reset() noexcept
	{
		m_armed = true;
		m_heldSince = -1.0;
		m_falseSince = -1.0;
	}

	AutoCoverResult AutoCover::Update(const AutoCoverSettings& a_s, const AutoCoverInput& a_in) noexcept
	{
		AutoCoverResult r;
		if (a_in.state != CoverState::None) {
			// any cover state switches it off; the clock of the re-arm starts when the state is None again
			m_armed = false;
			m_heldSince = -1.0;
			m_falseSince = -1.0;
			r.why = "a cover state is running";
			return r;
		}

		const char* why = "";
		if (!a_in.allowed) {
			why = "cover is not allowed now";
		} else if (a_in.sprinting) {
			why = "sprinting";
		} else if (a_in.moveMagnitude < a_in.stickMin) {
			why = "the movement input is below the stick minimum";
		} else if (!a_in.candidateFresh || a_in.kind == CoverKind::None) {
			why = "no fresh cover in this direction";
		} else if (a_in.angleDeg > a_s.angleDeg) {
			why = "the input does not point into the cover";
		} else if (a_in.standDistance > a_s.distance) {
			why = "the stand point is too far";
		} else if (a_in.kind == CoverKind::High && a_s.tallMode <= 0) {
			why = "tall cover is not taken by itself (iAutoCoverTall 0)";
		} else if (a_in.kind == CoverKind::High && a_s.tallMode == 1 && (a_s.tallEdge <= 0.0f || a_in.openEndDistance > a_s.tallEdge)) {
			why = "tall cover without an open end near";
		} else if (a_in.kind == CoverKind::High && a_s.tallMode >= 2 && !a_in.weaponDrawn) {
			why = "tall cover with the weapon not drawn";
		}
		r.why = why;
		r.into = why[0] == '\0';

		if (!r.into) {
			m_heldSince = -1.0;
			if (!m_armed) {
				if (m_falseSince < 0.0) {
					m_falseSince = a_in.now;
				}
				if (a_in.now - m_falseSince >= static_cast<double>(a_s.rearm)) {
					m_armed = true;
					m_falseSince = -1.0;
				}
			}
			return r;
		}

		m_falseSince = -1.0;
		if (!m_armed) {
			r.why = "not armed: the pull toward cover has not been off since the last cover";
			return r;
		}
		if (m_heldSince < 0.0) {
			m_heldSince = a_in.now;
		}
		const float need = a_in.weaponDrawn ? a_s.timeDrawn : a_s.time;  // card M4b: cover at once with the weapon drawn, the old time with it away (timeTall is no longer read)
		if (a_in.now - m_heldSince >= static_cast<double>(need)) {
			r.enter = true;
			r.heldFor = static_cast<float>(a_in.now - m_heldSince);
			m_heldSince = -1.0;
			m_armed = false;  // the state leaves None next tick; a refusal by the machine re-arms through the false condition
		}
		return r;
	}
}
