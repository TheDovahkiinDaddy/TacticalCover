#include "Engine/T2Script.h"

#include <deque>
#include <functional>
#include <optional>

#include "Config.h"
#include "Engine/Camera.h"
#include "Engine/CoverSense.h"
#include "Engine/Frame.h"
#include "Engine/Guard.h"
#include "Engine/PlayerControl.h"
#include "Engine/Rig.h"
#include "Engine/WorldProbe.h"

namespace Engine::T2
{
	namespace
	{
		struct Ctx
		{
			const Frame::Snapshot& snap;
			double                 now;
			double                 t;      // seconds since the stage started
			bool                   first;  // first call of the stage
		};
		using StageFn = bool (*)(const Ctx&);  // returns true when the stage is finished
		struct Stage
		{
			const char* name;
			StageFn     fn;
			bool        usesKeys;  // the safety check also wants the game window in front
		};

		constexpr float kPi = 3.14159265358979f;

		bool        s_done{ false };
		bool        s_started{ false };
		std::size_t s_stage{ 0 };
		double      s_stageStart{ 0.0 };
		bool        s_stageFirst{ true };

		void Result(const std::string& a_text)
		{
			REX::INFO("RESULT {}", a_text);
		}

		[[nodiscard]] float Dist3(const RE::NiPoint3& a, const RE::NiPoint3& b)
		{
			return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
		}

		// ---- stage: wait until the game is ready --------------------------------------------------------------------
		bool         s_haveAnchor{ false };
		RE::NiPoint3 s_anchor{};
		double       s_stillSince{ 0.0 };
		RE::NiPoint3 s_startPos{};

		bool StageReady(const Ctx& c)
		{
			const auto& snap = c.snap;
			if (!snap.valid || snap.paused || !Camera::GetStats().installed) {
				s_haveAnchor = false;
				return false;
			}
			if (!s_haveAnchor || std::fabs(snap.position.x - s_anchor.x) > 0.02f || std::fabs(snap.position.y - s_anchor.y) > 0.02f || std::fabs(snap.position.z - s_anchor.z) > 0.02f) {
				s_anchor = snap.position;
				s_haveAnchor = true;
				s_stillSince = c.now;
				return false;
			}
			if (c.now - s_stillSince < 3.0 || !Rig::InForeground()) {
				return false;
			}
			if (snap.cameraIndex != RE::CameraStates::kThirdPerson) {
				static double s_lastForce = -10.0;
				if (c.now - s_lastForce > 2.5) {
					s_lastForce = c.now;
					REX::INFO("S0 rig: t2: the camera is not third person (index {}); asking for third person", snap.cameraIndex);
					(void)Camera::ForceThirdPerson();
				}
				return false;
			}
			if (!WorldProbe::Available()) {
				return false;  // the cast queue is served by the third-person Update hook
			}
			s_startPos = snap.position;
			REX::INFO("S0 rig: script 't2' starting at ({:.2f},{:.2f},{:.2f}), third person, game window in the foreground, cast queue available", snap.position.x, snap.position.y, snap.position.z);
			return true;
		}

		// ---- T2.0: the engine's collision factor while we cast --------------------------------------------------------
		struct Range
		{
			float        lo{ 1e30f };
			float        hi{ -1e30f };
			std::uint64_t n{ 0 };
			void         Add(const float v)
			{
				lo = (std::min)(lo, v);
				hi = (std::max)(hi, v);
				++n;
			}
			[[nodiscard]] float Spread() const { return n ? hi - lo : 0.0f; }
		};
		Range s_factor, s_timer, s_w2f;       // current stage
		Range s_baseFactor, s_baseTimer, s_baseW2f;  // kept from the baseline stage
		double s_lastLog{ 0.0 };

		void SampleCollision()
		{
			const auto cs = Camera::ReadCollision();
			if (cs.ok) {
				s_factor.Add(cs.factor);
				s_timer.Add(cs.timer);
				s_w2f.Add(cs.wantedToFinal);
			}
		}

		void LogCollision(const char* a_what)
		{
			const auto cs = Camera::ReadCollision();
			REX::INFO("T2 collision [{}]: factor {:.5f} timer {:.4f} wanted-to-final {:.4f} m (read {})", a_what, cs.factor, cs.timer, cs.wantedToFinal, cs.ok);
		}

		bool StageBaseline(const Ctx& c)
		{
			if (c.first) {
				s_factor = s_timer = s_w2f = Range{};
				LogCollision("baseline start");
			}
			SampleCollision();
			if (c.t < 6.0) {
				return false;
			}
			s_baseFactor = s_factor;
			s_baseTimer = s_timer;
			s_baseW2f = s_w2f;
			Result(std::format("T2.0 baseline (no cast of ours, standing still, 6 s): collision factor {:.5f} to {:.5f}, timer {:.4f} to {:.4f}, wanted-to-final {:.4f} to {:.4f} m over {} samples",
				s_factor.lo, s_factor.hi, s_timer.lo, s_timer.hi, s_w2f.lo, s_w2f.hi, s_factor.n));
			return true;
		}

		// 200 casts a second for 30 s through the queue. Ten patterns in turn: eight horizontal rays from the chest (one of them points
		// at the wall behind the player in Save29, so the cast leaves a fraction below 1 in the camera object), one straight down
		// and one steep diagonal.
		struct Out
		{
			WorldProbe::Ticket ticket;
			double             at;
		};
		std::deque<Out> s_out;
		std::uint64_t   s_sub{ 0 }, s_ans{ 0 }, s_ansOk{ 0 }, s_ansHit{ 0 }, s_ansNo{ 0 };
		double          s_nextCast{ 0.0 }, s_maxLatency{ 0.0 }, s_latSum{ 0.0 };
		WorldProbe::Stats s_stats0;
		double          s_stressEnd{ 0.0 };

		void SubmitPattern(const Frame::Snapshot& a_snap, const std::uint64_t a_i)
		{
			const auto&        p = a_snap.position;
			const RE::NiPoint3 chest{ p.x, p.y, p.z + 1.2f };
			const auto         k = a_i % 10;
			RE::NiPoint3       to{};
			if (k < 8) {
				const float a = a_snap.angle.z + static_cast<float>(k) * (kPi / 4.0f);
				to = { chest.x + std::sin(a) * 6.0f, chest.y + std::cos(a) * 6.0f, chest.z };
			} else if (k == 8) {
				to = { chest.x, chest.y, chest.z - 3.0f };
			} else {
				const float pitch = 60.0f * kPi / 180.0f;
				to = { chest.x + std::sin(a_snap.angle.z) * std::cos(pitch) * 3.0f, chest.y + std::cos(a_snap.angle.z) * std::cos(pitch) * 3.0f, chest.z - std::sin(pitch) * 3.0f };
			}
			const auto t = WorldProbe::Submit(chest, to, Config::Get().probeRadius);
			++s_sub;
			if (t == 0) {
				++s_ans;
				++s_ansNo;
				return;
			}
			s_out.push_back({ t, Frame::Now() });
		}

		void DrainAnswers()
		{
			while (!s_out.empty()) {
				const auto r = WorldProbe::Poll(s_out.front().ticket);
				if (!r) {
					break;
				}
				const double lat = Frame::Now() - s_out.front().at;
				s_maxLatency = (std::max)(s_maxLatency, lat);
				s_latSum += lat;
				++s_ans;
				if (r->ok) {
					++s_ansOk;
					s_ansHit += r->hit ? 1 : 0;
				} else {
					++s_ansNo;
				}
				s_out.pop_front();
			}
		}

