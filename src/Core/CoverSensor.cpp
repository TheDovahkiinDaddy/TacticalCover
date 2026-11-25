#include "Core/CoverSensor.h"

#include <algorithm>
#include <cmath>

namespace Core
{
	namespace
	{
		constexpr float kDegToRad = 0.017453292519943f;
		constexpr float kFitMinExtentFactor = 1.9f;  // the fit points must spread over at least this many fit steps
		constexpr float kWallCastLength = 0.3f;      // casts toward the wall end this far behind the line of centres
		constexpr float kTrackWallCastLength = 0.4f;
	}

	const char* ToString(const Reject a_reason) noexcept
	{
		switch (a_reason) {
		case Reject::None: return "none";
		case Reject::BadInput: return "no intent direction";
		case Reject::ProbeUnavailable: return "probe unavailable";
		case Reject::NoSurface: return "no surface in range";
		case Reject::TooSmallToFit: return "too small to fit a wall line (pole or corner of something)";
		case Reject::NotFlat: return "hit points do not lie on one line";
		case Reject::NoBase: return "wall does not reach the floor";
		case Reject::TooLow: return "too low";
		case Reject::NoFloor: return "no floor at the stand point";
		case Reject::PathBlocked: return "path to the stand point blocked";
		case Reject::TooNarrow: return "too narrow";
		case Reject::TooFarToTheSide: return "the wall is off to the side and not near";
		}
		return "?";
	}

	const char* ToString(const CoverKind a_kind) noexcept
	{
		switch (a_kind) {
		case CoverKind::None: return "none";
		case CoverKind::Low: return "low";
		case CoverKind::High: return "high";
		}
		return "?";
	}

	const char* ToString(const EdgeKind a_kind) noexcept
	{
		switch (a_kind) {
		case EdgeKind::None: return "none within range";
		case EdgeKind::Open: return "open";
		case EdgeKind::Blocked: return "blocked";
		}
		return "?";
	}

	const char* ToString(const TrackStatus a_status) noexcept
	{
		switch (a_status) {
		case TrackStatus::Ok: return "ok";
		case TrackStatus::Corner: return "corner";
		case TrackStatus::Lost: return "lost";
		case TrackStatus::Unavailable: return "unavailable";
		}
		return "?";
	}

	CoverSensor::CoverSensor(IProbe& a_probe, const SensorSettings& a_settings) :
		m_probe(a_probe), m_cfg(a_settings)
	{}

	float CoverSensor::HorizontalDistance(const Vec3& a, const Vec3& b) const
	{
		return Length({ b.x - a.x, b.y - a.y });
	}

	// ---- batches --------------------------------------------------------------------------------------------------
	std::size_t CoverSensor::Add(const Vec3& a_from, const Vec3& a_to)
	{
		Item item;
		item.ticket = m_probe.Submit({ a_from, a_to, m_cfg.probeRadius });
		item.answered = item.ticket == 0;  // not accepted: answered "not available"
		m_batch.items.push_back(item);
		++m_result.casts;
		return m_batch.items.size() - 1;
	}

	bool CoverSensor::BatchDone()
	{
		bool all = true;
		for (auto& item : m_batch.items) {
			if (item.answered) {
				continue;
			}
			if (const auto a = m_probe.Poll(item.ticket)) {
				item.answer = *a;
				item.answered = true;
			} else {
				all = false;
			}
		}
		return all;
	}

	bool CoverSensor::AnyUnavailable() const
	{
		return std::ranges::any_of(m_batch.items, [](const Item& i) { return !i.answer.available; });
	}

	// ---- starting and ending a query ------------------------------------------------------------------------------
	void CoverSensor::Begin(const bool a_track)
	{
		m_batch.items.clear();
		m_result = SensorResult{};
		m_result.isTrack = a_track;
		m_turn = 0.0f;
	}

	void CoverSensor::Cancel()
	{
		m_batch.items.clear();
		m_phase = Phase::Idle;
	}

