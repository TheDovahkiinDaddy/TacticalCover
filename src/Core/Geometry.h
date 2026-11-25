#pragma once

// T2.1: engine-free geometry for cover sensing (PLAN.md section 3.3). Wall-line fit, height classification, top finding
// from per-height hit samples, edge finding from per-step samples. No engine types: everything runs in the unit tests
// without the game.
//
// Conventions (docs/REFERENCE.md 12.1): world units are metres, x and y horizontal, z up. A heading h points along
// (sin h, cos h); the right-hand side of a forward vector f is (f.y, -f.x).

#include <cstddef>
#include <span>

namespace Core
{
	struct Vec2
	{
		float x{ 0.0f };
		float y{ 0.0f };
	};

	struct Vec3
	{
		float x{ 0.0f };
		float y{ 0.0f };
		float z{ 0.0f };
	};

	[[nodiscard]] constexpr Vec2 operator+(const Vec2 a, const Vec2 b) noexcept { return { a.x + b.x, a.y + b.y }; }
	[[nodiscard]] constexpr Vec2 operator-(const Vec2 a, const Vec2 b) noexcept { return { a.x - b.x, a.y - b.y }; }
	[[nodiscard]] constexpr Vec2 operator*(const Vec2 a, const float s) noexcept { return { a.x * s, a.y * s }; }
	[[nodiscard]] constexpr float Dot(const Vec2 a, const Vec2 b) noexcept { return a.x * b.x + a.y * b.y; }
	[[nodiscard]] constexpr float Cross(const Vec2 a, const Vec2 b) noexcept { return a.x * b.y - a.y * b.x; }
	[[nodiscard]] float           Length(Vec2 a) noexcept;
	// A unit vector in the same direction; (0, 0) when the length is below 1e-6.
	[[nodiscard]] Vec2 Normalized(Vec2 a) noexcept;
	// a rotated by +90 degrees (counter-clockwise seen from above, z up).
	[[nodiscard]] constexpr Vec2 Perp(const Vec2 a) noexcept { return { -a.y, a.x }; }

	// Horizontal part of a 3D point.
	[[nodiscard]] constexpr Vec2 XY(const Vec3 a) noexcept { return { a.x, a.y }; }

	// Angle in degrees (0 to 180) between two unit vectors.
	[[nodiscard]] float AngleBetweenDeg(Vec2 a, Vec2 b) noexcept;

	// ---- wall line -----------------------------------------------------------------------------------------------
	struct LineFit
	{
		bool        ok{ false };
		Vec2        centroid{};  // a point on the line
		Vec2        dir{};       // unit tangent; its sign is arbitrary (use NormalToward for orientation)
		float       rms{ 0.0f };     // root mean square distance of the points from the line
		float       maxDev{ 0.0f };  // largest distance of any point from the line
		float       extent{ 0.0f };  // length of the points' spread along the line
		std::size_t count{ 0 };
	};

	// Total least squares line through the points. ok needs at least two distinct points.
	[[nodiscard]] LineFit FitLine(std::span<const Vec2> a_points);

	// The foot of the perpendicular from a_point onto the line through a_linePoint along the unit tangent a_tangent.
	[[nodiscard]] Vec2 ProjectOnLine(Vec2 a_point, Vec2 a_linePoint, Vec2 a_tangent) noexcept;

	// The unit normal of the line (through a_linePoint along a_tangent) that points to the side where a_target lies.
	// A target exactly on the line gives the counter-clockwise normal.
	[[nodiscard]] Vec2 NormalToward(Vec2 a_tangent, Vec2 a_linePoint, Vec2 a_target) noexcept;

	// ---- height --------------------------------------------------------------------------------------------------
	enum class HeightClass
	{
		TooLow,  // below a_lowMin: not cover
		Low,     // from a_lowMin up to a_highMin
		High,    // a_highMin and above
	};

	[[nodiscard]] HeightClass ClassifyHeight(float a_height, float a_lowMin, float a_highMin) noexcept;

	// One cast toward the wall at a known height above the floor. near = it hit, and at about the distance the wall line
	// predicts (a surface that is not the wall does not count).
	struct HeightSample
	{
		float height{ 0.0f };
		bool  near{ false };
	};

	struct TopResult
	{
		bool  ok{ false };      // false when the lowest sample does not hit: nothing stands on the floor here
		float top{ 0.0f };      // estimated height of the top of the wall above the floor
		bool  capped{ false };  // every sample hit: the wall is at least this tall (the estimate is a lower bound)
		int   hits{ 0 };        // length of the run of hits from the bottom
	};

	// a_samples go from the lowest to the highest height with equal spacing. The top is the end of the run of hits that
	// starts at the lowest sample (a ceiling or a sign higher up does not make a wall taller). A sphere of radius a_radius
	// centred at height h touches a top at T when h - a_radius < T, so the top lies in (h_last - r, h_last - r + step]; the
	// estimate is the middle of that interval.
	[[nodiscard]] TopResult FindTop(std::span<const HeightSample> a_samples, float a_radius);

	// ---- edges ---------------------------------------------------------------------------------------------------
	enum class EdgeKind
	{
		None,     // no edge found within the range (the cover goes on at least that far)
		Open,     // the cast missed: the wall ends and there is open space beyond
		Blocked,  // the cast hit something that is not on the wall line: an inside corner or an object in front
	};

	// One cast toward the wall at a step along the tangent, in order of distance from the foot (k = 1, 2, ...).
	struct EdgeSample
	{
		bool     good{ false };  // hit, on the wall line
		EdgeKind badKind{ EdgeKind::Open };  // what a not-good sample means
	};

	struct EdgeResult
	{
		bool     found{ false };
		EdgeKind kind{ EdgeKind::None };
		float    distance{ 0.0f };  // from the foot to the edge; the full range when nothing was found
	};

	// a_samples[k-1] was taken a_step * k from the foot. The edge is where a_missCount samples in a row are not good: its
	// distance is the middle of the last good sample and the first bad one, minus a_radius (a sphere centre still hits a
	// flat end a radius beyond the end itself). a_range is the distance of the last sample.
	[[nodiscard]] EdgeResult FindEdge(std::span<const EdgeSample> a_samples, float a_step, float a_radius, std::size_t a_missCount);
}
