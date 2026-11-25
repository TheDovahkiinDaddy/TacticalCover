#include "Core/CoverMovement.h"

#include <algorithm>
#include <cmath>

namespace Core
{
	Vec2 InputToWorld(const float a_yaw, const float a_x, const float a_y) noexcept
	{
		const float s = std::sin(a_yaw);
		const float c = std::cos(a_yaw);
		return { c * a_x + s * a_y, -s * a_x + c * a_y };
	}

	Vec2 WorldToInput(const float a_yaw, const Vec2 a_world) noexcept
	{
		const float s = std::sin(a_yaw);
		const float c = std::cos(a_yaw);
		return { c * a_world.x - s * a_world.y, s * a_world.x + c * a_world.y };
	}

	MoveLimits LimitsFor(const CoverCandidate& a_candidate, const Vec2 a_player, const float a_edgeMargin) noexcept
	{
		MoveLimits l;
		const float t = Dot(a_player - a_candidate.linePoint, a_candidate.tangent);
		if (a_candidate.edgeRightKind != EdgeKind::None) {
			l.remainPlus = (a_candidate.edgeRight - a_edgeMargin) - t;
		}
		if (a_candidate.edgeLeftKind != EdgeKind::None) {
			l.remainMinus = (a_candidate.edgeLeft - a_edgeMargin) + t;
		}
		return l;
	}

	StickResult RewriteStick(const float a_x, const float a_y, const float a_yaw, const Vec2 a_tangent, const Vec2 a_normal, const MoveLimits& a_limits, const float a_moveScale) noexcept
	{
		StickResult r;
		const Vec2  d = InputToWorld(a_yaw, a_x, a_y);
		float       a = Dot(d, a_tangent);
		const float b = Dot(d, a_normal);
		r.along = a;
		r.away = (std::max)(0.0f, b);
		if (std::fabs(a) < a_limits.alongMin) {
			a = 0.0f;  // below the dead zone along the wall: exactly (0, 0) is written (the Tangent times 0 is a zero vector)
		}
		a *= a_moveScale;
		a = std::clamp(a, -a_limits.maxInput, a_limits.maxInput);
		const float remain = a > 0.0f ? a_limits.remainPlus : a_limits.remainMinus;
		if (a != 0.0f) {
			const float eff = remain - a_limits.stopDistance;  // the room left after the stop distance
			if (eff <= 0.0f) {
				a = 0.0f;
				r.atEnd = true;
			} else if (eff < a_limits.brakeDistance) {
				a *= eff / (std::max)(a_limits.brakeDistance, 1e-3f);
			}
		}
		const Vec2 in = WorldToInput(a_yaw, a_tangent * a);
		r.x = in.x;
		r.y = in.y;
		return r;
	}

	TrackDecision DecideTrack(const TrackStatus a_status, const TrackFilter& a_prev, const int a_lastSide) noexcept
	{
		TrackDecision d;
		d.next = a_prev;
		switch (a_status) {
		case TrackStatus::Ok:
			d.next = TrackFilter{};
			d.useCandidate = true;
			break;
		case TrackStatus::Corner:
			// not counted, nothing stops but the side of the last movement (a side already blocked stays blocked)
			if (a_lastSide != 0) {
				d.next.blockedSide = a_lastSide > 0 ? 1 : -1;
			}
			break;
		case TrackStatus::Lost:
		case TrackStatus::Unavailable:
			++d.next.lostCount;
			d.next.stopAll = true;
			break;
		}
		return d;
	}

	void ApplyBlockedSide(MoveLimits& a_limits, const int a_blockedSide) noexcept
	{
		if (a_blockedSide > 0) {
			a_limits.remainPlus = (std::min)(a_limits.remainPlus, 0.0f);
		} else if (a_blockedSide < 0) {
			a_limits.remainMinus = (std::min)(a_limits.remainMinus, 0.0f);
		}
	}

	float AwayInput(const float a_x, const float a_y, const float a_yaw, const Vec2 a_normal) noexcept
	{
		return (std::max)(0.0f, Dot(InputToWorld(a_yaw, a_x, a_y), a_normal));
	}

	Correction DriftCorrection(const CoverCandidate& a_candidate, const Vec2 a_player, const MoveLimits& a_limits, const float a_tolerance, const float a_maxStep) noexcept
	{
		Correction c;
		const Vec2 stand = XY(a_candidate.standPoint);
		c.perp = Dot(a_player - stand, a_candidate.normal);
		c.overshoot = (std::max)(0.0f, (std::max)(-a_limits.remainPlus, -a_limits.remainMinus));
		if (std::fabs(c.perp) <= a_tolerance) {
			return c;
		}
		const float len = std::fabs(c.perp);
		const Vec2  across = a_candidate.normal * (-c.perp);
		c.needed = true;
		c.delta = len > a_maxStep ? across * (a_maxStep / len) : across;
		return c;
	}
}