	SensorResult CoverSensor::TakeResult()
	{
		auto out = m_result;
		if (m_phase == Phase::Done) {
			m_phase = Phase::Idle;
		}
		return out;
	}

	namespace
	{
		// T3.0: the rejections after which the search goes on with the other group of rays (a surface that is not cover says nothing about the next one)
		[[nodiscard]] bool FallsBack(const Reject a_reason) noexcept
		{
			return a_reason == Reject::TooFarToTheSide || a_reason == Reject::TooSmallToFit || a_reason == Reject::NotFlat || a_reason == Reject::NoBase;
		}
	}

	void CoverSensor::Fail(const Reject a_reason)
	{
		if (m_haveAlt && !m_inFallback && FallsBack(a_reason)) {
			// one more try with the best ray of the outer group; everything learned about the first surface is kept for the report
			m_primary = m_c;
			m_primary.kind = CoverKind::None;
			m_primary.why = a_reason;
			m_primaryPts = m_fitPts;
			m_inFallback = true;
			m_haveAlt = false;
			m_primaryHit = m_hit0;
			m_hit0 = m_altHit;
			m_dir0 = m_altDir;
			m_result.searchHit = m_hit0;
			m_c = CoverCandidate{};
			m_c.usedFallback = true;
			m_c.otherWhy = a_reason;
			StartFit();
			return;
		}
		if (m_inFallback) {
			// both attempts were rejected: the report is the first one's, with the second one's reason next to it
			m_c = m_primary;
			m_c.usedFallback = true;
			m_c.otherWhy = a_reason;
			m_result.fitPoints = m_primaryPts;
			m_result.searchHit = m_primaryHit;
		} else {
			m_c.kind = CoverKind::None;
			m_c.why = a_reason;
			m_result.fitPoints = m_fitPts;
		}
		m_result.candidate = m_c;
		m_phase = Phase::Done;
	}

	void CoverSensor::FinishTrack(const TrackStatus a_status)
	{
		m_result.status = a_status;
		m_result.turnDeg = m_turn;
		m_result.candidate = a_status == TrackStatus::Ok ? m_c : m_old;
		m_phase = Phase::Done;
	}

	bool CoverSensor::Find(const Vec3& a_feet, const Vec2& a_intent)
	{
		if (Busy()) {
			return false;
		}
		Begin(false);
		m_feet = a_feet;
		m_c = CoverCandidate{};
		m_haveAlt = false;
		m_inFallback = false;
		m_fitPts.clear();
		m_intent = Normalized(a_intent);
		if (Length(m_intent) < 0.5f) {
			Fail(Reject::BadInput);
			return true;
		}
		StartSearch();
		return true;
	}

	bool CoverSensor::Track(const CoverCandidate& a_current, const Vec3& a_feet)
	{
		if (Busy()) {
			return false;
		}
		Begin(true);
		m_feet = a_feet;
		m_old = a_current;
		m_c = a_current;
		if (a_current.kind == CoverKind::None) {
			FinishTrack(TrackStatus::Lost);
			return true;
		}
		StartTrackFit();
		return true;
	}

	void CoverSensor::Update()
	{
		if (!Busy()) {
			return;
		}
		++m_result.updates;
		if (!BatchDone()) {
			return;
		}
		switch (m_phase) {
		case Phase::Search: EvalSearch(); break;
		case Phase::Fit: EvalFit(); break;
		case Phase::Fit2: EvalFit2(); break;
		case Phase::Ends: EvalEnds(); break;
		case Phase::Profile: EvalProfile(); break;
		case Phase::Edges: EvalEdges(false); break;
		case Phase::TrackFit: EvalTrackFit(); break;
		case Phase::TrackEdges: EvalEdges(true); break;
		default: break;
		}
	}

