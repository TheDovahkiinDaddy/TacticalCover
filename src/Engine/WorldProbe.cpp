#include "Engine/WorldProbe.h"

#include "Config.h"
#include "Engine/Frame.h"
#include "Engine/Guard.h"
#include "Engine/Input.h"
#include "Engine/Offsets.h"

#include <functional>
#include <mutex>
#include <unordered_map>

namespace Engine::WorldProbe
{
	namespace
	{
		constexpr float kPi = 3.14159265358979f;

		// T2.0 queue. Submit() may come from any thread; RunQueued() runs on the camera's thread inside the Update hook.
		struct Pending
		{
			Ticket       id{ 0 };
			RE::NiPoint3 from{};
			RE::NiPoint3 to{};
			float        radius{ 0.1f };
			double       at{ 0.0 };
		};
		struct Answer
		{
			CastResult result{};
			double     at{ 0.0 };
		};
		constexpr std::size_t kMaxQueued = 4096;
		constexpr double      kStaleRequestSeconds = 0.5;  // not run within this long: answered "not available"
		constexpr double      kStaleAnswerSeconds = 5.0;   // nobody collected the answer: dropped
		constexpr double      kAvailableWithin = 0.25;     // the hook counts as running when it ran this recently

		std::mutex                         s_mutex;
		std::vector<Pending>               s_queue;
		std::unordered_map<Ticket, Answer> s_answers;
		std::atomic<Ticket>                s_nextTicket{ 1 };
		std::atomic<double>                s_lastPump{ -100.0 };
		std::atomic<std::uint64_t>         s_submitted{ 0 };
		std::atomic<std::uint64_t>         s_executed{ 0 };
		std::atomic<std::uint64_t>         s_unavailable{ 0 };
		std::atomic<std::uint64_t>         s_frames{ 0 };
		std::atomic<std::uint64_t>         s_framesWithCasts{ 0 };
		std::atomic<std::uint32_t>         s_maxPerFrame{ 0 };

		std::atomic<bool> s_broken{ false };  // a cast faulted: all casts off for this session
		bool         s_idChecked{ false };
		bool         s_idOk{ false };
		std::uint32_t s_probeCount{ 0 };

		// What the query looked like last time we looked, so a change is logged once.
		struct QueryView
		{
			const void*    camera{ nullptr };
			int            cameraIndex{ -2 };
			const void*    dialogueState{ nullptr };
			std::uintptr_t query{ 0 };            // what Cast() hands to the game: the PlayerCamera itself (see ReadView)
			std::uintptr_t dialogueQuery{ 0 };    // the pointer found at dialogue state + 0x48 (the reference plugin's "query")
			std::uintptr_t queryVtableRva{ 0 };
			bool operator==(const QueryView&) const = default;
		};
		QueryView      s_lastView{};
		bool           s_viewLogged{ false };
		std::atomic<std::uintptr_t> s_knownQueryVtable{ 0 };  // vtable RVA of the query object, remembered once seen

		[[nodiscard]] std::uintptr_t ModuleBase()
		{
			return REX::FModule::GetExecutingModule().GetBaseAddress();
		}

		[[nodiscard]] bool LooksLikePointer(const std::uintptr_t a_value) noexcept
		{
			return a_value > 0x10000 && a_value < 0x7FFFFFFFFFFF && (a_value & 7) == 0;
		}

		// The sphere cast, called through a function pointer inside a guard.
		struct CastCall
		{
			void*              query;
			const RE::NiPoint3* from;
			const RE::NiPoint3* to;
			float              radius;
			bool               hit;
			float              fraction;
		};

		void CastRaw(CastCall& a_call)
		{
			using func_t = bool (*)(void*, const RE::NiPoint3*, const RE::NiPoint3*, float, void**, void*, float);
			const auto fn = reinterpret_cast<func_t>(kSphereCast.address());
			void*      out{ nullptr };
			a_call.hit = fn(a_call.query, a_call.from, a_call.to, a_call.radius, &out, nullptr, 0.0f);
			a_call.fraction = *reinterpret_cast<const float*>(reinterpret_cast<std::uintptr_t>(a_call.query) + kCollisionQueryHitFraction);
		}

