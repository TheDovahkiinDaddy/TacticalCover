// Engine-free tests for Core/Geometry (T2.1): wall-line fit, height classification, top finding, edge finding.

#include "Core/Geometry.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
	int g_failures = 0;

#define CHECK(cond)                                                     \
	do {                                                                \
		if (!(cond)) {                                                  \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++g_failures;                                               \
		}                                                               \
	} while (false)

	bool Near(const float a, const float b, const float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

	void TestVec()
	{
		using namespace Core;
		CHECK(Near(Length({ 3, 4 }), 5.0f));
		CHECK(Near(Normalized({ 0, 2 }).y, 1.0f));
		CHECK(Length(Normalized({ 0, 0 })) == 0.0f);
		const auto p = Perp({ 1, 0 });
		CHECK(Near(p.x, 0.0f) && Near(p.y, 1.0f));
		CHECK(Near(AngleBetweenDeg({ 1, 0 }, { 0, 1 }), 90.0f));
		CHECK(Near(AngleBetweenDeg({ 1, 0 }, { -1, 0 }), 180.0f));
		CHECK(Near(AngleBetweenDeg({ 1, 0 }, { 1, 0 }), 0.0f));
	}

	void TestFitLine()
	{
		using namespace Core;
		// a wall along x at y = 2
		const std::vector<Vec2> flat{ { -0.3f, 2 }, { -0.15f, 2 }, { 0, 2 }, { 0.15f, 2 }, { 0.3f, 2 } };
		const auto              f = FitLine(flat);
		CHECK(f.ok);
		CHECK(Near(std::fabs(f.dir.x), 1.0f) && Near(f.dir.y, 0.0f));
		CHECK(Near(f.rms, 0.0f) && Near(f.extent, 0.6f) && f.count == 5);
		CHECK(Near(f.centroid.y, 2.0f));

		// a wall at 30 degrees with 1 cm noise
		const float            a = 30.0f * 0.017453292f;
		const Vec2             d{ std::cos(a), std::sin(a) };
		std::vector<Vec2>      tilt;
		const float            noise[5] = { 0.01f, -0.01f, 0.0f, 0.01f, -0.01f };
		for (int i = 0; i < 5; ++i) {
			const float s = -0.3f + 0.15f * static_cast<float>(i);
			tilt.push_back({ 5.0f + d.x * s - d.y * noise[i], 1.0f + d.y * s + d.x * noise[i] });
		}
		const auto t = FitLine(tilt);
		CHECK(t.ok && Near(std::fabs(Dot(t.dir, d)), 1.0f, 2e-3f));
		CHECK(t.rms < 0.012f && t.maxDev < 0.013f);

		// a vertical line (dir along y) must not break the angle formula
		const std::vector<Vec2> vert{ { 1, -1 }, { 1, 0 }, { 1, 2 } };
		const auto              v = FitLine(vert);
		CHECK(v.ok && Near(std::fabs(v.dir.y), 1.0f) && Near(v.extent, 3.0f));

		// not a line: an arc of radius 0.5 over a 0.6 m span has a large deviation, a gentle arc of radius 4 does not
		std::vector<Vec2> tight, gentle;
		for (int i = 0; i < 5; ++i) {
			const float s = -0.3f + 0.15f * static_cast<float>(i);
			tight.push_back({ s, 0.5f - std::sqrt(0.25f - s * s) });
			gentle.push_back({ s, 4.0f - std::sqrt(16.0f - s * s) });
		}
		CHECK(FitLine(tight).rms > 0.02f);
		CHECK(FitLine(gentle).rms < 0.006f);

		// degenerate input
		CHECK(!FitLine({}).ok);
		const std::vector<Vec2> one{ { 1, 1 } };
		CHECK(!FitLine(one).ok);
		const std::vector<Vec2> same{ { 1, 1 }, { 1, 1 }, { 1, 1 } };
		CHECK(!FitLine(same).ok);
	}

	void TestNormal()
	{
		using namespace Core;
		// wall along x through (0, 2); the player at the origin is on the -y side
		auto n = NormalToward({ 1, 0 }, { 0, 2 }, { 0, 0 });
		CHECK(Near(n.x, 0.0f) && Near(n.y, -1.0f));
		n = NormalToward({ -1, 0 }, { 0, 2 }, { 0, 0 });  // the sign of the tangent does not matter
		CHECK(Near(n.y, -1.0f));
		n = NormalToward({ 1, 0 }, { 0, 2 }, { 3, 5 });
		CHECK(Near(n.y, 1.0f));
		const auto foot = ProjectOnLine({ 3, 0 }, { 0, 2 }, { 1, 0 });
		CHECK(Near(foot.x, 3.0f) && Near(foot.y, 2.0f));
		// the tangent is the player's right-hand side when facing the wall: wall at +x, normal (-1, 0), facing +x, right is -y
		const auto t = Perp(Vec2{ -1, 0 });
		CHECK(Near(t.x, 0.0f) && Near(t.y, -1.0f));
	}

	void TestClassify()
	{
		using Core::HeightClass;
		CHECK(Core::ClassifyHeight(0.30f, 0.7f, 1.4f) == HeightClass::TooLow);
		CHECK(Core::ClassifyHeight(0.69f, 0.7f, 1.4f) == HeightClass::TooLow);
		CHECK(Core::ClassifyHeight(0.70f, 0.7f, 1.4f) == HeightClass::Low);
		CHECK(Core::ClassifyHeight(1.39f, 0.7f, 1.4f) == HeightClass::Low);
		CHECK(Core::ClassifyHeight(1.40f, 0.7f, 1.4f) == HeightClass::High);
		CHECK(Core::ClassifyHeight(2.5f, 0.7f, 1.4f) == HeightClass::High);
	}

	std::vector<Core::HeightSample> Samples(const float a_top, const float a_radius, const float a_start = 0.3f, const float a_step = 0.15f, const float a_max = 1.8f)
	{
		// the casts of a wall with its top at a_top: a sphere at height h hits when h - r < top
		std::vector<Core::HeightSample> out;
		for (float h = a_start; h <= a_max + 1e-4f; h += a_step) {
			out.push_back({ h, h - a_radius < a_top });
		}
		return out;
	}

	void TestTop()
	{
		const float r = 0.1f;
		for (const float top : { 0.55f, 0.75f, 0.9f, 1.0f, 1.2f, 1.45f }) {
			const auto res = Core::FindTop(Samples(top, r), r);
			CHECK(res.ok && !res.capped);
			CHECK(std::fabs(res.top - top) <= 0.08f);  // within half a step
		}
		// taller than the highest cast: capped
		const auto tall = Core::FindTop(Samples(2.5f, r), r);
		CHECK(tall.ok && tall.capped && tall.top > 1.6f);
		// nothing at the lowest cast: not standing on the floor
		auto up = Samples(1.0f, r);
		up[0].near = false;
		CHECK(!Core::FindTop(up, r).ok);
		CHECK(!Core::FindTop({}, r).ok);
		// a ceiling or a sign higher up does not make the wall taller: hits at 0.3 to 0.6, a gap, hits again from 1.5
		auto gap = Samples(0.6f, r);
		gap[8].near = gap[9].near = gap[10].near = true;
		const auto g = Core::FindTop(gap, r);
		CHECK(g.ok && !g.capped && g.top < 0.7f);
		CHECK(g.hits == 3);
		// a single sample
		const std::vector<Core::HeightSample> one{ { 0.3f, true } };
		CHECK(Core::FindTop(one, r).ok);
	}

	std::vector<Core::EdgeSample> Edges(const std::size_t a_n, const std::size_t a_firstBad, const Core::EdgeKind a_kind)
	{
		std::vector<Core::EdgeSample> out;
		for (std::size_t k = 1; k <= a_n; ++k) {
			out.push_back({ k < a_firstBad, a_kind });
		}
		return out;
	}

	void TestEdge()
	{
		using Core::EdgeKind;
		const float step = 0.15f, r = 0.1f;
		// the first bad sample is number 8 (1.2 m from the foot): the edge is between 1.05 and 1.2, less the radius
		auto e = Core::FindEdge(Edges(20, 8, EdgeKind::Open), step, r, 1);
		CHECK(e.found && e.kind == EdgeKind::Open && Near(e.distance, 7.5f * step - r));
		e = Core::FindEdge(Edges(20, 8, EdgeKind::Blocked), step, r, 1);
		CHECK(e.found && e.kind == EdgeKind::Blocked);
		// no bad sample: no edge within the range
		e = Core::FindEdge(Edges(20, 99, EdgeKind::Open), step, r, 1);
		CHECK(!e.found && e.kind == EdgeKind::None && Near(e.distance, 3.0f));
		// the very first sample bad: the edge is at the foot
		e = Core::FindEdge(Edges(20, 1, EdgeKind::Open), step, r, 1);
		CHECK(e.found && Near(e.distance, 0.0f));
		// two misses in a row are needed: one gap (a railing bar) is ignored
		auto gap = Edges(20, 99, EdgeKind::Open);
		gap[4].good = false;
		CHECK(!Core::FindEdge(gap, step, r, 2).found);
		CHECK(Core::FindEdge(gap, step, r, 1).found);
		gap[5].good = false;
		const auto two = Core::FindEdge(gap, step, r, 2);
		CHECK(two.found && Near(two.distance, 4.5f * step - r));  // at the first of the two
		// a run at the very end that is shorter than the miss count is not an edge
		auto tail = Edges(20, 99, EdgeKind::Open);
		tail[19].good = false;
		CHECK(!Core::FindEdge(tail, step, r, 2).found);
		CHECK(!Core::FindEdge({}, step, r, 1).found);
	}
}

int RunGeometryTests()
{
	g_failures = 0;
	TestVec();
	TestFitLine();
	TestNormal();
	TestClassify();
	TestTop();
	TestEdge();
	std::printf("geometry tests: %s\n", g_failures == 0 ? "ok" : "FAILED");
	return g_failures;
}