	// ---- 1. search ------------------------------------------------------------------------------------------------
	void CoverSensor::StartSearch()
	{
		m_phase = Phase::Search;
		m_batch.items.clear();
		m_fanDirs.clear();
		m_fanOffsets.clear();
		const int   n = (std::max)(m_cfg.searchRays, 1);
		const float a0 = std::atan2(m_intent.x, m_intent.y);  // heading convention: forward = (sin a, cos a)
		const Vec3  from = AtHeight(XY(m_feet), m_cfg.searchHeight);
		for (int i = 0; i < n; ++i) {
			const float off = n > 1 ? m_cfg.searchHalfAngleDeg * (2.0f * static_cast<float>(i) / static_cast<float>(n - 1) - 1.0f) : 0.0f;
			const float a = a0 + off * kDegToRad;
			const Vec2  dir{ std::sin(a), std::cos(a) };
			m_fanDirs.push_back(dir);
			m_fanOffsets.push_back(off);
			Add(from, { from.x + dir.x * m_cfg.maxSnapDistance, from.y + dir.y * m_cfg.maxSnapDistance, from.z });
		}
	}

	void CoverSensor::EvalSearch()
	{
		if (AnyUnavailable()) {
			Fail(Reject::ProbeUnavailable);
			return;
		}
		// T3.0 "ahead first": the nearest hit among the rays within aheadHalfAngleDeg of the intent decides; the outer rays only when none of the
		// middle ones hits. The best outer ray is kept for one fallback (Fail) in case the surface ahead turns out not to be cover.
		int   bestAhead = -1;
		int   bestOuter = -1;
		float scoreAhead = 1e30f;
		float scoreOuter = 1e30f;
		for (std::size_t i = 0; i < m_batch.items.size(); ++i) {
			const auto& a = m_batch.items[i].answer;
			if (!a.hit) {
				continue;
			}
			const float surface = Length({ a.stop.x - m_feet.x, a.stop.y - m_feet.y }) + m_cfg.probeRadius;
			const float score = surface * (1.0f + m_cfg.searchAngleWeight * std::fabs(m_fanOffsets[i]) / (std::max)(m_cfg.searchHalfAngleDeg, 1.0f));
			const bool  ahead = std::fabs(m_fanOffsets[i]) <= m_cfg.aheadHalfAngleDeg + 1e-3f;
			if (ahead && score < scoreAhead) {
				scoreAhead = score;
				bestAhead = static_cast<int>(i);
			} else if (!ahead && score < scoreOuter) {
				scoreOuter = score;
				bestOuter = static_cast<int>(i);
			}
		}
		const int best = bestAhead >= 0 ? bestAhead : bestOuter;
		if (best < 0) {
			Fail(Reject::NoSurface);
			return;
		}
		if (bestAhead >= 0 && bestOuter >= 0) {
			m_haveAlt = true;
			m_altHit = XY(m_batch.items[static_cast<std::size_t>(bestOuter)].answer.stop);
			m_altDir = m_fanDirs[static_cast<std::size_t>(bestOuter)];
		}
		m_hit0 = XY(m_batch.items[static_cast<std::size_t>(best)].answer.stop);
		m_dir0 = m_fanDirs[static_cast<std::size_t>(best)];
		m_result.searchHit = m_hit0;
		StartFit();
	}

	// ---- 2. wall line ---------------------------------------------------------------------------------------------
	void CoverSensor::StartFit()
	{
		m_phase = Phase::Fit;
		m_batch.items.clear();
		const Vec2 p = Perp(m_dir0);
		const Vec2 fromXY = XY(m_feet);
		const Vec3 from = AtHeight(fromXY, m_cfg.searchHeight);
		const float s = m_cfg.wallFitStep;
		for (const float off : { -2.0f * s, -s, s, 2.0f * s }) {
			const Vec2  target = m_hit0 + p * off;
			const Vec2  v = target - fromXY;
			const float len = Length(v);
			const Vec2  u = len > 1e-4f ? v * (1.0f / len) : m_dir0;
			Add(from, { from.x + u.x * (len + 0.4f), from.y + u.y * (len + 0.4f), from.z });
		}
	}