		[[nodiscard]] bool CheckId()
		{
			if (!s_idChecked) {
				s_idChecked = true;
				const auto rva = kSphereCast.address() - ModuleBase();
				s_idOk = rva == kSphereCastRva;
				COVER_DETAIL("S3 sphere cast: Address Library ID {} resolves to RVA 0x{:X} (expected 0x{:X}) {}", kSphereCast.id(), rva, kSphereCastRva,
					s_idOk ? "- OK" : "- MISMATCH, casts disabled");
			}
			return s_idOk;
		}

		// Reads the query object the game's dialogue camera would use. Step 1 of the S3 plan: is it there in
		// ordinary play?
		[[nodiscard]] QueryView ReadView()
		{
			QueryView view{};
			const auto camera = RE::PlayerCamera::GetSingleton();
			view.camera = camera;
			if (!camera) {
				return view;
			}
			view.cameraIndex = -1;
			for (std::uint32_t i = 0; i < RE::CameraStates::kTotal; ++i) {
				if (camera->currentState && camera->cameraStates[i] == camera->currentState) {
					view.cameraIndex = static_cast<int>(i);
				}
			}
			view.dialogueState = camera->cameraStates[RE::CameraStates::kDialogue];
			if (view.dialogueState) {
				std::uintptr_t query{ 0 };
				if (SafeRead(reinterpret_cast<const std::byte*>(view.dialogueState) + kCameraStateCollisionQuery, query) && LooksLikePointer(query)) {
					view.dialogueQuery = query;
				}
			}

			// Test run 1 (2026-11-16): the pointer stored at dialogue state + 0x48 equals the PlayerCamera singleton
			// (vtable RVA 0x4D04F88, the camera's primary vtable), also in ordinary play. So the "collision query" of
			// the borrowed cast is the PlayerCamera object, which always exists, and Cast() passes it directly.
			// The dialogue pointer is still read and logged so a run can confirm they stay equal.
			view.query = reinterpret_cast<std::uintptr_t>(camera);
			std::uintptr_t vptr{ 0 };
			if (SafeRead(camera, vptr) && vptr > ModuleBase()) {
				view.queryVtableRva = vptr - ModuleBase();
				s_knownQueryVtable = view.queryVtableRva;
			}
			return view;
		}

		void LogView(const QueryView& a_view, const char* a_why)
		{
			COVER_DETAIL("S3 query object ({}): PlayerCamera={} currentState index={} dialogueState={} dialogueState+0x48={} query used={} ({}) queryVtableRVA=0x{:X}", a_why,
				a_view.camera, a_view.cameraIndex, a_view.dialogueState, reinterpret_cast<const void*>(a_view.dialogueQuery),
				reinterpret_cast<const void*>(a_view.query), a_view.query == a_view.dialogueQuery ? "same as the dialogue pointer" : "DIFFERS from the dialogue pointer",
				a_view.queryVtableRva);
		}

		// Step 3 of the S3 plan, as a diagnostic: look through the first 0x400 bytes of the third- and
		// first-person camera states for a pointer to an object with the same vtable as the query object.
		void ScanForQuery()
		{
			if (!s_knownQueryVtable) {
				COVER_DETAIL("S3 scan: the query object's vtable is not known yet (it has never been readable), so there is nothing to look for");
				return;
			}
			const auto camera = RE::PlayerCamera::GetSingleton();
			if (!camera) {
				return;
			}
			const std::pair<int, const char*> states[] = { { RE::CameraStates::kThirdPerson, "third person" }, { RE::CameraStates::kFirstPerson, "first person" } };
			for (const auto& [index, label] : states) {
				const auto* state = reinterpret_cast<const std::byte*>(camera->cameraStates[index]);
				if (!state) {
					COVER_DETAIL("S3 scan: no {} camera state object", label);
					continue;
				}
				int found{ 0 };
				for (std::ptrdiff_t off = 0; off < 0x400; off += 8) {
					std::uintptr_t word{ 0 };
					if (!SafeRead(state + off, word) || !LooksLikePointer(word)) {
						continue;
					}
					std::uintptr_t vptr{ 0 };
					if (SafeRead(reinterpret_cast<const void*>(word), vptr) && vptr > ModuleBase() && vptr - ModuleBase() == s_knownQueryVtable) {
						++found;
						COVER_DETAIL("S3 scan: {} camera state {} + 0x{:X} holds {} with the query vtable (RVA 0x{:X})", label, static_cast<const void*>(state), static_cast<std::uint32_t>(off),
							reinterpret_cast<const void*>(word), s_knownQueryVtable.load());
					}
				}
				if (!found) {
					COVER_DETAIL("S3 scan: {} camera state {}: no pointer to an object with the query vtable in its first 0x400 bytes", label, static_cast<const void*>(state));
				}
			}
		}