		bool StageStress(const Ctx& c)
		{
			if (c.first) {
				s_factor = s_timer = s_w2f = Range{};
				s_out.clear();
				s_sub = s_ans = s_ansOk = s_ansHit = s_ansNo = 0;
				s_maxLatency = s_latSum = 0.0;
				s_nextCast = c.now;
				s_stats0 = WorldProbe::GetStats();
				s_lastLog = c.now;
				s_stressEnd = 0.0;
				LogCollision("stress start");
			}
			SampleCollision();
			if (c.t < 30.0) {
				int guard = 0;
				while (c.now >= s_nextCast && guard++ < 8) {
					SubmitPattern(c.snap, s_sub);
					s_nextCast += 0.005;  // 200 a second
				}
				if (c.now > s_nextCast + 0.1) {
					s_nextCast = c.now;  // no catching up after a stall
				}
			}
			DrainAnswers();
			if (c.now - s_lastLog >= 5.0) {
				s_lastLog = c.now;
				const auto st = WorldProbe::GetStats();
				REX::INFO("T2.0 stress t={:.0f}s: submitted {} answered {} (ok {}, hit {}, not available {}) queued now {} | engine factor so far {:.5f} to {:.5f}, wanted-to-final {:.4f} to {:.4f}", c.t, s_sub, s_ans, s_ansOk, s_ansHit,
					s_ansNo, st.queued, s_factor.lo, s_factor.hi, s_w2f.lo, s_w2f.hi);
			}
			if (c.t >= 30.0 && s_stressEnd == 0.0) {
				s_stressEnd = c.now;
			}
			if (s_stressEnd == 0.0 || (c.now - s_stressEnd < 3.0 && s_ans < s_sub)) {
				return false;
			}
			const auto  st = WorldProbe::GetStats();
			const float dF = std::fabs(s_factor.hi - s_baseFactor.hi) + std::fabs(s_factor.lo - s_baseFactor.lo);
			Result(std::format("T2.0 stress (30 s standing still, {} casts submitted = {:.0f}/s, {} answered, {} ok, {} hit, {} not available, longest wait for an answer {:.0f} ms, mean {:.1f} ms): collision factor {:.5f} to {:.5f} against {:.5f} to {:.5f} in the baseline "
							   "(difference {:.6f}), timer {:.4f} to {:.4f} against {:.4f} to {:.4f}, wanted-to-final {:.4f} to {:.4f} against {:.4f} to {:.4f} m, {} samples -> {}",
				s_sub, static_cast<double>(s_sub) / 30.0, s_ans, s_ansOk, s_ansHit, s_ansNo, s_maxLatency * 1000.0, s_ans ? s_latSum / static_cast<double>(s_ans) * 1000.0 : 0.0, s_factor.lo, s_factor.hi, s_baseFactor.lo, s_baseFactor.hi, dF,
				s_timer.lo, s_timer.hi, s_baseTimer.lo, s_baseTimer.hi, s_w2f.lo, s_w2f.hi, s_baseW2f.lo, s_baseW2f.hi, s_factor.n, dF < 0.0005f && s_ansOk > 0 ? "UNCHANGED (pass)" : "CHANGED or no casts (see lines above)"));
			Result(std::format("T2.0 queue: {} casts executed inside the Update hook in {} frames with casts of {} hook calls (at most {} in one frame), {} requests answered without running, {} still queued, broken={}",
				st.executed - s_stats0.executed, st.framesWithCasts - s_stats0.framesWithCasts, st.frames - s_stats0.frames, st.maxPerFrame, st.unavailable - s_stats0.unavailable, st.queued, st.broken));
			return true;
		}

		// ---- a scan of the surroundings: rings of casts at four heights -------------------------------------------------
		struct Scan
		{
			WorldProbe::Batch batch;
			bool              pending{ false };
			double            started{ 0.0 };
			std::string       label;
			RE::NiPoint3      pos{};
		};
		Scan s_scan;
		constexpr std::array<float, 4> kScanHeights{ 0.3f, 0.9f, 1.5f, 2.1f };
		constexpr int                  kScanDirs = 36;
		constexpr float                kScanRange = 6.0f;

		void StartScan(const std::string& a_label, const Frame::Snapshot& a_snap, const double a_now)
		{
			s_scan.batch.Clear();
			s_scan.pending = true;
			s_scan.started = a_now;
			s_scan.label = a_label;
			s_scan.pos = a_snap.position;
			for (const float h : kScanHeights) {
				for (int k = 0; k < kScanDirs; ++k) {
					const float a = static_cast<float>(k) * (2.0f * kPi / static_cast<float>(kScanDirs));
					const RE::NiPoint3 from{ s_scan.pos.x, s_scan.pos.y, s_scan.pos.z + h };
					s_scan.batch.Add(from, { from.x + std::sin(a) * kScanRange, from.y + std::cos(a) * kScanRange, from.z }, 0.1f);
				}
			}
		}

		// true when the scan has been logged (or given up on)
		bool PollScan(const double a_now)
		{
			if (!s_scan.pending) {
				return true;
			}
			if (!s_scan.batch.Done()) {
				if (a_now - s_scan.started > 5.0) {
					s_scan.pending = false;
					REX::WARN("T2 scan [{}]: not answered within 5 s", s_scan.label);
					return true;
				}
				return false;
			}
			s_scan.pending = false;
			REX::INFO("T2 scan [{}] around ({:.2f},{:.2f},{:.2f}): distance to the first surface in tenths of a metre, '.' = nothing within {:.0f} m, one cell per 10 degrees of world heading (0 = +y, 90 = +x), rows are heights above the feet",
				s_scan.label, s_scan.pos.x, s_scan.pos.y, s_scan.pos.z, kScanRange);
			for (std::size_t hi = 0; hi < kScanHeights.size(); ++hi) {
				std::string line;
				for (int k = 0; k < kScanDirs; ++k) {
					if (k % 9 == 0) {
						line += std::format("[{}] ", k * 10);
					}
					const auto& r = s_scan.batch[hi * kScanDirs + static_cast<std::size_t>(k)];
					line += !r.ok ? "? " : (!r.hit ? ". " : std::format("{} ", static_cast<int>(std::lround((r.fraction * kScanRange + 0.1f) * 10.0f))));
				}
				REX::INFO("T2 scan [{}] z+{:.1f}: {}", s_scan.label, kScanHeights[hi], line);
			}
			return true;
		}

		bool StageSurroundings(const Ctx& c)
		{
			if (c.first) {
				StartScan("start", c.snap, c.now);
			}
			return PollScan(c.now);
		}

		// ---- T2.0: floor or no floor ----------------------------------------------------------------------------------
		// The straight-down cast hits at raw fraction 0 (G1 note), but the end point it moves back is the floor plus the sphere radius
		// (REFERENCE 12.3). The check used here: a cast from 1.0 m above the player's feet straight down for 6 m; floor = a hit whose
		// end point minus the radius lies within 0.35 m of the feet height. The other candidate, a 60 degree diagonal ending at the point,
		// is run beside it at the transition.
		enum class FloorClass
		{
			Floor,
			Lower,
			Higher,
			None,
			Unavailable
		};
		constexpr float kFloorTolerance = 0.35f;
		constexpr float kRadius = 0.1f;

		FloorClass ClassifyDown(const WorldProbe::CastResult& r, const float a_feetZ)
		{
			if (!r.ok) {
				return FloorClass::Unavailable;
			}
			if (!r.hit) {
				return FloorClass::None;
			}
			const float dz = r.safePoint.z - kRadius - a_feetZ;
			return std::fabs(dz) <= kFloorTolerance ? FloorClass::Floor : (dz < 0.0f ? FloorClass::Lower : FloorClass::Higher);
		}
		[[nodiscard]] char FloorChar(const FloorClass f)
		{
			switch (f) {
			case FloorClass::Floor: return 'F';
			case FloorClass::Lower: return 'L';
			case FloorClass::Higher: return 'H';
			case FloorClass::None: return 'N';
			default: return '?';
			}
		}