	void CoverSensor::EvalFit()
	{
		if (AnyUnavailable()) {
			Fail(Reject::ProbeUnavailable);
			return;
		}
		m_fitPts.assign(1, m_hit0);
		bool hit[4]{};
		for (std::size_t i = 0; i < 4; ++i) {
			hit[i] = m_batch.items[i].answer.hit;  // 0 and 1 at -2 and -1 steps, 2 and 3 at +1 and +2 steps
			if (hit[i]) {
				m_fitPts.push_back(XY(m_batch.items[i].answer.stop));
			}
		}
		// T2.4: the wall ends beside the first hit when the samples miss on one side only: take the missing ones on the other side (out to 4 steps =
		// 0.60 m), where the wall goes on. Misses on both sides are a pole or a corner of something, and stay that.
		const bool leftMiss = !(hit[0] && hit[1]);
		const bool rightMiss = !(hit[2] && hit[3]);
		if (leftMiss != rightMiss) {
			StartFit2(leftMiss ? 1.0f : -1.0f);
			return;
		}
		FinishFit();
	}

	// the samples at 3 and 4 steps on the side where the wall goes on
	void CoverSensor::StartFit2(const float a_side)
	{
		m_phase = Phase::Fit2;
		m_batch.items.clear();
		const Vec2 p = Perp(m_dir0);
		const Vec2 fromXY = XY(m_feet);
		const Vec3 from = AtHeight(fromXY, m_cfg.searchHeight);
		const float s = m_cfg.wallFitStep;
		for (const float k : { 3.0f, 4.0f }) {
			const Vec2  target = m_hit0 + p * (a_side * k * s);
			const Vec2  v = target - fromXY;
			const float len = Length(v);
			const Vec2  u = len > 1e-4f ? v * (1.0f / len) : m_dir0;
			Add(from, { from.x + u.x * (len + 0.4f), from.y + u.y * (len + 0.4f), from.z });
		}
	}

	void CoverSensor::EvalFit2()
	{
		if (AnyUnavailable()) {
			Fail(Reject::ProbeUnavailable);
			return;
		}
		for (const auto& item : m_batch.items) {
			if (item.answer.hit) {
				m_fitPts.push_back(XY(item.answer.stop));
			}
		}
		FinishFit();
	}

	void CoverSensor::FinishFit()
	{
		if (m_fitPts.size() < 3) {
			Fail(Reject::TooSmallToFit);
			return;
		}
		const auto fit = FitLine(m_fitPts);
		if (!fit.ok || fit.extent < kFitMinExtentFactor * m_cfg.wallFitStep) {
			Fail(Reject::TooSmallToFit);
			return;
		}
		m_c.fitRms = fit.rms;
		if (fit.rms > m_cfg.wallFitMaxRms) {
			Fail(Reject::NotFlat);
			return;
		}
		const Vec2 feetXY = XY(m_feet);
		const Vec2 n = NormalToward(fit.dir, fit.centroid, feetXY);
		m_c.normal = n;
		m_c.tangent = Perp(n);
		m_foot = ProjectOnLine(feetXY, fit.centroid, m_c.tangent);  // the raw foot; Ends may move it along the wall
		m_c.linePoint = m_foot;
		m_c.distance = Length(feetXY - m_foot) + m_cfg.probeRadius;  // the surface lies a radius beyond the line of centres
		StartEnds();
	}