		// ---- the cast itself: camera thread only, inside RunQueued -------------------------------------------------
		[[nodiscard]] CastResult ExecuteCast(void* a_query, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, const float a_radius)
		{
			CastResult result{};
			if (s_broken.load() || !a_query) {
				return result;
			}
			RE::NiPoint3 from = a_from;  // the cast writes into its arguments, so it gets copies
			RE::NiPoint3 to = a_to;
			CastCall     call{ a_query, &from, &to, a_radius, false, 1.0f };

			unsigned long code{ 0 };
			if (!Guarded([&] { CastRaw(call); }, code)) {
				s_broken = true;
				REX::ERROR("S3: the sphere cast faulted (exception 0x{:08X}); world probe disabled for this session", code);
				return result;
			}

			result.ok = true;
			result.hit = call.hit;
			result.rawFraction = (call.hit && std::isfinite(call.fraction)) ? std::clamp(call.fraction, 0.0f, 1.0f) : 1.0f;
			result.safePoint = to;  // the cast moved its end point back to the last clear spot

			// Runs 4 to 8 (2026-11-16): the raw fraction is right for horizontal casts but reads 0.000 for an exactly
			// vertical one, while the moved-back end point is right for both (floor + the sphere radius). So the distance
			// and the hit point come from the end point; the raw fraction is kept for comparison.
			const float full = std::sqrt((a_to.x - a_from.x) * (a_to.x - a_from.x) + (a_to.y - a_from.y) * (a_to.y - a_from.y) + (a_to.z - a_from.z) * (a_to.z - a_from.z));
			const float moved = std::sqrt((to.x - a_from.x) * (to.x - a_from.x) + (to.y - a_from.y) * (to.y - a_from.y) + (to.z - a_from.z) * (to.z - a_from.z));
			if (call.hit && full > 1e-4f && std::isfinite(moved)) {
				result.fraction = std::clamp(moved / full, 0.0f, 1.0f);
				result.point = to;
			} else {
				result.fraction = 1.0f;
				result.point = a_to;
			}
			return result;
		}

		// ---- reports: a debug probe is a batch plus the function that logs it once every answer is in ---------------
		struct Report
		{
			Batch                       batch;
			std::function<void(Batch&)> finish;
			double                      started{ 0.0 };
		};
		std::optional<Report> s_report;  // tick thread only

		[[nodiscard]] float Dist3(const RE::NiPoint3& a, const RE::NiPoint3& b)
		{
			return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
		}