		constexpr int   kGridHalf = 16;  // 33 x 33 points
		constexpr float kGridStep = 1.5f;
		constexpr int   kFineSteps = 12;
		WorldProbe::Batch s_grid;
		WorldProbe::Batch s_fine;
		int               s_floorPhase{ 0 };
		double            s_floorStarted{ 0.0 };
		RE::NiPoint3      s_floorBase{};
		RE::NiPoint3      s_fineA{}, s_fineU{};  // fine scan start (a floor cell) and its unit direction
		bool              s_haveLedge{ false };

		bool StageFloor(const Ctx& c)
		{
			const float feetZ = s_floorBase.z;
			if (c.first) {
				s_floorPhase = 0;
				s_grid.Clear();
				s_fine.Clear();
				s_floorBase = c.snap.position;
				s_floorStarted = c.now;
				s_haveLedge = false;
				for (int iy = -kGridHalf; iy <= kGridHalf; ++iy) {
					for (int ix = -kGridHalf; ix <= kGridHalf; ++ix) {
						const RE::NiPoint3 from{ s_floorBase.x + static_cast<float>(ix) * kGridStep, s_floorBase.y + static_cast<float>(iy) * kGridStep, s_floorBase.z + 1.0f };
						s_grid.Add(from, { from.x, from.y, from.z - 6.0f }, kRadius);
					}
				}
			}
			if (c.now - s_floorStarted > 20.0) {
				Result("T2.0 floor: not answered within 20 s");
				return true;
			}
			constexpr int n = 2 * kGridHalf + 1;
			if (s_floorPhase == 0) {
				if (!s_grid.Done()) {
					return false;
				}
				int count[5]{};
				std::vector<FloorClass> cls(static_cast<std::size_t>(n * n));
				for (std::size_t i = 0; i < cls.size(); ++i) {
					cls[i] = ClassifyDown(s_grid[i], feetZ);
					++count[static_cast<int>(cls[i])];
				}
				REX::INFO("T2 floor grid around ({:.2f},{:.2f},{:.2f}), {} x {} points {} m apart, a down cast from 1.0 m above the feet over 6 m (F floor within {:.2f} m of the feet, L lower, H higher, N nothing, ? unavailable); north = +y at the top:",
					s_floorBase.x, s_floorBase.y, s_floorBase.z, n, n, kGridStep, kFloorTolerance);
				for (int iy = n - 1; iy >= 0; --iy) {
					std::string row;
					for (int ix = 0; ix < n; ++ix) {
						row += FloorChar(cls[static_cast<std::size_t>(iy * n + ix)]);
					}
					REX::INFO("T2 floor grid y{:+.1f}: {}", static_cast<float>(iy - kGridHalf) * kGridStep, row);
				}
				// the first neighbouring pair with a floor on one side and a non-floor on the other
				for (int iy = 0; iy < n && !s_haveLedge; ++iy) {
					for (int ix = 0; ix < n && !s_haveLedge; ++ix) {
						const auto here = cls[static_cast<std::size_t>(iy * n + ix)];
						if (here != FloorClass::Floor) {
							continue;
						}
						const int dx[2] = { 1, 0 }, dy[2] = { 0, 1 };
						for (int d = 0; d < 2 && !s_haveLedge; ++d) {
							const int jx = ix + dx[d], jy = iy + dy[d];
							if (jx >= n || jy >= n) {
								continue;
							}
							const auto there = cls[static_cast<std::size_t>(jy * n + jx)];
							if (there == FloorClass::Lower || there == FloorClass::None) {
								s_haveLedge = true;
								s_fineA = { s_floorBase.x + static_cast<float>(ix - kGridHalf) * kGridStep, s_floorBase.y + static_cast<float>(iy - kGridHalf) * kGridStep, s_floorBase.z };
								s_fineU = { static_cast<float>(dx[d]), static_cast<float>(dy[d]), 0.0f };
							}
						}
					}
				}
				Result(std::format("T2.0 floor grid: of {} points {} are floor, {} lower than the floor by more than {:.2f} m, {} higher, {} with nothing within 6 m below, {} unavailable; a floor-to-no-floor transition {}", n * n, count[0], count[1],
					kFloorTolerance, count[2], count[3], count[4], s_haveLedge ? std::format("exists at ({:.2f},{:.2f}) towards ({:+.0f},{:+.0f})", s_fineA.x, s_fineA.y, s_fineU.x, s_fineU.y) : "was NOT found"));
				if (!s_haveLedge) {
					return true;
				}
				// the fine scan across it: every 0.125 m over one grid step, the down cast and the 60 degree diagonal ending at the point
				const float pitch = 60.0f * kPi / 180.0f;
				for (int k = 0; k <= kFineSteps; ++k) {
					const float s = kGridStep * static_cast<float>(k) / static_cast<float>(kFineSteps);
					const RE::NiPoint3 p{ s_fineA.x + s_fineU.x * s, s_fineA.y + s_fineU.y * s, s_floorBase.z };
					const RE::NiPoint3 top{ p.x, p.y, p.z + 1.0f };
					s_fine.Add(top, { top.x, top.y, top.z - 6.0f }, kRadius);
					// diagonal: starts 0.69 m before the point along the direction of travel, 1.2 m up, aimed at the point (60 degrees down), 3 m long
					const RE::NiPoint3 start{ p.x - s_fineU.x * 0.69f, p.y - s_fineU.y * 0.69f, p.z + 1.2f };
					s_fine.Add(start, { start.x + s_fineU.x * std::cos(pitch) * 3.0f, start.y + s_fineU.y * std::cos(pitch) * 3.0f, start.z - std::sin(pitch) * 3.0f }, kRadius);
				}
				s_floorPhase = 1;
				return false;
			}
			if (!s_fine.Done()) {
				return false;
			}
			std::string seq;
			int         agree = 0;
			int         floorN = 0, noN = 0;
			for (int k = 0; k <= kFineSteps; ++k) {
				const float s = kGridStep * static_cast<float>(k) / static_cast<float>(kFineSteps);
				const RE::NiPoint3 p{ s_fineA.x + s_fineU.x * s, s_fineA.y + s_fineU.y * s, s_floorBase.z };
				const auto&        dn = s_fine[static_cast<std::size_t>(k) * 2];
				const auto&        dg = s_fine[static_cast<std::size_t>(k) * 2 + 1];
				const auto         dcls = ClassifyDown(dn, feetZ);
				// the diagonal says floor when its contact lies within 0.4 m of the point horizontally and at floor height
				bool diagFloor = false;
				if (dg.ok && dg.hit) {
					const float hx = dg.safePoint.x - p.x, hy = dg.safePoint.y - p.y;
					diagFloor = std::hypot(hx, hy) <= 0.4f && std::fabs(dg.safePoint.z - kRadius - feetZ) <= kFloorTolerance;
				}
				const bool downFloor = dcls == FloorClass::Floor;
				agree += downFloor == diagFloor ? 1 : 0;
				(downFloor ? floorN : noN) += 1;
				seq += std::format(" {:.3f}:{}{}/{}{}", s, FloorChar(dcls), dn.ok && dn.hit ? std::format("{:+.2f}", dn.safePoint.z - kRadius - feetZ) : std::string("-"), diagFloor ? 'F' : 'N',
					dg.ok && dg.hit ? std::format("{:+.2f}", dg.safePoint.z - kRadius - feetZ) : std::string("-"));
			}
			REX::INFO("T2 floor transition from ({:.2f},{:.2f}) along ({:+.0f},{:+.0f}) every {:.3f} m, each as down-class and height of the floor under the point / diagonal verdict and its contact height:{}", s_fineA.x, s_fineA.y, s_fineU.x, s_fineU.y,
				kGridStep / static_cast<float>(kFineSteps), seq);
			Result(std::format("T2.0 floor method: the down cast from 1.0 m above the feet calls {} of the {} points across the transition floor and {} no floor (it changes from F at the start to a non-floor within {:.2f} m); the diagonal agrees with it at {} of {} points",
				floorN, kFineSteps + 1, noN, kGridStep, agree, kFineSteps + 1));
			return true;
		}