	// ---- 2b. the ends near the first hit (T2.4) -------------------------------------------------------------------------
	// Casts toward the wall at steps along the tangent from the first hit, both ways, far enough to cover the raw foot and the edge margin
	// beyond it, at the search height. They give the ends of the wall near where the player is, so that the foot (and with it the stand point)
	// can be put fEdgeMargin inside an open end, and so that the distance and direction of the candidate are those of the point the player
	// would stand against. The full edge scan of step 4 then runs from that point.
	void CoverSensor::StartEnds()
	{
		m_phase = Phase::Ends;
		m_batch.items.clear();
		const Vec2 n = m_c.normal;
		const Vec2 t = m_c.tangent;
		m_tHit = Dot(m_hit0 - m_foot, t);
		m_edgeStep = (std::max)(m_cfg.edgeProbeStep, 0.02f);
		m_nEnd = (std::min)(static_cast<std::size_t>(std::ceil((std::fabs(m_tHit) + m_cfg.edgeMargin + m_edgeStep) / m_edgeStep)), static_cast<std::size_t>(20));
		for (const float side : { -1.0f, 1.0f }) {
			for (std::size_t k = 1; k <= m_nEnd; ++k) {
				const Vec2 xy = m_foot + t * (m_tHit + side * static_cast<float>(k) * m_edgeStep);
				Add(AtHeight(xy + n * m_cfg.standoff, m_cfg.searchHeight), AtHeight(xy - n * kWallCastLength, m_cfg.searchHeight));
			}
		}
	}

	void CoverSensor::EvalEnds()
	{
		if (AnyUnavailable()) {
			Fail(Reject::ProbeUnavailable);
			return;
		}
		const Vec2 n = m_c.normal;
		const Vec2 t = m_c.tangent;
		EdgeResult res[2];
		for (int s = 0; s < 2; ++s) {
			const float             side = s == 0 ? -1.0f : 1.0f;
			std::vector<EdgeSample> samples;
			for (std::size_t k = 1; k <= m_nEnd; ++k) {
				const auto& a = m_batch.items[static_cast<std::size_t>(s) * m_nEnd + k - 1].answer;
				const Vec2  start = m_foot + t * (m_tHit + side * static_cast<float>(k) * m_edgeStep) + n * m_cfg.standoff;
				const bool  good = a.hit && std::fabs(Length(XY(a.stop) - start) - m_cfg.standoff) <= m_cfg.nearTolerance;
				samples.push_back({ good, a.hit ? EdgeKind::Blocked : EdgeKind::Open });
			}
			res[s] = FindEdge(samples, m_edgeStep, m_cfg.probeRadius, static_cast<std::size_t>((std::max)(m_cfg.edgeMissCount, 1)));
		}
		// the wall's extent along the tangent, from the raw foot at 0: only an open end (a miss) holds the foot back, not an inside corner
		const float eMinus = m_tHit - res[0].distance;
		const float ePlus = m_tHit + res[1].distance;
		const bool  openMinus = res[0].found && res[0].kind == EdgeKind::Open;
		const bool  openPlus = res[1].found && res[1].kind == EdgeKind::Open;
		const float lo = openMinus ? eMinus + m_cfg.edgeMargin : -1e9f;
		const float hi = openPlus ? ePlus - m_cfg.edgeMargin : 1e9f;
		float       shift = 0.0f;
		if (lo > hi) {
			shift = 0.5f * (eMinus + ePlus);  // narrower than two margins: the middle
		} else {
			shift = std::clamp(0.0f, lo, hi);
		}
		m_c.clampShift = shift;
		m_foot = m_foot + t * shift;
		m_c.linePoint = m_foot;
		const Vec2 feetXY = XY(m_feet);
		m_c.distance = Length(feetXY - m_foot) + m_cfg.probeRadius;
		m_stand = m_foot + n * (m_cfg.capsuleRadius + m_cfg.standGap - m_cfg.probeRadius);

		// T2.4 item 3: direction and distance are those of the point of the wall surface the player would stand against
		const Vec2  anchor = m_foot - n * m_cfg.probeRadius;
		const Vec2  v = anchor - feetXY;
		const float dist = Length(v);
		m_c.wallPointDistance = dist;
		m_c.wallPointAngleDeg = dist > 1e-3f ? AngleBetweenDeg(v * (1.0f / dist), m_intent) : 0.0f;
		if (m_c.wallPointAngleDeg > m_cfg.sideSnapAngleDeg && dist > m_cfg.sideSnapDistance) {
			Fail(Reject::TooFarToTheSide);
			return;
		}
		StartProfile();
	}