		void RunProbe(const Frame::Snapshot& a_snap)
		{
			const auto& cfg = Config::Get();
			++s_probeCount;

			const auto view = ReadView();
			LogView(view, "at probe");

			const float h = a_snap.angle.z;
			const RE::NiPoint3 forward{ std::sin(h), std::cos(h), 0.0f };  // heading convention assumption, checked against S1's moveDir
			const RE::NiPoint3 from{ a_snap.position.x, a_snap.position.y, a_snap.position.z + cfg.chestHeight };

			COVER_DETAIL("S3 probe #{}: player pos=({:.2f},{:.2f},{:.2f}) heading={:.1f}deg forward=({:.3f},{:.3f}) from=({:.2f},{:.2f},{:.2f}) range={:.1f} radius={:.2f} camera={}",
				s_probeCount, a_snap.position.x, a_snap.position.y, a_snap.position.z, h * 180.0f / kPi, forward.x, forward.y,
				from.x, from.y, from.z, cfg.probeRange, cfg.probeRadius, a_snap.cameraIndex);

			Report rep;
			rep.started = a_snap.time;
			constexpr int kRays = 9;
			for (int i = 0; i < kRays; ++i) {
				const float offsetDeg = cfg.fanHalfAngleDeg * (static_cast<float>(i) - 4.0f) / 4.0f;  // i = 4 is straight ahead
				const float a = h + offsetDeg * kPi / 180.0f;
				const RE::NiPoint3 to{ from.x + std::sin(a) * cfg.probeRange, from.y + std::cos(a) * cfg.probeRange, from.z };
				rep.batch.Add(from, to, cfg.probeRadius);
			}
			// Calibration that needs no tape measure: straight down from the chest hits the floor, which is
			// chestHeight below (minus about the sphere radius, depending on how the game reports the hit).
			rep.batch.Add(from, { from.x, from.y, from.z - 3.0f }, cfg.probeRadius);

			const auto  count = s_probeCount;
			const float fan = cfg.fanHalfAngleDeg, range = cfg.probeRange, radius = cfg.probeRadius, chest = cfg.chestHeight;
			rep.finish = [=](Batch& b) {
				int hits{ 0 };
				for (int i = 0; i < kRays; ++i) {
					const float offsetDeg = fan * (static_cast<float>(i) - 4.0f) / 4.0f;
					const auto& r = b[static_cast<std::size_t>(i)];
					if (!r.ok) {
						COVER_DETAIL("S3   ray {} ({:+.0f}deg): cast unavailable", i, offsetDeg);
						continue;
					}
					hits += r.hit ? 1 : 0;
					COVER_DETAIL("S3   ray {} ({:+.0f}deg){}: hit={} fraction={:.4f} distance={:.3f} point=({:.2f},{:.2f},{:.2f}) | end point moved to distance {:.3f}", i, offsetDeg,
						i == 4 ? " [straight ahead]" : "", r.hit, r.fraction, r.fraction * range, r.point.x, r.point.y, r.point.z, Dist3(r.safePoint, from));
				}
				COVER_DETAIL("S3 probe #{} done: {} of {} rays reported a hit", count, hits, kRays);
				const auto& d = b[static_cast<std::size_t>(kRays)];
				COVER_DETAIL("S3 calibration: straight down from the chest: ok={} hit={} fraction={:.4f} distance={:.3f} (the floor is {:.2f} below the chest, sphere radius {:.2f})",
					d.ok, d.hit, d.fraction, d.fraction * 3.0f, chest, radius);
			};
			s_report = std::move(rep);

			// Step 3 of the plan runs whenever the straight cast could not run, and the first time regardless.
			if (!view.query || s_broken.load() || s_probeCount == 1) {
				ScanForQuery();
			}
		}

		// Compact probe logged on its own while the game runs (no key needed): straight ahead, down, left and right
		// from chest height, one line. Walking toward a wall shows the forward distance shrinking at walking speed
		// and the hit point staying put; that is the accuracy check that needs no tape measure.
		double s_nextAuto{ 0.0 };
		float  s_lastFwd{ -1.0f };       // last straight-ahead reading of the automatic probe (probe range when clear), -1 none yet
		float  s_fwdWatchMin{ -1.0f };   // smallest reading since BeginForwardWatch, -1 none
		float  s_lastBack{ -1.0f };      // the same two for the ray straight behind the player
		float  s_backWatchMin{ -1.0f };

