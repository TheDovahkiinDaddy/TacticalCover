#pragma once

// T2.2: CoverSensor. Finds a cover candidate in front of the player (Find) and re-estimates it while the player moves along it
// (Track), with nothing but sphere casts (PLAN.md section 3.3). Engine-free: the casts go through IProbe, which the plugin
// implements on top of Engine/WorldProbe and the tests implement with a fake world.
//
// Casts answer a frame later (T2.0), so the sensor is a small state machine: each phase submits a batch of casts and the next
// phase starts when all of them have been answered. Call Update() every tick; start a query with Find() or Track(); when
// HasResult() is true, TakeResult() returns it.
//
// Find, in phases (the numbers are PLAN.md 3.3 steps):
//   Search   (1) a fan of casts at SearchHeight around the intent direction; the nearest hit, weighted against the angle
//   Fit      (2) four more casts either side of it; a line through the hit points gives tangent and normal
//   Profile  (3) casts toward the wall at heights from 0.3 m up give the top; (4) a floor cast and the path to the stand point
//   Edges    (5) casts toward the wall at steps along the tangent in both directions; the first miss is an edge
// Track is Fit and Edges again, around the player's present position, plus a floor and a height check (3.3 step 6).
//
// Sphere casts report where the sphere's centre stopped, a radius short of the surface. The centre points of a flat wall lie on
// a line one radius in front of it, so the fitted line is that line; surface, stand point and edge distances correct for it.

#include <cstdint>
#include <string>
#include <optional>
#include <vector>

#include "Core/Geometry.h"

namespace Core
{
	// Every value is an INI setting ([Sensor], PLAN.md section 3.7; the ones PLAN.md does not name are marked "T2.2").
	struct SensorSettings
	{
		float maxSnapDistance{ 2.5f };     // farthest wall surface the search looks for
		float probeRadius{ 0.10f };        // sphere radius of every cast
		float lowCoverMinHeight{ 0.70f };  // below: not cover
		float highCoverMinHeight{ 1.70f };  // from here up: high cover (T2.4: was 1.40; standing cover must hide the head)
		float overPeekMaxHeight{ 1.25f };   // T2.4: up to this top height the player can stand up and aim over the cover (CoverCandidate::canPeekOver)
		float minCoverWidth{ 0.60f };      // narrower than this (both sides of the foot added): rejected, so poles are not cover
		float edgeProbeStep{ 0.15f };      // spacing of the edge casts
		float edgeMargin{ 0.25f };         // for the movement constraint (Phase 3); not used by the sensor itself
		float trackRateHz{ 20.0f };        // how often the owner should call Track

		float searchHeight{ 0.5f };         // T2.2: height above the feet of the search casts. 0.5 catches low cover that a chest-high cast passes over
		int   searchRays{ 9 };              // T2.2: casts in the fan
		float searchHalfAngleDeg{ 50.0f };  // T2.2: the fan reaches this far either side of the intent
		float searchAngleWeight{ 0.5f };    // T2.2: a hit at the edge of the fan counts (1 + weight) times as far as one straight ahead; larger prefers what the player points at
		float wallFitStep{ 0.15f };         // T2.2: the fit casts go this far (and twice this far) either side of the nearest hit
		float wallFitMaxRms{ 0.04f };       // T2.2: the hit points must lie this close to one line (rms), else "not a flat surface"
		float standoff{ 0.8f };             // T2.2: height, width and edge casts start this far in front of the wall line
		float nearTolerance{ 0.15f };       // T2.2: a hit counts as "the wall" when its distance is within this of the expected one
		float heightStep{ 0.15f };          // T2.2: spacing of the height casts (PLAN.md says 0.3; finer gives the top to about 0.08 m)
		float heightStart{ 0.30f };         // T2.2: lowest height cast
		float maxMeasuredHeight{ 1.80f };   // T2.2: highest height cast; a wall that tall is reported as "at least"
		float capsuleRadius{ 0.30f };       // T2.2: the player's body radius; NOT verified against the engine
		float standGap{ 0.05f };            // T2.2: space left between the body and the wall
		float maxFloorStep{ 0.35f };        // T2.2: the floor at the stand point may differ from the player's feet by this much
		float edgeSearchRange{ 4.0f };      // T2.2: Find looks for the ends of the cover this far either side of the foot
		float trackEdgeRange{ 2.0f };       // T2.2: Track looks this far (it runs at 20 Hz)
		int   edgeMissCount{ 1 };           // T2.2: casts in a row that must fail for an edge (PLAN.md: the first miss)
		float maxNormalTurnDeg{ 45.0f };    // T2.2: Track: a turn of the normal between two updates beyond this is a corner
		float trackTolerance{ 0.30f };      // T2.2: Track: a hit counts as "the wall" within this of the expected distance
		float sideSnapAngleDeg{ 60.0f };    // T2.4: a wall whose standing point lies more than this far from the intent is taken only when it is near ...
		float sideSnapDistance{ 1.0f };     // T2.4: ... within this distance of the player (measured to the point of the wall the player would stand against)
		float aheadHalfAngleDeg{ 20.0f };   // T3.0: a ray this close to the intent that hits in reach decides the search; the outer rays count only when none of these hits
	};