	// ---- 3 and 4. height, floor, path -----------------------------------------------------------------------------
	void CoverSensor::StartProfile()
	{
		m_phase = Phase::Profile;
		m_batch.items.clear();
		const Vec2 n = m_c.normal;
		const Vec2 start = m_foot + n * m_cfg.standoff;
		const Vec2 end = m_foot - n * kWallCastLength;
		m_nHeights = static_cast<std::size_t>((m_cfg.maxMeasuredHeight - m_cfg.heightStart) / m_cfg.heightStep + 1e-3f) + 1;
		for (std::size_t i = 0; i < m_nHeights; ++i) {
			const float h = m_cfg.heightStart + static_cast<float>(i) * m_cfg.heightStep;
			Add(AtHeight(start, h), AtHeight(end, h));
		}
		// the floor under the stand point
		m_iFloor = Add(AtHeight(m_stand, 1.0f), AtHeight(m_stand, -2.0f));
		// the way there, at knee and chest height
		m_iPath = m_batch.items.size();
		m_nPath = 0;
		if (Length(m_stand - XY(m_feet)) > 0.05f) {
			for (const float h : { 0.4f, 1.0f }) {
				Add(AtHeight(XY(m_feet), h), AtHeight(m_stand, h));
				++m_nPath;
			}
		}
	}

	void CoverSensor::EvalProfile()
	{
		if (AnyUnavailable()) {
			Fail(Reject::ProbeUnavailable);
			return;
		}
		const Vec2 start = m_foot + m_c.normal * m_cfg.standoff;
		std::vector<HeightSample> samples;
		for (std::size_t i = 0; i < m_nHeights; ++i) {
			const auto& a = m_batch.items[i].answer;
			const float dist = Length(XY(a.stop) - start);
			samples.push_back({ m_cfg.heightStart + static_cast<float>(i) * m_cfg.heightStep, a.hit && std::fabs(dist - m_cfg.standoff) <= m_cfg.nearTolerance });
		}
		const auto top = FindTop(samples, m_cfg.probeRadius);
		if (!top.ok) {
			Fail(Reject::NoBase);
			return;
		}
		const auto& fl = m_batch.items[m_iFloor].answer;
		if (!fl.hit) {
			Fail(Reject::NoFloor);
			return;
		}
		m_c.floorZ = fl.stop.z - m_cfg.probeRadius;
		if (std::fabs(m_c.floorZ - m_feet.z) > m_cfg.maxFloorStep) {
			Fail(Reject::NoFloor);
			return;
		}
		m_c.height = (m_feet.z + top.top) - m_c.floorZ;
		m_c.heightCapped = top.capped;
		// A top above the highest cast is only known as a lower bound, measured from the player's feet; against a stand floor a little higher than the
		// feet it can read under fHighCoverMinHeight (play check P2 #69: 1.60 m, "at least"). It is High then: the wall passes every cast.
		const auto cls = (m_c.heightCapped && m_c.height >= m_cfg.lowCoverMinHeight) ? HeightClass::High : ClassifyHeight(m_c.height, m_cfg.lowCoverMinHeight, m_cfg.highCoverMinHeight);
		m_c.canPeekOver = !m_c.heightCapped && m_c.height <= m_cfg.overPeekMaxHeight;
		m_c.anchor = { m_foot.x - m_c.normal.x * m_cfg.probeRadius, m_foot.y - m_c.normal.y * m_cfg.probeRadius, m_c.floorZ };
		m_c.standPoint = { m_stand.x, m_stand.y, m_c.floorZ };
		if (cls == HeightClass::TooLow) {
			Fail(Reject::TooLow);
			return;
		}
		for (std::size_t i = 0; i < m_nPath; ++i) {
			if (m_batch.items[m_iPath + i].answer.hit) {
				Fail(Reject::PathBlocked);
				return;
			}
		}
		m_c.kind = cls == HeightClass::Low ? CoverKind::Low : CoverKind::High;
		m_c.edgeHeight = std::clamp(top.top - 0.2f, 0.3f, 1.0f);
		StartEdges(m_cfg.edgeSearchRange, false);
	}