		void AutoProbe(const Frame::Snapshot& a_snap)
		{
			const auto& cfg = Config::Get();
			const float h = a_snap.angle.z;
			const RE::NiPoint3 from{ a_snap.position.x, a_snap.position.y, a_snap.position.z + cfg.chestHeight };
			const float  range = cfg.probeRange, radius = cfg.probeRadius;
			const auto   pos = a_snap.position;
			const double t = a_snap.time;
			const int    cam = a_snap.cameraIndex;

			Report rep;
			rep.started = a_snap.time;
			const auto ray = [&](const float a_headingOffsetDeg, const float a_range) {
				const float a = h + a_headingOffsetDeg * kPi / 180.0f;
				const RE::NiPoint3 to{ from.x + std::sin(a) * a_range, from.y + std::cos(a) * a_range, from.z };
				return rep.batch.Add(from, to, radius);
			};
			const auto iFwd = ray(0.0f, range);
			const auto iBack = ray(180.0f, range);
			const auto iLeft = ray(-90.0f, 3.0f);
			const auto iRight = ray(90.0f, 3.0f);
			const std::size_t iDown = rep.batch.Size();
			for (const float h2 : { 0.4f, 1.2f, 2.0f }) {  // straight down from three heights above the player's feet
				const RE::NiPoint3 start{ pos.x, pos.y, pos.z + h2 };
				rep.batch.Add(start, { start.x, start.y, start.z - 3.0f }, radius);
			}
			// A steep diagonal, 60 degrees below horizontal and forward, 3 m long, from the chest. On flat ground
			// the floor is chestHeight / sin(60 deg) = 1.39 m away along it.
			const float pitch = 60.0f * kPi / 180.0f;
			const auto  iDiag = rep.batch.Add(from, { from.x + std::sin(h) * std::cos(pitch) * 3.0f, from.y + std::cos(h) * std::cos(pitch) * 3.0f, from.z - std::sin(pitch) * 3.0f }, radius);
			// A low forward ray (knee height), to compare with the chest-height one.
			const RE::NiPoint3 low{ pos.x, pos.y, pos.z + 0.4f };
			const auto         iLow = rep.batch.Add(low, { low.x + std::sin(h) * range, low.y + std::cos(h) * range, low.z }, radius);

			rep.finish = [=](Batch& b) {
				const auto fmt = [&](const std::size_t i, const float a_range) -> std::string {
					const auto& r = b[i];
					if (!r.ok) {
						return "n/a";
					}
					if (!r.hit) {
						return std::format("clear(>{:.1f})", a_range);
					}
					return std::format("{:.2f}@({:.2f},{:.2f})raw{:.2f}", r.fraction * a_range, r.point.x, r.point.y, r.rawFraction * a_range);
				};
				const auto reading = [&](const std::size_t i) {  // the distance when it hit, the range when clear, -1 unavailable
					const auto& r = b[i];
					return !r.ok ? -1.0f : (r.hit ? r.fraction * range : range);
				};
				s_lastFwd = reading(iFwd);
				if (s_lastFwd >= 0.0f && s_fwdWatchMin >= 0.0f) {
					s_fwdWatchMin = (std::min)(s_fwdWatchMin, s_lastFwd);
				}
				s_lastBack = reading(iBack);
				if (s_lastBack >= 0.0f && s_backWatchMin >= 0.0f) {
					s_backWatchMin = (std::min)(s_backWatchMin, s_lastBack);
				}
				// h:hit/fraction*3/distance the end point was moved back to
				std::string down;
				std::size_t k = iDown;
				for (const float h2 : { 0.4f, 1.2f, 2.0f }) {
					const auto& r = b[k++];
					const RE::NiPoint3 start{ pos.x, pos.y, pos.z + h2 };
					down += std::format(" h{:.1f}:{}/raw{:.3f}/{:.2f}", h2, !r.ok ? "n/a" : (r.hit ? "hit" : "clear"), r.rawFraction * 3.0f, start.z - r.safePoint.z);
				}
				const auto&       dg = b[iDiag];
				const std::string diag = std::format("{}/{:.3f}/{:.2f}@z{:.2f}", !dg.ok ? "n/a" : (dg.hit ? "hit" : "clear"), dg.fraction * 3.0f, Dist3(dg.safePoint, from), dg.point.z - pos.z);
				const auto&       lw = b[iLow];
				const std::string fwdLow = !lw.ok ? "n/a" : (lw.hit ? std::format("{:.2f}", lw.fraction * range) : "clear");
				COVER_DETAIL("S3 auto t={:.1f}s pos=({:.2f},{:.2f},{:.2f}) heading={:.1f}deg cam={} | fwd={} fwdLow={} left={} right={} down[{} ] diag60={} (hit/distance/end-point-moved-to/height-above-feet)", t, pos.x,
					pos.y, pos.z, h * 180.0f / kPi, cam, fmt(iFwd, range), fwdLow, fmt(iLeft, 3.0f), fmt(iRight, 3.0f), down, diag);
			};
			s_report = std::move(rep);
		}