		// ---- T2.3: the sensor at several spots --------------------------------------------------------------------------
		// At each spot: a scan of the surroundings (above), then CoverSensor::Find toward behind, forward, right and left of the player's
		// heading, one RESULT line each, next to what the scan says in that direction (the nearest cell of 10 degrees, at the four heights)
		// so that the line can be checked against the surroundings. The rig passes the direction as the sensor's intent; it does not turn
		// the player (no verified call for that), which makes no difference to the sensor.
		std::optional<Core::CoverSensor> s_sensor;
		CoverSense::ProbeAdapter         s_adapter;

		Core::Vec2  s_extraDir{};
		const char* s_extraName{ "" };
		bool        s_haveExtra{ false };

		struct Spot
		{
			int           phase{ 0 };  // 0 scan, 1 finds, 2 key test, 3 key wait, 4 done
			int           dir{ 0 };
			bool          asked{ false };
			double        askedAt{ 0.0 };
			RE::NiPoint3  pos{};
			float         heading{ 0.0f };
			std::uint32_t keyBefore{ 0 };
			double        keyAt{ 0.0 };
			bool          have{ false };  // a candidate was found at this spot: Track runs on it once
			Core::CoverCandidate cand{};
		};
		Spot                     s_spot;
		std::vector<std::string> s_spotSummary;

		[[nodiscard]] std::string ScanAlong(const float a_headingDeg)
		{
			int k = static_cast<int>(std::lround(a_headingDeg / 10.0f));
			k = ((k % kScanDirs) + kScanDirs) % kScanDirs;
			std::string out = std::format("scan at world heading {} deg:", k * 10);
			for (std::size_t hi = 0; hi < kScanHeights.size(); ++hi) {
				const auto& r = s_scan.batch[hi * kScanDirs + static_cast<std::size_t>(k)];
				out += !r.ok ? std::format(" z+{:.1f}=?", kScanHeights[hi]) : (!r.hit ? std::format(" z+{:.1f}=clear", kScanHeights[hi]) : std::format(" z+{:.1f}={:.2f}m", kScanHeights[hi], r.fraction * kScanRange + 0.1f));
			}
			return out;
		}

		bool SpotStage(const Ctx& c, const char* a_label, const bool a_keyTest)
		{
			if (c.first) {
				s_spot = Spot{};
				s_spot.pos = c.snap.position;
				s_spot.heading = c.snap.angle.z;
				StartScan(a_label, c.snap, c.now);
				if (!s_sensor) {
					s_sensor.emplace(s_adapter, CoverSense::Settings());
				}
				s_sensor->Cancel();
				s_sensor->SetSettings(CoverSense::Settings());
			}
			auto& sensor = *s_sensor;
			if (s_spot.phase == 0) {
				if (!PollScan(c.now)) {
					return false;
				}
				s_spot.phase = 1;
			}
			if (s_spot.phase == 1) {
				const float      h = s_spot.heading;
				const Core::Vec2 fwd{ std::sin(h), std::cos(h) };
				const Core::Vec2 right{ fwd.y, -fwd.x };
				const Core::Vec2 dirs[5] = { fwd * -1.0f, fwd, right, right * -1.0f, s_extraDir };
				const char*      names[5] = { "behind", "forward", "right", "left", s_extraName };
				const int        nDirs = s_haveExtra ? 5 : 4;
				sensor.Update();
				if (s_spot.asked && sensor.HasResult()) {
					const auto  r = sensor.TakeResult();
					const auto& d = dirs[s_spot.dir];
					const float hd = CoverSense::HeadingDeg(d);
					Result(std::format("T2.3 find [{}, {} = heading {:.0f} deg] from ({:.2f},{:.2f},{:.2f}): {} | {}", a_label, names[s_spot.dir], hd, s_spot.pos.x, s_spot.pos.y, s_spot.pos.z, CoverSense::Describe(r),
						ScanAlong(hd < 0.0f ? hd + 360.0f : hd)));
					if (!s_spot.have && r.candidate.kind != Core::CoverKind::None) {
						s_spot.have = true;
						s_spot.cand = r.candidate;
					}
					s_spotSummary.push_back(std::format("{} {}: {}", a_label, names[s_spot.dir],
						r.candidate.kind == Core::CoverKind::None ? std::string("none (") + Core::ToString(r.candidate.why) + ")" : std::string(Core::ToString(r.candidate.kind))));
					s_spot.asked = false;
					++s_spot.dir;
				} else if (s_spot.asked && c.now - s_spot.askedAt > 5.0) {
					sensor.Cancel();
					Result(std::format("T2.3 find [{}, {}]: no answer within 5 s", a_label, names[s_spot.dir]));
					s_spot.asked = false;
					++s_spot.dir;
				}
				if (!s_spot.asked && s_spot.dir < nDirs) {
					s_spot.asked = sensor.Find({ s_spot.pos.x, s_spot.pos.y, s_spot.pos.z }, dirs[s_spot.dir]);
					s_spot.askedAt = c.now;
					return false;
				}
				if (s_spot.dir < nDirs) {
					return false;
				}
				s_spot.phase = s_spot.have ? 5 : (a_keyTest ? 2 : 4);
				s_spot.asked = false;
			}
			if (s_spot.phase == 5) {
				sensor.Update();
				if (!s_spot.asked) {
					s_spot.asked = sensor.Track(s_spot.cand, { s_spot.pos.x, s_spot.pos.y, s_spot.pos.z });
					s_spot.askedAt = c.now;
					return false;
				}
				if (sensor.HasResult()) {
					Result(std::format("T2.3 track [{}] on the first candidate, from the same position: {}", a_label, CoverSense::Describe(sensor.TakeResult())));
					s_spot.phase = a_keyTest ? 2 : 4;
				} else if (c.now - s_spot.askedAt > 5.0) {
					sensor.Cancel();
					Result(std::format("T2.3 track [{}]: no answer within 5 s", a_label));
					s_spot.phase = a_keyTest ? 2 : 4;
				} else {
					return false;
				}
			}
			if (s_spot.phase == 2) {
				s_spot.keyBefore = CoverSense::KeyQueries();
				s_spot.keyAt = c.now;
				Rig::Tap(Rig::Key{ 0x68, 0x48 }, 0.1);  // Numpad 8, the sense key
				REX::INFO("T2.3: tapping the sense key (Numpad 8, scan code 0x48) to try the debug key by itself");
				s_spot.phase = 3;
				return false;
			}
			if (s_spot.phase == 3) {
				if (c.now - s_spot.keyAt < 2.5) {
					return false;
				}
				Result(std::format("T2.3 debug key: {} (queries started by the key: {} -> {}; the matching 'COVER candidate' line follows in the log, aimed along the camera's view)",
					CoverSense::KeyQueries() > s_spot.keyBefore ? "the synthetic Numpad 8 reached the game and started a query" : "the synthetic Numpad 8 did NOT start a query", s_spot.keyBefore, CoverSense::KeyQueries()));
				s_spot.phase = 4;
			}
			return true;
		}

		bool StageSpotA(const Ctx& c) { return SpotStage(c, "A spawn", true); }
		bool StageSpotN(const Ctx& c) { return SpotStage(c, "N next to the north wall (real input: D)", false); }