	// ---- the probe the sensor casts through ------------------------------------------------------------------------
	struct ProbeRequest
	{
		Vec3  from{};
		Vec3  to{};
		float radius{ 0.1f };
	};
	struct ProbeAnswer
	{
		bool available{ false };  // the cast ran; false: the probe cannot answer (first person, off, fault)
		bool hit{ false };
		Vec3 stop{};              // where the sphere centre stopped; equals `to` when nothing was hit
	};
	using ProbeTicket = std::uint32_t;  // 0 = not accepted

	class IProbe
	{
	public:
		virtual ~IProbe() = default;
		// Queues a cast. 0 means it was not accepted (treated as "not available").
		virtual ProbeTicket Submit(const ProbeRequest& a_request) = 0;
		// nullopt: not answered yet. Otherwise the answer (it is not asked for again).
		virtual std::optional<ProbeAnswer> Poll(ProbeTicket a_ticket) = 0;
	};

	// ---- the result ------------------------------------------------------------------------------------------------
	enum class CoverKind
	{
		None,
		Low,
		High,
	};

	enum class Reject
	{
		None,
		BadInput,          // no intent direction
		ProbeUnavailable,  // a cast could not run
		NoSurface,         // nothing within the snap distance in front
		TooSmallToFit,     // fewer than three of the five fit casts hit: a pole, a corner of something, a thin object
		NotFlat,           // the hit points do not lie on one line: a curve too tight, a corner, a mess
		NoBase,            // the lowest height cast does not hit the wall: it does not reach the floor (a ledge, an overhang)
		TooLow,            // lower than fLowCoverMinHeight
		NoFloor,           // no floor under the stand point, or one more than fMaxFloorStep away
		PathBlocked,       // something stands between the player and the stand point
		TooNarrow,         // narrower than fMinCoverWidth
		TooFarToTheSide,   // the wall lies more than fSideSnapAngleDeg off the intent and farther than fSideSnapDistance
	};
	[[nodiscard]] const char* ToString(Reject a_reason) noexcept;
	[[nodiscard]] const char* ToString(CoverKind a_kind) noexcept;
	[[nodiscard]] const char* ToString(EdgeKind a_kind) noexcept;

	struct CoverCandidate
	{
		CoverKind kind{ CoverKind::None };
		Reject    why{ Reject::None };  // when kind is None: the reason. The fields below hold what was learned before the rejection

		float height{ 0.0f };           // top above the floor; a lower bound when heightCapped
		bool  heightCapped{ false };
		bool  canPeekOver{ false };    // height <= fOverPeekMaxHeight: the player can stand up and aim over the top (Phase 3)
		Vec2  normal{};                 // unit, horizontal, from the wall toward the player
		Vec2  tangent{};                // Perp(normal): the player's right-hand side when facing the wall
		Vec2  linePoint{};              // the foot of the perpendicular from the player on the line of sphere centres
		Vec3  anchor{};                 // the point of the wall's surface in front of the player, at floor height
		Vec3  standPoint{};             // where the player should stand, at floor height
		float floorZ{ 0.0f };
		float distance{ 0.0f };         // from the player to the wall surface
		float clampShift{ 0.0f };       // how far the foot was moved along the wall to stand fEdgeMargin inside an open end (+ toward the tangent)
		float wallPointDistance{ 0.0f };  // from the player to the point of the wall surface they would stand against (the foot after the clamp)
		float wallPointAngleDeg{ 0.0f };  // the angle between the intent and the direction to that point
		float fitRms{ 0.0f };
		float edgeHeight{ 0.0f };       // height above the feet at which the edge casts ran

		float    edgeLeft{ 0.0f };      // from the foot to the end of the cover facing the wall, left (-tangent); the search range when edgeLeftKind is None
		float    edgeRight{ 0.0f };     // the same to the right (+tangent)
		EdgeKind edgeLeftKind{ EdgeKind::None };
		EdgeKind edgeRightKind{ EdgeKind::None };

		// T3.0: the search takes the nearest ray ahead of the intent first. When that surface is rejected (TooFarToTheSide, TooSmallToFit, NotFlat or
		// NoBase) it goes on once with the best ray of the outer group. usedFallback: the second attempt ran; otherWhy is then the reason the FIRST
		// attempt was rejected when this candidate is from the second, or the second attempt's reason when both were rejected (this candidate and
		// `why` are the first attempt's then)
		bool   usedFallback{ false };
		Reject otherWhy{ Reject::None };
	};

	enum class TrackStatus
	{
		Ok,           // the candidate was updated
		Corner,       // the normal turned by more than fMaxNormalTurnDeg, or the points no longer form one line: stop moving
		Lost,         // the wall, its height or the floor is gone
		Unavailable,  // a cast could not run
	};
	[[nodiscard]] const char* ToString(TrackStatus a_status) noexcept;