	// ---- 5. edges -------------------------------------------------------------------------------------------------
	void CoverSensor::StartEdges(const float a_range, const bool a_track)
	{
		m_phase = a_track ? Phase::TrackEdges : Phase::Edges;
		m_batch.items.clear();
		m_edgeStep = (std::max)(m_cfg.edgeProbeStep, 0.02f);
		m_nEdge = static_cast<std::size_t>(a_range / m_edgeStep + 1e-3f);
		const Vec2 n = m_c.normal;
		const Vec2 t = m_c.tangent;
		for (const float side : { -1.0f, 1.0f }) {
			for (std::size_t k = 1; k <= m_nEdge; ++k) {
				const Vec2 xy = m_foot + t * (side * static_cast<float>(k) * m_edgeStep);
				Add(AtHeight(xy + n * m_cfg.standoff, m_c.edgeHeight), AtHeight(xy - n * (a_track ? kTrackWallCastLength : kWallCastLength), m_c.edgeHeight));
			}
		}
	}

	void CoverSensor::EvalEdges(const bool a_track)
	{
		if (AnyUnavailable()) {
			if (a_track) {
				FinishTrack(TrackStatus::Unavailable);
			} else {
				Fail(Reject::ProbeUnavailable);
			}
			return;
		}
		const float tol = a_track ? m_cfg.trackTolerance : m_cfg.nearTolerance;
		const Vec2  n = m_c.normal;
		const Vec2  t = m_c.tangent;
		EdgeResult  res[2];
		for (int s = 0; s < 2; ++s) {
			const float               side = s == 0 ? -1.0f : 1.0f;
			std::vector<EdgeSample>   samples;
			for (std::size_t k = 1; k <= m_nEdge; ++k) {
				const auto& a = m_batch.items[static_cast<std::size_t>(s) * m_nEdge + k - 1].answer;
				const Vec2  start = m_foot + t * (side * static_cast<float>(k) * m_edgeStep) + n * m_cfg.standoff;
				const float dist = Length(XY(a.stop) - start);
				const bool  good = a.hit && std::fabs(dist - m_cfg.standoff) <= tol;
				samples.push_back({ good, a.hit ? EdgeKind::Blocked : EdgeKind::Open });
			}
			res[s] = FindEdge(samples, m_edgeStep, m_cfg.probeRadius, static_cast<std::size_t>((std::max)(m_cfg.edgeMissCount, 1)));
		}
		m_c.edgeLeft = res[0].distance;
		m_c.edgeLeftKind = res[0].kind;
		m_c.edgeRight = res[1].distance;
		m_c.edgeRightKind = res[1].kind;
		if (a_track) {
			FinishTrack(TrackStatus::Ok);
			return;
		}
		if (m_c.edgeLeft + m_c.edgeRight < m_cfg.minCoverWidth) {
			Fail(Reject::TooNarrow);
			return;
		}
		m_result.candidate = m_c;
		m_result.fitPoints = m_fitPts;
		m_phase = Phase::Done;
	}

	// ---- Track ----------------------------------------------------------------------------------------------------
	void CoverSensor::StartTrackFit()
	{
		m_phase = Phase::TrackFit;
		m_batch.items.clear();
		const Vec2 n0 = m_old.normal;
		const Vec2 t0 = m_old.tangent;
		const Vec2 f0 = ProjectOnLine(XY(m_feet), m_old.linePoint, t0);
		m_foot = f0;
		const float s = m_cfg.wallFitStep;
		const float h = m_old.edgeHeight;
		for (const float off : { -2.0f * s, -s, 0.0f, s, 2.0f * s }) {
			const Vec2 xy = f0 + t0 * off;
			Add(AtHeight(xy + n0 * m_cfg.standoff, h), AtHeight(xy - n0 * kTrackWallCastLength, h));
		}
		const Vec2 stand0 = f0 + n0 * (m_cfg.capsuleRadius + m_cfg.standGap - m_cfg.probeRadius);
		m_iFloor = Add(AtHeight(stand0, 1.0f), AtHeight(stand0, -2.0f));
		// the height check: the wall must still reach just below its old top
		const float hb = (std::max)(0.3f, (std::min)(m_old.height - 0.15f, m_cfg.maxMeasuredHeight));
		m_iBelow = Add(AtHeight(f0 + n0 * m_cfg.standoff, hb), AtHeight(f0 - n0 * kTrackWallCastLength, hb));
	}