		// Real input: the key is held for at most a_maxSeconds; with a_watch the stage casts ahead of the player and lets go of the key when a
		// surface is nearer than 1.4 m (casts through the queue, one at a time).
		struct Walk
		{
			WorldProbe::Ticket ticket{ 0 };
			RE::NiPoint3       from{};
			RE::NiPoint3       start{};
			float              heading{ 0.0f };
			bool               stopped{ false };
			double             stoppedAt{ 0.0 };
			std::string        why;
		};
		Walk s_walk;

		bool WalkStage(const Ctx& c, const Rig::Key a_key, const double a_maxSeconds, const bool a_watch, const float a_offsetDeg, const char* a_label)
		{
			if (c.first) {
				s_walk = Walk{};
				s_walk.start = c.snap.position;
				s_walk.heading = c.snap.angle.z + a_offsetDeg * kPi / 180.0f;
				Rig::Tap(a_key, a_maxSeconds);
			}
			if (!s_walk.stopped) {
				if (a_watch) {
					if (s_walk.ticket != 0) {
						if (const auto r = WorldProbe::Poll(s_walk.ticket)) {
							s_walk.ticket = 0;
							if (r->ok && r->hit) {
								const float dist = Dist3(s_walk.from, r->point) + kRadius;
								if (dist < 1.4f) {
									Rig::ReleaseEverything("something ahead");
									s_walk.stopped = true;
									s_walk.stoppedAt = c.t;
									s_walk.why = std::format("a surface {:.2f} m ahead at 0.5 m height", dist);
								}
							}
						}
					} else if (c.t > 0.3) {
						const auto& p = c.snap.position;
						s_walk.from = { p.x, p.y, p.z + 0.5f };
						s_walk.ticket = WorldProbe::Submit(s_walk.from, { s_walk.from.x + std::sin(s_walk.heading) * 3.0f, s_walk.from.y + std::cos(s_walk.heading) * 3.0f, s_walk.from.z }, kRadius);
					}
				}
				if (!s_walk.stopped && c.t >= a_maxSeconds) {
					s_walk.stopped = true;
					s_walk.stoppedAt = c.t;
					s_walk.why = std::format("the key was held {:.1f} s", a_maxSeconds);
				}
			}
			if (!s_walk.stopped || c.t < s_walk.stoppedAt + 1.5) {
				return false;
			}
			const auto& p = c.snap.position;
			Result(std::format("T2.3 walk [{}]: stopped because {}; moved from ({:.2f},{:.2f}) to ({:.2f},{:.2f}), {:.2f} m", a_label, s_walk.why, s_walk.start.x, s_walk.start.y, p.x, p.y,
				std::hypot(p.x - s_walk.start.x, p.y - s_walk.start.y)));
			return true;
		}

		// ---- T2.3: discovery of cover-like things around the spawn, and visits ---------------------------------------------------------
		// From the spawn, 72 bearings (5 degrees) of casts at 0.5 m and 1.5 m height out to 20 m. Per bearing: "tall" when both hit at about the same
		// distance, "low" when the 0.5 m cast hits and the 1.5 m cast is clear or much farther. Neighbouring bearings of one class and distance are a
		// cluster; a tall cluster that is under 0.5 m wide is a pole. The nearest low thing, the nearest pole and the nearest wide tall thing (and one
		// more of whichever is left) farther than 2.5 m are the targets: the player is moved to 1.3 m in front of each (eased SetPosition, like S5)
		// and the sensor runs there. Floor under each stand point is checked first (the plaza water is not floor).
		constexpr int                  kDiscDirs = 72;
		constexpr float                kDiscRange = 20.0f;
		constexpr std::array<float, 2> kDiscHeights{ 0.5f, 1.5f };

		struct Target
		{
			bool         valid{ false };
			std::string  label;
			RE::NiPoint3 stand{};
			Core::Vec2   toward{};
			float        surface{ 0.0f };
			float        bearing{ 0.0f };
		};
		std::array<Target, 4> s_targets;
		WorldProbe::Batch     s_disc;
		WorldProbe::Batch     s_discFloor;
		int                   s_discPhase{ 0 };
		double                s_discStarted{ 0.0 };
		RE::NiPoint3          s_discOrigin{};
		struct Cluster
		{
			int         cls{ 0 };  // 1 tall, 2 low
			int         first{ 0 }, count{ 0 };
			float       dist{ 1e9f };
			int         nearestIdx{ 0 };
		};
		std::vector<Cluster> s_clusters;
		std::vector<Target>  s_pending;  // candidates whose floor is being checked