		[[nodiscard]] bool cfg_AutoProbeOn(const Frame::Snapshot& a_snap)
		{
			const auto& cfg = Config::Get();
			if (!cfg.autoProbe || !a_snap.valid || a_snap.paused || a_snap.time < s_nextAuto || !Available()) {
				return false;
			}
			s_nextAuto = a_snap.time + static_cast<double>(cfg.autoProbeMs) / 1000.0;
			return true;
		}

		// Requests nobody ran (the hook does not run outside third person) are answered "not available"; answers nobody
		// collected are dropped. A fault empties the queue the same way.
		void ExpireStale(const double a_now)
		{
			std::scoped_lock lk(s_mutex);
			const bool       all = s_broken.load();
			std::size_t      keep = 0;
			for (std::size_t i = 0; i < s_queue.size(); ++i) {
				if (all || a_now - s_queue[i].at > kStaleRequestSeconds) {
					s_answers[s_queue[i].id] = Answer{ CastResult{}, a_now };
					s_unavailable.fetch_add(1);
				} else {
					s_queue[keep++] = s_queue[i];
				}
			}
			s_queue.resize(keep);
			for (auto it = s_answers.begin(); it != s_answers.end();) {
				it = (a_now - it->second.at > kStaleAnswerSeconds) ? s_answers.erase(it) : std::next(it);
			}
		}

		void TickImpl(const Frame::Snapshot& a_snap)
		{
			const auto view = ReadView();
			if (!s_viewLogged || view != s_lastView) {
				LogView(view, s_viewLogged ? "changed" : "first look");
				s_viewLogged = true;
				s_lastView = view;
			}

			ExpireStale(Frame::Now());

			if (s_report) {
				if (s_report->batch.Done()) {
					auto rep = std::move(*s_report);
					s_report.reset();
					rep.finish(rep.batch);
				} else if (a_snap.time - s_report->started > 3.0) {
					COVER_DETAIL("S3 probe: no answer within 3 s (the casts run inside the third-person camera update; first person or a fault stops them)");
					s_report.reset();
				}
			}

			if (!s_report && cfg_AutoProbeOn(a_snap)) {
				AutoProbe(a_snap);
			}

			if (!s_report && Input::ConsumeProbeRequest()) {
				if (!a_snap.valid) {
					COVER_DETAIL("S3 probe key pressed, but there is no player in a loaded cell");
				} else {
					// The paused flag comes from a CommonLibSF offset that the S1 log still has to confirm, so it
					// is reported here but does not block the probe.
					if (a_snap.paused) {
						COVER_DETAIL("S3 probe key pressed while Main::isGameMenuPaused reads true (running the probe anyway)");
					}
					RunProbe(a_snap);
				}
			}
		}
	}

	Ticket Submit(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, const float a_radius)
	{
		if (!Config::Get().worldProbe || s_broken.load()) {
			s_unavailable.fetch_add(1);
			return 0;
		}
		const auto       now = Frame::Now();
		std::scoped_lock lk(s_mutex);
		if (s_queue.size() >= kMaxQueued) {
			s_unavailable.fetch_add(1);
			return 0;
		}
		Ticket id = s_nextTicket.fetch_add(1);
		if (id == 0) {
			id = s_nextTicket.fetch_add(1);
		}
		s_queue.push_back({ id, a_from, a_to, a_radius, now });
		s_submitted.fetch_add(1);
		return id;
	}

	std::optional<CastResult> Poll(const Ticket a_ticket)
	{
		std::scoped_lock lk(s_mutex);
		const auto       it = s_answers.find(a_ticket);
		if (it == s_answers.end()) {
			return std::nullopt;
		}
		const auto result = it->second.result;
		s_answers.erase(it);
		return result;
	}