	void CoverSensor::EvalTrackFit()
	{
		if (AnyUnavailable()) {
			FinishTrack(TrackStatus::Unavailable);
			return;
		}
		const Vec2 n0 = m_old.normal;
		const Vec2 t0 = m_old.tangent;
		const float s = m_cfg.wallFitStep;
		const float offs[5] = { -2.0f * s, -s, 0.0f, s, 2.0f * s };
		std::vector<Vec2> pts;
		for (std::size_t i = 0; i < 5; ++i) {
			const auto& a = m_batch.items[i].answer;
			const Vec2  start = m_foot + t0 * offs[i] + n0 * m_cfg.standoff;
			if (a.hit && std::fabs(Length(XY(a.stop) - start) - m_cfg.standoff) <= m_cfg.trackTolerance) {
				pts.push_back(XY(a.stop));
			}
		}
		const auto& below = m_batch.items[m_iBelow].answer;
		const Vec2  belowStart = m_foot + n0 * m_cfg.standoff;
		const bool  belowNear = below.hit && std::fabs(Length(XY(below.stop) - belowStart) - m_cfg.standoff) <= m_cfg.trackTolerance;
		const auto& fl = m_batch.items[m_iFloor].answer;
		const float floorZ = fl.stop.z - m_cfg.probeRadius;
		if (pts.size() < 3 || !belowNear || !fl.hit || std::fabs(floorZ - m_feet.z) > m_cfg.maxFloorStep) {
			m_result.why = "wall points " + std::to_string(pts.size()) + " of 5 (need 3), height check " + (belowNear ? "ok" : "FAILED") + ", floor " + (fl.hit ? "hit" : "MISSED") + ", floor step " + std::to_string(floorZ - m_feet.z) + " m (limit " + std::to_string(m_cfg.maxFloorStep) + ")";
			FinishTrack(TrackStatus::Lost);
			return;
		}
		const auto fit = FitLine(pts);
		if (!fit.ok || fit.extent < kFitMinExtentFactor * s) {
			FinishTrack(TrackStatus::Lost);
			return;
		}
		const Vec2 feetXY = XY(m_feet);
		const Vec2 n1 = NormalToward(fit.dir, fit.centroid, feetXY);
		m_turn = AngleBetweenDeg(n0, n1);
		if (fit.rms > m_cfg.wallFitMaxRms * 1.5f || m_turn > m_cfg.maxNormalTurnDeg) {
			FinishTrack(TrackStatus::Corner);
			return;
		}
		m_c.normal = n1;
		m_c.tangent = Perp(n1);
		m_foot = ProjectOnLine(feetXY, fit.centroid, m_c.tangent);
		m_c.linePoint = m_foot;
		m_c.fitRms = fit.rms;
		m_c.floorZ = floorZ;
		m_c.distance = Length(feetXY - m_foot) + m_cfg.probeRadius;  // the surface lies a radius beyond the line of centres
		m_stand = m_foot + n1 * (m_cfg.capsuleRadius + m_cfg.standGap - m_cfg.probeRadius);
		m_c.anchor = { m_foot.x - n1.x * m_cfg.probeRadius, m_foot.y - n1.y * m_cfg.probeRadius, floorZ };
		m_c.standPoint = { m_stand.x, m_stand.y, floorZ };
		StartEdges(m_cfg.trackEdgeRange, true);
	}
}