		bool StageDiscovery(const Ctx& c)
		{
			if (c.first) {
				s_discPhase = 0;
				s_disc.Clear();
				s_discFloor.Clear();
				s_targets = {};
				s_discStarted = c.now;
				s_discOrigin = c.snap.position;
				for (int k = 0; k < kDiscDirs; ++k) {
					const float a = static_cast<float>(k) * (2.0f * kPi / static_cast<float>(kDiscDirs));
					for (const float h : kDiscHeights) {
						const RE::NiPoint3 from{ s_discOrigin.x, s_discOrigin.y, s_discOrigin.z + h };
						s_disc.Add(from, { from.x + std::sin(a) * kDiscRange, from.y + std::cos(a) * kDiscRange, from.z }, kRadius);
					}
				}
			}
			if (c.now - s_discStarted > 10.0) {
				Result("T2.3 discovery: not answered within 10 s");
				return true;
			}
			if (s_discPhase == 0) {
				if (!s_disc.Done()) {
					return false;
				}
				struct Col
				{
					int   cls{ 0 };
					float d{ 0.0f };
				};
				std::vector<Col> col(kDiscDirs);
				for (int k = 0; k < kDiscDirs; ++k) {
					const auto& lo = s_disc[static_cast<std::size_t>(k) * 2];
					const auto& hi = s_disc[static_cast<std::size_t>(k) * 2 + 1];
					const bool  hl = lo.ok && lo.hit, hh = hi.ok && hi.hit;
					const float dl = lo.fraction * kDiscRange + kRadius, dh = hi.fraction * kDiscRange + kRadius;
					if (hl && hh && std::fabs(dl - dh) < 0.5f) {
						col[static_cast<std::size_t>(k)] = { 1, dl };
					} else if (hl && (!hh || dh - dl > 1.5f)) {
						col[static_cast<std::size_t>(k)] = { 2, dl };
					}
				}
				s_clusters.clear();
				for (int k = 0; k < kDiscDirs; ++k) {
					const auto& cur = col[static_cast<std::size_t>(k)];
					if (cur.cls == 0) {
						continue;
					}
					const auto& prev = col[static_cast<std::size_t>((k + kDiscDirs - 1) % kDiscDirs)];
					const bool  continues = prev.cls == cur.cls && std::fabs(prev.d - cur.d) < 0.6f && !s_clusters.empty();
					if (continues) {
						auto& cl = s_clusters.back();
						++cl.count;
						if (cur.d < cl.dist) {
							cl.dist = cur.d;
							cl.nearestIdx = k;
						}
					} else {
						s_clusters.push_back({ cur.cls, k, 1, cur.d, k });
					}
				}
				std::string list;
				for (const auto& cl : s_clusters) {
					const float widthM = cl.dist * static_cast<float>(cl.count) * 5.0f * 0.017453292f;
					list += std::format("\n  {} from bearing {} deg, {} bearings, nearest {:.2f} m at {} deg, about {:.1f} m wide", cl.cls == 1 ? "tall" : "low ", cl.first * 5, cl.count, cl.dist, cl.nearestIdx * 5, widthM);
				}
				Result(std::format("T2.3 discovery from ({:.2f},{:.2f},{:.2f}): {} clusters within {:.0f} m:{}", s_discOrigin.x, s_discOrigin.y, s_discOrigin.z, s_clusters.size(), kDiscRange, list));

				// pick: nearest low, nearest pole, nearest wide tall, then one more
				const auto widthOf = [](const Cluster& cl) { return cl.dist * static_cast<float>(cl.count) * 5.0f * 0.017453292f; };
				std::vector<const Cluster*> picks;
				const auto                  best = [&](const auto& a_pred) -> const Cluster* {
                    const Cluster* b = nullptr;
                    for (const auto& cl : s_clusters) {
                        if (cl.dist >= 2.5f && a_pred(cl) && std::find(picks.begin(), picks.end(), &cl) == picks.end() && (!b || cl.dist < b->dist)) {
                            b = &cl;
                        }
                    }
                    return b;
				};
				const auto add = [&](const Cluster* cl) {
					if (cl) {
						picks.push_back(cl);
					}
				};
				add(best([](const Cluster& cl) { return cl.cls == 2; }));
				add(best([&](const Cluster& cl) { return cl.cls == 1 && widthOf(cl) < 0.5f; }));
				add(best([&](const Cluster& cl) { return cl.cls == 1 && widthOf(cl) >= 1.5f; }));
				add(best([](const Cluster& cl) { return cl.cls == 2; }));
				add(best([](const Cluster&) { return true; }));
				s_pending.clear();
				for (const auto* cl : picks) {
					if (s_pending.size() >= 4) {
						break;
					}
					const float bearing = static_cast<float>(cl->nearestIdx) * 5.0f * 0.017453292f;
					Target      t;
					t.valid = true;
					t.toward = { std::sin(bearing), std::cos(bearing) };
					t.surface = cl->dist;
					t.bearing = static_cast<float>(cl->nearestIdx) * 5.0f;
					t.label = std::format("{} at {:.0f} deg {:.1f} m{}", cl->cls == 1 ? (widthOf(*cl) < 0.5f ? "pole" : "tall") : "low", t.bearing, cl->dist, "");
					const float back = (std::max)(cl->dist - 1.3f, 0.0f);
					t.stand = { s_discOrigin.x + t.toward.x * back, s_discOrigin.y + t.toward.y * back, s_discOrigin.z };
					s_pending.push_back(t);
					s_discFloor.Add({ t.stand.x, t.stand.y, t.stand.z + 1.0f }, { t.stand.x, t.stand.y, t.stand.z - 2.0f }, kRadius);
				}
				s_discPhase = 1;
				return s_pending.empty() ? (Result("T2.3 discovery: no target farther than 2.5 m"), true) : false;
			}
			if (!s_discFloor.Done()) {
				return false;
			}
			std::size_t n = 0;
			for (std::size_t i = 0; i < s_pending.size(); ++i) {
				auto&       t = s_pending[i];
				const auto& f = s_discFloor[i];
				const auto  cls = ClassifyDown(f, s_discOrigin.z);
				if (cls == FloorClass::Floor) {
					t.stand.z = f.safePoint.z - kRadius;
					s_targets[n++] = t;
					Result(std::format("T2.3 target {}: {} -> stand point ({:.2f},{:.2f},{:.2f}) floor ok", n, t.label, t.stand.x, t.stand.y, t.stand.z));
				} else {
					Result(std::format("T2.3 target skipped: {} (the floor under its stand point is class {})", t.label, FloorChar(cls)));
				}
			}
			return true;
		}

		// A visit: eased move to the stand point, a second to settle, then the spot stage with the bearing of the target as a fifth direction.
		struct Visit
		{
			int                      phase{ 0 };
			Engine::PlayerControl::EasedMove move;
			double                   settleAt{ 0.0 };
			bool                     spotFirst{ true };
		};
		Visit s_visit;

		bool VisitStage(const Ctx& c, const std::size_t a_index)
		{
			const auto& t = s_targets[a_index];
			if (!t.valid) {
				return true;
			}
			if (c.first) {
				s_visit = Visit{};
				const auto& p = c.snap.position;
				const float dist = std::hypot(t.stand.x - p.x, t.stand.y - p.y);
				REX::INFO("T2.3 visit {}: {} - moving {:.1f} m from ({:.2f},{:.2f}) with an eased SetPosition", a_index + 1, t.label, dist, p.x, p.y);
				s_visit.move.Begin({ t.stand.x - p.x, t.stand.y - p.y, t.stand.z - p.z }, (std::max)(1.0f, dist / 3.0f), true);
			}
			if (s_visit.phase == 0) {
				if (!s_visit.move.Step(c.now)) {
					return false;
				}
				s_visit.phase = 1;
				s_visit.settleAt = c.now + 1.5;
				return false;
			}
			if (s_visit.phase == 1) {
				if (c.now < s_visit.settleAt) {
					return false;
				}
				s_visit.phase = 2;
			}
			Ctx inner{ c.snap, c.now, c.t, s_visit.spotFirst };
			s_visit.spotFirst = false;
			s_extraDir = t.toward;
			s_extraName = "toward the target";
			s_haveExtra = true;
			const auto done = SpotStage(inner, t.label.c_str(), false);
			if (done) {
				s_haveExtra = false;
			}
			return done;
		}

		bool StageVisit0(const Ctx& c) { return VisitStage(c, 0); }
		bool StageVisit1(const Ctx& c) { return VisitStage(c, 1); }
		bool StageVisit2(const Ctx& c) { return VisitStage(c, 2); }
		bool StageVisit3(const Ctx& c) { return VisitStage(c, 3); }

		bool StageStrafeNorth(const Ctx& c) { return WalkStage(c, Rig::kKeyD, 1.6, true, 90.0f, "D (to the right = north) up to 1.6 s"); }

		bool StageSummary(const Ctx&)
		{
			std::string all;
			for (const auto& s : s_spotSummary) {
				all += "\n  " + s;
			}
			Result(std::format("T2.3 summary of the {} finds:{}", s_spotSummary.size(), all));
			return true;
		}

		// ---- sense on stop ([Sensor] bSenseOnStop): no key, the plugin senses by itself ---------------------------------------------------
		// Walk with real input (W), stop, and count the queries the plugin starts on its own: none while moving, one after each stop, none while
		// standing on. Then turn the view with synthetic mouse movement and count again. The `COVER candidate` and `COVER scan` lines are in the log.
		struct Sense
		{
			int           phase{ 0 };
			double        phaseAt{ 0.0 };
			std::uint32_t base{ 0 };
			bool          acted{ false };
			RE::NiPoint3  pos0{};
			float         yaw0{ 0.0f };
			double        lastSend{ 0.0 };
			float         turned{ 0.0f };
			std::uint32_t atStart{ 0 };
			std::string   walkFail;
		};
		Sense s_sense;