	bool Available()
	{
		return Config::Get().worldProbe && !s_broken.load() && Frame::Now() - s_lastPump.load() < kAvailableWithin;
	}

	std::size_t Batch::Add(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, const float a_radius)
	{
		Item item;
		item.ticket = Submit(a_from, a_to, a_radius);
		item.answered = item.ticket == 0;  // not queued: answered "not available" (the default CastResult)
		m_items.push_back(item);
		return m_items.size() - 1;
	}

	bool Batch::Done()
	{
		bool all = true;
		for (auto& item : m_items) {
			if (item.answered) {
				continue;
			}
			if (const auto r = Poll(item.ticket)) {
				item.result = *r;
				item.answered = true;
			} else {
				all = false;
			}
		}
		return all;
	}

	void RunQueued()
	{
		const auto& cfg = Config::Get();
		if (!cfg.worldProbe) {
			return;
		}
		const double now = Frame::Now();
		s_lastPump = now;
		s_frames.fetch_add(1);
		if (s_broken.load()) {
			return;  // ExpireStale on the tick answers whatever is queued
		}

		std::vector<Pending> work;
		{
			std::scoped_lock lk(s_mutex);
			const auto       n = (std::min)(s_queue.size(), static_cast<std::size_t>((std::max)(cfg.maxCastsPerFrame, 1)));
			work.assign(s_queue.begin(), s_queue.begin() + static_cast<std::ptrdiff_t>(n));
			s_queue.erase(s_queue.begin(), s_queue.begin() + static_cast<std::ptrdiff_t>(n));
		}
		if (work.empty()) {
			return;
		}

		void* query = RE::PlayerCamera::GetSingleton();
		bool  idOk = false;
		{
			unsigned long code{ 0 };
			if (!Guarded([&] { idOk = CheckId(); }, code)) {
				s_broken = true;
				REX::ERROR("S3: exception 0x{:08X} while resolving the sphere cast; world probe disabled for this session", code);
			}
		}
		std::vector<std::pair<Ticket, Answer>> out;
		out.reserve(work.size());
		std::uint32_t ran{ 0 };
		for (const auto& w : work) {
			CastResult r{};
			if (idOk && !s_broken.load()) {
				r = ExecuteCast(query, w.from, w.to, w.radius);
				ran += r.ok ? 1u : 0u;
			}
			out.push_back({ w.id, Answer{ r, now } });
		}
		{
			std::scoped_lock lk(s_mutex);
			for (const auto& [id, a] : out) {
				s_answers[id] = a;
			}
		}
		s_executed.fetch_add(ran);
		s_unavailable.fetch_add(static_cast<std::uint64_t>(work.size()) - ran);
		if (ran > 0) {
			s_framesWithCasts.fetch_add(1);
			auto prev = s_maxPerFrame.load();
			while (ran > prev && !s_maxPerFrame.compare_exchange_weak(prev, ran)) {}
		}
	}

	Stats GetStats()
	{
		Stats s;
		s.submitted = s_submitted.load();
		s.executed = s_executed.load();
		s.unavailable = s_unavailable.load();
		s.frames = s_frames.load();
		s.framesWithCasts = s_framesWithCasts.load();
		s.maxPerFrame = s_maxPerFrame.load();
		s.broken = s_broken.load();
		std::scoped_lock lk(s_mutex);
		s.queued = static_cast<std::uint32_t>(s_queue.size());
		return s;
	}

	void BeginForwardWatch(const bool a_backward)
	{
		(a_backward ? s_backWatchMin : s_fwdWatchMin) = a_backward ? s_lastBack : s_lastFwd;
	}

	float ForwardWatchMin(const bool a_backward)
	{
		return a_backward ? s_backWatchMin : s_fwdWatchMin;
	}

	void OnTick(const Frame::Snapshot& a_snap)
	{
		const auto& cfg = Config::Get();
		if (!cfg.enabled || !cfg.worldProbe) {
			return;
		}
		unsigned long code{ 0 };
		if (!Guarded([&] { TickImpl(a_snap); }, code)) {
			s_broken = true;
			REX::ERROR("S3: exception 0x{:08X} in the world probe tick; world probe disabled for this session", code);
		}
	}
}
