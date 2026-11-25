#include "Core/Geometry.h"

#include <algorithm>
#include <cmath>

namespace Core
{
	float Length(const Vec2 a) noexcept
	{
		return std::sqrt(a.x * a.x + a.y * a.y);
	}

	Vec2 Normalized(const Vec2 a) noexcept
	{
		const float len = Length(a);
		if (len < 1e-6f) {
			return {};
		}
		return { a.x / len, a.y / len };
	}

	float AngleBetweenDeg(const Vec2 a, const Vec2 b) noexcept
	{
		const float dot = std::clamp(Dot(a, b), -1.0f, 1.0f);
		const float cross = Cross(a, b);
		return std::atan2(std::fabs(cross), dot) * 57.29577951f;
	}

	LineFit FitLine(const std::span<const Vec2> a_points)
	{
		LineFit out;
		out.count = a_points.size();
		if (a_points.size() < 2) {
			return out;
		}
		double mx = 0.0, my = 0.0;
		for (const auto& p : a_points) {
			mx += p.x;
			my += p.y;
		}
		const double n = static_cast<double>(a_points.size());
		mx /= n;
		my /= n;
		double sxx = 0.0, syy = 0.0, sxy = 0.0;
		for (const auto& p : a_points) {
			const double dx = p.x - mx, dy = p.y - my;
			sxx += dx * dx;
			syy += dy * dy;
			sxy += dx * dy;
		}
		if (sxx + syy < 1e-12) {
			return out;  // all points the same
		}
		// The principal axis of the covariance matrix.
		const double angle = 0.5 * std::atan2(2.0 * sxy, sxx - syy);
		out.dir = { static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)) };
		out.centroid = { static_cast<float>(mx), static_cast<float>(my) };

		double sumSq = 0.0;
		float  lo = 1e30f, hi = -1e30f;
		for (const auto& p : a_points) {
			const Vec2  d{ p.x - out.centroid.x, p.y - out.centroid.y };
			const float along = Dot(d, out.dir);
			const float off = Cross(out.dir, d);
			sumSq += static_cast<double>(off) * static_cast<double>(off);
			out.maxDev = (std::max)(out.maxDev, std::fabs(off));
			lo = (std::min)(lo, along);
			hi = (std::max)(hi, along);
		}
		out.rms = static_cast<float>(std::sqrt(sumSq / n));
		out.extent = hi - lo;
		out.ok = true;
		return out;
	}

	Vec2 ProjectOnLine(const Vec2 a_point, const Vec2 a_linePoint, const Vec2 a_tangent) noexcept
	{
		const float along = Dot(a_point - a_linePoint, a_tangent);
		return a_linePoint + a_tangent * along;
	}

	Vec2 NormalToward(const Vec2 a_tangent, const Vec2 a_linePoint, const Vec2 a_target) noexcept
	{
		const Vec2  n = Normalized(Perp(a_tangent));
		const float side = Dot(a_target - a_linePoint, n);
		return side < 0.0f ? n * -1.0f : n;
	}

	HeightClass ClassifyHeight(const float a_height, const float a_lowMin, const float a_highMin) noexcept
	{
		if (a_height < a_lowMin) {
			return HeightClass::TooLow;
		}
		return a_height < a_highMin ? HeightClass::Low : HeightClass::High;
	}

	TopResult FindTop(const std::span<const HeightSample> a_samples, const float a_radius)
	{
		TopResult out;
		if (a_samples.empty() || !a_samples.front().near) {
			return out;
		}
		std::size_t last = 0;
		while (last + 1 < a_samples.size() && a_samples[last + 1].near) {
			++last;
		}
		out.hits = static_cast<int>(last + 1);
		out.capped = last + 1 == a_samples.size();
		const float step = a_samples.size() >= 2 ? a_samples[1].height - a_samples[0].height : 0.0f;
		out.top = (std::max)(0.0f, a_samples[last].height - a_radius + step * 0.5f);
		out.ok = true;
		return out;
	}

	EdgeResult FindEdge(const std::span<const EdgeSample> a_samples, const float a_step, const float a_radius, const std::size_t a_missCount)
	{
		EdgeResult  out;
		const auto  need = (std::max)(a_missCount, static_cast<std::size_t>(1));
		std::size_t run = 0;
		for (std::size_t i = 0; i < a_samples.size(); ++i) {
			run = a_samples[i].good ? 0 : run + 1;
			if (run >= need) {
				const std::size_t firstBad = i + 1 - run;  // index of the first sample of the run
				out.found = true;
				out.kind = a_samples[firstBad].badKind;
				// the first bad sample is number k = firstBad + 1; the edge lies between k - 1 and k steps
				out.distance = (std::max)(0.0f, (static_cast<float>(firstBad) + 0.5f) * a_step - a_radius);
				return out;
			}
		}
		out.distance = static_cast<float>(a_samples.size()) * a_step;
		return out;
	}
}