		bool StageSenseOnStop(const Ctx& c)
		{
			auto& s = s_sense;
			if (c.first) {
				s = Sense{};
				s.phaseAt = c.now;
				s.base = s.atStart = CoverSense::AutoQueries();
				REX::INFO("T2 sense-on-stop test starting at ({:.2f},{:.2f}); automatic queries so far {}", c.snap.position.x, c.snap.position.y, s.base);
			}
			const double dt = c.now - s.phaseAt;
			const auto   next = [&] {
                ++s.phase;
                s.phaseAt = c.now;
                s.base = CoverSense::AutoQueries();
                s.acted = false;
			};
			const auto fresh = [&] { return CoverSense::AutoQueries() - s.base; };
			const auto walk = [&](const char* a_label, const bool a_check) {
				if (!s.acted) {
					s.acted = true;
					s.pos0 = c.snap.position;
					Rig::Tap(Rig::kKeyW, 1.2);
					return false;
				}
				if (dt < 1.5) {
					return false;
				}
				const auto& p = c.snap.position;
				Result(std::format("T2 sense-on-stop {}: W held 1.2 s, moved {:.2f} m; automatic queries started while moving (and 0.3 s after letting go): {} (expected 0){}", a_label, std::hypot(p.x - s.pos0.x, p.y - s.pos0.y), fresh(),
					a_check ? "" : ""));
				next();
				return true;
			};
			const auto stand = [&](const char* a_label, const double a_seconds, const char* a_expected) {
				if (dt < a_seconds) {
					return false;
				}
				Result(std::format("T2 sense-on-stop {}: automatic queries started in the {:.1f} s of standing: {} (expected {})", a_label, a_seconds, fresh(), a_expected));
				next();
				return true;
			};
			switch (s.phase) {
			case 0: (void)stand("standing at the start", 2.5, "0, nothing has moved"); return false;
			case 1: (void)walk("walk 1", true); return false;
			case 2: (void)stand("after stop 1", 2.0, "1, from the first stop"); return false;
			case 3: (void)walk("walk 2", true); return false;
			case 4: (void)stand("after stop 2", 2.0, "1, from the second stop"); return false;
			case 5: (void)stand("standing on, no new movement", 2.5, "0"); return false;
			case 6: {
				// turn the view with the mouse: relative movement every 40 ms until it has turned 50 degrees (at most 3.5 s)
				float yaw{ 0.0f };
				const bool have = Camera::ReadViewYaw(yaw);
				if (!s.acted) {
					s.acted = true;
					s.yaw0 = yaw;
					s.lastSend = c.now;
					REX::INFO("T2 sense-on-stop: turning the view with synthetic mouse movement from yaw {:.1f} deg (read {})", yaw * 180.0f / kPi, have);
				}
				if (have) {
					s.turned = std::fabs(std::atan2(std::sin(yaw - s.yaw0), std::cos(yaw - s.yaw0))) * 180.0f / kPi;
				}
				if (s.turned < 50.0f && dt < 3.5) {
					if (c.now - s.lastSend >= 0.04) {
						s.lastSend = c.now;
						Rig::MouseMove(150, 0);
					}
					return false;
				}
				Result(std::format("T2 sense-on-stop turn: the synthetic mouse movement turned the view by {:.1f} degrees in {:.1f} s; automatic queries started by the end of the movement: {} (expected 0 while it turns)", s.turned, dt, fresh()));
				next();
				s.walkFail = s.turned > 30.0f ? "" : "the view turned 30 degrees or less: the turn trigger could not be tested";
				return false;
			}
			case 7:
				if (dt < 2.0) {
					return false;
				}
				Result(std::format("T2 sense-on-stop after the turn: automatic queries started in the 2.0 s after the movement stopped: {} (expected 1 if the view turned more than 30 degrees){}", fresh(),
					s.walkFail.empty() ? "" : std::string(" - ") + s.walkFail));
				next();
				return false;
			default: {
				Result(std::format("T2 sense-on-stop total: {} automatic queries in the whole test (expected 3: two stops and one turn); the matching COVER candidate / COVER scan lines are in the log", CoverSense::AutoQueries() - s.atStart));
				return true;
			}
			}
		}

		// ---- T2.4: the wall end ------------------------------------------------------------------------------------------------------------
		// Run 34 found, from (-716.93, 1934.98), a wall toward heading 180 deg at 0.95 m with its open end 1.02 m to the right. A reference Find
		// from there gives the line, the normal and the end (the sensor's own numbers); then three spots along the line: 0.5 m inside the end, at
		// the end and 0.3 m outside it, always 0.35 m from the wall surface, each with Finds facing the wall straight and 30 degrees off to either
		// side, next to the scan of the spot.
		struct EndRef
		{
			bool                 have{ false };
			Core::CoverCandidate cand{};
			Core::Vec2           end{};  // the end of the wall surface along the line of centres
		};
		EndRef s_endRef;

		struct EndSpot
		{
			int           phase{ 0 };  // 0 move, 1 settle, 2 scan, 3 finds
			double        phaseAt{ 0.0 };
			Engine::PlayerControl::EasedMove move;
			RE::NiPoint3  pos{};
			int           dir{ 0 };
			bool          asked{ false };
			double        askedAt{ 0.0 };
		};
		EndSpot s_endSpot;

		[[nodiscard]] Core::Vec2 Rotate(const Core::Vec2 a_v, const float a_deg)
		{
			const float r = a_deg * kPi / 180.0f;
			return { a_v.x * std::cos(r) - a_v.y * std::sin(r), a_v.x * std::sin(r) + a_v.y * std::cos(r) };
		}

		bool StageEndReference(const Ctx& c)
		{
			if (!s_sensor) {
				s_sensor.emplace(s_adapter, CoverSense::Settings());
			}
			auto& sensor = *s_sensor;
			if (c.first) {
				s_endSpot = EndSpot{};
				s_endRef = EndRef{};
				sensor.Cancel();
				sensor.SetSettings(CoverSense::Settings());
				const auto& p = c.snap.position;
				const RE::NiPoint3 target{ -716.93f, 1934.98f, p.z };
				REX::INFO("T2.4 reference: moving {:.1f} m to ({:.2f},{:.2f}) with an eased SetPosition", std::hypot(target.x - p.x, target.y - p.y), target.x, target.y);
				s_endSpot.move.Begin({ target.x - p.x, target.y - p.y, 0.0f }, 1.5f, true);
			}
			if (s_endSpot.phase == 0) {
				if (!s_endSpot.move.Step(c.now)) {
					return false;
				}
				s_endSpot.phase = 1;
				s_endSpot.phaseAt = c.now;
				return false;
			}
			if (s_endSpot.phase == 1) {
				if (c.now - s_endSpot.phaseAt < 1.5) {
					return false;
				}
				s_endSpot.asked = sensor.Find({ c.snap.position.x, c.snap.position.y, c.snap.position.z }, { 0.0f, -1.0f });  // toward heading 180
				s_endSpot.askedAt = c.now;
				s_endSpot.pos = c.snap.position;
				s_endSpot.phase = 2;
				return false;
			}
			sensor.Update();
			if (!sensor.HasResult()) {
				if (c.now - s_endSpot.askedAt > 5.0) {
					sensor.Cancel();
					Result("T2.4 reference: no answer within 5 s");
					return true;
				}
				return false;
			}
			const auto r = sensor.TakeResult();
			const auto& cd = r.candidate;
			Result(std::format("T2.4 reference find from ({:.2f},{:.2f},{:.2f}) toward heading 180: {}", s_endSpot.pos.x, s_endSpot.pos.y, s_endSpot.pos.z, CoverSense::Describe(r)));
			if (cd.kind != Core::CoverKind::None && cd.edgeRightKind == Core::EdgeKind::Open) {
				s_endRef.have = true;
				s_endRef.cand = cd;
				s_endRef.end = cd.linePoint + cd.tangent * cd.edgeRight;
				Result(std::format("T2.4 reference: the wall's open end is at ({:.2f},{:.2f}) on the line of centres; normal ({:+.2f},{:+.2f}), tangent ({:+.2f},{:+.2f}); the spots are 0.35 m from the surface", s_endRef.end.x, s_endRef.end.y, cd.normal.x, cd.normal.y,
					cd.tangent.x, cd.tangent.y));
			} else {
				Result("T2.4 reference: no open end to the right of the reference find; the spot stages are skipped");
			}
			return true;
		}

