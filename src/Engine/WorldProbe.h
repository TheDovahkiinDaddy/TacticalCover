#pragma once

// S3 / T2.0: the world probe. Wraps the game's own sphere cast (borrowed from the dialogue camera, see
// Engine/Offsets.h) behind a guard and an INI switch. It returns a hit flag and a fraction along the
// line, no normal and no collision layer: enough for CoverSensor, which derives wall direction, height
// and edges from several casts (PLAN.md section 3.3).
//
// T2.0 (2026-11-17): the cast's query object is the PlayerCamera singleton and the cast leaves its hit fraction in
// PlayerCamera + 0x2B4, which the engine's own camera collision reads on the camera's thread right after its own cast
// (docs/REFERENCE.md 14.8). So no cast runs from an SFSE task any more. Requests are queued from any thread with
// Submit(), executed inside the ThirdPersonState::Update hook before the original Update runs (RunQueued, called by
// Engine/Camera), and answered a frame later; Poll() reads the answer. The hook runs in third person only: outside it
// every request is answered "not available" after a short wait.

#include <optional>
#include <vector>

namespace Engine::Frame
{
	struct Snapshot;
}

namespace Engine::WorldProbe
{
	struct CastResult
	{
		bool         ok{ false };       // the cast ran; false when unavailable (kill switch, fault, not third person)
		bool         hit{ false };      // the game's return value
		float        fraction{ 1.0f };  // along from->to where the first hit is, 1 when clear. Derived from the end point
		                                // the cast moved back (the raw fraction field reads 0 for a vertical cast)
		float        rawFraction{ 1.0f };  // the float at query + 0x2B4 as the game left it; logged next to the derived one
		RE::NiPoint3 point{};           // the end point the cast moved back to when it hit, else a_to
		RE::NiPoint3 safePoint{};       // where the cast moved its end point to (the last clear spot); equals the end point when clear
	};

	using Ticket = std::uint32_t;  // 0 = no ticket (the request could not be queued)

	// Queues one sphere cast from a_from towards a_to. Any thread. Returns 0 when the probe is off, has faulted or the
	// queue is full; treat that like an answer with ok = false.
	[[nodiscard]] Ticket Submit(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, float a_radius);

	// nullopt: not answered yet. Otherwise the answer, which is removed from the store (a second Poll of the same ticket
	// returns nullopt too). ok = false in the answer: the cast could not run.
	[[nodiscard]] std::optional<CastResult> Poll(Ticket a_ticket);

	// True when the Update hook has run lately (third person, probe on, no fault): requests will be answered.
	[[nodiscard]] bool Available();

	// A group of casts submitted together and polled together.
	class Batch
	{
	public:
		// Submits now. Returns the index of the cast in this batch.
		std::size_t Add(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, float a_radius);
		// Polls what is still open; true when every cast has an answer (also for an empty batch).
		[[nodiscard]] bool Done();
		[[nodiscard]] const CastResult& operator[](const std::size_t a_index) const { return m_items[a_index].result; }
		[[nodiscard]] std::size_t       Size() const noexcept { return m_items.size(); }
		void                            Clear() { m_items.clear(); }

	private:
		struct Item
		{
			Ticket     ticket{ 0 };
			bool       answered{ false };
			CastResult result{};
		};
		std::vector<Item> m_items;
	};

	// Called by Engine/Camera from the ThirdPersonState::Update hook, on the engine's own thread, before the original
	// Update runs. Executes up to [WorldProbe] iMaxCastsPerFrame queued casts. Guarded; never throws.
	void RunQueued();

	struct Stats
	{
		std::uint64_t submitted{ 0 };
		std::uint64_t executed{ 0 };      // casts run inside RunQueued
		std::uint64_t unavailable{ 0 };   // requests answered ok = false without running
		std::uint64_t frames{ 0 };        // RunQueued calls
		std::uint64_t framesWithCasts{ 0 };
		std::uint32_t maxPerFrame{ 0 };
		std::uint32_t queued{ 0 };        // waiting now
		bool          broken{ false };    // a cast faulted: all casts are off for this session
	};
	[[nodiscard]] Stats GetStats();

	// Forward-clearance watch for the S2b movement test. After BeginForwardWatch, ForwardWatchMin() is the smallest
	// straight-ahead distance the automatic probe reported since then (the last reading before the call counts as
	// the first one). A clear ray counts as the probe range. Returns a negative number when no reading exists.
	// The same for the ray straight behind the player (a_backward = true), for movement test attempts that walk backward.
	void BeginForwardWatch(bool a_backward = false);
	[[nodiscard]] float ForwardWatchMin(bool a_backward = false);

	// Called every tick after S1 (SFSE task thread): logs changes in what the query object looks like, finishes the
	// pending debug probe, starts one when the probe key was pressed, expires requests nobody answered.
	void OnTick(const Frame::Snapshot& a_snap);
}