	struct SensorResult
	{
		bool           isTrack{ false };
		CoverCandidate candidate{};  // Find: the candidate or the rejection. Track: the updated candidate (the old one unless status is Ok)
		TrackStatus    status{ TrackStatus::Ok };
		float          turnDeg{ 0.0f };  // Track: how far the normal turned
		std::string    why;              // Track, Lost: which condition failed (for the log)
		std::uint32_t  casts{ 0 };       // casts submitted for this query
		std::uint32_t  updates{ 0 };     // Update() calls while it ran
		std::vector<Vec2> fitPoints;     // Find: the hit points the wall line was fitted through (of the attempt the candidate belongs to); for the log of a rejection
		Vec2           searchHit{};      // Find: the sphere centre of the ray the search chose (that attempt)
	};

	class CoverSensor
	{
	public:
		explicit CoverSensor(IProbe& a_probe, const SensorSettings& a_settings = {});

		void                         SetSettings(const SensorSettings& a_settings) { m_cfg = a_settings; }
		[[nodiscard]] const SensorSettings& Settings() const noexcept { return m_cfg; }

		// Starts a search. a_feet is the player's position at the floor; a_intent a horizontal direction (movement input, else the
		// camera's forward), not necessarily unit. Starting a query drops an unread result. False when a query is running.
		bool Find(const Vec3& a_feet, const Vec2& a_intent);
		// Starts a re-estimate of a_current from the player's present position.
		bool Track(const CoverCandidate& a_current, const Vec3& a_feet);

		// Advances the state machine: polls the probe and starts the next phase. Call every tick.
		void Update();

		[[nodiscard]] bool Busy() const noexcept { return m_phase != Phase::Idle && m_phase != Phase::Done; }
		[[nodiscard]] bool HasResult() const noexcept { return m_phase == Phase::Done; }
		// The finished query's result; the sensor is idle again afterwards.
		[[nodiscard]] SensorResult TakeResult();
		void                       Cancel();

	private:
		enum class Phase
		{
			Idle,
			Search,
			Fit,
			Fit2,
			Ends,
			Profile,
			Edges,
			TrackFit,
			TrackEdges,
			Done,
		};

		struct Item
		{
			ProbeTicket ticket{ 0 };
			bool        answered{ false };
			ProbeAnswer answer{};
		};
		struct Batch
		{
			std::vector<Item> items;
		};

		std::size_t Add(const Vec3& a_from, const Vec3& a_to);
		bool        BatchDone();
		[[nodiscard]] bool AnyUnavailable() const;

		void Begin(bool a_track);
		void Fail(Reject a_reason);
		void FinishTrack(TrackStatus a_status);

		void StartSearch();
		void EvalSearch();
		void StartFit();
		void EvalFit();
		void StartFit2(float a_side);
		void EvalFit2();
		void FinishFit();
		void StartEnds();
		void EvalEnds();
		void StartProfile();
		void EvalProfile();
		void StartEdges(float a_range, bool a_track);
		void EvalEdges(bool a_track);
		void StartTrackFit();
		void EvalTrackFit();

		[[nodiscard]] Vec3 AtHeight(const Vec2& a_xy, float a_heightAboveFeet) const { return { a_xy.x, a_xy.y, m_feet.z + a_heightAboveFeet }; }
		[[nodiscard]] float HorizontalDistance(const Vec3& a, const Vec3& b) const;

		IProbe&        m_probe;
		SensorSettings m_cfg;
		Phase          m_phase{ Phase::Idle };
		Batch          m_batch;
		SensorResult   m_result{};

		Vec3 m_feet{};
		Vec2 m_intent{};
		CoverCandidate m_c{};  // what is known so far

		// Search
		std::vector<Vec2> m_fanDirs;
		std::vector<float> m_fanOffsets;  // degrees from the intent
		Vec2 m_hit0{};       // the nearest hit (sphere centre)
		Vec2 m_dir0{};       // the cast direction that found it
		bool m_haveAlt{ false };   // T3.0: the best outer ray, kept for one fallback when the ahead surface is rejected
		Vec2 m_altHit{};
		Vec2 m_altDir{};
		bool m_inFallback{ false };
		CoverCandidate m_primary{};  // the rejected first attempt, kept while the fallback runs
		std::vector<Vec2> m_primaryPts;
		Vec2 m_primaryHit{};
		// Fit and Ends
		std::vector<Vec2> m_fitPts;  // the hit points collected for the line
		float             m_tHit{ 0.0f };  // the first hit along the tangent, from the raw foot
		std::size_t       m_nEnd{ 0 };
		// Profile
		Vec2 m_foot{};       // the foot on the centre line (moved fEdgeMargin inside an open end by Ends)
		Vec2 m_stand{};      // the stand point (horizontal)
		std::size_t m_nHeights{ 0 };
		std::size_t m_iFloor{ 0 };
		std::size_t m_iPath{ 0 };
		std::size_t m_nPath{ 0 };
		std::size_t m_iBelow{ 0 };  // Track: the height check
		// Edges
		std::size_t m_nEdge{ 0 };  // casts per side
		float       m_edgeStep{ 0.15f };
		// Track
		CoverCandidate m_old{};
		Vec2           m_trackN{};
		float          m_turn{ 0.0f };
		TrackStatus    m_trackStatus{ TrackStatus::Ok };
	};
}