		bool EndSpotStage(const Ctx& c, const char* a_label, const float a_offset)
		{
			if (!s_endRef.have) {
				return true;
			}
			auto& sensor = *s_sensor;
			auto& es = s_endSpot;
			const auto& cd = s_endRef.cand;
			if (c.first) {
				es = EndSpot{};
				sensor.Cancel();
				sensor.SetSettings(CoverSense::Settings());
				// 0.35 m from the surface is 0.25 m from the line of centres; a positive offset is outside the wall (toward the open end)
				const Core::Vec2 p = s_endRef.end + cd.tangent * a_offset + cd.normal * (0.35f - Config::Get().probeRadius);
				const auto&      now = c.snap.position;
				REX::INFO("T2.4 spot [{}]: moving {:.1f} m to ({:.2f},{:.2f})", a_label, std::hypot(p.x - now.x, p.y - now.y), p.x, p.y);
				es.move.Begin({ p.x - now.x, p.y - now.y, cd.floorZ - now.z }, (std::max)(1.0f, std::hypot(p.x - now.x, p.y - now.y) / 1.5f), true);
			}
			if (es.phase == 0) {
				if (!es.move.Step(c.now)) {
					return false;
				}
				es.phase = 1;
				es.phaseAt = c.now;
				return false;
			}
			if (es.phase == 1) {
				if (c.now - es.phaseAt < 1.5) {
					return false;
				}
				es.pos = c.snap.position;
				StartScan(a_label, c.snap, c.now);
				es.phase = 2;
				return false;
			}
			if (es.phase == 2) {
				if (!PollScan(c.now)) {
					return false;
				}
				es.phase = 3;
			}
			const Core::Vec2 towardWall = cd.normal * -1.0f;
			const Core::Vec2 dirs[3] = { towardWall, Rotate(towardWall, 30.0f), Rotate(towardWall, -30.0f) };
			const char*      names[3] = { "straight at the wall", "30 deg off (rotated counter-clockwise)", "30 deg off (rotated clockwise)" };
			sensor.Update();
			if (es.asked && sensor.HasResult()) {
				const auto  r = sensor.TakeResult();
				float       hd = CoverSense::HeadingDeg(dirs[es.dir]);
				hd = hd < 0.0f ? hd + 360.0f : hd;
				Result(std::format("T2.4 find [{}, {}, heading {:.0f} deg] from ({:.2f},{:.2f},{:.2f}): {} | {}", a_label, names[es.dir], hd, es.pos.x, es.pos.y, es.pos.z, CoverSense::Describe(r), ScanAlong(hd)));
				s_spotSummary.push_back(std::format("T2.4 {} {}: {}", a_label, names[es.dir],
					r.candidate.kind == Core::CoverKind::None ? std::string("none (") + Core::ToString(r.candidate.why) + ")"
					                                          : std::format("{} edgeLeft {:.2f} edgeRight {:.2f} foot moved {:+.2f}", Core::ToString(r.candidate.kind), r.candidate.edgeLeft, r.candidate.edgeRight, r.candidate.clampShift)));
				es.asked = false;
				++es.dir;
			} else if (es.asked && c.now - es.askedAt > 5.0) {
				sensor.Cancel();
				Result(std::format("T2.4 find [{}, {}]: no answer within 5 s", a_label, names[es.dir]));
				es.asked = false;
				++es.dir;
			}
			if (!es.asked && es.dir < 3) {
				es.asked = sensor.Find({ es.pos.x, es.pos.y, es.pos.z }, dirs[es.dir]);
				es.askedAt = c.now;
				return false;
			}
			return es.dir >= 3;
		}

		bool StageEndInside(const Ctx& c) { return EndSpotStage(c, "0.5 m inside the end", -0.5f); }
		bool StageEndAt(const Ctx& c) { return EndSpotStage(c, "at the end", 0.0f); }
		bool StageEndOutside(const Ctx& c) { return EndSpotStage(c, "0.3 m outside the end", 0.3f); }

		// ---- the sequence ----------------------------------------------------------------------------------------------
		constexpr std::array<Stage, 20> kStages{ {
			{ "ready", StageReady, false },
			{ "T2.0 baseline (collision factor, no casts)", StageBaseline, false },
			{ "T2.0 stress (200 casts per second for 30 s)", StageStress, false },
			{ "T2 surroundings scan", StageSurroundings, false },
			{ "T2.0 floor or no floor", StageFloor, false },
			{ "T2.3 spot A: the spawn (and the debug key)", StageSpotA, true },
			{ "T2.3 discovery of cover-like things around the spawn", StageDiscovery, false },
			{ "T2.3 strafe: D toward the north wall", StageStrafeNorth, true },
			{ "T2.3 spot N", StageSpotN, true },
			{ "T2.3 visit 1", StageVisit0, false },
			{ "T2.3 visit 2", StageVisit1, false },
			{ "T2.3 visit 3", StageVisit2, false },
			{ "T2.3 visit 4", StageVisit3, false },
			{ "T2.3 summary", StageSummary, false },
			{ "T2 sense on stop (no key)", StageSenseOnStop, true },
			{ "T2.4 reference find at the wall end of run 34", StageEndReference, false },
			{ "T2.4 spot: 0.5 m inside the end", StageEndInside, false },
			{ "T2.4 spot: at the end", StageEndAt, false },
			{ "T2.4 spot: 0.3 m outside the end", StageEndOutside, false },
			{ "T2.4 summary", StageSummary, false },
		} };

		void Finish(const bool a_ok, const char* a_why)
		{
			Rig::ReleaseEverything(a_ok ? "script finished" : "script aborted");
			const auto st = WorldProbe::GetStats();
			REX::INFO("RESULT rig t2: {} ({}) | world probe: {} submitted, {} executed in the Update hook, {} answered without running, broken={}", a_ok ? "all stages ran" : "stopped early", a_why, st.submitted, st.executed, st.unavailable,
				st.broken);
			REX::INFO("TEST COMPLETE{}", a_ok ? "" : " (aborted)");
			s_done = true;
			if (Config::Get().quitAfterTest) {
				if (const auto main = RE::Main::GetSingleton()) {
					main->quitGame = true;
					REX::INFO("S0 rig: Main::quitGame set; the game should close now");
				}
			}
		}
	}

	void Abort(const char* a_why)
	{
		if (!s_done) {
			Finish(false, a_why);
		}
	}

	bool Finished() noexcept
	{
		return s_done;
	}

	void OnTick(const Frame::Snapshot& a_snap)
	{
		if (s_done) {
			return;
		}
		const double now = a_snap.time;
		constexpr std::size_t kTotal = kStages.size();

		if (s_stage > 0) {
			const char* abort = nullptr;
			if (!a_snap.valid || a_snap.paused) {
				abort = "the game paused or left the world";
			} else if (kStages[s_stage].usesKeys && !Rig::InForeground()) {
				abort = "the game window lost the foreground";
			}
			if (abort) {
				Finish(false, abort);
				return;
			}
		}

		if (s_stage >= kTotal) {
			Finish(true, "end of the sequence");
			return;
		}
		if (!s_started) {
			s_started = true;
			s_stageStart = now;
			s_stageFirst = true;
			REX::INFO("S0 rig: t2 stage 1 of {}: {}", kTotal, kStages[0].name);
		}
		const Ctx ctx{ a_snap, now, now - s_stageStart, s_stageFirst };
		s_stageFirst = false;
		if (kStages[s_stage].fn(ctx)) {
			++s_stage;
			if (s_stage == 1) {
				s_stage = (std::min)(static_cast<std::size_t>(Config::Get().t2StartStage), kTotal - 1);
			}
			s_stageStart = now;
			s_stageFirst = true;
			if (s_stage < kTotal) {
				REX::INFO("S0 rig: t2 stage {} of {}: {}", s_stage + 1, kTotal, kStages[s_stage].name);
			}
		}
	}
}
