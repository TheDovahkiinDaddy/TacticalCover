#include "Engine/P3Script.h"

#include <array>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Config.h"
#include "Core/PluginNames.h"
#include "Core/CoverSensor.h"
#include "Engine/AnimRecon.h"
#include "Engine/AudioProbe.h"
#include "Engine/BlindFire.h"
#include "Engine/Camera.h"
#include "Engine/Cover.h"
#include "Engine/CoverPose.h"
#include "Engine/CoverSense.h"
#include "Engine/EquipPending.h"
#include "Engine/FootIK.h"
#include "Engine/Frame.h"
#include "Engine/GraphEvents.h"
#include "Engine/Guard.h"
#include "Engine/Input.h"
#include "Engine/PlayerControl.h"
#include "Engine/ScopeAim.h"
#include "Engine/TallAim.h"
#include "Engine/Rig.h"
#include "Engine/WeaponDraw.h"
#include "Engine/WorldProbe.h"

namespace Engine::P3
{
	namespace
	{
		constexpr float kPi = 3.14159265358979f;
		constexpr float kRad2Deg = 180.0f / kPi;
		constexpr Rig::Key kKeySpace{ 0x20, 0x39 };

		struct Ctx
		{
			const Frame::Snapshot& snap;
			double                 now;
			double                 t;      // seconds since the stage started
			bool                   first;  // first call of the stage
		};
		using StageFn = bool (*)(const Ctx&);
		struct Stage
		{
			const char* name;
			StageFn     fn;
			bool        usesKeys;
		};

		bool        s_done{ false };
		bool        s_started{ false };
		std::size_t s_stage{ 0 };
		double      s_stageStart{ 0.0 };
		bool        s_stageFirst{ true };
		double      s_fgLostSince{ -1.0 };
		bool        s_shotWaiting{ false };  // a screenshot hand-off is in progress: the foreground check waits
		int         s_pass{ 0 };
		int         s_fail{ 0 };
		std::vector<std::string> s_summary;

		void Result(const std::string& a_text)
		{
			REX::INFO("RESULT {}", a_text);
		}

		[[nodiscard]] double Now() { return Frame::Now(); }
		[[nodiscard]] const Frame::Snapshot& Snap() { return Frame::Latest(); }

		const char* Verdict(const bool a_ok)
		{
			++(a_ok ? s_pass : s_fail);
			return a_ok ? "PASS" : "FAIL";
		}

		void Summarise(const std::string& a_name, const bool a_ok)
		{
			s_summary.push_back(std::format("{}: {}", a_name, a_ok ? "PASS" : "FAIL"));
		}

		[[nodiscard]] float WrapDeg(float a_deg)
		{
			while (a_deg > 180.0f) {
				a_deg -= 360.0f;
			}
			while (a_deg < -180.0f) {
				a_deg += 360.0f;
			}
			return a_deg;
		}

		[[nodiscard]] float HeadingOf(const float a_x, const float a_y) { return std::atan2(a_x, a_y) * kRad2Deg; }  // 0 = +y, 90 = +x

		[[nodiscard]] int Stance()
		{
			const auto v = PlayerControl::ReadVar("iSyncActorStance");
			return v ? static_cast<int>(std::lround(*v)) : -1;
		}

		[[nodiscard]] float Dist2(const RE::NiPoint3& a, const RE::NiPoint3& b) { return std::hypot(a.x - b.x, a.y - b.y); }

		// ---- a tiny sequencer: steps with a delay after the previous one and optionally a condition to wait for -----------------
		struct Step
		{
			double                 delay{ 0.0 };
			double                 timeout{ 0.0 };
			std::function<bool()>  cond;       // empty: an action
			std::function<void(bool)> done;    // called once with true (condition met / action) or false (timed out)
			bool                   started{ false };
			double                 readyAt{ 0.0 };
		};

		struct Seq
		{
			std::vector<Step> steps;
			std::size_t       i{ 0 };
			double            last{ 0.0 };
			bool              started{ false };

			void Do(std::function<void()> a_fn, const double a_delay = 0.0)
			{
				Step s;
				s.delay = a_delay;
				s.done = [fn = std::move(a_fn)](bool) { fn(); };
				steps.push_back(std::move(s));
			}
			void Wait(const double a_seconds) { Do([] {}, a_seconds); }
			void Until(std::function<bool()> a_cond, const double a_timeout, std::function<void(bool)> a_done = {})
			{
				Step s;
				s.timeout = a_timeout;
				s.cond = std::move(a_cond);
				s.done = a_done ? std::move(a_done) : [](bool) {};
				steps.push_back(std::move(s));
			}
		};

		// true when every step has run
		bool Run(Seq& a_seq, const double a_now)
		{
			if (!a_seq.started) {
				a_seq.started = true;
				a_seq.last = a_now;
			}
			while (a_seq.i < a_seq.steps.size()) {
				auto& st = a_seq.steps[a_seq.i];
				if (!st.started) {
					st.started = true;
					st.readyAt = a_seq.last + st.delay;
				}
				if (a_now < st.readyAt) {
					return false;
				}
				if (!st.cond) {
					st.done(true);
				} else if (st.cond()) {
					st.done(true);
				} else if (a_now - st.readyAt >= st.timeout) {
					st.done(false);
				} else {
					return false;
				}
				a_seq.last = a_now;
				++a_seq.i;
			}
			return true;
		}

		// ---- moving the player (eased SetPosition at 60 Hz) ---------------------------------------------------------------
		struct RigMove
		{
			bool         active{ false };
			RE::NiPoint3 from{};
			RE::NiPoint3 to{};
			double       start{ 0.0 };
			double       seconds{ 1.0 };
			double       next{ 0.0 };
			double       holdUntil{ 0.0 };
		};
		RigMove s_mv;

		void WritePos(const RE::NiPoint3& a_target)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}
			unsigned long code{ 0 };
			if (!Guarded([&] { player->SetPosition(a_target, true); }, code)) {
				REX::ERROR("P3: exception 0x{:08X} in SetPosition", code);
			}
		}

		void MoveBegin(const RE::NiPoint3& a_to, const double a_seconds)
		{
			s_mv = {};
			s_mv.active = true;
			s_mv.from = Snap().position;
			s_mv.to = a_to;
			s_mv.start = Now();
			s_mv.seconds = (std::max)(a_seconds, 0.1);
			s_mv.next = s_mv.start;
		}

		bool MoveStep()
		{
			if (!s_mv.active) {
				return true;
			}
			const double now = Now();
			if (now < s_mv.next) {
				return false;
			}
			s_mv.next = now + 1.0 / 60.0;
			const double t = now - s_mv.start;
			const float  c = std::clamp(static_cast<float>(t / s_mv.seconds), 0.0f, 1.0f);
			const float  u = c * c * (3.0f - 2.0f * c);
			const RE::NiPoint3 target{ s_mv.from.x + (s_mv.to.x - s_mv.from.x) * u, s_mv.from.y + (s_mv.to.y - s_mv.from.y) * u, s_mv.from.z + (s_mv.to.z - s_mv.from.z) * u };
			WritePos(target);
			if (t >= s_mv.seconds) {
				if (s_mv.holdUntil == 0.0) {
					s_mv.holdUntil = now + 0.2;
				} else if (now >= s_mv.holdUntil) {
					s_mv.active = false;
					return true;
				}
			}
			return false;
		}

		// ---- a cast straight down for the floor under a spot ---------------------------------------------------------------
		struct Floor
		{
			WorldProbe::Ticket ticket{ 0 };
			bool               have{ false };
			float              z{ 0.0f };
		};
		Floor s_floor;

		void FloorBegin(const float a_x, const float a_y, const float a_zRef)
		{
			s_floor = {};
			s_floor.ticket = WorldProbe::Submit({ a_x, a_y, a_zRef + 1.0f }, { a_x, a_y, a_zRef - 3.0f }, 0.1f);
		}

		bool FloorPoll()
		{
			if (s_floor.ticket == 0) {
				return true;
			}
			const auto r = WorldProbe::Poll(s_floor.ticket);
			if (!r) {
				return false;
			}
			s_floor.ticket = 0;
			s_floor.have = r->ok && r->hit;
			s_floor.z = r->point.z - 0.1f;
			return true;
		}

		// ---- turning the view to a heading with the mouse (150 counts turned 51 degrees in run 33) -------------------------------
		struct Align
		{
			int    tries{ 0 };
			double next{ 0.0 };
			float  sign{ 1.0f };
			float  lastErr{ 0.0f };
			bool   haveLast{ false };
		};
		Align s_align;

		bool AlignStep(const float a_headingDeg)
		{
			const double now = Now();
			if (now < s_align.next) {
				return false;
			}
			float yaw{ 0.0f };
			if (!Camera::ReadViewYaw(yaw)) {
				return false;
			}
			const float err = WrapDeg(a_headingDeg - yaw * kRad2Deg);
			if (std::fabs(err) < 3.0f) {
				return true;
			}
			if (s_align.haveLast && std::fabs(err) > std::fabs(s_align.lastErr) + 1.0f) {
				s_align.sign = -s_align.sign;  // the last move made it worse: the mouse sign is the other way
			}
			s_align.lastErr = err;
			s_align.haveLast = true;
			if (++s_align.tries > 12) {
				REX::WARN("P3: the view could not be turned to heading {:.0f} (still {:.1f} degrees off); going on", a_headingDeg, err);
				return true;
			}
			const int dx = static_cast<int>(std::clamp(s_align.sign * err / 0.34f, -400.0f, 400.0f));
			Rig::MouseMove(dx, 0);
			s_align.next = now + 0.35;
			return false;
		}

		// ---- the sensor, for the stages that ask it directly -------------------------------------------------------------------
		CoverSense::ProbeAdapter         s_adapter;
		std::optional<Core::CoverSensor> s_sensor;

		Core::CoverSensor& Sensor()
		{
			if (!s_sensor) {
				s_sensor.emplace(s_adapter, CoverSense::Settings());
			}
			return *s_sensor;
		}

		constexpr int                  kScanDirs = 7;
		constexpr std::array<float, 3> kScanHeights{ 0.3f, 0.9f, 1.5f };
		WorldProbe::Batch              s_scan;
		float                          s_scanHeading{ 0.0f };

		void ScanBegin(const RE::NiPoint3& a_pos, const float a_headingRad)
		{
			s_scan.Clear();
			s_scanHeading = a_headingRad;
			for (int k = 0; k < kScanDirs; ++k) {
				const float a = a_headingRad + static_cast<float>(k - 3) * 30.0f * kPi / 180.0f;
				for (const float h : kScanHeights) {
					const RE::NiPoint3 from{ a_pos.x, a_pos.y, a_pos.z + h };
					s_scan.Add(from, { from.x + std::sin(a) * 6.0f, from.y + std::cos(a) * 6.0f, from.z }, 0.1f);
				}
			}
		}

		std::string ScanText()
		{
			std::string cells;
			for (int k = 0; k < kScanDirs; ++k) {
				cells += std::format(" {:+d}:", (k - 3) * 30);
				for (std::size_t h = 0; h < kScanHeights.size(); ++h) {
					const auto& r = s_scan[static_cast<std::size_t>(k) * kScanHeights.size() + h];
					cells += !r.ok ? "?" : (!r.hit ? "." : std::format("{:.2f}", r.fraction * 6.0f + 0.1f));
					cells += h + 1 < kScanHeights.size() ? "/" : "";
				}
			}
			return std::format("scan (nearest surface in metres at 0.3/0.9/1.5 m, offsets from heading {:.0f}):{}", s_scanHeading * kRad2Deg, cells);
		}

		// ---- readiness --------------------------------------------------------------------------------------------------------
		bool         s_haveAnchor{ false };
		RE::NiPoint3 s_anchor{};
		double       s_stillSince{ 0.0 };
		RE::NiPoint3 s_spawn{};

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
					REX::INFO("S0 rig: p3: the camera is not third person (index {}); asking for third person", snap.cameraIndex);
					(void)Camera::ForceThirdPerson();
				}
				return false;
			}
			if (!WorldProbe::Available() || !Cover::GetInfo().enabled) {
				return false;
			}
			s_spawn = snap.position;
			REX::INFO("S0 rig: script 'p3' set {} starting at ({:.2f},{:.2f},{:.2f}), third person, game window in front, cast queue available, cover feature on", Config::Get().p3Set, snap.position.x, snap.position.y, snap.position.z);
			return true;
		}

		// ======================================== set 1: Save29, the wall east of the spawn ======================================================
		// The spawn is 1.23 m from the wall's surface; the test position is 2.0 m from it, west of the spawn (the open plaza). S (back) points
		// at the wall because the player faces west.
		RE::NiPoint3 Base()
		{
			return { s_spawn.x - 0.8f, s_spawn.y, s_spawn.z };
		}

		struct DirCase
		{
			RE::NiPoint3     a{}, b{};
			Cover::InputDir  in{};
			double           ta{ 0 }, tb{ 0 };
		};

		void AddDirection(Seq& s, const char* a_name, const Rig::Key a_key)
		{
			auto d = std::make_shared<DirCase>();
			s.Do([] {
				Rig::ReleaseEverything("p3 direction");
				Cover::ForceReset("p3 direction check");
				MoveBegin(Base(), 1.0);
			});
			s.Until(MoveStep, 3.0);
			s.Wait(1.0);
			s.Do([a_key] { Rig::Tap(a_key, 0.5); });
			s.Do([d] { d->a = Snap().position; d->ta = Now(); }, 0.15);
			s.Do([d] { d->in = Cover::GetInputDir(); }, 0.10);
			s.Do([d] { d->b = Snap().position; d->tb = Now(); }, 0.20);
			s.Wait(0.6);
			s.Do([d, a_name] {
				const float moved = Dist2(d->a, d->b);
				const float moveHeading = HeadingOf(d->b.x - d->a.x, d->b.y - d->a.y);
				const float inHeading = HeadingOf(d->in.dir.x, d->in.dir.y);
				const float diff = std::fabs(WrapDeg(moveHeading - inHeading));
				const bool  ok = d->in.valid && d->in.magnitude > 0.5f && moved > 0.15f && diff <= 10.0f;
				Result(std::format("P3 direction {}: the input was read as magnitude {:.2f} toward heading {:.1f} deg (camera yaw {:.1f} deg); the player moved {:.2f} m in {:.2f} s toward heading {:.1f} deg; difference {:.1f} deg (limit 10) -> {}", a_name,
					d->in.magnitude, inHeading, d->in.yaw * kRad2Deg, moved, d->tb - d->ta, moveHeading, diff, Verdict(ok)));
				Summarise(std::format("direction {}", a_name), ok);
			});
		}

		bool StageDirection(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				AddDirection(s, "W (forward)", Rig::kKeyW);
				AddDirection(s, "A (left)", Rig::kKeyA);
				AddDirection(s, "S (back, toward the wall)", Rig::kKeyS);
			}
			return Run(s, c.now);
		}

		// One enter-high attempt (R1): at the test position, S held, a Sneak tap once the background sensor has an answer.
		struct CaseData
		{
			std::size_t  t0{ 0 };
			std::uint32_t presses0{ 0 }, swallowed0{ 0 };
			bool         tapped{ false };
			double       tapT{ 0.0 };
			double       tIn{ -1.0 };
			float        err{ -1.0f };
			int          stanceBefore{ -1 };
			int          stanceInCover{ -1 };
			Core::CoverKind kind{ Core::CoverKind::None };
			RE::NiPoint3 standPoint{};
			// leaving
			double       leaveT{ 0.0 };
			double       tNone{ -1.0 };
			int          stanceAfter{ -1 };
			float        freeDist{ -1.0f };
			float        zRise{ 0.0f };
			bool         factsOk{ false };
			// batch 4: the game vector after the leave (W held) and the along-wall displacement 0.5 s after InCover
			bool         freeOk{ false };
			float        vx{ 0.0f }, vy{ 0.0f }, wx{ 0.0f }, wy{ 0.0f };
			std::uint64_t writes0{ 0 }, writesDelta{ 0 };
			bool         have05{ false };
			float        along05{ 0.0f }, across05{ 0.0f };
		};

		bool         s_zTrack{ false };
		float        s_zMax{ 0.0f };
		float        s_zStart{ 0.0f };

		bool FreshCandidate(const Core::CoverKind a_want)
		{
			const auto i = Cover::GetInfo();
			return i.haveCached && i.cachedKind != Core::CoverKind::None && (a_want == Core::CoverKind::None || i.cachedKind == a_want) && i.cachedAge < 0.25;
		}

		// at the test position with the stance a_stance, state None
		void AddPrepare(Seq& s, const int a_stance, const RE::NiPoint3 a_pos, const std::optional<float> a_heading)
		{
			s.Do([] {
				Rig::ReleaseEverything("p3 prepare");
				Cover::ForceReset("p3 prepare");
			});
			s.Do([a_stance] {
				if (Stance() != a_stance) {
					Rig::Tap(Rig::kKeyLCtrl, 0.08);  // nothing is pushed and no cover is taken: the vanilla toggle
				}
			}, 0.2);
			s.Until([a_stance] { return Stance() == a_stance; }, 1.5);
			s.Wait(0.3);
			s.Do([a_pos] { MoveBegin(a_pos, 1.0); });
			s.Until(MoveStep, 3.0);
			if (a_heading) {
				s.Do([] { s_align = Align{}; });
				s.Until([h = *a_heading] { return AlignStep(h); }, 6.0);
			}
			s.Wait(0.8);
		}

		void AddEnter(Seq& s, const std::shared_ptr<CaseData>& d, const Rig::Key a_key, const Core::CoverKind a_want)
		{
			s.Do([d, a_key] {
				const auto i = Cover::GetInfo();
				d->t0 = Cover::TransitionCount();
				d->presses0 = i.presses;
				d->swallowed0 = i.swallowed;
				d->stanceBefore = Stance();
				Rig::Tap(a_key, 1.7);
			});
			s.Until([a_want] { return FreshCandidate(a_want); }, 1.5, [d](bool ok) {
				if (ok) {
					d->tapped = true;
					d->tapT = Now();
					Rig::Tap(Rig::kKeyLCtrl, 0.08);
				} else {
					Result("P3 enter: the background sensor never produced a candidate (no Sneak tap sent)");
				}
			});
			s.Until([] { return Cover::GetInfo().state == Core::CoverState::InCover; }, 1.0, [d](bool ok) {
				if (ok) {
					d->tIn = Now();
				}
			});
			s.Until([a_want] { return Stance() == (a_want == Core::CoverKind::Low ? 1 : 0); }, 1.5);  // M1: the stance is made by a press of the plugin
			s.Wait(0.2);
			s.Do([d] {
				const auto i = Cover::GetInfo();
				d->kind = i.candidate.kind;
				d->standPoint = { i.candidate.standPoint.x, i.candidate.standPoint.y, i.candidate.standPoint.z };
				d->err = Dist2(Snap().position, d->standPoint);
				d->stanceInCover = Stance();
			});
			s.Wait(0.2);
			s.Do([d] {
				const auto  i = Cover::GetInfo();
				const auto& p = Snap().position;
				d->along05 = (p.x - d->standPoint.x) * i.candidate.tangent.x + (p.y - d->standPoint.y) * i.candidate.tangent.y;
				d->across05 = (p.x - d->standPoint.x) * i.candidate.normal.x + (p.y - d->standPoint.y) * i.candidate.normal.y;
				d->have05 = true;
			});
		}

		void LogEnter(const char* a_label, const std::shared_ptr<CaseData>& d, const int a_expectedStance, const bool a_expectSwallow)
		{
			const auto  i = Cover::GetInfo();
			const bool  swallowed = i.swallowed > d->swallowed0;
			(void)a_expectSwallow;  // M1: an Enter is always swallowed, the stance is made by a press of the plugin
			const double dt = d->tIn - d->tapT;
			const float  standLimit = Camera::Compat() ? 0.08f : 0.05f;  // with Stride loaded the hold tolerance (0.08 m) is the limit
			const bool  ok = d->tapped && d->tIn > 0.0 && dt <= 0.5 && d->err <= standLimit && d->stanceInCover == a_expectedStance && swallowed;
			Result(std::format("P3 {}: {} cover, the Sneak tap at the first fresh answer; InCover {:.2f} s after the tap (limit 0.5); the player {:.3f} m from the stand point ({:.2f},{:.2f}) (limit {:.2f}); stance before {}, 0.3 s after InCover {} (expected {}); the press was {} (expected {}); 0.5 s after InCover the player is {:+.3f} m along and {:+.3f} m across the wall from the stand point -> {}",
				a_label, d->kind == Core::CoverKind::Low ? "LOW" : (d->kind == Core::CoverKind::High ? "HIGH" : "no"), d->tIn > 0.0 ? dt : -1.0, d->err, d->standPoint.x, d->standPoint.y, standLimit, d->stanceBefore, d->stanceInCover, a_expectedStance, swallowed ? "swallowed" : "passed to the game",
				"swallowed", d->along05, d->across05, Verdict(ok)));
			Summarise(a_label, ok);
		}

		// leaving by tapping Sneak again; afterwards the player must move freely
		void AddLeaveSneak(Seq& s, const std::shared_ptr<CaseData>& d)
		{
			s.Do([d] { d->leaveT = Now(); Rig::Tap(Rig::kKeyLCtrl, 0.08); });
			s.Until([] { return Cover::GetInfo().state == Core::CoverState::None; }, 1.0, [d](bool ok) { d->tNone = ok ? Now() : -1.0; });
			s.Wait(0.5);
			s.Do([d] { d->stanceAfter = Stance(); Rig::ReleaseEverything("p3 leave"); });
		}

		void AddFreeMove(Seq& s, const std::shared_ptr<CaseData>& d)
		{
			s.Wait(0.3);
			s.Do([d] {
				d->freeDist = -1.0f;
				d->writes0 = Input::LastMovementVector().writes;
				d->standPoint = Snap().position;
				Rig::Tap(Rig::kKeyW, 0.9);
			});
			// from the moment the hook sees the forward movement: the game vector must read (0, +1) after the original and the hook must have written nothing
			s.Until([] { return Input::LastMovementVector().y > 0.5f; }, 1.5, [d](bool) { d->standPoint = Snap().position; });
			s.Wait(0.3);
			s.Do([d] {
				const auto v = Input::LastMovementVector();
				d->vx = v.x;
				d->vy = v.y;
				d->wx = v.wroteX;
				d->wy = v.wroteY;
				d->writesDelta = v.writes - d->writes0;
				d->freeOk = std::fabs(v.x) < 0.01f && std::fabs(v.y - 1.0f) < 0.01f && d->writesDelta == 0 && v.wroteX == v.x && v.wroteY == v.y;
			});
			s.Wait(0.5);
			s.Do([d] { d->freeDist = Dist2(Snap().position, d->standPoint); });
		}

		void LogLeave(const char* a_label, const std::shared_ptr<CaseData>& d, const int a_expectedStance, const bool a_checkStance, const char* a_extra, const bool a_extraOk)
		{
			const double dt = d->tNone - d->leaveT;
			const bool   ok = d->tNone > 0.0 && dt <= 0.5 && d->freeOk && (!a_checkStance || d->stanceAfter == a_expectedStance) && a_extraOk;
			Result(std::format("P3 {}: state None {:.2f} s after the action (limit 0.5); stance 0.5 s later {} (expected {}); after it, with W held, the game vector read ({:+.3f},{:+.3f}) after the original (expected (0,+1)), the vector left in the manager ({:+.3f},{:+.3f}) and the hook wrote it {} times (expected 0); the player moved {:.2f} m in 0.8 s (information only){} -> {}", a_label, d->tNone > 0.0 ? dt : -1.0,
				d->stanceAfter, a_checkStance ? std::to_string(a_expectedStance) : std::string("not checked"), d->vx, d->vy, d->wx, d->wy, d->writesDelta, d->freeDist, a_extra, Verdict(ok)));
			Summarise(a_label, ok);
		}

		bool StageEnterHighSneak(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto d = std::make_shared<CaseData>();
				AddPrepare(s, 0, Base(), std::nullopt);
				AddEnter(s, d, Rig::kKeyS, Core::CoverKind::High);
				s.Do([d] { LogEnter("R1 enter high (S held, Sneak tapped)", d, 0, true); });
				AddLeaveSneak(s, d);
				AddFreeMove(s, d);
				s.Do([d] { LogLeave("R3 leave by Sneak", d, d->stanceBefore, true, "", true); });
			}
			return Run(s, c.now);
		}

		bool StageVanillaToggle(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto d = std::make_shared<CaseData>();
				AddPrepare(s, 0, Base(), std::nullopt);
				s.Do([d] {
					const auto i = Cover::GetInfo();
					d->presses0 = i.presses;
					d->stanceBefore = Stance();
					d->tapT = Now();
					Rig::Tap(Rig::kKeyLCtrl, 0.08);
				});
				s.Until([] { return Stance() == 1; }, 1.0, [d](bool ok) { d->tIn = ok ? Now() : -1.0; });
				s.Wait(0.4);
				s.Do([d] {
					const auto i = Cover::GetInfo();
					d->stanceInCover = Stance();
					d->kind = i.candidate.kind;
					const bool ok = d->tIn > 0.0 && d->stanceInCover == 1 && i.state == Core::CoverState::None && i.presses == d->presses0 + 1;
					Result(std::format("P3 R1 no key held: the Sneak tap toggled the stance {} -> {} after {:.2f} s, state {}, the press was decided once ({} -> {}) -> {}", d->stanceBefore, d->stanceInCover, d->tIn > 0.0 ? d->tIn - d->tapT : -1.0, Core::ToString(i.state), d->presses0, i.presses,
						Verdict(ok)));
					Summarise("R1 no key held: vanilla toggle", ok);
					Rig::Tap(Rig::kKeyLCtrl, 0.08);  // back to standing
				});
				s.Until([] { return Stance() == 0; }, 1.0);
				s.Wait(0.4);
			}
			return Run(s, c.now);
		}

		bool StageEnterHighJump(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto d = std::make_shared<CaseData>();
				AddPrepare(s, 0, Base(), std::nullopt);
				AddEnter(s, d, Rig::kKeyS, Core::CoverKind::High);
				s.Do([d] { LogEnter("R3 setup enter high", d, 0, true); });
				s.Do([d] {
					d->leaveT = Now();
					s_zStart = Snap().position.z;
					s_zMax = s_zStart;
					s_zTrack = true;
					Rig::ReleaseEverything("p3 jump");
					Rig::Tap(kKeySpace, 0.1);
				});
				s.Until([] { return Cover::GetInfo().state == Core::CoverState::None; }, 1.0, [d](bool ok) { d->tNone = ok ? Now() : -1.0; });
				s.Wait(1.3);
				s.Do([d] {
					s_zTrack = false;
					d->zRise = s_zMax - s_zStart;
					d->stanceAfter = Stance();
				});
				AddFreeMove(s, d);
				s.Do([d] { LogLeave("R3 leave by Jump", d, 0, false, std::format("; the player rose {:.2f} m (limit 0.3)", d->zRise).c_str(), d->zRise > 0.3f); });
			}
			return Run(s, c.now);
		}

		bool StageEnterHighSprint(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto d = std::make_shared<CaseData>();
				AddPrepare(s, 0, Base(), std::nullopt);
				AddEnter(s, d, Rig::kKeyS, Core::CoverKind::High);
				s.Do([d] { LogEnter("R3 setup enter high", d, 0, true); });
				s.Do([d] {
					d->leaveT = Now();
					Rig::ReleaseEverything("p3 sprint");
					Rig::Tap(Rig::kKeyShift, 0.1);
				});
				s.Until([] { return Cover::GetInfo().state == Core::CoverState::None; }, 1.0, [d](bool ok) { d->tNone = ok ? Now() : -1.0; });
				s.Wait(0.5);
				s.Do([d] { d->stanceAfter = Stance(); });
				AddFreeMove(s, d);
				s.Do([d] { LogLeave("R3 leave by Sprint", d, 0, false, "", true); });
			}
			return Run(s, c.now);
		}

		bool StageEnterHighFirstPerson(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto d = std::make_shared<CaseData>();
				AddPrepare(s, 0, Base(), std::nullopt);
				AddEnter(s, d, Rig::kKeyS, Core::CoverKind::High);
				s.Do([d] { LogEnter("R3 setup enter high", d, 0, true); });
				s.Do([d] {
					d->leaveT = Now();
					Rig::ReleaseEverything("p3 first person");
					(void)Camera::ForceFirstPerson();
				});
				s.Until([] { return Cover::GetInfo().state == Core::CoverState::None; }, 1.0, [d](bool ok) { d->tNone = ok ? Now() : -1.0; });
				s.Wait(0.5);
				s.Do([d] {
					d->stanceAfter = Stance();
					(void)Camera::ForceThirdPerson();
				});
				s.Until([] { return Snap().cameraIndex == RE::CameraStates::kThirdPerson; }, 4.0);
				s.Wait(1.5);
				AddFreeMove(s, d);
				s.Do([d] { LogLeave("R3 leave by forcing first person", d, 0, false, "", true); });
			}
			return Run(s, c.now);
		}

		// T3.4 spike evidence: several button events in one queue (two movement keys and the aim button held together); Engine/Input logs their addresses
		bool StageQueueLayout(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				s.Do([] {
					Rig::ReleaseEverything("p3 queue layout");
					Cover::ForceReset("p3 queue layout");
					Rig::Tap(Rig::kKeyW, 1.4);
					Rig::Tap(Rig::kKeyD, 1.4);
					Rig::MouseRight(true);
				});
				s.Wait(1.5);
				s.Do([] {
					Rig::MouseRight(false);
					Rig::ReleaseEverything("p3 queue layout done");
				});
				s.Wait(0.5);
			}
			return Run(s, c.now);
		}

		// R2: staying on cover. Enter high (S held, Sneak tapped), then A and D in turn along the wall, then W (away from it).
		struct R2Data
		{
			std::shared_ptr<CaseData> enter;
			RE::NiPoint3 start{};
			Core::Vec2   tangent0{};
			float        maxPerp{ 0.0f };
			float        minPlus{ 1e9f }, minMinus{ 1e9f };  // the smallest room left toward each end during a move (negative: past the stop point)
			double       windowEnd{ 0.0 };
			double       legStart{ 0.0 }, lastMove{ 0.0 }, lastT{ 0.0 };
			RE::NiPoint3 lastP{};
			float        topSpeed{ 0.0f };
			RE::NiPoint3 posAfterA{}, posAfterD{};
			float        remainA{ 0.0f }, remainD{ 0.0f };
			float        alongA{ 0.0f }, alongD{ 0.0f };  // alongA: the signed move of leg A (leg D must go the other way)
			Core::CoverState stateAfterA{ Core::CoverState::None }, stateAfterD{ Core::CoverState::None };
			std::uint32_t tracks0{ 0 }, lost0{ 0 };
			double       wPress{ 0.0 };
			double       wExit{ -1.0 };
			Core::ExitReason exitReason{ Core::ExitReason::None };
			std::size_t  t0{ 0 };
		};

		void AddR2Move(Seq& s, const std::shared_ptr<R2Data>& d, const Rig::Key a_key, const char* a_name, const double a_seconds, const bool a_first)
		{
			s.Do([d, a_key, a_seconds] {
				d->maxPerp = 0.0f;
				d->minPlus = 1e9f;
				d->minMinus = 1e9f;
				d->windowEnd = Now() + a_seconds + 0.5;
				d->start = Snap().position;
				d->legStart = Now();
				d->lastMove = Now();
				d->lastT = Now();
				d->lastP = Snap().position;
				d->topSpeed = 0.0f;
				Rig::Tap(a_key, a_seconds);
			});
			s.Until([d] {
				const auto i = Cover::GetInfo();
				d->maxPerp = (std::max)(d->maxPerp, std::fabs(i.perp));
				d->minPlus = (std::min)(d->minPlus, i.remainPlus);
				d->minMinus = (std::min)(d->minMinus, i.remainMinus);
				{
					const double now = Now();
					if (now - d->lastT >= 0.1) {
						const auto& p = Snap().position;
						const float v = static_cast<float>(std::hypot(p.x - d->lastP.x, p.y - d->lastP.y) / (now - d->lastT));
						d->topSpeed = (std::max)(d->topSpeed, v);
						if (v > 0.15f) {
							d->lastMove = now;
						}
						d->lastP = p;
						d->lastT = now;
					}
				}
				return Now() >= d->windowEnd || i.state != Core::CoverState::InCover;
			}, 8.0);
			s.Do([d, a_name, a_first] {
				const auto  i = Cover::GetInfo();
				const auto& p = Snap().position;
				const float along = (p.x - d->start.x) * d->tangent0.x + (p.y - d->start.y) * d->tangent0.y;
				const float remain = along >= 0.0f ? i.remainPlus : i.remainMinus;
				const float minToward = along >= 0.0f ? d->minPlus : d->minMinus;
				const bool  ok = i.state == Core::CoverState::InCover && d->maxPerp <= 0.10f && remain >= -0.10f && remain <= 0.40f && minToward >= -0.10f && std::fabs(along) > 0.3f && (a_first || along * d->alongA < 0.0f);
				Result(std::format("P3 R2 {} along the wall with the key held throughout: state {}, moved {:+.2f} m along the first tangent to ({:.2f},{:.2f}); the largest distance off the cover line {:.3f} m (limit 0.10); the player rests {:.3f} m inside the stop point (limit: between -0.10 and +0.40 of it) and the smallest room left was {:+.3f} m (limit -0.10); the leg took {:.2f} s to the last movement above 0.15 m/s, the top speed seen {:.2f} m/s, rest position ({:.2f},{:.2f}), stance {}; tracks {} (lost {}) -> {}",
					a_name, Core::ToString(i.state), along, p.x, p.y, d->maxPerp, remain, minToward, d->lastMove - d->legStart, d->topSpeed, p.x, p.y, Stance(), i.tracks - d->tracks0, i.trackLost - d->lost0, Verdict(ok)));
				Summarise(std::string("R2 ") + a_name, ok);
				if (a_first) {
					d->alongA = along;
				}
			});
			s.Wait(0.3);
		}

		void AddLowPrepare(Seq& s, const int a_stance);  // defined with the low-wall stages below

		bool StayStage(const Ctx& c, const bool a_low)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto d = std::make_shared<R2Data>();
				d->enter = std::make_shared<CaseData>();
				if (a_low) {
					AddLowPrepare(s, 0);
					AddEnter(s, d->enter, Rig::kKeyW, Core::CoverKind::Low);
				} else {
					AddPrepare(s, 0, Base(), std::nullopt);
					AddEnter(s, d->enter, Rig::kKeyS, Core::CoverKind::High);
				}
				s.Do([d, a_low] {
					LogEnter(a_low ? "R2 setup enter low" : "R2 setup enter high", d->enter, a_low ? 1 : 0, !a_low ? true : false);
					Rig::ReleaseEverything("p3 stay");
					const auto i = Cover::GetInfo();
					d->tangent0 = { i.candidate.tangent.x, i.candidate.tangent.y };
					d->tracks0 = i.tracks;
					d->lost0 = i.trackLost;
					Result(std::format("P3 R2 start: normal ({:+.2f},{:+.2f}), tangent ({:+.2f},{:+.2f}), edges left {:.2f} m ({}) right {:.2f} m ({}), room {:+.2f} / {:+.2f} m", i.candidate.normal.x, i.candidate.normal.y, i.candidate.tangent.x, i.candidate.tangent.y,
						i.candidate.edgeLeft, Core::ToString(i.candidate.edgeLeftKind), i.candidate.edgeRight, Core::ToString(i.candidate.edgeRightKind), i.remainPlus, i.remainMinus));
				});
				s.Wait(0.4);
				AddR2Move(s, d, Rig::kKeyA, "A (left)", 4.2, true);  // the walk speed of the capped input is about 1 m/s: 3.2 s reach an end 2 m away (run 11 held 1.6 s and stopped in the middle)
				AddR2Move(s, d, Rig::kKeyD, "D (right, to the other end)", 5.0, false);
				// away from the wall: W on the high wall of Save29 (the player faces west, the wall is east), S on the low wall of Save30 (the player faces the wall)
				s.Do([d, a_low] {
					d->t0 = Cover::TransitionCount();
					d->wPress = Now();
					Rig::Tap(a_low ? Rig::kKeyS : Rig::kKeyW, 1.2);
				});
				s.Until([] { return Cover::GetInfo().state != Core::CoverState::InCover; }, 2.0, [d](bool ok) {
					const auto i = Cover::GetInfo();
					d->exitReason = i.lastExit;
					if (ok) {
						for (const auto& t : Cover::Transitions(d->t0)) {
							if (t.to == Core::CoverState::Exiting) {
								d->wExit = t.time;
								break;
							}
						}
					}
				});
				s.Wait(0.4);
				s.Do([d] {
					Rig::ReleaseEverything("p3 stay done");
					const double dt = d->wExit - d->wPress;
					const bool   ok = d->wExit > 0.0 && dt >= 0.2 && dt <= 0.6 && d->exitReason == Core::ExitReason::HoldAway;
					Result(std::format("P3 R2 (away from the wall) held: the cover was left {:.2f} s after the press (fExitHoldTime 0.25, expected 0.2 to 0.6), reason {} (expected the input held away from the wall) -> {}", d->wExit > 0.0 ? dt : -1.0, Core::ToString(d->exitReason), Verdict(ok)));
					Summarise("R2 W (away) leaves after the hold time", ok);
				});
				s.Wait(1.0);
			}
			return Run(s, c.now);
		}

		bool StageStay(const Ctx& c) { return StayStage(c, false); }
		bool StageStayLow(const Ctx& c) { return StayStage(c, true); }

		// ---- T3.3b: probe and write test of the game movement vector ---------------------------------------------------------------------------------
		bool StageVectorProbe(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				s.Do([] {
					Rig::ReleaseEverything("p3 vector probe");
					Cover::ForceReset("p3 vector probe");
					Input::SetMovementProbeLog(true);
					MoveBegin(Base(), 1.0);
				});
				s.Until(MoveStep, 3.0);
				s.Wait(0.8);
				// nothing held
				s.Do([] {
					const auto v = Input::LastMovementVector();
					const bool ok = std::fabs(v.x) < 0.01f && std::fabs(v.y) < 0.01f && std::fabs(v.z) < 0.01f && std::fabs(v.prevX) < 0.01f && std::fabs(v.prevY) < 0.01f;
					Result(std::format("P3 T3.3b probe, nothing held: the game vector after the original returned x={:+.3f} y={:+.3f} z={:+.3f}, the copy at +0x7C/+0x80 = ({:+.3f},{:+.3f}) (expected zeros); {} calls seen -> {}", v.x, v.y, v.z, v.prevX, v.prevY, v.calls, Verdict(ok)));
					Summarise("T3.3b probe: nothing held", ok);
				});
				struct K { Rig::Key key; const char* name; float ex, ey; };
				for (const K k : { K{ Rig::kKeyW, "W", 0.0f, 1.0f }, K{ Rig::kKeyA, "A", -1.0f, 0.0f }, K{ Rig::kKeyS, "S", 0.0f, -1.0f }, K{ Rig::kKeyD, "D", 1.0f, 0.0f } }) {
					s.Do([k] { Rig::Tap(k.key, 0.7); });
					s.Wait(0.4);
					s.Do([k] {
						const auto v = Input::LastMovementVector();
						const bool ok = std::fabs(v.x - k.ex) < 0.01f && std::fabs(v.y - k.ey) < 0.01f && std::fabs(v.z) < 0.01f && std::fabs(v.prevX - k.ex) < 0.01f && std::fabs(v.prevY - k.ey) < 0.01f;
						Result(std::format("P3 T3.3b probe, {} held: x={:+.3f} y={:+.3f} z={:+.3f}, the copy at +0x7C/+0x80 = ({:+.3f},{:+.3f}) (expected ({:+.1f},{:+.1f})) -> {}", k.name, v.x, v.y, v.z, v.prevX, v.prevY, k.ex, k.ey, Verdict(ok)));
						Summarise(std::string("T3.3b probe: ") + k.name + " held", ok);
					});
					s.Wait(0.6);
					s.Do([] { MoveBegin(Base(), 0.8); });
					s.Until(MoveStep, 3.0);
					s.Wait(0.5);
				}
				s.Do([] {
					const auto v = Input::LastMovementVector();
					Result(std::format("P3 T3.3b probe done: {} calls of the hook read the vector, nothing faulted: {}", v.calls, v.faulted ? "FAULT" : "no"));
					Input::SetMovementProbeLog(false);
				});
			}
			return Run(s, c.now);
		}

		// With W held in the open: write (0, 0) for 0.6 s, then (0, m) for 0.9 s, then (0, 0) for 1.2 s; per m the steady speed and the distance travelled after the step to zero.
		struct WriteData
		{
			struct Sample
			{
				double       t;
				RE::NiPoint3 p;
			};
			std::vector<Sample> samples;
			double              tStart{ 0.0 }, tStep{ 0.0 };
			RE::NiPoint3        pStart{}, pStep{};
		};

		// the stance by the plugin's own made-up Sneak press (checked with iSyncActorStance); the aim held keeps events flowing for the hook
		void AddStance(Seq& s, const int a_want)
		{
			s.Do([] {
				Rig::ReleaseEverything("p3 stance");
				Rig::Tap(Rig::kKeyShift, 0.05);  // a real button event passes the hook: the template of the made-up one
				Rig::MouseRight(true);
			});
			s.Wait(0.5);
			s.Do([a_want] {
				if (Stance() != a_want) {
					Input::RequestSneakPress();
				}
			});
			s.Until([a_want] { return Stance() == a_want; }, 1.2);
			s.Do([a_want] {
				Rig::MouseRight(false);
				Input::ClearMadeUpRequests();
				Result(std::format("P3 T3.3b write test stance: wanted {}, iSyncActorStance reads {} -> {}", a_want, Stance(), Verdict(Stance() == a_want)));
			});
			s.Wait(0.8);
		}

		void AddWriteSegment(Seq& s, const float a_m, const bool a_crouched = false)
		{
			auto d = std::make_shared<WriteData>();
			s.Do([] {
				Rig::ReleaseEverything("p3 write test");
				MoveBegin(Base(), 1.0);
			});
			s.Until(MoveStep, 3.0);
			s.Wait(0.8);
			s.Do([d] {
				Rig::Tap(Rig::kKeyW, 4.0);
				Input::SetMovementOverride(true, 0.0f, 0.0f);
			});
			s.Wait(0.6);
			s.Do([d, a_m] {
				d->samples.clear();
				d->tStart = Now();
				d->pStart = Snap().position;
				Input::SetMovementOverride(true, 0.0f, a_m);
			});
			s.Until([d] {
				d->samples.push_back({ Now(), Snap().position });
				return Now() - d->tStart >= 0.9;
			}, 3.0);
			s.Do([d] {
				d->tStep = Now();
				d->pStep = Snap().position;
				Input::SetMovementOverride(true, 0.0f, 0.0f);
			});
			s.Until([d] {
				d->samples.push_back({ Now(), Snap().position });
				return Now() - d->tStep >= 1.2;
			}, 3.0);
			s.Do([d, a_m, a_crouched] {
				const int stanceNow = Stance();
				Input::SetMovementOverride(false, 0.0f, 0.0f);
				Rig::ReleaseEverything("p3 write test segment");
				// steady speed: the last 0.4 s of the 0.9 s run
				RE::NiPoint3 pa{};
				RE::NiPoint3 pb{};
				double       ta = 0.0;
				double       tb = 0.0;
				bool         haveA = false;
				for (const auto& smp : d->samples) {
					if (smp.t >= d->tStart + 0.5 && !haveA) {
						pa = smp.p;
						ta = smp.t;
						haveA = true;
					}
					if (smp.t <= d->tStep) {
						pb = smp.p;
						tb = smp.t;
					}
				}
				const float steady = tb > ta ? static_cast<float>(std::hypot(pb.x - pa.x, pb.y - pa.y) / (tb - ta)) : 0.0f;
				const auto& pEnd = d->samples.empty() ? d->pStep : d->samples.back().p;
				const float stopDist = std::hypot(pEnd.x - d->pStep.x, pEnd.y - d->pStep.y);
				// how long after the step the player is still faster than 0.3 m/s
				double tStop = 0.0;
				for (std::size_t i = 1; i < d->samples.size(); ++i) {
					const auto& a = d->samples[i - 1];
					const auto& b = d->samples[i];
					if (b.t > d->tStep && b.t - a.t > 1e-4) {
						const double v = std::hypot(b.p.x - a.p.x, b.p.y - a.p.y) / (b.t - a.t);
						(void)v;
					}
				}
				for (std::size_t i = d->samples.size(); i-- > 1;) {
					const auto& a = d->samples[i - 1];
					const auto& b = d->samples[i];
					if (b.t > d->tStep && b.t - a.t > 0.02) {
						const double v = std::hypot(b.p.x - a.p.x, b.p.y - a.p.y) / (b.t - a.t);
						if (v > 0.3) {
							tStop = b.t - d->tStep;
							break;
						}
					}
				}
				Result(std::format("P3 T3.3b write test {} m={:.2f}: the vector (0, {:.2f}) was written after the original for 0.9 s with W held: steady speed {:.2f} m/s (over the last 0.4 s); after the step to (0, 0) the player still travelled {:.2f} m in 1.2 s (about {:.2f} s above 0.3 m/s); stance read {}; {} samples",
					a_crouched ? "crouched" : "standing", a_m, a_m, steady, stopDist, tStop, stanceNow, d->samples.size()));
			});
			s.Wait(0.5);
		}

		bool StageWriteTest(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				for (const float m : { 0.5f, 0.55f, 0.6f, 1.0f }) {
					AddWriteSegment(s, m, false);
				}
				AddStance(s, 1);
				for (const float m : { 0.3f, 0.5f, 0.55f, 0.6f, 0.7f, 1.0f }) {
					AddWriteSegment(s, m, true);
				}
				AddStance(s, 0);
				s.Do([] {
					const auto v = Input::LastMovementVector();
					Result(std::format("P3 T3.3b write test done: {} calls, {} wrote a vector back, faulted {}", v.calls, v.writes, v.faulted ? "YES" : "no"));
				});
			}
			return Run(s, c.now);
		}

		// ======================================== set 4: T4.0, live with Stride ============================================================
		// Run with Stride (DyMC.dll) enabled in the profile: CoverSF must be in compatibility mode (no vtable hook, the cast queue pumped from the call-site hook), the
		// cast stress of T2.0 must leave the engine collision factor alone, R1 must pass, and a synthetic X key (Stride's shoulder swap) must move the camera to the
		// other shoulder and back.
		bool StageCompat(const Ctx&)
		{
			const auto st = Camera::GetStats();
			const bool ok = st.compat && st.siteInstalled && st.installed && WorldProbe::Available();
			Result(std::format("P3 T4.0 compatibility mode: compat {}, call-site hook installed {}, camera pump installed {}, cast queue available {}, call-site calls so far {} -> {}", st.compat, st.siteInstalled, st.installed, WorldProbe::Available(), st.siteCalls, Verdict(ok)));
			Summarise("T4.0 compatibility mode", ok);
			return true;
		}

		struct StressData
		{
			float                 lo{ 1e30f }, hi{ -1e30f };
			float                 blo{ 1e30f }, bhi{ -1e30f };
			std::uint64_t         sub{ 0 }, ans{ 0 }, ok{ 0 }, hit{ 0 };
			double                nextCast{ 0.0 }, tStart{ 0.0 };
			std::deque<WorldProbe::Ticket> out;
		};

		bool StageStress(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto d = std::make_shared<StressData>();
				s.Do([d] {
					Rig::ReleaseEverything("p3 stress");
					Cover::ForceReset("p3 stress");
					Rig::Tap(Rig::kKeyW, 1.5);  // a real key: the engine collision factor only recovers after real movement
					d->tStart = Now();
				});
				s.Wait(2.0);
				s.Do([d] { d->tStart = Now(); });
				s.Until([d] {
					const auto cs = Camera::ReadCollision();
					if (cs.ok) {
						d->blo = (std::min)(d->blo, cs.factor);
						d->bhi = (std::max)(d->bhi, cs.factor);
					}
					return Now() - d->tStart >= 4.0;
				}, 8.0);
				s.Do([d] { d->tStart = Now(); d->nextCast = Now(); });
				s.Until([d] {
					const double now = Now();
					const auto   cs = Camera::ReadCollision();
					if (cs.ok) {
						d->lo = (std::min)(d->lo, cs.factor);
						d->hi = (std::max)(d->hi, cs.factor);
					}
					if (now - d->tStart < 12.0) {
						int guard = 0;
						while (now >= d->nextCast && guard++ < 8) {
							const auto&        p = Snap().position;
							const RE::NiPoint3 chest{ p.x, p.y, p.z + 1.2f };
							const auto         k = d->sub % 10;
							RE::NiPoint3       to{};
							if (k < 8) {
								const float a = Snap().angle.z + static_cast<float>(k) * (kPi / 4.0f);
								to = { chest.x + std::sin(a) * 6.0f, chest.y + std::cos(a) * 6.0f, chest.z };
							} else if (k == 8) {
								to = { chest.x, chest.y, chest.z - 3.0f };
							} else {
								to = { chest.x + 2.0f, chest.y, chest.z - 2.5f };
							}
							const auto t = WorldProbe::Submit(chest, to, 0.1f);
							++d->sub;
							if (t == 0) {
								++d->ans;
							} else {
								d->out.push_back(t);
							}
							d->nextCast += 0.005;
						}
						if (now > d->nextCast + 0.1) {
							d->nextCast = now;
						}
					}
					while (!d->out.empty()) {
						const auto r = WorldProbe::Poll(d->out.front());
						if (!r) {
							break;
						}
						++d->ans;
						d->ok += r->ok ? 1 : 0;
						d->hit += (r->ok && r->hit) ? 1 : 0;
						d->out.pop_front();
					}
					return now - d->tStart >= 12.0 && d->out.empty();
				}, 20.0);
				s.Do([d] {
					const float dF = std::fabs(d->hi - d->bhi) + std::fabs(d->lo - d->blo);
					const bool  ok = dF < 0.0005f && d->ok > 0;
					Result(std::format("P3 T4.0 cast stress ({} casts, {} answered, {} ok, {} hit, 200 per second for 12 s, with Stride, after the walk): the engine collision factor {:.5f} to {:.5f} against {:.5f} to {:.5f} in the 4 s baseline (it started from {:.5f}{}; difference {:.6f}) -> {}", d->sub, d->ans, d->ok, d->hit, d->lo, d->hi, d->blo, d->bhi, d->bhi, d->bhi < 0.99f ? ", below 1.0: the camera is still pulled in, the test is weak" : ", 1.0: free", dF,
						ok ? "UNCHANGED (pass)" : "CHANGED or no casts (FAIL)"));
					Verdict(ok);
					Summarise("T4.0 cast stress unchanged", ok);
				});
			}
			return Run(s, c.now);
		}

		// the camera root's sideways distance from the player in the yaw frame (positive: to the right of the view)
		float CameraSideways(bool& a_ok)
		{
			const auto smp = Camera::SampleRoot();
			float      yaw{ 0.0f };
			a_ok = smp.ok && Camera::ReadViewYaw(yaw);
			if (!a_ok) {
				return 0.0f;
			}
			const float dx = smp.rootWorld.x - smp.player.x;
			const float dy = smp.rootWorld.y - smp.player.y;
			return dx * std::cos(yaw) - dy * std::sin(yaw);
		}

		bool StageShoulder(const Ctx& c)
		{
			static Seq s;
			static float v[3];
			static bool  okv[3];
			if (c.first) {
				s = Seq{};
				s.Do([] {
					Rig::ReleaseEverything("p3 shoulder");
					Cover::ForceReset("p3 shoulder");
					MoveBegin(Base(), 1.0);
				});
				s.Until(MoveStep, 3.0);
				s.Wait(1.5);
				s.Do([] { v[0] = CameraSideways(okv[0]); Rig::Tap(Rig::kKeyX, 0.08); });
				s.Wait(1.2);
				s.Do([] { v[1] = CameraSideways(okv[1]); Rig::Tap(Rig::kKeyX, 0.08); });
				s.Wait(1.2);
				s.Do([] {
					v[2] = CameraSideways(okv[2]);
					const bool ok = okv[0] && okv[1] && okv[2] && std::fabs(v[1] - v[0]) > 0.3f && std::fabs(v[2] - v[0]) < 0.15f;
					Result(std::format("P3 T4.0 shoulder swap (a synthetic X key, scan code 0x2D, twice): the camera root is {:+.3f} m to the right of the view before, {:+.3f} m after the first X, {:+.3f} m after the second (expected a change of more than 0.3 m and back to within 0.15 m){} -> {}",
						v[0], v[1], v[2], (v[0] > 0.0f) != (v[1] > 0.0f) ? "; the sign changed" : "; the sign did not change", Verdict(ok)));
					Summarise("T4.0 shoulder swap by X and back", ok);
				});
			}
			return Run(s, c.now);
		}

		// R6: in open ground with W held a Sneak tap is the vanilla toggle; in first person it changes no state either.
		bool StageOpenGround(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto d = std::make_shared<CaseData>();
				AddPrepare(s, 0, Base(), std::nullopt);
				s.Do([d] {
					d->presses0 = Cover::GetInfo().presses;
					d->stanceBefore = Stance();
					Rig::Tap(Rig::kKeyW, 1.2);  // forward = west, open plaza
				});
				s.Do([d] { d->tapT = Now(); Rig::Tap(Rig::kKeyLCtrl, 0.08); }, 0.7);
				s.Wait(0.5);
				s.Do([d] {
					const auto i = Cover::GetInfo();
					const bool ok = Stance() == 1 && i.state == Core::CoverState::None && i.presses == d->presses0 + 1;
					Result(std::format("P3 R6 open ground, W held plus Sneak: stance {} -> {}, state {} (expected None, a plain sneak toggle) -> {}", d->stanceBefore, Stance(), Core::ToString(i.state), Verdict(ok)));
					Summarise("R6 open ground: vanilla toggle", ok);
					Rig::ReleaseEverything("p3 open ground");
					Rig::Tap(Rig::kKeyLCtrl, 0.08);
				}, 0.3);
				s.Until([] { return Stance() == 0; }, 1.0);
				// first person
				AddPrepare(s, 0, Base(), std::nullopt);
				s.Do([d] {
					d->presses0 = Cover::GetInfo().entered;  // covers entered so far in the run
					(void)Camera::ForceFirstPerson();
				});
				s.Do([d] { Rig::Tap(Rig::kKeyS, 1.0); }, 1.0);
				s.Do([d] { Rig::Tap(Rig::kKeyLCtrl, 0.08); }, 0.4);
				s.Wait(0.6);
				s.Do([d] {
					const auto i = Cover::GetInfo();
					const bool ok = i.state == Core::CoverState::None && i.entered == d->presses0;
					Result(std::format("P3 R6 first person, S held plus Sneak: state {}, covers entered during this test {} (expected None, and none in first person) -> {}", Core::ToString(i.state), i.entered - d->presses0, Verdict(ok)));
					Summarise("R6 first person: no state change", ok);
					Rig::ReleaseEverything("p3 first person");
					if (Stance() == 1) {
						Rig::Tap(Rig::kKeyLCtrl, 0.08);
					}
					(void)Camera::ForceThirdPerson();
				}, 0.3);
				s.Until([] { return Snap().cameraIndex == RE::CameraStates::kThirdPerson; }, 4.0);
				s.Wait(1.0);
			}
			return Run(s, c.now);
		}

		// ======================================== set 2: Save30, the low wall along x = -615.3 ==============================================
		struct LowSpot
		{
			float x, y, heading;
			const char* name;
		};
		constexpr std::array<LowSpot, 2> kLowSpotsSave30{ { { -613.3f, 2116.0f, 270.0f, "east of the wall, looking west" }, { -617.3f, 2116.0f, 90.0f, "west of the wall, looking east" } } };
		// card M4b, sets 41 and 42: the low wall beside the "Enhance!" kiosk (stand point (-526.289, 2033.890), normal (+0.16, +0.99): the wall lies to the south)
		constexpr std::array<LowSpot, 1> kLowSpotsKiosk{ { { -525.9f, 2035.4f, 189.0f, "north of the kiosk's low wall, looking south" } } };
		constexpr std::array<LowSpot, 1> kLowSpotsRev{ { { -411.1f, 2143.66f, 90.0f, "west of the user's low wall of the straight tall wall's place, looking east" } } };
		// 23.32, set 83: the two low walls of the user's play at 19:47 (poses #25 and #26 of `real4`; stand points (972.780,735.390) and (974.203,732.613), wall normals (-0.42,+0.91) and (-0.97,+0.26)),
		// the start 1.2 m from the stand point along the normal, looking at the wall
		constexpr std::array<LowSpot, 2> kLowSpotsSave3{ { { 972.28f, 736.48f, 155.2f, "the user's low wall of pose #25, looking at it" }, { 973.04f, 732.93f, 105.1f, "the user's low wall of pose #26, looking at it" } } };
		[[nodiscard]] std::span<const LowSpot> LowSpots()
		{
			if (Config::Get().p3Set >= 83 && Config::Get().p3Set <= 85) {
				return kLowSpotsSave3;
			}
			if (Config::Get().p3Set == 62) {
				return kLowSpotsSave30;
			}
			if (Config::Get().p3Set == 47 || Config::Get().p3Set == 48 || Config::Get().p3Set == 52 || Config::Get().p3Set == 53 || Config::Get().p3Set == 54 || Config::Get().p3Set == 57 || (Config::Get().p3Set >= 58 && Config::Get().p3Set <= 60) || Config::Get().p3Set == 63 || Config::Get().p3Set == 64 || Config::Get().p3Set == 66 || Config::Get().p3Set == 68 || Config::Get().p3Set == 69 || Config::Get().p3Set == 70 || Config::Get().p3Set == 71 || Config::Get().p3Set == 72 || Config::Get().p3Set == 73 || (Config::Get().p3Set >= 74 && Config::Get().p3Set <= 82)) {
				return kLowSpotsRev;
			}
			if (Config::Get().p3Set >= 41) {
				return kLowSpotsKiosk;
			}
			return kLowSpotsSave30;
		}
		int          s_lowSpot{ -1 };
		RE::NiPoint3 s_lowPos{};

		// floor cast at the target, then an eased move at about a_speed m/s; skipped once the low wall has been found
		void AddGoTo(Seq& s, const float a_x, const float a_y, const float a_speed)
		{
			s.Do([a_x, a_y] {
				if (s_lowSpot < 0) {
					FloorBegin(a_x, a_y, Snap().position.z);
				}
			});
			s.Until(FloorPoll, 3.0);
			s.Do([a_x, a_y, a_speed] {
				if (s_lowSpot >= 0) {
					return;
				}
				if (!s_floor.have || std::fabs(s_floor.z - Snap().position.z) > 1.0f) {
					Result(std::format("P3 R4 search: no floor within 1 m of the player's height at ({:.1f},{:.1f}) (floor {}); not moving there", a_x, a_y, s_floor.have ? std::format("{:.2f}", s_floor.z) : "none"));
					return;
				}
				const auto& p = Snap().position;
				MoveBegin({ a_x, a_y, s_floor.z }, (std::max)(1.0, static_cast<double>(std::hypot(a_x - p.x, a_y - p.y) / a_speed)));
			});
			s.Until(MoveStep, 15.0);
		}

		bool StageLowSearch(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				s_lowSpot = -1;
				for (std::size_t k = 0; k < LowSpots().size(); ++k) {
					const auto spot = LowSpots()[k];
					s.Do([] {
						Rig::ReleaseEverything("p3 low search");
						Cover::ForceReset("p3 low search");
					});
					if (k == 1 && !(Config::Get().p3Set >= 83 && Config::Get().p3Set <= 85)) {
						AddGoTo(s, -615.3f, 2111.0f, 3.0f);  // round the south end of the wall, not through it
					}
					AddGoTo(s, spot.x, spot.y, 3.0f);
					s.Do([spot] {
						if (s_lowSpot < 0) {
							s_align = Align{};
						}
						(void)spot;
					});
					s.Until([spot] { return s_lowSpot >= 0 || AlignStep(spot.heading); }, 6.0);
					s.Wait(0.8);
					s.Do([spot] {
						if (s_lowSpot >= 0) {
							return;
						}
						const auto& p = Snap().position;
						const float h = spot.heading * kPi / 180.0f;
						Sensor().SetSettings(CoverSense::Settings());
						(void)Sensor().Find({ p.x, p.y, p.z }, { std::sin(h), std::cos(h) });
					});
					s.Until([] {
						auto& sensor = Sensor();
						sensor.Update();
						return !sensor.Busy();
					}, 5.0);
					s.Do([spot, k] {
						auto& sensor = Sensor();
						if (!sensor.HasResult()) {
							return;
						}
						const auto r = sensor.TakeResult();
						Result(std::format("P3 R4 search {} ({:.1f},{:.1f}) from ({:.2f},{:.2f},{:.2f}): {}", spot.name, spot.x, spot.y, Snap().position.x, Snap().position.y, Snap().position.z, CoverSense::Describe(r)));
						if (r.candidate.kind == Core::CoverKind::Low && s_lowSpot < 0) {
							s_lowSpot = static_cast<int>(k);
							s_lowPos = Snap().position;
						}
					});
				}
				s.Do([] {
					Result(s_lowSpot >= 0 ? std::format("P3 R4 search: the low wall was found from the spot {} ({})", s_lowSpot, LowSpots()[static_cast<std::size_t>(s_lowSpot)].name) : std::string("P3 R4 search: no LOW candidate from either spot: R4 is skipped"));
				});
			}
			return Run(s, c.now);
		}

		void AddLowPrepare(Seq& s, const int a_stance)
		{
			s.Do([a_stance] {
				if (s_lowSpot < 0) {
					return;
				}
				Rig::ReleaseEverything("p3 low prepare");
				Cover::ForceReset("p3 low prepare");
				(void)a_stance;
			});
			s.Do([a_stance] {
				if (s_lowSpot >= 0 && Stance() != a_stance) {
					Rig::Tap(Rig::kKeyLCtrl, 0.08);
				}
			}, 0.2);
			s.Until([a_stance] { return s_lowSpot < 0 || Stance() == a_stance; }, 1.5);
			s.Wait(0.3);
			s.Do([] {
				if (s_lowSpot >= 0) {
					const auto& sp = LowSpots()[static_cast<std::size_t>(s_lowSpot)];
					MoveBegin({ sp.x, sp.y, s_lowPos.z }, 1.0);
				}
			});
			s.Until(MoveStep, 3.0);
			s.Do([] { s_align = Align{}; });
			s.Until([] { return s_lowSpot < 0 || AlignStep(LowSpots()[static_cast<std::size_t>(s_lowSpot)].heading); }, 6.0);
			s.Wait(0.8);
		}

		// ---- T3.4 spike: the Sneak press made by the plugin, ten times (no cover, no input) ---------------------------------------------------
		struct MadeUpData
		{
			int                      toggled{ 0 };
			std::vector<double>      dts;
			int                      s0{ 0 };
			double                   t0{ 0.0 };
			std::uint32_t            presses0{ 0 };
			Input::MadeUpStats       stats0{};
		};

		bool StageMadeUp(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto d = std::make_shared<MadeUpData>();
				s.Do([d] {
					Rig::ReleaseEverything("p3 made-up press");
					Cover::ForceReset("p3 made-up press");
					d->presses0 = Cover::GetInfo().presses;
					d->stats0 = Input::MadeUpPressStats();
					Rig::Tap(Rig::kKeyShift, 0.05);  // a real button event passes the hook: the template of the made-up one
					Rig::MouseRight(true);           // the aim button held: a queue with an event in it every frame (an idle queue may not call the hook at all)
				});
				s.Wait(0.6);
				for (int k = 0; k < 10; ++k) {
					s.Do([d] {
						d->s0 = Stance();
						d->t0 = Now();
						Input::RequestSneakPress();
					});
					s.Until([d] { return Stance() != d->s0; }, 0.6, [d](bool ok) {
						d->dts.push_back(ok ? Now() - d->t0 : -1.0);
						if (ok) {
							++d->toggled;
						}
					});
					s.Wait(0.5);
				}
				s.Do([d] {
					Rig::MouseRight(false);
					std::string list;
					bool        within = true;
					for (const double t : d->dts) {
						list += t < 0.0 ? " none" : std::format(" {:.2f}", t);
						within = within && t >= 0.0 && t <= 0.3;
					}
					const auto st = Input::MadeUpPressStats();
					const auto i = Cover::GetInfo();
					const bool ok = d->toggled == 10 && within && st.delivered - d->stats0.delivered == 10 && i.presses == d->presses0;
					Result(std::format("P3 T3.4 spike, a Sneak press made by the plugin (a copy of a real button event, 96 bytes, linked at the tail of the list for one call, the release in the next): the stance toggled {} of 10 times; seconds from the request to the change:{} (limit 0.3); requested {}, delivered {}, dropped for want of a template {}, expired {}; hook calls with an empty queue so far {}; presses the cover logic saw: {} (expected 0); final stance {} -> {}",
						d->toggled, list, st.requested - d->stats0.requested, st.delivered - d->stats0.delivered, st.noTemplate - d->stats0.noTemplate, st.expired - d->stats0.expired, st.nullHeadCalls, i.presses - d->presses0, Stance(), Verdict(ok)));
					Summarise("T3.4 spike: made-up Sneak press 10 of 10", ok);
					Input::ClearMadeUpRequests();
				});
			}
			return Run(s, c.now);
		}

		// ---- R5: peeks (Save30, the low wall) -------------------------------------------------------------------------------------------------
		struct R5Data
		{
			std::shared_ptr<CaseData> enter;
			double       tAim{ 0.0 }, tPeek{ -1.0 }, tStand{ -1.0 }, tRel{ 0.0 }, tCrouch{ -1.0 }, tIn{ -1.0 };
			int          stanceAim{ -1 };
			Core::PeekKind kindOver{ Core::PeekKind::None };
			RE::NiPoint3 pos0{}, posOut{}, posBack{};
			Core::Vec2   spot{};
			double       tOut{ -1.0 }, tBack{ -1.0 }, tPeekSide{ -1.0 };
			Core::PeekKind kindSide{ Core::PeekKind::None };
			float        errOut{ -1.0f }, errBack{ -1.0f };
			float        remain0{ 0.0f };
		};

		bool StagePeeks(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto d = std::make_shared<R5Data>();
				d->enter = std::make_shared<CaseData>();
				AddLowPrepare(s, 0);
				AddEnter(s, d->enter, Rig::kKeyW, Core::CoverKind::Low);
				s.Do([d] {
					LogEnter("R5 setup enter low", d->enter, 1, false);
					Rig::ReleaseEverything("p3 peeks");
				});
				s.Wait(0.6);
				// ---- peek over, in the middle of the wall (both ends are about 2 m away) ----
				s.Do([d] {
					d->tAim = Now();
					Rig::MouseRight(true);
				});
				s.Until([] { const auto i = Cover::GetInfo(); return i.state == Core::CoverState::Peeking; }, 0.8, [d](bool ok) {
					d->tPeek = ok ? Now() : -1.0;
					d->kindOver = Cover::GetInfo().peek;
				});
				s.Until([] { return Stance() == 0; }, 0.8, [d](bool ok) { d->tStand = ok ? Now() : -1.0; });
				s.Wait(0.4);
				s.Do([d] {
					d->stanceAim = Stance();
					d->tRel = Now();
					Rig::MouseRight(false);
				});
				s.Until([] { return Stance() == 1; }, 0.8, [d](bool ok) { d->tCrouch = ok ? Now() : -1.0; });
				s.Until([] { return Cover::GetInfo().state == Core::CoverState::InCover; }, 0.8, [d](bool ok) { d->tIn = ok ? Now() : -1.0; });
				s.Do([d] {
					const bool ok = d->kindOver == Core::PeekKind::Over && d->tPeek > 0.0 && d->tStand > 0.0 && d->tStand - d->tAim <= 0.3 && d->stanceAim == 0 && d->tCrouch > 0.0 && d->tCrouch - d->tRel <= 0.3;
					Result(std::format("P3 R5 peek over (aim held in the middle of the 0.74 m wall): state Peeking ({}) {:.2f} s after the aim; stance 0 (standing) {:.2f} s after the aim (limit 0.3), stance while aiming {}; after the release the stance was 1 (crouched) {:.2f} s later (limit 0.3), the state InCover {:.2f} s later -> {}",
						Core::ToString(d->kindOver), d->tPeek > 0.0 ? d->tPeek - d->tAim : -1.0, d->tStand > 0.0 ? d->tStand - d->tAim : -1.0, d->stanceAim, d->tCrouch > 0.0 ? d->tCrouch - d->tRel : -1.0, d->tIn > 0.0 ? d->tIn - d->tRel : -1.0, Verdict(ok)));
					Summarise("R5 peek over: stand up on aim, crouch on release", ok);
				});
				s.Wait(0.6);
				// ---- side peek: D (along +tangent when facing the wall) to the open end, then aim ----
				s.Do([] { Rig::Tap(Rig::kKeyD, 3.5); });
				s.Until([] { return Cover::GetInfo().remainPlus <= 0.55f || Cover::GetInfo().state != Core::CoverState::InCover; }, 4.0);
				s.Do([] { Rig::ReleaseEverything("p3 at the end"); });
				s.Wait(0.8);
				s.Do([d] {
					const auto i = Cover::GetInfo();
					d->pos0 = Snap().position;
					d->remain0 = i.remainPlus;
					const auto& cd = i.candidate;
					d->spot = { cd.standPoint.x + cd.tangent.x * (cd.edgeRight + 0.5f), cd.standPoint.y + cd.tangent.y * (cd.edgeRight + 0.5f) };
					Result(std::format("P3 R5 at the end: state {}, player ({:.2f},{:.2f}), room left {:.2f} m, the open end is {:.2f} m from the foot of the cover ({}), the peek spot should be ({:.2f},{:.2f}) (0.5 m past the end)", Core::ToString(i.state), d->pos0.x, d->pos0.y, i.remainPlus,
						cd.edgeRight, Core::ToString(cd.edgeRightKind), d->spot.x, d->spot.y));
					d->tAim = Now();
					Rig::MouseRight(true);
				});
				s.Until([] { return Cover::GetInfo().state == Core::CoverState::Peeking; }, 1.0, [d](bool ok) {
					d->tPeekSide = ok ? Now() : -1.0;
					d->kindSide = Cover::GetInfo().peek;
				});
				s.Until([d] { const auto& p = Snap().position; return std::hypot(p.x - d->spot.x, p.y - d->spot.y) <= 0.05f; }, 0.8, [d](bool ok) { d->tOut = ok ? Now() : -1.0; });
				s.Wait(0.3);
				s.Do([d] {
					const auto& p = Snap().position;
					d->posOut = p;
					d->errOut = std::hypot(p.x - d->spot.x, p.y - d->spot.y);
					d->tRel = Now();
					Rig::MouseRight(false);
				});
				s.Until([d] { const auto& p = Snap().position; return std::hypot(p.x - d->pos0.x, p.y - d->pos0.y) <= 0.05f && Cover::GetInfo().state == Core::CoverState::InCover; }, 1.0, [d](bool ok) { d->tBack = ok ? Now() : -1.0; });
				s.Wait(0.3);
				s.Do([d] {
					d->posBack = Snap().position;
					d->errBack = std::hypot(d->posBack.x - d->pos0.x, d->posBack.y - d->pos0.y);
					const float peekLimit = (std::max)(0.3f, static_cast<float>(std::hypot(d->pos0.x - d->spot.x, d->pos0.y - d->spot.y)) / Config::Get().peekSpeed + 0.15f);  // T3.3 batch 3
					const bool ok = d->kindSide == Core::PeekKind::Right && d->tOut > 0.0 && d->tOut - d->tAim <= peekLimit && d->errOut <= 0.05f && d->tBack > 0.0 && d->tBack - d->tRel <= peekLimit && d->errBack <= 0.05f;
					Result(std::format("P3 R5 side peek ({}) at the open end: state Peeking {:.2f} s after the aim; at the spot (within 0.05 m) {:.2f} s after the aim (limit {:.2f}), {:.3f} m from it 0.3 s later; after the release back within 0.05 m of where it was {:.2f} s later (limit {:.2f}), {:.3f} m off 0.3 s after that, state {}; the move was {:.2f} m -> {}",
						Core::ToString(d->kindSide), d->tPeekSide > 0.0 ? d->tPeekSide - d->tAim : -1.0, d->tOut > 0.0 ? d->tOut - d->tAim : -1.0, peekLimit, d->errOut, d->tBack > 0.0 ? d->tBack - d->tRel : -1.0, peekLimit, d->errBack, Core::ToString(Cover::GetInfo().state), std::hypot(d->pos0.x - d->spot.x, d->pos0.y - d->spot.y), Verdict(ok)));
					Summarise("R5 side peek: out 0.5 m past the end and back", ok);
					Rig::Tap(Rig::kKeyLCtrl, 0.08);  // leave
				});
				s.Wait(0.8);
				s.Do([] { Rig::ReleaseEverything("p3 peeks done"); });
				s.Until([] { return Stance() == 0; }, 1.0);
			}
			return Run(s, c.now);
		}

		// ---- T3.5: the INI reload, the dedicated key (mode 0) and the held Sneak control (mode 2) -----------------------------------------------------
		bool StageReload(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				s.Do([] {
					Config::Mutable().coverStickMin = 0.9f;
					Config::Mutable().peekTime = 0.9f;
					Rig::Tap(Rig::kKeyNum9, 0.08);
				});
				s.Wait(0.6);
				s.Do([] {
					const auto& cfg = Config::Get();
					const bool  ok = std::fabs(cfg.coverStickMin - 0.4f) < 1e-4f && std::fabs(cfg.peekTime - 0.15f) < 1e-4f;
					Result(std::format("P3 T3.5 reload: fCoverStickMin was set to 0.90 and fPeekTime to 0.90 in memory, Numpad 9 pressed: they read {:.2f} and {:.2f} now (the INI says 0.40 and 0.15) -> {}", cfg.coverStickMin, cfg.peekTime, Verdict(ok)));
					Summarise("T3.5 INI reload on Numpad 9", ok);
				});
			}
			return Run(s, c.now);
		}

		bool StageMode0(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto d = std::make_shared<CaseData>();
				AddLowPrepare(s, 0);
				s.Do([d] {
					Config::Mutable().coverTrigger = 0;
					Cover::ForceReset("p3 mode 0");
					d->presses0 = Cover::GetInfo().presses;
				});
				s.Until([] { return FreshCandidate(Core::CoverKind::Low); }, 3.0, [](bool ok) {
					if (!ok) {
						Result("P3 T3.5 mode 0: the background sensor produced no candidate along the view within 3 s");
					}
				});
				s.Do([d] { d->tapT = Now(); d->tIn = -1.0; d->tNone = -1.0; Rig::Tap(Rig::kKeyNum5, 0.08); });
				s.Until([] { return Cover::GetInfo().state == Core::CoverState::InCover; }, 1.0, [d](bool ok) { d->tIn = ok ? Now() : -1.0; });
				s.Until([] { return Stance() == 1; }, 2.0, [d](bool ok) { d->tNone = ok ? Now() : -1.0; });  // reused: when the stance became 1
				s.Wait(0.4);
				s.Do([d] {
					const auto i = Cover::GetInfo();
					const bool ok = d->tIn > 0.0 && d->tIn - d->tapT <= 0.5 && d->tNone > 0.0 && d->tNone - d->tapT <= 1.0 && i.state == Core::CoverState::InCover && Stance() == 1 && i.presses == d->presses0;
					Result(std::format("P3 T3.5 mode 0 enter (Numpad 5, no movement input, standing before): InCover {:.2f} s after the key (limit 0.5), the stance became 1 (low cover) {:.2f} s after the key by a press made by the plugin (limit 1.0), state {}, Sneak presses seen by the cover logic {} (expected 0) -> {}",
						d->tIn > 0.0 ? d->tIn - d->tapT : -1.0, d->tNone > 0.0 ? d->tNone - d->tapT : -1.0, Core::ToString(i.state), i.presses - d->presses0, Verdict(ok)));
					Summarise("T3.5 mode 0: the dedicated key takes cover", ok);
					d->tapT = Now();
					d->tIn = -1.0;
					d->tNone = -1.0;
					Rig::Tap(Rig::kKeyNum5, 0.08);
				});
				s.Until([] { return Cover::GetInfo().state == Core::CoverState::None; }, 1.0, [d](bool ok) { d->tIn = ok ? Now() : -1.0; });
				s.Until([] { return Stance() == 0; }, 2.0, [d](bool ok) { d->tNone = ok ? Now() : -1.0; });
				s.Wait(0.3);
				s.Do([d] {
					const bool ok = d->tIn > 0.0 && d->tIn - d->tapT <= 0.5 && d->tNone > 0.0 && d->tNone - d->tapT <= 1.2 && Stance() == 0;
					Result(std::format("P3 T3.5 mode 0 leave (Numpad 5 again): state None {:.2f} s after the key (limit 0.5), the stance back to 0 {:.2f} s after the key by a press made by the plugin (limit 1.2) -> {}", d->tIn > 0.0 ? d->tIn - d->tapT : -1.0,
						d->tNone > 0.0 ? d->tNone - d->tapT : -1.0, Verdict(ok)));
					Summarise("T3.5 mode 0: the key leaves, stance restored", ok);
					Config::Mutable().coverTrigger = 1;
				});
			}
			return Run(s, c.now);
		}

		bool StageMode2(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto d = std::make_shared<CaseData>();
				AddLowPrepare(s, 0);
				s.Do([] {
					Config::Mutable().coverTrigger = 2;
					Cover::ForceReset("p3 mode 2");
					Rig::Tap(Rig::kKeyW, 6.0);  // pushed toward the wall
				});
				s.Until([] { return FreshCandidate(Core::CoverKind::Low); }, 2.0);
				// a tap stays the vanilla toggle
				s.Do([d] { d->stanceBefore = Stance(); Rig::Tap(Rig::kKeyLCtrl, 0.08); });
				s.Wait(0.7);
				s.Do([d] {
					const auto i = Cover::GetInfo();
					const bool ok = i.state == Core::CoverState::None && Stance() == 1 && d->stanceBefore == 0;
					Result(std::format("P3 T3.5 mode 2, a tap with the input pushed toward the wall: state {} (expected None), stance {} -> {} (a vanilla toggle) -> {}", Core::ToString(i.state), d->stanceBefore, Stance(), Verdict(ok)));
					Summarise("T3.5 mode 2: a tap stays vanilla", ok);
					Rig::Tap(Rig::kKeyLCtrl, 0.08);  // back to standing
				});
				s.Until([] { return Stance() == 0; }, 1.0);
				s.Wait(0.4);
				// a hold takes cover
				s.Do([d] {
					d->tapT = Now();
					d->tIn = -1.0;
					d->stanceBefore = Stance();
					Rig::Tap(Rig::kKeyLCtrl, 0.8);
				});
				s.Until([] { return Cover::GetInfo().state == Core::CoverState::InCover; }, 1.2, [d](bool ok) { d->tIn = ok ? Now() : -1.0; });
				s.Wait(0.8);
				s.Do([d] {
					const auto i = Cover::GetInfo();
					const double dt = d->tIn - d->tapT;
					const bool   ok = d->tIn > 0.0 && dt >= 0.2 && dt <= 0.6 && i.state == Core::CoverState::InCover && Stance() == 1;
					Result(std::format("P3 T3.5 mode 2, the Sneak control held 0.8 s with the input pushed toward the wall: InCover {:.2f} s after the press (fCoverHoldTime 0.25, expected 0.2 to 0.6), state {}, stance {} (low cover, expected 1) -> {}", d->tIn > 0.0 ? dt : -1.0,
						Core::ToString(i.state), Stance(), Verdict(ok)));
					Summarise("T3.5 mode 2: a hold takes cover", ok);
					Rig::ReleaseEverything("p3 mode 2");
					Rig::Tap(Rig::kKeyLCtrl, 0.08);  // leave
				});
				s.Until([] { return Cover::GetInfo().state == Core::CoverState::None; }, 1.0);
				s.Until([] { return Stance() == 0; }, 1.5);
				s.Do([] { Config::Mutable().coverTrigger = 1; });
				s.Wait(0.3);
			}
			return Run(s, c.now);
		}

		// batch 5: in low cover, the key that points straight into the wall held for 1.5 s: the dead zone must write exactly (0,0) and the player must not drift along the wall
		struct StraightData
		{
			RE::NiPoint3  p0{};
			Core::Vec2    tangent{}, normal{};
			double        t0{ 0.0 };
			std::uint64_t lastCalls{ 0 }, calls{ 0 }, nonZero{ 0 };
			float         maxAlong{ 0.0f }, maxAcross{ 0.0f };
			float         gx{ 0.0f }, gy{ 0.0f };
		};

		void AddStraightIn(Seq& s)
		{
			auto h = std::make_shared<StraightData>();
			s.Do([h] {
				const auto i = Cover::GetInfo();
				h->tangent = { i.candidate.tangent.x, i.candidate.tangent.y };
				h->normal = { i.candidate.normal.x, i.candidate.normal.y };
				h->p0 = Snap().position;
				h->t0 = Now();
				h->lastCalls = Input::LastMovementVector().calls;
				Rig::Tap(Rig::kKeyW, 1.5);
			});
			s.Until([h] {
				const auto v = Input::LastMovementVector();
				if (v.calls != h->lastCalls) {
					h->lastCalls = v.calls;
					++h->calls;
					if (v.y > 0.5f) {
						h->gx = v.x;
						h->gy = v.y;
					}
					if (h->calls > 0 && Now() - h->t0 > 0.15 && (v.wroteX != 0.0f || v.wroteY != 0.0f)) {
						++h->nonZero;  // a vector written (or left) that is not exactly zero after the key has been seen
					}
				}
				const auto& p = Snap().position;
				h->maxAlong = (std::max)(h->maxAlong, std::fabs((p.x - h->p0.x) * h->tangent.x + (p.y - h->p0.y) * h->tangent.y));
				h->maxAcross = (std::max)(h->maxAcross, std::fabs((p.x - h->p0.x) * h->normal.x + (p.y - h->p0.y) * h->normal.y));
				return Now() - h->t0 >= 1.5;
			}, 3.0);
			s.Do([h] {
				Rig::ReleaseEverything("p3 straight in");
				const auto st = Cover::GetInfo().state;
				const bool ok = h->maxAlong < 0.05f && h->nonZero == 0 && h->calls > 20 && st == Core::CoverState::InCover;
				Result(std::format("P3 R4a straight into the wall (W held 1.5 s in low cover): the game vector read ({:+.3f},{:+.3f}); the vector left in the manager was not exactly (0,0) in {} of {} hook calls (limit 0); the largest move along the wall {:.3f} m (limit 0.05), across {:.3f} m; state {} -> {}",
					h->gx, h->gy, h->nonZero, h->calls, h->maxAlong, h->maxAcross, Core::ToString(st), Verdict(ok)));
				Summarise("R4a straight into the wall: dead zone, no drift", ok);
			});
			s.Wait(0.4);
		}

		bool StageLowEnter(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto d = std::make_shared<CaseData>();
				// R4a: standing -> the press passes, stance 1 within 0.3 s; leave by Sneak: 0 again
				AddLowPrepare(s, 0);
				s.Do([d] {
					if (s_lowSpot < 0) {
						d->tapped = false;
					}
				});
				AddEnter(s, d, Rig::kKeyW, Core::CoverKind::Low);
				s.Do([d] { LogEnter("R4a enter low from standing", d, 1, false); });
				AddStraightIn(s);
				s.Do([d] {
					d->stanceAfter = -1;
					d->leaveT = Now();
					Rig::Tap(Rig::kKeyLCtrl, 0.08);
				});
				s.Until([] { return Cover::GetInfo().state == Core::CoverState::None; }, 1.0, [d](bool ok) { d->tNone = ok ? Now() : -1.0; });
				s.Until([] { return Stance() == 0; }, 1.5, [d](bool ok) { d->tIn = ok ? Now() : -1.0; });
				s.Wait(0.4);
				s.Do([d] {
					Rig::ReleaseEverything("p3 low leave");
					const double dt = d->tNone - d->leaveT;
					const bool   ok = d->tNone > 0.0 && dt <= 0.5 && d->tIn > 0.0 && Stance() == 0;
					Result(std::format("P3 R4a leave by Sneak: state None {:.2f} s after the tap (limit 0.5); stance back to {} (expected 0) {:.2f} s after the tap -> {}", d->tNone > 0.0 ? dt : -1.0, Stance(), d->tIn > 0.0 ? d->tIn - d->leaveT : -1.0, Verdict(ok)));
					Summarise("R4a leave low by Sneak: stance 0 again", ok);
				});
				// R4b: already sneaking -> the press is swallowed, stance stays 1; leaving by Sneak keeps 1
				auto e = std::make_shared<CaseData>();
				AddLowPrepare(s, 1);
				AddEnter(s, e, Rig::kKeyW, Core::CoverKind::Low);
				s.Do([e] { LogEnter("R4b enter low while already sneaking", e, 1, true); });
				s.Do([e] {
					e->leaveT = Now();
					Rig::Tap(Rig::kKeyLCtrl, 0.08);
				});
				s.Until([] { return Cover::GetInfo().state == Core::CoverState::None; }, 1.0, [e](bool ok) { e->tNone = ok ? Now() : -1.0; });
				s.Wait(0.5);
				s.Do([e] {
					Rig::ReleaseEverything("p3 low leave 2");
					e->stanceAfter = Stance();
					const double dt = e->tNone - e->leaveT;
					const bool   ok = e->tNone > 0.0 && dt <= 0.5 && e->stanceAfter == 1;
					Result(std::format("P3 R4b leave by Sneak after entering while sneaking: state None {:.2f} s after the tap (limit 0.5); stance {} (expected 1: the stance before the cover was sneaking) -> {}", e->tNone > 0.0 ? dt : -1.0, e->stanceAfter, Verdict(ok)));
					Summarise("R4b leave low by Sneak: stance stays 1", ok);
					Rig::Tap(Rig::kKeyLCtrl, 0.08);  // back to standing
				});
				s.Until([] { return Stance() == 0; }, 1.0);
				s.Wait(0.3);
			}
			return Run(s, c.now);
		}

		// ======================================== set 3: the doorway of card P2 (R7) ======================================================
		// The route is the user's own, read from the S1 position lines of the play check (build\CoverSF-playcheck-P2.log, 05:32:54 to 05:33:42, in
		// reverse); every point lies on a place the player stood, all on the floor at z 16.07. The save's spawn is the player's position when they quit.
		struct Wp
		{
			float x, y;
		};
		constexpr std::array<Wp, 12> kRoute{ { { 284.34f, -149.66f }, { 284.84f, -150.65f }, { 286.20f, -152.13f }, { 286.65f, -154.55f }, { 285.07f, -155.84f }, { 283.67f, -159.61f }, { 281.91f, -161.63f },
			{ 283.12f, -164.34f }, { 283.06f, -167.25f }, { 281.13f, -170.21f }, { 279.53f, -169.53f }, { 279.20f, -172.75f } } };
		struct SpotR7
		{
			float x, y, view;
			const char* name;
		};
		constexpr std::array<SpotR7, 3> kR7{ { { 279.53f, -169.53f, 122.0f, "#86 (279.53,-169.53) view 122" }, { 279.20f, -172.75f, 29.0f, "#85 (279.20,-172.75) view 29" }, { 279.33f, -173.78f, 18.0f, "#84 (279.33,-173.78) view 18" } } };

		bool         s_routeBlocked{ false };
		RE::NiPoint3 s_segFrom{};

		// A leg is free when casts at knee and chest height along it (ending at its end point) hit nothing.
		struct Leg
		{
			float                x0, y0, x1, y1;
			WorldProbe::Ticket   t[2]{};
		};
		std::vector<Leg>         s_legs;       // direct leg first, then two legs per sidestep candidate
		std::vector<float>       s_viaOffsets{ 0.6f, -0.6f, 1.2f, -1.2f, 1.8f, -1.8f };
		std::optional<Core::Vec2> s_via;
		bool                     s_direct{ true };

		void SubmitLeg(Leg& a_leg, const float a_z)
		{
			const float len = std::hypot(a_leg.x1 - a_leg.x0, a_leg.y1 - a_leg.y0);
			int         k = 0;
			for (const float h : { 0.4f, 1.0f }) {
				a_leg.t[k++] = WorldProbe::Submit({ a_leg.x0, a_leg.y0, a_z + h }, { a_leg.x1, a_leg.y1, a_z + h }, 0.1f);
			}
			(void)len;
		}

		// polls both casts of a leg; true when answered. a_blocked: one of them hit something (hit point logged once)
		bool PollLeg(Leg& a_leg, bool& a_blocked, std::string* a_where = nullptr)
		{
			static std::map<WorldProbe::Ticket, WorldProbe::CastResult> s_answered;
			bool all = true;
			a_blocked = false;
			for (int k = 0; k < 2; ++k) {
				if (a_leg.t[k] == 0) {
					continue;
				}
				if (!s_answered.contains(a_leg.t[k])) {
					if (const auto r = WorldProbe::Poll(a_leg.t[k])) {
						s_answered[a_leg.t[k]] = *r;
					} else {
						all = false;
						continue;
					}
				}
				const auto& r = s_answered[a_leg.t[k]];
				if (r.ok && r.hit) {
					a_blocked = true;
					if (a_where) {
						*a_where = std::format("{} cast hit at ({:.2f},{:.2f},{:.2f})", k == 0 ? "knee-height" : "chest-height", r.point.x, r.point.y, r.point.z);
					}
				}
			}
			return all;
		}

		// one segment of the route: a clear-path check at knee and chest height; when the direct line is blocked, a sidestep waypoint beside the middle of
		// it (0.6, 1.2, 1.8 m to either side) with both legs clear is tried; then the eased move(s) at 1.5 m/s
		void AddSegment(Seq& s, const float a_x, const float a_y, const std::string a_label)
		{
			s.Do([a_x, a_y] {
				s_via.reset();
				s_direct = true;
				if (s_routeBlocked) {
					return;
				}
				const auto& p = Snap().position;
				s_segFrom = p;
				s_legs.clear();
				s_legs.push_back({ p.x, p.y, a_x, a_y });
				SubmitLeg(s_legs[0], p.z);
			});
			s.Until([a_x, a_y, a_label] {
				if (s_routeBlocked) {
					return true;
				}
				bool blocked = false;
				std::string where;
				if (!PollLeg(s_legs[0], blocked, &where)) {
					return false;
				}
				if (blocked) {
					s_direct = false;
					REX::INFO("P3 R7 route: the direct step to {} ({:.2f},{:.2f}) from ({:.2f},{:.2f}) is blocked: {}; trying sidestep waypoints", a_label, a_x, a_y, s_segFrom.x, s_segFrom.y, where);
					const float dx = a_x - s_segFrom.x, dy = a_y - s_segFrom.y;
					const float len = (std::max)(std::hypot(dx, dy), 0.01f);
					const float px = -dy / len, py = dx / len;  // perpendicular
					const float mx = s_segFrom.x + dx * 0.5f, my = s_segFrom.y + dy * 0.5f;
					s_legs.resize(1);
					for (const float off : s_viaOffsets) {
						const float vx = mx + px * off, vy = my + py * off;
						s_legs.push_back({ s_segFrom.x, s_segFrom.y, vx, vy });
						s_legs.push_back({ vx, vy, a_x, a_y });
					}
					for (std::size_t k = 1; k < s_legs.size(); ++k) {
						SubmitLeg(s_legs[k], s_segFrom.z);
					}
				}
				return true;
			}, 3.0);
			s.Until([] {
				if (s_routeBlocked || s_direct) {
					return true;
				}
				for (std::size_t k = 1; k < s_legs.size(); ++k) {
					bool b = false;
					if (!PollLeg(s_legs[k], b)) {
						return false;
					}
				}
				return true;
			}, 4.0);
			s.Do([a_x, a_y, a_label] {
				if (s_routeBlocked) {
					return;
				}
				if (!s_direct) {
					for (std::size_t k = 1; k + 1 < s_legs.size(); k += 2) {
						bool b1 = false, b2 = false;
						(void)PollLeg(s_legs[k], b1);
						(void)PollLeg(s_legs[k + 1], b2);
						if (!b1 && !b2) {
							s_via = Core::Vec2{ s_legs[k].x1, s_legs[k].y1 };
							REX::INFO("P3 R7 route: going to {} through the sidestep waypoint ({:.2f},{:.2f}), both legs clear", a_label, s_via->x, s_via->y);
							break;
						}
					}
					if (!s_via) {
						s_routeBlocked = true;
						Result(std::format("P3 R7 route: the step to {} ({:.2f},{:.2f}) is blocked and no sidestep waypoint is free; giving up", a_label, a_x, a_y));
						return;
					}
				}
				const auto& p = Snap().position;
				const float tx = s_via ? s_via->x : a_x, ty = s_via ? s_via->y : a_y;
				MoveBegin({ tx, ty, p.z }, (std::max)(0.6, std::hypot(tx - p.x, ty - p.y) / 1.5));
			});
			s.Until([] { return s_routeBlocked || MoveStep(); }, 8.0);
			s.Do([a_x, a_y] {
				if (s_routeBlocked || !s_via) {
					return;
				}
				const auto& p = Snap().position;
				MoveBegin({ a_x, a_y, p.z }, (std::max)(0.6, std::hypot(a_x - p.x, a_y - p.y) / 1.5));
			});
			s.Until([] { return s_routeBlocked || MoveStep(); }, 8.0);
		}

		void AddR7Find(Seq& s, const SpotR7 a_spot)
		{
			s.Wait(1.0);
			s.Do([a_spot] {
				if (s_routeBlocked) {
					return;
				}
				const auto& p = Snap().position;
				const float h = a_spot.view * kPi / 180.0f;
				Sensor().SetSettings(CoverSense::Settings());
				(void)Sensor().Find({ p.x, p.y, p.z }, { std::sin(h), std::cos(h) });
				ScanBegin(p, h);
			});
			s.Until([] {
				if (s_routeBlocked) {
					return true;
				}
				auto& sensor = Sensor();
				sensor.Update();
				return sensor.HasResult() && s_scan.Done();
			}, 6.0);
			s.Do([a_spot] {
				if (s_routeBlocked) {
					return;
				}
				auto& sensor = Sensor();
				if (!sensor.HasResult()) {
					Result(std::format("P3 R7 {}: no answer from the sensor within 6 s", a_spot.name));
					return;
				}
				const auto  r = sensor.TakeResult();
				const auto& p = Snap().position;
				std::string pts;
				for (const auto& q : r.fitPoints) {
					pts += std::format(" ({:.2f},{:.2f})", q.x, q.y);
				}
				const bool wantHigh = a_spot.view != 29.0f;
				const bool high = r.candidate.kind == Core::CoverKind::High;
				Result(std::format("P3 R7 find at {} from ({:.2f},{:.2f},{:.2f}) with the user's view as intent: {}", a_spot.name, p.x, p.y, p.z, CoverSense::Describe(r)));
				Result(std::format("P3 R7 find at {}: search chose the ray whose centre is ({:.2f},{:.2f}); fit points ({}):{} | {}", a_spot.name, r.searchHit.x, r.searchHit.y, r.fitPoints.size(), pts, ScanText()));
				Result(std::format("P3 R7 at {}: expected {} -> {}", a_spot.name, wantHigh ? "HIGH" : "no expectation (card P2 #85 read NONE)", wantHigh ? Verdict(high) : "INFO"));
				Summarise(std::string("R7 ") + a_spot.name, wantHigh ? high : true);
			});
		}

		bool StageDoorway(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				s_routeBlocked = false;
				s.Do([] {
					Rig::ReleaseEverything("p3 doorway");
					Cover::ForceReset("p3 doorway");
					Result(std::format("P3 R7 start: the player is at ({:.2f},{:.2f},{:.2f}); the route to the doorway has {} points, about 25 m", Snap().position.x, Snap().position.y, Snap().position.z, kRoute.size()));
				});
				for (std::size_t i = 0; i < kRoute.size(); ++i) {
					AddSegment(s, kRoute[i].x, kRoute[i].y, std::format("route point {}", i + 1));
					if (i == 10) {
						AddR7Find(s, kR7[0]);
					}
					if (i == 11) {
						AddR7Find(s, kR7[1]);
					}
				}
				AddSegment(s, kR7[2].x, kR7[2].y, "spot #84");
				AddR7Find(s, kR7[2]);
			}
			return Run(s, c.now);
		}

		bool StageSummary(const Ctx&)
		{
			Result(std::format("P3 summary: {} passed, {} failed", s_pass, s_fail));
			for (const auto& line : s_summary) {
				REX::INFO("RESULT P3 summary {}", line);
			}
			return true;
		}

		// ======================================== M1b: the held press (R9) and the automatic mode (R10) ================================================
		struct StanceWatch
		{
			int    last{ -1 };
			int    changes{ 0 };
			double firstAt{ -1.0 };
			double lastAt{ -1.0 };
		};

		void WatchStep(StanceWatch& w)
		{
			const int s = Stance();
			if (s < 0) {
				return;
			}
			if (w.last < 0) {
				w.last = s;
				return;
			}
			if (s != w.last) {
				++w.changes;
				if (w.firstAt < 0.0) {
					w.firstAt = Now();
				}
				w.lastAt = Now();
				w.last = s;
			}
		}

		struct R9Data
		{
			StanceWatch   w;
			double        tapT{ 0.0 };
			double        tIn{ -1.0 };
			double        tNone{ -1.0 };
			double        tStance{ -1.0 };
			int           stanceBefore{ -1 };
			std::uint32_t presses0{ 0 };
			std::uint32_t swallowed0{ 0 };
		};

		// R9: the Sneak control held for 1.5 s while pushing at the low wall: InCover, stance 1, exactly one stance change and none at the release; held again in cover: left, stance as before
		bool StageR9(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto d = std::make_shared<R9Data>();
				AddLowPrepare(s, 0);
				s.Do([] {
					Config::Mutable().coverTrigger = 1;
					Cover::ForceReset("p3 R9");
					Rig::Tap(Rig::kKeyW, 8.0);  // pushed toward the wall
				});
				s.Until([] { return FreshCandidate(Core::CoverKind::Low); }, 3.0, [](bool ok) {
					if (!ok) {
						Result("P3 R9: no fresh LOW candidate within 3 s (no press sent)");
					}
				});
				s.Do([d] {
					const auto i = Cover::GetInfo();
					d->stanceBefore = Stance();
					d->w = StanceWatch{};
					d->w.last = Stance();
					d->presses0 = i.presses;
					d->swallowed0 = i.swallowed;
					d->tapT = Now();
					Rig::Tap(Rig::kKeyLCtrl, 1.5);
				});
				s.Until([d] {
					WatchStep(d->w);
					if (d->tIn < 0.0 && Cover::GetInfo().state == Core::CoverState::InCover) {
						d->tIn = Now();
					}
					if (d->tStance < 0.0 && Stance() == 1) {
						d->tStance = Now();
					}
					return Now() - d->tapT >= 2.6;
				}, 4.5);
				s.Do([d] {
					const auto i = Cover::GetInfo();
					const bool ok = d->tIn > 0.0 && i.state == Core::CoverState::InCover && Stance() == 1 && d->w.changes == 1 && d->stanceBefore == 0 && i.presses == d->presses0 + 1 && i.swallowed == d->swallowed0 + 1;
					Result(std::format("P3 R9a the Sneak key held 1.5 s while pushing at the low wall: InCover {:.2f} s after the press, the stance made by the plugin became 1 {:.2f} s after the press (the key was let go at 1.50 s), stance changes seen in 2.6 s: {} (expected exactly 1; first at {:.2f} s, last at {:.2f} s), stance before {} now {}, the press was swallowed {} (expected 1), state {} -> {}",
						d->tIn > 0.0 ? d->tIn - d->tapT : -1.0, d->tStance > 0.0 ? d->tStance - d->tapT : -1.0, d->w.changes, d->w.firstAt > 0.0 ? d->w.firstAt - d->tapT : -1.0, d->w.lastAt > 0.0 ? d->w.lastAt - d->tapT : -1.0, d->stanceBefore,
						Stance(), i.swallowed - d->swallowed0, Core::ToString(i.state), Verdict(ok)));
					Summarise("R9a held press enters low: stance 1, one change", ok);
					Rig::ReleaseEverything("p3 R9");
					d->w = StanceWatch{};
					d->w.last = Stance();
					d->presses0 = i.presses;
					d->tapT = Now();
					d->tNone = -1.0;
					d->tStance = -1.0;
					Rig::Tap(Rig::kKeyLCtrl, 1.5);
				}, 0.3);
				s.Do([d] { d->w.last = Stance(); d->tapT = Now(); }, 0.0);
				s.Until([d] {
					WatchStep(d->w);
					if (d->tNone < 0.0 && Cover::GetInfo().state == Core::CoverState::None) {
						d->tNone = Now();
					}
					if (d->tStance < 0.0 && Stance() == 0) {
						d->tStance = Now();
					}
					return Now() - d->tapT >= 2.6;
				}, 4.5);
				s.Do([d] {
					const auto i = Cover::GetInfo();
					const bool ok = d->tNone > 0.0 && i.state == Core::CoverState::None && Stance() == 0 && d->w.changes == 1;
					Result(std::format("P3 R9b the Sneak key held 1.5 s in cover: state None {:.2f} s after the press, the stance back to 0 {:.2f} s after the press, stance changes seen in 2.6 s: {} (expected exactly 1; first at {:.2f} s, last at {:.2f} s), state {} stance {} -> {}",
						d->tNone > 0.0 ? d->tNone - d->tapT : -1.0, d->tStance > 0.0 ? d->tStance - d->tapT : -1.0, d->w.changes, d->w.firstAt > 0.0 ? d->w.firstAt - d->tapT : -1.0, d->w.lastAt > 0.0 ? d->w.lastAt - d->tapT : -1.0, Core::ToString(i.state), Stance(), Verdict(ok)));
					Summarise("R9b held press leaves: stance as before", ok);
					Rig::ReleaseEverything("p3 R9");
				});
				s.Wait(0.4);
			}
			return Run(s, c.now);
		}

		struct R10Data
		{
			double        tPush{ 0.0 };
			double        tIn{ -1.0 };
			double        tNone{ -1.0 };
			double        tSt{ -1.0 };
			bool          everIn{ false };
			bool          sawFresh{ false };
			std::uint32_t presses0{ 0 };
			std::uint32_t entered0{ 0 };
		};

		void R10Begin(Seq& s, const std::shared_ptr<R10Data>& d)
		{
			s.Do([d] {
				Rig::ReleaseEverything("p3 R10");
				Cover::ForceReset("p3 R10");
				Config::Mutable().coverTrigger = 3;
				Config::Mutable().autoCoverTall = 1;  // the tests of R10 are those of the rule "tall cover only near an open end" (iAutoCoverTall 2, the card, takes tall cover anywhere with the weapon drawn)
				d->tIn = -1.0;
				d->tNone = -1.0;
				d->tSt = -1.0;
				d->everIn = false;
				d->sawFresh = false;
				d->presses0 = Cover::GetInfo().presses;
			});
		}

		// R10 (mode 3, the low wall of Save30): W into the wall from about 1.7 m takes cover by itself; S leaves; W again enters again; B in cover with W held leaves and there is no
		// re-entry while W stays held; along the wall at a flat angle nothing happens in 2 s
		bool StageR10Low(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto d = std::make_shared<R10Data>();
				// a: W into the wall
				AddLowPrepare(s, 0);
				R10Begin(s, d);
				s.Do([d] {
					d->tPush = Now();
					Rig::Tap(Rig::kKeyW, 9.0);
				});
				s.Until([d] {
					if (Cover::GetInfo().state != Core::CoverState::None && d->tIn < 0.0) {
						d->tIn = Now();
					}
					return d->tIn > 0.0;
				}, 5.0);
				s.Until([d] { return Stance() == 1 || (d->tIn < 0.0); }, 2.0, [d](bool ok) { d->tSt = ok ? Now() : -1.0; });
				s.Wait(0.4);
				s.Do([d] {
					const auto i = Cover::GetInfo();
					const bool ok = d->tIn > 0.0 && i.state == Core::CoverState::InCover && Stance() == 1 && i.presses == d->presses0;
					Result(std::format("P3 R10a mode 3, W held toward the low wall from 1.66 m (the wall's stand point): InCover {:.2f} s after W went down (the condition needs the stand point within 0.60 m, then 0.30 s), the stance became 1 {:.2f} s after W, Sneak presses seen {} (expected 0), state {} stance {} -> {}",
						d->tIn > 0.0 ? d->tIn - d->tPush : -1.0, d->tSt > 0.0 ? d->tSt - d->tPush : -1.0, i.presses - d->presses0, Core::ToString(i.state), Stance(), Verdict(ok)));
					Summarise("R10a automatic entry at the low wall", ok);
				});
				// b: S leaves
				s.Do([d] {
					Rig::ReleaseEverything("p3 R10 b");
					d->tNone = -1.0;
					d->tSt = -1.0;
					d->tPush = Now();
					Rig::Tap(Rig::kKeyS, 6.0);
				});
				s.Until([] { return Cover::GetInfo().state == Core::CoverState::None; }, 3.0, [d](bool ok) { d->tNone = ok ? Now() : -1.0; });
				s.Until([] { return Stance() == 0; }, 2.0, [d](bool ok) { d->tSt = ok ? Now() : -1.0; });
				s.Wait(0.3);
				s.Do([d] {
					const auto i = Cover::GetInfo();
					const bool ok = d->tNone > 0.0 && i.state == Core::CoverState::None && Stance() == 0;
					Result(std::format("P3 R10b S held in cover: state None {:.2f} s after S, the stance back to 0 {:.2f} s after S -> {}", d->tNone > 0.0 ? d->tNone - d->tPush : -1.0, d->tSt > 0.0 ? d->tSt - d->tPush : -1.0, Verdict(ok)));
					Summarise("R10b S leaves, stance back", ok);
					Rig::ReleaseEverything("p3 R10 b");
				});
				// c: W again enters again
				s.Wait(0.7);
				s.Do([d] {
					d->tIn = -1.0;
					d->tSt = -1.0;
					d->tPush = Now();
					Rig::Tap(Rig::kKeyW, 9.0);
				});
				s.Until([d] {
					if (Cover::GetInfo().state != Core::CoverState::None && d->tIn < 0.0) {
						d->tIn = Now();
					}
					return d->tIn > 0.0;
				}, 5.0);
				s.Until([d] { return Stance() == 1 || (d->tIn < 0.0); }, 2.0, [d](bool ok) { d->tSt = ok ? Now() : -1.0; });
				s.Wait(0.4);
				s.Do([d] {
					const auto i = Cover::GetInfo();
					const bool ok = d->tIn > 0.0 && i.state == Core::CoverState::InCover && Stance() == 1;
					Result(std::format("P3 R10c W again after S: InCover {:.2f} s after W, stance became 1 {:.2f} s after W, state {} stance {} -> {}", d->tIn > 0.0 ? d->tIn - d->tPush : -1.0, d->tSt > 0.0 ? d->tSt - d->tPush : -1.0, Core::ToString(i.state), Stance(), Verdict(ok)));
					Summarise("R10c automatic entry again after leaving", ok);
				});
				// d: B in cover with W held leaves, and W held does not enter again by itself
				s.Do([d] {
					d->tNone = -1.0;
					d->everIn = false;
					d->tPush = Now();
					Rig::Tap(Rig::kKeyLCtrl, 0.08);
				});
				s.Until([] { return Cover::GetInfo().state == Core::CoverState::None; }, 1.2, [d](bool ok) { d->tNone = ok ? Now() : -1.0; });
				s.Until([d] {
					if (d->tNone > 0.0 && Cover::GetInfo().state != Core::CoverState::None) {
						d->everIn = true;
					}
					return d->tNone < 0.0 || Now() - d->tNone >= 2.0;
				}, 4.0);
				s.Do([d] {
					const auto i = Cover::GetInfo();
					const bool ok = d->tNone > 0.0 && !d->everIn && i.state == Core::CoverState::None;
					Result(std::format("P3 R10d B in cover with W still held: state None {:.2f} s after B; for the next 2.0 s with W held the state was {} (expected: never in cover again, the automatic entry waits for the pull toward cover to end) -> {}", d->tNone > 0.0 ? d->tNone - d->tPush : -1.0,
						d->everIn ? "taken again" : "None throughout", Verdict(ok)));
					Summarise("R10d no re-entry while W stays held after B", ok);
					Rig::ReleaseEverything("p3 R10 d");
				});
				s.Wait(0.5);
				// e: flat angle: W and D together are 45 deg off the wall's normal, beyond fAutoCoverAngleDeg (35)
				AddLowPrepare(s, 0);
				R10Begin(s, d);
				s.Do([d] {
					d->tPush = Now();
					Rig::Tap(Rig::kKeyW, 2.0);
					Rig::Tap(Rig::kKeyD, 2.0);
				});
				s.Until([d] {
					if (FreshCandidate(Core::CoverKind::None)) {
						d->sawFresh = true;
					}
					if (Cover::GetInfo().state != Core::CoverState::None) {
						d->everIn = true;
					}
					return Now() - d->tPush >= 2.2;
				}, 4.0);
				s.Do([d] {
					const auto& p = Snap().position;
					const bool  ok = !d->everIn && Cover::GetInfo().state == Core::CoverState::None;
					Result(std::format("P3 R10e W and D held together for 2.0 s toward the low wall (45 deg off its normal, limit 35): a fresh cover answer was seen {}; state never left None: {}; the player ended at ({:.2f},{:.2f}) -> {}", d->sawFresh ? "yes" : "no", d->everIn ? "NO" : "yes", p.x, p.y, Verdict(ok)));
					Summarise("R10e flat angle: no automatic entry in 2 s", ok);
					Rig::ReleaseEverything("p3 R10 e");
					Cover::ForceReset("p3 R10 end");
					Config::Mutable().coverTrigger = 1;
				});
				s.Wait(0.5);
			}
			return Run(s, c.now);
		}

		// R10 on the tall wall of Save29 (mode 3): S held toward the wall, nothing near an open end: no entry in 2 s; then B pushing toward the wall enters at once (stance stays 0)
		bool StageR10Tall(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto d = std::make_shared<R10Data>();
				auto e = std::make_shared<CaseData>();
				AddPrepare(s, 0, Base(), std::nullopt);
				R10Begin(s, d);
				s.Do([d] {
					d->tPush = Now();
					Rig::Tap(Rig::kKeyS, 2.2);
				});
				s.Until([d] {
					if (FreshCandidate(Core::CoverKind::High)) {
						d->sawFresh = true;
					}
					if (Cover::GetInfo().state != Core::CoverState::None) {
						d->everIn = true;
					}
					return Now() - d->tPush >= 2.4;
				}, 4.5);
				s.Do([d] {
					const auto i = Cover::GetInfo();
					const bool ok = !d->everIn && i.state == Core::CoverState::None && d->sawFresh;
					Result(std::format("P3 R10f mode 3, S held 2.2 s toward the tall wall of Save29 (no open end within the search range of its ends): a fresh HIGH answer was seen {}; the state stayed None: {} (the automatic entry for tall cover needs an open end within fAutoCoverTallEdge {:.1f}) -> {}",
						d->sawFresh ? "yes" : "NO", d->everIn ? "NO, cover was taken" : "yes", Config::Get().autoCoverTallEdge, Verdict(ok)));
					Summarise("R10f tall wall away from its ends: no automatic entry", ok);
					Rig::ReleaseEverything("p3 R10 f");
				});
				s.Wait(0.4);
				// B pushing toward the wall still enters at once in mode 3
				AddPrepare(s, 0, Base(), std::nullopt);
				R10Begin(s, d);
				AddEnter(s, e, Rig::kKeyS, Core::CoverKind::High);
				s.Do([e] { LogEnter("R10g mode 3: B pushing toward the tall wall enters at once", e, 0, true); });
				AddLeaveSneak(s, e);
				s.Do([e] {
					const bool ok = e->tNone > 0.0 && e->tNone - e->leaveT <= 0.5 && e->stanceAfter == 0;
					Result(std::format("P3 R10h mode 3, B in tall cover leaves: state None {:.2f} s after the press, stance {} (expected 0) -> {}", e->tNone > 0.0 ? e->tNone - e->leaveT : -1.0, e->stanceAfter, Verdict(ok)));
					Summarise("R10h B leaves in mode 3", ok);
					Cover::ForceReset("p3 R10 end");
					Config::Mutable().coverTrigger = 1;
				});
				s.Wait(0.4);
			}
			return Run(s, c.now);
		}


		// ======================================== M1c: T5.3a R11 the cover camera, T3.8 R12 B while peeking over ===================================
		// screenshots by the hand-off of the g2 script (CLAUDE.md): at "SCREENSHOT NOW <name>" the script waits (at most 25 s) for the ack file
		std::filesystem::path ShotAckPath()
		{
			const auto& f = Config::Get().shotAckFile;
			return f.empty() ? Config::Directory() / "g2_shot_ack.txt" : std::filesystem::path(f);
		}
		double s_shotStart{ 0.0 };

		void ShotBegin(const char* a_name)
		{
			std::error_code ec;
			std::filesystem::remove(ShotAckPath(), ec);
			s_shotStart = Now();
			s_shotWaiting = true;
			REX::INFO("SCREENSHOT NOW {} (the script waits for the ack file, at most 25 s)", a_name);
		}

		bool ShotDone()
		{
			std::error_code ec;
			const bool      acked = std::filesystem::exists(ShotAckPath(), ec);
			if (acked || Now() - s_shotStart > 25.0) {
				std::filesystem::remove(ShotAckPath(), ec);
				s_shotWaiting = false;
				REX::INFO("P3: screenshot {} after {:.1f} s; the script goes on", acked ? "acknowledged" : "timed out", Now() - s_shotStart);
				return true;
			}
			return false;
		}

		void AddShot(Seq& s, const std::string& a_name)
		{
			s.Do([a_name] { ShotBegin(a_name.c_str()); });
			s.Until(ShotDone, 26.0);
		}

		// the camera root as the engine reads it: horizontal distance and height above the player, the two rig nodes' local values, the collision factor
		struct CamAcc
		{
			int    n{ 0 };
			double sumH{ 0 }, sumZ{ 0 }, sumBoomY{ 0 }, sumPivotZ{ 0 };
			float  minH{ 1e9f }, maxH{ -1e9f }, minZ{ 1e9f }, maxZ{ -1e9f }, minF{ 1e9f };
			double last{ 0.0 }, t0{ 0.0 };
			const char* dump{ nullptr };  // a label: every sample is logged

			void Begin()
			{
				const char* keep = dump;
				*this = CamAcc{};
				dump = keep;
				t0 = Now();
			}
			void Step()
			{
				if (Now() - last < 0.05) {
					return;
				}
				last = Now();
				const auto r = Camera::ReadRig();
				if (!r.ok || !r.n1F8.present || !r.n200.present) {
					return;
				}
				const float h = r.rootToPlayerHoriz;
				const float z = r.rootWorld.z - r.player.z;
				++n;
				sumH += h;
				sumZ += z;
				sumBoomY += r.n1F8.t.y;
				sumPivotZ += r.n200.t.z;
				minH = (std::min)(minH, h);
				maxH = (std::max)(maxH, h);
				minZ = (std::min)(minZ, z);
				maxZ = (std::max)(maxZ, z);
				minF = (std::min)(minF, r.factor);
				if (dump) {
					const auto cc = Camera::GetCoverCam();
					REX::INFO("P3 R11 sample [{}] t={:.2f} s: horizontal {:.3f} m, height {:.3f} m, boom y {:+.4f}, pivot z {:+.4f}, root ({:+.3f},{:+.3f},{:+.3f}), player ({:+.3f},{:+.3f},{:+.3f}), offset now back {:.3f} up {:.3f}, writes {}", dump, Now() - t0, h, z, r.n1F8.t.y, r.n200.t.z,
						r.rootWorld.x, r.rootWorld.y, r.rootWorld.z, r.player.x, r.player.y, r.player.z, cc.backNow, cc.upNow, cc.writes);
				}
			}
			[[nodiscard]] float H() const { return n ? static_cast<float>(sumH / n) : -1.0f; }
			[[nodiscard]] float Z() const { return n ? static_cast<float>(sumZ / n) : -1.0f; }
			[[nodiscard]] float BoomY() const { return n ? static_cast<float>(sumBoomY / n) : 0.0f; }
			[[nodiscard]] float PivotZ() const { return n ? static_cast<float>(sumPivotZ / n) : 0.0f; }
			[[nodiscard]] std::string Text() const
			{
				return std::format("{} samples: horizontal {:.3f} m (range {:.3f}), height {:.3f} m (range {:.3f}), boom y {:+.3f}, pivot z {:+.3f}, collision factor min {:.3f}", n, H(), n ? maxH - minH : 0.0f, Z(), n ? maxZ - minZ : 0.0f, BoomY(), PivotZ(), n ? minF : 0.0f);
			}
		};

		void AddWindow(Seq& s, const std::shared_ptr<CamAcc>& a, const double a_seconds)
		{
			s.Do([a] { a->Begin(); });
			s.Until([a, a_seconds] {
				a->Step();
				return Now() - a->t0 >= a_seconds;
			}, a_seconds + 2.0);
		}

		struct R11Cycle
		{
			bool                      on{ false };
			std::shared_ptr<CamAcc>   before{ std::make_shared<CamAcc>() };
			std::shared_ptr<CamAcc>   cover{ std::make_shared<CamAcc>() };
			std::shared_ptr<CamAcc>   aim{ std::make_shared<CamAcc>() };
			std::shared_ptr<CamAcc>   after{ std::make_shared<CamAcc>() };
			std::shared_ptr<CaseData> enter{ std::make_shared<CaseData>() };
			bool                      inCover{ false };
		};

		// one pass at the low wall: standing before, in cover, aiming in cover, left, standing after; the cover camera on or off
		void AddR11Cycle(Seq& s, const std::shared_ptr<R11Cycle>& c, const bool a_shots)
		{
			AddLowPrepare(s, 0);
			s.Do([c] {
				Config::Mutable().coverTrigger = 1;
				Config::Mutable().coverCamera = c->on;
				Cover::ForceReset("p3 R11");
				REX::INFO("P3 R11 cycle: the cover camera is {}", c->on ? "ON" : "off");
			});
			s.Wait(1.2);
			AddWindow(s, c->before, 1.0);
			if (a_shots) {
				AddShot(s, "r11_before");
			}
			AddEnter(s, c->enter, Rig::kKeyW, Core::CoverKind::Low);
			s.Do([c] {
				Rig::ReleaseEverything("p3 R11");
				c->inCover = Cover::GetInfo().state == Core::CoverState::InCover;
			});
			s.Until([c] { return c->enter->tIn > 0.0 && Now() - c->enter->tIn >= 1.0; }, 3.0);
			s.Do([c, a_shots] { c->cover->dump = a_shots ? "cover on" : "cover off"; });
			AddWindow(s, c->cover, 2.0);
			if (a_shots) {
				AddShot(s, "r11_cover");
			}
			s.Do([] { Rig::MouseRight(true); });
			s.Wait(1.0);
			AddWindow(s, c->aim, 1.0);
			if (a_shots) {
				AddShot(s, "r11_aim");
			}
			s.Do([] { Rig::MouseRight(false); });
			s.Until([] { return Cover::GetInfo().state == Core::CoverState::InCover && Stance() == 1; }, 1.5);
			s.Wait(0.4);
			s.Do([] { Rig::Tap(Rig::kKeyLCtrl, 0.08); });  // leave
			s.Until([] { return Cover::GetInfo().state == Core::CoverState::None; }, 1.0);
			s.Until([] { return Stance() == 0; }, 1.5);
			s.Wait(1.0);
			AddWindow(s, c->after, 1.0);
			if (a_shots) {
				AddShot(s, "r11_after");
			}
		}

		// R11 (T5.3a, set 2 on Save30, Stride on): two cycles in the same launch, the camera offset off and on; the offset is what the second cycle shows beyond the first
		bool StageR11(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto off = std::make_shared<R11Cycle>();
				auto on = std::make_shared<R11Cycle>();
				off->on = false;
				on->on = true;
				const float backLow = Config::Get().coverCamBackLow;
				const float upLow = Config::Get().coverCamUpLow;
				const float share = Config::Get().coverCamAimShare;
				AddR11Cycle(s, off, false);
				s.Do([off] {
					const auto cc = Camera::GetCoverCam();
					Result(std::format("P3 R11 cycle OFF (bCoverCamera 0): in cover {}; before: {} | in cover: {} | aiming: {} | after: {}; the cover camera wrote the nodes {} times", off->inCover ? "yes" : "NO", off->before->Text(), off->cover->Text(), off->aim->Text(), off->after->Text(), cc.writes));
				});
				AddR11Cycle(s, on, true);
				s.Do([off, on, backLow, upLow, share] {
					const auto cc = Camera::GetCoverCam();
					Result(std::format("P3 R11 cycle ON  (bCoverCamera 1, back {:.2f} up {:.2f} aim share {:.2f}): in cover {}; before: {} | in cover: {} | aiming: {} | after: {}; the cover camera wrote the nodes {} times, took the offset out of an unrewritten value {} times, left a changed node alone {} times, faulted {}",
						backLow, upLow, share, on->inCover ? "yes" : "NO", on->before->Text(), on->cover->Text(), on->aim->Text(), on->after->Text(), cc.writes, cc.restored, cc.leftAlone, cc.faulted));
					const float dH = on->cover->H() - off->cover->H();
					const float dZ = on->cover->Z() - off->cover->Z();
					const float aH = on->aim->H() - off->aim->H();
					const float aZ = on->aim->Z() - off->aim->Z();
					constexpr float kWantUp = 0.35f;  // the task's figure for the camera root's rise (the pivot's rise is fCoverCamUpLow; the boom is pitched, so pulling it back raises the root as well)
					const bool  grow = on->inCover && off->inCover && std::fabs(dH - backLow) <= 0.15f && std::fabs(dZ - kWantUp) <= 0.15f;
					Result(std::format("P3 R11a in cover, 1 s after InCover (fCoverCamBackLow {:.2f}, fCoverCamUpLow {:.2f}): the camera root is {:+.3f} m farther (horizontal) and {:+.3f} m higher than with the offset off (expected {:+.2f} and {:+.2f}, within 0.15) -> {}", backLow, upLow, dH, dZ, backLow, kWantUp, Verdict(grow)));
					Summarise("R11a the camera grows by back and up in cover", grow);
					const float rH = on->cover->maxH - on->cover->minH;
					const float rZ = on->cover->maxZ - on->cover->minZ;
					const bool  steady = on->cover->n > 10 && rH <= 0.03f && rZ <= 0.03f;
					Result(std::format("P3 R11b steady over 2 s in cover: horizontal range {:.3f} m, height range {:.3f} m (limit 0.03) -> {}", rH, rZ, Verdict(steady)));
					Summarise("R11b steady within 0.03 m over 2 s", steady);
					const float aimH = backLow * share;
					const float aimZ = kWantUp * share;
					const bool  aimOk = std::fabs(aH - aimH) <= 0.15f && std::fabs(aZ - aimZ) <= 0.15f;
					Result(std::format("P3 R11c aiming in cover (share {:.2f}): {:+.3f} m farther and {:+.3f} m higher than with the offset off (expected {:+.3f} and {:+.3f}, within 0.15; the boom nodes' own change: {:+.3f} y, the pivot's {:+.3f} z) -> {}", share, aH, aZ, aimH, aimZ, on->aim->BoomY() - off->aim->BoomY(), on->aim->PivotZ() - off->aim->PivotZ(), Verdict(aimOk)));
					Summarise("R11c aiming: the offset times the aim share", aimOk);
					const float bH = on->after->H() - on->before->H();
					const float bZ = on->after->Z() - on->before->Z();
					const bool  back = std::fabs(bH) <= 0.05f && std::fabs(bZ) <= 0.05f;
					Result(std::format("P3 R11d 1 s after leaving: the camera root is {:+.3f} m (horizontal) and {:+.3f} m (height) from where it was before the cover (limit 0.05) -> {}", bH, bZ, Verdict(back)));
					Summarise("R11d the camera is back after leaving", back);
					const bool open = on->after->n > 0 && on->after->minF >= 0.99f && on->before->minF >= 0.99f;
					Result(std::format("P3 R11e the engine's collision factor in the open: before {:.3f}, after {:.3f} (expected 1.0) -> {}", on->before->minF, on->after->minF, Verdict(open)));
					Summarise("R11e collision factor 1.0 in the open", open);
					Config::Mutable().coverCamera = false;
					Cover::ForceReset("p3 R11 end");
				});
				s.Wait(0.5);
			}
			return Run(s, c.now);
		}

		struct R12Data
		{
			StanceWatch               w;
			std::shared_ptr<CaseData> enter{ std::make_shared<CaseData>() };
			double                    leaveT{ 0.0 };
			double                    tNone{ -1.0 };
			bool                      peeking{ false };
			int                       stanceAim{ -1 };
			int                       stanceBefore{ -1 };
			std::uint32_t             presses0{ 0 };
		};

		// one T3.8 case: the cover is entered with the stance a_before, the aim held (peek over: standing), B pressed while peeking, the aim let go 0.5 s later: the stance must end as it was
		void AddR12Case(Seq& s, const int a_before, const char* a_tag)
		{
			auto d = std::make_shared<R12Data>();
			AddLowPrepare(s, a_before);
			s.Do([d] {
				Config::Mutable().coverTrigger = 1;
				Config::Mutable().coverCamera = false;
				Cover::ForceReset("p3 R12");
				d->stanceBefore = Stance();
			});
			AddEnter(s, d->enter, Rig::kKeyW, Core::CoverKind::Low);
			s.Do([d] {
				Rig::ReleaseEverything("p3 R12");
				d->presses0 = Cover::GetInfo().presses;
			});
			s.Wait(0.6);
			s.Do([] { Rig::MouseRight(true); });
			s.Until([] { return Cover::GetInfo().state == Core::CoverState::Peeking; }, 1.0, [d](bool ok) { d->peeking = ok; });
			s.Until([] { return Stance() == 0; }, 1.0);
			s.Wait(0.4);
			s.Do([d] {
				d->stanceAim = Stance();
				d->w = StanceWatch{};
				d->w.last = Stance();
				d->tNone = -1.0;
				d->leaveT = Now();
				Rig::Tap(Rig::kKeyLCtrl, 0.08);  // B while peeking
			});
			s.Until([d] {
				WatchStep(d->w);
				if (d->tNone < 0.0 && Cover::GetInfo().state == Core::CoverState::None) {
					d->tNone = Now();
				}
				return Now() - d->leaveT >= 0.5;
			}, 2.0);
			s.Do([] { Rig::MouseRight(false); });  // the aim let go after the leave, as in the play
			s.Until([d] {
				WatchStep(d->w);
				return Now() - d->leaveT >= 3.0;
			}, 4.0);
			s.Do([d, a_before, a_tag] {
				const auto i = Cover::GetInfo();
				const int  expectChanges = a_before == 0 ? 0 : 1;
				const bool ok = d->peeking && d->stanceAim == 0 && d->tNone > 0.0 && i.state == Core::CoverState::None && Stance() == a_before && d->w.changes == expectChanges;
				Result(std::format("P3 R12{} T3.8: entered with the stance {}, aim held: Peeking {}, stance while aiming {} (expected 0); B pressed while peeking: state None {:.2f} s after B; the aim let go 0.5 s after B; stance changes from B to 3 s later: {} (expected {}), the stance now {} (expected {}), state {} -> {}",
					a_tag, d->stanceBefore, d->peeking ? "yes" : "NO", d->stanceAim, d->tNone > 0.0 ? d->tNone - d->leaveT : -1.0, d->w.changes, expectChanges, Stance(), a_before, Core::ToString(i.state), Verdict(ok)));
				Summarise(std::string("R12") + a_tag + " B while peeking over: the stance as before the cover", ok);
				Rig::ReleaseEverything("p3 R12");
			});
			s.Wait(0.4);
		}

		bool StageR12(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				AddR12Case(s, 0, "a (entered standing)");
				AddR12Case(s, 1, "b (entered crouched)");
				s.Do([] { Cover::ForceReset("p3 R12 end"); });
			}
			return Run(s, c.now);
		}

		// ======================================== M1d: T5.2 the cover poses, stage A (sets 6 and 7: bCoverPoses 1 on the low wall of Save30 and the tall wall of Save29) =======
		bool g_tall{ false };      // card M4b (sets 41, 42): the pose stages run at the tall billboard column instead of the low wall
		bool g_autoTall{ false };  // ... and enter it by the automatic entry (mode 3) instead of the press
		struct Spot { float x{ 0 }, y{ 0 }, z{ 0 }, h{ 0 }; bool set{ false }; };
		Spot g_spot;               // card M4b: a place found by the ring search, used instead of the column
		Spot g_leftSpot;
		[[nodiscard]] bool PoseTallFacing() { return g_tall || Config::Get().p3Set == 13 || Config::Get().p3Set == 27 || Config::Get().p3Set == 28 || Config::Get().p3Set == 29 || Config::Get().p3Set == 31 || Config::Get().p3Set == 33 || Config::Get().p3Set == 34; }  // the tall wall of Save29 with the camera turned toward the wall (the W key goes into it)
		[[nodiscard]] bool PoseLow() { return !g_tall && Config::Get().p3Set != 7 && Config::Get().p3Set != 9 && Config::Get().p3Set != 12 && Config::Get().p3Set != 13 && Config::Get().p3Set != 27 && Config::Get().p3Set != 28 && Config::Get().p3Set != 29 && Config::Get().p3Set != 31 && Config::Get().p3Set != 33 && Config::Get().p3Set != 34; }
		[[nodiscard]] int  PoseCycles() { return Config::Get().p3Set == 8 ? 5 : 10; }
		[[nodiscard]] Rig::Key PoseToward() { return (PoseLow() || PoseTallFacing()) ? Rig::kKeyW : Rig::kKeyS; }
		[[nodiscard]] Rig::Key PoseAway() { return (PoseLow() || PoseTallFacing()) ? Rig::kKeyS : Rig::kKeyW; }
		// along the wall to the actor's right (+tangent) when facing it: with the camera turned to the wall (low wall) that is the D key, with the camera turned away (the tall wall of Save29) the A key
		[[nodiscard]] Rig::Key PoseRightKey() { return (PoseLow() || PoseTallFacing()) ? Rig::kKeyD : Rig::kKeyA; }
		[[nodiscard]] Rig::Key PoseLeftKey() { return (PoseLow() || PoseTallFacing()) ? Rig::kKeyA : Rig::kKeyD; }
		[[nodiscard]] double   VarOr(const char* a_name, const double a_missing)
		{
			const auto v = PlayerControl::ReadVar(a_name);
			return v ? *v : a_missing;
		}
		// the camera boom node's local y: about -1.9 on the own graph, -0.52 on the NPC graph (REFERENCE 21.2); read with the camera hold off
		[[nodiscard]] bool OwnGraph()
		{
			const auto r = Camera::ReadRig();
			return r.ok && r.n1F8.present && r.n1F8.t.y < -0.75f;
		}

		std::string g_yawLabel;  // M3h: while not empty the pose entry starts the yaw probe at the push

		struct PoseRun
		{
			double       tPush{ 0.0 }, tState{ -1.0 }, tPosed{ -1.0 }, tLeave{ -1.0 }, tNone{ -1.0 }, tOff{ -1.0 };
			int          sessions0{ 0 };
			RE::NiPoint3 posBefore{};
			bool         camera{ false };
			// walking
			std::vector<std::pair<double, RE::NiPoint3>> trace;
			float        minPlus{ 1e9f }, minMinus{ 1e9f };
			double       t0{ 0.0 };
			RE::NiPoint3 p0{};
			// aim
			double       tRelease{ -1.0 }, tCleared{ -1.0 };
			// camera
			std::shared_ptr<CamAcc> before{ std::make_shared<CamAcc>() }, cover{ std::make_shared<CamAcc>() }, aim{ std::make_shared<CamAcc>() }, after{ std::make_shared<CamAcc>() };
			std::vector<std::array<float, 3>> samples;  // t, horizontal, height
		};

		void AddPoseEnter(Seq& s, const std::shared_ptr<PoseRun>& r, const int a_stanceBefore, const bool a_camera)
		{
			if (PoseLow()) {
				AddLowPrepare(s, a_stanceBefore);
			} else {
				if (g_spot.set) {
					AddPrepare(s, a_stanceBefore, { g_spot.x, g_spot.y, g_spot.z }, g_spot.h);
				} else if (Config::Get().p3Set == 28 || g_tall) {
					AddPrepare(s, a_stanceBefore, { -538.3f, 2045.6f, 3.42f }, 196.0f);  // the billboard column of Save32 (card M3e: the stand point (-538.64, 2043.91), the wall toward heading 196 degrees)
				} else {
					AddPrepare(s, a_stanceBefore, Base(), PoseTallFacing() ? std::optional<float>((Config::Get().p3Set == 29 || Config::Get().p3Set == 31 || Config::Get().p3Set == 33 || Config::Get().p3Set == 34) ? 102.0f : 90.0f) : std::nullopt);
				}
			}
			s.Do([r, a_camera] {
				Config::Mutable().coverPoses = true;
				Config::Mutable().coverCamera = a_camera;
				Config::Mutable().coverTrigger = (PoseLow() || g_autoTall) ? 3 : 1;
				Cover::ForceReset("p3 pose enter");
				r->tState = r->tPosed = r->tLeave = r->tNone = r->tOff = -1.0;
				r->sessions0 = CoverPose::GetInfo().sessions;
				r->tPush = Now();
				if (!g_yawLabel.empty()) { Camera::YawProbeBegin(g_yawLabel.c_str()); }
				Rig::Tap(PoseToward(), PoseLow() ? 9.0 : (g_autoTall ? 6.0 : 1.7));
			});
			if (!PoseLow() && !g_autoTall) {
				s.Until([] { return FreshCandidate(Core::CoverKind::High); }, 1.5, [r](bool ok) {
					if (ok) {
						r->tPush = Now();
						Rig::Tap(Rig::kKeyLCtrl, 0.08);
					}
				});
			}
			s.Until([r] {
				if (r->tState < 0.0 && Cover::GetInfo().state != Core::CoverState::None) {
					r->tState = Now();
				}
				return r->tState > 0.0;
			}, 5.0);
			s.Until([r] {
				const auto i = CoverPose::GetInfo();
				return i.state == CoverPose::State::Posed && i.sessions > r->sessions0;
			}, 5.0, [r](bool ok) { r->tPosed = ok ? Now() : -1.0; });
			s.Do([] { Rig::ReleaseEverything("p3 pose enter"); });
			s.Wait(0.5);
		}

		void AddPoseLeaveSneak(Seq& s, const std::shared_ptr<PoseRun>& r)
		{
			s.Do([r] {
				r->tNone = r->tOff = -1.0;
				r->posBefore = Snap().position;
				r->tLeave = Now();
				Rig::Tap(Rig::kKeyLCtrl, 0.08);
			});
			s.Until([] { return Cover::GetInfo().state == Core::CoverState::None; }, 2.0, [r](bool ok) { r->tNone = ok ? Now() : -1.0; });
			s.Until([] { return CoverPose::GetInfo().state == CoverPose::State::Off; }, 4.0, [r](bool ok) { r->tOff = ok ? Now() : -1.0; });
			s.Wait(0.8);
		}

		[[nodiscard]] std::string PoseProof()
		{
			bool       ret{ true };
			const bool ran = CoverPose::ProbeStanceCall(ret);
			return std::format("stance call on this graph: ran {}, returned {}; camera boom node {} ; stance field {}", ran, ret, OwnGraph() ? "reads the own graph" : "reads the NPC graph", CoverPose::StanceField());
		}

		// ---- in: the switch into the pose ----------------------------------------------------------------------------------------------
		bool StagePoseIn(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				AddPoseEnter(s, r, 0, false);
				s.Do([r] {
					const auto i = CoverPose::GetInfo();
					const auto ci = Cover::GetInfo();
					const auto& sp = ci.candidate.standPoint;
					const float dist = std::hypot(Snap().position.x - sp.x, Snap().position.y - sp.y);
					const double inCover = VarOr("bIsInCover", -1.0);
					const int    field = CoverPose::StanceField();
					const bool   ok = i.state == CoverPose::State::Posed && inCover > 0.5 && field == (PoseLow() ? 3 : 5) && dist <= 0.05f && i.lastCallReturned;
					Result(std::format("P3 POSE in ({} wall): Phase 3 state {:.2f} s after the push; the pose began {:.3f} s later; the NPC graph showed {:.3f} s after that (archetype cleared at {:.3f}); the stance call (stance {}, side {}, returned {}, {} call(s)) {:.3f} s after the archetype was cleared; bIsInCover 1 {:.3f} s after the stance call, {:.3f} s after the archetype was cleared, {:.3f} s after Phase 3 reached InCover; bIsInCover {:g}, the stance field {} (expected {}), the player {:.3f} m from the stand point ({:.2f},{:.2f}) (limit 0.05), state {} -> {}",
						PoseLow() ? "low" : "tall", r->tState > 0.0 ? r->tState - r->tPush : -1.0, i.tBegin - r->tState, i.tGraphReady - i.tBegin, i.tBegin, i.lastStance, i.lastSide ? "Left" : "Right", i.lastCallReturned, i.stanceCalls, i.tCall - i.tBegin,
						i.tPosed - i.tCall, i.tPosed - i.tBegin, i.tPosed - r->tState, inCover, field, PoseLow() ? 3 : 5, dist, sp.x, sp.y, CoverPose::ToString(i.state), Verdict(ok)));
					Summarise(PoseLow() ? "POSE in at the low wall" : "POSE in at the tall wall", ok);
					REX::INFO("{}", Camera::DescribeRig("pose in", Camera::ReadRig()));
				});
				AddShot(s, PoseLow() ? "pose_in" : "pose_in_tall");
				// 3 s idle: does the cover hold? (A2: the keyword is back in mode 2)
				{
					auto idle = std::make_shared<std::array<double, 4>>();  // t0, next, minInCover, minStance
					s.Do([idle] { (*idle) = { Now(), 0.0, 9.0, 9.0 }; });
					s.Until([idle] {
						const double now = Now();
						if (now >= (*idle)[1]) {
							(*idle)[1] = now + 0.1;
							const double inCover = VarOr("bIsInCover", -1.0);
							const int    field = CoverPose::StanceField();
							(*idle)[2] = (std::min)((*idle)[2], inCover);
							(*idle)[3] = (std::min)((*idle)[3], static_cast<double>(field));
							const auto rig = Camera::ReadRig();
							REX::INFO("P3 POSE idle +{:.1f} s: bIsInCover {:g}, the stance field {}, pending byte {}, boom y {:+.4f}, state {} / pose {}", now - (*idle)[0], inCover, field, CoverPose::PendingByte(), rig.ok ? rig.n1F8.t.y : 0.0f, Core::ToString(Cover::GetInfo().state),
								CoverPose::ToString(CoverPose::GetInfo().state));
						}
						return now - (*idle)[0] >= 3.0;
					}, 5.0);
					s.Do([idle] {
						const bool ok = (*idle)[2] > 0.5 && (*idle)[3] >= 2.0 && CoverPose::GetInfo().state == CoverPose::State::Posed;
						Result(std::format("P3 POSE idle 3 s (archetype mode {}): bIsInCover never below {:g}, the stance field never below {:g}, pending byte {}, pose {} -> {}", Config::Get().poseArchetypeMode, (*idle)[2], (*idle)[3], CoverPose::PendingByte(), CoverPose::ToString(CoverPose::GetInfo().state), Verdict(ok)));
						Summarise("POSE idle: the cover holds for 3 s", ok);
					});
				}
				s.Wait(0.3);
			}
			return Run(s, c.now);
		}

		// ---- walk: the game's own key along the wall, both ways --------------------------------------------------------------------------------
		void AddWalkLeg(Seq& s, const std::shared_ptr<PoseRun>& r, const Rig::Key a_key, const char* a_name, const bool a_expectLeft, const char* a_shot)
		{
			s.Do([r, a_key, a_name] {
				r->trace.clear();
				r->minPlus = r->minMinus = 1e9f;
				r->t0 = Now();
				r->p0 = Snap().position;
				Rig::Tap(a_key, 8.0);
				REX::INFO("P3 POSE walk [{}]: key held from ({:.3f},{:.3f}); side flag {:g}", a_name, r->p0.x, r->p0.y, VarOr("iSyncRightLeft", -1.0));
			});
			s.Until([r, a_expectLeft] {
				const double now = Now();
				if (r->trace.empty() || now - r->trace.back().first >= 0.05) {
					r->trace.push_back({ now, Snap().position });
					const auto i = Cover::GetInfo();
					r->minPlus = (std::min)(r->minPlus, i.remainPlus);
					r->minMinus = (std::min)(r->minMinus, i.remainMinus);
				}
				if (now - r->t0 < 1.0 || r->trace.size() < 14) {
					return false;
				}
				const auto& a = r->trace[r->trace.size() - 13].second;
				const auto& b = r->trace.back().second;
				const bool  moved = Dist2(r->p0, b) >= 0.3f;
				// M3a: the key is let go 0.7 m of room before the end: the wall's ends are low and a track answer there (0.57 m high at the left end of Save30's wall) ends the cover, which says nothing about the pose
				const float room = a_expectLeft ? r->minMinus : r->minPlus;
				const bool  nearEnd = moved && room < 0.70f;
				if (nearEnd) {
					Rig::ReleaseEverything("p3 pose walk, near the end of the wall");  // at once: the picture hand-off after this wait would let the player walk on
				}
				return (moved && Dist2(a, b) < 0.03f) || nearEnd || now - r->t0 > 7.5;
			}, 9.0);
			if (a_shot && *a_shot) {
				AddShot(s, a_shot);
			}
			s.Do([r, a_name, a_expectLeft] {
				Rig::ReleaseEverything("p3 pose walk");
				float maxSpeed = 0.0f;
				for (std::size_t k = 5; k < r->trace.size(); ++k) {
					const double dt = r->trace[k].first - r->trace[k - 5].first;
					if (dt > 0.1) {
						maxSpeed = (std::max)(maxSpeed, static_cast<float>(Dist2(r->trace[k].second, r->trace[k - 5].second) / dt));
					}
				}
				double tMove = -1.0;
				for (const auto& [t, p] : r->trace) {
					if (Dist2(p, r->p0) > 0.05f) {
						tMove = t;
						break;
					}
				}
				const auto  i = Cover::GetInfo();
				const float minRemain = a_expectLeft ? r->minMinus : r->minPlus;
				const double side = VarOr("iSyncRightLeft", -1.0);
				const bool ok = i.state == Core::CoverState::InCover && CoverPose::GetInfo().state == CoverPose::State::Posed && minRemain >= -0.10f && maxSpeed > 0.3f && std::lround(side) == (a_expectLeft ? 1 : 0);
				Result(std::format("P3 POSE walk {} (the game's own key): the player started moving {:.2f} s after the key, moved {:.2f} m to ({:.2f},{:.2f}), top speed {:.2f} m/s (0.25 s windows), the room left to the Phase 3 stop point went down to {:+.3f} m (limit -0.10), side flag iSyncRightLeft {:g} (expected {}), Direction {:g}, Speed {:g}, iSyncIdleLocomotion {:g}, state {} / pose {} -> {}",
					a_name, tMove > 0.0 ? tMove - r->t0 : -1.0, Dist2(r->p0, Snap().position), Snap().position.x, Snap().position.y, maxSpeed, minRemain, side, a_expectLeft ? 1 : 0, VarOr("Direction", -1.0), VarOr("Speed", -1.0), VarOr("iSyncIdleLocomotion", -1.0),
					Core::ToString(i.state), CoverPose::ToString(CoverPose::GetInfo().state), Verdict(ok)));
				Summarise(std::string("POSE walk ") + a_name, ok);
			});
			s.Wait(0.8);
		}

		bool StagePoseWalk(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				AddWalkLeg(s, r, PoseRightKey(), PoseLow() ? "D (to the right)" : "A (to the right, the camera looks away from the wall)", false, PoseLow() ? "pose_walk_d" : "pose_walk_d_tall");
				AddWalkLeg(s, r, PoseLeftKey(), PoseLow() ? "A (to the left, turning round first)" : "D (to the left, turning round first)", true, PoseLow() ? "pose_walk_a" : "pose_walk_a_tall");
			}
			return Run(s, c.now);
		}

		// ---- aim: the game's own aim input, one shot, the release ------------------------------------------------------------------------------
		bool StagePoseAim(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				auto aimT0 = std::make_shared<double>(0.0);
				s.Do([aimT0] { *aimT0 = Now(); Rig::MouseRight(true); });
				{
					auto next = std::make_shared<double>(0.0);
					s.Until([aimT0, next] {
						const double now = Now();
						if (now >= *next) {
							*next = now + 0.05;
							REX::INFO("P3 POSE aim trace +{:.2f} s: bIsInCover {:g}, stance field {}, pending byte {}, iSyncCoverStates {:g}, iIsSighted {:g}, iSyncSighted {:g}, boom y {:+.4f}, state {} / pose {}", now - *aimT0, VarOr("bIsInCover", -1.0), CoverPose::StanceField(), CoverPose::PendingByte(), VarOr("iSyncCoverStates", -1.0), VarOr("iIsSighted", -1.0), VarOr("iSyncSighted", -1.0),
								Camera::ReadRig().n1F8.t.y, Core::ToString(Cover::GetInfo().state), CoverPose::ToString(CoverPose::GetInfo().state));
						}
						return now - *aimT0 >= 1.5;
					}, 3.0);
				}
				s.Do([] {
					Result(std::format("P3 POSE aim held 1.5 s: iSyncCoverStates {:g}, iIsSighted {:g}, iSyncSighted {:g}, iIsBlindFiring {:g}, iSyncRightLeft {:g}, state {} / pose {}, the stance field {}", VarOr("iSyncCoverStates", -1.0), VarOr("iIsSighted", -1.0), VarOr("iSyncSighted", -1.0), VarOr("iIsBlindFiring", -1.0), VarOr("iSyncRightLeft", -1.0),
						Core::ToString(Cover::GetInfo().state), CoverPose::ToString(CoverPose::GetInfo().state), CoverPose::StanceField()));
				});
				AddShot(s, PoseLow() ? "pose_aim" : "pose_aim_tall");
				s.Do([] { REX::INFO("P3 POSE aim: one shot (left mouse button tapped)"); Rig::MouseLeft(true); });
				s.Wait(0.12);
				s.Do([] { Rig::MouseLeft(false); });
				s.Wait(0.5);
				AddShot(s, PoseLow() ? "pose_aim_fired" : "pose_aim_fired_tall");
				s.Do([r] {
					r->tRelease = Now();
					r->tCleared = -1.0;
					Rig::MouseRight(false);
				});
				s.Until([] { return VarOr("iIsSighted", 1.0) < 0.5 && VarOr("iSyncSighted", 1.0) < 0.5; }, 4.0, [r](bool ok) { r->tCleared = ok ? Now() : -1.0; });
				s.Do([r] {
					const bool ok = r->tCleared > 0.0 && Cover::GetInfo().state == Core::CoverState::InCover && CoverPose::GetInfo().state == CoverPose::State::Posed;
					Result(std::format("P3 POSE aim released: iIsSighted and iSyncSighted clear {:.2f} s after the release (the Out clip is 1.2 to 2.2 s), iSyncCoverStates {:g}, state {} / pose {} -> {}", r->tCleared > 0.0 ? r->tCleared - r->tRelease : -1.0, VarOr("iSyncCoverStates", -1.0),
						Core::ToString(Cover::GetInfo().state), CoverPose::ToString(CoverPose::GetInfo().state), Verdict(ok)));
					Summarise("POSE aim: the flags clear on release", ok);
				});
				s.Wait(0.5);
			}
			return Run(s, c.now);
		}

		// ---- out: pulling away, Sneak, Sneak with the crouch before ------------------------------------------------------------------------------
		void AddOutResult(Seq& s, const std::shared_ptr<PoseRun>& r, const char* a_name, const int a_stanceExpected)
		{
			s.Do([r, a_name, a_stanceExpected] {
				const auto i = CoverPose::GetInfo();
				const float moved = Dist2(r->posBefore, Snap().position);
				const bool  ok = r->tNone > 0.0 && r->tOff > 0.0 && OwnGraph() && Stance() == a_stanceExpected;
				Result(std::format("P3 POSE out {}: Phase 3 state None {:.2f} s after the trigger; the own graph back {:.2f} s after the trigger ({:.2f} s after the exit function; {} holster fallback); the stance {} (expected {}); the player moved {:.3f} m from where it was when the trigger came; {} -> {}", a_name,
					r->tNone > 0.0 ? r->tNone - r->tLeave : -1.0, r->tOff > 0.0 ? r->tOff - r->tLeave : -1.0, i.tOwnBack - i.tLeave, i.usedHolsterFallback ? "with" : "no", Stance(), a_stanceExpected, moved, PoseProof(), Verdict(ok)));
				Summarise(std::string("POSE out ") + a_name, ok);
			});
		}

		bool StagePoseOut(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				// a: pulling away (the stage runs in the pose from the walk stage: the player is somewhere along the wall)
				s.Do([r] {
					r->tNone = r->tOff = -1.0;
					r->posBefore = Snap().position;
					r->tLeave = Now();
					Rig::Tap(PoseAway(), 6.0);
				});
				s.Until([] { return Cover::GetInfo().state == Core::CoverState::None; }, 3.0, [r](bool ok) { r->tNone = ok ? Now() : -1.0; });
				s.Until([] { return CoverPose::GetInfo().state == CoverPose::State::Off; }, 4.0, [r](bool ok) { r->tOff = ok ? Now() : -1.0; });
				s.Do([r] { r->p0 = Snap().position; r->t0 = Now(); });
				s.Wait(1.0);
				s.Do([r] {
					const float free = Dist2(r->p0, Snap().position);
					Result(std::format("P3 POSE out a (pulling away, the key held on): in the 1.0 s after the own graph was back the player walked {:.2f} m on its own graph (free movement expected: above 0.8 m) -> {}", free, Verdict(free > 0.8f)));
					Summarise("POSE out a: free movement after the own graph is back", free > 0.8f);
					Rig::ReleaseEverything("p3 pose out a");
				});
				s.Wait(1.2);
				s.Do([r] {
					const auto i = CoverPose::GetInfo();
					const bool ok = r->tNone > 0.0 && r->tOff > 0.0 && OwnGraph() && Stance() == 0;
					Result(std::format("P3 POSE out a (pulling away): Phase 3 state None {:.2f} s after the key; the own graph back {:.2f} s after the key ({:.2f} s after the exit function, {} holster fallback); the stance {} (expected 0); {} -> {}", r->tNone > 0.0 ? r->tNone - r->tLeave : -1.0, r->tOff > 0.0 ? r->tOff - r->tLeave : -1.0, i.tOwnBack - i.tLeave,
						i.usedHolsterFallback ? "with" : "no", Stance(), PoseProof(), Verdict(ok)));
					Summarise("POSE out by pulling away", ok);
				});
				// b: Sneak, entered standing
				AddPoseEnter(s, r, 0, false);
				AddPoseLeaveSneak(s, r);
				AddOutResult(s, r, "b (Sneak, entered standing)", 0);
				// c: Sneak, entered crouched: the stance comes back after the pose
				AddPoseEnter(s, r, 1, false);
				AddPoseLeaveSneak(s, r);
				s.Wait(0.8);
				AddOutResult(s, r, "c (Sneak, entered crouched)", 1);
				s.Do([] { Rig::ReleaseEverything("p3 pose out"); });
				s.Wait(0.3);
			}
			return Run(s, c.now);
		}

		// ---- ten ins and outs, and a hard reset (first person) from inside the pose ---------------------------------------------------------------------
		struct TenData
		{
			int    n{ 0 };
			int    posed{ 0 };
			int    own{ 0 };
			int    probeFalse{ 0 };
			double maxIn{ 0.0 }, maxOut{ 0.0 };
			double tPush{ 0.0 }, tLeave{ 0.0 }, tPosed{ -1.0 }, tOff{ -1.0 };
			int    sessions0{ 0 };
		};

		bool StagePoseTen(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto t = std::make_shared<TenData>();
				auto r = std::make_shared<PoseRun>();
				AddPoseEnter(s, r, 0, false);
				AddPoseLeaveSneak(s, r);  // the first pass is the warm-up of the next ones: leave, then ten full cycles
				for (int k = 0; k < PoseCycles(); ++k) {
					s.Do([t] {
						t->tPosed = t->tOff = -1.0;
						t->sessions0 = CoverPose::GetInfo().sessions;
						t->tPush = Now();
						Config::Mutable().coverTrigger = PoseLow() ? 3 : 1;
						Rig::Tap(PoseToward(), PoseLow() ? 6.0 : 1.7);
					});
					if (!PoseLow()) {
						s.Until([] { return FreshCandidate(Core::CoverKind::High); }, 1.5, [t](bool ok) {
							if (ok) {
								Rig::Tap(Rig::kKeyLCtrl, 0.08);
							}
						});
					}
					s.Until([t] {
						const auto i = CoverPose::GetInfo();
						return i.state == CoverPose::State::Posed && i.sessions > t->sessions0;
					}, 5.0, [t](bool ok) { t->tPosed = ok ? Now() : -1.0; });
					s.Do([] { Rig::ReleaseEverything("p3 pose ten"); });
					s.Wait(0.5);
					s.Do([t] { t->tLeave = Now(); Rig::Tap(Rig::kKeyLCtrl, 0.08); });
					s.Until([] { return CoverPose::GetInfo().state == CoverPose::State::Off && Cover::GetInfo().state == Core::CoverState::None; }, 5.0, [t](bool ok) { t->tOff = ok ? Now() : -1.0; });
					s.Wait(0.4);
					s.Do([t, k] {
						++t->n;
						bool       ret{ true };
						const bool ran = CoverPose::ProbeStanceCall(ret);
						const bool own = OwnGraph();
						t->posed += t->tPosed > 0.0 ? 1 : 0;
						t->own += (t->tOff > 0.0 && own) ? 1 : 0;
						t->probeFalse += (ran && !ret) ? 1 : 0;
						(void)ran;
						(void)ret;
						if (t->tPosed > 0.0) {
							t->maxIn = (std::max)(t->maxIn, t->tPosed - t->tPush);
						}
						if (t->tOff > 0.0) {
							t->maxOut = (std::max)(t->maxOut, t->tOff - t->tLeave);
						}
						REX::INFO("P3 POSE ten #{}: in the pose {:.2f} s after the push, the own graph back {:.2f} s after the leave, the stance call on the own graph: ran {} returned {}, camera boom node {}, stance {}", k + 1, t->tPosed > 0.0 ? t->tPosed - t->tPush : -1.0, t->tOff > 0.0 ? t->tOff - t->tLeave : -1.0, ran, ret, own ? "reads the own graph" : "STILL THE NPC GRAPH", Stance());
					});
					s.Wait(0.3);
				}
				s.Do([t] {
					const bool ok = t->n == PoseCycles() && t->posed == PoseCycles() && t->own == PoseCycles();
					Result(std::format("P3 POSE ten in and out: {} cycles, {} reached the pose (slowest {:.2f} s after the push), {} gave the own graph back (slowest {:.2f} s after the leave), the stance call made on the own graph afterwards returned false {} times (it returns true there now: no proof) -> {}", t->n, t->posed, t->maxIn, t->own, t->maxOut, t->probeFalse, Verdict(ok)));
					Summarise("POSE ten ins and outs", ok);
				});
				// a hard reset from inside the pose: first person
				s.Do([t] {
					t->tPosed = -1.0;
					t->sessions0 = CoverPose::GetInfo().sessions;
					Rig::Tap(PoseToward(), PoseLow() ? 6.0 : 1.7);
				});
				if (!PoseLow()) {
					s.Until([] { return FreshCandidate(Core::CoverKind::High); }, 1.5, [](bool ok) {
						if (ok) {
							Rig::Tap(Rig::kKeyLCtrl, 0.08);
						}
					});
				}
				s.Until([t] {
					const auto i = CoverPose::GetInfo();
					return i.state == CoverPose::State::Posed && i.sessions > t->sessions0;
				}, 5.0, [t](bool ok) { t->tPosed = ok ? Now() : -1.0; });
				s.Do([] { Rig::ReleaseEverything("p3 pose hard reset"); });
				s.Wait(0.6);
				s.Do([t] {
					t->tLeave = Now();
					t->tOff = -1.0;
					REX::INFO("P3 POSE hard reset: first person forced from inside the pose (state {}, pose {})", Core::ToString(Cover::GetInfo().state), CoverPose::ToString(CoverPose::GetInfo().state));
					(void)Camera::ForceFirstPerson();
				});
				s.Until([] { return CoverPose::GetInfo().state == CoverPose::State::Off; }, 4.0, [t](bool ok) { t->tOff = ok ? Now() : -1.0; });
				s.Wait(0.8);
				s.Do([t] {
					const auto i = CoverPose::GetInfo();
					const bool ok = t->tPosed > 0.0 && t->tOff > 0.0 && Cover::GetInfo().state == Core::CoverState::None;
					Result(std::format("P3 POSE hard reset (first person from inside the pose): the pose was over {:.3f} s after the call (the exit function and the own archetype asked for {:.3f} s after the call; the own graph is checked once the camera is third person again, next line); state {} -> {}", t->tOff > 0.0 ? t->tOff - t->tLeave : -1.0, i.tRestoreCall - t->tLeave,
						Core::ToString(Cover::GetInfo().state), Verdict(ok)));
					Summarise("POSE hard reset: the pose is over", ok);
					(void)Camera::ForceThirdPerson();
				});
				s.Wait(2.0);
				s.Do([] {
					const bool own = OwnGraph();
					Result(std::format("P3 POSE hard reset, back in third person: the camera boom node reads {} -> {}", own ? "the own graph" : "the NPC graph", Verdict(own)));
					Summarise("POSE hard reset: the own graph back (read in third person)", own);
				});
				s.Do([] { Cover::ForceReset("p3 pose ten end"); });
			}
			return Run(s, c.now);
		}

		// ---- the camera in the pose: the R11 numbers, every sample logged ------------------------------------------------------------------------------------
		void AddPoseCamWindow(Seq& s, const std::shared_ptr<PoseRun>& r, const std::shared_ptr<CamAcc>& a, const double a_seconds, const char* a_label)
		{
			s.Do([a, a_label] {
				a->Begin();
				a->dump = a_label;
			});
			s.Until([a, a_seconds] {
				a->Step();
				return Now() - a->t0 >= a_seconds;
			}, a_seconds + 2.0);
			s.Do([a] { a->dump = nullptr; });
			(void)r;
		}

		void AddPoseCamCycle(Seq& s, const std::shared_ptr<PoseRun>& r, const bool a_on, const bool a_shots)
		{
			AddPoseEnter(s, r, 0, a_on);
			s.Until([r] { return r->tPosed > 0.0 && Now() - r->tPosed >= 1.0; }, 3.0);
			AddPoseCamWindow(s, r, r->cover, 2.0, a_on ? "pose on" : "pose off");
			if (a_shots) {
				AddShot(s, PoseLow() ? "pose_cam_on" : "pose_cam_on_tall");
			}
			s.Do([] { Rig::MouseRight(true); });
			s.Wait(1.0);
			AddPoseCamWindow(s, r, r->aim, 1.0, a_on ? "pose aim on" : "pose aim off");
			s.Do([] { Rig::MouseRight(false); });
			s.Wait(2.5);
			AddPoseLeaveSneak(s, r);
			s.Wait(1.0);
			AddPoseCamWindow(s, r, r->after, 1.0, nullptr);
		}

		bool StagePoseCam(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto off = std::make_shared<PoseRun>();
				auto on = std::make_shared<PoseRun>();
				const float backLow = PoseLow() ? Config::Get().coverCamBackLow : Config::Get().coverCamBackTall;
				// the standing view before any cover: a window with the own graph and no cover
				s.Do([off] { Config::Mutable().coverPoses = false; Cover::ForceReset("p3 pose cam"); });
				if (PoseLow()) {
					AddLowPrepare(s, 0);
				} else {
					AddPrepare(s, 0, Base(), std::nullopt);
				}
				s.Wait(1.0);
				AddPoseCamWindow(s, off, off->before, 1.0, nullptr);
				AddPoseCamCycle(s, off, false, false);
				AddPoseCamCycle(s, on, true, true);
				s.Do([off, on, backLow] {
					const float dH = on->cover->H() - off->cover->H();
					const float dZ = on->cover->Z() - off->cover->Z();
					Result(std::format("P3 POSE cam, in the pose with the offset off: {} | with it on: {} | standing before (own graph): {}", off->cover->Text(), on->cover->Text(), off->before->Text()));
					const bool grow = PoseLow() ? (std::fabs(dH - 0.82f) <= 0.15f && std::fabs(dZ - 0.58f) <= 0.15f) : (dH >= 0.3f && dZ >= 0.0f);
					Result(std::format("P3 POSE cam R11a in the pose: the camera root is {:+.3f} m farther and {:+.3f} m higher with the offset on (R11 on the own graph: +0.818 and +0.580, within 0.15; fCoverCamBack {:.2f}) -> {}", dH, dZ, backLow, Verdict(grow)));
					Summarise("POSE cam: the offset moves the camera as in R11", grow);
					// every sample of the on window is in the log ("P3 R11 sample [pose on]"): the shake is the largest step from one sample to the next, the drift the change from the first half to the second
					const bool steady = on->cover->n > 10 && on->cover->maxH - on->cover->minH <= 0.03f && on->cover->maxZ - on->cover->minZ <= 0.03f;
					Result(std::format("P3 POSE cam R11b steady over 2 s in the pose: horizontal range {:.3f} m, height range {:.3f} m (limit 0.03; the offset-off window: {:.3f} and {:.3f}) -> {}", on->cover->maxH - on->cover->minH, on->cover->maxZ - on->cover->minZ, off->cover->maxH - off->cover->minH, off->cover->maxZ - off->cover->minZ, Verdict(steady)));
					Summarise("POSE cam: steady within 0.03 m", steady);
					const float bH = on->after->H() - off->before->H();
					const float bZ = on->after->Z() - off->before->Z();
					const bool  back = std::fabs(bH) <= 0.05f && std::fabs(bZ) <= 0.05f;
					Result(std::format("P3 POSE cam R11d after leaving the pose: {:+.3f} m horizontal and {:+.3f} m height from the standing view before the cover (limit 0.05) -> {}", bH, bZ, Verdict(back)));
					Summarise("POSE cam: the view is back after leaving", back);
					Config::Mutable().coverCamera = false;
				});
				s.Wait(0.5);
			}
			return Run(s, c.now);
		}

		// ======================================== M1f: stage A3, what drops the cover on the first aim press (sets 10, 11 and 12) ====================================================
		void A3Line(const char* a_tag, const double a_t)
		{
			const auto ci = Cover::GetInfo();
			REX::INFO("P3 A3 [{}] +{:.2f} s: bIsInCover {:g}, stance field {}, iIsSighted {:g}, iSyncSighted {:g}, iSyncCoverStates {:g}, iSyncWeaponDrawState {:g}, iSyncRelaxedReadyAlert {:g}, {}, state {}, peek {}, filter {}, position writes {} (+{} blocked), made-up Sneak presses {}, s_aimHeld {}, pose {}",
				a_tag, a_t, VarOr("bIsInCover", -1.0), CoverPose::StanceField(), VarOr("iIsSighted", -1.0), VarOr("iSyncSighted", -1.0), VarOr("iSyncCoverStates", -1.0), VarOr("iSyncWeaponDrawState", -1.0), VarOr("iSyncRelaxedReadyAlert", -1.0), PlayerControl::ActorFlags(),
				Core::ToString(ci.state), Core::ToString(ci.peek), ci.filterMode == 2 ? "wall" : (ci.filterMode == 1 ? "zero" : "off"), ci.positionWrites, ci.positionWritesBlocked, Input::MadeUpPressStats().requested, ci.aimHeldSeen, CoverPose::ToString(CoverPose::GetInfo().state));
		}

		struct A3Data
		{
			double        t0{ 0.0 }, next{ 0.0 }, drop{ -1.0 };
			double        minIn{ 9.0 };
			std::uint32_t writes0{ 0 }, blocked0{ 0 }, sneak0{ 0 };
			double        coverStates{ -1.0 };
			double        tShot{ -1.0 };
			bool          shot{ false };
			double        tRelease{ -1.0 }, tCleared{ -1.0 };
		};

		struct A3Variant
		{
			const char* name;
			int         mask;
			bool        cam;
		};

		void A3Begin(const std::shared_ptr<A3Data>& d)
		{
			*d = A3Data{};
			d->t0 = Now();
			const auto ci = Cover::GetInfo();
			d->writes0 = ci.positionWrites;
			d->blocked0 = ci.positionWritesBlocked;
			d->sneak0 = Input::MadeUpPressStats().requested;
			Rig::MouseRight(true);
		}

		// samples every 0.02 s; lines (each sample) for the first a_lineSeconds; true when a_seconds have passed
		bool A3Step(const std::shared_ptr<A3Data>& d, const char* a_tag, const double a_lineSeconds, const double a_seconds)
		{
			const double now = Now();
			if (now >= d->next) {
				d->next = now + 0.02;
				const double in = VarOr("bIsInCover", -1.0);
				d->minIn = (std::min)(d->minIn, in);
				if (in < 0.5 && d->drop < 0.0) {
					d->drop = now - d->t0;
				}
				const double cs = VarOr("iSyncCoverStates", -1.0);
				if (cs >= 0.0) {
					d->coverStates = cs;
				}
				if (now - d->t0 <= a_lineSeconds) {
					A3Line(a_tag, now - d->t0);
				}
			}
			return now - d->t0 >= a_seconds;
		}

		bool StageA3Bisect(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				static const std::array<A3Variant, 7> kVariants{ {
					{ "a baseline (stage A2 mode 1)", 0, true },
					{ "b the aim is not given to Phase 3", 1, true },
					{ "c no position writes", 2, true },
					{ "d the movement filter off", 4, true },
					{ "e no crouch under the pose", 8, true },
					{ "f the cover camera offset off", 0, false },
					{ "g b to f together", 15, false },
				} };
				for (const auto& v : kVariants) {
					auto r = std::make_shared<PoseRun>();
					auto d = std::make_shared<A3Data>();
					const int  mask = v.mask;
					const char* name = v.name;
					s.Do([mask] { Config::Mutable().poseDiagMask = mask; });
					AddPoseEnter(s, r, 0, v.cam);
					s.Wait(0.7);
					s.Do([d, name] {
						REX::INFO("P3 A3 variant [{}]: the pose is on (pose {}, bIsInCover {:g}, stance field {}); the aim button goes down now", name, CoverPose::ToString(CoverPose::GetInfo().state), VarOr("bIsInCover", -1.0), CoverPose::StanceField());
						A3Begin(d);
					});
					s.Until([d, name] { return A3Step(d, name, 0.8, 1.5); }, 3.0);
					s.Do([d, name, mask] {
						const auto ci = Cover::GetInfo();
						const bool held = d->drop < 0.0 && CoverPose::GetInfo().state == CoverPose::State::Posed;
						Result(std::format("P3 A3 {} (diag mask {}): the first aim press held 1.5 s: the cover {}{}; the lowest bIsInCover {:g}; iSyncCoverStates {:g}; pose {}; state {}; Phase 3 position writes in the window {} (+{} blocked), made-up Sneak presses {}", name, mask, held ? "HELD" : "DROPPED",
							d->drop >= 0.0 ? std::format(" at +{:.2f} s", d->drop) : std::string(), d->minIn, d->coverStates, CoverPose::ToString(CoverPose::GetInfo().state), Core::ToString(ci.state), ci.positionWrites - d->writes0, ci.positionWritesBlocked - d->blocked0,
							Input::MadeUpPressStats().requested - d->sneak0));
						Summarise(std::string("A3 ") + name + (held ? ": held" : ": dropped"), held);
						Rig::MouseRight(false);
					});
					s.Wait(0.8);
					AddPoseLeaveSneak(s, r);
					s.Wait(0.5);
				}
				s.Do([] { Config::Mutable().poseDiagMask = 0; Config::Mutable().coverCamera = false; Cover::ForceReset("p3 A3 end"); });
			}
			return Run(s, c.now);
		}

		// five fresh poses on the low wall (set 11) or three on the tall wall (set 12): the first aim press of each, with the diag mask from the INI
		bool StageA3Rep(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				const int n = Config::Get().p3Set == 11 ? 5 : 3;
				auto held = std::make_shared<int>(0);
				for (int k = 0; k < n; ++k) {
					auto r = std::make_shared<PoseRun>();
					auto d = std::make_shared<A3Data>();
					AddPoseEnter(s, r, 0, true);
					s.Wait(0.8);
					s.Do([d, k] {
						REX::INFO("P3 A3 rep #{}: the pose is on (pose {}, bIsInCover {:g}); the aim button goes down now", k + 1, CoverPose::ToString(CoverPose::GetInfo().state), VarOr("bIsInCover", -1.0));
						A3Begin(d);
					});
					s.Until([d] { return A3Step(d, "rep", 0.8, 0.9); }, 3.0);
					s.Do([d] { REX::INFO("P3 A3 rep: one shot (left mouse button tapped)"); Rig::MouseLeft(true); d->tShot = Now(); });
					s.Wait(0.12);
					s.Do([] { Rig::MouseLeft(false); });
					s.Until([d] { return A3Step(d, "rep", 0.0, 1.5); }, 3.0);
					s.Do([d, held, k] {
						const bool ok = d->drop < 0.0 && CoverPose::GetInfo().state == CoverPose::State::Posed;
						*held += ok ? 1 : 0;
						Result(std::format("P3 A3 rep #{} ({} wall, diag mask {}): the first aim press held 1.5 s with a shot at +0.9 s: the cover {}{}; the lowest bIsInCover {:g}; iSyncCoverStates {:g}, iIsSighted {:g}, iSyncSighted {:g}, pose {}", k + 1, PoseLow() ? "low" : "tall", Config::Get().poseDiagMask, ok ? "HELD" : "DROPPED",
							d->drop >= 0.0 ? std::format(" at +{:.2f} s", d->drop) : std::string(), d->minIn, d->coverStates, VarOr("iIsSighted", -1.0), VarOr("iSyncSighted", -1.0), CoverPose::ToString(CoverPose::GetInfo().state)));
					});
					if (k == 0) {
						AddShot(s, PoseLow() ? "a3_low_aim" : "a3_tall_aim");
					}
					s.Do([d] { d->tRelease = Now(); d->tCleared = -1.0; Rig::MouseRight(false); });
					s.Until([] { return VarOr("iIsSighted", 1.0) < 0.5 && VarOr("iSyncSighted", 1.0) < 0.5; }, 4.0, [d](bool ok) { d->tCleared = ok ? Now() : -1.0; });
					s.Do([d, k] {
						Result(std::format("P3 A3 rep #{}: after the release the flags cleared {:.2f} s later; pose {}, bIsInCover {:g}", k + 1, d->tCleared > 0.0 ? d->tCleared - d->tRelease : -1.0, CoverPose::ToString(CoverPose::GetInfo().state), VarOr("bIsInCover", -1.0)));
					});
					s.Wait(0.5);
					AddPoseLeaveSneak(s, r);
					s.Wait(0.4);
				}
				s.Do([held, n] {
					const bool ok = *held == n;
					Result(std::format("P3 A3 rep: the first aim press held the cover in {} of {} fresh poses -> {}", *held, n, Verdict(ok)));
					Summarise("A3 the first aim press holds the cover on every fresh pose", ok);
					Cover::ForceReset("p3 A3 rep end");
				});
			}
			return Run(s, c.now);
		}

		// ---- stage A3, the weapon state (set 11): the first aim press with a sheathed and with a drawn weapon ---------------------------------------------------------------------
		void AddWeaponTo(Seq& s, const bool a_drawn)
		{
			s.Do([a_drawn] {
				REX::INFO("P3 A3 weapon: the weapon is {} (state {} now)", a_drawn ? "drawn" : "holstered", CoverPose::WeaponState());
				(void)AnimRecon::DrawWeapon(a_drawn);
			});
			s.Until([a_drawn] { return a_drawn ? CoverPose::WeaponState() == 3 : CoverPose::WeaponState() == 0; }, 4.0, [a_drawn](bool ok) {
				REX::INFO("P3 A3 weapon: state {} ({})", CoverPose::WeaponState(), ok ? (a_drawn ? "drawn" : "sheathed") : "NOT reached");
			});
			s.Wait(0.8);
		}

		void AddWeaponCase(Seq& s, const std::string& a_label, const bool a_holster, const int a_mask, const bool a_drawCfg, const bool a_expectHold)
		{
			auto r = std::make_shared<PoseRun>();
			auto d = std::make_shared<A3Data>();
			auto began = std::make_shared<bool>(false);
			auto wsPress = std::make_shared<int>(-1);
			auto sessions0 = std::make_shared<int>(0);
			s.Do([a_mask, a_drawCfg, sessions0] {
				Config::Mutable().poseDiagMask = a_mask;
				Config::Mutable().poseDrawWeapon = a_drawCfg;
				*sessions0 = CoverPose::GetInfo().sessions;
			});
			AddWeaponTo(s, !a_holster);
			AddPoseEnter(s, r, 0, true);
			s.Wait(0.7);
			s.Do([d, a_label, began, wsPress, sessions0] {
				*began = CoverPose::GetInfo().sessions > *sessions0;
				*wsPress = CoverPose::WeaponState();
				REX::INFO("P3 A3 weapon case [{}]: before the aim press: weapon state {}, pose {} (began {}), bIsInCover {:g}, stance field {}", a_label, *wsPress, CoverPose::ToString(CoverPose::GetInfo().state), *began, VarOr("bIsInCover", -1.0), CoverPose::StanceField());
				A3Begin(d);
			});
			s.Until([d, a_label] { return A3Step(d, a_label.c_str(), 0.8, 1.5); }, 3.0);
			s.Do([d, a_label, began, wsPress, a_expectHold, a_mask, a_drawCfg] {
				const bool held = *began && d->drop < 0.0 && CoverPose::GetInfo().state == CoverPose::State::Posed;
				const bool ok = *began ? (held == a_expectHold) : !a_expectHold;
				Result(std::format("P3 A3 weapon case {} (weapon state {} at the press, diag mask {}, bPoseDrawWeapon {}): the pose {}; the first aim press held 1.5 s: the cover {}{}; the lowest bIsInCover {:g}; iSyncCoverStates {:g}; weapon state after {}; pose {}; expected {} -> {}", a_label, *wsPress, a_mask, a_drawCfg,
					*began ? "began" : "did not begin", *began ? (held ? "HELD" : "DROPPED") : "(no pose)", d->drop >= 0.0 ? std::format(" at +{:.2f} s", d->drop) : std::string(), d->minIn, d->coverStates, CoverPose::WeaponState(), CoverPose::ToString(CoverPose::GetInfo().state), a_expectHold ? "HELD" : "not held", Verdict(ok)));
				Summarise("A3 weapon " + a_label, ok);
				Rig::MouseRight(false);
			});
			s.Wait(0.8);
			AddPoseLeaveSneak(s, r);
			s.Wait(0.5);
		}

		bool StageA3Weapon(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				AddWeaponCase(s, "1 sheathed, no weapon check (diag)", true, 16, false, false);
				for (int k = 1; k <= 5; ++k) {
					AddWeaponCase(s, std::format("2.{} drawn, normal", k), false, 0, false, true);
				}
				AddWeaponCase(s, "3 sheathed, bPoseDrawWeapon 1", true, 0, true, true);
				AddWeaponCase(s, "4 sheathed, bPoseDrawWeapon 0 (no pose expected)", true, 0, false, false);
				s.Do([] {
					Config::Mutable().poseDiagMask = 0;
					Config::Mutable().poseDrawWeapon = false;
					(void)AnimRecon::DrawWeapon(true);
					Cover::ForceReset("p3 A3 weapon end");
				});
				s.Wait(1.0);
			}
			return Run(s, c.now);
		}

		// ======================================== M1g: stage B, blind fire in the pose and the camera in the pose (sets 13 and 14) ============================================
		void BfLine(const char* a_tag, const double a_t)
		{
			const auto b = BlindFire::GetInfo();
			REX::INFO("P3 BF [{}] +{:.2f} s: iIsBlindFiring {:g}, iSyncCoverStates {:g}, iIsSighted {:g}, bIsInCover {:g}, stance field {}, blind fire {} (starts {}, ends {}, swallowed {}, freed {}, straight in linger {}), pose {}, boom y {:+.3f}", a_tag, a_t, VarOr("iIsBlindFiring", -1.0),
				VarOr("iSyncCoverStates", -1.0), VarOr("iIsSighted", -1.0), VarOr("bIsInCover", -1.0), CoverPose::StanceField(), BlindFire::ToString(b.state), b.starts, b.ends, b.swallowed, b.freed, b.passedInLinger, CoverPose::ToString(CoverPose::GetInfo().state), Camera::ReadRig().n1F8.t.y);
		}

		struct BfRun
		{
			double             t0{ 0.0 }, next{ 0.0 }, tPress{ 0.0 }, tFirstBlind{ -1.0 }, tFirstState{ -1.0 }, tFlagsClear{ -1.0 }, tRelease{ -1.0 };
			double             maxBlind{ 0.0 }, maxStates{ -1.0 };
			BlindFire::Info    i0;
		};

		// a_step: logs every 0.1 s and keeps the maxima; true when a_seconds have passed since a_since
		bool BfStep(const std::shared_ptr<BfRun>& r, const char* a_tag, const double a_since, const double a_seconds)
		{
			const double now = Now();
			if (now >= r->next) {
				r->next = now + 0.1;
				const double bl = VarOr("iIsBlindFiring", 0.0);
				const double cs = VarOr("iSyncCoverStates", -1.0);
				r->maxBlind = (std::max)(r->maxBlind, bl);
				r->maxStates = (std::max)(r->maxStates, cs);
				if (bl > 0.5 && r->tFirstBlind < 0.0) {
					r->tFirstBlind = now - r->tPress;
				}
				if (r->tRelease > 0.0 && r->tFlagsClear < 0.0 && bl < 0.5 && VarOr("iIsSighted", 0.0) < 0.5) {
					r->tFlagsClear = now - r->tRelease;
				}
				BfLine(a_tag, now - r->tPress);
			}
			return now - a_since >= a_seconds;
		}

		// a hold of the fire control: a_hold seconds down, then a_after seconds of watching; a picture before and one after
		void AddBfHold(Seq& s, const std::string& a_tag, const double a_hold, const double a_after, const bool a_expectBlind, const double a_expectStates, const std::string& a_pictureAfter)
		{
			auto r = std::make_shared<BfRun>();
			s.Do([r, a_tag] {
				*r = BfRun{};
				r->i0 = BlindFire::GetInfo();
				r->tPress = Now();
				r->next = 0.0;
				REX::INFO("P3 BF [{}]: the fire control goes down (pose {}, weapon state {}, aim held false)", a_tag, CoverPose::ToString(CoverPose::GetInfo().state), CoverPose::WeaponState());
				Rig::MouseLeft(true);
			});
			s.Until([r, a_tag, a_hold] { return BfStep(r, a_tag.c_str(), r->tPress, a_hold); }, a_hold + 2.0);
			s.Do([r] { r->tRelease = Now(); Rig::MouseLeft(false); });
			s.Until([r, a_tag, a_after] { return BfStep(r, a_tag.c_str(), r->tRelease, a_after); }, a_after + 2.0);
			s.Do([r, a_tag, a_hold, a_expectBlind, a_expectStates] {
				const auto b = BlindFire::GetInfo();
				const bool posed = CoverPose::GetInfo().state == CoverPose::State::Posed;
				const bool blindOk = !a_expectBlind || (r->maxBlind > 0.5 && r->maxStates == a_expectStates);
				const bool ok = posed && blindOk && b.starts == r->i0.starts + (a_expectBlind ? 1u : 0u) && b.state == BlindFire::State::Idle;
				Result(std::format("P3 BF {}: the fire control held {:.1f} s from the idle pose: blind fire start events {} (expected {}), BlindFireEnd events {}, events swallowed {}, passed as a fresh press {} (at +{:.2f} s after the press; Into {:.2f} s), straight in the linger time {}; the largest iIsBlindFiring {:g}, iSyncCoverStates {:g} (expected {:g}); iIsBlindFiring first 1 at +{:.2f} s; the flags {} {:.2f} s after the release; pose {}, blind fire {} -> {}",
					a_tag, a_hold, b.starts - r->i0.starts, a_expectBlind ? 1 : 0, b.ends - r->i0.ends, b.swallowed - r->i0.swallowed, b.freed - r->i0.freed, b.tFreed > 0.0 ? b.tFreed - b.tPress : -1.0, Config::Get().blindFireIntoLow, b.passedInLinger - r->i0.passedInLinger, r->maxBlind, r->maxStates, a_expectStates,
					r->tFirstBlind, r->tFlagsClear > 0.0 ? "cleared" : "did NOT clear", r->tFlagsClear, CoverPose::ToString(CoverPose::GetInfo().state), BlindFire::ToString(b.state), Verdict(ok)));
				Summarise("BF " + a_tag, ok);
			});
			if (!a_pictureAfter.empty()) {
				AddShot(s, a_pictureAfter);
			}
		}

		void AddBfEnter(Seq& s, const std::shared_ptr<PoseRun>& r)
		{
			s.Do([] { Config::Mutable().coverCamera = true; });
			AddWeaponTo(s, true);
			AddPoseEnter(s, r, 0, true);
			s.Wait(1.0);
		}

		bool StageBfLow(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				AddBfEnter(s, r);
				AddShot(s, "bf_low_0");
				AddBfHold(s, "A hold 2.0 s", 2.0, 3.0, true, 4.0, "bf_low_A");
				// B: a tap from the idle pose: the press and the release are swallowed, nothing fires, the mode runs and ends
				{
					auto rb = std::make_shared<BfRun>();
					s.Do([rb] { *rb = BfRun{}; rb->i0 = BlindFire::GetInfo(); rb->tPress = Now(); rb->next = 0.0; Rig::MouseLeft(true); });
					s.Wait(0.12);
					s.Do([rb] { rb->tRelease = Now(); Rig::MouseLeft(false); });
					s.Until([rb] { return BfStep(rb, "B tap", rb->tRelease, 3.0); }, 5.0);
					s.Do([rb] {
						const auto b = BlindFire::GetInfo();
						const bool ok = b.starts == rb->i0.starts + 1 && b.freed == rb->i0.freed && b.ends == rb->i0.ends + 1 && b.state == BlindFire::State::Idle;
						Result(std::format("P3 BF B a tap (0.12 s) from the idle pose: start events {}, passed as a fresh press {} (none: the press and the release were swallowed before Into was over), BlindFireEnd {}, the largest iIsBlindFiring {:g}, iSyncCoverStates {:g}, blind fire {} -> {}", b.starts - rb->i0.starts, b.freed - rb->i0.freed, b.ends - rb->i0.ends,
							rb->maxBlind, rb->maxStates, BlindFire::ToString(b.state), Verdict(ok)));
						Summarise("BF B tap from idle", ok);
					});
				}
				AddShot(s, "bf_low_B");
				// E: a hold of 1.0 s alone (the part of D that fires)
				AddBfHold(s, "E hold 1.0 s", 1.0, 3.0, true, 4.0, "bf_low_E");
				// D: hold 1.0 s, then two taps 0.5 s apart inside the linger time
				{
					auto rd = std::make_shared<BfRun>();
					s.Do([rd] { *rd = BfRun{}; rd->i0 = BlindFire::GetInfo(); rd->tPress = Now(); rd->next = 0.0; Rig::MouseLeft(true); });
					s.Until([rd] { return BfStep(rd, "D hold+taps", rd->tPress, 1.0); }, 3.0);
					s.Do([rd] { rd->tRelease = Now(); Rig::MouseLeft(false); });
					s.Wait(0.3);
					s.Do([] { Rig::MouseLeft(true); });
					s.Wait(0.12);
					s.Do([] { Rig::MouseLeft(false); });
					s.Wait(0.4);
					s.Do([] { Rig::MouseLeft(true); });
					s.Wait(0.12);
					s.Do([] { Rig::MouseLeft(false); });
					s.Until([rd] { return BfStep(rd, "D hold+taps", rd->tRelease, 4.0); }, 6.0);
					s.Do([rd] {
						const auto b = BlindFire::GetInfo();
						const bool ok = b.starts == rd->i0.starts + 1 && b.passedInLinger >= rd->i0.passedInLinger + 2 && b.ends == rd->i0.ends + 1 && b.state == BlindFire::State::Idle;
						Result(std::format("P3 BF D a hold of 1.0 s and two taps (0.12 s) 0.3 s and 0.9 s after the release, 0.5 s apart: start events {} (expected 1: no second Into), presses straight to the game inside the linger time {} (expected 2), BlindFireEnd {} (expected 1, after the last release plus the linger time), the flags cleared {:.2f} s after the last release; the largest iIsBlindFiring {:g} -> {}",
							b.starts - rd->i0.starts, b.passedInLinger - rd->i0.passedInLinger, b.ends - rd->i0.ends, rd->tFlagsClear, rd->maxBlind, Verdict(ok)));
						Summarise("BF D hold and two taps, no second Into", ok);
					});
				}
				AddShot(s, "bf_low_D");
				// G: the aim pressed during blind fire
				{
					auto rg = std::make_shared<BfRun>();
					s.Do([rg] { *rg = BfRun{}; rg->i0 = BlindFire::GetInfo(); rg->tPress = Now(); rg->next = 0.0; Rig::MouseLeft(true); });
					s.Until([rg] { return BfStep(rg, "G aim in blind fire", rg->tPress, 1.3); }, 3.0);
					s.Do([rg] { REX::INFO("P3 BF [G]: the aim button goes down during blind fire"); rg->tRelease = Now(); Rig::MouseRight(true); });
					s.Until([rg] { return BfStep(rg, "G aim in blind fire", rg->tRelease, 2.2); }, 4.0);
					s.Do([rg] {
						const auto b = BlindFire::GetInfo();
						const bool ok = b.ends >= rg->i0.ends + 1 && !b.aimDeferred && CoverPose::GetInfo().state == CoverPose::State::Posed;
						Result(std::format("P3 BF G the aim pressed 1.3 s into a hold of the fire control: BlindFireEnd events {}, the aim press passed to the game after the Out clip (deferred now {}), iIsSighted {:g}, iSyncCoverStates {:g}, bIsInCover {:g}, pose {} -> {}", b.ends - rg->i0.ends, b.aimDeferred, VarOr("iIsSighted", -1.0), VarOr("iSyncCoverStates", -1.0), VarOr("bIsInCover", -1.0),
							CoverPose::ToString(CoverPose::GetInfo().state), Verdict(ok)));
						Summarise("BF G the aim during blind fire", ok);
						Rig::MouseLeft(false);
						Rig::MouseRight(false);
					});
					s.Wait(2.0);
				}
				// F: a leave by pulling away in the middle of blind fire
				{
					auto rf = std::make_shared<BfRun>();
					s.Do([rf, r] {
						Config::Mutable().coverCamera = true;
						Cover::ForceReset("p3 BF F");
					});
					AddWeaponTo(s, true);
					AddPoseEnter(s, r, 0, true);
					s.Wait(1.0);
					s.Do([rf] { *rf = BfRun{}; rf->i0 = BlindFire::GetInfo(); rf->tPress = Now(); rf->next = 0.0; Rig::MouseLeft(true); });
					s.Until([rf] { return BfStep(rf, "F leave", rf->tPress, 1.2); }, 3.0);
					s.Do([rf] { rf->tRelease = Now(); REX::INFO("P3 BF [F]: pulling away (the away key) in the middle of blind fire"); Rig::Tap(PoseAway(), 4.0); });
					s.Until([rf] { return BfStep(rf, "F leave", rf->tRelease, 3.0); }, 5.0);
					s.Do([rf] {
						Rig::MouseLeft(false);
						Rig::ReleaseEverything("p3 BF F");
						const auto b = BlindFire::GetInfo();
						const bool ok = CoverPose::GetInfo().state == CoverPose::State::Off && OwnGraph() && Cover::GetInfo().state == Core::CoverState::None && b.state == BlindFire::State::Idle;
						Result(std::format("P3 BF F a leave by pulling away 1.2 s into blind fire: pose {}, the own graph {}, Phase 3 state {}, blind fire {}, iIsBlindFiring {:g}, stance field {} -> {}", CoverPose::ToString(CoverPose::GetInfo().state), OwnGraph() ? "back" : "NOT back", Core::ToString(Cover::GetInfo().state), BlindFire::ToString(b.state),
							VarOr("iIsBlindFiring", -1.0), CoverPose::StanceField(), Verdict(ok)));
						Summarise("BF F leave in the middle of blind fire", ok);
					});
					s.Wait(1.0);
				}
				s.Do([] { Cover::ForceReset("p3 BF low end"); });
			}
			return Run(s, c.now);
		}

		bool StageBfTall(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				AddBfEnter(s, r);
				AddShot(s, "bf_tall_0");
				AddBfHold(s, "T hold 2.0 s (tall, BlindFireStart)", 2.0, 3.0, true, 3.0, "bf_tall_T");
				s.Do([] { Cover::ForceReset("p3 BF tall end"); });
			}
			return Run(s, c.now);
		}

		// ---- the camera in the pose (T5.3): the camera root's height above the player in the same situation on the own graph and in the pose ----------------------------------------------------
		void AddOwnWindow(Seq& s, const std::shared_ptr<CamAcc>& a, const char* a_label, const double a_seconds)
		{
			s.Do([a, a_label] { a->Begin(); a->dump = nullptr; (void)a_label; });
			s.Until([a, a_seconds] {
				a->Step();
				return Now() - a->t0 >= a_seconds;
			}, a_seconds + 2.0);
		}

		bool StageCamPose(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				struct Acc
				{
					std::shared_ptr<CamAcc> ownIdle{ std::make_shared<CamAcc>() }, ownAim{ std::make_shared<CamAcc>() }, ownSneak{ std::make_shared<CamAcc>() }, poseIdle{ std::make_shared<CamAcc>() }, poseAim{ std::make_shared<CamAcc>() };
				};
				auto a = std::make_shared<Acc>();
				auto r = std::make_shared<PoseRun>();
				const bool low = PoseLow();
				// the own graph, standing, idle and aiming; then sneaking (low only: the pose there is crouched)
				s.Do([] { Config::Mutable().coverPoses = false; Config::Mutable().coverCamera = false; Cover::ForceReset("p3 cam pose"); });
				if (low) {
					AddLowPrepare(s, 0);
				} else {
					AddPrepare(s, 0, Base(), PoseTallFacing() ? std::optional<float>(90.0f) : std::nullopt);
				}
				s.Wait(1.5);
				AddOwnWindow(s, a->ownIdle, "own standing", 1.0);
				s.Do([] { Rig::MouseRight(true); });
				s.Wait(1.0);
				AddOwnWindow(s, a->ownAim, "own standing aiming", 1.0);
				s.Do([] { Rig::MouseRight(false); });
				s.Wait(1.5);
				if (low) {
					s.Do([] { Rig::Tap(Rig::kKeyLCtrl, 0.08); });
					s.Wait(1.8);
					AddOwnWindow(s, a->ownSneak, "own sneaking", 1.0);
					s.Do([] { Rig::Tap(Rig::kKeyLCtrl, 0.08); });
					s.Wait(1.2);
				}
				// the pose, idle and aiming (the cover camera offset off, so that the numbers compare with the own graph's)
				s.Do([] { Config::Mutable().coverCamera = false; });
				s.Do([] {
					if (CoverPose::WeaponState() != 3) {
						(void)AnimRecon::DrawWeapon(true);
					}
				});
				s.Wait(1.0);
				AddPoseEnter(s, r, 0, false);
				s.Wait(2.0);
				AddOwnWindow(s, a->poseIdle, "pose idle", 1.0);
				AddShot(s, low ? "cam_low_idle" : "cam_tall_idle");
				s.Do([] { Rig::MouseRight(true); });
				s.Wait(1.2);
				AddOwnWindow(s, a->poseAim, "pose aim", 1.0);
				AddShot(s, low ? "cam_low_aim" : "cam_tall_aim");
				s.Do([] { Rig::MouseRight(false); });
				s.Wait(0.8);
				s.Do([a, low] {
					const auto sets = CoverPose::GetRigSets();
					std::string seen;
					for (int k = 0; k < 4; ++k) {
						seen += std::format(" set {} {} (boom y {:+.3f}, pivot z {:+.3f});", k, sets.seen[k] ? "seen" : "default", sets.boom[k].y, sets.pivot[k].z);
					}
					Result(std::format("P3 CAM ({} wall, the view {}): the camera root above the player: own standing idle {:.3f} m (horizontal {:.3f}), own standing aiming {:.3f} m ({:.3f}){}; in the pose idle {:.3f} m ({:.3f}), in the pose aiming {:.3f} m ({:.3f}); the rig sets:{}", low ? "low" : "tall", PoseLow() ? "toward the wall" : (PoseTallFacing() ? "toward the wall" : "away from the wall"),
						a->ownIdle->Z(), a->ownIdle->H(), a->ownAim->Z(), a->ownAim->H(), low ? std::format(", own sneaking {:.3f} m ({:.3f})", a->ownSneak->Z(), a->ownSneak->H()) : std::string(), a->poseIdle->Z(), a->poseIdle->H(), a->poseAim->Z(), a->poseAim->H(), seen));
					const float refIdle = low ? a->ownSneak->Z() : a->ownIdle->Z();
					const bool ok = std::fabs(a->poseIdle->Z() - refIdle) <= 0.3f;
					Result(std::format("P3 CAM idle: the camera root {:.3f} m above the player in the pose against {:.3f} m on the own graph in the same situation ({}) -> {}", a->poseIdle->Z(), refIdle, low ? "sneaking" : "standing", Verdict(ok)));
					Summarise("CAM pose idle against the own graph", ok);
				});
				AddPoseLeaveSneak(s, r);
				s.Do([] { Cover::ForceReset("p3 cam pose end"); });
			}
			return Run(s, c.now);
		}

		// ======================================== M3a: the regression of the card (sets 16 and 17) =====================================================================================
		bool StageRegSheathed(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				auto w0 = std::make_shared<int>(-1);
				s.Do([] { Config::Mutable().coverTrigger = 3; });
				AddWeaponTo(s, false);
				s.Do([w0] { *w0 = CoverPose::WeaponState(); });
				AddPoseEnter(s, r, 0, true);
				s.Wait(0.8);
				s.Do([r, w0] {
					const auto i = CoverPose::GetInfo();
					const bool ok = i.state == CoverPose::State::Posed && CoverPose::WeaponState() == 3 && r->tPosed > 0.0;
					Result(std::format("P3 REG sheathed entry (weapon state {} before; automatic mode, walking into the low wall): Phase 3 state {:.2f} s after the push, the pose {:.2f} s after the push (the weapon drawn first: state now {}), pose {}, bIsInCover {:g} -> {}", *w0, r->tState > 0.0 ? r->tState - r->tPush : -1.0,
						r->tPosed > 0.0 ? r->tPosed - r->tPush : -1.0, CoverPose::WeaponState(), CoverPose::ToString(i.state), VarOr("bIsInCover", -1.0), Verdict(ok)));
					Summarise("REG sheathed entry: the weapon drawn first, then the pose", ok);
				});
				AddPoseLeaveSneak(s, r);
				s.Do([r] {
					const bool ok = r->tNone > 0.0 && r->tOff > 0.0 && OwnGraph();
					Result(std::format("P3 REG sheathed entry, out by Sneak: state None {:.2f} s after the press, the own graph {:.2f} s after it -> {}", r->tNone > 0.0 ? r->tNone - r->tLeave : -1.0, r->tOff > 0.0 ? r->tOff - r->tLeave : -1.0, Verdict(ok)));
					Summarise("REG sheathed entry, out by Sneak", ok);
				});
				s.Wait(0.5);
			}
			return Run(s, c.now);
		}

		bool StageBlindTail(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				AddBfEnter(s, r);
				AddShot(s, "bt_0");
				AddBfHold(s, "H hold 0.5 s after Into", 1.3, 3.0, true, 4.0, "bt_H");
				{
					auto rb = std::make_shared<BfRun>();
					s.Do([rb] { *rb = BfRun{}; rb->i0 = BlindFire::GetInfo(); rb->tPress = Now(); rb->next = 0.0; Rig::MouseLeft(true); });
					s.Wait(0.12);
					s.Do([rb] { rb->tRelease = Now(); Rig::MouseLeft(false); });
					s.Until([rb] { return BfStep(rb, "T tap", rb->tRelease, 3.0); }, 5.0);
					s.Do([rb] {
						const auto b = BlindFire::GetInfo();
						const bool ok = b.starts == rb->i0.starts + 1 && b.madeShots == rb->i0.madeShots + 1 && b.ends == rb->i0.ends + 1 && b.state == BlindFire::State::Idle;
						Result(std::format("P3 BF T a tap (0.12 s) from the idle pose: start events {}, the lost tap fired by the plugin's own press {} (expected 1), BlindFireEnd {}, the largest iIsBlindFiring {:g}, iSyncCoverStates {:g}, blind fire {} -> {} (the rounds: the HUD in bt_tap.jpg against bt_H.jpg)", b.starts - rb->i0.starts, b.madeShots - rb->i0.madeShots,
							b.ends - rb->i0.ends, rb->maxBlind, rb->maxStates, BlindFire::ToString(b.state), Verdict(ok)));
						Summarise("BF T the lost tap fires once", ok);
					});
				}
				AddShot(s, "bt_tap");
				s.Do([] { Cover::ForceReset("p3 M3a tail end"); });
			}
			return Run(s, c.now);
		}

		// the tall wall with bPoseTall 0: Basic cover, B enters and leaves, no pose, the camera offset as in R11
		bool StageRegTall(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto before = std::make_shared<CamAcc>();
				auto offW = std::make_shared<CamAcc>();
				auto onW = std::make_shared<CamAcc>();
				auto sess0 = std::make_shared<int>(0);
				s.Do([sess0] { Config::Mutable().coverPoses = true; Config::Mutable().coverTrigger = 1; *sess0 = CoverPose::GetInfo().sessions; });
				for (int pass = 0; pass < 2; ++pass) {
					const bool on = pass == 1;
					auto e = std::make_shared<CaseData>();
					auto acc = on ? onW : offW;
					AddPrepare(s, 0, Base(), std::nullopt);
					s.Do([on] { Config::Mutable().coverCamera = on; Cover::ForceReset("p3 M3a tall"); });
					s.Wait(1.2);
					if (!on) {
						AddWindow(s, before, 1.0);
					}
					AddEnter(s, e, Rig::kKeyS, Core::CoverKind::High);
					s.Do([e, on] { LogEnter(on ? "REG tall B enters (offset on)" : "REG tall B enters (offset off)", e, 0, true); });
					s.Wait(1.2);
					AddWindow(s, acc, 1.0);
					s.Do([e, sess0, on] {
						const auto i = Cover::GetInfo();
						const bool ok = i.state == Core::CoverState::InCover && i.candidate.kind == Core::CoverKind::High && CoverPose::GetInfo().sessions == *sess0 && !CoverPose::Busy();
						Result(std::format("P3 REG tall cover (offset {}): Phase 3 state {} at a {} cover, the pose never began (sessions {} -> {}, pose {}) -> {}", on ? "on" : "off", Core::ToString(i.state), Core::ToString(i.candidate.kind), *sess0, CoverPose::GetInfo().sessions, CoverPose::ToString(CoverPose::GetInfo().state), Verdict(ok)));
						Summarise(std::string("REG tall: Basic cover, no pose (offset ") + (on ? "on)" : "off)"), ok);
					});
					AddLeaveSneak(s, e);
					s.Do([e, on] {
						const bool ok = e->tNone > 0.0 && e->tNone - e->leaveT <= 0.5;
						Result(std::format("P3 REG tall B leaves (offset {}): state None {:.2f} s after the press, stance {} -> {}", on ? "on" : "off", e->tNone > 0.0 ? e->tNone - e->leaveT : -1.0, e->stanceAfter, Verdict(ok)));
						Summarise(std::string("REG tall: B leaves (offset ") + (on ? "on)" : "off)"), ok);
					});
					s.Wait(1.5);
				}
				s.Do([offW, onW, before] {
					const float dH = onW->H() - offW->H();
					const float dZ = onW->Z() - offW->Z();
					Result(std::format("P3 REG tall camera: in cover with the offset off {} | on {} | standing before {}; the root is {:+.3f} m farther and {:+.3f} m higher with the offset on (fCoverCamBackTall {:.2f}, fCoverCamUpTall {:.2f}; R11 on the low wall: +0.818 and +0.580)", offW->Text(), onW->Text(), before->Text(), dH, dZ, Config::Get().coverCamBackTall, Config::Get().coverCamUpTall));
					Config::Mutable().coverCamera = false;
					Cover::ForceReset("p3 M3a tall end");
				});
				s.Wait(0.5);
			}
			return Run(s, c.now);
		}

		// ---- the sequences ----------------------------------------------------------------------------------------------------
		constexpr std::array<Stage, 14> kSet1{ {
			{ "ready", StageReady, false },
			{ "T3.3b probe: the game movement vector with W, A, S, D held in turn", StageVectorProbe, true },
			{ "T3.3b write test: vector (0, m) written after the original", StageWriteTest, true },
			{ "direction check: W, A, S against the computed input direction", StageDirection, true },
			{ "R1 enter high (S held, Sneak tapped), R3 leave by Sneak", StageEnterHighSneak, true },
			{ "R1 Sneak tap with no key held: the vanilla toggle", StageVanillaToggle, true },
			{ "S3 queue layout: W, D and the aim button held together", StageQueueLayout, true },
			{ "R2 staying on cover: A, D along the wall, W away", StageStay, true },
			{ "R3 leave by Jump", StageEnterHighJump, true },
			{ "R3 leave by Sprint", StageEnterHighSprint, true },
			{ "R3 leave by forcing first person", StageEnterHighFirstPerson, true },
			{ "R6 open ground and first person", StageOpenGround, true },
			{ "M1b R10 mode 3 at the tall wall", StageR10Tall, true },
			{ "summary", StageSummary, false },
		} };
		constexpr std::array<Stage, 14> kSet2{ {
			{ "ready", StageReady, false },
			{ "T3.4 spike: a Sneak press made by the plugin, ten times", StageMadeUp, true },
			{ "R4 find the low wall from either side", StageLowSearch, true },
			{ "R4 enter low from standing, leave; enter while sneaking, leave", StageLowEnter, true },
			{ "R5 peek over and side peek", StagePeeks, true },
			{ "R2 staying on the low wall: A, D along it, S away", StageStayLow, true },
			{ "T3.5 INI reload on Numpad 9", StageReload, true },
			{ "T3.5 mode 0: the dedicated key", StageMode0, true },
			{ "T3.5 mode 2: the held Sneak control", StageMode2, true },
			{ "M1b R9 the held Sneak press", StageR9, true },
			{ "M1b R10 mode 3 at the low wall", StageR10Low, true },
			{ "M1c T5.3a R11 the cover camera, off and on", StageR11, true },
			{ "M1c T3.8 R12 B while peeking over", StageR12, true },
			{ "summary", StageSummary, false },
		} };
		constexpr std::array<Stage, 3> kSet3{ {
			{ "ready", StageReady, false },
			{ "R7 the doorway of card P2", StageDoorway, false },
			{ "summary", StageSummary, false },
		} };

		// ======================================== set 5: R8, taking cover from a run (batch 4) ============================================================
		// Per variant: the player is brought to a start point as far from the wall as the free floor allows (at most 7 m), the key toward the wall is held, the Sneak tap comes at
		// the first fresh answer of the sensor once the stand point is within 2.5 m (fEnterMaxDistance is 3.0), and for 1.5 s after InCover every call of the input hook logs the
		// time, the position (along and across the wall from the stand point), the game's movement vector, the vector written back and the state.
		bool IsSave29() { return s_spawn.x < -650.0f; }

		struct R8Geo
		{
			float      ax, ay;       // the wall's anchor
			float      nx, ny;       // from the wall toward the player
			float      standX;       // the stand point's x
			float      heading;      // the camera heading to face (degrees)
			Rig::Key   key;
			const char* keyName;
		};

		R8Geo Geo()
		{
			if (IsSave29()) {
				return { -713.28f, 1937.40f, -1.0f, 0.0f, -713.63f, 270.0f, Rig::kKeyS, "S" };
			}
			return { -615.31f, 2116.00f, 1.0f, 0.0f, -614.96f, 270.0f, Rig::kKeyW, "W" };
		}

		[[nodiscard]] int Alert()
		{
			const auto v = PlayerControl::ReadVar("iSyncRelaxedReadyAlert");
			return v ? static_cast<int>(std::lround(*v)) : -1;
		}

		struct R8Sample
		{
			double           t;
			RE::NiPoint3     p;
			float            gx, gy, wx, wy;
			bool             wrote;
			Core::CoverState st;
		};

		struct R8Data
		{
			bool         ready{ false };
			bool         held{ false };
			std::string  label;
			R8Geo        geo{};
			WorldProbe::Ticket ticket{ 0 };
			float        freeDist{ 2.0f };
			float        dist{ 2.0f };
			float        z{ 0.0f };
			bool         alertOk{ false };
			int          alertStart{ -1 }, alertAtTap{ -1 };
			int          stanceAtTap{ -1 };
			double       t0{ 0.0 };
			RE::NiPoint3 lastP{};
			double       lastT{ 0.0 };
			float        speed{ 0.0f };
			bool         tapped{ false };
			double       tapT{ 0.0 };
			float        speedAtTap{ 0.0f }, distAtTap{ 0.0f };
			double       tIn{ -1.0 };
			Core::Vec2   tangent{}, normal{}, stand{};
			std::uint64_t lastCalls{ 0 };
			std::vector<R8Sample> samples;
			int          logged{ 0 };
		};

		void AddR8Variant(Seq& s, const bool a_ready, const bool a_held)
		{
			auto d = std::make_shared<R8Data>();
			d->ready = a_ready;
			d->held = a_held;
			d->label = std::format("{}, key {}", a_ready ? "ready" : "relaxed", a_held ? "held throughout" : "released at InCover");

			// how far from the wall the floor is free
			s.Do([d] {
				Rig::ReleaseEverything("p3 r8");
				Cover::ForceReset("p3 r8");
				d->geo = Geo();
				const float z = Snap().position.z + 1.0f;
				d->ticket = WorldProbe::Submit({ d->geo.standX, d->geo.ay, z }, { d->geo.standX + d->geo.nx * 7.5f, d->geo.ay + d->geo.ny * 7.5f, z }, 0.25f);
			});
			s.Until([d] {
				const auto r = WorldProbe::Poll(d->ticket);
				if (!r) {
					return false;
				}
				d->freeDist = !r->ok ? 2.0f : (r->hit ? r->fraction * 7.5f - 0.5f : 7.0f);
				return true;
			}, 3.0);
			s.Do([d] {
				d->dist = std::clamp(d->freeDist, 2.0f, 7.0f);
				FloorBegin(d->geo.standX + d->geo.nx * d->dist, d->geo.ay + d->geo.ny * d->dist, Snap().position.z);
			});
			s.Until(FloorPoll, 3.0);
			s.Do([d] {
				if (!s_floor.have || std::fabs(s_floor.z - Snap().position.z) > 1.0f) {
					d->dist = 2.0f;
					d->z = Snap().position.z;
				} else {
					d->z = s_floor.z;
				}
				MoveBegin({ d->geo.standX + d->geo.nx * d->dist, d->geo.ay + d->geo.ny * d->dist, d->z }, (std::max)(1.0, static_cast<double>(d->dist) / 3.0));
			});
			s.Until(MoveStep, 15.0);
			// stance standing, the weapon state asked for
			s.Do([] {
				if (Stance() != 0) {
					Rig::Tap(Rig::kKeyLCtrl, 0.08);
				}
			});
			s.Wait(0.7);
			s.Do([d] {
				if (Alert() != (d->ready ? 1 : 0)) {
					Rig::Tap(Rig::kKeyR, 0.1);
				}
			});
			s.Until([d] { return Alert() == (d->ready ? 1 : 0); }, 2.5, [d](bool ok) { d->alertOk = ok; });
			s.Wait(1.5);
			s.Do([] { s_align = Align{}; });
			s.Until([d] { return AlignStep(d->geo.heading); }, 6.0);
			s.Wait(0.8);
			// the run
			s.Do([d] {
				Cover::ForceReset("p3 r8 start");
				d->alertStart = Alert();
				d->t0 = Now();
				d->lastP = Snap().position;
				d->lastT = Now();
				d->speed = 0.0f;
				Rig::Tap(d->geo.key, 9.0);
			});
			s.Until([d] {
				const double now = Now();
				if (now - d->lastT >= 0.1) {
					const auto& p = Snap().position;
					d->speed = static_cast<float>(std::hypot(p.x - d->lastP.x, p.y - d->lastP.y) / (now - d->lastT));
					d->lastP = p;
					d->lastT = now;
				}
				const float across = std::fabs(Snap().position.x - d->geo.standX);
				return FreshCandidate(Core::CoverKind::None) && across <= 2.5f;
			}, 6.0, [d](bool ok) {
				if (!ok) {
					Result(std::format("P3 R8 {}: the sensor gave no fresh answer within 2.5 m of the stand point (no Sneak tap sent)", d->label));
					return;
				}
				d->tapped = true;
				d->tapT = Now();
				d->speedAtTap = d->speed;
				d->distAtTap = std::fabs(Snap().position.x - d->geo.standX);
				d->alertAtTap = Alert();
				d->stanceAtTap = Stance();
				Rig::Tap(Rig::kKeyLCtrl, 0.08);
			});
			s.Until([] { return Cover::GetInfo().state == Core::CoverState::InCover; }, 1.2, [d](bool ok) {
				if (!ok) {
					return;
				}
				d->tIn = Now();
				const auto i = Cover::GetInfo();
				d->tangent = { i.candidate.tangent.x, i.candidate.tangent.y };
				d->normal = { i.candidate.normal.x, i.candidate.normal.y };
				d->stand = { i.candidate.standPoint.x, i.candidate.standPoint.y };
				d->lastCalls = Input::LastMovementVector().calls - 1;
				if (!d->held) {
					Rig::ReleaseEverything("p3 r8 released at InCover");
				}
			});
			// 1.5 s after InCover: one sample per call of the input hook
			s.Until([d] {
				if (d->tIn < 0.0) {
					return true;
				}
				const auto v = Input::LastMovementVector();
				if (v.calls != d->lastCalls) {
					d->lastCalls = v.calls;
					if (d->samples.size() < 400) {
						d->samples.push_back({ Now(), Snap().position, v.x, v.y, v.wroteX, v.wroteY, v.wroteLast, Cover::GetInfo().state });
					}
				}
				return Now() >= d->tIn + 1.5;
			}, 3.0);
			s.Do([d] {
				Rig::ReleaseEverything("p3 r8 done");
				const auto  endState = Cover::GetInfo().state;
				if (d->tIn < 0.0) {
					Result(std::format("P3 R8 {}: {} cover; no InCover within 1.2 s of the tap (tapped {}, speed at the tap {:.2f} m/s, {:.2f} m from the stand point, start {:.1f} m from the wall, weapon state {} at the start and {} at the tap, wanted {}{}) -> FAIL",
						d->label, "no", d->tapped, d->speedAtTap, d->distAtTap, d->dist, d->alertStart, d->alertAtTap, d->ready ? 1 : 0, d->alertOk ? "" : " (not reached)"));
					Summarise(std::string("R8 ") + d->label, false);
					return;
				}
				auto along = [d](const RE::NiPoint3& p) { return (p.x - d->stand.x) * d->tangent.x + (p.y - d->stand.y) * d->tangent.y; };
				auto across = [d](const RE::NiPoint3& p) { return (p.x - d->stand.x) * d->normal.x + (p.y - d->stand.y) * d->normal.y; };
				float maxAlong = 0.0f;
				float zeroAlong = 0.0f;
				int   zeroCount = 0;
				int   zeroMoved = 0;
				for (std::size_t k = 0; k < d->samples.size(); ++k) {
					const auto& sm = d->samples[k];
					maxAlong = (std::max)(maxAlong, std::fabs(along(sm.p)));
					if (k > 0) {
						const auto& pv = d->samples[k - 1];
						if (pv.wx == 0.0f && pv.wy == 0.0f) {
							++zeroCount;
							const float da = std::fabs(along(sm.p) - along(pv.p));
							zeroAlong += da;
							zeroMoved += da > 0.002f ? 1 : 0;
						}
					}
					if (d->logged < 150) {
						++d->logged;
						REX::INFO("R8 sample [{}] t={:+.3f} pos=({:.3f},{:.3f}) along={:+.3f} across={:+.3f} game=({:+.3f},{:+.3f}) wrote=({:+.3f},{:+.3f}){} state {}", d->label, sm.t - d->tIn, sm.p.x, sm.p.y, along(sm.p), across(sm.p), sm.gx, sm.gy, sm.wx, sm.wy,
							sm.wrote ? " [changed]" : "", Core::ToString(sm.st));
					}
				}
				auto at = [d, &along, &across](const double a_dt, float& a_al, float& a_ac) {
					a_al = 0.0f;
					a_ac = 0.0f;
					const R8Sample* best = nullptr;
					for (const auto& sm : d->samples) {
						if (!best || std::fabs(sm.t - (d->tIn + a_dt)) < std::fabs(best->t - (d->tIn + a_dt))) {
							best = &sm;
						}
					}
					if (best) {
						a_al = along(best->p);
						a_ac = across(best->p);
					}
				};
				float a03, c03, a06, c06, a10, c10, a15, c15;
				at(0.3, a03, c03);
				at(0.6, a06, c06);
				at(1.0, a10, c10);
				at(1.5, a15, c15);
				const double dtIn = d->tIn - d->tapT;
				const bool   ok = dtIn <= 0.6 && maxAlong <= 0.10f && endState == Core::CoverState::InCover;
				Result(std::format("P3 R8 {}: speed at the tap {:.2f} m/s, the stand point {:.2f} m away at the tap (start {:.1f} m from the wall), weapon state {} at the start and {} at the tap (wanted {}{}), stance {}; InCover {:.2f} s after the tap (limit 0.6); from the stand point (along / across, m): 0.3 s {:+.3f} / {:+.3f}, 0.6 s {:+.3f} / {:+.3f}, 1.0 s {:+.3f} / {:+.3f}, 1.5 s {:+.3f} / {:+.3f}; largest along {:.3f} m (limit 0.10); state at the end {}; {} samples; the player moved along the wall while the written vector was exactly (0,0): {} ({:.3f} m summed; {} of the {} intervals that began with a zero written vector showed a move) -> {}",
					d->label, d->speedAtTap, d->distAtTap, d->dist, d->alertStart, d->alertAtTap, d->ready ? 1 : 0, d->alertOk ? "" : ", NOT reached", d->stanceAtTap, dtIn, a03, c03, a06, c06, a10, c10, a15, c15, maxAlong, Core::ToString(endState), d->samples.size(),
					zeroAlong > 0.01f ? "YES" : "no", zeroAlong, zeroMoved, zeroCount, Verdict(ok)));
				Summarise(std::string("R8 ") + d->label, ok);
			});
			s.Wait(0.4);
		}

		bool StageR8(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				AddR8Variant(s, false, false);
				AddR8Variant(s, false, true);
				AddR8Variant(s, true, false);
				AddR8Variant(s, true, true);
			}
			return Run(s, c.now);
		}

		constexpr std::array<Stage, 3> kSet5{ {
			{ "ready", StageReady, false },
			{ "R8 enter from a run", StageR8, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 6> kSet4{ {
			{ "ready", StageReady, false },
			{ "T4.0 compatibility mode", StageCompat, false },
			{ "R1 enter high (S held, Sneak tapped), R3 leave by Sneak", StageEnterHighSneak, true },
			{ "T4.0 cast stress with Stride", StageStress, false },
			{ "T4.0 shoulder swap by X", StageShoulder, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 9> kSet6{ {
			{ "ready", StageReady, false },
			{ "R4 find the low wall from either side", StageLowSearch, true },
			{ "M1d POSE in at the low wall", StagePoseIn, true },
			{ "M1d POSE walk along the wall", StagePoseWalk, true },
			{ "M1d POSE aim", StagePoseAim, true },
			{ "M1d POSE out", StagePoseOut, true },
			{ "M1d POSE ten ins and outs, hard reset", StagePoseTen, true },
			{ "M1d POSE camera", StagePoseCam, true },
			{ "summary", StageSummary, false },
		} };
		constexpr std::array<Stage, 8> kSet7{ {
			{ "ready", StageReady, false },
			{ "M1d POSE in at the tall wall", StagePoseIn, true },
			{ "M1d POSE walk along the wall", StagePoseWalk, true },
			{ "M1d POSE aim", StagePoseAim, true },
			{ "M1d POSE out", StagePoseOut, true },
			{ "M1d POSE ten ins and outs, hard reset", StagePoseTen, true },
			{ "M1d POSE camera", StagePoseCam, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 8> kSet8{ {
			{ "ready", StageReady, false },
			{ "R4 find the low wall from either side", StageLowSearch, true },
			{ "M1e POSE in at the low wall, 3 s idle", StagePoseIn, true },
			{ "M1e POSE walk along the wall", StagePoseWalk, true },
			{ "M1e POSE the first aim press", StagePoseAim, true },
			{ "M1e POSE out", StagePoseOut, true },
			{ "M1e POSE five ins and outs, hard reset", StagePoseTen, true },
			{ "summary", StageSummary, false },
		} };
		constexpr std::array<Stage, 5> kSet9{ {
			{ "ready", StageReady, false },
			{ "M1e POSE in at the tall wall, 3 s idle", StagePoseIn, true },
			{ "M1e POSE the first aim press", StagePoseAim, true },
			{ "M1e POSE out", StagePoseOut, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 4> kSet10{ {
			{ "ready", StageReady, false },
			{ "R4 find the low wall from either side", StageLowSearch, true },
			{ "M1f A3 the bisect of the first aim press, a to g", StageA3Bisect, true },
			{ "summary", StageSummary, false },
		} };
		constexpr std::array<Stage, 4> kSet11{ {
			{ "ready", StageReady, false },
			{ "R4 find the low wall from either side", StageLowSearch, true },
			{ "M1f A3 the weapon state and the first aim press", StageA3Weapon, true },
			{ "summary", StageSummary, false },
		} };
		constexpr std::array<Stage, 3> kSet12{ {
			{ "ready", StageReady, false },
			{ "M1f A3 three fresh poses on the tall wall, the first aim press of each", StageA3Rep, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 5> kSet13{ {
			{ "ready", StageReady, false },
			{ "M1g blind fire at the tall wall", StageBfTall, true },
			{ "M1g the camera in the pose, tall wall", StageCamPose, true },
			{ "summary", StageSummary, false },
			{ "summary", StageSummary, false },
		} };
		constexpr std::array<Stage, 6> kSet14{ {
			{ "ready", StageReady, false },
			{ "R4 find the low wall from either side", StageLowSearch, true },
			{ "M1g blind fire at the low wall", StageBfLow, true },
			{ "M1g the camera in the pose, low wall", StageCamPose, true },
			{ "summary", StageSummary, false },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 4> kSet15{ {
			{ "ready", StageReady, false },
			{ "R4 find the low wall from either side", StageLowSearch, true },
			{ "M1g the camera in the pose, low wall", StageCamPose, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 9> kSet16{ {
			{ "ready", StageReady, false },
			{ "R4 find the low wall from either side", StageLowSearch, true },
			{ "M3a the sheathed entry: the weapon drawn first, then the pose", StageRegSheathed, true },
			{ "M3a the pose with the weapon drawn, 3 s idle", StagePoseIn, true },
			{ "M3a the walk both ways", StagePoseWalk, true },
			{ "M3a the first aim press with a shot", StagePoseAim, true },
			{ "M3a out by pulling away and by Sneak", StagePoseOut, true },
			{ "M3a blind fire: a hold and a tap, last because of the crowd", StageBlindTail, true },
			{ "summary", StageSummary, false },
		} };
		constexpr std::array<Stage, 3> kSet17{ {
			{ "ready", StageReady, false },
			{ "M3a the tall wall: Basic cover, B, the camera offset", StageRegTall, true },
			{ "summary", StageSummary, false },
		} };


		// ======================================== M3b: the weapon controls in the pose (set 18) and tall cover by itself (set 19) ===============================================================
		struct SwapRun
		{
			double tPress{ 0.0 }, tLeave{ -1.0 }, tNone{ -1.0 }, tOff{ -1.0 }, tBack{ -1.0 };
			int    sessions0{ 0 };
			int    wsMin{ 9 };
		};

		// one weapon control pressed in the pose: the pose must end at once, the own graph come back, and the pose return by itself (a_expectBack) 1.0 s or more later with the weapon drawn
		void AddSwapCase(Seq& s, const char* a_name, const Rig::Key a_key, const double a_hold, const bool a_expectBack, const char* a_shot)
		{
			auto rr = std::make_shared<SwapRun>();
			s.Do([rr, a_name, a_key, a_hold] {
				*rr = SwapRun{};
				rr->tPress = Now();
				rr->sessions0 = CoverPose::GetInfo().sessions;
				REX::INFO("P3 SWAP [{}]: the control is pressed ({:.2f} s), pose {}, weapon state {}", a_name, a_hold, CoverPose::ToString(CoverPose::GetInfo().state), CoverPose::WeaponState());
				Rig::Tap(a_key, a_hold);
			});
			s.Until([rr, a_expectBack] {
				const double now = Now();
				const auto   pi = CoverPose::GetInfo();
				rr->wsMin = (std::min)(rr->wsMin, CoverPose::WeaponState());
				if (rr->tLeave < 0.0 && pi.state != CoverPose::State::Posed) {
					rr->tLeave = now;
				}
				if (rr->tNone < 0.0 && Cover::GetInfo().state == Core::CoverState::None) {
					rr->tNone = now;
				}
				if (rr->tLeave > 0.0 && rr->tOff < 0.0 && pi.state == CoverPose::State::Off) {
					rr->tOff = now;
				}
				if (rr->tOff > 0.0 && rr->tBack < 0.0 && pi.state == CoverPose::State::Posed && pi.sessions > rr->sessions0) {
					rr->tBack = now;
				}
				return a_expectBack ? rr->tBack > 0.0 : now - rr->tPress >= 4.0;
			}, 10.0);
			s.Wait(0.9);
			if (a_shot && *a_shot) {
				AddShot(s, a_shot);
			}
			s.Do([rr, a_name, a_expectBack] {
				const auto pi = CoverPose::GetInfo();
				const int  ws = CoverPose::WeaponState();
				bool       ok = rr->tLeave > 0.0 && rr->tLeave - rr->tPress <= 0.35 && rr->tOff > 0.0 && Cover::GetInfo().state != Core::CoverState::None;
				if (a_expectBack) {
					ok = ok && rr->tBack > 0.0 && rr->tBack - rr->tOff >= 0.9 && pi.state == CoverPose::State::Posed && ws == 3;
				} else {
					ok = ok && pi.state == CoverPose::State::Off && ws != 3;
				}
				Result(std::format("P3 SWAP {}: the pose left {:.2f} s after the press, the own graph back {:.2f} s, Phase 3 {} (cover lost {}), the pose back {:.2f} s after the own graph (expected {}), weapon state min {} now {}, pose {} -> {}", a_name,
					rr->tLeave > 0.0 ? rr->tLeave - rr->tPress : -1.0, rr->tOff > 0.0 ? rr->tOff - rr->tPress : -1.0, Core::ToString(Cover::GetInfo().state), rr->tNone > 0.0 ? "yes" : "no", rr->tBack > 0.0 && rr->tOff > 0.0 ? rr->tBack - rr->tOff : -1.0,
					a_expectBack ? "1.0 or more" : "no pose", rr->wsMin, ws, CoverPose::ToString(pi.state), Verdict(ok)));
				Summarise(std::string("SWAP ") + a_name, ok);
			});
		}

		bool StageSwap(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				s.Do([] { Config::Mutable().coverTrigger = 3; });
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.5);
				AddShot(s, "sw_0");
				AddSwapCase(s, "number key 2", Rig::kKey2, 0.12, true, "sw_k2");
				s.Wait(1.0);
				AddSwapCase(s, "number key 3", Rig::kKey3, 0.12, true, "sw_k3");
				s.Wait(1.0);
				AddSwapCase(s, "number key 1", Rig::kKey1, 0.12, true, "sw_k1");
				s.Wait(1.0);
				AddSwapCase(s, "the ready/holster control held 0.8 s", Rig::kKeyR, 0.8, false, "sw_holster");
				// the weapon drawn again by a tap of the control: the pose comes back by itself
				{
					auto rr = std::make_shared<SwapRun>();
					s.Do([rr] {
						*rr = SwapRun{};
						rr->tPress = Now();
						rr->sessions0 = CoverPose::GetInfo().sessions;
						REX::INFO("P3 SWAP [draw again]: tap of the ready control, weapon state {}, pose {}", CoverPose::WeaponState(), CoverPose::ToString(CoverPose::GetInfo().state));
						Rig::Tap(Rig::kKeyR, 0.08);
					});
					s.Until([rr] {
						if (rr->tBack < 0.0 && CoverPose::GetInfo().state == CoverPose::State::Posed && CoverPose::GetInfo().sessions > rr->sessions0) {
							rr->tBack = Now();
						}
						return rr->tBack > 0.0;
					}, 8.0);
					s.Wait(0.8);
					AddShot(s, "sw_redraw");
					s.Do([rr] {
						const bool ok = rr->tBack > 0.0 && CoverPose::WeaponState() == 3;
						Result(std::format("P3 SWAP draw again: the weapon state {}, the pose back {:.2f} s after the tap (the 0.5 s of a drawn weapon and the 1.0 s are the least) -> {}", CoverPose::WeaponState(), rr->tBack > 0.0 ? rr->tBack - rr->tPress : -1.0, Verdict(ok)));
						Summarise("SWAP draw again: the pose comes back", ok);
					});
				}
				s.Do([] { Cover::ForceReset("p3 M3b swap end"); });
				s.Wait(1.0);
			}
			return Run(s, c.now);
		}

		// the tall wall of Save29, mode 3 with iAutoCoverTall 2: nothing with the weapon sheathed, an entry after 0.45 s with it drawn, B works either way
		bool StageTallAuto(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto d = std::make_shared<R10Data>();
				auto e = std::make_shared<CaseData>();
				auto sess0 = std::make_shared<int>(0);
				s.Do([sess0] { *sess0 = CoverPose::GetInfo().sessions; Config::Mutable().coverPoses = true; Config::Mutable().coverCamera = true; });
				AddWeaponTo(s, false);
				AddPrepare(s, 0, Base(), std::nullopt);
				R10Begin(s, d);
				s.Do([d] {
					d->tPush = Now();
					Rig::Tap(Rig::kKeyS, 2.6);
				});
				s.Until([d] {
					if (FreshCandidate(Core::CoverKind::High)) {
						d->sawFresh = true;
					}
					if (Cover::GetInfo().state != Core::CoverState::None) {
						d->everIn = true;
					}
					return Now() - d->tPush >= 2.8;
				}, 5.0);
				s.Do([d] {
					const auto i = Cover::GetInfo();
					const bool ok = !d->everIn && i.state == Core::CoverState::None && d->sawFresh;
					Result(std::format("P3 M3b tall, weapon sheathed, S held 2.6 s toward the tall wall of Save29 (iAutoCoverTall {}): a fresh HIGH answer {}; the state stayed None: {} -> {}", Config::Get().autoCoverTall, d->sawFresh ? "yes" : "NO", d->everIn ? "NO, cover was taken" : "yes", Verdict(ok)));
					Summarise("M3b tall wall, weapon sheathed: no entry", ok);
					Rig::ReleaseEverything("p3 M3b tall a");
				});
				s.Wait(0.4);
				AddWeaponTo(s, true);
				AddPrepare(s, 0, Base(), std::nullopt);
				R10Begin(s, d);
				s.Do([d] {
					d->tPush = Now();
					Rig::Tap(Rig::kKeyS, 3.0);
				});
				s.Until([d] {
					if (d->tIn < 0.0 && Cover::GetInfo().state == Core::CoverState::InCover) {
						d->tIn = Now();
					}
					return d->tIn > 0.0 || Now() - d->tPush > 3.2;
				}, 5.0);
				s.Wait(0.6);
				s.Do([d, sess0] {
					const auto   i = Cover::GetInfo();
					const double dt = d->tIn > 0.0 ? d->tIn - d->tPush : -1.0;
					const bool   ok = d->tIn > 0.0 && i.state == Core::CoverState::InCover && i.candidate.kind == Core::CoverKind::High && CoverPose::GetInfo().sessions == *sess0 && !CoverPose::Busy();
					Result(std::format("P3 M3b tall, weapon drawn (state {}), S held toward the tall wall: in cover (reached) {:.2f} s after the push (fAutoCoverTimeTall {:.2f} plus the sensor and the slide), Phase 3 {} at a {} cover, no pose (sessions {} -> {}) -> {}", CoverPose::WeaponState(), dt,
						Config::Get().autoCoverTimeTall, Core::ToString(i.state), Core::ToString(i.candidate.kind), *sess0, CoverPose::GetInfo().sessions, Verdict(ok)));
					Summarise("M3b tall wall, weapon drawn: automatic entry, no pose", ok);
					Rig::ReleaseEverything("p3 M3b tall b");
				});
				AddLeaveSneak(s, e);
				s.Do([e] {
					const bool ok = e->tNone > 0.0 && e->tNone - e->leaveT <= 0.5;
					Result(std::format("P3 M3b tall: B leaves the automatic cover: state None {:.2f} s after the press -> {}", e->tNone > 0.0 ? e->tNone - e->leaveT : -1.0, Verdict(ok)));
					Summarise("M3b tall: B leaves", ok);
				});
				s.Wait(0.6);
				// B pushing toward the wall enters with the weapon sheathed too
				AddWeaponTo(s, false);
				AddPrepare(s, 0, Base(), std::nullopt);
				s.Do([] { Cover::ForceReset("p3 M3b tall c"); Config::Mutable().coverTrigger = 1; });
				AddEnter(s, e, Rig::kKeyS, Core::CoverKind::High);
				s.Do([e] { LogEnter("M3b tall, weapon sheathed: B enters", e, 0, true); });
				AddLeaveSneak(s, e);
				s.Do([e] {
					const bool ok = e->tNone > 0.0 && e->tNone - e->leaveT <= 0.5;
					Result(std::format("P3 M3b tall, weapon sheathed: B leaves: state None {:.2f} s after the press -> {}", e->tNone > 0.0 ? e->tNone - e->leaveT : -1.0, Verdict(ok)));
					Summarise("M3b tall, sheathed: B enters and leaves", ok);
					Cover::ForceReset("p3 M3b tall end");
					Config::Mutable().coverTrigger = 3;
					Config::Mutable().coverCamera = false;
				});
				s.Wait(0.5);
			}
			return Run(s, c.now);
		}

		constexpr std::array<Stage, 4> kSet18{ {
			{ "ready", StageReady, false },
			{ "R4 find the low wall from either side", StageLowSearch, true },
			{ "M3b the weapon controls in the pose", StageSwap, true },
			{ "summary", StageSummary, false },
		} };
		constexpr std::array<Stage, 3> kSet19{ {
			{ "ready", StageReady, false },
			{ "M3b tall cover by itself, weapon sheathed and drawn", StageTallAuto, true },
			{ "summary", StageSummary, false },
		} };


		// ======================================== M3c: the reload in the pose (set 20) =====================================================================================================
		// graph variables that might say "reloading" (a probe: unresolved names read -1 and are not logged), watched together with the cover variables and the actor state words
		struct RlProbe
		{
			double                         t0{ 0.0 };
			std::map<std::string, double>  last;
			std::string                    lastFlags;
		};

		void RlProbeTick(const std::shared_ptr<RlProbe>& pr)
		{
			static constexpr const char* kNames[] = { "iSyncReloadState", "iIsReloading", "bIsReloading", "iReloadState", "iSyncReload", "bReloading", "iSyncGunState", "iSyncWeaponState", "iSyncAmmoState", "iSyncReloadType",
				"bIsReloadingWeapon", "iIsReload", "iReload", "iSyncGunAction", "iSyncWeaponAction", "iIsGunReloading", "iSyncCoverReload", "iSyncCoverStates", "iIsBlindFiring", "iIsSighted", "iSyncSighted", "bIsInCover",
				"iSyncRelaxedReadyAlert", "iSyncGunDown", "iIsGunDown", "iSyncWeaponDrawState", "iSyncActorStance", "iPartialCover", "bIsEnteringCover", "bIsExitingCover" };
			const double t = Now() - pr->t0;
			for (const char* n : kNames) {
				const double v = CoverPose::VarValue(n);
				const auto   it = pr->last.find(n);
				if (it == pr->last.end()) {
					pr->last[n] = v;
					if (v > -0.5 || v < -1.5) {
						REX::INFO("P3 RL var {} = {:g} at +{:.2f} s (first read)", n, v, t);
					}
				} else if (it->second != v) {
					REX::INFO("P3 RL var {}: {:g} -> {:g} at +{:.2f} s (pose {}, weapon state {})", n, it->second, v, t, CoverPose::ToString(CoverPose::GetInfo().state), CoverPose::WeaponState());
					it->second = v;
				}
			}
			const auto flags = PlayerControl::ActorFlags();
			if (flags != pr->lastFlags) {
				REX::INFO("P3 RL {} at +{:.2f} s", flags, t);
				pr->lastFlags = flags;
			}
		}

		bool StageReloadPose(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				auto pr = std::make_shared<RlProbe>();
				auto rr = std::make_shared<SwapRun>();
				s.Do([pr] { Config::Mutable().coverTrigger = 3; pr->t0 = Now(); });
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.5);
				AddShot(s, "rl_0");
				// a tap of the fire control: blind fire, a few rounds out of the magazine
				s.Do([] { Rig::MouseLeft(true); });
				s.Wait(0.12);
				s.Do([] { Rig::MouseLeft(false); });
				s.Wait(2.6);
				AddShot(s, "rl_1");
				// the reload control tapped in the pose
				s.Do([rr, pr] {
					*rr = SwapRun{};
					rr->tPress = Now();
					rr->sessions0 = CoverPose::GetInfo().sessions;
					pr->t0 = rr->tPress;
					REX::INFO("P3 RL manual: the reload control is tapped, pose {}, weapon state {}", CoverPose::ToString(CoverPose::GetInfo().state), CoverPose::WeaponState());
					Rig::Tap(Rig::kKeyR, 0.12);
				});
				s.Until([rr, pr] {
					RlProbeTick(pr);
					const double now = Now();
					const auto   pi = CoverPose::GetInfo();
					if (rr->tLeave < 0.0 && pi.state != CoverPose::State::Posed) {
						rr->tLeave = now;
					}
					if (rr->tLeave > 0.0 && rr->tOff < 0.0 && pi.state == CoverPose::State::Off) {
						rr->tOff = now;
					}
					if (rr->tOff > 0.0 && rr->tBack < 0.0 && pi.state == CoverPose::State::Posed && pi.sessions > rr->sessions0) {
						rr->tBack = now;
					}
					return rr->tBack > 0.0;
				}, 14.0);
				s.Wait(1.0);
				AddShot(s, "rl_2");
				s.Do([rr] {
					const auto pi = CoverPose::GetInfo();
					const bool ok = rr->tLeave > 0.0 && rr->tLeave - rr->tPress <= 0.35 && rr->tOff > 0.0 && rr->tBack > 0.0 && rr->tBack - rr->tPress >= Config::Get().poseReloadTime && pi.state == CoverPose::State::Posed && CoverPose::WeaponState() == 3;
					Result(std::format("P3 RL manual reload in the pose: the pose left {:.2f} s after the tap, the own graph back {:.2f} s, the pose back {:.2f} s after the tap (fPoseReloadTime {:.1f} from the made tap), weapon state {}, pose {} (the ammunition: the HUD in rl_1.jpg against rl_2.jpg) -> {}",
						rr->tLeave > 0.0 ? rr->tLeave - rr->tPress : -1.0, rr->tOff > 0.0 ? rr->tOff - rr->tPress : -1.0, rr->tBack > 0.0 ? rr->tBack - rr->tPress : -1.0, Config::Get().poseReloadTime, CoverPose::WeaponState(), CoverPose::ToString(pi.state), Verdict(ok)));
					Summarise("RL manual reload: the pose ends first and comes back", ok);
				});
				// the magazine emptied by blind fire: the engine reloads by itself (the probe says what the graph does; nothing is decided from it yet)
				auto ar = std::make_shared<SwapRun>();
				s.Do([pr, ar] {
					pr->t0 = Now();
					*ar = SwapRun{};
					ar->sessions0 = CoverPose::GetInfo().sessions;
					REX::INFO("P3 RL auto: the fire control is held 3.5 s");
					Rig::MouseLeft(true);
				});
				s.Until([pr, ar] {
					RlProbeTick(pr);
					const double now = Now();
					const auto   pi = CoverPose::GetInfo();
					if (ar->tPress <= 0.0 && CoverPose::VarValue("bIsReloading") > 0.5) {
						ar->tPress = now;  // the reload began
					}
					if (ar->tPress > 0.0 && ar->tLeave < 0.0 && pi.state != CoverPose::State::Posed) {
						ar->tLeave = now;
					}
					if (ar->tLeave > 0.0 && ar->tOff < 0.0 && pi.state == CoverPose::State::Off) {
						ar->tOff = now;
					}
					if (ar->tOff > 0.0 && ar->tBack < 0.0 && pi.state == CoverPose::State::Posed && pi.sessions > ar->sessions0) {
						ar->tBack = now;
					}
					return now - pr->t0 >= 3.5;
				}, 5.0);
				s.Do([pr] { Rig::MouseLeft(false); pr->t0 = Now(); });
				s.Until([pr, ar] {
					RlProbeTick(pr);
					const double now = Now();
					const auto   pi = CoverPose::GetInfo();
					if (ar->tPress <= 0.0 && CoverPose::VarValue("bIsReloading") > 0.5) {
						ar->tPress = now;
					}
					if (ar->tPress > 0.0 && ar->tLeave < 0.0 && pi.state != CoverPose::State::Posed) {
						ar->tLeave = now;
					}
					if (ar->tLeave > 0.0 && ar->tOff < 0.0 && pi.state == CoverPose::State::Off) {
						ar->tOff = now;
					}
					if (ar->tOff > 0.0 && ar->tBack < 0.0 && pi.state == CoverPose::State::Posed && pi.sessions > ar->sessions0) {
						ar->tBack = now;
					}
					return now - pr->t0 >= 9.0 || (ar->tBack > 0.0 && now - ar->tBack >= 1.5);
				}, 12.0);
				AddShot(s, "rl_3");
				s.Do([ar] {
					const bool ok = ar->tPress > 0.0 && ar->tLeave > 0.0 && ar->tLeave - ar->tPress <= 0.15 && ar->tOff > 0.0 && ar->tBack > 0.0 && CoverPose::WeaponState() == 3;
					Result(std::format("P3 RL auto reload (the magazine emptied by blind fire): bIsReloading rose, the pose left {:.2f} s later, the own graph back {:.2f} s, the pose back {:.2f} s after the reload began, weapon state {} (the ammunition: the HUD in rl_3.jpg) -> {}",
						ar->tLeave > 0.0 && ar->tPress > 0.0 ? ar->tLeave - ar->tPress : -1.0, ar->tOff > 0.0 && ar->tPress > 0.0 ? ar->tOff - ar->tPress : -1.0, ar->tBack > 0.0 && ar->tPress > 0.0 ? ar->tBack - ar->tPress : -1.0, CoverPose::WeaponState(), Verdict(ok)));
					Summarise("RL auto reload: the pose ends when the engine's reload begins and comes back", ok);
				});
				s.Do([] { Cover::ForceReset("p3 M3c reload end"); });
				s.Wait(0.8);
			}
			return Run(s, c.now);
		}

		constexpr std::array<Stage, 4> kSet20{ {
			{ "ready", StageReady, false },
			{ "R4 find the low wall from either side", StageLowSearch, true },
			{ "M3c the reload in the pose", StageReloadPose, true },
			{ "summary", StageSummary, false },
		} };


		// ======================================== M3c: the pitch probe (set 21) and the locks (set 22) ======================================================================================
		struct PitchRun
		{
			double t0{ 0.0 }, next{ 0.0 };
			float  camMin[9], camMax[9];
			float  dMin[6], dMax[6];
			float  angMin{ 1e9f }, angMax{ -1e9f };
			float  mMin[48], mMax[48];  // the process data: +0x500 to +0x5BC
			float  tMin[52], tMax[52];  // ThirdPersonState floats: +0x160 to +0x1AC (20) and +0x220 to +0x29C (32)
			int    n{ 0 };
			PitchRun()
			{
				for (auto& v : camMin) { v = 1e9f; }
				for (auto& v : camMax) { v = -1e9f; }
				for (auto& v : dMin) { v = 1e9f; }
				for (auto& v : dMax) { v = -1e9f; }
				for (auto& v : mMin) { v = 1e9f; }
				for (auto& v : mMax) { v = -1e9f; }
				for (auto& v : tMin) { v = 1e9f; }
				for (auto& v : tMax) { v = -1e9f; }
			}
		};

		void PitchSample(const std::shared_ptr<PitchRun>& pr, const char* a_phase, const double a_t, const bool a_log)
		{
			float cam[9]{};
			float d[6]{};
			int   aimActive{ -1 };
			const bool haveCam = Camera::ReadRootRot(cam);
			const bool haveD = CoverPose::ReadDirectAt(d, aimActive);
			const auto& snap = Frame::Latest();
			if (haveCam) {
				for (int i = 0; i < 9; ++i) {
					pr->camMin[i] = (std::min)(pr->camMin[i], cam[i]);
					pr->camMax[i] = (std::max)(pr->camMax[i], cam[i]);
				}
			}
			if (haveD) {
				for (int i = 0; i < 6; ++i) {
					pr->dMin[i] = (std::min)(pr->dMin[i], d[i]);
					pr->dMax[i] = (std::max)(pr->dMax[i], d[i]);
				}
			}
			float mw[48]{};
			if (CoverPose::ReadMiddleHighFloats(0x500, 48, mw)) {
				for (int i = 0; i < 48; ++i) {
					pr->mMin[i] = (std::min)(pr->mMin[i], mw[i]);
					pr->mMax[i] = (std::max)(pr->mMax[i], mw[i]);
				}
			}
			float tw[52]{};
			if (Camera::ReadTpsFloats(0x160, 20, tw) && Camera::ReadTpsFloats(0x220, 32, tw + 20)) {
				for (int i = 0; i < 52; ++i) {
					pr->tMin[i] = (std::min)(pr->tMin[i], tw[i]);
					pr->tMax[i] = (std::max)(pr->tMax[i], tw[i]);
				}
			}
			pr->angMin = (std::min)(pr->angMin, snap.angle.x);
			pr->angMax = (std::max)(pr->angMax, snap.angle.x);
			++pr->n;
			if (a_log) {
				REX::INFO("P3 PITCH {} t={:.2f}: cam rot rows ({:+.3f},{:+.3f},{:+.3f}) ({:+.3f},{:+.3f},{:+.3f}) ({:+.3f},{:+.3f},{:+.3f}) | DirectAt head {:+.3f} pitch {:+.3f} headMin {:+.3f} headMax {:+.3f} pitchMin {:+.3f} pitchMax {:+.3f} aimActive {} | angle ({:+.3f},{:+.3f},{:+.3f}) | sighted {:g}/{:g}, pose {}, state {}",
					a_phase, a_t, cam[0], cam[1], cam[2], cam[3], cam[4], cam[5], cam[6], cam[7], cam[8], d[0], d[1], d[2], d[3], d[4], d[5], aimActive, snap.angle.x, snap.angle.y, snap.angle.z,
					CoverPose::VarValue("iIsSighted"), CoverPose::VarValue("iSyncSighted"), CoverPose::ToString(CoverPose::GetInfo().state), Core::ToString(Cover::GetInfo().state));
			}
		}

		// one phase: a_aim = the aim held; the mouse goes up 300 counts in 1 s, rests, down 600 in 2 s, rests (15 counts every 0.05 s)
		void AddPitchPhase(Seq& s, const char* a_phase, const bool a_aim)
		{
			auto pr = std::make_shared<PitchRun>();
			s.Do([pr, a_phase, a_aim] {
				*pr = PitchRun{};
				pr->t0 = Now();
				pr->next = pr->t0;
				REX::INFO("P3 PITCH {}: starts (aim held {}, pose {}, weapon state {})", a_phase, a_aim, CoverPose::ToString(CoverPose::GetInfo().state), CoverPose::WeaponState());
				Camera::ProbeBegin(a_phase);
				if (a_aim) {
					Rig::MouseRight(true);
				}
			});
			s.Until([pr, a_phase] {
				const double now = Now();
				const double t = now - pr->t0;
				if (now >= pr->next) {
					pr->next = now + 0.05;
					if (t >= 0.5 && t < 1.5) {
						Rig::MouseMove(0, -20);
					} else if (t >= 2.0 && t < 4.0) {
						Rig::MouseMove(0, 20);
					}
					PitchSample(pr, a_phase, t, true);
				}
				return t >= 5.0;
			}, 7.0);
			s.Do([pr, a_phase, a_aim] {
				Camera::ProbeEnd();
				if (a_aim) {
					Rig::MouseRight(false);
				}
				REX::INFO("P3 PITCH {} view pitch (the camera root's forward z, in degrees): {:+.1f} .. {:+.1f}, a range of {:.1f} degrees", a_phase, std::asin(std::clamp(pr->camMin[5], -1.0f, 1.0f)) * 57.29578f, std::asin(std::clamp(pr->camMax[5], -1.0f, 1.0f)) * 57.29578f,
					(std::asin(std::clamp(pr->camMax[5], -1.0f, 1.0f)) - std::asin(std::clamp(pr->camMin[5], -1.0f, 1.0f))) * 57.29578f);
				Result(std::format("P3 PITCH {} summary: {} samples; the camera root's rotation entries (rows) min..max: [{:+.3f}..{:+.3f}] [{:+.3f}..{:+.3f}] [{:+.3f}..{:+.3f}] / [{:+.3f}..{:+.3f}] [{:+.3f}..{:+.3f}] [{:+.3f}..{:+.3f}] / [{:+.3f}..{:+.3f}] [{:+.3f}..{:+.3f}] [{:+.3f}..{:+.3f}]; DirectAt head {:+.3f}..{:+.3f} pitch {:+.3f}..{:+.3f} headMin {:+.3f}..{:+.3f} headMax {:+.3f}..{:+.3f} pitchMin {:+.3f}..{:+.3f} pitchMax {:+.3f}..{:+.3f}; the actor's angle x {:+.3f}..{:+.3f}",
					a_phase, pr->n, pr->camMin[0], pr->camMax[0], pr->camMin[1], pr->camMax[1], pr->camMin[2], pr->camMax[2], pr->camMin[3], pr->camMax[3], pr->camMin[4], pr->camMax[4], pr->camMin[5], pr->camMax[5], pr->camMin[6], pr->camMax[6], pr->camMin[7], pr->camMax[7], pr->camMin[8], pr->camMax[8],
					pr->dMin[0], pr->dMax[0], pr->dMin[1], pr->dMax[1], pr->dMin[2], pr->dMax[2], pr->dMin[3], pr->dMax[3], pr->dMin[4], pr->dMax[4], pr->dMin[5], pr->dMax[5], pr->angMin, pr->angMax));
			});
			s.Do([pr, a_phase] {
				std::string changed, still;
				for (int i = 0; i < 52; ++i) {
					const int off = i < 20 ? 0x160 + 4 * i : 0x220 + 4 * (i - 20);
					if (pr->tMax[i] - pr->tMin[i] > 1e-4f) {
						changed += std::format(" +{:03X}[{:+.3f}..{:+.3f}]", off, pr->tMin[i], pr->tMax[i]);
					} else {
						still += std::format(" +{:03X}={:+.3f}", off, pr->tMin[i]);
					}
				}
				REX::INFO("P3 PITCH {} ThirdPersonState floats that changed:{} | constant:{}", a_phase, changed, still);
				std::string mc;
				for (int i = 0; i < 48; ++i) {
					if (pr->mMax[i] - pr->mMin[i] > 1e-4f) {
						mc += std::format(" +{:03X}[{:+.3f}..{:+.3f}]", 0x500 + 4 * i, pr->mMin[i], pr->mMax[i]);
					}
				}
				REX::INFO("P3 PITCH {} process data (+0x500..+0x5BC) floats that changed:{}", a_phase, mc.empty() ? " none" : mc);
			});
			s.Wait(0.8);
		}

		bool StagePitch(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				s.Do([] { Config::Mutable().coverTrigger = 3; Config::Mutable().poseFreePitch = Config::Get().poseFreePitch; });
				AddWeaponTo(s, true);
				AddPitchPhase(s, "own_idle", false);
				AddPitchPhase(s, "own_aim", true);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.5);
				AddPitchPhase(s, "pose_idle", false);
				AddPitchPhase(s, "pose_aim", true);
				s.Do([] { Cover::ForceReset("p3 M3c pitch end"); });
				s.Wait(0.8);
			}
			return Run(s, c.now);
		}

		constexpr std::array<Stage, 4> kSet21{ {
			{ "ready", StageReady, false },
			{ "R4 find the low wall from either side", StageLowSearch, true },
			{ "M3c the pitch probe: own graph, pose idle, pose aim", StagePitch, true },
			{ "summary", StageSummary, false },
		} };

		// the locks: the reload inside the pose (the tap and the empty magazine), a quick-key and the holster hold in the pose
		bool StageLocks(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				auto st = std::make_shared<SwapRun>();
				auto lockSess = std::make_shared<int>(0);
				s.Do([] { Config::Mutable().coverTrigger = 3; });
				AddWeaponTo(s, true);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.5);
				AddShot(s, "lk_0");
				// a tap of the fire control: a few rounds out of the magazine
				s.Do([] { Rig::MouseLeft(true); });
				s.Wait(0.12);
				s.Do([] { Rig::MouseLeft(false); });
				s.Wait(2.6);
				AddShot(s, "lk_1");
				// the reload control tapped: the reload stays in the pose
				s.Do([st, lockSess] {
					*st = SwapRun{};
					st->tPress = Now();
					st->sessions0 = CoverPose::GetInfo().sessions;
					*lockSess = CoverPose::GetInfo().sessions;
					REX::INFO("P3 LOCK reload tap: the reload control is tapped, bPoseReload {}, pose {}", Config::Get().poseReload, CoverPose::ToString(CoverPose::GetInfo().state));
					Rig::Tap(Rig::kKeyR, 0.12);
				});
				s.Until([st] {
					const double now = Now();
					if (st->tOff < 0.0 && CoverPose::VarValue("bIsReloading") > 0.5) {
						st->tOff = now;  // the reload began
					}
					if (CoverPose::GetInfo().state != CoverPose::State::Posed && st->tLeave < 0.0) {
						st->tLeave = now;
					}
					return now - st->tPress >= 5.5;
				}, 8.0);
				AddShot(s, "lk_2");
				s.Do([st] {
					const bool ok = st->tOff > 0.0 && st->tLeave < 0.0 && CoverPose::GetInfo().state == CoverPose::State::Posed;
					Result(std::format("P3 LOCK reload tap in the pose (bPoseReload {}): bIsReloading rose {:.2f} s after the tap, the pose {} left (state {}) (the ammunition: the HUD in lk_1.jpg against lk_2.jpg) -> {}", Config::Get().poseReload, st->tOff > 0.0 ? st->tOff - st->tPress : -1.0,
						st->tLeave > 0.0 ? "was" : "was never", CoverPose::ToString(CoverPose::GetInfo().state), Verdict(ok)));
					Summarise("LOCK reload tap stays in the pose", ok);
				});
				// the magazine emptied by blind fire: the engine's own reload, in the pose
				s.Do([st] { *st = SwapRun{}; st->tPress = Now(); REX::INFO("P3 LOCK auto reload: the fire control is held 3.5 s"); Rig::MouseLeft(true); });
				s.Until([st] {
					const double now = Now();
					if (st->tOff < 0.0 && CoverPose::VarValue("bIsReloading") > 0.5) {
						st->tOff = now;
					}
					if (CoverPose::GetInfo().state != CoverPose::State::Posed && st->tLeave < 0.0) {
						st->tLeave = now;
					}
					return now - st->tPress >= 3.5;
				}, 5.0);
				s.Do([st] { Rig::MouseLeft(false); });
				s.Until([st] {
					const double now = Now();
					if (st->tOff < 0.0 && CoverPose::VarValue("bIsReloading") > 0.5) {
						st->tOff = now;
					}
					if (CoverPose::GetInfo().state != CoverPose::State::Posed && st->tLeave < 0.0) {
						st->tLeave = now;
					}
					return now - st->tPress >= 10.0;
				}, 12.0);
				AddShot(s, "lk_3");
				s.Do([st] {
					const bool ok = st->tOff > 0.0 && st->tLeave < 0.0 && CoverPose::GetInfo().state == CoverPose::State::Posed;
					Result(std::format("P3 LOCK auto reload in the pose (bPoseReload {}): bIsReloading rose {:.2f} s after the fire control went down, the pose {} left, state {} (the ammunition: the HUD in lk_3.jpg) -> {}", Config::Get().poseReload, st->tOff > 0.0 ? st->tOff - st->tPress : -1.0,
						st->tLeave > 0.0 ? "was" : "was never", CoverPose::ToString(CoverPose::GetInfo().state), Verdict(ok)));
					Summarise("LOCK auto reload stays in the pose", ok);
				});
				// a quick-key in the pose: nothing changes, the pose stays
				for (const auto& [label, key] : { std::pair<const char*, Rig::Key>{ "number key 2", Rig::kKey2 }, std::pair<const char*, Rig::Key>{ "number key 3", Rig::kKey3 } }) {
					auto kr = std::make_shared<SwapRun>();
					const std::string name = label;
					s.Do([kr, name, key] {
						*kr = SwapRun{};
						kr->tPress = Now();
						kr->sessions0 = CoverPose::GetInfo().sessions;
						kr->wsMin = CoverPose::WeaponState();
						REX::INFO("P3 LOCK [{}]: pressed in the pose, weapon state {}", name, CoverPose::WeaponState());
						Rig::Tap(key, 0.12);
					});
					s.Until([kr] {
						kr->wsMin = (std::min)(kr->wsMin, CoverPose::WeaponState());
						if (CoverPose::GetInfo().state != CoverPose::State::Posed && kr->tLeave < 0.0) {
							kr->tLeave = Now();
						}
						return Now() - kr->tPress >= 3.0;
					}, 5.0);
					AddShot(s, std::string("lk_") + (key.vk == Rig::kKey2.vk ? "k2" : "k3"));
					s.Do([kr, name] {
						const bool ok = kr->tLeave < 0.0 && CoverPose::GetInfo().state == CoverPose::State::Posed && CoverPose::GetInfo().sessions == kr->sessions0 && kr->wsMin == 3;
						Result(std::format("P3 LOCK {} in the pose (iPoseWeaponSwap {}): the pose {} left, weapon state min {} (the weapon in the picture and the HUD unchanged), pose {} -> {}", name, Config::Get().poseWeaponSwap, kr->tLeave > 0.0 ? "was" : "was never", kr->wsMin, CoverPose::ToString(CoverPose::GetInfo().state), Verdict(ok)));
						Summarise(std::string("LOCK ") + name + " changes nothing", ok);
					});
				}
				// the holster hold in the pose: the weapon stays drawn, the pose stays
				{
					auto hr = std::make_shared<SwapRun>();
					s.Do([hr] {
						*hr = SwapRun{};
						hr->tPress = Now();
						hr->sessions0 = CoverPose::GetInfo().sessions;
						hr->wsMin = CoverPose::WeaponState();
						REX::INFO("P3 LOCK holster hold: the ready control held 0.9 s in the pose, weapon state {}", CoverPose::WeaponState());
						Rig::Tap(Rig::kKeyR, 0.9);
					});
					s.Until([hr] {
						hr->wsMin = (std::min)(hr->wsMin, CoverPose::WeaponState());
						if (CoverPose::GetInfo().state != CoverPose::State::Posed && hr->tLeave < 0.0) {
							hr->tLeave = Now();
						}
						return Now() - hr->tPress >= 3.5;
					}, 6.0);
					AddShot(s, "lk_holster");
					s.Do([hr] {
						const bool ok = hr->tLeave > 0.0;  // by design since card M3b: the holster hold ends the pose (and holsters)
						Result(std::format("P3 LOCK holster hold 0.9 s in the pose (iPoseWeaponSwap {}): the pose {} left, weapon state min {} (3 = never holstered), pose {} -> {}", Config::Get().poseWeaponSwap, hr->tLeave > 0.0 ? "was" : "was never", hr->wsMin, CoverPose::ToString(CoverPose::GetInfo().state), Verdict(ok)));
						Summarise("LOCK holster hold: the pose ends (by design)", ok);
					});
				}
				s.Do([] { Cover::ForceReset("p3 M3c locks end"); });
				s.Wait(1.5);
				// out of the pose the quick-keys work again: number key 2 changes the weapon (the HUD in lk_after.jpg)
				s.Do([] { REX::INFO("P3 LOCK after the pose: favourites locked {}, pose {}; number key 2 is pressed", CoverPose::FavoritesLocked(), CoverPose::ToString(CoverPose::GetInfo().state)); Rig::Tap(Rig::kKey2, 0.12); });
				s.Wait(2.5);
				AddShot(s, "lk_after");
				s.Wait(0.5);
			}
			return Run(s, c.now);
		}

		constexpr std::array<Stage, 4> kSet22{ {
			{ "ready", StageReady, false },
			{ "R4 find the low wall from either side", StageLowSearch, true },
			{ "M3c the locks: reload, quick-keys and the holster hold in the pose", StageLocks, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 5> kSet23{ {
			{ "ready", StageReady, false },
			{ "R4 find the low wall from either side", StageLowSearch, true },
			{ "M3c the pitch probe: own graph, pose idle, pose aim", StagePitch, true },
			{ "M3c the locks: reload, quick-keys and the holster hold in the pose", StageLocks, true },
			{ "summary", StageSummary, false },
		} };

		// the aim in the pose: a shot with the view pitched up and one pitched down (the impacts in the pictures against the crosshair), then the favourites work again out of the pose
		bool StageHighLow(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				s.Do([] { Config::Mutable().coverTrigger = 3; });
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.5);
				s.Do([] { Rig::MouseRight(true); });
				s.Wait(1.2);
				for (int pass = 0; pass < 2; ++pass) {
					const int dir = pass == 0 ? -1 : 1;  // negative counts: the view up
					auto      t0 = std::make_shared<double>(0.0);
					s.Do([t0] { *t0 = Now(); });
					s.Until([t0, dir] {
						if (Now() - *t0 < 1.0) {
							Rig::MouseMove(0, dir * 12);
						}
						return Now() - *t0 >= 1.2;
					}, 3.0);
					s.Do([] { Rig::MouseLeft(true); });
					s.Wait(0.12);
					s.Do([] { Rig::MouseLeft(false); });
					s.Wait(0.6);
					s.Do([pass] { REX::INFO("P3 HIGHLOW {}: the shot is fired, pose {}", pass == 0 ? "view up" : "view down (more)", CoverPose::ToString(CoverPose::GetInfo().state)); });
					AddShot(s, pass == 0 ? "pf_up" : "pf_down");
					if (pass == 0) {
						// down twice as far
						auto t1 = std::make_shared<double>(0.0);
						s.Do([t1] { *t1 = Now(); });
						s.Until([t1] {
							if (Now() - *t1 < 1.0) {
								Rig::MouseMove(0, 12);
							}
							return Now() - *t1 >= 1.2;
						}, 3.0);
					}
				}
				s.Do([] { Rig::MouseRight(false); Cover::ForceReset("p3 M3d highlow end"); });
				s.Wait(1.5);
				s.Do([] { REX::INFO("P3 LOCK after the pose: favourites locked {}; number key 2 is pressed", CoverPose::FavoritesLocked()); Rig::Tap(Rig::kKey2, 0.12); });
				s.Wait(2.5);
				AddShot(s, "lk_after");
				s.Wait(0.5);
			}
			return Run(s, c.now);
		}

		constexpr std::array<Stage, 5> kSet24{ {
			{ "ready", StageReady, false },
			{ "R4 find the low wall from either side", StageLowSearch, true },
			{ "M3d the pitch: own graph (learns), pose idle, pose aim", StagePitch, true },
			{ "M3d a shot high and a shot low in the pose aim; the favourites out of the pose", StageHighLow, true },
			{ "summary", StageSummary, false },
		} };

		// set 25 (card M3e): the pitch phases with the drive off, then on, in one pose; the shots; the own graph with the switch on
		// a larger sweep for the pose: 800 counts up in 2 s, a rest, 800 down in 2 s
		void AddPitchSweep(Seq& s, const char* a_phase, const bool a_aim)
		{
			auto pr = std::make_shared<PitchRun>();
			s.Do([pr, a_phase, a_aim] {
				*pr = PitchRun{};
				pr->t0 = Now();
				pr->next = pr->t0;
				REX::INFO("P3 PITCH {}: starts (a sweep of 800 counts up and 800 down, aim held {}, pose {})", a_phase, a_aim, CoverPose::ToString(CoverPose::GetInfo().state));
				Camera::ProbeBegin(a_phase);
				if (a_aim) {
					Rig::MouseRight(true);
				}
			});
			s.Until([pr, a_phase] {
				const double now = Now();
				const double t = now - pr->t0;
				if (now >= pr->next) {
					pr->next = now + 0.05;
					if (t >= 0.5 && t < 2.5) {
						Rig::MouseMove(0, -20);
					} else if (t >= 3.0 && t < 5.0) {
						Rig::MouseMove(0, 20);
					}
					PitchSample(pr, a_phase, t, false);
				}
				return t >= 5.8;
			}, 8.0);
			s.Do([pr, a_phase, a_aim] {
				Camera::ProbeEnd();
				if (a_aim) {
					Rig::MouseRight(false);
				}
				const float lo = std::asin(std::clamp(pr->camMin[5], -1.0f, 1.0f)) * 57.29578f, hi = std::asin(std::clamp(pr->camMax[5], -1.0f, 1.0f)) * 57.29578f;
				REX::INFO("P3 PITCH {} view pitch (the camera root's forward z, in degrees): {:+.1f} .. {:+.1f}, a range of {:.1f} degrees; DirectAtPitchVar {:+.3f} .. {:+.3f} rad", a_phase, lo, hi, hi - lo, pr->dMin[1], pr->dMax[1]);
				Result(std::format("P3 PITCH {} sweep: the view's pitch range {:.1f} degrees (at least 40 wanted), DirectAtPitchVar {:+.3f} .. {:+.3f} -> {}", a_phase, hi - lo, pr->dMin[1], pr->dMax[1], Verdict(hi - lo >= 40.0f)));
				Summarise(std::string("PITCH sweep ") + a_phase, hi - lo >= 40.0f);
			});
			s.Wait(0.8);
		}

		bool StagePitch2(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				s.Do([] { Config::Mutable().coverTrigger = 3; Config::Mutable().poseFreePitch = false; });
				AddWeaponTo(s, true);
				AddPitchPhase(s, "own_idle", false);
				AddPitchPhase(s, "own_aim", true);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.5);
				AddPitchPhase(s, "pose_idle_drive_off", false);
				AddPitchPhase(s, "pose_aim_drive_off", true);
				s.Do([] { Config::Mutable().poseFreePitch = true; REX::INFO("P3 PITCH: the drive (bPoseFreePitch) is switched on for the next phases"); });
				AddPitchPhase(s, "pose_idle_drive_on", false);
				AddPitchPhase(s, "pose_aim_drive_on", true);
				AddPitchSweep(s, "pose_idle_sweep", false);
				AddPitchSweep(s, "pose_aim_sweep", true);
			}
			return Run(s, c.now);
		}

		bool StageOwnOn(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				s.Do([] { Cover::ForceReset("p3 M3e own graph with the drive on"); });
				s.Wait(2.0);
				AddPitchPhase(s, "own_idle_drive_on", false);
				AddPitchPhase(s, "own_aim_drive_on", true);
			}
			return Run(s, c.now);
		}

		constexpr std::array<Stage, 6> kSet25{ {
			{ "ready", StageReady, false },
			{ "R4 find the low wall from either side", StageLowSearch, true },
			{ "M3e the pitch phases: drive off, then on, in one pose", StagePitch2, true },
			{ "M3e a shot high and a shot low in the pose aim; the favourites out of the pose", StageHighLow, true },
			{ "M3e the own graph with the drive on", StageOwnOn, true },
			{ "summary", StageSummary, false },
		} };


		// ======================================== M3f: the pose at once (set 26) ================================================================================================================
		struct FastRun
		{
			double       tPush{ 0.0 }, tState{ -1.0 }, tVar{ -1.0 }, tPosed{ -1.0 }, tCheck{ -1.0 };
			int          sessions0{ 0 };
			RE::NiPoint3 prevRoot{};
			bool         havePrev{ false };
			float        maxJump{ 0.0f };
			double       nextMove{ 0.0 };
			int          moveDir{ 1 };
			int          sinceSwitch{ 0 };
		};

		// one entry by walking into the low wall (weapon drawn or sheathed), the view moving slowly meanwhile; the times and the distance to the stand point
		void AddFastCase(Seq& s, const std::shared_ptr<int>& a_pass, const std::shared_ptr<int>& a_n, const bool a_drawn)
		{
			auto f = std::make_shared<FastRun>();
			auto r = std::make_shared<PoseRun>();
			AddWeaponTo(s, a_drawn);
			AddLowPrepare(s, 0);
			s.Do([f, a_drawn] {
				*f = FastRun{};
				Config::Mutable().coverPoses = true;
				Config::Mutable().coverCamera = true;
				Config::Mutable().coverTrigger = 3;
				Cover::ForceReset("p3 M3f fast case");
				f->sessions0 = CoverPose::GetInfo().sessions;
				f->tPush = Now();
				f->nextMove = Now();
				Rig::Tap(PoseToward(), 9.0);
				REX::INFO("P3 FAST [{}]: the walk into the wall begins, weapon state {}", a_drawn ? "drawn" : "sheathed", CoverPose::WeaponState());
			});
			s.Until([f] {
				const double now = Now();
				// the view moves slowly all the time: 2 counts up and down every 0.05 s
				if (now >= f->nextMove) {
					f->nextMove = now + 0.05;
					Rig::MouseMove(0, 2 * f->moveDir);
					f->moveDir = -f->moveDir;
				}
				if (f->tState < 0.0 && Cover::GetInfo().state != Core::CoverState::None) {
					f->tState = now;
				}
				if (f->tVar < 0.0 && VarOr("bIsInCover", 0.0) > 0.5 && CoverPose::GetInfo().sessions > f->sessions0) {
					f->tVar = now;
				}
				const auto pi = CoverPose::GetInfo();
				if (f->tPosed < 0.0 && pi.state == CoverPose::State::Posed && pi.sessions > f->sessions0) {
					f->tPosed = now;
				}
				// the camera root's position from one tick to the next, from the pose's begin to 0.4 s after the switch
				const auto rig = Camera::ReadRig();
				if (rig.ok && pi.sessions > f->sessions0 && pi.tBegin > 0.0 && now - pi.tBegin < 0.6) {
					if (f->havePrev) {
						const float dx = rig.rootWorld.x - f->prevRoot.x, dy = rig.rootWorld.y - f->prevRoot.y, dz = rig.rootWorld.z - f->prevRoot.z;
						const float jump = std::sqrt(dx * dx + dy * dy + dz * dz);
						if (jump > 0.10f) {
							REX::INFO("P3 FAST jump {:.3f} m at +{:.3f} s after the pose began: root ({:+.3f},{:+.3f},{:+.3f}) -> ({:+.3f},{:+.3f},{:+.3f}), boom y {:+.3f}, pivot z {:+.3f}, bone present {}, flag291 {}, blend {:.3f}, weapon state {}, pose {}", jump, now - pi.tBegin, f->prevRoot.x, f->prevRoot.y, f->prevRoot.z,
								rig.rootWorld.x, rig.rootWorld.y, rig.rootWorld.z, rig.n1F8.t.y, rig.n200.t.z, rig.n210.present, static_cast<int>(rig.flag291), rig.blend, CoverPose::WeaponState(), CoverPose::ToString(pi.state));
						}
						f->maxJump = (std::max)(f->maxJump, jump);
					}
					f->prevRoot = rig.rootWorld;
					f->havePrev = true;
				}
				return f->tPosed > 0.0 && f->tVar > 0.0;
			}, 8.0);
			s.Do([] { Rig::ReleaseEverything("p3 M3f fast case"); });
			s.Wait(0.6);
			s.Do([f, a_drawn, a_pass, a_n] {
				const auto  ci = Cover::GetInfo();
				const auto& sp = ci.candidate.standPoint;
				const float dist = std::hypot(Snap().position.x - sp.x, Snap().position.y - sp.y);
				const double dt = f->tVar > 0.0 && f->tState > 0.0 ? f->tVar - f->tState : -1.0;
				const bool   ok = f->tVar > 0.0 && f->tPosed > 0.0 && dt <= (a_drawn ? 0.15 : 0.6) && dist <= 0.05f && f->maxJump <= 0.10f;
				Result(std::format("P3 FAST #{} ({}): the automatic entry to bIsInCover 1 {:.3f} s (limit {:.2f}), to the pose {:.3f} s, the player {:.3f} m from the stand point 0.6 s later (limit 0.05), the largest jump of the camera root between two ticks around the switch {:.3f} m (limit 0.10), pose {} -> {}",
					*a_n + 1, a_drawn ? "weapon drawn" : "weapon sheathed", dt, a_drawn ? 0.15 : 0.6, f->tPosed > 0.0 && f->tState > 0.0 ? f->tPosed - f->tState : -1.0, dist, f->maxJump, CoverPose::ToString(CoverPose::GetInfo().state), Verdict(ok)));
				++(*a_n);
				if (ok) {
					++(*a_pass);
				}
			});
			// out by Sneak
			AddPoseLeaveSneak(s, r);
			s.Wait(0.5);
		}

		bool StageFast(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto pass = std::make_shared<int>(0);
				auto n = std::make_shared<int>(0);
				for (int i = 0; i < 10; ++i) {
					AddFastCase(s, pass, n, i % 2 == 0);
				}
				s.Do([pass, n] {
					const bool ok = *pass == 10;
					Result(std::format("P3 FAST summary: {} of {} entries inside the limits -> {}", *pass, *n, Verdict(ok)));
					Summarise("FAST ten ins and outs", ok);
				});
			}
			return Run(s, c.now);
		}

		// the aim, blind fire, the reload and the pitch once each in a fresh pose
		bool StageFastUse(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				auto ar = std::make_shared<SwapRun>();
				s.Do([] { Config::Mutable().coverTrigger = 3; });
				AddWeaponTo(s, true);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.0);
				// the aim held 1.2 s
				s.Do([ar] { *ar = SwapRun{}; ar->tPress = Now(); ar->sessions0 = CoverPose::GetInfo().sessions; Rig::MouseRight(true); });
				s.Wait(1.2);
				s.Do([ar] {
					const bool ok = VarOr("iIsSighted", 0.0) > 0.5 && CoverPose::GetInfo().state == CoverPose::State::Posed && CoverPose::GetInfo().sessions == ar->sessions0;
					Result(std::format("P3 FAST use: the aim held in the pose: iIsSighted {:g}, pose {} -> {}", VarOr("iIsSighted", -1.0), CoverPose::ToString(CoverPose::GetInfo().state), Verdict(ok)));
					Summarise("FAST use: aim", ok);
					Rig::MouseRight(false);
				});
				s.Wait(2.0);
				// blind fire: the fire control held 1.0 s
				s.Do([ar] { *ar = SwapRun{}; ar->tPress = Now(); ar->tOff = static_cast<double>(BlindFire::GetInfo().starts); Rig::MouseLeft(true); });
				s.Wait(1.0);
				s.Do([ar] {
					Rig::MouseLeft(false);
					const bool ok = BlindFire::GetInfo().starts > static_cast<std::uint32_t>(ar->tOff) && CoverPose::GetInfo().state == CoverPose::State::Posed;
					Result(std::format("P3 FAST use: blind fire: start events {} (was {:.0f}), pose {} -> {}", BlindFire::GetInfo().starts, ar->tOff, CoverPose::ToString(CoverPose::GetInfo().state), Verdict(ok)));
					Summarise("FAST use: blind fire", ok);
				});
				s.Wait(3.0);
				// the reload (a tap of R)
				s.Do([ar] { *ar = SwapRun{}; ar->tPress = Now(); Rig::Tap(Rig::kKeyR, 0.12); });
				s.Until([ar] { if (ar->tOff < 0.0 && CoverPose::VarValue("bIsReloading") > 0.5) { ar->tOff = Now(); } return Now() - ar->tPress >= 3.5; }, 6.0);
				s.Do([ar] {
					const bool ok = ar->tOff > 0.0 && CoverPose::GetInfo().state == CoverPose::State::Posed;
					Result(std::format("P3 FAST use: the reload in the pose: bIsReloading rose {:.2f} s after the tap, pose {} -> {}", ar->tOff > 0.0 ? ar->tOff - ar->tPress : -1.0, CoverPose::ToString(CoverPose::GetInfo().state), Verdict(ok)));
					Summarise("FAST use: reload", ok);
				});
				s.Wait(1.0);
				AddPitchSweep(s, "fast_pose_sweep", true);
				s.Do([] { Cover::ForceReset("p3 M3f use end"); });
				s.Wait(1.0);
			}
			return Run(s, c.now);
		}

		constexpr std::array<Stage, 5> kSet26{ {
			{ "ready", StageReady, false },
			{ "R4 find the low wall from either side", StageLowSearch, true },
			{ "M3f ten ins and outs, weapon drawn and sheathed, the view moving", StageFast, true },
			{ "M3f the aim, blind fire, the reload and the pitch once each", StageFastUse, true },
			{ "summary", StageSummary, false },
		} };


		// ======================================== M3g: the tall pose (sets 27 on the tall wall of Save29, 28 on the billboard column of Save32) ====================================================
		// the camera root against the wall: the signed distance along the wall's normal (positive = the player's side), the horizontal and vertical distance to the player
		void LogTallCam(const char* a_label)
		{
			const auto rig = Camera::ReadRig();
			const auto ci = Cover::GetInfo();
			if (!rig.ok) {
				REX::INFO("P3 TALL CAM [{}]: the rig could not be read", a_label);
				return;
			}
			const auto& a = ci.candidate.anchor;
			const auto& n = ci.candidate.normal;
			const float dWall = (rig.rootWorld.x - a.x) * n.x + (rig.rootWorld.y - a.y) * n.y;
			const float dPlayer = (Snap().position.x - a.x) * n.x + (Snap().position.y - a.y) * n.y;
			REX::INFO("P3 TALL CAM [{}]: the camera root is {:+.2f} m from the wall line along the normal (the player {:+.2f} m; negative = behind the wall), {:.2f} m away horizontally and {:+.2f} m higher than the player's feet; boom y {:+.3f}, pivot z {:+.3f}, collision factor {:.3f}",
				a_label, dWall, dPlayer, rig.rootToPlayerHoriz, rig.rootWorld.z - Snap().position.z, rig.n1F8.t.y, rig.n200.t.z, rig.factor);
		}

		struct TallRun
		{
			Core::TallVerb verb{ Core::TallVerb::None };
			double         tPress{ 0.0 }, tFlag{ -1.0 }, tClear{ -1.0 };
			std::uint32_t  starts0{ 0 }, ends0{ 0 }, turns0{ 0 }, bfStarts0{ 0 };
			int            sessions0{ 0 };
			float          maxSighted{ 0.0f };
			double         maxBlind{ 0.0 };
			bool           sideLeft0{ false };
			bool           logged03{ false }, logged12{ false };
		};

		// the aim at the tall pose: the real aim input held 3.2 s, a shot, the release; what the engine does by itself and what the plugin sends
		void AddM4a(Seq& s, const std::string& a_name, int a_expectSit, int a_expectSide, float a_expectBack, double a_window = 1.0);

		void AddTallAimCase(Seq& s, const std::shared_ptr<TallRun>& t, const char* a_label, const char* a_shot)
		{
			s.Do([t, a_label] {
				*t = TallRun{};
				t->verb = TallAim::Decide();
				t->tPress = Now();
				t->starts0 = TallAim::GetInfo().aimStarts;
				t->ends0 = TallAim::GetInfo().aimEnds;
				t->turns0 = TallAim::GetInfo().turns;
				t->sessions0 = CoverPose::GetInfo().sessions;
				t->sideLeft0 = VarOr("iSyncRightLeft", 0.0) > 0.5;
				const auto ci = Cover::GetInfo();
				REX::INFO("P3 TALL aim [{}]: the aim is held now: the rule says {} (side {}, ends left {:.2f} m ({}) right {:.2f} m ({}), range {:.2f} m); the real aim input is {}", a_label, Core::ToString(t->verb), t->sideLeft0 ? "Left" : "Right", ci.candidate.edgeLeft, Core::ToString(ci.candidate.edgeLeftKind),
					ci.candidate.edgeRight, Core::ToString(ci.candidate.edgeRightKind), Config::Get().edgePeekRange, Config::Get().poseTallAimSwallow ? "held back" : "passed to the game");
				Rig::MouseRight(true);
			});
			s.Until([t, a_label] {
				const double now = Now();
				const double dt = now - t->tPress;
				t->maxSighted = (std::max)(t->maxSighted, static_cast<float>(VarOr("iSyncSighted", 0.0)));
				if (t->tFlag < 0.0 && VarOr("iIsSighted", 0.0) > 0.5) {
					t->tFlag = now;
				}
				if (dt >= 0.3 && !t->logged03) {
					t->logged03 = true;
					REX::INFO("P3 TALL aim [{}] +0.3 s: iIsSighted {:g} iSyncSighted {:g} iSyncCoverStates {:g} iSyncRightLeft {:g}, TallAim {}", a_label, VarOr("iIsSighted", -1.0), VarOr("iSyncSighted", -1.0), VarOr("iSyncCoverStates", -1.0), VarOr("iSyncRightLeft", -1.0), TallAim::ToString(TallAim::GetInfo().state));
				}
				if (dt >= 1.2 && !t->logged12) {
					t->logged12 = true;
					REX::INFO("P3 TALL aim [{}] +1.2 s: iIsSighted {:g} iSyncSighted {:g} iSyncCoverStates {:g} iSyncRightLeft {:g}, TallAim {}", a_label, VarOr("iIsSighted", -1.0), VarOr("iSyncSighted", -1.0), VarOr("iSyncCoverStates", -1.0), VarOr("iSyncRightLeft", -1.0), TallAim::ToString(TallAim::GetInfo().state));
				}
				return dt >= 3.2;
			}, 5.0);
			s.Do([] { LogTallCam("side aim"); });
			AddM4a(s, std::string("tall_aim_") + a_label, 7, 0, 0.0f);
			AddShot(s, a_shot);
			s.Do([] { Rig::MouseLeft(true); });
			s.Wait(0.12);
			s.Do([] { Rig::MouseLeft(false); });
			s.Wait(0.8);
			s.Do([t, a_label] {
				const auto ta = TallAim::GetInfo();
				REX::INFO("P3 TALL aim [{}]: held {:.1f} s: TallAim state {}, starts {}, turns {}; iIsSighted {:g} iSyncSighted {:g} (max {:.0f}) iSyncCoverStates {:g} iSyncRightLeft {:g}; DirectAtPitchVar {:+.3f}", a_label, Now() - t->tPress, TallAim::ToString(ta.state), ta.aimStarts - t->starts0, ta.turns - t->turns0, VarOr("iIsSighted", -1.0),
					VarOr("iSyncSighted", -1.0), t->maxSighted, VarOr("iSyncCoverStates", -1.0), VarOr("iSyncRightLeft", -1.0), VarOr("DirectAtPitchVar", 0.0));
				t->tPress = Now();
				Rig::MouseRight(false);
				t->tClear = -1.0;
			});
			s.Until([t] {
				if (t->tClear < 0.0 && VarOr("iIsSighted", 1.0) < 0.5 && VarOr("iSyncSighted", 1.0) < 0.5) {
					t->tClear = Now();
				}
				return t->tClear > 0.0 || Now() - t->tPress > 4.0;
			}, 6.0);
			s.Wait(0.5);
			s.Do([t, a_label] {
				const auto ta = TallAim::GetInfo();
				const bool engineAimed = t->maxSighted > 0.5f;
				bool       ok{ false };
				if (t->verb == Core::TallVerb::None) {
					ok = ta.aimStarts == t->starts0 && CoverPose::GetInfo().state == CoverPose::State::Posed;
				} else {
					ok = ta.aimStarts == t->starts0 + 1 && ta.aimEnds == t->ends0 + 1 && engineAimed && t->tClear > 0.0 && CoverPose::GetInfo().state == CoverPose::State::Posed && CoverPose::GetInfo().sessions == t->sessions0;
				}
				Result(std::format("P3 TALL aim [{}] (rule {}): SightedStart sent by the plugin {}, SightedEnd sent {}, turns {}, iSyncSighted reached {:.0f} (the aim happened {}), the flags cleared {:.2f} s after the release, the pose {} (sessions {} -> {}) -> {}", a_label, Core::ToString(t->verb), ta.aimStarts - t->starts0, ta.aimEnds - t->ends0, ta.turns - t->turns0,
					t->maxSighted, engineAimed ? "yes" : "NO", t->tClear > 0.0 ? t->tClear - t->tPress : -1.0, CoverPose::ToString(CoverPose::GetInfo().state), t->sessions0, CoverPose::GetInfo().sessions, Verdict(ok)));
				Summarise(std::string("TALL aim ") + a_label, ok);
			});
			s.Wait(1.5);
		}

		bool StageTallPose(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				auto t = std::make_shared<TallRun>();
				s.Do([] { Config::Mutable().coverPoses = true; Config::Mutable().poseTall = true; Config::Mutable().coverTrigger = 1; });
				AddWeaponTo(s, true);
				AddPoseEnter(s, r, 0, true);
				s.Do([r] {
					const auto  i = CoverPose::GetInfo();
					const auto  ci = Cover::GetInfo();
					const auto& sp = ci.candidate.standPoint;
					const float dist = std::hypot(Snap().position.x - sp.x, Snap().position.y - sp.y);
					const double inCover = VarOr("bIsInCover", -1.0);
					const int    field = CoverPose::StanceField();
					const bool   ok = i.state == CoverPose::State::Posed && inCover > 0.5 && field == 5 && dist <= 0.08f && i.lastCallReturned;
					Result(std::format("P3 TALL in: Phase 3 state {:.2f} s after the push, the pose {:.2f} s after the push, stance {} side {} (iSyncRightLeft {:g}), bIsInCover {:g}, the stance field {} (expected 5), the player {:.3f} m from the stand point (limit 0.08), the ends left {:.2f} m ({}) right {:.2f} m ({}), kind {}, state {} -> {}",
						r->tState > 0.0 ? r->tState - r->tPush : -1.0, r->tPosed > 0.0 ? r->tPosed - r->tPush : -1.0, i.lastStance, i.lastSide ? "Left" : "Right", VarOr("iSyncRightLeft", -1.0), inCover, field, dist, ci.candidate.edgeLeft, Core::ToString(ci.candidate.edgeLeftKind),
						ci.candidate.edgeRight, Core::ToString(ci.candidate.edgeRightKind), Core::ToString(ci.candidate.kind), CoverPose::ToString(i.state), Verdict(ok)));
					Summarise("TALL in: the pose at the tall wall", ok);
				});
				s.Wait(1.0);
				s.Do([] { LogTallCam("idle"); });
				AddShot(s, "tall_idle");
				// the cover holds for 3 s
				s.Wait(3.0);
				s.Do([] {
					const bool ok = CoverPose::GetInfo().state == CoverPose::State::Posed && VarOr("bIsInCover", 0.0) > 0.5;
					Result(std::format("P3 TALL idle 3 s: pose {}, bIsInCover {:g} -> {}", CoverPose::ToString(CoverPose::GetInfo().state), VarOr("bIsInCover", -1.0), Verdict(ok)));
					Summarise("TALL idle: the cover holds", ok);
				});
				AddTallAimCase(s, t, "early aim (right after the pose)", "tall_aim_early");
				s.Wait(1.0);
				// blind fire: the fire control held 1.6 s
				s.Do([t] {
					*t = TallRun{};
					t->verb = TallAim::Decide();
					t->bfStarts0 = BlindFire::GetInfo().starts;
					t->turns0 = TallAim::GetInfo().turns;
					t->sessions0 = CoverPose::GetInfo().sessions;
					t->tPress = Now();
					REX::INFO("P3 TALL blind fire: the fire control is held: the rule says {} (side {})", Core::ToString(t->verb), VarOr("iSyncRightLeft", 0.0) > 0.5 ? "Left" : "Right");
					Rig::MouseLeft(true);
				});
				s.Until([t] {
					t->maxBlind = (std::max)(t->maxBlind, VarOr("iIsBlindFiring", 0.0));
					return Now() - t->tPress >= 1.3;
				}, 3.0);
				s.Do([] { LogTallCam("blind fire"); });
				AddShot(s, "tall_bf");
				s.Wait(0.3);
				s.Do([] { Rig::MouseLeft(false); });
				s.Wait(2.5);
				s.Do([t] {
					const auto b = BlindFire::GetInfo();
					const auto ta = TallAim::GetInfo();
					bool       ok{ false };
					if (t->verb == Core::TallVerb::Here) {
						ok = b.starts == t->bfStarts0 + 1 && t->maxBlind > 0.5 && CoverPose::GetInfo().state == CoverPose::State::Posed;
					} else if (t->verb == Core::TallVerb::Turn) {
						ok = ta.turns == t->turns0 + 1 && b.starts == t->bfStarts0 && CoverPose::GetInfo().state == CoverPose::State::Posed;
					} else {
						ok = b.starts == t->bfStarts0 && CoverPose::GetInfo().state == CoverPose::State::Posed;
					}
					Result(std::format("P3 TALL blind fire (rule {}): BlindFireStart events {}, turns {}, the largest iIsBlindFiring {:g}, iSyncCoverStates {:g}, pose {} -> {}", Core::ToString(t->verb), b.starts - t->bfStarts0, ta.turns - t->turns0, t->maxBlind, VarOr("iSyncCoverStates", -1.0), CoverPose::ToString(CoverPose::GetInfo().state), Verdict(ok)));
					Summarise("TALL blind fire by the end rule", ok);
				});
				s.Wait(1.0);
				// the reload (a tap of R): stays in the pose
				{
					auto rl = std::make_shared<SwapRun>();
					s.Do([rl] { *rl = SwapRun{}; rl->tPress = Now(); Rig::Tap(Rig::kKeyR, 0.12); });
					s.Until([rl] { if (rl->tOff < 0.0 && CoverPose::VarValue("bIsReloading") > 0.5) { rl->tOff = Now(); } return Now() - rl->tPress >= 3.5; }, 6.0);
					s.Do([rl] {
						const bool ok = rl->tOff > 0.0 && CoverPose::GetInfo().state == CoverPose::State::Posed;
						Result(std::format("P3 TALL reload: bIsReloading rose {:.2f} s after the tap, pose {} -> {}", rl->tOff > 0.0 ? rl->tOff - rl->tPress : -1.0, CoverPose::ToString(CoverPose::GetInfo().state), Verdict(ok)));
						Summarise("TALL reload stays in the pose", ok);
					});
				}
				// the favourites lock: key 2 changes nothing
				s.Do([] { Rig::Tap(Rig::kKey2, 0.12); });
				s.Wait(1.5);
				s.Do([] {
					const bool ok = CoverPose::GetInfo().state == CoverPose::State::Posed && CoverPose::FavoritesLocked();
					Result(std::format("P3 TALL favourites: key 2 in the pose: pose {}, favourites locked {} -> {}", CoverPose::ToString(CoverPose::GetInfo().state), CoverPose::FavoritesLocked(), Verdict(ok)));
					Summarise("TALL favourites locked", ok);
				});
				// the pitch: a sweep with the aim held
				AddPitchSweep(s, "tall_pose_sweep", false);
				// the walk both ways with the game's own keys (the view toward the wall: D to the actor's right, A to the left)
				AddWalkLeg(s, r, PoseRightKey(), "D (to the right)", false, nullptr);
				AddWalkLeg(s, r, PoseLeftKey(), "A (to the left, turning round first)", true, nullptr);
				AddWalkLeg(s, r, PoseRightKey(), "D again (to the right, turning round first)", false, nullptr);
				AddTallAimCase(s, t, "late aim (after the walk)", "tall_aim");
				// out by Sneak
				AddPoseLeaveSneak(s, r);
				s.Do([r] {
					const bool ok = r->tNone > 0.0 && r->tOff > 0.0 && OwnGraph();
					Result(std::format("P3 TALL out by Sneak: state None {:.2f} s after the press, the own graph back {:.2f} s after it -> {}", r->tNone > 0.0 ? r->tNone - r->tLeave : -1.0, r->tOff > 0.0 ? r->tOff - r->tLeave : -1.0, Verdict(ok)));
					Summarise("TALL out by Sneak", ok);
				});
				s.Wait(0.5);
			}
			return Run(s, c.now);
		}

		constexpr std::array<Stage, 3> kSet27{ {
			{ "ready", StageReady, false },
			{ "M3g the tall pose on the tall wall of Save29", StageTallPose, true },
			{ "summary", StageSummary, false },
		} };


		// ======================================== M3h: the view yaw through the pose's in and out, the legs at the wall, the way out (sets 29 on the tall wall of Save29, 30 on the low wall of Save30) ===========
		struct YawRun
		{
			double tPress{ 0.0 };
			double maxIdle{ 0.0 };
			int    nIdle{ 0 };
			double nextSample{ 0.0 };
			int    zero0{ 0 };
			float  angStart{ 0.0f }, angEnd{ 0.0f }, offStart{ 0.0f };
		};

		[[nodiscard]] float ReadAngZ()
		{
			float v{ 0.0f };
			if (const auto p = RE::PlayerCharacter::GetSingleton()) {
				v = p->data.angle.z;
			}
			return v;
		}

		// a movement key pushed into the wall for 3 s, then along it: iSyncIdleLocomotion is the walk clip's own variable, the pictures show the legs
		void AddWallPush(Seq& s, const std::shared_ptr<YawRun>& y, const char* a_label)
		{
			s.Do([y, a_label] {
				*y = YawRun{};
				y->tPress = Now();
				y->zero0 = Cover::ZeroInputCount();
				REX::INFO("P3 WALLPUSH [{}]: the key into the wall is held for 3 s (pose {}, side {:g}, zero input {})", a_label, CoverPose::ToString(CoverPose::GetInfo().state), VarOr("iSyncRightLeft", -1.0), Config::Get().poseZeroInput ? "on" : "off");
				Rig::Tap(PoseToward(), 3.2);
			});
			s.Until([y, a_label] {
				const double now = Now();
				if (now >= y->nextSample) {
					y->nextSample = now + 0.25;
					const double idle = VarOr("iSyncIdleLocomotion", -1.0);
					y->maxIdle = (std::max)(y->maxIdle, idle);
					++y->nIdle;
					REX::INFO("P3 WALLPUSH [{}] +{:.2f} s: iSyncIdleLocomotion {:g} Speed {:g} Direction {:g} bIsInCover {:g}, pose {}", a_label, now - y->tPress, idle, VarOr("Speed", -1.0), VarOr("Direction", -1.0), VarOr("bIsInCover", -1.0), CoverPose::ToString(CoverPose::GetInfo().state));
				}
				return now - y->tPress >= 3.0;
			}, 5.0);
			s.Do([y, a_label] {
				Rig::ReleaseEverything("p3 wall push end");
				const bool ok = y->maxIdle < 0.5 && CoverPose::GetInfo().state == CoverPose::State::Posed;
				Result(std::format("P3 WALLPUSH [{}]: the key into the wall held 3 s: the largest iSyncIdleLocomotion {:g} over {} samples (expected 0), movement inputs zeroed during it {}, pose {} -> {}", a_label, y->maxIdle, y->nIdle, Cover::ZeroInputCount() - y->zero0, CoverPose::ToString(CoverPose::GetInfo().state), Verdict(ok)));
				Summarise(std::string("WALLPUSH ") + a_label, ok);
			});
			s.Wait(0.6);
		}

		struct YawVariant
		{
			const char* name;
			float       blend;  // fPoseEntryBlend
			bool        exitHold;
			int         exitClip;
			bool        look;  // the view turned 120 degrees with the mouse before the exit
			bool        world{ false };  // bPoseHoldWorld
			bool        pull{ false };   // the way out by holding W (with the view turned: away from the wall) instead of Sneak
		};

		struct YawCycleOut
		{
			Camera::YawSummary in{}, out{};
			RE::NiPoint3       p0{};
		};

		// one cycle: the pose entered (the probe from the push; criteria: the camera never closer than 0.8 m to the head, no step of the camera root above 0.10 m), the view turned or not, the
		// way out by Sneak, W held after it; then the state after the exit and the distance walked
		void AddYawCycle(Seq& s, const std::shared_ptr<PoseRun>& r, const std::shared_ptr<YawCycleOut>& o, const YawVariant a_v, const bool a_first)
		{
			const bool low = PoseLow();
			s.Do([a_v, low] {
				Config::Mutable().poseEntryBlend = a_v.blend;
				Config::Mutable().poseHoldWorld = a_v.world;
				Config::Mutable().poseExitHold = a_v.exitHold;
				Config::Mutable().poseExitClip = a_v.exitClip;
				Config::Mutable().poseExitTurn = 0;
				Config::Mutable().poseZeroInput = true;
				g_yawLabel = std::format("{}_{}_in", low ? "low" : "tall", a_v.name);
				REX::INFO("P3 YAW cycle [{}]: fPoseEntryBlend {:.2f}, bPoseExitHold {}, iPoseExitClip {}, view turned {}", a_v.name, a_v.blend, a_v.exitHold, a_v.exitClip, a_v.look);
			});
			if (a_first) {
				AddWeaponTo(s, true);
			}
			AddPoseEnter(s, r, 0, true);
			s.Wait(1.2);
			s.Do([o] { o->in = Camera::YawProbeEnd(); g_yawLabel.clear(); });
			s.Do([r, o, a_v] {
				const auto i = CoverPose::GetInfo();
				const bool posed = i.state == CoverPose::State::Posed && i.sessions > r->sessions0;
				const bool ok = posed && o->in.poseMinHead >= 0.8f && o->in.poseMaxJump <= 0.10f;
				Result(std::format("P3 YAW in [{}]: pose {} {:.2f} s after the push; from the pose's first frame the camera root's least distance from the head {:.2f} m (limit 0.8), its largest step {:.3f} m (limit 0.10) -> {}", a_v.name, CoverPose::ToString(i.state), r->tPosed > 0.0 ? r->tPosed - r->tPush : -1.0,
					o->in.poseMinHead, o->in.poseMaxJump, Verdict(ok)));
				Summarise(std::string("YAW in ") + a_v.name, ok);
			});
			s.Wait(1.0);
			if (a_v.look) {
				for (int k = 0; k < 6; ++k) {
					s.Do([] { Rig::MouseMove(88, 0); });
					s.Wait(0.14);
				}
				s.Wait(0.5);
			}
			s.Do([a_v, low, o] {
				float vy{ 0.0f };
				(void)Camera::ReadViewYaw(vy);
				REX::INFO("P3 YAW before the exit [{}]: the view yaw {:.1f} deg, the body's angle z {:.1f} deg, pose {}", a_v.name, vy * 57.29578f, ReadAngZ() * 57.29578f, CoverPose::ToString(CoverPose::GetInfo().state));
				Camera::YawProbeBegin(std::format("{}_{}_out", low ? "low" : "tall", a_v.name).c_str());
				o->p0 = Snap().position;
			});
			if (a_v.pull) {
				s.Do([r, o] {
					r->tNone = r->tOff = -1.0;
					r->tLeave = Now();
					o->p0 = Snap().position;
					Rig::Tap(Rig::kKeyW, 3.0);
				});
				s.Until([r] {
					if (r->tNone < 0.0 && Cover::GetInfo().state == Core::CoverState::None) {
						r->tNone = Now();
					}
					if (r->tOff < 0.0 && CoverPose::GetInfo().state == CoverPose::State::Off) {
						r->tOff = Now();
					}
					return r->tOff > 0.0;
				}, 4.0);
				s.Wait(1.5);
			} else {
				AddPoseLeaveSneak(s, r);
				s.Do([o, low, a_v] { o->p0 = Snap().position; if (!low || a_v.look) { Rig::Tap(Rig::kKeyW, 1.2); } });  // at the low wall with the view toward it W walks into the wall and enters the cover again (Phase 3)
				s.Wait(2.0);
			}
			s.Do([r, o, a_v] {
				Rig::ReleaseEverything("p3 yaw out");
				o->out = Camera::YawProbeEnd();
				const float walked = Dist2(o->p0, Snap().position);
				const bool  state = CoverPose::StanceField() == 0 && VarOr("bIsInCover", 1.0) < 0.5 && VarOr("bIsExitingCover", 1.0) < 0.5 && OwnGraph() && CoverPose::WeaponState() == 3 && Cover::GetInfo().state == Core::CoverState::None && CoverPose::GetInfo().state == CoverPose::State::Off;
				const bool  camOk = a_v.look || a_v.exitClip == 0 || (o->out.minHead >= 0.8f && o->out.maxJump <= 0.10f);  // the exit function releases the hold at once: its camera step is known (REFERENCE 22.19), not a criterion
				const bool  ok = r->tNone > 0.0 && r->tOff > 0.0 && state && (!a_v.look || walked > 1.0f) && camOk;
				Result(std::format("P3 YAW out [{}]: state None {:.2f} s after the press, the own graph back {:.2f} s after it; stance field {}, bIsInCover {:g}, bIsExitingCover {:g}, own graph {}, weapon state {}, Cover {}, pose {}; walked {:.2f} m in 1.2 s with W; the camera root's least distance from the head {:.2f} m, its largest step {:.3f} m{} -> {}", a_v.name,
					r->tNone > 0.0 ? r->tNone - r->tLeave : -1.0, r->tOff > 0.0 ? r->tOff - r->tLeave : -1.0, CoverPose::StanceField(), VarOr("bIsInCover", -1.0), VarOr("bIsExitingCover", -1.0), OwnGraph(), CoverPose::WeaponState(), Core::ToString(Cover::GetInfo().state), CoverPose::ToString(CoverPose::GetInfo().state),
					walked, o->out.minHead, o->out.maxJump, a_v.look ? " (the view was turned: the camera limits are not the criterion)" : " (limits 0.8 m and 0.10 m)", Verdict(ok)));
				Summarise(std::string("YAW out ") + a_v.name, ok);
			});
			s.Wait(0.5);
		}

		bool StageYaw(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				auto o = std::make_shared<YawCycleOut>();
				s.Do([] { Config::Mutable().coverPoses = true; Config::Mutable().poseTall = true; Config::Mutable().coverTrigger = (Config::Get().p3Set == 30 || Config::Get().p3Set == 32 || Config::Get().p3Set == 35) ? 3 : 1; });
				const int set = Config::Get().p3Set;
				if (set == 32 || set == 35) {
					// the low pose with the M3i settings: one cycle with the exit function for the figures, then ten ins and outs without the clip (every second one with the view turned)
					AddYawCycle(s, r, o, { "fn", 0.0f, false, 0, false }, true);
					static const char* lnames[10] = { "l1", "l2", "l3", "l4", "l5", "l6", "l7", "l8", "l9", "l10" };
					for (int k = 0; k < 10; ++k) {
						AddYawCycle(s, r, o, { lnames[k], 0.0f, true, 1, (k % 2) == 1 }, false);
					}
				} else if (set == 34 || set == 28) {
					// M4a: the entry and exit cycles at the tall wall with the own camera: the nearest distance to the head and the largest step per frame
					AddYawCycle(s, r, o, { "plain", 0.0f, true, 1, false }, true);
					AddYawCycle(s, r, o, { "plain2", 0.0f, true, 1, false }, false);
					AddYawCycle(s, r, o, { "walkexit", 0.0f, true, 1, true }, false);
				} else if (set == 33) {
					// the entry dive without Stride: the same entry with the world write, with the blend, and plain, twice
					AddYawCycle(s, r, o, { "plain", 0.0f, true, 1, false }, true);
					AddYawCycle(s, r, o, { "world", 0.0f, true, 1, false, true }, false);
					AddYawCycle(s, r, o, { "blend", 1.5f, true, 1, false }, false);
					AddYawCycle(s, r, o, { "plain2", 0.0f, true, 1, false }, false);
					AddYawCycle(s, r, o, { "pull", 0.0f, true, 1, true, false, true }, false);
					AddYawCycle(s, r, o, { "pullfn", 0.0f, true, 0, true, false, true }, false);
				} else if (set == 31) {
					// the ten ins and outs of the exit without the clip: five with the view straight, five turned
					bool first = true;
					for (int k = 0; k < 10; ++k) {
						static const char* names[10] = { "d1", "d2", "d3", "d4", "d5", "d6", "d7", "d8", "d9", "d10" };
						AddYawCycle(s, r, o, { names[k], 1.5f, true, 1, (k % 2) == 1 }, first);
						first = false;
					}
				} else {
					AddYawCycle(s, r, o, { "old", 0.0f, false, 0, false }, true);
					AddYawCycle(s, r, o, { "blend", 1.5f, true, 0, false }, false);
					AddYawCycle(s, r, o, { "blendd", 1.5f, true, 1, false }, false);
					AddYawCycle(s, r, o, { "blenddlook", 1.5f, true, 1, true }, false);
					AddYawCycle(s, r, o, { "blendclip", 1.5f, true, 0, false }, false);
					AddYawCycle(s, r, o, { "blend2", 2.0f, true, 1, false }, false);
				}
			}
			return Run(s, c.now);
		}

		// ======================================== M4a (Phase 4): the own camera, measured in the view frame, and the pictures =========================================
		struct M4aAcc
		{
			int    n{ 0 };
			double sSide{ 0 }, sBack{ 0 }, sUp{ 0 }, sHead{ 0 }, sBoomY{ 0 }, sPivX{ 0 }, sTurn{ 0 }, last{ 0 }, t0{ 0 };
			float  minF{ 1e9f };
			int    cnt[8]{};
			int    sit{ -1 };
			bool   on{ false };

			void Begin()
			{
				*this = M4aAcc{};
				t0 = Now();
			}
			void Step()
			{
				if (Now() - last < 0.05) {
					return;
				}
				last = Now();
				const auto r = Camera::ReadRig();
				float      yaw{ 0.0f };
				if (!r.ok || !Camera::ReadViewYaw(yaw)) {
					return;
				}
				const float dx = r.rootWorld.x - r.player.x, dy = r.rootWorld.y - r.player.y, dz = r.rootWorld.z - r.player.z;
				sSide += dx * std::cos(yaw) - dy * std::sin(yaw);
				sBack += -(dx * std::sin(yaw) + dy * std::cos(yaw));
				sUp += dz;
				sHead += std::sqrt(dx * dx + dy * dy + (dz - 1.55f) * (dz - 1.55f));
				const auto f = Camera::GetFraming();
				sBoomY += f.boomY;
				sPivX += f.pivotX;
				sit = f.situation;
				if (sit >= 0 && sit < 8) {
					++cnt[sit];
				}
				{
					// the camera's own heading (the forward row of the root's rotation) against the view yaw: a view that swings away from the yaw is the look-at on a stale point
					float rot[9]{};
					if (Camera::ReadRootRot(rot)) {
						float d = std::atan2(rot[3], rot[4]) - yaw;
						while (d > 3.14159265f) { d -= 6.2831853f; }
						while (d < -3.14159265f) { d += 6.2831853f; }
						sTurn += d * 57.29578f;
					}
				}
				on = f.on;
				minF = (std::min)(minF, r.factor);
				++n;
			}
			[[nodiscard]] float Side() const { return n ? static_cast<float>(sSide / n) : 0.0f; }
			[[nodiscard]] float Back() const { return n ? static_cast<float>(sBack / n) : 0.0f; }
			[[nodiscard]] float Up() const { return n ? static_cast<float>(sUp / n) : 0.0f; }
			[[nodiscard]] float Head() const { return n ? static_cast<float>(sHead / n) : 0.0f; }
			[[nodiscard]] float Turn() const { return n ? static_cast<float>(sTurn / n) : 0.0f; }
			[[nodiscard]] int   Main() const { int b = 0; for (int i = 1; i < 8; ++i) { if (cnt[i] > cnt[b]) { b = i; } } return b; }
		};

		// a window of 1 s, then the result: the situation, the camera root in the view frame (to the right of the view, behind it, above the feet), the nodes as written, the collision factor
		void AddM4a(Seq& s, const std::string& a_name, const int a_expectSit, const int a_expectSide, const float a_expectBack, const double a_window)
		{
			auto m = std::make_shared<M4aAcc>();
			s.Do([m] { m->Begin(); });
			s.Until([m, a_window] { m->Step(); return Now() - m->t0 >= a_window; }, a_window + 2.0);
			s.Do([m, a_name, a_expectSit, a_expectSide, a_expectBack] {
				const auto f = Camera::GetFraming();
				m->sit = m->Main();
				const bool sitOk = m->sit == a_expectSit;
				const bool sideOk = a_expectSide == 0 || (a_expectSide > 0 ? m->Side() > 0.15f : m->Side() < -0.15f);
				const bool backOk = a_expectBack <= 0.0f || m->minF < 0.99f || std::fabs(m->Back() - a_expectBack) <= 0.5f;
				const bool turnOk = std::fabs(m->Turn()) < 2.5f;
				const bool ok = f.on && m->n >= 5 && sitOk && sideOk && backOk && turnOk;
				Result(std::format("P3 M4A [{}]: situation {} (expected {}); the camera root in the view frame: {:+.3f} m to the right, {:.3f} m behind (expected about {:.2f}), {:.3f} m above the feet, {:.3f} m from the head; the camera's heading is {:+.2f} deg from the view yaw (limit 2.5); written: boom y {:+.3f}, pivot x {:+.3f}; collision factor min {:.3f}; {} samples; framing {}, side target {} -> {}",
					a_name, Camera::SituationName(m->sit), Camera::SituationName(a_expectSit), m->Side(), m->Back(), a_expectBack, m->Up(), m->Head(), m->Turn(), static_cast<float>(m->sBoomY / (std::max)(m->n, 1)), static_cast<float>(m->sPivX / (std::max)(m->n, 1)), m->minF, m->n,
					f.on ? "on" : "OFF", f.sideTarget > 0 ? "right" : "left", Verdict(ok)));
				Summarise("M4A " + a_name, ok);
			});
			AddShot(s, "m4a_" + a_name);
		}

		void AddFire(Seq& s)
		{
			s.Do([] { REX::INFO("M4A FIRE: one burst now"); Rig::MouseLeft(true); });
			s.Wait(0.15);
			s.Do([] { Rig::MouseLeft(false); });
			s.Wait(0.5);
		}

		bool StageM4aTall(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				const auto& cfg = Config::Get();
				s.Do([] {
					Config::Mutable().coverPoses = true;
					Config::Mutable().poseTall = true;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 1;
					REX::INFO("P3 M4A: the own camera {}: bFraming {}, bCoverCamera 1", Camera::Compat() ? "is OFF (compatibility mode)" : "is on", Config::Get().framing);
				});
				// sprinting, from 8.5 m in front of the wall (the wall surface is about 1.23 m east of the spawn)
				AddPrepare(s, 0, { s_spawn.x - 13.0f, s_spawn.y, s_spawn.z }, 102.0f);
				AddWeaponTo(s, false);
				s.Do([] { Rig::Tap(Rig::kKeyShift, 3.2); });
				s.Wait(0.3);
				s.Do([] { Rig::Tap(Rig::kKeyW, 2.8); });
				s.Wait(1.5);
				AddM4a(s, "sprint", 3, 0, cfg.frameDistSprint, 0.5);
				s.Do([] { Rig::ReleaseEverything("m4a sprint"); });
				s.Wait(1.0);
				// 5 m from the wall, the view toward it
				AddPrepare(s, 0, { s_spawn.x - 3.77f, s_spawn.y, s_spawn.z }, 102.0f);
				AddWeaponTo(s, false);
				s.Wait(1.0);
				AddM4a(s, "explore_right", 0, 1, cfg.frameDistExplore);
				AddWeaponTo(s, true);
				AddM4a(s, "drawn_right", 1, 1, cfg.frameDistDrawn);
				s.Do([] { Rig::Tap(Rig::kKeyX, 0.08); });
				s.Wait(0.8);
				AddM4a(s, "drawn_left", 1, -1, cfg.frameDistDrawn);
				s.Do([] { Rig::MouseRight(true); });
				s.Wait(1.0);
				AddM4a(s, "aim_left", 2, -1, cfg.frameDistAim);
				AddFire(s);
				AddShot(s, "m4a_aim_left_after_shot");
				s.Do([] { Rig::MouseRight(false); });
				s.Wait(1.0);
				s.Do([] { Rig::Tap(Rig::kKeyX, 0.08); });
				s.Wait(0.8);
				s.Do([] { Rig::MouseRight(true); });
				s.Wait(1.0);
				AddM4a(s, "aim_right", 2, 1, cfg.frameDistAim);
				AddFire(s);
				AddShot(s, "m4a_aim_right_after_shot");
				s.Do([] { Rig::MouseRight(false); });
				s.Wait(1.0);
				// first person and back: the camera leaves everything alone, comes back with the shoulder it had (the right one)
				s.Do([] { (void)Camera::ForceFirstPerson(); });
				s.Wait(1.5);
				s.Do([] { (void)Camera::ForceThirdPerson(); });
				s.Wait(1.5);
				AddM4a(s, "drawn_right_after_first_person", 1, 1, cfg.frameDistDrawn);
				// tall cover, the pose idle and aiming
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.2);
				AddM4a(s, "tall_idle", 5, 1, cfg.frameDistDrawn + cfg.coverCamBackTall);
				s.Do([] { Rig::MouseRight(true); });
				s.Wait(1.4);
				AddM4a(s, "tall_aim", 5, 1, 0.0f);  // no open end at this wall: the aim does nothing there (the tall aim needs one), the camera stays in the tall framing
				s.Do([] { Rig::MouseRight(false); });
				s.Wait(1.5);
				AddPoseLeaveSneak(s, r);
				s.Wait(1.0);
			}
			return Run(s, c.now);
		}

		bool StageM4aLow(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				const auto& cfg = Config::Get();
				s.Do([] {
					Config::Mutable().coverPoses = true;
					Config::Mutable().poseTall = true;
					Config::Mutable().coverCamera = true;
					REX::INFO("P3 M4A: the own camera {}: bFraming {}, bCoverCamera 1", Camera::Compat() ? "is OFF (compatibility mode)" : "is on", Config::Get().framing);
				});
				AddWeaponTo(s, true);
				AddM4a(s, "low_before_drawn", 1, 1, cfg.frameDistDrawn);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.2);
				AddM4a(s, "low_idle", 4, 1, cfg.frameDistDrawn + cfg.coverCamBackLow);
				s.Do([] { Rig::MouseRight(true); });
				s.Wait(1.4);
				AddM4a(s, "low_aim", 6, 1, 0.0f);
				s.Do([] { Rig::MouseRight(false); });
				s.Wait(1.5);
				s.Do([] { Rig::Tap(Rig::kKeyX, 0.08); });
				s.Wait(0.8);
				AddM4a(s, "low_idle_left", 4, -1, cfg.frameDistDrawn + cfg.coverCamBackLow);
				s.Do([] { Rig::Tap(Rig::kKeyX, 0.08); });
				s.Wait(0.8);
				AddPoseLeaveSneak(s, r);
				s.Wait(1.0);
			}
			return Run(s, c.now);
		}

		// the camera as the rig sees it with the framing on or off, or the situation forced: logged, no verdict (REFERENCE 23.2: where the 0.45 m of the exploring distance come from)
		void AddM4aProbe(Seq& s, const std::string& a_name, const bool a_framing, const int a_force)
		{
			auto m = std::make_shared<M4aAcc>();
			s.Do([a_framing, a_force] { Config::Mutable().framing = a_framing; Camera::ForceSituation(a_force); });
			s.Wait(1.2);
			s.Do([m] { m->Begin(); });
			s.Until([m] { m->Step(); return Now() - m->t0 >= 1.0; }, 3.0);
			s.Do([m, a_name, a_framing, a_force] {
				const auto r = Camera::ReadRig();
				Result(std::format("P3 M4A PROBE [{}]: framing {}, situation forced {}: the game says {}; the camera root in the view frame {:+.3f} to the right, {:.3f} behind, {:.3f} above the feet; heading {:+.2f} deg from the view yaw; the graph's nodes between frames: boom ({:+.3f},{:+.3f},{:+.3f}), pivot ({:+.3f},{:+.3f},{:+.3f}); weapon state {}, sprint var {:g}, flags {}",
					a_name, a_framing ? "on" : "OFF", a_force, Camera::SituationName(m->Main()), m->Side(), m->Back(), m->Up(), m->Turn(), r.n1F8.t.x, r.n1F8.t.y, r.n1F8.t.z, r.n200.t.x, r.n200.t.y, r.n200.t.z,
					CoverPose::WeaponState(), VarOr("iSyncSprintState", -1.0), PlayerControl::ActorFlags()));
				Config::Mutable().framing = true;
				Camera::ForceSituation(-1);
			});
			s.Wait(0.5);
		}

		bool StageM4aProbe(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				s.Do([] { Config::Mutable().coverCamera = true; });
				AddPrepare(s, 0, Base(), 102.0f);
				AddWeaponTo(s, false);
				AddM4aProbe(s, "explore_framing_on", true, -1);
				AddM4aProbe(s, "explore_framing_off", false, -1);
				AddWeaponTo(s, true);
				AddM4aProbe(s, "drawn_framing_off", false, -1);
				AddM4aProbe(s, "drawn_framing_on", true, -1);
				AddM4aProbe(s, "sprint_forced", true, 3);
				AddM4aProbe(s, "explore_forced", true, 0);
				AddM4aProbe(s, "sprint_forced_explore_graph", true, 3);
			}
			return Run(s, c.now);
		}

		// ---- the aim correction (REFERENCE 23.2): a plain wall found by casts, 5 m away; from each shoulder aiming and one burst, then the view turned by a few degrees and a second burst:
		// the pictures before and after give the hole against the crosshair, the second shot gives the scale in pixels per degree
		struct AimWall
		{
			float        d[24][5]{};
			float        heading{ 102.0f }, d0{ 0.0f }, err{ 1e9f };
			bool         ok{ false };
			RE::NiPoint3 from{}, hit{};
			float        D{ 0.0f };
		};

		void AimLog(const char* a_tag)
		{
			const auto r = Camera::ReadRig();
			float      yaw{ 0.0f };
			(void)Camera::ReadViewYaw(yaw);
			const float dx = r.rootWorld.x - r.player.x, dy = r.rootWorld.y - r.player.y, dz = r.rootWorld.z - r.player.z;
			REX::INFO("P3 M4A AIM [{}]: view yaw {:.2f} deg, the camera root {:+.3f} m to the right, {:.3f} behind, {:.3f} above the feet, the player at ({:.2f},{:.2f}), shoulder {}", a_tag, yaw * kRad2Deg, dx * std::cos(yaw) - dy * std::sin(yaw), -(dx * std::sin(yaw) + dy * std::cos(yaw)), dz,
				r.player.x, r.player.y, Camera::GetFraming().sideTarget > 0 ? "right" : "left");
		}

		void AddAimRound(Seq& s, const char* a_tag, const int a_dir)
		{
			s.Do([a_dir] { Camera::SetShoulder(a_dir); });
			s.Wait(0.9);
			s.Do([] { Rig::MouseRight(true); });
			s.Wait(1.3);
			s.Do([a_tag] { AimLog((std::string(a_tag) + " before the first burst").c_str()); });
			AddShot(s, std::string("m4a_aim_") + a_tag + "_1pre");
			AddFire(s);
			s.Wait(0.9);
			s.Do([a_tag] { AimLog((std::string(a_tag) + " after the first burst").c_str()); });
			AddShot(s, std::string("m4a_aim_") + a_tag + "_2post");
			s.Do([] { Rig::MouseMove(14, 0); });
			s.Wait(1.0);
			s.Do([a_tag] { AimLog((std::string(a_tag) + " turned").c_str()); });
			AddFire(s);
			s.Wait(0.9);
			s.Do([a_tag] { AimLog((std::string(a_tag) + " after the second burst").c_str()); });
			AddShot(s, std::string("m4a_aim_") + a_tag + "_3post");
			s.Do([] { Rig::MouseRight(false); });
			s.Wait(1.2);
		}

		bool StageM4aAim(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto wall = std::make_shared<AimWall>();
				auto scan = std::make_shared<WorldProbe::Batch>();
				s.Do([wall] {
					Rig::ReleaseEverything("m4a aim");
					Cover::ForceReset("m4a aim");
					const auto pos = Snap().position;
					wall->from = { pos.x, pos.y, pos.z + 1.4f };
				});
				for (int k = 0; k < 24; ++k) {
					s.Do([scan, wall, k] {
						scan->Clear();
						for (int o = -2; o <= 2; ++o) {
							const float a = (static_cast<float>(k) * 15.0f + static_cast<float>(o) * 4.0f) * 0.0174533f;
							scan->Add(wall->from, { wall->from.x + std::sin(a) * 14.0f, wall->from.y + std::cos(a) * 14.0f, wall->from.z }, 0.1f);
						}
					});
					s.Until([scan] { return scan->Done(); }, 3.0);
					s.Do([scan, wall, k] {
						for (int o = 0; o < 5; ++o) {
							const auto& r = (*scan)[static_cast<std::size_t>(o)];
							wall->d[k][o] = r.ok && r.hit ? r.fraction * 14.0f : 99.0f;
						}
					});
				}
				s.Do([wall] {
					// a plane across the heading: d(offset) * cos(offset) is the same for the five rays; the best of those 3.5 to 12 m away
					for (int k = 0; k < 24; ++k) {
						bool  allHit = true;
						float err = 0.0f;
						for (int o = 0; o < 5; ++o) {
							allHit = allHit && wall->d[k][o] < 50.0f;
							err = (std::max)(err, std::fabs(wall->d[k][o] * std::cos(static_cast<float>(o - 2) * 4.0f * 0.0174533f) - wall->d[k][2]));
						}
						REX::INFO("P3 M4A WALL scan heading {}: distances {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} m, planar error {:.3f} m{}", k * 15, wall->d[k][0], wall->d[k][1], wall->d[k][2], wall->d[k][3], wall->d[k][4], err, allHit ? "" : " (a ray missed)");
						if (allHit && wall->d[k][2] >= 3.5f && wall->d[k][2] <= 12.0f && err < wall->err) {
							wall->err = err;
							wall->heading = static_cast<float>(k * 15);
							wall->d0 = wall->d[k][2];
							wall->ok = true;
						}
					}
					const float h = wall->heading * 0.0174533f;
					wall->hit = { wall->from.x + std::sin(h) * wall->d0, wall->from.y + std::cos(h) * wall->d0, wall->from.z };
					Result(std::format("P3 M4A WALL: {} heading {:.0f}, {:.2f} m away, planar error {:.3f} m", wall->ok ? "found" : "NOT found, the default heading 102 is used;", wall->heading, wall->d0, wall->err));
					const RE::NiPoint3 to{ wall->hit.x - std::sin(h) * 5.0f, wall->hit.y - std::cos(h) * 5.0f, Snap().position.z };
					if (wall->ok) {
						MoveBegin(to, 1.0);
					}
				});
				s.Until(MoveStep, 3.0);
				s.Do([wall] { s_align = Align{}; });
				s.Until([wall] { return AlignStep(wall->heading); }, 6.0);
				s.Wait(1.0);
				AddWeaponTo(s, true);
				// the distance as it is now: one cast along the view
				s.Do([scan, wall] {
					scan->Clear();
					float yaw{ 0.0f };
					(void)Camera::ReadViewYaw(yaw);
					const auto pos = Snap().position;
					wall->from = { pos.x, pos.y, pos.z + 1.4f };
					scan->Add(wall->from, { wall->from.x + std::sin(yaw) * 14.0f, wall->from.y + std::cos(yaw) * 14.0f, wall->from.z }, 0.1f);
				});
				s.Until([scan] { return scan->Done(); }, 3.0);
				s.Do([scan, wall] {
					const auto& r = (*scan)[0];
					wall->D = r.ok && r.hit ? r.fraction * 14.0f : 0.0f;
					REX::INFO("P3 M4A AIM: the wall is {:.2f} m from the camera's centre ray (cast from the chest, {:.2f} m of eye height above the feet)", wall->D, 1.4f);
				});
				AddAimRound(s, "right", 1);
				AddAimRound(s, "left", -1);
				s.Do([] { Camera::SetShoulder(1); });
				// the view back to the spawn's heading and the player to the test position: the stages of set 1 take the wall behind the player
				s.Do([] { s_align = Align{}; MoveBegin(Base(), 1.0); });
				s.Until(MoveStep, 3.0);
				s.Until([] { return AlignStep(268.9f); }, 6.0);
				s.Wait(0.8);
			}
			return Run(s, c.now);
		}

		constexpr std::array<Stage, 18> kSet36{ {
			{ "ready", StageReady, false },
			{ "M4a the exploring distance: framing on and off, forced sprint", StageM4aProbe, true },
			{ "M4a the aim correction at a plain wall", StageM4aAim, true },
			{ "T3.3b probe: the game movement vector with W, A, S, D held in turn", StageVectorProbe, true },
			{ "T3.3b write test: vector (0, m) written after the original", StageWriteTest, true },
			{ "direction check: W, A, S against the computed input direction", StageDirection, true },
			{ "R1 enter high (S held, Sneak tapped), R3 leave by Sneak", StageEnterHighSneak, true },
			{ "R1 Sneak tap with no key held: the vanilla toggle", StageVanillaToggle, true },
			{ "S3 queue layout: W, D and the aim button held together", StageQueueLayout, true },
			{ "R2 staying on cover: A, D along the wall, W away", StageStay, true },
			{ "R3 leave by Jump", StageEnterHighJump, true },
			{ "R3 leave by Sprint", StageEnterHighSprint, true },
			{ "R3 leave by forcing first person", StageEnterHighFirstPerson, true },
			{ "R6 open ground and first person", StageOpenGround, true },
			{ "M1b R10 mode 3 at the tall wall", StageR10Tall, true },
			{ "summary", StageSummary, false },
		} };

		bool StageFrameOn(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				s.Do([] { Config::Mutable().framing = true; REX::INFO("P3 M4A: the framing is ON for the next stages"); });
			}
			return Run(s, c.now);
		}

		bool StageFrameOff(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				s.Do([] { Config::Mutable().framing = false; REX::INFO("P3 M4A: the framing is OFF for the next stages"); });
			}
			return Run(s, c.now);
		}

		// set 37 (Save30): the pose in, the aim with a shot and its release, out; with the framing on, off, on, off (card M4a: the cover track was lost during the aim)
		constexpr std::array<Stage, 19> kSet37{ {
			{ "ready", StageReady, false },
			{ "R4 find the low wall from either side", StageLowSearch, true },
			{ "framing on", StageFrameOn, false },
			{ "the pose with the weapon drawn", StagePoseIn, true },
			{ "the first aim press with a shot", StagePoseAim, true },
			{ "out", StagePoseOut, true },
			{ "framing off", StageFrameOff, false },
			{ "the pose with the weapon drawn", StagePoseIn, true },
			{ "the first aim press with a shot", StagePoseAim, true },
			{ "out", StagePoseOut, true },
			{ "framing on", StageFrameOn, false },
			{ "the pose with the weapon drawn", StagePoseIn, true },
			{ "the first aim press with a shot", StagePoseAim, true },
			{ "out", StagePoseOut, true },
			{ "framing off", StageFrameOff, false },
			{ "the pose with the weapon drawn", StagePoseIn, true },
			{ "the first aim press with a shot", StagePoseAim, true },
			{ "out", StagePoseOut, true },
			{ "summary", StageSummary, false },
		} };

		bool StagePosesOff(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				s.Do([] {
					Rig::ReleaseEverything("m4a poses off");
					Cover::ForceReset("m4a poses off");
					Config::Mutable().coverPoses = false;
					Config::Mutable().framing = true;
					REX::INFO("P3 M4A: the poses are OFF for the next stages (the Basic cover, what a player without poses gets)");
				});
				s.Wait(0.5);
			}
			return Run(s, c.now);
		}

		// ---- the aim at the low wall's end, 0.66 m from it and 0.5 m farther, the framing on and off (REFERENCE 23.4) ----
		void AddEndAim(Seq& s, const std::string& a_tag)
		{
			const auto note = [a_tag](const char* a_when) {
				const auto ci = Cover::GetInfo();
				const auto p = Snap().position;
				REX::INFO("P3 M4A END [{}] {}: the player at ({:.2f},{:.2f}), cover {} / pose {}, the wall's height as the track reads it {:.2f} m, ends left {:.2f} m ({}) right {:.2f} m ({}), framing {}", a_tag, a_when, p.x, p.y, Core::ToString(ci.state), CoverPose::ToString(CoverPose::GetInfo().state), ci.candidate.height,
					ci.candidate.edgeLeft, Core::ToString(ci.candidate.edgeLeftKind), ci.candidate.edgeRight, Core::ToString(ci.candidate.edgeRightKind), Config::Get().framing ? "on" : "OFF");
			};
			s.Do([note] { note("before the aim"); Rig::MouseRight(true); });
			s.Wait(1.3);
			AddFire(s);
			s.Wait(0.8);
			s.Do([note] { note("after 2.6 s of aim and a burst"); Rig::MouseRight(false); });
			s.Wait(1.5);
		}

		void AddEndSession(Seq& s, const std::shared_ptr<PoseRun>& r, const bool a_framing, const std::string& a_tag)
		{
			s.Do([a_framing, a_tag] {
				Config::Mutable().framing = a_framing;
				Config::Mutable().coverPoses = true;
				Config::Mutable().poseTall = true;
				Config::Mutable().coverCamera = true;
				REX::INFO("P3 M4A END session [{}]: the framing is {}", a_tag, a_framing ? "ON" : "OFF");
			});
			AddPoseEnter(s, r, 0, true);
			s.Wait(1.0);
			s.Do([] { Rig::Tap(PoseLeftKey(), 7.0); });
			s.Wait(7.5);
			s.Do([] { Rig::ReleaseEverything("m4a end walk"); });
			s.Wait(0.8);
			AddEndAim(s, a_tag + "_end");
			s.Do([] { Rig::Tap(PoseRightKey(), 1.9); });  // the first 1.3 s of it turn the character round (REFERENCE 22.x), the rest walks about 0.5 m: the END line has the position
			s.Wait(3.2);
			AddEndAim(s, a_tag + "_plus05");
			AddPoseLeaveSneak(s, r);
			s.Wait(1.0);
		}

		bool StageEndSpot(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				AddEndSession(s, r, true, "on1");
				AddEndSession(s, r, false, "off1");
				AddEndSession(s, r, true, "on2");
				AddEndSession(s, r, false, "off2");
				s.Do([] { Config::Mutable().framing = true; });
			}
			return Run(s, c.now);
		}

		// ---- card M4b step 2: the exit after a stay of any length (REFERENCE 23.6) ----
		struct StayCase  // aggregate: stay, aim, sneak, variant, aimHold
		{
			double stay;      // seconds from "posed" to the leave
			bool   aim;       // an aim (held 1.0 s, 0.3 s after "posed") before the leave
			bool   sneak;     // the leave by the Sneak control instead of pulling away
			int    variant{ 0 };    // iPoseExitVariant for this leave
			double aimHold{ 1.0 };  // how long the aim is held when aim is true
		};

		struct StayAcc
		{
			int clean{ 0 }, dirty{ 0 };
		};

		void AddStayCase(Seq& s, const std::shared_ptr<PoseRun>& r, const std::shared_ptr<StayAcc>& acc, const StayCase a_c, const int a_n)
		{
			s.Do([] {
				if (CoverPose::WeaponState() != 3) {
					(void)AnimRecon::DrawWeapon(true);
				}
			});
			s.Until([] { return CoverPose::WeaponState() == 3; }, 4.0);
			s.Wait(0.5);
			AddPoseEnter(s, r, 0, true);  // 0.5 s after "posed" already passed in the helper
			const double rest = (std::max)(a_c.stay - 0.5, 0.0);
			if (a_c.aim) {
				s.Do([] { Rig::MouseRight(true); });
				s.Wait(a_c.aimHold);
				s.Do([] { Rig::MouseRight(false); });
				s.Wait((std::max)(rest - a_c.aimHold, 0.0));
			} else {
				s.Wait(rest);
			}
			s.Do([r, a_c, a_n] {
				r->tNone = r->tOff = -1.0;
				r->posBefore = Snap().position;
				r->tLeave = Now();
				Config::Mutable().poseExitVariant = 0;
				Config::Mutable().poseExitRemedy = a_c.variant;
				const auto i = CoverPose::GetInfo();
				REX::INFO("P3 M4B STAY #{} variant {}: the leave comes {:.2f} s after posed ({:.2f} s after the pose began), aim {}, by {}; graph variables: bIsInCover {:g} bIsEnteringCover {:g} bIsExitingCover {:g} bIsShuffling {:g} iSyncIdleLocomotion {:g} iSyncRightLeft {:g} iSyncSighted {:g} iIsSighted {:g}; boom y {}",
					a_n, a_c.variant, Now() - i.tPosed, Now() - i.tBegin, a_c.aim ? "yes" : "no", a_c.sneak ? "Sneak" : "pulling away", VarOr("bIsInCover", -1), VarOr("bIsEnteringCover", -1), VarOr("bIsExitingCover", -1), VarOr("bIsShuffling", -1), VarOr("iSyncIdleLocomotion", -1),
					VarOr("iSyncRightLeft", -1), VarOr("iSyncSighted", -1), VarOr("iIsSighted", -1), Camera::ReadRig().n1F8.present ? std::format("{:+.4f}", Camera::ReadRig().n1F8.t.y) : std::string("n/a"));
				if (a_c.sneak) {
					Rig::Tap(Rig::kKeyLCtrl, 0.08);
				} else {
					Rig::Tap(PoseAway(), 2.0);
				}
			});
			s.Until([] { return CoverPose::GetInfo().state == CoverPose::State::Off; }, 7.0, [r](bool ok) { r->tOff = ok ? Now() : -1.0; });
			s.Do([r, acc, a_c, a_n] {
				Rig::ReleaseEverything("m4b stay");
				const auto i = CoverPose::GetInfo();
				const bool clean = r->tOff > 0.0 && !i.usedHolsterFallback && OwnGraph();
				(clean ? acc->clean : acc->dirty)++;
				Result(std::format("P3 M4B STAY #{} (stay {:.1f} s, aim {}, by {}, variant {}): the pose was off {:.2f} s after the leave, own graph back {:.2f} s after the exit call, {} holster fallback (weapon cycle) -> {}", a_n, a_c.stay, a_c.aim ? "yes" : "no", a_c.sneak ? "Sneak" : "away", a_c.variant,
					r->tOff > 0.0 ? r->tOff - r->tLeave : -1.0, i.tOwnBack - i.tLeave, i.usedHolsterFallback ? "with" : "no", Verdict(clean)));
				Summarise(std::format("M4B exit after a stay of {:.1f} s, aim {}, by {}", a_c.stay, a_c.aim ? "yes" : "no", a_c.sneak ? "Sneak" : "away"), clean);
			});
			s.Wait(1.2);
		}

		bool StageShortStay(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				auto acc = std::make_shared<StayAcc>();
				std::vector<StayCase> cases{
					{ 2.0, false, false, 0 }, { 2.0, false, false, 1 }, { 2.0, false, false, 0 }, { 2.0, false, false, 2 }, { 2.0, false, false, 0 }, { 2.0, false, false, 3 }, { 2.0, false, false, 0 }, { 2.0, false, false, 4 },
					{ 2.0, false, false, 0 }, { 2.0, false, false, 5 }, { 2.0, false, false, 1 }, { 2.0, false, false, 2 }, { 2.0, false, false, 3 }, { 2.0, false, false, 4 }, { 2.0, false, false, 5 }, { 2.0, false, false, 0 },
				};
				if (Config::Get().p3Set >= 41) {
					cases.clear();  // the smoke of the default build: eight leaves by pulling away, the default remedy
					for (int k = 0; k < 8; ++k) {
						cases.push_back({ 2.0, false, false, 1 });
					}
				}
				int n = 0;
				for (const auto& cs : cases) {
					AddStayCase(s, r, acc, cs, ++n);
				}
				s.Do([acc] { REX::INFO("P3 M4B STAY total: {} clean, {} not clean", acc->clean, acc->dirty); });
			}
			return Run(s, c.now);
		}

		constexpr std::array<Stage, 4> kSet40{ {
			{ "ready", StageReady, false },
			{ "R4 find the low wall from either side", StageLowSearch, true },
			{ "M4b the exit after a stay of 0.3 to 6 s, with and without an aim", StageShortStay, true },
			{ "summary", StageSummary, false },
		} };

		// ---- card M4b: the player's skeleton by name (a probe: GetObjectByName on the root of the camera nodes' parent chain), the wall plane, the swing (REFERENCE 23.7) ----
		struct ChainNode
		{
			const void* ptr{ nullptr };
			std::string name;
			float       x{ 0 }, y{ 0 }, z{ 0 };
		};

		[[nodiscard]] std::vector<ChainNode> RigChain()
		{
			std::vector<ChainNode> out;
			const auto rig = Camera::ReadRig();
			if (!rig.ok || !rig.n200.ptr) {
				return out;
			}
			std::array<const RE::NiAVObject*, 40> ptrs{};
			std::array<std::array<float, 3>, 40>  pos{};
			std::array<std::array<char, 48>, 40>  nm{};
			int                                   n = 0;
			unsigned long                         code{ 0 };
			const auto                            start = static_cast<const RE::NiAVObject*>(rig.n200.ptr);
			(void)Guarded([&] {
				const RE::NiAVObject* cur = start;
				while (cur && n < 40) {
					ptrs[static_cast<std::size_t>(n)] = cur;
					pos[static_cast<std::size_t>(n)] = { cur->world.translate.x, cur->world.translate.y, cur->world.translate.z };
					const char* s = cur->name.c_str();
					if (s) {
						std::strncpy(nm[static_cast<std::size_t>(n)].data(), s, 47);
					}
					++n;
					cur = cur->parent;
				}
			}, code);
			for (int i = 0; i < n; ++i) {
				ChainNode c;
				c.ptr = ptrs[static_cast<std::size_t>(i)];
				c.name = nm[static_cast<std::size_t>(i)].data();
				c.x = pos[static_cast<std::size_t>(i)][0];
				c.y = pos[static_cast<std::size_t>(i)][1];
				c.z = pos[static_cast<std::size_t>(i)][2];
				out.push_back(std::move(c));
			}
			return out;
		}

		struct BoneRead
		{
			bool        ok{ false };
			float       x{ 0 }, y{ 0 }, z{ 0 };
			float       rot[6]{};  // the world rotation's three rows, x and y of each
			std::string name;
		};

		[[nodiscard]] BoneRead ReadBone(const void* a_root, const char* a_name)
		{
			BoneRead      r;
			char          nm[48]{};
			float         p[3]{};
			float         rr[6]{};
			bool          found = false;
			unsigned long code{ 0 };
			if (!a_root) {
				return r;
			}
			const bool ran = Guarded([&] {
				const RE::BSFixedString fs{ a_name };
				auto*                   node = const_cast<RE::NiAVObject*>(static_cast<const RE::NiAVObject*>(a_root))->GetObjectByName(fs);
				if (node) {
					p[0] = node->world.translate.x;
					p[1] = node->world.translate.y;
					p[2] = node->world.translate.z;
					for (int k = 0; k < 3; ++k) {
						rr[k * 2] = node->world.rotate.entry[k].x;
						rr[k * 2 + 1] = node->world.rotate.entry[k].y;
					}
					const char* s = node->name.c_str();
					if (s) {
						std::strncpy(nm, s, 47);
					}
					found = true;
				}
			}, code);
			if (!ran) {
				REX::WARN("P3 M4B BONE: the lookup of '{}' faulted (0x{:08X})", a_name, code);
				return r;
			}
			r.ok = found;
			r.x = p[0]; r.y = p[1]; r.z = p[2];
			for (int k = 0; k < 6; ++k) {
				r.rot[k] = rr[k];
			}
			r.name = nm;
			return r;
		}

		[[nodiscard]] bool CameraWorld(float (&a_out)[3])
		{
			const auto rig = Camera::ReadRig();
			if (!rig.ok || !rig.n1F8.ptr) {
				return false;
			}
			unsigned long code{ 0 };
			const auto    n = static_cast<const RE::NiAVObject*>(rig.n1F8.ptr);
			return Guarded([&] { a_out[0] = n->world.translate.x; a_out[1] = n->world.translate.y; a_out[2] = n->world.translate.z; }, code);
		}

		struct SwingRun
		{
			const void* root{ nullptr };
			std::vector<std::string> names;  // the bones found, logged per frame
			float  ax{ 0 }, ay{ 0 }, nx{ 0 }, ny{ 0 };  // the wall: a point on its surface and the normal toward the player
			double next{ 0.0 }, t0{ 0.0 };
			float  minHead{ 1e9f }, minSpine{ 1e9f }, minRoot{ 1e9f }, maxDev{ 0.0f }, minCam{ 1e9f };
			std::string phase;
			double headingAtAim{ 0.0 };
			// card M4b corner card: the open end's direction along the wall (unit) and its place along it
			bool  havePlane{ false };
			float tx{ 0 }, ty{ 0 }, uEnd{ 0 };
			float comMargin{ 0 }, weaponMargin{ 0 };
			int   inside{ 0 };  // samples with the COM or the weapon bone behind the wall plane and short of the open end
			float minComD{ 1e9f }, minWeaponD{ 1e9f };
		};

		void SwingProbe(const std::shared_ptr<SwingRun>& a_r)
		{
			const auto chain = RigChain();
			REX::INFO("P3 M4B BONE probe: the parent chain of the camera pivot has {} nodes", chain.size());
			for (std::size_t i = 0; i < chain.size(); ++i) {
				REX::INFO("P3 M4B BONE chain[{}] {} '{}' world ({:+.2f},{:+.2f},{:+.2f})", i, chain[i].ptr, chain[i].name, chain[i].x, chain[i].y, chain[i].z);
			}
			if (chain.empty()) {
				return;
			}
			const auto& p = Snap().position;
			const char* kNames[] = { "Head", "head", "Neck", "SPINE2", "Spine2", "SPINE1", "Spine1", "Pelvis", "COM", "Root", "Camera", "CameraTarget", "Weapon", "WEAPON" };
			// the skeleton's own root first (REFERENCE 23.7: launch 3 of 2026-11-18 took an unnamed ancestor whose lookups found other actors' bones), then the rest from the top
			for (std::size_t ci = 0; ci < chain.size(); ++ci) {
				if (chain[ci].name == "HumanExportRoot" && !a_r->root) {
					a_r->root = chain[ci].ptr;
					REX::INFO("P3 M4B BONE: the skeleton root is chain[{}] 'HumanExportRoot'", ci);
				}
			}
			// the root first, then every ancestor: the first one that finds a name near the player wins
			for (std::size_t ci = chain.size(); ci-- > 0;) {
				int hits = 0;
				for (const char* nmx : kNames) {
					const auto br = ReadBone(chain[ci].ptr, nmx);
					if (br.ok) {
						++hits;
						const float dh = std::hypot(br.x - p.x, br.y - p.y);
						REX::INFO("P3 M4B BONE from chain[{}] '{}': '{}' -> '{}' at ({:+.3f},{:+.3f},{:+.3f}), {:.2f} m from the player horizontally, dz {:+.2f}", ci, chain[ci].name, nmx, br.name, br.x, br.y, br.z, dh, br.z - p.z);
						if (dh < 2.5f && std::fabs(br.z - p.z) < 2.5f && !a_r->root) {
							a_r->root = chain[ci].ptr;
						}
					}
				}
				if (hits > 0 && a_r->root) {
					REX::INFO("P3 M4B BONE: the lookups work from chain[{}] ({} names found)", ci, hits);
					break;
				}
			}
			if (a_r->root) {
				for (const char* nmx : kNames) {
					const auto br = ReadBone(a_r->root, nmx);
					if (br.ok && std::hypot(br.x - p.x, br.y - p.y) < 2.5f && a_r->names.size() < 4 && (std::string(nmx) == "COM" || std::string(nmx) == "Weapon")) {
						a_r->names.emplace_back(nmx);
					}
				}
			}
		}

		void SwingTick(const std::shared_ptr<SwingRun>& a_r)
		{
			const double now = Now();
			if (now < a_r->next) {
				return;
			}
			a_r->next = now + 0.05;
			const auto& p = Snap().position;
			float       heading = 0.0f;
			if (const auto pl = RE::PlayerCharacter::GetSingleton()) {
				heading = pl->data.angle.z * kRad2Deg;
			}
			float yaw{ 0.0f };
			const bool haveYaw = Camera::ReadViewYaw(yaw);
			const auto wd = [&](const float x, const float y) { return (x - a_r->ax) * a_r->nx + (y - a_r->ay) * a_r->ny; };
			std::string bones;
			for (const auto& nmx : a_r->names) {
				const auto br = ReadBone(a_r->root, nmx.c_str());
				if (br.ok) {
					const float d = wd(br.x, br.y);
					bones += std::format(" {}=({:+.2f},{:+.2f},{:+.2f}) d={:+.3f}", nmx, br.x, br.y, br.z, d);
					if (a_r->havePlane) {
						const float m = (br.x - a_r->ax) * a_r->tx + (br.y - a_r->ay) * a_r->ty - a_r->uEnd;
						bones += std::format(" m={:+.3f}", m);
						if (nmx == "COM") { a_r->comMargin = m; a_r->minComD = (std::min)(a_r->minComD, d); } else if (nmx == "Weapon") { a_r->weaponMargin = m; a_r->minWeaponD = (std::min)(a_r->minWeaponD, d); }
						if (d < -0.02f && m < 0.0f) {
							++a_r->inside;
						}
					}
					if (nmx == "Weapon") {
						bones += std::format(" rot[{:+.2f},{:+.2f}|{:+.2f},{:+.2f}|{:+.2f},{:+.2f}]", br.rot[0], br.rot[1], br.rot[2], br.rot[3], br.rot[4], br.rot[5]);
						a_r->minHead = (std::min)(a_r->minHead, d);
					} else {
						a_r->minSpine = (std::min)(a_r->minSpine, d);
					}
				}
			}
			float cam[3]{};
			const bool haveCam = CameraWorld(cam);
			const float dRoot = wd(p.x, p.y);
			a_r->minRoot = (std::min)(a_r->minRoot, dRoot);
			const float hdiff = WrapDeg(heading - static_cast<float>(a_r->headingAtAim));
			a_r->maxDev = (std::max)(a_r->maxDev, std::fabs(hdiff));
			std::string camS = "n/a";
			if (haveCam) {
				const float dc = wd(cam[0], cam[1]);
				a_r->minCam = (std::min)(a_r->minCam, dc);
				camS = std::format("({:+.2f},{:+.2f},{:+.2f}) d={:+.3f}", cam[0], cam[1], cam[2], dc);
			}
			REX::INFO("P3 M4B SWING +{:.2f} [{}] heading {:.1f} (from the aim start {:+.1f}) view {} actor ({:+.3f},{:+.3f}) d={:+.3f}{} camera {}", now - a_r->t0, a_r->phase, heading, hdiff, haveYaw ? std::format("{:.1f}", yaw * kRad2Deg) : std::string("n/a"), p.x, p.y, dRoot, bones, camS);
		}

		// the view turned to an absolute heading (degrees) by mouse movements, a tick of the log on each pass of the loop
		void AddSwingTurn(Seq& s, const std::shared_ptr<SwingRun>& r, const std::string& a_phase, const float a_heading, const double a_hold)
		{
			s.Do([r, a_phase] { r->phase = a_phase; s_align = Align{}; REX::INFO("P3 M4B SWING phase: {}", a_phase); });
			s.Until([r, a_heading] {
				SwingTick(r);
				return AlignStep(a_heading);
			}, 7.0);
			auto t0 = std::make_shared<double>(0.0);
			s.Do([t0] { *t0 = Now(); });
			s.Until([r, t0, a_hold] {
				SwingTick(r);
				return Now() - *t0 >= a_hold;
			}, a_hold + 2.0);
		}

		bool StageGoTall(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				s.Do([] {
					g_tall = true;
					g_autoTall = true;
					Rig::ReleaseEverything("m4b go tall");
					Cover::ForceReset("m4b go tall");
					Config::Mutable().coverPoses = true;
					Config::Mutable().poseTall = true;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					Config::Mutable().autoCoverTall = 2;
					if (Config::Get().p3Set >= 42) {
						Config::Mutable().poseCornerHold = 1;
						Config::Mutable().poseCornerShoulder = true;
						Config::Mutable().poseCornerCamera = true;
					}
					REX::INFO("P3 M4B: the stages from here on run at the tall billboard column, the entry automatic, the corner hold {}", Config::Get().poseCornerHold);
				});
				AddWeaponTo(s, true);
				s.Do([] { FloorBegin(-538.3f, 2047.5f, Snap().position.z); });
				s.Until(FloorPoll, 3.0);
				s.Do([] {
					if (!s_floor.have || std::fabs(s_floor.z - Snap().position.z) > 1.5f) {
						Result(std::format("P3 M4B go tall: no floor near the player's height at the column (floor {})", s_floor.have ? std::format("{:.2f}", s_floor.z) : "none"));
						return;
					}
					const auto& p = Snap().position;
					MoveBegin({ -538.3f, 2047.5f, s_floor.z }, (std::max)(1.0, static_cast<double>(std::hypot(-538.3f - p.x, 2047.5f - p.y) / 3.0f)));
				});
				s.Until(MoveStep, 15.0);
				s.Wait(0.5);
			}
			return Run(s, c.now);
		}

		// the entry timing at the tall column with the weapon drawn: the log lines "COVER automatic", "in cover (reached)" and "POSE begin" carry the numbers
		bool StageTallSmoke(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				AddWeaponTo(s, true);
				AddPoseEnter(s, r, 0, true);
				s.Do([r] {
					const auto i = CoverPose::GetInfo();
					const auto ci = Cover::GetInfo();
					const bool ok = i.state == CoverPose::State::Posed && ci.candidate.kind == Core::CoverKind::High && r->tState > 0.0;
					Result(std::format("P3 M4B tall smoke: automatic entry with the weapon drawn (state {}): Phase 3 state {:.2f} s after the push, the pose {:.2f} s after the push, kind {}, pose {} -> {}", CoverPose::WeaponState(), r->tState > 0.0 ? r->tState - r->tPush : -1.0, r->tPosed > 0.0 ? r->tPosed - r->tPush : -1.0,
						Core::ToString(ci.candidate.kind), CoverPose::ToString(i.state), Verdict(ok)));
					Summarise("M4B tall: automatic entry with the weapon drawn, then the pose", ok);
				});
				s.Wait(1.0);
				AddPoseLeaveSneak(s, r);
				s.Wait(0.5);
			}
			return Run(s, c.now);
		}

		// the tall side aim at an open end, the view turned by +90 and -90 degrees: the measurement of REFERENCE 23.5's diagnosis
		bool StageSwing(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				auto w = std::make_shared<SwingRun>();
				AddWeaponTo(s, true);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.0);
				s.Do([w] {
					const auto ci = Cover::GetInfo();
					w->ax = ci.candidate.anchor.x;
					w->ay = ci.candidate.anchor.y;
					w->nx = ci.candidate.normal.x;
					w->ny = ci.candidate.normal.y;
					if (const auto pl = RE::PlayerCharacter::GetSingleton()) {
						w->headingAtAim = pl->data.angle.z * kRad2Deg;
					}
					w->t0 = Now();
					w->phase = "before the aim";
					REX::INFO("P3 M4B SWING start: pose {}, the wall's anchor ({:.3f},{:.3f}) normal ({:+.3f},{:+.3f}) kind {}, ends left {:.2f} m ({}) right {:.2f} m ({}), the cover's heading {:.1f} deg, the rule says {}", CoverPose::ToString(CoverPose::GetInfo().state), w->ax, w->ay, w->nx, w->ny, Core::ToString(ci.candidate.kind),
						ci.candidate.edgeLeft, Core::ToString(ci.candidate.edgeLeftKind), ci.candidate.edgeRight, Core::ToString(ci.candidate.edgeRightKind), w->headingAtAim, Core::ToString(TallAim::Decide()));
					SwingProbe(w);
				});
				s.Until([w] { SwingTick(w); return Now() - w->t0 >= 1.0; }, 3.0);
				s.Do([w] { w->phase = "aim held"; REX::INFO("P3 M4B SHOT aim start"); Rig::MouseRight(true); });
				s.Until([w] { SwingTick(w); return Now() - w->t0 >= 4.2; }, 6.0);
				s.Do([w] {
					float yaw{ 0.0f };
					(void)Camera::ReadViewYaw(yaw);
					w->headingAtAim = w->headingAtAim;  // the cover's heading stays the reference
					REX::INFO("P3 M4B SHOT aim held");
					REX::INFO("P3 M4B SWING: the aim has been held 3 s: iIsSighted {:g} iSyncSighted {:g} iSyncCoverStates {:g} iSyncRightLeft {:g}, view {:.1f}", VarOr("iIsSighted", -1.0), VarOr("iSyncSighted", -1.0), VarOr("iSyncCoverStates", -1.0), VarOr("iSyncRightLeft", -1.0), yaw * kRad2Deg);
				});
				{
					auto yaw0 = std::make_shared<float>(0.0f);
					s.Do([yaw0] { float y{ 0.0f }; (void)Camera::ReadViewYaw(y); *yaw0 = y * kRad2Deg; });
					auto turnTo = [&](const float a_delta, const char* a_label) {
						s.Do([yaw0, a_delta, w, a_label] {
							w->phase = a_label;
							s_align = Align{};
							REX::INFO("P3 M4B SWING phase: {} (the view from {:.1f} to {:.1f})", a_label, *yaw0, *yaw0 + a_delta);
						});
						s.Until([yaw0, a_delta, w] {
							SwingTick(w);
							return AlignStep(WrapDeg(*yaw0 + a_delta));
						}, 7.0);
						auto t0 = std::make_shared<double>(0.0);
						s.Do([t0] { *t0 = Now(); });
						s.Until([w, t0] { SwingTick(w); return Now() - *t0 >= 1.2; }, 3.5);
						s.Do([a_label] { REX::INFO("P3 M4B SHOT {}", a_label); });
					};
					turnTo(90.0f, "view +90");
					turnTo(0.0f, "view back");
					turnTo(-90.0f, "view -90");
					turnTo(0.0f, "view back again");
				}
				{
					// where a shot straight ahead of the view goes: rays 40 m along the view from the weapon bone and from the actor's head, the first obstruction and its place against the wall
					auto batch = std::make_shared<WorldProbe::Batch>();
					s.Do([w, batch] {
						float yaw{ 0.0f };
						(void)Camera::ReadViewYaw(yaw);
						const auto br = ReadBone(w->root, "Weapon");
						const auto& p = Snap().position;
						batch->Clear();
						const float dx = std::sin(yaw) * 40.0f, dy = std::cos(yaw) * 40.0f;
						if (br.ok) {
							batch->Add({ br.x, br.y, br.z }, { br.x + dx, br.y + dy, br.z }, 0.03f);
						} else {
							batch->Add({ p.x, p.y, p.z + 1.1f }, { p.x + dx, p.y + dy, p.z + 1.1f }, 0.03f);
						}
						batch->Add({ p.x, p.y, p.z + 1.5f }, { p.x + dx, p.y + dy, p.z + 1.5f }, 0.03f);
						REX::INFO("P3 M4B BURST: casts of 40 m along the view {:.1f} deg from the weapon bone {} and from the actor's head", yaw * kRad2Deg, br.ok ? "(found)" : "(not found: from the hip)");
					});
					s.Until([batch] { return batch->Done(); }, 3.0);
					s.Do([w, batch] {
						const char* kWho[] = { "from the weapon bone", "from the actor's head" };
						for (std::size_t i = 0; i < 2; ++i) {
							const auto& r = (*batch)[i];
							if (r.ok && r.hit) {
								REX::INFO("P3 M4B BURST ray {}: the first obstruction {:.2f} m away", kWho[i], r.fraction * 40.0f);
							} else {
								REX::INFO("P3 M4B BURST ray {}: {}", kWho[i], r.ok ? "clear for 40 m" : "the cast did not run");
							}
						}
					});
				}
				s.Do([w] { w->phase = "burst"; REX::INFO("P3 M4B SHOT burst"); Rig::MouseLeft(true); });
				s.Wait(0.4);
				s.Do([w] { Rig::MouseLeft(false); w->phase = "aim released"; REX::INFO("P3 M4B SHOT released"); Rig::MouseRight(false); });
				s.Until([w] { SwingTick(w); return false; }, 2.6, [w](bool) {});
				s.Do([w] {
					const bool bonesOk = !w->names.empty();
					Result(std::format("P3 M4B SWING summary: the lowest signed distance from the wall plane: actor {:+.3f} m, weapon bone {}, COM {}, camera {:+.3f} m; the largest heading change from the aim start {:.1f} deg; bones read: {}",
						w->minRoot, w->minHead < 1e8f ? std::format("{:+.3f} m", w->minHead) : std::string("n/a"), w->minSpine < 1e8f ? std::format("{:+.3f} m", w->minSpine) : std::string("n/a"), w->minCam, w->maxDev, bonesOk ? "yes" : "NO"));
					Summarise("M4B swing measurement made", bonesOk);
				});
				AddPoseLeaveSneak(s, r);
				s.Wait(0.5);
			}
			return Run(s, c.now);
		}

		// ---- card M4b, the corner card (REFERENCE 23.8) ----------------------------------------------------------------------------------------------------------------------
		void CornerCapture(const std::shared_ptr<SwingRun>& w, const int a_side, const char* a_tag)
		{
			const auto  ci = Cover::GetInfo();
			const auto& cd = ci.candidate;
			const float tl = std::hypot(cd.tangent.x, cd.tangent.y);
			w->ax = cd.anchor.x;
			w->ay = cd.anchor.y;
			w->nx = cd.normal.x;
			w->ny = cd.normal.y;
			w->tx = tl > 0.1f ? cd.tangent.x / tl * static_cast<float>(a_side) : 0.0f;
			w->ty = tl > 0.1f ? cd.tangent.y / tl * static_cast<float>(a_side) : 0.0f;
			const float edge = a_side > 0 ? cd.edgeRight : cd.edgeLeft;
			const auto& foot = Snap().position;
			w->uEnd = (foot.x - w->ax) * w->tx + (foot.y - w->ay) * w->ty + edge;
			w->havePlane = true;
			REX::INFO("P3 M4B CORNER [{}] capture: the {} end is {:.2f} m from the actor ({}), the other {:.2f} m ({}); wall anchor ({:.3f},{:.3f}) normal ({:+.3f},{:+.3f}), the end at {:.3f} along the wall", a_tag, a_side > 0 ? "right" : "left", edge, Core::ToString(a_side > 0 ? cd.edgeRightKind : cd.edgeLeftKind),
				a_side > 0 ? cd.edgeLeft : cd.edgeRight, Core::ToString(a_side > 0 ? cd.edgeLeftKind : cd.edgeRightKind), w->ax, w->ay, w->nx, w->ny, w->uEnd);
		}

		[[nodiscard]] int OpenSide()
		{
			const auto& cd = Cover::GetInfo().candidate;
			const bool  r = cd.edgeRightKind == Core::EdgeKind::Open, l = cd.edgeLeftKind == Core::EdgeKind::Open;
			if (r && (!l || cd.edgeRight <= cd.edgeLeft)) {
				return 1;
			}
			return l ? -1 : 1;
		}

		// the actor put at a_target metres from the open end by an eased move along the wall (a walk with the keys turns the pose's side round, REFERENCE 23.8)
		void AddWalkToEdge(Seq& s, const int a_side, const float a_target)
		{
			s.Do([a_side, a_target] {
				const auto& cd = Cover::GetInfo().candidate;
				const float tl = std::hypot(cd.tangent.x, cd.tangent.y);
				const float edge = a_side > 0 ? cd.edgeRight : cd.edgeLeft;
				if (tl < 0.1f) {
					return;
				}
				const float k = static_cast<float>(a_side) * (edge - a_target) / tl;
				const auto& p = Snap().position;
				REX::INFO("P3 M4B CORNER: the actor is put {:.2f} m from the open end (it is {:.2f} m now): a slide of {:+.2f} m along the wall", a_target, edge, edge - a_target);
				MoveBegin({ p.x + cd.tangent.x * k, p.y + cd.tangent.y * k, p.z }, 0.8);
			});
			s.Until(MoveStep, 3.0);
			s.Wait(1.2);
		}

		[[nodiscard]] bool CameraRay(float (&a_pos)[3], float (&a_fwd)[3])
		{
			unsigned long code{ 0 };
			bool          ok = false;
			(void)Guarded([&] {
				const auto cam = RE::PlayerCamera::GetSingleton();
				if (cam && cam->cameraRoot) {
					const auto& w = cam->cameraRoot->world;
					a_pos[0] = w.translate.x; a_pos[1] = w.translate.y; a_pos[2] = w.translate.z;
					a_fwd[0] = w.rotate.entry[1].x; a_fwd[1] = w.rotate.entry[1].y; a_fwd[2] = w.rotate.entry[1].z;
					ok = std::isfinite(a_pos[0]) && std::isfinite(a_fwd[0]);
				}
			}, code);
			return ok;
		}

		void AddTrajectory(Seq& s, const std::shared_ptr<SwingRun>& w, const std::string a_tag)
		{
			auto batch = std::make_shared<WorldProbe::Batch>();
			auto info = std::make_shared<std::array<float, 8>>();  // cam xyz, fwd xyz
			s.Do([batch, info] {
				float cp[3]{}, cf[3]{};
				batch->Clear();
				if (!CameraRay(cp, cf)) {
					REX::INFO("P3 M4B TRAJ: the camera's position could not be read");
					return;
				}
				(*info) = { cp[0], cp[1], cp[2], cf[0], cf[1], cf[2], 1.0f, 0.0f };
				batch->Add({ cp[0], cp[1], cp[2] }, { cp[0] + cf[0] * 60.0f, cp[1] + cf[1] * 60.0f, cp[2] + cf[2] * 60.0f }, 0.02f);
			});
			s.Until([batch, info] { return (*info)[6] < 0.5f || batch->Done(); }, 3.0);
			auto tgt = std::make_shared<std::array<float, 3>>();
			s.Do([batch, info, w, tgt, a_tag] {
				if ((*info)[6] < 0.5f) {
					return;
				}
				const auto& r = (*batch)[0];
				const float dist = r.ok && r.hit ? r.fraction * 60.0f : 60.0f;
				(*tgt) = { (*info)[0] + (*info)[3] * dist, (*info)[1] + (*info)[4] * dist, (*info)[2] + (*info)[5] * dist };
				(*info)[7] = dist;
				const auto br = ReadBone(w->root, "Weapon");
				REX::INFO("P3 M4B TRAJ [{}]: the crosshair line from the camera ({:+.2f},{:+.2f},{:+.2f}) toward ({:+.3f},{:+.3f},{:+.3f}) {} {:.1f} m away at ({:+.2f},{:+.2f},{:+.2f}); the weapon bone {}", a_tag, (*info)[0], (*info)[1], (*info)[2], (*info)[3], (*info)[4], (*info)[5],
					r.ok && r.hit ? "hits at" : "is clear for", dist, (*tgt)[0], (*tgt)[1], (*tgt)[2], br.ok ? std::format("({:+.2f},{:+.2f},{:+.2f})", br.x, br.y, br.z) : std::string("not found"));
				batch->Clear();
				if (br.ok) {
					batch->Add({ br.x, br.y, br.z }, { (*tgt)[0], (*tgt)[1], (*tgt)[2] }, 0.03f);
					(*info)[6] = 2.0f;
				} else {
					(*info)[6] = 0.0f;
				}
			});
			s.Until([batch, info] { return (*info)[6] < 1.5f || batch->Done(); }, 3.0);
			s.Do([batch, info, w, tgt, a_tag] {
				if ((*info)[6] < 1.5f) {
					return;
				}
				const auto& r = (*batch)[0];
				const auto  br = ReadBone(w->root, "Weapon");
				const float len = br.ok ? std::hypot(std::hypot((*tgt)[0] - br.x, (*tgt)[1] - br.y), (*tgt)[2] - br.z) : 0.0f;
				const bool  clear2 = !(r.ok && r.hit && r.fraction * len < 2.0f);
				if (r.ok && r.hit) {
					REX::INFO("P3 M4B TRAJ [{}]: the shot from the weapon bone to the crosshair target ({:.1f} m) is stopped {:.2f} m from the weapon -> {}", a_tag, len, r.fraction * len, clear2 ? "PASS (nothing within 2 m)" : "FAIL (hit within 2 m)");
				} else {
					REX::INFO("P3 M4B TRAJ [{}]: the shot from the weapon bone to the crosshair target ({:.1f} m) is clear all the way -> PASS", a_tag, len);
				}
			});
		}

		struct CornerRes
		{
			std::vector<float> margins;       // the COM's margin beyond the end at the settled hold, per start distance
			std::vector<float> starts;
			int                inside{ 0 };
			bool               trajOk{ true };
		};

		// one aim at the corner from a start distance: walk to it, aim, hold, (the sweep and the burst), release
		void AddCornerCycle(Seq& s, const std::shared_ptr<SwingRun>& w, const std::shared_ptr<CornerRes>& cr, const int a_side, const float a_dist, const bool a_full, const std::string& a_tag)
		{
			AddWalkToEdge(s, a_side, a_dist);
			auto yaw0 = std::make_shared<float>(0.0f);
			auto start = std::make_shared<float>(0.0f);
			s.Do([w, a_side, a_tag, yaw0, start] {
				CornerCapture(w, a_side, a_tag.c_str());
				const auto& cd = Cover::GetInfo().candidate;
				*start = a_side > 0 ? cd.edgeRight : cd.edgeLeft;
				float y{ 0.0f };
				(void)Camera::ReadViewYaw(y);
				*yaw0 = y * kRad2Deg;
				w->phase = a_tag + " idle";
				w->t0 = Now();
				w->inside = 0;
				REX::INFO("P3 M4B SHOT {} aim start", a_tag);
				Rig::MouseRight(true);
			});
			s.Until([w] { SwingTick(w); return Now() - w->t0 >= 3.6; }, 6.0);
			s.Do([w, cr, start, a_tag, a_side] {
				const auto& cd = Cover::GetInfo().candidate;
				const float now = a_side > 0 ? cd.edgeRight : cd.edgeLeft;
				cr->starts.push_back(*start);
				cr->margins.push_back(w->comMargin);
				const auto st = Camera::GetCornerStats();
				REX::INFO("P3 M4B CORNER [{}]: start {:.2f} m from the end -> the actor now {:.2f} m from it; the body (COM) {:+.3f} m beyond the end, the weapon bone {:+.3f} m; samples over the wall's footprint so far {}; hook: {} frames, {} corrections (max {:.2f} deg), {} late (max {:.2f} deg), {} clamps; iIsSighted {:g} side {:g}",
					a_tag, *start, now, w->comMargin, w->weaponMargin, w->inside, st.frames, st.corrections, st.maxDevDeg, st.lateFrames, st.lateMaxDeg, st.clamps, VarOr("iIsSighted", -1.0), VarOr("iSyncRightLeft", -1.0));
				REX::INFO("P3 M4B SHOT {} held", a_tag);
			});
			if (a_full) {
				// the view from limit to limit: ten legs, then both limits pushed further
				auto leg = [&](const float a_delta, const char* a_label, const double a_hold, const bool a_mark) {
					s.Do([yaw0, a_delta, w, a_label] {
						w->phase = a_label;
						s_align = Align{};
					});
					s.Until([yaw0, a_delta, w] {
						SwingTick(w);
						return AlignStep(WrapDeg(*yaw0 + a_delta));
					}, 7.0);
					auto t0 = std::make_shared<double>(0.0);
					s.Do([t0] { *t0 = Now(); });
					s.Until([w, t0, a_hold] { SwingTick(w); return Now() - *t0 >= a_hold; }, a_hold + 2.0);
					if (a_mark) {
						s.Do([a_label, a_tag] { REX::INFO("P3 M4B SHOT {} {}", a_tag, a_label); });
					}
				};
				for (int k = 0; k < 4; ++k) {
					leg(50.0f, "sweep +50", 0.8, false);
					leg(-50.0f, "sweep -50", 0.8, false);
				}
				leg(65.0f, "limit +", 1.2, true);
				leg(-65.0f, "limit -", 1.2, true);
				leg(0.0f, "straight", 1.0, true);
				s.Do([w, cr] {
					const auto st = Camera::GetCornerStats();
					REX::INFO("P3 M4B CORNER sweep done: the heading seen by the rig's samples strayed at most {:.2f} deg from the aim start; hook: {} frames, {} corrections at the hook's entry (max {:.2f} deg), {} late (max {:.2f} deg), {} clamps", w->maxDev, st.frames, st.corrections, st.maxDevDeg, st.lateFrames, st.lateMaxDeg, st.clamps);
				});
				AddTrajectory(s, w, a_tag);
				s.Do([w, a_tag] { w->phase = "burst"; REX::INFO("P3 M4B SHOT {} burst", a_tag); Rig::MouseLeft(true); });
				s.Until([w] { SwingTick(w); return false; }, 0.5, [w](bool) {});
				s.Do([w] { Rig::MouseLeft(false); });
				s.Wait(0.8);
				{
					// the input along the wall during the aim is swallowed: the actor does not move
					auto p0 = std::make_shared<RE::NiPoint3>();
					s.Do([p0] { *p0 = Snap().position; Rig::Tap(PoseRightKey(), 1.2); REX::INFO("P3 M4B CORNER: D (along the wall) held 1.2 s during the corner aim"); });
					s.Wait(1.5);
					s.Do([p0, a_tag] {
						const float moved = Dist2(*p0, Snap().position);
						Result(std::format("P3 M4B CORNER [{}] input along the wall during the corner aim: the actor moved {:.3f} m (swallowed: 0 expected), pose {} -> {}", a_tag, moved, CoverPose::ToString(CoverPose::GetInfo().state), Verdict(moved < 0.05f)));
					});
				}
			}
			s.Do([w, a_tag] { w->phase = "release"; REX::INFO("P3 M4B SHOT {} release", a_tag); Rig::MouseRight(false); w->t0 = Now(); });
			s.Until([w] { SwingTick(w); return Now() - w->t0 >= 2.4; }, 5.0);
			s.Do([w, cr, a_tag] {
				cr->inside += w->inside;
				REX::INFO("P3 M4B CORNER [{}] done: samples over the wall's footprint {}; the lowest signed distance from the wall plane: COM {:+.3f} m, weapon bone {:+.3f} m", a_tag, w->inside, w->minComD, w->minWeaponD);
				w->inside = 0;
			});
			s.Wait(0.5);
		}

		bool StageCornerEnd(const Ctx& c, const bool a_left)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				auto w = std::make_shared<SwingRun>();
				auto cr = std::make_shared<CornerRes>();
				g_spot = a_left ? g_leftSpot : Spot{};  // at construction: AddPoseEnter reads it while the sequence is built
				if (a_left && !g_leftSpot.set) {
					s.Do([] { Result("P3 M4B CORNER left: no left end was found by the ring search: skipped"); });
					return Run(s, c.now);
				}
				s.Do([a_left] {
					g_spot = a_left ? g_leftSpot : Spot{};
					Config::Mutable().poseCornerHold = 1;
					Config::Mutable().poseCornerCamera = true;
					Config::Mutable().poseCornerShoulder = true;
					REX::INFO("P3 M4B CORNER {}: starting", a_left ? "LEFT end" : "RIGHT end");
				});
				AddWeaponTo(s, true);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.0);
				const char* side = a_left ? "L" : "R";
				s.Do([w, side] {
					SwingProbe(w);
					REX::INFO("P3 M4B CORNER {}: pose {}, side {:g} (0 right), the open side is the {}", side, CoverPose::ToString(CoverPose::GetInfo().state), VarOr("iSyncRightLeft", -1.0), OpenSide() > 0 ? "right" : "left");
				});
				const int sd = a_left ? -1 : 1;
				AddCornerCycle(s, w, cr, sd, 0.90f, false, std::string(side) + "_0.9");
				AddCornerCycle(s, w, cr, sd, 0.30f, false, std::string(side) + "_0.3");
				AddCornerCycle(s, w, cr, sd, 0.60f, true, std::string(side) + "_0.6");
				s.Do([cr, w, side] {
					float lo = 1e9f, hi = -1e9f;
					for (const float m : cr->margins) {
						lo = (std::min)(lo, m);
						hi = (std::max)(hi, m);
					}
					const bool spread = !cr->margins.empty() && hi - lo <= 0.05f;
					const bool ok = spread && cr->inside == 0 && !w->names.empty();
					std::string v;
					for (std::size_t i = 0; i < cr->margins.size(); ++i) {
						v += std::format(" start {:.2f} -> {:+.3f}", cr->starts[i], cr->margins[i]);
					}
					Result(std::format("P3 M4B CORNER {} summary: the COM beyond the end at the settled hold:{} (spread {:.3f} m, limit 0.05); samples over the wall's footprint {} (limit 0); bones read {} -> {}", side, v, hi - lo, cr->inside, w->names.empty() ? "NO" : "yes", Verdict(ok)));
					Summarise(std::string("M4B corner ") + side + ": start distances, footprint", ok);
				});
				AddPoseLeaveSneak(s, r);
				s.Do([] { g_spot = Spot{}; });
			}
			return Run(s, c.now);
		}
		bool StageCornerRight(const Ctx& c) { return StageCornerEnd(c, false); }
		bool StageCornerLeft(const Ctx& c) { return StageCornerEnd(c, true); }

		// a ring of places round the kiosk (centre estimated from the user's walk of card M4a: about (-540.0, 2039.5), radius of the wall about 4.4 m): which of them see a tall wall with an open end on the left
		bool StageFindLeft(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				g_leftSpot = Spot{};
				s.Do([] { Rig::ReleaseEverything("m4b ring"); Cover::ForceReset("m4b ring"); });
				for (int k = 0; k < 16; ++k) {
					const float phi = static_cast<float>(k) * 22.5f * kPi / 180.0f;
					const float x = -540.0f + 5.2f * std::sin(phi), y = 2039.5f + 5.2f * std::cos(phi);
					const float h = std::atan2(-std::sin(phi), -std::cos(phi)) * kRad2Deg;
					s.Do([x, y] { FloorBegin(x, y, Snap().position.z); });
					s.Until(FloorPoll, 3.0);
					s.Do([x, y] {
						if (!s_floor.have || std::fabs(s_floor.z - Snap().position.z) > 1.5f) {
							MoveBegin(Snap().position, 0.1);
							return;
						}
						const auto& p = Snap().position;
						MoveBegin({ x, y, s_floor.z }, (std::max)(1.0, static_cast<double>(std::hypot(x - p.x, y - p.y) / 3.5f)));
					});
					s.Until(MoveStep, 12.0);
					s.Do([] { s_align = Align{}; });
					s.Until([h] { return AlignStep(h); }, 6.0);
					s.Wait(0.5);
					s.Do([h] {
						const auto& p = Snap().position;
						const float hr = h * kPi / 180.0f;
						Sensor().SetSettings(CoverSense::Settings());
						(void)Sensor().Find({ p.x, p.y, p.z }, { std::sin(hr), std::cos(hr) });
					});
					s.Until([] { auto& sn = Sensor(); sn.Update(); return !sn.Busy(); }, 5.0);
					s.Do([k, x, y, h] {
						auto& sn = Sensor();
						if (!sn.HasResult()) {
							return;
						}
						const auto  r = sn.TakeResult();
						const auto& cd = r.candidate;
						REX::INFO("P3 M4B RING {} ({:.1f},{:.1f}) heading {:.0f}: {} kind {} left {:.2f} ({}) right {:.2f} ({}) height {:.2f}", k, x, y, h, Core::ToString(cd.why), Core::ToString(cd.kind), cd.edgeLeft, Core::ToString(cd.edgeLeftKind), cd.edgeRight, Core::ToString(cd.edgeRightKind), cd.height);
						if (cd.kind == Core::CoverKind::High && cd.edgeLeftKind == Core::EdgeKind::Open && cd.edgeLeft < 1.3f && cd.edgeRightKind != Core::EdgeKind::Open && !g_leftSpot.set) {
							g_leftSpot = { x, y, Snap().position.z, h, true };
						}
					});
				}
				s.Do([] {
					Result(g_leftSpot.set ? std::format("P3 M4B RING: a tall wall with an open end on the left only: from ({:.1f},{:.1f}) heading {:.0f}", g_leftSpot.x, g_leftSpot.y, g_leftSpot.h) : std::string("P3 M4B RING: no spot with an open end on the left only"));
				});
			}
			return Run(s, c.now);
		}

		// ---- card M4b, the side before the aim (REFERENCE 23.9): ten tries at the column ------------------------------------------------------------------------------------------
		struct TurnTry
		{
			double tStop{ 0.0 }, tPress{ -1.0 }, tAim{ -1.0 }, tFlip{ -1.0 };
			bool   pressTry{ false }, pressed{ false };
			int    sideAtStop{ -1 }, sideAtAim{ -1 };
			float  edgeAtStop{ 0.0f };
		};
		struct TurnRes
		{
			int    pass{ 0 }, fail{ 0 };
			double maxPressToAim{ 0.0 }, maxFlip{ 0.0 };
		};

		// kinds: 0 = a press 0.15 s after the stop (the turn before the aim), 1 = no press (the automatic turn), 2 = a press during the automatic turn
		void AddSideTurnTry(Seq& s, const std::shared_ptr<TurnRes>& res, const int a_n)
		{
			auto tr = std::make_shared<TurnTry>();
			auto t0 = std::make_shared<double>(0.0);
			const int kind = (a_n - 1) % 3;
			s.Do([tr, a_n, t0, kind] {
				*tr = TurnTry{};
				tr->pressTry = kind != 1;
				*t0 = Now();
				Rig::ReleaseEverything("m4b turn try");
				Config::Mutable().tallAutoTurn = false;  // the setup below must not be undone by the automatic turn
				bool acc{ false };
				(void)AnimRecon::SendEvent("EnterCoverStandingLeft", acc);  // the pose's side to Left without a walk
			});
			s.Until([t0] { return VarOr("iSyncRightLeft", 0.0) > 0.5 || Now() - *t0 > 2.5; }, 3.0);
			s.Wait(0.3);
			AddWalkToEdge(s, 1, 0.80f);  // an eased slide along the wall: the right end 0.80 m away, in range
			s.Do([tr, kind] {
				const auto& cd = Cover::GetInfo().candidate;
				tr->sideAtStop = VarOr("iSyncRightLeft", -1.0) > 0.5 ? 1 : 0;
				tr->edgeAtStop = cd.edgeRight;
				if (kind != 0) {
					Config::Mutable().tallAutoTurn = true;
				}
				tr->tStop = Now();
				REX::INFO("P3 M4B TURN try (kind {}): the pose's side is {} (iSyncRightLeft {:g}), the right end {:.2f} m ({}), the left {:.2f} m ({}); the automatic turn {}", kind, tr->sideAtStop ? "Left" : "Right", VarOr("iSyncRightLeft", -1.0), cd.edgeRight, Core::ToString(cd.edgeRightKind), cd.edgeLeft, Core::ToString(cd.edgeLeftKind), kind != 0 ? "ON" : "off");
			});
			s.Do([tr, kind] {
				if (kind == 0) {
					tr->tPress = Now() + 0.15;
				} else if (kind == 2) {
					tr->tPress = Now() + 0.45;
				}
			});
			s.Until([tr, kind] {
				const double now = Now();
				if (tr->pressTry && tr->tPress > 0.0 && now >= tr->tPress && tr->tPress < 1e8 && !tr->pressed) {
					tr->pressed = true;
					Rig::MouseRight(true);
				}
				if (tr->tFlip < 0.0 && VarOr("iSyncRightLeft", 1.0) < 0.5 && tr->sideAtStop == 1) {
					tr->tFlip = now;
				}
				if (tr->pressTry && tr->tAim < 0.0 && VarOr("iIsSighted", 0.0) > 0.5) {
					tr->tAim = now;
					tr->sideAtAim = VarOr("iSyncRightLeft", -1.0) > 0.5 ? 1 : 0;
				}
				if (tr->pressTry) {
					return tr->tAim > 0.0 || now - tr->tStop > 4.5;
				}
				return tr->tFlip > 0.0 || now - tr->tStop > 3.5;
			}, 6.0);
			s.Do([tr, res, a_n, kind] {
				bool ok = false;
				if (tr->pressTry) {
					const double dt = tr->tAim > 0.0 ? tr->tAim - tr->tPress : -1.0;
					ok = tr->tAim > 0.0 && tr->sideAtAim == 0 && dt <= 3.0 && tr->sideAtStop == 1;
					res->maxPressToAim = (std::max)(res->maxPressToAim, dt);
					Result(std::format("P3 M4B TURN #{} (kind {}: a press {:.2f} s after the stop{}): the side was {} at the stop; the aim began {:.2f} s after the press ({:.2f} s after the stop), on the {} side; the side flipped {:.2f} s after the stop -> {}", a_n, kind, tr->tPress - tr->tStop, kind == 2 ? ", during the automatic turn" : "",
						tr->sideAtStop ? "Left" : "Right", dt, tr->tAim > 0.0 ? tr->tAim - tr->tStop : -1.0, tr->sideAtAim == 0 ? "Right" : (tr->sideAtAim == 1 ? "Left" : "?"), tr->tFlip > 0.0 ? tr->tFlip - tr->tStop : -1.0, Verdict(ok)));
				} else {
					const double dt = tr->tFlip > 0.0 ? tr->tFlip - tr->tStop : -1.0;
					ok = tr->tFlip > 0.0 && dt <= 2.0 && VarOr("iIsSighted", 0.0) < 0.5 && tr->sideAtStop == 1;
					res->maxFlip = (std::max)(res->maxFlip, dt);
					Result(std::format("P3 M4B TURN #{} (kind 1: no press): the side was {} at the stop and {} after {:.2f} s (by itself); no aim: {} -> {}", a_n, tr->sideAtStop ? "Left" : "Right", tr->tFlip > 0.0 ? "turned to Right" : "NOT turned", dt, VarOr("iIsSighted", 0.0) < 0.5 ? "yes" : "NO", Verdict(ok)));
				}
				(ok ? res->pass : res->fail)++;
			});
			s.Wait(1.2);
			s.Do([tr] { if (tr->pressTry) { Rig::MouseRight(false); } Config::Mutable().tallAutoTurn = true; });
			s.Wait(2.6);
		}

		bool StageSideTurn(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				auto res = std::make_shared<TurnRes>();
				s.Do([] {
					g_spot = Spot{};
					Config::Mutable().poseCornerHold = 1;
					Config::Mutable().poseCornerCamera = true;
					Config::Mutable().poseCornerShoulder = true;
				});
				AddWeaponTo(s, true);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.0);
				for (int k = 1; k <= 10; ++k) {
					AddSideTurnTry(s, res, k);
				}
				s.Do([res] {
					const bool ok = res->pass == 10 && res->fail == 0;
					Result(std::format("P3 M4B TURN summary: {} of 10 tries passed; the longest press-to-aim time {:.2f} s; the longest time to turn by itself {:.2f} s -> {}", res->pass, res->maxPressToAim, res->maxFlip, Verdict(ok)));
					Summarise("M4B the side before the aim, ten tries", ok);
				});
				AddPoseLeaveSneak(s, r);
			}
			return Run(s, c.now);
		}

		// the user's left-end aim of the play (REFERENCE 23.5 / 23.9): the kiosk's east face, stand point (-535.339, 2040.866), wall normal (+0.96, +0.27)
		bool StageLeftSetup(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				s.Do([] { Rig::ReleaseEverything("m4b left setup"); Cover::ForceReset("m4b left setup"); FloorBegin(-534.2f, 2041.2f, Snap().position.z); });
				s.Until(FloorPoll, 3.0);
				s.Do([] {
					if (!s_floor.have || std::fabs(s_floor.z - Snap().position.z) > 1.5f) {
						Result("P3 M4B left setup: no floor at the left-end spot");
						return;
					}
					g_leftSpot = { -534.2f, 2041.2f, s_floor.z, 254.3f, true };
					Result(std::format("P3 M4B left setup: the spot ({:.2f},{:.2f},{:.2f}) heading {:.1f}", g_leftSpot.x, g_leftSpot.y, g_leftSpot.z, g_leftSpot.h));
				});
			}
			return Run(s, c.now);
		}

		constexpr std::array<Stage, 6> kSet44{ {
			{ "ready", StageReady, false },
			{ "M4b go to the tall column (the corner card on)", StageGoTall, true },
			{ "M4b the side before the aim: ten tries", StageSideTurn, true },
			{ "M4b the user's left end at the kiosk: the spot", StageLeftSetup, true },
			{ "M4b the corner at the left end", StageCornerLeft, true },
			{ "summary", StageSummary, false },
		} };

		// ---- card M4b, the tall walk (REFERENCE 23.10): the skeleton's tree, listed once ----------------------------------------------------------------------------------------
		struct ChildInfo
		{
			const void* ptr{ nullptr };
			std::string name;
			float       x{ 0 }, y{ 0 }, z{ 0 };
		};

		// the children of a node. The library's layout of NiNode::children is not trusted: the node's memory from +0x130 on is scanned for a pointer whose pointee is a run of
		// objects whose parent field (+0x38) is the node (the array), or for a direct child pointer; every child must name the node as its parent
		[[nodiscard]] std::vector<ChildInfo> ListChildren(const void* a_node)
		{
			std::vector<ChildInfo> out;
			if (!a_node) {
				return out;
			}
			struct Raw
			{
				const void* ptr;
				char        name[48];
				float       pos[3];
			};
			std::array<Raw, 96> raw{};
			int                 n = 0;
			unsigned long       code{ 0 };
			(void)Guarded([&] {
				const auto base = reinterpret_cast<const char*>(a_node);
				const auto parentOf = [&](const void* o, const void*& a_out) -> bool {
					if (!o || (reinterpret_cast<std::uintptr_t>(o) & 7) != 0) {
						return false;
					}
					return SafeRead(reinterpret_cast<const char*>(o) + 0x38, a_out);
				};
				for (std::ptrdiff_t off = 0x130; off < 0x1F8 && n == 0; off += 8) {
					const void* v{ nullptr };
					if (!SafeRead(base + off, v) || !v || (reinterpret_cast<std::uintptr_t>(v) & 7) != 0) {
						continue;
					}
					// the array: v points to a run of child pointers
					int         run = 0;
					const void* kids[96]{};
					for (int k = 0; k < 96; ++k) {
						const void* c{ nullptr };
						const void* par{ nullptr };
						if (!SafeRead(reinterpret_cast<const char*>(v) + k * 8, c) || !parentOf(c, par) || par != a_node) {
							break;
						}
						kids[run++] = c;
					}
					if (run == 0) {
						continue;
					}
					for (int k = 0; k < run && n < 96; ++k) {
						auto&       r = raw[static_cast<std::size_t>(n++)];
						const auto  c = static_cast<const RE::NiAVObject*>(kids[k]);
						r.ptr = c;
						const char* s = c->name.c_str();
						if (s) {
							std::strncpy(r.name, s, 47);
						}
						r.pos[0] = c->world.translate.x; r.pos[1] = c->world.translate.y; r.pos[2] = c->world.translate.z;
					}
				}
			}, code);
			for (int k = 0; k < n; ++k) {
				ChildInfo c;
				c.ptr = raw[static_cast<std::size_t>(k)].ptr;
				c.name = raw[static_cast<std::size_t>(k)].name;
				c.x = raw[static_cast<std::size_t>(k)].pos[0]; c.y = raw[static_cast<std::size_t>(k)].pos[1]; c.z = raw[static_cast<std::size_t>(k)].pos[2];
				out.push_back(std::move(c));
			}
			return out;
		}

		// every node under a_root (depth first, at most a_max), the names and positions
		[[nodiscard]] std::vector<std::pair<int, ChildInfo>> WalkSkeleton(const void* a_root, const int a_max)
		{
			std::vector<std::pair<int, ChildInfo>> all;
			struct Item { const void* p; int depth; };
			std::vector<Item> stack{ { a_root, 0 } };
			while (!stack.empty() && static_cast<int>(all.size()) < a_max) {
				const auto it = stack.back();
				stack.pop_back();
				const auto kids = ListChildren(it.p);
				for (auto k = kids.rbegin(); k != kids.rend(); ++k) {
					all.emplace_back(it.depth + 1, *k);
					if (it.depth < 16) {
						stack.push_back({ k->ptr, it.depth + 1 });
					}
				}
			}
			return all;
		}

		[[nodiscard]] const void* SkeletonRoot()
		{
			for (const auto& n : RigChain()) {
				if (n.name == "HumanExportRoot") {
					return n.ptr;
				}
			}
			return nullptr;
		}

		bool StageSkeleton(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				s.Do([] {
					const void* root = SkeletonRoot();
					if (!root) {
						Result("P3 M4B SKELETON: HumanExportRoot not found");
						return;
					}
					const auto all = WalkSkeleton(root, 900);
					for (const auto& [d, k] : all) {
						REX::INFO("P3 M4B SKELETON {}{} ({:+.2f},{:+.2f},{:+.2f})", std::string(static_cast<std::size_t>(d) * 2, ' '), k.name.empty() ? "(unnamed)" : k.name, k.x, k.y, k.z);
					}
					if (all.empty()) {
						// what the node's memory looks like, for the next try
						unsigned long code{ 0 };
						(void)Guarded([&] {
							const auto base = reinterpret_cast<const std::uint64_t*>(root);
							for (int q = 0x130 / 8; q < 0x1F8 / 8; q += 4) {
								REX::INFO("P3 M4B SKELETON raw +0x{:X}: {:016X} {:016X} {:016X} {:016X}", q * 8, base[q], base[q + 1], base[q + 2], base[q + 3]);
							}
						}, code);
					}
					Result(std::format("P3 M4B SKELETON: {} nodes listed under HumanExportRoot", all.size()));
				});
			}
			return Run(s, c.now);
		}

		// the smoke of the play candidate (REFERENCE 23.10): the stages of set 41 and a corner aim at each end
		constexpr std::array<Stage, 13> kSet45{ {
			{ "ready", StageReady, false },
			{ "R4 find the kiosk's low wall", StageLowSearch, true },
			{ "M4b the low pose with the weapon drawn, 3 s idle", StagePoseIn, true },
			{ "M4b the first aim press with a shot (low, over)", StagePoseAim, true },
			{ "M4b the exit after a stay, pulling away, eight times", StageShortStay, true },
			{ "M4b blind fire: a hold and a tap", StageBlindTail, true },
			{ "M4b go to the tall column", StageGoTall, true },
			{ "M4b tall cover with the weapon drawn: the automatic entry and the pose", StageTallSmoke, true },
			{ "M4b the corner at the right end", StageCornerRight, true },
			{ "M4b the user's left end at the kiosk: the spot", StageLeftSetup, true },
			{ "M4b the corner at the left end", StageCornerLeft, true },
			{ "M4b the skeleton's node names", StageSkeleton, true },
			{ "summary", StageSummary, false },
		} };

		// ---- card M4b, the tall walk's yardstick (REFERENCE 23.10): the feet while the actor walks along the wall ---------------------------------------------------------------
		struct WalkRun
		{
			const void*              root{ nullptr };
			std::vector<std::string> bones;
			double                   next{ 0.0 }, t0{ 0.0 };
			std::string              phase;
			double                   nextTurn{ 0.0 };
			float                    sign{ 1.0f }, lastErr{ 0.0f };
			bool                     haveLast{ false };
		};

		// the view kept on the wall (the way a player turns the camera round a curve): the keys stay along the wall
		void WalkFollow(const std::shared_ptr<WalkRun>& w)
		{
			const double now = Now();
			if (now < w->nextTurn) {
				return;
			}
			const auto& cd = Cover::GetInfo().candidate;
			if (cd.kind == Core::CoverKind::None) {
				return;
			}
			float yaw{ 0.0f };
			if (!Camera::ReadViewYaw(yaw)) {
				return;
			}
			const float target = std::atan2(-cd.normal.x, -cd.normal.y) * kRad2Deg;
			const float err = WrapDeg(target - yaw * kRad2Deg);
			if (w->haveLast && std::fabs(err) > std::fabs(w->lastErr) + 1.0f) {
				w->sign = -w->sign;
			}
			w->lastErr = err;
			w->haveLast = true;
			if (std::fabs(err) < 2.5f) {
				return;
			}
			Rig::MouseMove(static_cast<int>(std::clamp(w->sign * err / 0.34f, -120.0f, 120.0f)), 0);
			w->nextTurn = now + 0.12;
		}

		void WalkSetup(const std::shared_ptr<WalkRun>& w)
		{
			w->root = SkeletonRoot();
			w->bones.clear();
			if (!w->root) {
				REX::INFO("P3 M4B WALK: no skeleton root");
				return;
			}
			const auto all = WalkSkeleton(w->root, 900);
			for (const auto& [d, k] : all) {
				std::string low = k.name;
				for (auto& ch : low) {
					ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
				}
				const bool foot = low.find("foot") != std::string::npos || low.find("toe") != std::string::npos || low.find("ankle") != std::string::npos || low.find("ball") != std::string::npos;
				const bool key = low == "com" || low == "root" || low.find("pelvis") != std::string::npos || low.find("calf") != std::string::npos || low.find("shin") != std::string::npos;
				if ((foot || key) && w->bones.size() < 12) {
					w->bones.push_back(k.name);
				}
			}
			std::string joined;
			for (const auto& n : w->bones) {
				joined += n + " ";
			}
			REX::INFO("P3 M4B WALK: {} nodes in the skeleton, the bones logged: {}", all.size(), joined);
		}

		void WalkTick(const std::shared_ptr<WalkRun>& w)
		{
			const double now = Now();
			if (now < w->next) {
				return;
			}
			w->next = now + 0.02;
			const auto& p = Snap().position;
			std::string bs;
			for (const auto& nm : w->bones) {
				const auto br = ReadBone(w->root, nm.c_str());
				if (br.ok) {
					bs += std::format(" {}=({:+.3f},{:+.3f},{:+.3f})", nm, br.x, br.y, br.z);
				}
			}
			REX::INFO("P3 M4B WALK +{:.3f} [{}] actor=({:+.3f},{:+.3f},{:+.3f}) Speed={:g} Dir={:g} idle={:g} side={:g} cov={}{}", now - w->t0, w->phase, p.x, p.y, p.z, VarOr("Speed", -1.0), VarOr("Direction", -1.0), VarOr("iSyncIdleLocomotion", -1.0), VarOr("iSyncRightLeft", -1.0), Core::ToString(Cover::GetInfo().state), bs);
		}

		void AddWalkLegKeys(Seq& s, const std::shared_ptr<WalkRun>& w, const Rig::Key a_key, const double a_secs, const std::string& a_phase)
		{
			s.Do([w, a_key, a_secs, a_phase] {
				w->phase = a_phase;
				REX::INFO("P3 M4B WALK phase: {} (the key held {:.1f} s) pose {} side {:g}", a_phase, a_secs, CoverPose::ToString(CoverPose::GetInfo().state), VarOr("iSyncRightLeft", -1.0));
				Rig::Tap(a_key, a_secs);
			});
			auto t0 = std::make_shared<double>(0.0);
			s.Do([t0] { *t0 = Now(); });
			s.Until([w, t0, a_secs] { WalkFollow(w); WalkTick(w); return Now() - *t0 >= a_secs + 1.0; }, a_secs + 3.0);
			s.Do([a_phase] { REX::INFO("P3 M4B WALK phase end: {} pose {} side {:g}", a_phase, CoverPose::ToString(CoverPose::GetInfo().state), VarOr("iSyncRightLeft", -1.0)); });
		}

		bool StageWalkCurve(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				auto w = std::make_shared<WalkRun>();
				g_tall = true;
				g_autoTall = true;
				g_spot = g_leftSpot;
				s.Do([] {
					Rig::ReleaseEverything("m4b walk");
					Cover::ForceReset("m4b walk");
					Config::Mutable().coverPoses = true;
					Config::Mutable().poseTall = true;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					Config::Mutable().autoCoverTall = 2;
					Config::Mutable().poseCornerHold = 1;
					REX::INFO("P3 M4B WALK: the tall walk on the kiosk's curve, from the user's left-end spot");
				});
				AddWeaponTo(s, true);
				// a slow move to the spot (the floor first)
				s.Do([] { FloorBegin(g_spot.x, g_spot.y, Snap().position.z); });
				s.Until(FloorPoll, 3.0);
				s.Do([] {
					if (s_floor.have && std::fabs(s_floor.z - Snap().position.z) < 1.5f) {
						const auto& p = Snap().position;
						MoveBegin({ g_spot.x, g_spot.y, s_floor.z }, (std::max)(1.0, static_cast<double>(std::hypot(g_spot.x - p.x, g_spot.y - p.y) / 3.5f)));
					}
				});
				s.Until(MoveStep, 15.0);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.0);
				s.Do([w] { w->t0 = Now(); WalkSetup(w); });
				const std::vector<float> cfgs{ 0.50f, 0.67f };
				for (const float v : cfgs) {
					s.Do([v] { Config::Mutable().coverMaxInput = v; REX::INFO("P3 M4B WALK: fCoverMaxInput {:.2f}", v); });
					AddWalkLegKeys(s, w, PoseRightKey(), 4.5, std::format("D@{:.2f}", v));
					s.Wait(1.0);
					AddWalkLegKeys(s, w, PoseLeftKey(), 4.5, std::format("A@{:.2f}", v));
					s.Wait(1.0);
					AddWalkLegKeys(s, w, PoseRightKey(), 4.5, std::format("D2@{:.2f}", v));
					s.Wait(1.0);
					AddWalkLegKeys(s, w, PoseLeftKey(), 4.5, std::format("A2@{:.2f}", v));
					s.Wait(1.0);
				}
				s.Do([] { Config::Mutable().coverMaxInput = 0.5f; });
				AddPoseLeaveSneak(s, r);
				s.Do([] { g_spot = Spot{}; });
			}
			return Run(s, c.now);
		}

		constexpr std::array<Stage, 5> kSet46{ {
			{ "ready", StageReady, false },
			{ "M4b the user's left-end spot (floor)", StageLeftSetup, true },
			{ "M4b the skeleton's node names", StageSkeleton, true },
			{ "M4b the tall walk on the kiosk's curve: the feet, three speeds, both ways", StageWalkCurve, true },
			{ "summary", StageSummary, false },
		} };

		// ---- card M4b, the walk reversed without a stop (REFERENCE 23.11) ----------------------------------------------------------------------------------------------------
		// one leg: the other keys let go and this one pressed in the same tick (the stick thrown over), held a_secs
		void AddKeyLeg(Seq& s, const std::shared_ptr<WalkRun>& w, const Rig::Key a_key, const double a_secs, const std::string& a_phase)
		{
			auto t0 = std::make_shared<double>(0.0);
			s.Do([w, a_key, a_secs, a_phase, t0] {
				w->phase = a_phase;
				*t0 = Now();
				Rig::ReleaseEverything("m4b reverse leg");
				REX::INFO("P3 M4B REV leg {}: pose {} side {:g} idle {:g}", a_phase, CoverPose::ToString(CoverPose::GetInfo().state), VarOr("iSyncRightLeft", -1.0), VarOr("iSyncIdleLocomotion", -1.0));
			});
			auto tg = std::make_shared<double>(0.0);
			s.Do([tg] { *tg = Now(); });
			s.Until([tg] { return Now() - *tg >= 0.04; }, 0.5);  // the old key is up before the new one goes down (a held key that is pressed while another is still down is zeroed by the pose and then read as released)
			s.Do([a_key, a_secs] {
				Rig::Tap(a_key, a_secs + 0.5);
			});
			s.Until([w, t0, a_secs] { WalkFollow(w); WalkTick(w); return Now() - *t0 >= a_secs; }, a_secs + 2.0);
		}

		std::shared_ptr<RE::NiPoint3> g_revHome;  // where the cases start (the stage sets it after the pose's entry)

		void AddRevCase(Seq& s, const std::shared_ptr<WalkRun>& w, const int a_n, const int a_mode, const bool a_afterAim, const bool a_startRight, const double a_stop)
		{
			const std::string tag = std::format("c{}_{}_{}_{}_s{:.1f}", a_n, a_mode == 0 ? "base" : (a_mode == 1 ? "fix" : "fix2"), a_afterAim ? "aimed" : "noaim", a_startRight ? "R" : "L", a_stop);
			const Rig::Key k1 = a_startRight ? PoseRightKey() : PoseLeftKey();
			const Rig::Key k2 = a_startRight ? PoseLeftKey() : PoseRightKey();
			s.Do([a_mode] { Config::Mutable().poseReverse = a_mode; });
			AddKeyLeg(s, w, k1, 1.5, tag + ":1");
			if (a_stop > 0.0) {
				s.Do([w, tag] { w->phase = tag + ":stop"; Rig::ReleaseEverything("m4b reverse stop"); });
				auto t0 = std::make_shared<double>(0.0);
				s.Do([t0] { *t0 = Now(); });
				s.Until([w, t0, a_stop] { WalkTick(w); return Now() - *t0 >= a_stop; }, a_stop + 1.0);
			}
			AddKeyLeg(s, w, k2, 2.5, tag + ":2");
			s.Do([w, tag] { w->phase = tag + ":rest"; Rig::ReleaseEverything("m4b reverse rest"); });
			auto t1 = std::make_shared<double>(0.0);
			s.Do([t1] { *t1 = Now(); });
			s.Until([w, t1] { WalkTick(w); return Now() - *t1 >= 1.0; }, 2.5);
			s.Do([] {
				if (g_revHome) {
					const auto& p = Snap().position;
					MoveBegin({ g_revHome->x, g_revHome->y, p.z }, 0.8);
				}
			});
			s.Until(MoveStep, 3.0);
			s.Wait(0.4);
		}

		void AddRevAim(Seq& s)
		{
			s.Do([] { REX::INFO("P3 M4B REV: one aim"); Rig::MouseRight(true); });
			s.Wait(2.0);
			s.Do([] { Rig::MouseRight(false); });
			s.Wait(2.6);
		}

		bool StageReverseSetup(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				s.Do([] { Rig::ReleaseEverything("m4b rev setup"); Cover::ForceReset("m4b rev setup"); FloorBegin(-419.0f, 2114.6f, Snap().position.z); });
				s.Until(FloorPoll, 3.0);
				s.Do([] {
					if (!s_floor.have || std::fabs(s_floor.z - Snap().position.z) > 1.5f) {
						Result("P3 M4B REV setup: no floor at the straight tall wall's spot");
						return;
					}
					g_leftSpot = { -419.0f, 2114.6f, s_floor.z, 0.0f, true };
					Result(std::format("P3 M4B REV setup: the spot ({:.2f},{:.2f},{:.2f}) heading 0", g_leftSpot.x, g_leftSpot.y, g_leftSpot.z));
				});
			}
			return Run(s, c.now);
		}

		bool StageReverseTall(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				auto w = std::make_shared<WalkRun>();
				g_tall = true;
				g_autoTall = true;
				g_spot = g_leftSpot;
				s.Do([] {
					Rig::ReleaseEverything("m4b rev tall");
					Cover::ForceReset("m4b rev tall");
					Config::Mutable().coverPoses = true;
					Config::Mutable().poseTall = true;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					Config::Mutable().autoCoverTall = 2;
					Config::Mutable().poseCornerHold = 1;
					REX::INFO("P3 M4B REV: the straight tall wall, reversals");
				});
				AddWeaponTo(s, true);
				s.Do([] { FloorBegin(g_spot.x, g_spot.y, Snap().position.z); });
				s.Until(FloorPoll, 3.0);
				s.Do([] {
					if (s_floor.have && std::fabs(s_floor.z - Snap().position.z) < 1.5f) {
						const auto& p = Snap().position;
						MoveBegin({ g_spot.x, g_spot.y, s_floor.z }, (std::max)(1.0, static_cast<double>(std::hypot(g_spot.x - p.x, g_spot.y - p.y) / 3.5f)));
					}
				});
				s.Until(MoveStep, 15.0);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.0);
				s.Do([w] { w->t0 = Now(); WalkSetup(w); g_revHome = std::make_shared<RE::NiPoint3>(Snap().position); });
				int n = 0;
				const int rounds = 1;
				for (int aimed = 0; aimed < 2; ++aimed) {
					if (aimed == 1) {
						AddRevAim(s);
					}
					for (int fix = 0; fix < 2; ++fix) {
						for (int rr = 0; rr < (fix == 1 ? 2 : rounds); ++rr) {
							for (const bool startRight : { true, false }) {
								for (const double stop : { 0.0, 0.3, 1.0 }) {
									AddRevCase(s, w, ++n, fix, aimed == 1, startRight, stop);
								}
							}
						}
					}
				}
				s.Do([] { Config::Mutable().poseReverse = 1; });
				AddPoseLeaveSneak(s, r);
				s.Do([] { g_spot = Spot{}; });
			}
			return Run(s, c.now);
		}

		bool StageReverseLow(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				auto w = std::make_shared<WalkRun>();
				g_tall = false;
				g_autoTall = false;
				g_spot = Spot{};
				s.Do([] {
					Rig::ReleaseEverything("m4b rev low");
					Cover::ForceReset("m4b rev low");
					Config::Mutable().coverPoses = true;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					REX::INFO("P3 M4B REV: the low wall, reversals");
				});
				AddWeaponTo(s, true);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.0);
				s.Do([w] { w->t0 = Now(); WalkSetup(w); g_revHome = std::make_shared<RE::NiPoint3>(Snap().position); });
				int n = 100;
				for (int fix = 0; fix < 2; ++fix) {
					for (const bool startRight : { true, false }) {
						for (const double stop : { 0.0, 0.3, 1.0 }) {
							AddRevCase(s, w, ++n, fix, false, startRight, stop);
						}
					}
				}
				s.Do([] { Config::Mutable().poseReverse = 1; });
				AddPoseLeaveSneak(s, r);
			}
			return Run(s, c.now);
		}

		constexpr std::array<Stage, 6> kSet47{ {
			{ "ready", StageReady, false },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "M4b the tall walk reversed: with and without the fix, before and after an aim", StageReverseTall, true },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "M4b the low walk reversed", StageReverseLow, true },
			{ "summary", StageSummary, false },
		} };

		// ---- card M4b, the proof of the reversal rule (REFERENCE 23.12) ----------------------------------------------------------------------------------------------------------
		void AddRevToEndAndAim(Seq& s)
		{
			// walk right (the engine's own walk) until the open right end is within 0.85 m, let the side turn to Right, one corner aim, let go
			auto t0 = std::make_shared<double>(0.0);
			auto nextTap = std::make_shared<double>(0.0);
			s.Do([t0] { *t0 = Now(); });
			s.Until([t0, nextTap] {
				const auto& cd = Cover::GetInfo().candidate;
				if ((cd.edgeRightKind == Core::EdgeKind::Open && cd.edgeRight <= 0.85f) || Now() - *t0 > 5.0) {
					Rig::ReleaseEverything("m4b rev to end");
					return true;
				}
				if (Now() >= *nextTap) {
					*nextTap = Now() + 0.22;
					Rig::Tap(PoseRightKey(), 0.3);
				}
				return false;
			}, 6.0);
			s.Wait(2.0);
			s.Do([] { REX::INFO("P3 M4B REV: a corner aim at the right end (side {:g}, the right end {:.2f} m)", VarOr("iSyncRightLeft", -1.0), Cover::GetInfo().candidate.edgeRight); Rig::MouseRight(true); });
			s.Wait(2.0);
			s.Do([] { Rig::MouseRight(false); });
			s.Wait(2.6);
		}

		bool StageReverseProof(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				auto w = std::make_shared<WalkRun>();
				g_tall = true;
				g_autoTall = true;
				g_spot = g_leftSpot;
				s.Do([] {
					Rig::ReleaseEverything("m4b rev proof");
					Cover::ForceReset("m4b rev proof");
					Config::Mutable().coverPoses = true;
					Config::Mutable().poseTall = true;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					Config::Mutable().autoCoverTall = 2;
					Config::Mutable().poseCornerHold = 1;
					Config::Mutable().poseReverse = 1;
					REX::INFO("P3 M4B REV: the proof at the middle of the straight tall wall: 20 reversals and 5 after a corner aim");
				});
				AddWeaponTo(s, true);
				s.Do([] { FloorBegin(g_spot.x, g_spot.y, Snap().position.z); });
				s.Until(FloorPoll, 3.0);
				s.Do([] {
					if (s_floor.have && std::fabs(s_floor.z - Snap().position.z) < 1.5f) {
						const auto& p = Snap().position;
						MoveBegin({ g_spot.x, g_spot.y, s_floor.z }, (std::max)(1.0, static_cast<double>(std::hypot(g_spot.x - p.x, g_spot.y - p.y) / 3.5f)));
					}
				});
				s.Until(MoveStep, 15.0);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.0);
				s.Do([w] { w->t0 = Now(); WalkSetup(w); g_revHome = std::make_shared<RE::NiPoint3>(Snap().position); });
				int n = 0;
				for (int k = 0; k < 5; ++k) {
					AddRevCase(s, w, ++n, 1, false, true, 0.0);
					AddRevCase(s, w, ++n, 1, false, false, 0.0);
					AddRevCase(s, w, ++n, 1, false, true, 0.3);
					AddRevCase(s, w, ++n, 1, false, false, 0.3);
				}
				for (int k = 0; k < 5; ++k) {
					AddRevToEndAndAim(s);
					AddRevCase(s, w, ++n, 1, true, false, (k % 2) * 0.3);  // away from the end first, then the reversal toward it
				}
				AddPoseLeaveSneak(s, r);
				s.Do([] { g_spot = Spot{}; });
			}
			return Run(s, c.now);
		}

		// the low wall: the default (tall only) leaves it as it was (the backward walk is recorded), mode 2 fixes it
		bool StageReverseLowProof(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				auto w = std::make_shared<WalkRun>();
				g_tall = false;
				g_autoTall = false;
				g_spot = Spot{};
				s.Do([] {
					Rig::ReleaseEverything("m4b rev low proof");
					Cover::ForceReset("m4b rev low proof");
					Config::Mutable().coverPoses = true;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					REX::INFO("P3 M4B REV: the low wall: the default (tall only), then mode 2");
				});
				AddWeaponTo(s, true);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.0);
				s.Do([w] { w->t0 = Now(); WalkSetup(w); g_revHome = std::make_shared<RE::NiPoint3>(Snap().position); });
				int n = 100;
				for (const bool startRight : { true, false }) {
					AddRevCase(s, w, ++n, 1, false, startRight, 0.3);  // mode 1 = tall only: low is not touched
				}
				for (const bool startRight : { true, false }) {
					AddRevCase(s, w, ++n, 2, false, startRight, 0.3);
				}
				s.Do([] { Config::Mutable().poseReverse = 1; });
				AddPoseLeaveSneak(s, r);
			}
			return Run(s, c.now);
		}

		// the smoke's three reversals, on the kiosk's curve from the user's left-end spot
		bool StageReverseCurve(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				auto w = std::make_shared<WalkRun>();
				g_tall = true;
				g_autoTall = true;
				g_spot = g_leftSpot;
				s.Do([] {
					Rig::ReleaseEverything("m4b rev curve");
					Cover::ForceReset("m4b rev curve");
					Config::Mutable().coverPoses = true;
					Config::Mutable().poseTall = true;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					Config::Mutable().autoCoverTall = 2;
					REX::INFO("P3 M4B REV: three reversals on the kiosk's curve");
				});
				AddWeaponTo(s, true);
				s.Do([] { FloorBegin(g_spot.x, g_spot.y, Snap().position.z); });
				s.Until(FloorPoll, 3.0);
				s.Do([] {
					if (s_floor.have && std::fabs(s_floor.z - Snap().position.z) < 1.5f) {
						const auto& p = Snap().position;
						MoveBegin({ g_spot.x, g_spot.y, s_floor.z }, (std::max)(1.0, static_cast<double>(std::hypot(g_spot.x - p.x, g_spot.y - p.y) / 3.5f)));
					}
				});
				s.Until(MoveStep, 15.0);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.0);
				s.Do([w] { w->t0 = Now(); WalkSetup(w); g_revHome = std::make_shared<RE::NiPoint3>(Snap().position); });
				AddRevCase(s, w, 201, 1, false, true, 0.0);
				AddRevCase(s, w, 202, 1, false, false, 0.0);
				AddRevCase(s, w, 203, 1, false, true, 0.3);
				AddPoseLeaveSneak(s, r);
				s.Do([] { g_spot = Spot{}; });
			}
			return Run(s, c.now);
		}

		constexpr std::array<Stage, 6> kSet48{ {
			{ "ready", StageReady, false },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "M4b the proof: 20 reversals and 5 after a corner aim", StageReverseProof, true },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "M4b the low walk: as it was, and with mode 2", StageReverseLowProof, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 13> kSet49{ {
			{ "ready", StageReady, false },
			{ "R4 find the kiosk's low wall", StageLowSearch, true },
			{ "M4b the low pose with the weapon drawn, 3 s idle", StagePoseIn, true },
			{ "M4b the first aim press with a shot (low, over)", StagePoseAim, true },
			{ "M4b the exit after a stay, pulling away, eight times", StageShortStay, true },
			{ "M4b blind fire: a hold and a tap", StageBlindTail, true },
			{ "M4b go to the tall column", StageGoTall, true },
			{ "M4b tall cover with the weapon drawn: the automatic entry and the pose", StageTallSmoke, true },
			{ "M4b the corner at the right end", StageCornerRight, true },
			{ "M4b the user's left end at the kiosk: the spot", StageLeftSetup, true },
			{ "M4b the corner at the left end", StageCornerLeft, true },
			{ "M4b three reversals on the curve", StageReverseCurve, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 4> kSet50{ {
			{ "ready", StageReady, false },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "M4b the proof: 20 reversals and 5 after a corner aim", StageReverseProof, true },
			{ "summary", StageSummary, false },
		} };

		// the real corner aim, then the reversal (REFERENCE 23.12): at the kiosk's open left end (the straight wall's ends are not open ends to the sensor)
		bool StageReverseCornerAim(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				auto w = std::make_shared<WalkRun>();
				g_tall = true;
				g_autoTall = true;
				g_spot = g_leftSpot;
				s.Do([] {
					Rig::ReleaseEverything("m4b rev corner aim");
					Cover::ForceReset("m4b rev corner aim");
					Config::Mutable().coverPoses = true;
					Config::Mutable().poseTall = true;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					Config::Mutable().autoCoverTall = 2;
					Config::Mutable().poseCornerHold = 1;
					Config::Mutable().poseReverse = 1;
					REX::INFO("P3 M4B REV: five reversals right after a corner aim at the kiosk's left end");
				});
				AddWeaponTo(s, true);
				s.Do([] { FloorBegin(g_spot.x, g_spot.y, Snap().position.z); });
				s.Until(FloorPoll, 3.0);
				s.Do([] {
					if (s_floor.have && std::fabs(s_floor.z - Snap().position.z) < 1.5f) {
						const auto& p = Snap().position;
						MoveBegin({ g_spot.x, g_spot.y, s_floor.z }, (std::max)(1.0, static_cast<double>(std::hypot(g_spot.x - p.x, g_spot.y - p.y) / 3.5f)));
					}
				});
				s.Until(MoveStep, 15.0);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.0);
				s.Do([w] { w->t0 = Now(); WalkSetup(w); g_revHome = std::make_shared<RE::NiPoint3>(Snap().position); });
				int n = 300;
				for (int k = 0; k < 5; ++k) {
					AddWalkToEdge(s, -1, 0.90f);
					s.Do([] { REX::INFO("P3 M4B REV: a corner aim at the left end (side {:g}, the left end {:.2f} m, kind {})", VarOr("iSyncRightLeft", -1.0), Cover::GetInfo().candidate.edgeLeft, static_cast<int>(Cover::GetInfo().candidate.edgeLeftKind)); Rig::MouseRight(true); });
					s.Wait(2.0);
					s.Do([] { Rig::MouseRight(false); });
					s.Wait(2.6);
					AddRevCase(s, w, ++n, 1, true, true, (k % 2) * 0.3);  // away from the end, then back toward it
				}
				AddPoseLeaveSneak(s, r);
				s.Do([] { g_spot = Spot{}; });
			}
			return Run(s, c.now);
		}

		constexpr std::array<Stage, 14> kSet51{ {
			{ "ready", StageReady, false },
			{ "R4 find the kiosk's low wall", StageLowSearch, true },
			{ "M4b the low pose with the weapon drawn, 3 s idle", StagePoseIn, true },
			{ "M4b the first aim press with a shot (low, over)", StagePoseAim, true },
			{ "M4b the exit after a stay, pulling away, eight times", StageShortStay, true },
			{ "M4b blind fire: a hold and a tap", StageBlindTail, true },
			{ "M4b go to the tall column", StageGoTall, true },
			{ "M4b tall cover with the weapon drawn: the automatic entry and the pose", StageTallSmoke, true },
			{ "M4b the corner at the right end", StageCornerRight, true },
			{ "M4b the user's left end at the kiosk: the spot", StageLeftSetup, true },
			{ "M4b the corner at the left end", StageCornerLeft, true },
			{ "M4b three reversals on the curve", StageReverseCurve, true },
			{ "M4b five reversals after a corner aim", StageReverseCornerAim, true },
			{ "summary", StageSummary, false },
		} };

		// ---- spike 23.13: the reload sound (REFERENCE 23.13) -----------------------------------------------------------------------------------------------------------------
		// one reload: a few rounds out of the magazine (a short burst; not in the pose), the probe armed, the reload control tapped, bIsReloading and the pose watched
		struct AudioRun
		{
			double t0{ 0.0 };
			double last{ -1.0 };
			double tUp{ -1.0 };
			double tDown{ -1.0 };
			bool   upSeen{ false };
		};

		void AddAudioReload(Seq& s, const std::string& a_label, const bool a_burst, const double a_aimAfter, const double a_secs)
		{
			auto ar = std::make_shared<AudioRun>();
			if (a_burst) {
				s.Do([] { Rig::MouseLeft(true); });
				s.Wait(0.35);
				s.Do([] { Rig::MouseLeft(false); });
				s.Wait(1.2);
			}
			s.Do([ar, a_label] {
				*ar = AudioRun{};
				ar->t0 = Now();
				Engine::AudioProbe::Arm(true, a_label.c_str());
				REX::INFO("P3 M4C AUDIO {}: the reload control is tapped; pose {}, bIsReloading {:g}, stance field {}, weapon state {}", a_label, CoverPose::ToString(CoverPose::GetInfo().state), VarOr("bIsReloading", -1.0), CoverPose::StanceField(), CoverPose::WeaponState());
				Rig::Tap(Rig::kKeyR, 0.12);
			});
			auto aimed = std::make_shared<bool>(false);
			s.Until([ar, a_label, a_aimAfter, aimed, a_secs] {
				const double now = Now();
				const double v = VarOr("bIsReloading", -1.0);
				if (!ar->upSeen && v > 0.5) {
					ar->upSeen = true;
					ar->tUp = now;
					REX::INFO("P3 M4C AUDIO {}: bIsReloading rose {:.3f} s after the tap (pose {})", a_label, now - ar->t0, CoverPose::ToString(CoverPose::GetInfo().state));
				}
				if (ar->upSeen && ar->tDown < 0.0 && v < 0.5) {
					ar->tDown = now;
					REX::INFO("P3 M4C AUDIO {}: bIsReloading fell {:.3f} s after it rose (pose {})", a_label, now - ar->tUp, CoverPose::ToString(CoverPose::GetInfo().state));
				}
				if (a_aimAfter > 0.0 && !*aimed && ar->upSeen && now - ar->tUp >= a_aimAfter) {
					*aimed = true;
					REX::INFO("P3 M4C AUDIO {}: the aim control goes down {:.3f} s into the reload", a_label, now - ar->tUp);
					Rig::MouseRight(true);
				}
				return now - ar->t0 >= a_secs;
			}, a_secs + 2.0);
			if (a_aimAfter > 0.0) {
				s.Do([] { Rig::MouseRight(false); });
				s.Wait(2.6);
			}
			s.Do([ar, a_label] {
				Engine::AudioProbe::Arm(false, a_label.c_str());
				Result(std::format("P3 M4C AUDIO {}: bIsReloading rose {:.2f} s after the tap and fell {:.2f} s after that; pose {}", a_label, ar->upSeen ? ar->tUp - ar->t0 : -1.0, ar->tDown > 0.0 ? ar->tDown - ar->tUp : -1.0, CoverPose::ToString(CoverPose::GetInfo().state)));
			});
		}

		// the same reload, outside cover and inside the pose (low wall at the kiosk), then one the aim interrupts
		bool StageAudioReload(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				s.Do([] {
					Rig::ReleaseEverything("m4c audio");
					Config::Mutable().coverPoses = true;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					REX::INFO("P3 M4C AUDIO: the probe: hooks {} of 5", Engine::AudioProbe::GetInfo().sites);
				});
				AddWeaponTo(s, true);
				s.Wait(1.0);
				// outside the pose: where the search stage left the player (not in cover; the weapon drawn)
				AddAudioReload(s, "plain1", true, 0.0, 5.5);
				AddAudioReload(s, "plain2", true, 0.0, 5.5);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.0);
				AddAudioReload(s, "pose1", true, 0.0, 5.0);
				s.Wait(1.0);
				AddAudioReload(s, "pose2", true, 0.0, 5.0);
				s.Wait(1.0);
				AddAudioReload(s, "pose_aim_in_middle", true, 1.2, 5.0);
				s.Wait(1.0);
				AddAudioReload(s, "pose3_after_the_aim", true, 0.0, 5.0);
				s.Wait(1.0);
				AddAudioReload(s, "pose_leave_in_middle", true, 0.0, 2.2);
				AddPoseLeaveSneak(s, r);
				s.Wait(3.0);
				s.Do([] { const auto ai = Engine::AudioProbe::GetInfo(); REX::INFO("P3 M4C AUDIO: done, {} calls seen, learned {}, replays {}, posted {}, cancelled {}, ended {}, faulted {}", ai.calls, ai.learned, ai.replays, ai.posted, ai.cancelled, ai.ended, ai.faulted); });
			}
			return Run(s, c.now);
		}

		constexpr std::array<Stage, 4> kSet52{ {
			{ "ready", StageReady, false },
			{ "R4 find the kiosk's low wall", StageLowSearch, true },
			{ "M4c the reload sound: plain, in the pose, interrupted", StageAudioReload, true },
			{ "summary", StageSummary, false },
		} };

		void AddBurst(Seq& s)
		{
			s.Do([] { Rig::MouseLeft(true); });
			s.Wait(0.35);
			s.Do([] { Rig::MouseLeft(false); });
			s.Wait(1.2);
		}

		// the tall pose at the user's straight wall: an ordinary reload (learned), a burst, the pose, a reload in it
		bool StageAudioTall(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				g_tall = true;
				g_autoTall = true;
				g_spot = g_leftSpot;
				s.Do([] {
					Rig::ReleaseEverything("m4d audio tall");
					Cover::ForceReset("m4d audio tall");
					Config::Mutable().coverPoses = true;
					Config::Mutable().poseTall = true;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					Config::Mutable().autoCoverTall = 2;
					Config::Mutable().poseCornerHold = 1;
					REX::INFO("P3 M4D AUDIO: the tall wall; the weapon key now 0x{:08X}", Engine::AudioProbe::GetInfo().equippedKey);
				});
				AddWeaponTo(s, true);
				s.Do([] { FloorBegin(g_spot.x, g_spot.y, Snap().position.z); });
				s.Until(FloorPoll, 3.0);
				s.Do([] {
					if (s_floor.have && std::fabs(s_floor.z - Snap().position.z) < 1.5f) {
						const auto& p = Snap().position;
						MoveBegin({ g_spot.x, g_spot.y, s_floor.z }, (std::max)(1.0, static_cast<double>(std::hypot(g_spot.x - p.x, g_spot.y - p.y) / 3.5f)));
					}
				});
				s.Until(MoveStep, 15.0);
				s.Wait(1.0);
				if (Config::Get().p3Set != 54) {
					AddAudioReload(s, "tall_plain", true, 0.0, 5.5);
					if (Config::Get().p3Set == 64) {
						AddAudioReload(s, "tall_plain2", true, 0.0, 5.5);  // the smoke of 23.18: two ordinary reloads (the learn rule), then one in the pose
					}
				}
				if (Config::Get().p3Set == 57) {
					s.Do([] { Engine::AudioProbe::DebugForget(); });
				}
				AddBurst(s);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.0);
				AddAudioReload(s, "tall_pose", false, 0.0, 6.0);
				s.Wait(1.0);
				AddPoseLeaveSneak(s, r);
				s.Wait(1.0);
				s.Do([] { g_spot = Spot{}; });
			}
			return Run(s, c.now);
		}

		// the low pose: two reloads, one cut by a leave, then a weapon swap and a reload with the other weapon, and back
		bool StageAudioLow(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				g_tall = false;
				g_autoTall = false;
				g_spot = Spot{};
				s.Do([] {
					Rig::ReleaseEverything("m4d audio low");
					Cover::ForceReset("m4d audio low");
					Config::Mutable().coverPoses = true;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					REX::INFO("P3 M4D AUDIO: the low wall; the weapon key now 0x{:08X}", Engine::AudioProbe::GetInfo().equippedKey);
				});
				AddWeaponTo(s, true);
				s.Wait(1.0);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.0);
				AddAudioReload(s, "low_pose1", true, 0.0, 5.0);
				s.Wait(1.0);
				AddAudioReload(s, "low_pose2", true, 0.0, 5.0);
				s.Wait(1.0);
				AddAudioReload(s, "low_cut_by_leave", true, 0.0, 2.2);
				AddPoseLeaveSneak(s, r);
				s.Wait(2.0);
				// another weapon: number key 2, outside cover
				s.Do([] { REX::INFO("P3 M4D AUDIO: number key 2 (another weapon); the weapon key now 0x{:08X}", Engine::AudioProbe::GetInfo().equippedKey); Rig::Tap(Rig::kKey2, 0.12); });
				s.Wait(3.5);
				s.Do([] { REX::INFO("P3 M4D AUDIO: after key 2 the weapon key is 0x{:08X}", Engine::AudioProbe::GetInfo().equippedKey); });
				AddWeaponTo(s, true);
				s.Wait(1.0);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.0);
				AddAudioReload(s, "other_weapon_pose", true, 0.0, 6.0);
				AddPoseLeaveSneak(s, r);
				s.Wait(2.0);
				AddAudioReload(s, "other_weapon_plain", true, 0.0, 5.5);
				AddWeaponTo(s, true);
				s.Wait(1.0);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.0);
				AddAudioReload(s, "other_weapon_pose_again", true, 0.0, 6.0);
				AddPoseLeaveSneak(s, r);
				s.Wait(2.0);
				s.Do([] { Rig::Tap(Rig::kKey1, 0.12); });
				s.Wait(3.5);
				s.Do([] { REX::INFO("P3 M4D AUDIO: after key 1 the weapon key is 0x{:08X}", Engine::AudioProbe::GetInfo().equippedKey); });
				AddWeaponTo(s, true);
				s.Wait(1.0);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.0);
				AddAudioReload(s, "first_weapon_back_in_pose", true, 0.0, 5.0);
				AddPoseLeaveSneak(s, r);
				s.Wait(1.0);
				s.Do([] {
					const auto ai = Engine::AudioProbe::GetInfo();
					REX::INFO("P3 M4D AUDIO: done, {} calls seen, learned {}, replays {}, posted {}, cancelled {}, ended {}, faulted {}, equip events {}", ai.calls, ai.learned, ai.replays, ai.posted, ai.cancelled, ai.ended, ai.faulted, ai.equipEvents);
				});
			}
			return Run(s, c.now);
		}

		constexpr std::array<Stage, 6> kSet53{ {
			{ "ready", StageReady, false },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "M4d the reload sound at the tall wall", StageAudioTall, true },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "M4d the reload sound at the low wall and the weapon swap", StageAudioLow, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 4> kSet54{ {
			{ "ready", StageReady, false },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "M4d the first reload of the session in the tall pose (the sequence comes from the file)", StageAudioTall, true },
			{ "summary", StageSummary, false },
		} };

		// the weapon scan: where in the process data is the weapon in the hands? drawn, after number key 2, after number key 1 (not in cover)
		bool StageAudioScan(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				s.Do([] { Rig::ReleaseEverything("m4d scan"); Cover::ForceReset("m4d scan"); });
				AddWeaponTo(s, true);
				s.Wait(1.5);
				s.Do([] { Engine::AudioProbe::ScanWeapon("drawn"); });
				s.Do([] { Rig::Tap(Rig::kKey2, 0.12); });
				s.Wait(4.0);
				s.Do([] { REX::INFO("P3 M4D SCAN: after number key 2, equip key 0x{:08X}", Engine::AudioProbe::GetInfo().equippedKey); Engine::AudioProbe::ScanWeapon("after key 2"); });
				s.Do([] { Rig::Tap(Rig::kKey1, 0.12); });
				s.Wait(4.0);
				s.Do([] { REX::INFO("P3 M4D SCAN: after number key 1, equip key 0x{:08X}", Engine::AudioProbe::GetInfo().equippedKey); Engine::AudioProbe::ScanWeapon("after key 1"); });
				s.Do([] { Rig::Tap(Rig::kKey3, 0.12); });
				s.Wait(4.0);
				s.Do([] { REX::INFO("P3 M4D SCAN: after number key 3, equip key 0x{:08X}", Engine::AudioProbe::GetInfo().equippedKey); Engine::AudioProbe::ScanWeapon("after key 3"); });
			}
			return Run(s, c.now);
		}

		constexpr std::array<Stage, 15> kSet56{ {
			{ "ready", StageReady, false },
			{ "R4 find the kiosk's low wall", StageLowSearch, true },
			{ "M4b the low pose with the weapon drawn, 3 s idle", StagePoseIn, true },
			{ "M4b the first aim press with a shot (low, over)", StagePoseAim, true },
			{ "M4b the exit after a stay, pulling away, eight times", StageShortStay, true },
			{ "M4b blind fire: a hold and a tap", StageBlindTail, true },
			{ "M4b go to the tall column", StageGoTall, true },
			{ "M4b tall cover with the weapon drawn: the automatic entry and the pose", StageTallSmoke, true },
			{ "M4b the corner at the right end", StageCornerRight, true },
			{ "M4b the user's left end at the kiosk: the spot", StageLeftSetup, true },
			{ "M4b the corner at the left end", StageCornerLeft, true },
			{ "M4b three reversals on the curve", StageReverseCurve, true },
			{ "M4b five reversals after a corner aim", StageReverseCornerAim, true },
			{ "M4d the weapon scan", StageAudioScan, true },
			{ "summary", StageSummary, false },
		} };

		// ---- 23.16: the feet in the mirrored walk (the game's foot IK) and the automatic turn that works against the player --------------------------------------------------------------
		void AddSlideAlong(Seq& s, const std::shared_ptr<RE::NiPoint3>& a_home, const float a_k)
		{
			s.Do([a_home, a_k] {
				const auto& cd = Cover::GetInfo().candidate;
				const float tl = std::hypot(cd.tangent.x, cd.tangent.y);
				if (tl < 0.1f) {
					return;
				}
				MoveBegin({ a_home->x + cd.tangent.x / tl * a_k, a_home->y + cd.tangent.y / tl * a_k, a_home->z }, 0.8);
			});
			s.Until(MoveStep, 3.0);
			s.Wait(0.6);
		}

		void AddFootLeg(Seq& s, const std::shared_ptr<WalkRun>& w, const std::shared_ptr<RE::NiPoint3>& a_home, const bool a_right, const std::string& a_phase)
		{
			AddSlideAlong(s, a_home, a_right ? -1.6f : 1.6f);
			s.Do([a_phase] {
				const auto fi = Engine::FootIK::GetInfo();
				REX::INFO("P3 M4E FOOT leg {}: the foot IK settings: humanoid {} feet {} (changed {}, changes {}); pose {} side {:g}", a_phase, fi.humanoid, fi.feet, fi.changed, fi.changes, CoverPose::ToString(CoverPose::GetInfo().state), VarOr("iSyncRightLeft", -1.0));
			});
			AddWalkLegKeys(s, w, a_right ? PoseRightKey() : PoseLeftKey(), 4.5, a_phase);
		}

		// the proof stages (set 58): right and left as they are, with the two switches off, then on again, left once more
		bool StageFootProof(const Ctx& c, const bool a_tall)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				auto w = std::make_shared<WalkRun>();
				auto home = std::make_shared<RE::NiPoint3>();
				g_tall = a_tall;
				g_autoTall = a_tall;
				g_spot = a_tall ? g_leftSpot : Spot{};
				s.Do([a_tall] {
					Rig::ReleaseEverything("m4e foot proof");
					Cover::ForceReset("m4e foot proof");
					Config::Mutable().coverPoses = true;
					Config::Mutable().poseTall = a_tall;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					Config::Mutable().autoCoverTall = a_tall ? 2 : 0;
					Config::Mutable().poseCornerHold = 1;
					Config::Mutable().coverMaxInput = 0.5f;
					Config::Mutable().poseReverse = 2;  // the reversal rule for both heights: every leg starts with the right turn
					REX::INFO("P3 M4E FOOT: the {} wall; the foot IK switches found: {}", a_tall ? "tall straight" : "low", Engine::FootIK::Probe());
				});
				AddWeaponTo(s, true);
				if (a_tall) {
					s.Do([] { FloorBegin(g_spot.x, g_spot.y, Snap().position.z); });
					s.Until(FloorPoll, 3.0);
					s.Do([] {
						if (s_floor.have && std::fabs(s_floor.z - Snap().position.z) < 1.5f) {
							const auto& p = Snap().position;
							MoveBegin({ g_spot.x, g_spot.y, s_floor.z }, (std::max)(1.0, static_cast<double>(std::hypot(g_spot.x - p.x, g_spot.y - p.y) / 3.5f)));
						}
					});
					s.Until(MoveStep, 15.0);
				}
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.0);
				s.Do([w, home] { w->t0 = Now(); WalkSetup(w); *home = Snap().position; });
				const char* wall = a_tall ? "T" : "L";
				auto leg = [&](const bool a_right, const std::string& a_phase) {
					s.Do([a_phase] {
						const auto fi = Engine::FootIK::GetInfo();
						REX::INFO("P3 M4E FOOT leg {}: the foot IK settings: humanoid {} feet {} (changed {}, changes {}); pose {} side {:g}", a_phase, fi.humanoid, fi.feet, fi.changed, fi.changes, CoverPose::ToString(CoverPose::GetInfo().state), VarOr("iSyncRightLeft", -1.0));
					});
					AddWalkLegKeys(s, w, a_right ? PoseRightKey() : PoseLeftKey(), 4.5, a_phase);
				};
				if (a_tall) {
					leg(false, std::format("{}z_setup_A", wall));  // to the left end first; the legs then run end to end, no slides (a slide ends the pose: airborne)
				}
				leg(true, std::format("{}a_D", wall));
				leg(false, std::format("{}a_A", wall));
				s.Do([] { (void)Engine::FootIK::Set(false, "the rig: both switches off"); });
				s.Wait(0.5);
				leg(true, std::format("{}b_D_off", wall));
				leg(false, std::format("{}b_A_off", wall));
				s.Do([] { (void)Engine::FootIK::Set(true, "the rig: both switches on again"); });
				s.Wait(0.5);
				leg(true, std::format("{}c_D_on", wall));
				leg(false, std::format("{}c_A_on", wall));
				if (!a_tall) {
					// which of the two matters (the low wall): bHumanoidFootIKEnable alone off, then bEnableFootIK alone off
					s.Do([] { (void)Engine::FootIK::SetEach(false, true, "the rig: humanoid off, feet on"); });
					s.Wait(0.5);
					leg(true, std::format("{}d_D_hum_off", wall));
					leg(false, std::format("{}d_A_hum_off", wall));
					s.Do([] { (void)Engine::FootIK::SetEach(true, false, "the rig: humanoid on, feet off"); });
					s.Wait(0.5);
					leg(true, std::format("{}e_D_feet_off", wall));
					leg(false, std::format("{}e_A_feet_off", wall));
				}
				s.Do([] { Engine::FootIK::Restore("the rig: end of the stage"); });
				AddPoseLeaveSneak(s, r);
				s.Do([] { g_spot = Spot{}; });
			}
			return Run(s, c.now);
		}
		bool StageFootProofTall(const Ctx& c) { return StageFootProof(c, true); }
		bool StageFootProofLow(const Ctx& c) { return StageFootProof(c, false); }

		constexpr std::array<Stage, 6> kSet58{ {
			{ "ready", StageReady, false },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "M4e the foot IK proof at the tall wall", StageFootProofTall, true },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "M4e the foot IK proof at the low wall", StageFootProofLow, true },
			{ "summary", StageSummary, false },
		} };

		// the measuring stages (set 59): reversals without a stop, the iPoseFootIK keys as the INI has them; the moments of switching are in the log's FOOTIK lines
		bool StageFootSwitch(const Ctx& c, const bool a_tall)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				auto w = std::make_shared<WalkRun>();
				g_tall = a_tall;
				g_autoTall = a_tall;
				g_spot = a_tall ? g_leftSpot : Spot{};
				s.Do([a_tall] {
					Rig::ReleaseEverything("m4e foot switch");
					Cover::ForceReset("m4e foot switch");
					Config::Mutable().coverPoses = true;
					Config::Mutable().poseTall = a_tall;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					Config::Mutable().autoCoverTall = a_tall ? 2 : 0;
					Config::Mutable().poseCornerHold = 1;
					Config::Mutable().coverMaxInput = 0.5f;
					REX::INFO("P3 M4E SWITCH: the {} wall; iPoseFootIK {} / low {}; the switches found: {}", a_tall ? "tall straight" : "low", Config::Get().poseFootIK, Config::Get().poseFootIKLow, Engine::FootIK::Probe());
				});
				AddWeaponTo(s, true);
				if (a_tall) {
					s.Do([] { FloorBegin(g_spot.x, g_spot.y, Snap().position.z); });
					s.Until(FloorPoll, 3.0);
					s.Do([] {
						if (s_floor.have && std::fabs(s_floor.z - Snap().position.z) < 1.5f) {
							const auto& p = Snap().position;
							MoveBegin({ g_spot.x, g_spot.y, s_floor.z }, (std::max)(1.0, static_cast<double>(std::hypot(g_spot.x - p.x, g_spot.y - p.y) / 3.5f)));
						}
					});
					s.Until(MoveStep, 15.0);
				}
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.0);
				s.Do([w] { w->t0 = Now(); WalkSetup(w); });
				const char* wall = a_tall ? "T" : "L";
				// right, left, right, left with no stop between them (the reversal rule on): the pose's side changes under the walk each time; three passes: iPoseFootIK 0 (as today), 1, 2
				for (int mode = 0; mode < 3; ++mode) {
					s.Do([a_tall, mode] {
						(a_tall ? Config::Mutable().poseFootIK : Config::Mutable().poseFootIKLow) = mode;
						Config::Mutable().poseReverse = 2;
						REX::INFO("P3 M4E SWITCH: pass iPoseFootIK{} = {}", a_tall ? "" : "Low", mode);
					});
					for (int k = 0; k < 2; ++k) {
						AddKeyLeg(s, w, PoseRightKey(), 3.2, std::format("{}m{}s{}_D", wall, mode, k));
						AddKeyLeg(s, w, PoseLeftKey(), 3.2, std::format("{}m{}s{}_A", wall, mode, k));
					}
					s.Do([] { Rig::ReleaseEverything("m4e foot switch pass end"); });
					s.Wait(1.0);
				}
				s.Do([a_tall] { (a_tall ? Config::Mutable().poseFootIK : Config::Mutable().poseFootIKLow) = 0; });
				s.Do([] { Rig::ReleaseEverything("m4e foot switch end"); });
				s.Wait(1.0);
				s.Do([] { const auto fi = Engine::FootIK::GetInfo(); REX::INFO("P3 M4E SWITCH: end of the stage: the settings humanoid {} feet {} (changed {}, changes {}, original {}/{})", fi.humanoid, fi.feet, fi.changed, fi.changes, fi.origHumanoid, fi.origFeet); });
				AddPoseLeaveSneak(s, r);
				s.Wait(1.0);
				s.Do([] {
					const auto fi = Engine::FootIK::GetInfo();
					const bool ok = !fi.changed;
					Result(std::format("P3 M4E SWITCH: after the pose the settings are humanoid {} feet {} (original {} / {}), {} change(s) in all -> {}", fi.humanoid, fi.feet, fi.origHumanoid, fi.origFeet, fi.changes, Verdict(ok)));
					Summarise("M4E the foot IK is back on its original values after the pose", ok);
					g_spot = Spot{};
				});
			}
			return Run(s, c.now);
		}
		bool StageFootSwitchTall(const Ctx& c) { return StageFootSwitch(c, true); }
		bool StageFootSwitchLow(const Ctx& c) { return StageFootSwitch(c, false); }

		// the user's sequence at the open right end (05:03:15 to 05:03:20 of their log): at the end on the right side, a short push to the left turns the character to the left; it must stay there
		void AddTurnCase(Seq& s, const std::shared_ptr<WalkRun>& w, const std::shared_ptr<int>& a_autoCount, const bool a_lock, const std::string& a_tag)
		{
			s.Do([a_lock, a_tag] { Config::Mutable().turnLock = a_lock; REX::INFO("P3 M4E TURN {}: bTurnLock {}", a_tag, a_lock); });
			AddWalkLegKeys(s, w, PoseRightKey(), 4.5, a_tag + "_to_the_end");
			s.Wait(2.0);
			auto t0 = std::make_shared<double>(0.0);
			s.Do([a_tag, t0] {
				*t0 = Now();
				REX::INFO("P3 M4E TURN {}: at the right end: side {:g} (0 right), the right end {:.2f} m; the push to the left (0.4 s) begins", a_tag, VarOr("iSyncRightLeft", -1.0), Cover::GetInfo().candidate.edgeRight);
				Rig::Tap(PoseLeftKey(), 0.4);
			});
			s.Until([t0] { return Now() - *t0 >= 5.0; }, 6.0);
			s.Do([a_tag, a_lock] {
				const bool left = VarOr("iSyncRightLeft", -1.0) > 0.5;
				Result(std::format("P3 M4E TURN {}: 5 s after a 0.4 s push to the left at the right end: bTurnLock {}, the side is {} (it should be Left), cover {} -> see the TALL AIM lines for an automatic turn", a_tag, a_lock, left ? "Left" : "Right", Core::ToString(Cover::GetInfo().state)));
			});
		}

		bool StageTurnReplay(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				auto w = std::make_shared<WalkRun>();
				auto n = std::make_shared<int>(0);
				g_tall = true;
				g_autoTall = true;
				g_spot = g_leftSpot;
				s.Do([] {
					Rig::ReleaseEverything("m4e turn replay");
					Cover::ForceReset("m4e turn replay");
					Config::Mutable().coverPoses = true;
					Config::Mutable().poseTall = true;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					Config::Mutable().autoCoverTall = 2;
					Config::Mutable().poseCornerHold = 1;
					Config::Mutable().tallAutoTurn = true;
					Config::Mutable().coverMaxInput = 0.5f;
					REX::INFO("P3 M4E TURN: the user's sequence at the right end of the straight tall wall");
				});
				AddWeaponTo(s, true);
				s.Do([] { FloorBegin(g_spot.x, g_spot.y, Snap().position.z); });
				s.Until(FloorPoll, 3.0);
				s.Do([] {
					if (s_floor.have && std::fabs(s_floor.z - Snap().position.z) < 1.5f) {
						const auto& p = Snap().position;
						MoveBegin({ g_spot.x, g_spot.y, s_floor.z }, (std::max)(1.0, static_cast<double>(std::hypot(g_spot.x - p.x, g_spot.y - p.y) / 3.5f)));
					}
				});
				s.Until(MoveStep, 15.0);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.0);
				s.Do([w] { w->t0 = Now(); WalkSetup(w); });
				AddTurnCase(s, w, n, false, "lock_off");
				AddTurnCase(s, w, n, true, "lock_on");
				s.Do([] { Config::Mutable().turnLock = true; });
				AddPoseLeaveSneak(s, r);
				s.Do([] { g_spot = Spot{}; });
			}
			return Run(s, c.now);
		}

		constexpr std::array<Stage, 7> kSet59{ {
			{ "ready", StageReady, false },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "M4e the switching at the tall wall", StageFootSwitchTall, true },
			{ "M4e the user's sequence at the right end", StageTurnReplay, true },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "M4e the switching at the low wall", StageFootSwitchLow, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 9> kSet60{ {
			{ "ready", StageReady, false },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "M4e the foot IK proof at the tall wall", StageFootProofTall, true },
			{ "M4e the switching at the tall wall", StageFootSwitchTall, true },
			{ "M4e the user's sequence at the right end", StageTurnReplay, true },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "M4e the foot IK proof at the low wall", StageFootProofLow, true },
			{ "M4e the switching at the low wall", StageFootSwitchLow, true },
			{ "summary", StageSummary, false },
		} };

		// the smoke of the play build with the foot IK cure: the kiosk smoke of set 51 and then the tall walk on the kiosk's curve (the legs of set 46, the toes logged)
		constexpr std::array<Stage, 15> kSet61{ {
			{ "ready", StageReady, false },
			{ "R4 find the kiosk's low wall", StageLowSearch, true },
			{ "M4b the low pose with the weapon drawn, 3 s idle", StagePoseIn, true },
			{ "M4b the first aim press with a shot (low, over)", StagePoseAim, true },
			{ "M4b the exit after a stay, pulling away, eight times", StageShortStay, true },
			{ "M4b blind fire: a hold and a tap", StageBlindTail, true },
			{ "M4b go to the tall column", StageGoTall, true },
			{ "M4b tall cover with the weapon drawn: the automatic entry and the pose", StageTallSmoke, true },
			{ "M4b the corner at the right end", StageCornerRight, true },
			{ "M4b the user's left end at the kiosk: the spot", StageLeftSetup, true },
			{ "M4b the corner at the left end", StageCornerLeft, true },
			{ "M4b three reversals on the curve", StageReverseCurve, true },
			{ "M4b five reversals after a corner aim", StageReverseCornerAim, true },
			{ "M4b the tall walk on the kiosk's curve: the feet", StageWalkCurve, true },
			{ "summary", StageSummary, false },
		} };

		// ---- spike W1 (REFERENCE 23.18): a weapon switch inside the pose, facts only --------------------------------------------------------------------------------------------------
		std::vector<std::pair<std::string, const void*>> g_w1Nodes;

		[[nodiscard]] std::string W1NodeFact(const std::pair<std::string, const void*>& a_n)
		{
			std::uint64_t fl{ 0 };
			(void)SafeRead(reinterpret_cast<const char*>(a_n.second) + 0x118, fl);
			const auto kids = ListChildren(a_n.second);
			return std::format("{}:c{}{}", a_n.first, kids.size(), (fl & 1) != 0 ? ":culled" : "");
		}

		// once: the nodes whose names look like the weapon's place, with their flags, children and the children's names
		void W1Dump(const char* a_label)
		{
			g_w1Nodes.clear();
			const void* root = SkeletonRoot();
			if (!root) {
				REX::INFO("P3 W1 NODES [{}]: no skeleton root", a_label);
				return;
			}
			const auto all = WalkSkeleton(root, 1200);
			REX::INFO("P3 W1 NODES [{}]: {} nodes under the skeleton root; the ones named like a weapon place:", a_label, all.size());
			for (const auto& [d, k] : all) {
				std::string low = k.name;
				for (auto& ch : low) {
					ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
				}
				if (low.find("weapon") == std::string::npos && low.find("holster") == std::string::npos && low.find("scabbard") == std::string::npos && low != "gun" && low.find("magazine") == std::string::npos) {
					continue;
				}
				std::uint64_t fl{ 0 };
				(void)SafeRead(reinterpret_cast<const char*>(k.ptr) + 0x118, fl);
				const auto  kids = ListChildren(k.ptr);
				std::string names;
				for (std::size_t i = 0; i < kids.size() && i < 6; ++i) {
					names += (i ? ", " : "") + kids[i].name;
				}
				REX::INFO("P3 W1 NODE [{}] depth {} '{}' at {} flags 0x{:X} ({}) children {}: {}", a_label, d, k.name, k.ptr, fl, (fl & 1) != 0 ? "culled" : "not culled", kids.size(), names);
				if (g_w1Nodes.size() < 8) {
					g_w1Nodes.emplace_back(k.name, k.ptr);
				}
			}
		}

		void W1Vars()
		{
			static bool done = false;
			if (done) {
				return;
			}
			done = true;
			std::string found;
			for (const char* n : { "bIsEquipping", "bIsUnequipping", "bIsSheathing", "bIsDrawing", "iSyncEquipState", "iSyncUnequipState", "bUnequip", "bEquip", "iSyncWeaponDrawState", "bIsWeaponDrawn", "iWeaponDrawState", "bIsInCover", "bIsReloading", "iSyncGunDown", "iIsGunDown" }) {
				found += std::format(" {}={}", n, PlayerControl::ReadVar(n).has_value() ? "yes" : "no");
			}
			REX::INFO("P3 W1 VARS: which graph variables resolve:{}", found);
		}

		void W1TraceLine(const std::string& a_label, const double a_t)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			std::uint32_t s1{ 0 }, s2{ 0 };
			if (player) {
				unsigned long code{ 0 };
				(void)Guarded([&] { s1 = static_cast<std::uint32_t>(player->actorState1); s2 = static_cast<std::uint32_t>(player->actorState2); }, code);
			}
			std::string nodes;
			for (const auto& n : g_w1Nodes) {
				nodes += " " + W1NodeFact(n);
			}
			REX::INFO("P3 W1 {:+.3f} [{}] key=0x{:08X} aState=0x{:08X}/0x{:08X} ws={} wds={:g} eq={:g} uneq={:g} sheath={:g} cov={:g} stance={} pose={} pend={} rel={:g} gundown={:g} |{}", a_t, a_label, Engine::AudioProbe::CurrentWeapon(), s1, s2, CoverPose::WeaponState(),
				VarOr("iSyncWeaponDrawState", -9.0), VarOr("bIsEquipping", -9.0), VarOr("bIsUnequipping", -9.0), VarOr("bIsSheathing", -9.0), VarOr("bIsInCover", -9.0), CoverPose::StanceField(), CoverPose::ToString(CoverPose::GetInfo().state), CoverPose::PendingByte(), VarOr("bIsReloading", -9.0),
				VarOr("iSyncGunDown", -9.0), nodes);
		}

		// the trace of one case: every 20 ms from a_pre seconds before the key to a_post seconds after it (the key is pressed when a_pre has passed; none when a_key is null)
		void AddW1Trace(Seq& s, const std::string& a_label, const double a_pre, const double a_post, const Rig::Key* a_key)
		{
			auto t0 = std::make_shared<double>(0.0);
			auto next = std::make_shared<double>(0.0);
			auto st = std::make_shared<std::array<bool, 3>>();  // pressed, picture before, picture after
			const bool hasKey = a_key != nullptr;
			const Rig::Key key = a_key ? *a_key : Rig::Key{};
			s.Do([t0, next, st, a_label, hasKey] {
				*t0 = Now();
				*next = 0.0;
				*st = {};
				W1Vars();
				REX::INFO("P3 W1 CASE [{}] begins: the weapon key 0x{:08X}, pose {}, {}", a_label, Engine::AudioProbe::CurrentWeapon(), CoverPose::ToString(CoverPose::GetInfo().state), hasKey ? "a key press follows" : "no key");
			});
			s.Until([t0, next, st, a_label, a_pre, a_post, hasKey, key] {
				const double now = Now();
				if (now >= *next) {
					*next = now + 0.02;
					W1TraceLine(a_label, now - *t0 - a_pre);
				}
				if (!(*st)[1] && now - *t0 >= 0.05) {
					(*st)[1] = true;
					REX::INFO("P3 W1 PICTURE [{}] before the key (t {:.3f} s)", a_label, now - *t0 - a_pre);
				}
				if (hasKey && !(*st)[0] && now - *t0 >= a_pre) {
					(*st)[0] = true;
					REX::INFO("P3 W1 KEY [{}]: the quick-key is tapped (weapon key 0x{:08X})", a_label, Engine::AudioProbe::CurrentWeapon());
					Rig::Tap(key, 0.12);
				}
				if (hasKey && now - *t0 >= a_pre + 0.3 && now - *t0 < a_pre + 0.34) {
					static double lastMark = -10.0;
					if (now - lastMark > 1.0) {
						lastMark = now;
						REX::INFO("P3 W1 PICTURE [{}] 0.3 s after the key (weapon key 0x{:08X})", a_label, Engine::AudioProbe::CurrentWeapon());
					}
				}
				if (hasKey && now - *t0 >= a_pre + 1.0 && now - *t0 < a_pre + 1.04) {
					static double lastMark1 = -10.0;
					if (now - lastMark1 > 1.5) {
						lastMark1 = now;
						REX::INFO("P3 W1 PICTURE [{}] 1.0 s after the key (weapon key 0x{:08X})", a_label, Engine::AudioProbe::CurrentWeapon());
					}
				}
				if (!(*st)[2] && now - *t0 >= a_pre + 2.0) {
					(*st)[2] = true;
					REX::INFO("P3 W1 PICTURE [{}] 2 s after the key (weapon key 0x{:08X})", a_label, Engine::AudioProbe::CurrentWeapon());
				}
				return now - *t0 >= a_pre + a_post;
			}, a_pre + a_post + 2.0);
			s.Do([a_label] {
				const auto pi = CoverPose::GetInfo();
				Result(std::format("P3 W1 CASE [{}] ends: weapon key 0x{:08X}, pose {}, cover {}, weapon state {}, stance field {}", a_label, Engine::AudioProbe::CurrentWeapon(), CoverPose::ToString(pi.state), Core::ToString(Cover::GetInfo().state), CoverPose::WeaponState(), CoverPose::StanceField()));
			});
		}

		// the checks with the pose still on: walk 1 s each way, the aim for 1 s, one blind-fire tap, one reload (each only while the pose stands)
		void AddW1Checks(Seq& s, const std::string& a_tag, const bool a_tall)
		{
			auto step = [&s, &a_tag](const char* a_what, std::function<void()> a_do, const double a_secs) {
				s.Do([a_tag, a_what, a_do] {
					const auto pi = CoverPose::GetInfo();
					REX::INFO("P3 W1 CHECK [{}] {}: pose {}, weapon key 0x{:08X}", a_tag, a_what, CoverPose::ToString(pi.state), Engine::AudioProbe::CurrentWeapon());
					if (pi.state == CoverPose::State::Posed) {
						a_do();
					}
				});
				auto t0 = std::make_shared<double>(0.0);
				s.Do([t0] { *t0 = Now(); });
				s.Until([t0, a_secs] { return Now() - *t0 >= a_secs; }, a_secs + 1.0);
			};
			step("walk right 1 s", [] { Rig::Tap(PoseRightKey(), 1.0); }, 1.8);
			step("walk left 1 s", [] { Rig::Tap(PoseLeftKey(), 1.0); }, 1.8);
			if (a_tall) {
				step("walk right 3 s to the open end", [] { Rig::Tap(PoseRightKey(), 3.0); }, 4.2);
			}
			step("one blind-fire tap", [] { Rig::MouseLeft(true); }, 0.15);
			s.Do([] { Rig::MouseLeft(false); });
			s.Wait(2.0);
			step("one reload", [] { Rig::Tap(Rig::kKeyR, 0.12); }, 6.0);
			s.Do([a_tag] {
				const auto pi = CoverPose::GetInfo();
				Result(std::format("P3 W1 CHECKS [{}] end: pose {}, weapon key 0x{:08X}, weapon state {}", a_tag, CoverPose::ToString(pi.state), Engine::AudioProbe::CurrentWeapon(), CoverPose::WeaponState()));
			});
		}

		void AddW1Aim(Seq& s, const std::string& a_tag)
		{
			s.Do([a_tag] {
				const auto pi = CoverPose::GetInfo();
				REX::INFO("P3 W1 CHECK [{}] the aim 1 s: pose {}, weapon key 0x{:08X}", a_tag, CoverPose::ToString(pi.state), Engine::AudioProbe::CurrentWeapon());
				REX::INFO("P3 W1 CAMERA [{}] before the aim: camera state index {}", a_tag, Frame::Latest().cameraIndex);
				if (pi.state == CoverPose::State::Posed) {
					Rig::MouseRight(true);
				}
			});
			s.Wait(0.5);
			s.Do([a_tag] { REX::INFO("P3 W1 CAMERA [{}] 0.5 s into the aim: camera state index {}, pose {}", a_tag, Frame::Latest().cameraIndex, CoverPose::ToString(CoverPose::GetInfo().state)); });
			s.Wait(0.5);
			s.Do([a_tag] { REX::INFO("P3 W1 CAMERA [{}] 1 s into the aim: camera state index {}, pose {}", a_tag, Frame::Latest().cameraIndex, CoverPose::ToString(CoverPose::GetInfo().state)); Rig::MouseRight(false); });
			s.Wait(2.6);
			s.Do([a_tag] { REX::INFO("P3 W1 CAMERA [{}] 2.6 s after the aim: camera state index {}", a_tag, Frame::Latest().cameraIndex); });
			s.Do([a_tag] {
				const auto pi = CoverPose::GetInfo();
				Result(std::format("P3 W1 CHECK [{}] after the aim: pose {}, cover {}, weapon key 0x{:08X}, weapon state {}, stance field {}", a_tag, CoverPose::ToString(pi.state), Core::ToString(Cover::GetInfo().state), Engine::AudioProbe::CurrentWeapon(), CoverPose::WeaponState(), CoverPose::StanceField()));
			});
		}

		bool StageW1(const Ctx& c, const bool a_tall)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				g_tall = a_tall;
				g_autoTall = a_tall;
				g_spot = a_tall ? g_leftSpot : Spot{};
				s.Do([a_tall] {
					Rig::ReleaseEverything("w1");
					Cover::ForceReset("w1");
					Config::Mutable().coverPoses = true;
					Config::Mutable().poseTall = a_tall;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					Config::Mutable().autoCoverTall = a_tall ? 2 : 0;
					Config::Mutable().poseCornerHold = 1;
					Config::Mutable().poseWeaponSwap = 2;  // the spike's mode: nothing is locked, the plugin does nothing because of the weapon controls
					REX::INFO("P3 W1: the {} pose; iPoseWeaponSwap {}; the weapon key at the start 0x{:08X}", a_tall ? "tall" : "low", Config::Get().poseWeaponSwap, Engine::AudioProbe::CurrentWeapon());
					Engine::AudioProbe::Arm(true, "W1");
				});
				AddWeaponTo(s, true);
				if (a_tall) {
					s.Do([] { FloorBegin(g_spot.x, g_spot.y, Snap().position.z); });
					s.Until(FloorPoll, 3.0);
					s.Do([] {
						if (s_floor.have && std::fabs(s_floor.z - Snap().position.z) < 1.5f) {
							const auto& p = Snap().position;
							MoveBegin({ g_spot.x, g_spot.y, s_floor.z }, (std::max)(1.0, static_cast<double>(std::hypot(g_spot.x - p.x, g_spot.y - p.y) / 3.5f)));
						}
					});
					s.Until(MoveStep, 15.0);
				}
				s.Wait(1.0);
				s.Do([] { W1Dump("standing in the open, the first weapon"); });
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.0);
				const std::string tag = a_tall ? "T" : "L";
				// the first weapon is the save's own; the order of the keys: a_tall: 2 then 3; low (the second round, set 63): 3 then 2 (it starts with the weapon the tall round ended on)
				const bool lowSecond = !a_tall && Config::Get().p3Set == 63;
				const Rig::Key* k1 = lowSecond ? &Rig::kKey3 : &Rig::kKey2;
				const Rig::Key* k2 = lowSecond ? &Rig::kKey2 : &Rig::kKey3;
				AddW1Trace(s, tag + "_pose_switch1", 0.5, 4.0, k1);
				AddW1Checks(s, tag + "_after_switch1", a_tall);
				AddW1Trace(s, tag + "_pose_switch2", 0.5, 4.0, k2);
				AddW1Checks(s, tag + "_after_switch2", a_tall);
				AddW1Aim(s, tag + "_after_switch2");
				// out by pulling away is the sneak toggle here (the exit the rig has everywhere); then two seconds and the cover again
				AddPoseLeaveSneak(s, r);
				s.Wait(2.0);
				s.Do([] { REX::INFO("P3 W1: out of the pose: the weapon key 0x{:08X}, weapon state {}, cover {}; the cover is taken again", Engine::AudioProbe::CurrentWeapon(), CoverPose::WeaponState(), Core::ToString(Cover::GetInfo().state)); });
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.5);
				s.Do([] { REX::INFO("P3 W1: the way back in: pose {}, cover {}, the weapon key 0x{:08X}, weapon state {}, stance field {}", CoverPose::ToString(CoverPose::GetInfo().state), Core::ToString(Cover::GetInfo().state), Engine::AudioProbe::CurrentWeapon(), CoverPose::WeaponState(), CoverPose::StanceField()); });
				AddPoseLeaveSneak(s, r);
				s.Wait(2.0);
				// the reference in the open: the same keys standing (the nodes as the yardstick of "shown")
				s.Do([] { W1Dump("the open reference, before"); });
				AddW1Trace(s, tag + "_open_key2", 0.5, 4.0, &Rig::kKey2);
				AddW1Trace(s, tag + "_open_key3", 0.5, 4.0, &Rig::kKey3);
				s.Do([] { Rig::Tap(Rig::kKey2, 0.12); });  // ends on the second weapon again (it has a cover clip set), for the next round
				s.Wait(3.0);
				s.Do([] { Engine::AudioProbe::Arm(false, "W1"); Config::Mutable().poseWeaponSwap = 0; REX::INFO("P3 W1: done"); });
			}
			return Run(s, c.now);
		}
		bool StageW1Low(const Ctx& c) { return StageW1(c, false); }
		bool StageW1Tall(const Ctx& c) { return StageW1(c, true); }

		constexpr std::array<Stage, 4> kSet62{ {
			{ "ready", StageReady, false },
			{ "R4 find the low wall of Save30", StageLowSearch, true },
			{ "W1 the weapon switch in the low pose", StageW1Low, true },
			{ "summary", StageSummary, false },
		} };
		constexpr std::array<Stage, 6> kSet63{ {
			{ "ready", StageReady, false },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "W1 the weapon switch in the tall pose", StageW1Tall, true },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "W1 the weapon switch in the low pose (second round)", StageW1Low, true },
			{ "summary", StageSummary, false },
		} };
		constexpr std::array<Stage, 4> kSet64{ {
			{ "ready", StageReady, false },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "M4f the learn rule: two plain reloads, one in the tall pose", StageAudioTall, true },
			{ "summary", StageSummary, false },
		} };

		// ---- spike W2 (REFERENCE 23.20): after an equip inside the pose the plugin calls the game's weapon-draw work ------------------------------------------------------------------------
		void AddOpenAim(Seq& s, const std::string& a_tag)
		{
			s.Do([a_tag] { REX::INFO("P3 W2 OPEN AIM [{}]: before the aim: camera state index {}, weapon key 0x{:08X}", a_tag, Frame::Latest().cameraIndex, Engine::AudioProbe::CurrentWeapon()); Rig::MouseRight(true); });
			s.Wait(0.5);
			s.Do([a_tag] { REX::INFO("P3 W2 OPEN AIM [{}]: 0.5 s into the aim: camera state index {}", a_tag, Frame::Latest().cameraIndex); });
			s.Wait(0.5);
			s.Do([a_tag] { REX::INFO("P3 W2 OPEN AIM [{}]: 1 s into the aim: camera state index {}", a_tag, Frame::Latest().cameraIndex); Rig::MouseRight(false); });
			s.Wait(2.0);
			s.Do([a_tag] { REX::INFO("P3 W2 OPEN AIM [{}]: 2 s after the aim: camera state index {}", a_tag, Frame::Latest().cameraIndex); });
		}

		bool StageW2(const Ctx& c, const bool a_tall)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				g_tall = a_tall;
				g_autoTall = a_tall;
				g_spot = a_tall ? g_leftSpot : Spot{};
				s.Do([a_tall] {
					Rig::ReleaseEverything("w2");
					Cover::ForceReset("w2");
					Config::Mutable().coverPoses = true;
					Config::Mutable().poseTall = a_tall;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					Config::Mutable().autoCoverTall = a_tall ? 2 : 0;
					Config::Mutable().poseCornerHold = 1;
					Config::Mutable().poseWeaponSwap = 2;
					Config::Mutable().poseWeaponDraw = 1;
					REX::INFO("P3 W2: the {} pose; iPoseWeaponSwap {}, iPoseWeaponDraw {}, delay {:.2f} s; the weapon key at the start 0x{:08X}", a_tall ? "tall" : "low", Config::Get().poseWeaponSwap, Config::Get().poseWeaponDraw, Config::Get().poseWeaponDrawDelay, Engine::AudioProbe::CurrentWeapon());
					Engine::AudioProbe::Arm(true, "W2");
				});
				AddWeaponTo(s, true);
				if (a_tall) {
					s.Do([] { FloorBegin(g_spot.x, g_spot.y, Snap().position.z); });
					s.Until(FloorPoll, 3.0);
					s.Do([] {
						if (s_floor.have && std::fabs(s_floor.z - Snap().position.z) < 1.5f) {
							const auto& p = Snap().position;
							MoveBegin({ g_spot.x, g_spot.y, s_floor.z }, (std::max)(1.0, static_cast<double>(std::hypot(g_spot.x - p.x, g_spot.y - p.y) / 3.5f)));
						}
					});
					s.Until(MoveStep, 15.0);
				}
				s.Wait(1.0);
				s.Do([] { W1Dump("W2: standing in the open, the first weapon"); });
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.5);
				const std::string tag = a_tall ? "T" : "L";
				// 1: the save's own weapon in the hands: key 2 (the trace and the call)
				AddW1Trace(s, tag + "_W2_pose_key2", 1.0, 4.0, &Rig::kKey2);
				// 2: the checks with the new weapon (walk each way, one blind-fire tap, the reload)
				AddW1Checks(s, tag + "_W2_after_key2", a_tall);
				// 3: key 3 (the cutter), then key 2 again, each with its call
				AddW1Trace(s, tag + "_W2_pose_key3", 1.0, 4.0, &Rig::kKey3);
				AddW1Checks(s, tag + "_W2_after_key3", a_tall);
				AddW1Trace(s, tag + "_W2_pose_key2_again", 1.0, 4.0, &Rig::kKey2);
				// the aim last: tall at the open end (the walk to it is in the checks), low over the wall; it may end the pose
				AddW1Aim(s, tag + "_W2_after_key2_again");
				// 4: out by the rig's leave, two seconds, the cover again
				AddPoseLeaveSneak(s, r);
				s.Wait(2.0);
				s.Do([] { REX::INFO("P3 W2: out of the pose: the weapon key 0x{:08X}, weapon state {}, cover {}; the cover is taken again", Engine::AudioProbe::CurrentWeapon(), CoverPose::WeaponState(), Core::ToString(Cover::GetInfo().state)); });
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.5);
				s.Do([] { REX::INFO("P3 W2: the way back in: pose {}, cover {}, the weapon key 0x{:08X}, weapon state {}, stance field {}", CoverPose::ToString(CoverPose::GetInfo().state), Core::ToString(Cover::GetInfo().state), Engine::AudioProbe::CurrentWeapon(), CoverPose::WeaponState(), CoverPose::StanceField()); });
				AddPoseLeaveSneak(s, r);
				s.Wait(2.0);
				// 5: one switch in the open afterwards (it must run as in the reference of W1: the equip clip, bIsEquipping, the state 3, 2, 3)
				s.Do([] { Config::Mutable().poseWeaponDraw = 0; W1Dump("W2: the open switch afterwards, before"); });
				AddW1Trace(s, tag + "_W2_open_key3", 1.0, 4.0, &Rig::kKey3);
				AddW1Trace(s, tag + "_W2_open_key2", 1.0, 4.0, &Rig::kKey2);
				// the aim once in the open: does the camera leave third person here as well?
				AddOpenAim(s, tag + "_W2");
				// 0: the number keys not tried, in the open, once: which one gives the save's own weapon back
				for (const auto& [label, key] : { std::pair<const char*, const Rig::Key*>{ "4", &Rig::kKey4 }, { "5", &Rig::kKey5 }, { "6", &Rig::kKey6 }, { "7", &Rig::kKey7 }, { "8", &Rig::kKey8 }, { "9", &Rig::kKey9 }, { "1", &Rig::kKey1 } }) {
					const Rig::Key k = *key;
					const std::string lab = label;
					s.Do([k, lab] { REX::INFO("P3 W2 KEYPROBE: number key {} is tapped (weapon key before 0x{:08X})", lab, Engine::AudioProbe::CurrentWeapon()); Rig::Tap(k, 0.12); });
					s.Wait(2.2);
					s.Do([lab] { REX::INFO("P3 W2 KEYPROBE: after number key {}: weapon key 0x{:08X}", lab, Engine::AudioProbe::CurrentWeapon()); });
				}
				s.Do([] {
					Engine::AudioProbe::Arm(false, "W2");
					const auto wi = Engine::WeaponDraw::GetInfo();
					Result(std::format("P3 W2 done: the weapon-draw work called {} time(s), gave up {} time(s), available {}, faulted {}", wi.calls, wi.giveUps, wi.available, wi.faulted));
					Config::Mutable().poseWeaponSwap = 0;
					Config::Mutable().poseWeaponDraw = 0;
				});
			}
			return Run(s, c.now);
		}
		bool StageW2Tall(const Ctx& c) { return StageW2(c, true); }
		bool StageW2Low(const Ctx& c) { return StageW2(c, false); }

		constexpr std::array<Stage, 4> kSet65{ {
			{ "ready", StageReady, false },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "W2 the weapon-draw work after an equip in the tall pose", StageW2Tall, true },
			{ "summary", StageSummary, false },
		} };
		constexpr std::array<Stage, 4> kSet66{ {
			{ "ready", StageReady, false },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "W2 the weapon-draw work after an equip in the low pose", StageW2Low, true },
			{ "summary", StageSummary, false },
		} };

		// ---- W3 (REFERENCE 23.21): the weapon switch in the pose as a play candidate -----------------------------------------------------------------------------------------------------
		void W3Names()
		{
			for (const std::uint32_t id : { 0x00028A02u, 0x002773C8u, 0x00016758u, 0x0026D96Bu }) {
				std::string   ed = "?";
				unsigned long code{ 0 };
				(void)Guarded([&] {
					if (auto* f = RE::TESForm::LookupByID(id)) {
						const char* s = f->GetFormEditorID();
						ed = (s && *s) ? s : "(no editor id at run time)";
					} else {
						ed = "(form not found)";
					}
				}, code);
				REX::INFO("P3 W3 WEAPON 0x{:08X}: editor id {}{}", id, ed, code ? " (exception in the lookup)" : "");
			}
		}

		void W3Trace(const std::string& a_label, const double a_t)
		{
			const auto& p = Snap().position;
			REX::INFO("P3 W3 {:+.3f} [{}] camera={} cover={} bIsInCover={:g} stance={} pose={} sighted={:g} syncSighted={:g} coverStates={:g} rel={:g} pos=({:.3f},{:.3f}) key=0x{:08X}", a_t, a_label, Frame::Latest().cameraIndex, Core::ToString(Cover::GetInfo().state), VarOr("bIsInCover", -9.0), CoverPose::StanceField(),
				CoverPose::ToString(CoverPose::GetInfo().state), VarOr("iIsSighted", -9.0), VarOr("iSyncSighted", -9.0), VarOr("iSyncCoverStates", -9.0), VarOr("bIsReloading", -9.0), p.x, p.y, Engine::AudioProbe::CurrentWeapon());
		}

		struct W3Act
		{
			double                t;
			std::function<void()> fn;
			bool                  done{ false };
		};

		// a_secs seconds with the W3 trace every 20 ms and the actions at their times
		void AddW3Timed(Seq& s, const std::string& a_label, const double a_secs, std::vector<W3Act> a_acts)
		{
			auto acts = std::make_shared<std::vector<W3Act>>(std::move(a_acts));
			auto t0 = std::make_shared<double>(0.0);
			auto next = std::make_shared<double>(0.0);
			s.Do([t0, next, acts, a_label] {
				*t0 = Now();
				*next = 0.0;
				for (auto& a : *acts) {
					a.done = false;
				}
				REX::INFO("P3 W3 CASE [{}] begins", a_label);
			});
			s.Until([t0, next, acts, a_label, a_secs] {
				const double now = Now();
				if (now >= *next) {
					*next = now + 0.02;
					W3Trace(a_label, now - *t0);
				}
				for (auto& a : *acts) {
					if (!a.done && now - *t0 >= a.t) {
						a.done = true;
						a.fn();
					}
				}
				return now - *t0 >= a_secs;
			}, a_secs + 2.0);
		}

		bool StageW3(const Ctx& c, const bool a_tall)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				g_tall = a_tall;
				g_autoTall = a_tall;
				g_spot = a_tall ? g_leftSpot : Spot{};
				const std::string tag = a_tall ? "T" : "L";
				s.Do([a_tall] {
					Rig::ReleaseEverything("w3");
					Cover::ForceReset("w3");
					Config::Mutable().coverPoses = true;
					Config::Mutable().poseTall = a_tall;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					Config::Mutable().autoCoverTall = a_tall ? 2 : 0;
					Config::Mutable().poseCornerHold = 1;
					Config::Mutable().poseWeaponSwap = 3;
					Config::Mutable().poseScopeAim = 1;
					Config::Mutable().poseDrawReevaluate = 0;
					REX::INFO("P3 W3: the {} pose; iPoseWeaponSwap {}, iPoseScopeAim {}, draw delay {:.2f} s; the weapon key at the start 0x{:08X}", a_tall ? "tall" : "low", Config::Get().poseWeaponSwap, Config::Get().poseScopeAim, Config::Get().poseWeaponDrawDelay, Engine::AudioProbe::CurrentWeapon());
					W3Names();
					Engine::AudioProbe::Arm(true, "W3");
				});
				AddWeaponTo(s, true);
				if (a_tall) {
					s.Do([] { FloorBegin(g_spot.x, g_spot.y, Snap().position.z); });
					s.Until(FloorPoll, 3.0);
					s.Do([] {
						if (s_floor.have && std::fabs(s_floor.z - Snap().position.z) < 1.5f) {
							const auto& p = Snap().position;
							MoveBegin({ g_spot.x, g_spot.y, s_floor.z }, (std::max)(1.0, static_cast<double>(std::hypot(g_spot.x - p.x, g_spot.y - p.y) / 3.5f)));
						}
					});
					s.Until(MoveStep, 15.0);
				}
				s.Wait(1.0);
				s.Do([] { W1Dump("W3: standing in the open, the first weapon"); });
				if (Config::Get().p3Set == 70 && !a_tall) {
					s.Do([] { REX::INFO("P3 W3: the second round: the weapon is 0x{:08X}; key 3 (the cutter, no scope) is tapped in the open first", Engine::AudioProbe::CurrentWeapon()); Rig::Tap(Rig::kKey3, 0.12); });
					s.Wait(3.5);
				}
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.5);
				if (a_tall) {
					s.Do([] { Rig::Tap(PoseRightKey(), 3.0); REX::INFO("P3 W3: the walk to the open end"); });
					s.Wait(4.2);
				}
				// A2: a switch while the aim is held, with the save's own weapon (no scope): the aim 0.8 s, key 2, the aim held 1.7 s more
				AddW3Timed(s, tag + "_A2_switch_while_aiming", 5.5, {
					{ 0.2, [] { REX::INFO("P3 W3 PICTURE [A2] before the aim"); Rig::MouseRight(true); } },
					{ 1.0, [] { REX::INFO("P3 W3 PICTURE [A2] 0.8 s into the aim, the key 2 is tapped (weapon key 0x{:08X})", Engine::AudioProbe::CurrentWeapon()); Rig::Tap(Rig::kKey2, 0.12); } },
					{ 2.0, [] { REX::INFO("P3 W3 PICTURE [A2] 1 s after the key, the aim still held"); } },
					{ 2.7, [] { Rig::MouseRight(false); REX::INFO("P3 W3: the aim is released"); } },
					{ 4.5, [] { REX::INFO("P3 W3 PICTURE [A2] 1.8 s after the release (weapon key 0x{:08X})", Engine::AudioProbe::CurrentWeapon()); } },
				});
				s.Do([tag] { REX::INFO("P3 W3 [{}_A2] after: pose {}, cover {}, weapon key 0x{:08X}", tag, CoverPose::ToString(CoverPose::GetInfo().state), Core::ToString(Cover::GetInfo().state), Engine::AudioProbe::CurrentWeapon()); });
				// B: the scoped weapon's aim (the weapon is 0x002773C8 after A2's switch): the aim 2 s, a shot at 1.6 s, pictures at 0.6 s and 1.5 s and 1.0 s after the release
				AddW3Timed(s, tag + "_B_scope_aim", 6.0, {
					{ 0.5, [] { REX::INFO("P3 W3 PICTURE [B] before the aim (the HUD's count before the shot), weapon key 0x{:08X}", Engine::AudioProbe::CurrentWeapon()); Rig::MouseRight(true); } },
					{ 1.1, [] { REX::INFO("P3 W3 PICTURE [B] 0.6 s into the aim"); } },
					{ 2.0, [] { REX::INFO("P3 W3 PICTURE [B] 1.5 s into the aim"); } },
					{ 2.1, [] { REX::INFO("P3 W3 [B] the shot at 1.6 s into the aim"); Rig::MouseLeft(true); } },
					{ 2.25, [] { Rig::MouseLeft(false); } },
					{ 2.5, [] { Rig::MouseRight(false); REX::INFO("P3 W3: the aim is released (2.0 s)"); } },
					{ 3.0, [] { REX::INFO("P3 W3 PICTURE [B] 0.5 s after the release (the HUD's count after the shot)"); } },
					{ 3.5, [] { REX::INFO("P3 W3 PICTURE [B] 1.0 s after the release"); } },
				});
				s.Do([tag] { REX::INFO("P3 W3 [{}_B] after: pose {}, cover {}, camera {}, weapon key 0x{:08X}", tag, CoverPose::ToString(CoverPose::GetInfo().state), Core::ToString(Cover::GetInfo().state), Frame::Latest().cameraIndex, Engine::AudioProbe::CurrentWeapon()); });
				s.Do([] { Rig::Tap(PoseRightKey(), 1.0); });
				s.Wait(1.8);
				s.Do([] { Rig::Tap(PoseLeftKey(), 1.0); });
				s.Wait(1.8);
				s.Do([tag] { REX::INFO("P3 W3 [{}_B] after the walks: pose {}, cover {}, camera {}", tag, CoverPose::ToString(CoverPose::GetInfo().state), Core::ToString(Cover::GetInfo().state), Frame::Latest().cameraIndex); });
				AddPoseLeaveSneak(s, r);
				s.Wait(2.0);
				s.Do([tag] { REX::INFO("P3 W3 [{}_B] after the leave: pose {}, cover {}, camera {}, stance field {}", tag, CoverPose::ToString(CoverPose::GetInfo().state), Core::ToString(Cover::GetInfo().state), Frame::Latest().cameraIndex, CoverPose::StanceField()); });
				// the cover again for A1 and C
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.5);
				if (a_tall) {
					s.Do([] { Rig::Tap(PoseRightKey(), 3.0); });
					s.Wait(4.2);
				}
				// A1: a switch 1 s into a reload in the pose: a round spent by a blind-fire tap, the reload tapped, key 3 (the cutter) 1 s later, then key 2 again, a round spent and a reload afterwards
				s.Do([] { Rig::MouseLeft(true); });
				s.Wait(0.15);
				s.Do([] { Rig::MouseLeft(false); });
				s.Wait(2.6);
				s.Do([tag] { REX::INFO("P3 W3 [{}_A1]: the reload is tapped; the key 3 follows 1.0 s later; pose {}", tag, CoverPose::ToString(CoverPose::GetInfo().state)); Rig::Tap(Rig::kKeyR, 0.12); });
				AddW1Trace(s, tag + "_A1_switch_in_reload", 1.0, 4.0, &Rig::kKey3);
				AddW1Trace(s, tag + "_A1_back", 1.0, 4.0, &Rig::kKey2);
				s.Do([] { Rig::MouseLeft(true); });
				s.Wait(0.15);
				s.Do([] { Rig::MouseLeft(false); });
				s.Wait(2.6);
				AddW1Trace(s, tag + "_A1_reload_afterwards", 0.5, 6.0, &Rig::kKeyR);
				// C: the fourth weapon, key 4, in the pose: pictures at 0.3 s and 2 s, then the checks and the aim
				AddW1Trace(s, tag + "_C_pose_key4", 1.0, 4.0, &Rig::kKey4);
				AddW1Checks(s, tag + "_C_after_key4", a_tall);
				// the experiment, once: the event ReevaluateGraphState 0.1 s after the draw call (key 2, then key 4 again)
				s.Do([] { Config::Mutable().poseDrawReevaluate = 1; REX::INFO("P3 W3: the experiment: ReevaluateGraphState 0.1 s after the draw call (iPoseDrawReevaluate 1)"); });
				AddW1Trace(s, tag + "_C_exp_key2", 1.0, 3.0, &Rig::kKey2);
				AddW1Trace(s, tag + "_C_exp_key4", 1.0, 4.0, &Rig::kKey4);
				s.Do([] { Config::Mutable().poseDrawReevaluate = 0; });
				AddW1Aim(s, tag + "_C_after_key4");
				AddPoseLeaveSneak(s, r);
				s.Wait(2.0);
				// the open: key 3 then key 4, a picture and the HUD
				AddW1Trace(s, tag + "_C_open_key3", 1.0, 3.0, &Rig::kKey3);
				AddW1Trace(s, tag + "_C_open_key4", 1.0, 4.0, &Rig::kKey4);
				s.Do([] {
					Engine::AudioProbe::Arm(false, "W3");
					const auto wi = Engine::WeaponDraw::GetInfo();
					Result(std::format("P3 W3 done: the draw work was called {} time(s), gave up {} time(s), faulted {}", wi.calls, wi.giveUps, wi.faulted));
					Config::Mutable().poseWeaponSwap = 0;
					Config::Mutable().poseScopeAim = 0;
				});
			}
			return Run(s, c.now);
		}
		bool StageW3Tall(const Ctx& c) { return StageW3(c, true); }
		bool StageW3Low(const Ctx& c) { return StageW3(c, false); }

		constexpr std::array<Stage, 4> kSet68{ {
			{ "ready", StageReady, false },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "W3 the weapon switch as a play candidate, tall", StageW3Tall, true },
			{ "summary", StageSummary, false },
		} };
		constexpr std::array<Stage, 4> kSet69{ {
			{ "ready", StageReady, false },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "W3 the weapon switch as a play candidate, low", StageW3Low, true },
			{ "summary", StageSummary, false },
		} };

		// the rerun in one launch: the tall round, then the low round at the user's low wall (the second round starts with the weapon the tall one ended on)
		constexpr std::array<Stage, 6> kSet70{ {
			{ "ready", StageReady, false },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "W3 the weapon switch as a play candidate, tall", StageW3Tall, true },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "W3 the weapon switch as a play candidate, low", StageW3Low, true },
			{ "summary", StageSummary, false },
		} };

		// the smoke of the candidate (set 71): three switches in the pose with the INI's own iPoseWeaponSwap (3) and iPoseScopeAim, then the checks
		bool StageW3Smoke(const Ctx& c, const bool a_tall)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				g_tall = a_tall;
				g_autoTall = a_tall;
				g_spot = a_tall ? g_leftSpot : Spot{};
				const std::string tag = a_tall ? "T" : "L";
				s.Do([a_tall] {
					Rig::ReleaseEverything("w3 smoke");
					Cover::ForceReset("w3 smoke");
					Config::Mutable().coverPoses = true;
					Config::Mutable().poseTall = a_tall;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					Config::Mutable().autoCoverTall = a_tall ? 2 : 0;
					Config::Mutable().poseCornerHold = 1;
					REX::INFO("P3 W3 SMOKE: the {} pose; the INI's iPoseWeaponSwap {}, iPoseScopeAim {}, draw delay {:.2f} s; the weapon key at the start 0x{:08X}", a_tall ? "tall" : "low", Config::Get().poseWeaponSwap, Config::Get().poseScopeAim, Config::Get().poseWeaponDrawDelay, Engine::AudioProbe::CurrentWeapon());
					Engine::AudioProbe::Arm(true, "W3 smoke");
				});
				AddWeaponTo(s, true);
				if (a_tall) {
					s.Do([] { FloorBegin(g_spot.x, g_spot.y, Snap().position.z); });
					s.Until(FloorPoll, 3.0);
					s.Do([] {
						if (s_floor.have && std::fabs(s_floor.z - Snap().position.z) < 1.5f) {
							const auto& p = Snap().position;
							MoveBegin({ g_spot.x, g_spot.y, s_floor.z }, (std::max)(1.0, static_cast<double>(std::hypot(g_spot.x - p.x, g_spot.y - p.y) / 3.5f)));
						}
					});
					s.Until(MoveStep, 15.0);
				}
				s.Wait(1.0);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.5);
				AddW1Trace(s, tag + "_S_key2", 0.5, 3.0, &Rig::kKey2);
				AddW1Trace(s, tag + "_S_key3", 0.5, 3.0, &Rig::kKey3);
				AddW1Trace(s, tag + "_S_key2_again", 0.5, 3.0, &Rig::kKey2);
				AddW1Checks(s, tag + "_S_after", a_tall);
				s.Do([tag] {
					const auto wi = Engine::WeaponDraw::GetInfo();
					Result(std::format("P3 W3 SMOKE [{}]: pose {}, cover {}, weapon key 0x{:08X}; the draw work called {} time(s), gave up {} time(s), faulted {}", tag, CoverPose::ToString(CoverPose::GetInfo().state), Core::ToString(Cover::GetInfo().state), Engine::AudioProbe::CurrentWeapon(), wi.calls, wi.giveUps, wi.faulted));
					Summarise(std::string("W3 smoke: three switches in the pose, the pose stands, no give-up, no fault (") + tag + ")", CoverPose::GetInfo().state == CoverPose::State::Posed && wi.giveUps == 0 && !wi.faulted);
				});
				AddPoseLeaveSneak(s, r);
				s.Wait(1.0);
				s.Do([] { Engine::AudioProbe::Arm(false, "W3 smoke"); g_spot = Spot{}; });
			}
			return Run(s, c.now);
		}
		bool StageW3SmokeTall(const Ctx& c) { return StageW3Smoke(c, true); }
		bool StageW3SmokeLow(const Ctx& c) { return StageW3Smoke(c, false); }

		constexpr std::array<Stage, 9> kSet71{ {
			{ "ready", StageReady, false },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "M4f the reloads: two plain, one in the tall pose", StageAudioTall, true },
			{ "M4e the user's sequence at the right end", StageTurnReplay, true },
			{ "M4e the switching at the tall wall (reversals, toes)", StageFootSwitchTall, true },
			{ "W3 smoke: three switches in the tall pose", StageW3SmokeTall, true },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "W3 smoke: three switches in the low pose", StageW3SmokeLow, true },
			{ "summary", StageSummary, false },
		} };

		// ---- W4 (REFERENCE 23.22): the aim lock for a scoped weapon, the replay that ends with the weapon, the game's equip clip in the pose --------------------------------------------------
		struct W4Count
		{
			int n{ 0 }, camBad{ 0 }, coverBad{ 0 }, poseBad{ 0 }, sighted{ 0 }, blind{ 0 }, equipping{ 0 }, wsBad{ 0 };
			double tWs3{ -1.0 };
		};
		W4Count g_w4;
		std::array<double, 4> g_w4Has{ -9.0, -9.0, -9.0, -9.0 };
		std::array<double, 4> g_w4In{ -9.0, -9.0, -9.0, -9.0 };

		// W6 (23.24): the children of the weapon and holster nodes around a key change, and the Grendel's key if one of the slots 10 to 12 gives it back
		bool            g_w6On{ false };
		double          g_w6TapT{ -100.0 };
		std::string     g_w6Last;
		const Rig::Key* g_w6GKey{ nullptr };
		const char*     g_w6GName{ "none" };

		void W4Trace(const std::string& a_label, const double a_t)
		{
			const int  cam = Frame::Latest().cameraIndex;
			const auto pose = CoverPose::GetInfo().state;
			const double inCover = VarOr("bIsInCover", -9.0);
			const double sighted = VarOr("iIsSighted", -9.0);
			const double blind = VarOr("iIsBlindFiring", -9.0);
			++g_w4.n;
			if (cam != 20) {
				++g_w4.camBad;
			}
			if (inCover < 0.5) {
				++g_w4.coverBad;
			}
			if (pose != CoverPose::State::Posed) {
				++g_w4.poseBad;
			}
			if (sighted > 0.5) {
				++g_w4.sighted;
			}
			if (blind > 0.5) {
				++g_w4.blind;
			}
			if (VarOr("bIsEquipping", 0.0) > 0.5) {
				++g_w4.equipping;
			}
			if (CoverPose::WeaponState() != 3) {
				++g_w4.wsBad;
			}
			if (g_w6On) {
				const double dt = Now() - g_w6TapT;
				if (dt >= -0.1 && dt <= 0.8) {
					std::string kids;
					for (const auto& n : g_w1Nodes) {
						kids += n.first + "[";
						for (const auto& k : ListChildren(n.second)) {
							kids += std::format("{}@{} ", k.name, k.ptr);
						}
						kids += "] ";
					}
					if (kids != g_w6Last) {
						g_w6Last = kids;
						REX::INFO("P3 W6 KIDS {:+.3f} [{}] {}", dt, a_label, kids);
					}
				}
			}
			const auto si = Engine::ScopeAim::GetInfo();
			REX::INFO("P3 W4 {:+.3f} [{}] camera={} cover={} bIsInCover={:g} stance={} pose={} sighted={:g} syncSighted={:g} blind={:g} rel={:g} eq={:g} ws={} key=0x{:08X} scoped={} locks={}", a_t, a_label, cam, Core::ToString(Cover::GetInfo().state), inCover, CoverPose::StanceField(),
				CoverPose::ToString(pose), sighted, VarOr("iSyncSighted", -9.0), blind, VarOr("bIsReloading", -9.0), VarOr("bIsEquipping", -9.0), CoverPose::WeaponState(), Engine::AudioProbe::CurrentWeapon(), si.scoped ? 1 : 0, si.locks);
		}

		// a_secs seconds with the W4 trace every 20 ms and the actions at their times; at the end one RESULT line with the counters (a_expectClean: the camera stayed 20, bIsInCover 1, the pose Posed throughout)
		void AddW4Timed(Seq& s, const std::string& a_label, const double a_secs, std::vector<W3Act> a_acts, const bool a_expectClean, std::function<bool()> a_skip = nullptr)
		{
			auto acts = std::make_shared<std::vector<W3Act>>(std::move(a_acts));
			auto t0 = std::make_shared<double>(0.0);
			auto next = std::make_shared<double>(0.0);
			s.Do([t0, next, acts, a_label] {
				*t0 = Now();
				*next = 0.0;
				g_w4 = W4Count{};
				for (auto& a : *acts) {
					a.done = false;
				}
				REX::INFO("P3 W4 CASE [{}] begins", a_label);
			});
			s.Until([t0, next, acts, a_label, a_secs, a_skip] {
				const double now = Now();
				if (a_skip && a_skip()) {
					return true;
				}
				if (now >= *next) {
					*next = now + 0.02;
					W4Trace(a_label, now - *t0);
				}
				for (auto& a : *acts) {
					if (!a.done && now - *t0 >= a.t) {
						a.done = true;
						a.fn();
					}
				}
				return now - *t0 >= a_secs;
			}, a_secs + 2.0);
			s.Do([a_label, a_expectClean, a_skip] {
				if (a_skip && a_skip()) {
					REX::INFO("P3 W6 CASE [{}] skipped: no key gave the Grendel back", a_label);
					return;
				}
				const bool clean = g_w4.camBad == 0 && g_w4.coverBad == 0 && g_w4.poseBad == 0 && g_w4.equipping == 0 && g_w4.wsBad == 0;
				Result(std::format("P3 W4 CASE [{}] ends: {} ticks; camera not 20: {}; bIsInCover 0: {}; pose not Posed: {}; iIsSighted 1: {}; iIsBlindFiring 1: {}; bIsEquipping 1: {}; weapon state not 3: {}; weapon 0x{:08X}, pose {}, camera {}{}", a_label, g_w4.n, g_w4.camBad, g_w4.coverBad, g_w4.poseBad, g_w4.sighted, g_w4.blind, g_w4.equipping, g_w4.wsBad,
					Engine::AudioProbe::CurrentWeapon(), CoverPose::ToString(CoverPose::GetInfo().state), Frame::Latest().cameraIndex, a_expectClean ? (clean ? " | CLEAN" : " | NOT CLEAN") : ""));
				if (a_expectClean) {
					Summarise("W4 " + a_label + ": camera 20, in cover, pose Posed throughout", clean);
				}
			});
		}

		void W4Row(Seq& s, const int a_idx, const char* a_name, const Rig::Key* a_key)
		{
			const bool hasKey = a_key != nullptr;
			const Rig::Key key = a_key ? *a_key : Rig::Key{};
			const std::string name = a_name;
			s.Do([hasKey, key, name] {
				if (hasKey) {
					REX::INFO("P3 W4 TABLE [{}]: the quick-key is tapped (weapon key 0x{:08X} before)", name, Engine::AudioProbe::CurrentWeapon());
					Rig::Tap(key, 0.12);
				}
			});
			s.Wait(hasKey ? 3.5 : 0.5);
			s.Do([a_idx, name] {
				g_w4Has[static_cast<std::size_t>(a_idx)] = VarOr("bHasScope", -9.0);
				g_w4In[static_cast<std::size_t>(a_idx)] = VarOr("bInScopes", -9.0);
				REX::INFO("P3 W4 TABLE [{}] at rest in the open: weapon key 0x{:08X}, bHasScope {:g}, bInScopes {:g} (-9 = does not resolve), camera {}, ws {}", name, Engine::AudioProbe::CurrentWeapon(), g_w4Has[static_cast<std::size_t>(a_idx)], g_w4In[static_cast<std::size_t>(a_idx)], Frame::Latest().cameraIndex, CoverPose::WeaponState());
				Rig::MouseRight(true);
			});
			s.Wait(1.0);
			s.Do([name] {
				REX::INFO("P3 W4 TABLE [{}] 1 s into the aim: bHasScope {:g}, bInScopes {:g}, camera {}", name, VarOr("bHasScope", -9.0), VarOr("bInScopes", -9.0), Frame::Latest().cameraIndex);
				REX::INFO("P3 W4 PICTURE [{}] 1 s into the aim in the open", name);
				Rig::MouseRight(false);
			});
			s.Wait(2.0);
			s.Do([name] { REX::INFO("P3 W4 TABLE [{}] 2 s after the aim: camera {}", name, Frame::Latest().cameraIndex); });
		}

		// the table of the four weapons in the open and the decision which way the lock knows a scope: bHasScope when it is true for the Orion alone, else learning
		void AddW4Table(Seq& s)
		{
			s.Do([] { Engine::ScopeAim::Forget(); REX::INFO("P3 W4 TABLE: begins (weapon key {:#010x})", Engine::AudioProbe::CurrentWeapon()); });
			W4Row(s, 0, "Grendel 0x00028A02 (the save's own)", nullptr);
			W4Row(s, 1, "Orion 0x002773C8 (key 2)", &Rig::kKey2);
			W4Row(s, 2, "Cutter 0x00016758 (key 3)", &Rig::kKey3);
			W4Row(s, 3, "Coachman 0x0026D96B (key 4)", &Rig::kKey4);
			s.Do([] {
				const bool resolves = g_w4Has[0] > -8.0 && g_w4Has[1] > -8.0 && g_w4Has[2] > -8.0 && g_w4Has[3] > -8.0;
				const bool separates = resolves && g_w4Has[1] > 0.5 && g_w4Has[0] < 0.5 && g_w4Has[2] < 0.5 && g_w4Has[3] < 0.5;
				Result(std::format("P3 W4 TABLE: bHasScope Grendel {:g}, Orion {:g}, Cutter {:g}, Coachman {:g}; bInScopes {:g}, {:g}, {:g}, {:g}: the graph variable {} the Orion from the rest", g_w4Has[0], g_w4Has[1], g_w4Has[2], g_w4Has[3], g_w4In[0], g_w4In[1], g_w4In[2], g_w4In[3],
					!resolves ? "does not resolve and so cannot separate" : (separates ? "separates" : "does not separate")));
				if (separates) {
					Config::Mutable().scopeSource = 1;
					Engine::ScopeAim::ForgetLearned();
					REX::INFO("P3 W4 TABLE: the lock uses bHasScope alone from here (iScopeSource 1; what the open aims learned is forgotten)");
				} else {
					Config::Mutable().scopeSource = 3;
					REX::INFO("P3 W4 TABLE: the lock keeps both ways (iScopeSource 3): the learned weapons count");
				}
				Summarise(std::string("W4 table: bHasScope ") + (!resolves ? "does not resolve" : (separates ? "separates the Orion" : "does not separate")), true);
			});
		}

		// the Orion in the hands in the pose: the aim control held 2 s, three times
		void AddW4LockProof(Seq& s, const std::string& a_tag)
		{
			for (int i = 1; i <= 3; ++i) {
				AddW4Timed(s, a_tag + "_lock" + std::to_string(i), 3.5, {
					{ 0.3, [i, a_tag] { REX::INFO("P3 W4 PICTURE [{}_lock{}] before the aim (weapon key 0x{:08X})", a_tag, i, Engine::AudioProbe::CurrentWeapon()); Rig::MouseRight(true); } },
					{ 0.9, [i, a_tag] { REX::INFO("P3 W4 PICTURE [{}_lock{}] 0.6 s into the aim", a_tag, i); } },
					{ 1.8, [i, a_tag] { REX::INFO("P3 W4 PICTURE [{}_lock{}] 1.5 s into the aim", a_tag, i); } },
					{ 2.3, [] { Rig::MouseRight(false); } },
				}, true);
				s.Wait(0.6);
			}
		}

		bool StageW4(const Ctx& c, const bool a_tall)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				g_tall = a_tall;
				g_autoTall = a_tall;
				g_spot = a_tall ? g_leftSpot : Spot{};
				const std::string tag = a_tall ? "T" : "L";
				s.Do([a_tall] {
					Rig::ReleaseEverything("w4");
					Cover::ForceReset("w4");
					Config::Mutable().coverPoses = true;
					Config::Mutable().poseTall = a_tall;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					Config::Mutable().autoCoverTall = a_tall ? 2 : 0;
					Config::Mutable().poseCornerHold = 1;
					Config::Mutable().poseWeaponSwap = 3;
					Config::Mutable().poseScopeAim = 2;
					if (a_tall) {
						Config::Mutable().scopeSource = 3;
					}
					Config::Mutable().poseDrawReevaluate = 0;
					REX::INFO("P3 W4: the {} pose; iPoseWeaponSwap {}, iPoseScopeAim {}, iScopeSource {}, draw delay {:.2f} s; the weapon key at the start 0x{:08X}", a_tall ? "tall" : "low", Config::Get().poseWeaponSwap, Config::Get().poseScopeAim, Config::Get().scopeSource, Config::Get().poseWeaponDrawDelay, Engine::AudioProbe::CurrentWeapon());
					W3Names();
					Engine::AudioProbe::Arm(true, "W4");
				});
				AddWeaponTo(s, true);
				if (a_tall) {
					s.Do([] { FloorBegin(g_spot.x, g_spot.y, Snap().position.z); });
					s.Until(FloorPoll, 3.0);
					s.Do([] {
						if (s_floor.have && std::fabs(s_floor.z - Snap().position.z) < 1.5f) {
							const auto& p = Snap().position;
							MoveBegin({ g_spot.x, g_spot.y, s_floor.z }, (std::max)(1.0, static_cast<double>(std::hypot(g_spot.x - p.x, g_spot.y - p.y) / 3.5f)));
						}
					});
					s.Until(MoveStep, 15.0);
				}
				s.Wait(1.0);
				if (a_tall) {
					// the Grendel in the tall pose at the open end: the aim works as before (the one weapon of the save, so it is done first)
					AddPoseEnter(s, r, 0, true);
					s.Wait(1.5);
					s.Do([] { Rig::Tap(PoseRightKey(), 3.0); REX::INFO("P3 W4: the walk to the open end"); });
					s.Wait(4.2);
					AddW4Timed(s, "T_grendel_aim", 4.0, {
						{ 0.3, [] { REX::INFO("P3 W4 PICTURE [T_grendel_aim] before the aim (weapon key 0x{:08X})", Engine::AudioProbe::CurrentWeapon()); Rig::MouseRight(true); } },
						{ 1.3, [] { REX::INFO("P3 W4 PICTURE [T_grendel_aim] 1 s into the aim"); } },
						{ 2.3, [] { Rig::MouseRight(false); } },
					}, true);
					AddPoseLeaveSneak(s, r);
					s.Wait(2.0);
					// the table in the open (the Grendel, then keys 2, 3, 4: the round ends with the Coachman in the hands)
					AddW4Table(s);
				} else {
					s.Do([] { REX::INFO("P3 W4: the low round starts with the weapon 0x{:08X}", Engine::AudioProbe::CurrentWeapon()); });
				}
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.5);
				if (a_tall) {
					s.Do([] { Rig::Tap(PoseRightKey(), 3.0); REX::INFO("P3 W4: the walk to the open end"); });
					s.Wait(4.2);
				}
				s.Do([tag] { REX::INFO("P3 W4 [{}] in the pose: weapon 0x{:08X}, bHasScope reads {:g}, bInScopes {:g} in the pose's graph; the lock knows it as {}", tag, Engine::AudioProbe::CurrentWeapon(), VarOr("bHasScope", -9.0), VarOr("bInScopes", -9.0), Engine::ScopeAim::KnownScoped(Engine::AudioProbe::CurrentWeapon()) ? "scoped" : "not scoped"); });
				if (!a_tall) {
					// low round: the weapon is the one the tall round ended with (the Orion): the lock first
					AddW4LockProof(s, tag);
				} else {
					// the unscoped weapon aims: the Coachman at the open end
					AddW4Timed(s, tag + "_coachman_aim", 4.0, {
						{ 0.3, [tag] { REX::INFO("P3 W4 PICTURE [{}_coachman_aim] before the aim (weapon key 0x{:08X})", tag, Engine::AudioProbe::CurrentWeapon()); Rig::MouseRight(true); } },
						{ 1.3, [tag] { REX::INFO("P3 W4 PICTURE [{}_coachman_aim] 1 s into the aim", tag); } },
						{ 2.3, [] { Rig::MouseRight(false); } },
					}, true);
				}
				if (!a_tall) {
					// low: the Coachman in the pose, then its aim
					AddW1Trace(s, tag + "_to_coachman", 0.5, 3.0, &Rig::kKey4);
					AddW4Timed(s, tag + "_coachman_aim", 4.0, {
						{ 0.3, [tag] { REX::INFO("P3 W4 PICTURE [{}_coachman_aim] before the aim (weapon key 0x{:08X})", tag, Engine::AudioProbe::CurrentWeapon()); Rig::MouseRight(true); } },
						{ 1.3, [tag] { REX::INFO("P3 W4 PICTURE [{}_coachman_aim] 1 s into the aim", tag); } },
						{ 2.3, [] { Rig::MouseRight(false); } },
					}, false);
				}
				// A2: the aim held, the Orion arrives (key 2) 0.8 s into it; the aim ends at the switch, the control is released at 2.7 s
				AddW4Timed(s, tag + "_A2_switch_to_scoped_while_aiming", 6.0, {
					{ 0.2, [tag] { REX::INFO("P3 W4 PICTURE [{}_A2] before the aim (weapon key 0x{:08X})", tag, Engine::AudioProbe::CurrentWeapon()); Rig::MouseRight(true); } },
					{ 1.0, [tag] { REX::INFO("P3 W4 PICTURE [{}_A2] 0.8 s into the aim, key 2 is tapped (weapon key 0x{:08X})", tag, Engine::AudioProbe::CurrentWeapon()); Engine::WeaponDraw::LogState("W4 A2 pre-switch"); Rig::Tap(Rig::kKey2, 0.12); } },
					{ 2.0, [tag] { REX::INFO("P3 W4 PICTURE [{}_A2] 1 s after the key, the aim control still held", tag); } },
					{ 2.7, [] { Rig::MouseRight(false); REX::INFO("P3 W4: the aim control is released"); } },
					{ 4.5, [tag] { REX::INFO("P3 W4 PICTURE [{}_A2] 1.8 s after the release (weapon key 0x{:08X})", tag, Engine::AudioProbe::CurrentWeapon()); } },
				}, true);
				if (a_tall) {
					AddW4LockProof(s, tag);
				}
				// blind fire with the Orion in the pose: the count drops
				AddW4Timed(s, tag + "_orion_blind_fire", 3.5, {
					{ 0.5, [tag] { REX::INFO("P3 W4 PICTURE [{}_blind] before the blind fire (the HUD's count)", tag); Rig::MouseLeft(true); } },
					{ 0.65, [] { Rig::MouseLeft(false); } },
					{ 2.5, [tag] { REX::INFO("P3 W4 PICTURE [{}_blind] 1.85 s after the tap (the HUD's count)", tag); } },
				}, true);
				// item 3: a reload in the pose, a switch 1 s into it (key 3), the switch back at once (key 2), three times
				for (int rep = 1; rep <= 3; ++rep) {
					s.Do([] { Rig::MouseLeft(true); });
					s.Wait(0.15);
					s.Do([] { Rig::MouseLeft(false); });
					s.Wait(2.6);
					AddW4Timed(s, tag + "_X" + std::to_string(rep) + "_reload_switch_back", 6.0, {
						{ 0.3, [tag, rep] { REX::INFO("P3 W4 X{} [{}]: the reload is tapped (weapon 0x{:08X})", rep, tag, Engine::AudioProbe::CurrentWeapon()); Engine::WeaponDraw::LogState("W4X pre-reload"); Rig::Tap(Rig::kKeyR, 0.12); } },
						{ 1.3, [tag, rep] { REX::INFO("P3 W4 X{} [{}]: key 3 (the Cutter) 1.0 s into the reload", rep, tag); Engine::WeaponDraw::LogState("W4X pre-switch1"); Rig::Tap(Rig::kKey3, 0.12); } },
						{ 1.9, [tag, rep] { REX::INFO("P3 W4 X{} [{}]: key 2 (the Orion) 0.6 s after the first switch", rep, tag); Engine::WeaponDraw::LogState("W4X pre-switch2"); Rig::Tap(Rig::kKey2, 0.12); } },
						{ 5.5, [] { Engine::WeaponDraw::LogState("W4X end"); } },
					}, false);
					s.Wait(2.5);
					s.Do([tag, rep] { REX::INFO("P3 W4 X{} [{}] after: pose {}, cover {}, weapon 0x{:08X}", rep, tag, CoverPose::ToString(CoverPose::GetInfo().state), Core::ToString(Cover::GetInfo().state), Engine::AudioProbe::CurrentWeapon()); });
				}
				AddPoseLeaveSneak(s, r);
				s.Wait(2.0);
				// outside the pose nothing changes: the Orion's scope comes up in the open
				AddW4Timed(s, tag + "_open_orion_aim", 4.0, {
					{ 0.3, [] { Rig::MouseRight(true); } },
					{ 1.3, [tag] { REX::INFO("P3 W4 PICTURE [{}_open] 1 s into the aim in the open (weapon key 0x{:08X})", tag, Engine::AudioProbe::CurrentWeapon()); } },
					{ 2.3, [] { Rig::MouseRight(false); } },
				}, false);
				s.Do([tag] {
					const auto si = Engine::ScopeAim::GetInfo();
					const auto wi = Engine::WeaponDraw::GetInfo();
					Result(std::format("P3 W4 done [{}]: aim presses held back {}, notices {}, learned {}, aims ended at a switch {}; the draw work {} call(s), {} give-up(s), faulted {}", tag, si.locks, si.notices, si.learned, si.ends, wi.calls, wi.giveUps, wi.faulted));
					Engine::AudioProbe::Arm(false, "W4");
					g_spot = Spot{};
				});
			}
			return Run(s, c.now);
		}
		bool StageW4Tall(const Ctx& c) { return StageW4(c, true); }
		bool StageW4Low(const Ctx& c) { return StageW4(c, false); }

		constexpr std::array<Stage, 6> kSet72{ {
			{ "ready", StageReady, false },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "W4 the aim lock and the equip clip, tall", StageW4Tall, true },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "W4 the aim lock and the equip clip, low", StageW4Low, true },
			{ "summary", StageSummary, false },
		} };

		// the smoke of the play build (set 73): the INI's own values; the Grendel's aim in the low pose first, then the usual smoke, three or more switches per height, the locked aim once per height
		bool StageW4LowGrendel(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				g_tall = false;
				g_autoTall = false;
				g_spot = Spot{};
				s.Do([] {
					Rig::ReleaseEverything("w4 low grendel");
					Cover::ForceReset("w4 low grendel");
					Config::Mutable().coverPoses = true;
					Config::Mutable().poseTall = false;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					Config::Mutable().autoCoverTall = 0;
					Config::Mutable().poseCornerHold = 1;
					REX::INFO("P3 W4 SMOKE: the INI's iPoseWeaponSwap {}, iPoseScopeAim {}, iScopeSource {}, draw delay {:.2f} s; the weapon key at the start 0x{:08X}", Config::Get().poseWeaponSwap, Config::Get().poseScopeAim, Config::Get().scopeSource, Config::Get().poseWeaponDrawDelay, Engine::AudioProbe::CurrentWeapon());
					Engine::AudioProbe::Arm(true, "W4 smoke");
				});
				AddWeaponTo(s, true);
				s.Wait(1.0);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.5);
				AddW4Timed(s, "L_grendel_aim", 4.0, {
					{ 0.3, [] { REX::INFO("P3 W4 PICTURE [L_grendel_aim] before the aim (weapon key 0x{:08X})", Engine::AudioProbe::CurrentWeapon()); Rig::MouseRight(true); } },
					{ 1.3, [] { REX::INFO("P3 W4 PICTURE [L_grendel_aim] 1 s into the aim"); } },
					{ 2.3, [] { Rig::MouseRight(false); } },
				}, true);
				AddPoseLeaveSneak(s, r);
				s.Wait(1.5);
				s.Do([] { Engine::AudioProbe::Arm(false, "W4 smoke"); });
			}
			return Run(s, c.now);
		}

		bool StageW4Smoke(const Ctx& c, const bool a_tall)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				g_tall = a_tall;
				g_autoTall = a_tall;
				g_spot = a_tall ? g_leftSpot : Spot{};
				const std::string tag = a_tall ? "T" : "L";
				s.Do([a_tall] {
					Rig::ReleaseEverything("w4 smoke");
					Cover::ForceReset("w4 smoke");
					Config::Mutable().coverPoses = true;
					Config::Mutable().poseTall = a_tall;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					Config::Mutable().autoCoverTall = a_tall ? 2 : 0;
					Config::Mutable().poseCornerHold = 1;
					REX::INFO("P3 W4 SMOKE: the {} pose; the INI's iPoseWeaponSwap {}, iPoseScopeAim {}, iScopeSource {}; the weapon key at the start 0x{:08X}", a_tall ? "tall" : "low", Config::Get().poseWeaponSwap, Config::Get().poseScopeAim, Config::Get().scopeSource, Engine::AudioProbe::CurrentWeapon());
					if (Config::Get().p3Set == 73) {
						Engine::ScopeAim::LearnForRig(0x002773C8u);  // the play of an earlier hour: the Orion was aimed in the open once
					}
					Engine::AudioProbe::Arm(true, "W4 smoke");
				});
				AddWeaponTo(s, true);
				if (a_tall) {
					s.Do([] { FloorBegin(g_spot.x, g_spot.y, Snap().position.z); });
					s.Until(FloorPoll, 3.0);
					s.Do([] {
						if (s_floor.have && std::fabs(s_floor.z - Snap().position.z) < 1.5f) {
							const auto& p = Snap().position;
							MoveBegin({ g_spot.x, g_spot.y, s_floor.z }, (std::max)(1.0, static_cast<double>(std::hypot(g_spot.x - p.x, g_spot.y - p.y) / 3.5f)));
						}
					});
					s.Until(MoveStep, 15.0);
				}
				s.Wait(1.0);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.5);
				if (a_tall) {
					// the replay ends with the weapon: a reload in the pose with the Grendel (its sound was learned by the stages before), the Orion's key 1.0 s into it
					AddW4Timed(s, "T_replay_cancel", 5.0, {
						{ 0.3, [] { Rig::MouseLeft(true); } },
						{ 0.45, [] { Rig::MouseLeft(false); } },
						{ 2.0, [] { REX::INFO("P3 W4: the reload is tapped (weapon 0x{:08X}); key 2 follows 1.0 s later", Engine::AudioProbe::CurrentWeapon()); Rig::Tap(Rig::kKeyR, 0.12); } },
						{ 3.0, [] { REX::INFO("P3 W4: key 2 is tapped 1.0 s into the reload"); Rig::Tap(Rig::kKey2, 0.12); } },
					}, false);
					s.Wait(1.0);
					AddW1Trace(s, tag + "_S_key3", 0.5, 3.0, &Rig::kKey3);
					AddW1Trace(s, tag + "_S_key4", 0.5, 3.0, &Rig::kKey4);
					AddW1Trace(s, tag + "_S_key2", 0.5, 3.0, &Rig::kKey2);
					s.Do([] { Rig::Tap(PoseRightKey(), 3.0); REX::INFO("P3 W4: the walk to the open end"); });
					s.Wait(4.2);
				} else {
					AddW1Trace(s, tag + "_S_key3", 0.5, 3.0, &Rig::kKey3);
					AddW1Trace(s, tag + "_S_key4", 0.5, 3.0, &Rig::kKey4);
					AddW1Trace(s, tag + "_S_key2", 0.5, 3.0, &Rig::kKey2);
				}
				// the locked aim with the Orion once, the blind fire, the walks and the checks
				AddW4Timed(s, tag + "_S_orion_locked_aim", 3.5, {
					{ 0.3, [] { Rig::MouseRight(true); } },
					{ 1.0, [tag] { REX::INFO("P3 W4 PICTURE [{}_S_lock] 0.7 s into the aim", tag); } },
					{ 2.3, [] { Rig::MouseRight(false); } },
				}, true);
				AddW4Timed(s, tag + "_S_orion_blind_fire", 3.0, {
					{ 0.5, [] { Rig::MouseLeft(true); } },
					{ 0.65, [] { Rig::MouseLeft(false); } },
				}, true);
				AddW1Checks(s, tag + "_S_after", a_tall);
				s.Do([tag] {
					const auto wi = Engine::WeaponDraw::GetInfo();
					const auto si = Engine::ScopeAim::GetInfo();
					Result(std::format("P3 W4 SMOKE [{}]: pose {}, cover {}, weapon key 0x{:08X}; the draw work called {} time(s), gave up {} time(s), faulted {}; aim presses held back {}", tag, CoverPose::ToString(CoverPose::GetInfo().state), Core::ToString(Cover::GetInfo().state), Engine::AudioProbe::CurrentWeapon(), wi.calls, wi.giveUps, wi.faulted, si.locks));
					Summarise(std::string("W4 smoke: the pose stands, no give-up, no fault, the aim was locked (") + tag + ")", CoverPose::GetInfo().state == CoverPose::State::Posed && wi.giveUps == 0 && !wi.faulted && si.locks >= 1);
				});
				AddPoseLeaveSneak(s, r);
				s.Wait(1.0);
				s.Do([] { Engine::AudioProbe::Arm(false, "W4 smoke"); g_spot = Spot{}; });
			}
			return Run(s, c.now);
		}
		bool StageW4SmokeTall(const Ctx& c) { return StageW4Smoke(c, true); }
		bool StageW4SmokeLow(const Ctx& c) { return StageW4Smoke(c, false); }

		// the first aim with a scoped weapon in the pose that the lock does not know yet (nothing learned, bHasScope does not resolve): the engine takes the camera out of third person and ends the
		// cover once, the plugin learns the weapon from it; the second aim is locked (REFERENCE 23.22)
		bool StageW4FirstAim(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				g_tall = false;
				g_autoTall = false;
				g_spot = Spot{};
				s.Do([] {
					Rig::ReleaseEverything("w4 first aim");
					Cover::ForceReset("w4 first aim");
					Config::Mutable().coverPoses = true;
					Config::Mutable().poseTall = false;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					Config::Mutable().autoCoverTall = 0;
					Config::Mutable().poseCornerHold = 1;
					Engine::ScopeAim::Forget();
					REX::INFO("P3 W4 FIRST AIM: nothing is known about the weapons; the weapon key 0x{:08X}", Engine::AudioProbe::CurrentWeapon());
				});
				s.Wait(1.0);
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.5);
				AddW4Timed(s, "L_first_aim", 5.0, {
					{ 0.3, [] { REX::INFO("P3 W4 PICTURE [L_first_aim] before the first aim (weapon key 0x{:08X})", Engine::AudioProbe::CurrentWeapon()); Rig::MouseRight(true); } },
					{ 1.3, [] { REX::INFO("P3 W4 PICTURE [L_first_aim] 1 s into the first aim"); } },
					{ 2.3, [] { Rig::MouseRight(false); } },
				}, false);
				s.Wait(3.0);
				s.Do([] {
					const auto si = Engine::ScopeAim::GetInfo();
					Result(std::format("P3 W4 FIRST AIM: 5.3 s after: pose {}, cover {}, camera {}; learned {}, the weapon is {}", CoverPose::ToString(CoverPose::GetInfo().state), Core::ToString(Cover::GetInfo().state), Frame::Latest().cameraIndex, si.learned, si.scoped ? "scoped" : "not scoped"));
				});
				AddPoseLeaveSneak(s, r);
				s.Wait(1.0);
				// the second aim, if the pose was taken again: locked
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.5);
				AddW4Timed(s, "L_second_aim", 4.0, {
					{ 0.3, [] { Rig::MouseRight(true); } },
					{ 1.3, [] { REX::INFO("P3 W4 PICTURE [L_second_aim] 1 s into the second aim"); } },
					{ 2.3, [] { Rig::MouseRight(false); } },
				}, true);
				AddPoseLeaveSneak(s, r);
				s.Wait(1.0);
			}
			return Run(s, c.now);
		}

		constexpr std::array<Stage, 12> kSet73{ {
			{ "ready", StageReady, false },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "W4 the Grendel's aim in the low pose", StageW4LowGrendel, true },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "M4f the reloads: two plain, one in the tall pose", StageAudioTall, true },
			{ "M4e the user's sequence at the right end", StageTurnReplay, true },
			{ "M4e the switching at the tall wall (reversals, toes)", StageFootSwitchTall, true },
			{ "W4 smoke: the tall pose", StageW4SmokeTall, true },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "W4 smoke: the low pose", StageW4SmokeLow, true },
			{ "W4 the first aim with a scoped weapon the lock does not know", StageW4FirstAim, true },
			{ "summary", StageSummary, false },
		} };

		// ---- W5 (REFERENCE 23.23): the game's own equip clip in the pose: the events that enter the player's graph, the arrivals of the Orion ------------------------------------------------
		// one switch as a case of a_secs seconds: the quick-key at 0.5 s; the counters tell whether the equip clip ran (bIsEquipping, the weapon state not 3) and whether the cover was lost
		void AddW5Switch(Seq& s, const std::string& a_label, const Rig::Key* a_key, const double a_secs, std::function<bool()> a_skip = nullptr)
		{
			const Rig::Key key = *a_key;
			AddW4Timed(s, a_label, a_secs, {
				{ 0.5, [a_label, key] { REX::INFO("P3 W5 KEY [{}]: the quick-key is tapped (weapon key 0x{:08X}, pose {}, ws {})", a_label, Engine::AudioProbe::CurrentWeapon(), CoverPose::ToString(CoverPose::GetInfo().state), CoverPose::WeaponState()); g_w6TapT = Now(); Rig::Tap(key, 0.12); } },
			}, Config::Get().p3Set >= 75 && a_label.find("_to_orion") != std::string::npos, a_skip);
		}

		// Coachman to Orion (keys 4, 2) and Cutter to Orion (keys 3, 2), a_n times each; every case is a_secs long
		void AddW5Arrivals(Seq& s, const std::string& a_tag, const int a_n, const double a_secs)
		{
			for (int i = 1; i <= a_n; ++i) {
				AddW5Switch(s, std::format("{}_C{}_to_coachman", a_tag, i), &Rig::kKey4, a_secs);
				AddW5Switch(s, std::format("{}_C{}_to_orion", a_tag, i), &Rig::kKey2, a_secs);
			}
			for (int i = 1; i <= a_n; ++i) {
				AddW5Switch(s, std::format("{}_K{}_to_cutter", a_tag, i), &Rig::kKey3, a_secs);
				AddW5Switch(s, std::format("{}_K{}_to_orion", a_tag, i), &Rig::kKey2, a_secs);
			}
		}

		bool StageW5(const Ctx& c, const bool a_tall)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				g_tall = a_tall;
				g_autoTall = a_tall;
				g_spot = a_tall ? g_leftSpot : Spot{};
				const std::string tag = a_tall ? "T" : "L";
				const bool        obs = Config::Get().p3Set == 74;
				s.Do([a_tall] {
					Rig::ReleaseEverything("w5");
					Cover::ForceReset("w5");
					Config::Mutable().coverPoses = true;
					Config::Mutable().poseTall = a_tall;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					Config::Mutable().autoCoverTall = a_tall ? 2 : 0;
					Config::Mutable().poseCornerHold = 1;
					Config::Mutable().poseWeaponSwap = 3;
					Config::Mutable().poseScopeAim = 2;
					REX::INFO("P3 W5: the {} pose; iPoseWeaponSwap {}, iPoseScopeAim {}, iGraphEventProbe {}, iPoseEquipBlock {} ('{}'); the weapon key at the start 0x{:08X}", a_tall ? "tall" : "low", Config::Get().poseWeaponSwap, Config::Get().poseScopeAim, Config::Get().graphEventProbe, Config::Get().poseEquipBlock, Config::Get().equipBlockEvents,
						Engine::AudioProbe::CurrentWeapon());
					Engine::AudioProbe::Arm(true, "W5");
					if (Config::Get().p3Set == 75 && a_tall) {
						Engine::ScopeAim::LearnForRig(0x002773C8u, true);  // item 2, first half: the Orion is marked and goes into the cache file
					}
				});
				AddWeaponTo(s, true);
				if (a_tall) {
					s.Do([] { FloorBegin(g_spot.x, g_spot.y, Snap().position.z); });
					s.Until(FloorPoll, 3.0);
					s.Do([] {
						if (s_floor.have && std::fabs(s_floor.z - Snap().position.z) < 1.5f) {
							const auto& p = Snap().position;
							MoveBegin({ g_spot.x, g_spot.y, s_floor.z }, (std::max)(1.0, static_cast<double>(std::hypot(g_spot.x - p.x, g_spot.y - p.y) / 3.5f)));
						}
					});
					s.Until(MoveStep, 15.0);
				}
				s.Wait(1.0);
				if (a_tall && obs) {
					// item 3 of the turn (the replay ends with the weapon): the Grendel in the tall pose at the open end, a round spent by blind fire, the reload, key 2 one second into it
					AddPoseEnter(s, r, 0, true);
					s.Wait(1.5);
					s.Do([] { Rig::Tap(PoseRightKey(), 3.0); REX::INFO("P3 W5: the walk to the open end"); });
					s.Wait(4.2);
					s.Do([] { Rig::MouseLeft(true); });
					s.Wait(0.15);
					s.Do([] { Rig::MouseLeft(false); });
					s.Wait(2.6);
					AddW4Timed(s, "T_replay_cancel", 5.0, {
						{ 0.3, [] { REX::INFO("P3 W5: the reload is tapped (weapon 0x{:08X}); key 2 follows 1.0 s later", Engine::AudioProbe::CurrentWeapon()); Rig::Tap(Rig::kKeyR, 0.12); } },
						{ 1.3, [] { REX::INFO("P3 W5: key 2 is tapped 1.0 s into the reload"); Rig::Tap(Rig::kKey2, 0.12); } },
					}, false);
					s.Wait(1.0);
					AddPoseLeaveSneak(s, r);
					s.Wait(2.0);
					// the reference: the game's ordinary switch in the open (Coachman, Orion, Coachman, Orion)
					AddW5Switch(s, "T_open_to_coachman_1", &Rig::kKey4, 4.0);
					AddW5Switch(s, "T_open_to_orion_1", &Rig::kKey2, 4.0);
					AddW5Switch(s, "T_open_to_coachman_2", &Rig::kKey4, 4.0);
					AddW5Switch(s, "T_open_to_orion_2", &Rig::kKey2, 4.0);
				} else if (a_tall) {
					// the remedy runs start from the weapon the save has: the Orion is the first arrival
				}
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.5);
				AddW5Arrivals(s, tag, 6, 4.0);
				if (Config::Get().p3Set >= 75 && !a_tall) {
					// the low A2 again with the block on: the Coachman in the hands, the aim held, key 2 (the Orion) 0.8 s into it, the control released at 2.7 s
					AddW5Switch(s, "L_A2_to_coachman", &Rig::kKey4, 4.0);
					AddW4Timed(s, "L_A2_switch_to_scoped_while_aiming", 6.0, {
						{ 0.2, [] { REX::INFO("P3 W5 A2: the aim control is pressed (weapon key 0x{:08X})", Engine::AudioProbe::CurrentWeapon()); Rig::MouseRight(true); } },
						{ 1.0, [] { REX::INFO("P3 W5 A2: key 2 is tapped 0.8 s into the aim"); Engine::WeaponDraw::LogState("W5 A2 pre-switch"); Rig::Tap(Rig::kKey2, 0.12); } },
						{ 2.7, [] { Rig::MouseRight(false); REX::INFO("P3 W5 A2: the aim control is released"); } },
					}, true);
				}
				s.Do([tag] {
					const auto gi = Engine::GraphEvents::GetInfo();
					const auto wi = Engine::WeaponDraw::GetInfo();
					Result(std::format("P3 W5 done [{}]: pose {}, cover {}; the draw work {} call(s), {} give-up(s), faulted {}; graph events seen {}, blocked {}, windows {}, hook faulted {}", tag, CoverPose::ToString(CoverPose::GetInfo().state), Core::ToString(Cover::GetInfo().state), wi.calls, wi.giveUps, wi.faulted, gi.seen, gi.blocked, gi.windows, gi.faulted));
				});
				AddPoseLeaveSneak(s, r);
				s.Wait(2.0);
				if (Config::Get().p3Set >= 75) {
					// outside the pose the block is not on: the game's own switch runs its clip (Equip, bIsEquipping) as before
					AddW5Switch(s, tag + "_open_to_coachman", &Rig::kKey4, 4.0);
					AddW5Switch(s, tag + "_open_to_orion", &Rig::kKey2, 4.0);
				}
				s.Wait(1.0);
				s.Do([] { Engine::AudioProbe::Arm(false, "W5"); g_spot = Spot{}; });
			}
			return Run(s, c.now);
		}
		bool StageW5Tall(const Ctx& c) { return StageW5(c, true); }
		bool StageW5Low(const Ctx& c) { return StageW5(c, false); }

		constexpr std::array<Stage, 6> kSet74{ {
			{ "ready", StageReady, false },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "W5 the equip clip: arrivals of the Orion, tall", StageW5Tall, true },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "W5 the equip clip: arrivals of the Orion, low", StageW5Low, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 6> kSet75{ {
			{ "ready", StageReady, false },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "W5 the equip block: arrivals of the Orion, tall", StageW5Tall, true },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "W5 the equip block: arrivals of the Orion, low, the low A2", StageW5Low, true },
			{ "summary", StageSummary, false },
		} };

		// the smoke of the play build with the scoped weapons read from the file (set 76: set 73 without the pre-taught Orion and without the first-aim stage)
		constexpr std::array<Stage, 11> kSet76{ {
			{ "ready", StageReady, false },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "W4 the Grendel's aim in the low pose", StageW4LowGrendel, true },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "M4f the reloads: two plain, one in the tall pose", StageAudioTall, true },
			{ "M4e the user's sequence at the right end", StageTurnReplay, true },
			{ "M4e the switching at the tall wall (reversals, toes)", StageFootSwitchTall, true },
			{ "W4 smoke: the tall pose", StageW4SmokeTall, true },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "W4 smoke: the low pose", StageW4SmokeLow, true },
			{ "summary", StageSummary, false },
		} };

		// item 2 of the turn (the scoped weapons kept): the Orion is marked and written to the cache file, the file is read, the memory is emptied and the file is read again, as a start does
		bool StageW5FileProof(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				s.Do([] {
					Rig::ReleaseEverything("w5 file");
					REX::INFO("P3 W5 FILE: the Orion is marked scoped and goes into the cache file (known before: {})", Engine::ScopeAim::KnownScoped(0x002773C8u) ? "yes" : "no");
					Engine::ScopeAim::LearnForRig(0x002773C8u, true);
				});
				s.Wait(1.0);
				s.Do([] {
					int         n = 0;
					std::string lines;
					std::ifstream f(Config::Directory() / Core::ReloadSoundFileName(Config::BaseName()));
					std::string   line;
					while (f && std::getline(f, line)) {
						if (line.rfind("s ", 0) == 0) {
							++n;
							lines += " [" + line + "]";
						}
					}
					Result(std::format("P3 W5 FILE: the cache file holds {} 's' line(s):{}", n, lines));
					Summarise("W5 file: the Orion is in the cache file", n >= 1);
					Engine::ScopeAim::Forget();
					Engine::AudioProbe::DebugForget();  // the memory emptied, the file read again: what a start does
				});
				s.Wait(0.5);
				s.Do([] {
					const bool known = Engine::ScopeAim::KnownScoped(0x002773C8u);
					Result(std::format("P3 W5 FILE: after the memory was emptied and the file read again the Orion is {} (nothing aimed)", known ? "KNOWN as scoped" : "NOT known"));
					Summarise("W5 file: the Orion is known from the file with nothing aimed", known);
				});
			}
			return Run(s, c.now);
		}

		// set 77, the last launch of the turn: the file proof, the smoke of the play build (Grendel low aim, reload sounds, reversals, switches, locked Orion aim), then the equip block's twelve arrivals per height and the low A2
		constexpr std::array<Stage, 15> kSet77{ {
			{ "ready", StageReady, false },
			{ "W5 the scoped weapons kept: file proof", StageW5FileProof, false },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "W4 the Grendel's aim in the low pose", StageW4LowGrendel, true },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "M4f the reloads: two plain, one in the tall pose", StageAudioTall, true },
			{ "M4e the user's sequence at the right end", StageTurnReplay, true },
			{ "M4e the switching at the tall wall (reversals, toes)", StageFootSwitchTall, true },
			{ "W4 smoke: the tall pose", StageW4SmokeTall, true },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "W4 smoke: the low pose", StageW4SmokeLow, true },
			{ "W5 the equip block: arrivals of the Orion, tall", StageW5Tall, true },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "W5 the equip block: arrivals of the Orion, low, the low A2", StageW5Low, true },
			{ "summary", StageSummary, false },
		} };

		// the switch to the Grendel by the key that gave it back (item 4 of W6); skipped when none did
		void AddW6Grendel(Seq& s, const std::string& a_label, const double a_secs)
		{
			AddW4Timed(s, a_label, a_secs, {
				{ 0.5, [a_label] {
					if (!g_w6GKey) {
						return;
					}
					REX::INFO("P3 W5 KEY [{}]: the favourite key {} is tapped (weapon key 0x{:08X}, pose {}, ws {})", a_label, g_w6GName, Engine::AudioProbe::CurrentWeapon(), CoverPose::ToString(CoverPose::GetInfo().state), CoverPose::WeaponState());
					g_w6TapT = Now();
					Rig::Tap(*g_w6GKey, 0.12);
				} },
			}, false, [] { return g_w6GKey == nullptr; });
		}

		// W6, launch 1 (set 78): the call stacks of the engine's Equip (the probe's own log lines), the children of the weapon nodes around the key change, the draw call's timing, the Grendel's key
		bool StageW6(const Ctx& c, const bool a_tall)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				g_tall = a_tall;
				g_autoTall = a_tall;
				g_spot = a_tall ? g_leftSpot : Spot{};
				const std::string tag = a_tall ? "T" : "L";
				s.Do([a_tall] {
					Rig::ReleaseEverything("w6");
					Cover::ForceReset("w6");
					Config::Mutable().coverPoses = true;
					Config::Mutable().poseTall = a_tall;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					Config::Mutable().autoCoverTall = a_tall ? 2 : 0;
					Config::Mutable().poseCornerHold = 1;
					Config::Mutable().poseWeaponSwap = 3;
					Config::Mutable().poseScopeAim = 2;
					Config::Mutable().poseWeaponDrawDelay = 0.10f;
					g_w6On = true;
					REX::INFO("P3 W6: the {} pose; iPoseWeaponSwap {}, iPoseScopeAim {}, iGraphEventProbe {}, iPoseEquipBlock {}; the weapon key at the start 0x{:08X}", a_tall ? "tall" : "low", Config::Get().poseWeaponSwap, Config::Get().poseScopeAim, Config::Get().graphEventProbe, Config::Get().poseEquipBlock, Engine::AudioProbe::CurrentWeapon());
				});
				AddWeaponTo(s, true);
				if (a_tall) {
					s.Do([] { FloorBegin(g_spot.x, g_spot.y, Snap().position.z); });
					s.Until(FloorPoll, 3.0);
					s.Do([] {
						if (s_floor.have && std::fabs(s_floor.z - Snap().position.z) < 1.5f) {
							const auto& p = Snap().position;
							MoveBegin({ g_spot.x, g_spot.y, s_floor.z }, (std::max)(1.0, static_cast<double>(std::hypot(g_spot.x - p.x, g_spot.y - p.y) / 3.5f)));
						}
					});
					s.Until(MoveStep, 15.0);
				}
				s.Wait(1.0);
				s.Do([] { W1Dump("W6"); });
				if (a_tall) {
					// item 1 reference in the open, then item 4's keys (the weapon is the Orion after the second switch)
					AddW5Switch(s, "T_open_to_coachman", &Rig::kKey4, 3.5);
					AddW5Switch(s, "T_open_to_orion", &Rig::kKey2, 3.5);
					for (const auto& [name, key] : { std::pair<const char*, const Rig::Key*>{ "0 (slot 10)", &Rig::kKey0 }, { "- (slot 11)", &Rig::kKeyMinus }, { "= (slot 12)", &Rig::kKeyEquals } }) {
						const std::string nm = name;
						const Rig::Key    k = *key;
						const Rig::Key*   kp = key;
						s.Do([nm, k] { REX::INFO("P3 W6 KEYPROBE: the key {} is tapped in the open (weapon key before 0x{:08X})", nm, Engine::AudioProbe::CurrentWeapon()); Rig::Tap(k, 0.12); });
						s.Wait(3.0);
						s.Do([nm, kp] {
							const auto now = Engine::AudioProbe::CurrentWeapon();
							Result(std::format("P3 W6 KEYPROBE: after the key {}: weapon key 0x{:08X}{}", nm, now, now == 0x00028A02u ? "  <== THE GRENDEL" : ""));
							if (now == 0x00028A02u && !g_w6GKey) {
								g_w6GKey = kp;
								g_w6GName = kp == &Rig::kKey0 ? "0" : (kp == &Rig::kKeyMinus ? "-" : "=");
							}
						});
					}
					s.Do([] { Result(std::format("P3 W6 KEYPROBE: the Grendel's key: {}", g_w6GKey ? g_w6GName : "none of 0, -, = gave 0x00028A02 back")); Rig::Tap(Rig::kKey2, 0.12); });
					s.Wait(3.0);
				}
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.5);
				// items 1 and 2: Coachman to Orion four times, Cutter to Orion four times (the probe's stacks and the children's log)
				AddW5Arrivals(s, tag, 4, 3.5);
				// item 3: the draw call's timing: Coachman to Orion six times with the delay 0.10, 0.35, and with no draw call (iPoseWeaponSwap 2)
				for (const char round : { 'A', 'B', 'C' }) {
					s.Do([round, tag] {
						Config::Mutable().poseWeaponSwap = round == 'C' ? 2 : 3;
						Config::Mutable().poseWeaponDrawDelay = round == 'B' ? 0.35f : 0.10f;
						REX::INFO("P3 W6 ROUND {} [{}]: iPoseWeaponSwap {}, fPoseWeaponDrawDelay {:.2f} ({})", round, tag, Config::Get().poseWeaponSwap, Config::Get().poseWeaponDrawDelay, round == 'A' ? "as now" : (round == 'B' ? "the call at about +0.4 s" : "no draw call"));
					});
					for (int i = 1; i <= 6; ++i) {
						AddW5Switch(s, std::format("{}_R{}{}_to_coachman", tag, round, i), &Rig::kKey4, 3.5);
						AddW5Switch(s, std::format("{}_R{}{}_to_orion", tag, round, i), &Rig::kKey2, 3.5);
					}
				}
				s.Do([] { Config::Mutable().poseWeaponSwap = 3; Config::Mutable().poseWeaponDrawDelay = 0.10f; });
				// item 4 in the pose (only when a key gave the Grendel back)
				for (int i = 1; i <= 6; ++i) {
					AddW5Switch(s, std::format("{}_G{}_to_coachman", tag, i), &Rig::kKey4, 3.5, [] { return g_w6GKey == nullptr; });
					AddW6Grendel(s, std::format("{}_G{}_to_grendel", tag, i), 3.5);
				}
				for (int i = 1; i <= 6; ++i) {
					AddW5Switch(s, std::format("{}_H{}_to_cutter", tag, i), &Rig::kKey3, 3.5, [] { return g_w6GKey == nullptr; });
					AddW6Grendel(s, std::format("{}_H{}_to_grendel", tag, i), 3.5);
				}
				for (int i = 1; i <= 3; ++i) {
					AddW5Switch(s, std::format("{}_J{}_to_orion", tag, i), &Rig::kKey2, 3.5, [] { return g_w6GKey == nullptr; });
					AddW6Grendel(s, std::format("{}_J{}_to_grendel", tag, i), 3.5);
					AddW5Switch(s, std::format("{}_J{}_back_to_orion", tag, i), &Rig::kKey2, 3.5, [] { return g_w6GKey == nullptr; });
				}
				s.Do([tag] {
					const auto gi = Engine::GraphEvents::GetInfo();
					const auto wi = Engine::WeaponDraw::GetInfo();
					Result(std::format("P3 W6 done [{}]: pose {}, cover {}; the draw work {} call(s), {} give-up(s), faulted {}; graph events seen {}, windows {}, hook faulted {}", tag, CoverPose::ToString(CoverPose::GetInfo().state), Core::ToString(Cover::GetInfo().state), wi.calls, wi.giveUps, wi.faulted, gi.seen, gi.windows, gi.faulted));
					g_w6On = false;
					Config::Mutable().poseWeaponSwap = 3;
					Config::Mutable().poseWeaponDrawDelay = 0.10f;
				});
				AddPoseLeaveSneak(s, r);
				s.Wait(1.0);
				s.Do([] { g_spot = Spot{}; });
			}
			return Run(s, c.now);
		}
		bool StageW6Tall(const Ctx& c) { return StageW6(c, true); }
		bool StageW6Low(const Ctx& c) { return StageW6(c, false); }

		constexpr std::array<Stage, 6> kSet78{ {
			{ "ready", StageReady, false },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "W6 the late draw: stacks, children, timing, the Grendel's key, tall", StageW6Tall, true },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "W6 the late draw: stacks, children, timing, the Grendel's key, low", StageW6Low, true },
			{ "summary", StageSummary, false },
		} };

		// W7 (REFERENCE 23.25): the pending weapon animation. Part 1: the fields around three Coachman and three Orion arrivals per height (iEquipFieldProbe 1, no write). Part 2: iPoseEquipPending 1,
		// twelve Orion arrivals per height (six from the Coachman, six from the Cutter), a switch in the open, the cover again, and in the low round the A2.
		bool StageW7(const Ctx& c, const bool a_tall)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto r = std::make_shared<PoseRun>();
				g_tall = a_tall;
				g_autoTall = a_tall;
				g_spot = a_tall ? g_leftSpot : Spot{};
				const std::string tag = a_tall ? "T" : "L";
				s.Do([a_tall] {
					Rig::ReleaseEverything("w7");
					Cover::ForceReset("w7");
					Config::Mutable().coverPoses = true;
					Config::Mutable().poseTall = a_tall;
					Config::Mutable().coverCamera = true;
					Config::Mutable().coverTrigger = 3;
					Config::Mutable().autoCoverTall = a_tall ? 2 : 0;
					Config::Mutable().poseCornerHold = 1;
					Config::Mutable().poseWeaponSwap = 3;
					Config::Mutable().poseScopeAim = 2;
					Config::Mutable().poseEquipPending = 0;
					REX::INFO("P3 W7: the {} pose; iEquipFieldProbe {}, iPoseEquipPending {}, iGraphEventProbe {}; the weapon key at the start 0x{:08X}", a_tall ? "tall" : "low", Config::Get().equipFieldProbe, Config::Get().poseEquipPending, Config::Get().graphEventProbe, Engine::AudioProbe::CurrentWeapon());
				});
				AddWeaponTo(s, true);
				if (a_tall) {
					s.Do([] { FloorBegin(g_spot.x, g_spot.y, Snap().position.z); });
					s.Until(FloorPoll, 3.0);
					s.Do([] {
						if (s_floor.have && std::fabs(s_floor.z - Snap().position.z) < 1.5f) {
							const auto& p = Snap().position;
							MoveBegin({ g_spot.x, g_spot.y, s_floor.z }, (std::max)(1.0, static_cast<double>(std::hypot(g_spot.x - p.x, g_spot.y - p.y) / 3.5f)));
						}
					});
					s.Until(MoveStep, 15.0);
				}
				s.Wait(1.0);
				if (a_tall) {
					AddW5Switch(s, "T_open_to_coachman", &Rig::kKey4, 3.5);
					AddW5Switch(s, "T_open_to_orion", &Rig::kKey2, 3.5);
				}
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.5);
				// part 1: the observation
				for (int i = 1; i <= 3; ++i) {
					AddW5Switch(s, std::format("{}_O{}_to_coachman", tag, i), &Rig::kKey4, 3.5);
					AddW5Switch(s, std::format("{}_O{}_to_orion", tag, i), &Rig::kKey2, 3.5);
				}
				// part 2: the remedy
				s.Do([tag] { Config::Mutable().poseEquipPending = 1; REX::INFO("P3 W7 PART 2 [{}]: iPoseEquipPending 1: twelve Orion arrivals", tag); });
				AddW5Arrivals(s, tag + "P", 6, 3.5);
				s.Do([tag] { Result(std::format("P3 W7 [{}]: after the twelve arrivals: pose {}, cover {}, weapon key 0x{:08X}; EQUIP PENDING writes {}, reads of 1 {}", tag, CoverPose::ToString(CoverPose::GetInfo().state), Core::ToString(Cover::GetInfo().state), Engine::AudioProbe::CurrentWeapon(), Engine::EquipPending::GetInfo().writes, Engine::EquipPending::GetInfo().readsOne)); });
				AddPoseLeaveSneak(s, r);
				s.Wait(2.0);
				// the open: one switch must run as the game's own (the clip, the right weapon in the hands: nothing owed from the pose is paid late)
				AddW5Switch(s, tag + "_open_to_coachman_after", &Rig::kKey4, 4.0);
				s.Do([tag] { REX::INFO("P3 W7 PICTURE [{}_open_after] the Coachman in the hands in the open (weapon key 0x{:08X}, HUD)", tag, Engine::AudioProbe::CurrentWeapon()); Engine::WeaponDraw::LogState("W7 open after the pose, the Coachman"); });
				AddW5Switch(s, tag + "_open_to_orion_after", &Rig::kKey2, 4.0);
				s.Do([tag] { REX::INFO("P3 W7 PICTURE [{}_open_after] the Orion in the hands in the open (weapon key 0x{:08X}, HUD)", tag, Engine::AudioProbe::CurrentWeapon()); Engine::WeaponDraw::LogState("W7 open after the pose, the Orion"); });
				// the cover again
				AddPoseEnter(s, r, 0, true);
				s.Wait(1.5);
				s.Do([tag] { REX::INFO("P3 W7 [{}] the cover again: pose {}, cover {}, weapon key 0x{:08X}", tag, CoverPose::ToString(CoverPose::GetInfo().state), Core::ToString(Cover::GetInfo().state), Engine::AudioProbe::CurrentWeapon()); });
				if (!a_tall) {
					// the low A2 with the remedy on: the Coachman in the hands, the aim held, key 2 (the Orion) 0.8 s into it, the control released at 2.7 s
					AddW5Switch(s, "L_A2_to_coachman", &Rig::kKey4, 4.0);
					AddW4Timed(s, "L_A2_switch_to_scoped_while_aiming", 6.0, {
						{ 0.2, [] { REX::INFO("P3 W7 A2: the aim control is pressed (weapon key 0x{:08X})", Engine::AudioProbe::CurrentWeapon()); Rig::MouseRight(true); } },
						{ 1.0, [] { REX::INFO("P3 W7 A2: key 2 is tapped 0.8 s into the aim"); Engine::WeaponDraw::LogState("W7 A2 pre-switch"); Rig::Tap(Rig::kKey2, 0.12); } },
						{ 2.7, [] { Rig::MouseRight(false); REX::INFO("P3 W7 A2: the aim control is released"); } },
					}, true);
				}
				s.Do([tag] {
					const auto gi = Engine::GraphEvents::GetInfo();
					const auto wi = Engine::WeaponDraw::GetInfo();
					const auto ep = Engine::EquipPending::GetInfo();
					Result(std::format("P3 W7 done [{}]: pose {}, cover {}; the draw work {} call(s), {} give-up(s), faulted {}; EQUIP PENDING writes {}, reads of 1 {}, faulted {}; graph events seen {}, hook faulted {}", tag, CoverPose::ToString(CoverPose::GetInfo().state), Core::ToString(Cover::GetInfo().state), wi.calls, wi.giveUps, wi.faulted, ep.writes, ep.readsOne, ep.faulted, gi.seen, gi.faulted));
					Config::Mutable().poseEquipPending = 0;
				});
				AddPoseLeaveSneak(s, r);
				s.Wait(1.0);
				s.Do([] { g_spot = Spot{}; });
			}
			return Run(s, c.now);
		}
		bool StageW7Tall(const Ctx& c) { return StageW7(c, true); }
		bool StageW7Low(const Ctx& c) { return StageW7(c, false); }

		constexpr std::array<Stage, 6> kSet79{ {
			{ "ready", StageReady, false },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "W7 the pending weapon animation, tall", StageW7Tall, true },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "W7 the pending weapon animation, low, the low A2", StageW7Low, true },
			{ "summary", StageSummary, false },
		} };

		// ---- W8 (REFERENCE 23.29): the chain on slot 12 beside Dynamic Dialogue Camera (23.27) and 360 Directional Movement (23.28) -------------------------------------------------------------
		// set 80: the chain (the module report, the framing and the shoulder swap, the tall and the low pose); set 81: 360 at the user's low wall (the cases of 23.28) and the tall wall; set 82: the chain report, the framing and the 360 cases
		bool StageW8Chain(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				s.Do([] {
					const auto has = [](const wchar_t* a_name) { return REX::W32::GetModuleHandleW(a_name) != nullptr; };
					const auto st = Camera::GetStats();
					const bool ddc = has(L"DDC.dll"), dymc = has(L"DyMC.dll"), s360 = has(L"sf360.dll");
					const bool ok = st.installed && !st.faulted && (dymc ? st.compat : !st.compat) && (ddc && !dymc ? st.chained : !st.chained);
					Result(std::format("P3 W8 chain: modules loaded: DDC.dll {}, sf360.dll {}, DyMC.dll {}; the camera: installed {}, compat {}, chained {}, call-site hook {}, faulted {}; third-person calls so far {} -> {}", ddc, s360, dymc, st.installed, st.compat, st.chained, st.siteInstalled, st.faulted, st.callsThird, Verdict(ok)));
					Summarise("W8 chain report", ok);
				});
			}
			return Run(s, c.now);
		}

		// the framing in the open and the shoulder swap (key X): explore, drawn, drawn after the swap, the swap back
		bool StageW8Frame(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				const auto& cfg = Config::Get();
				s.Do([] { Rig::ReleaseEverything("w8 frame"); Cover::ForceReset("w8 frame"); REX::INFO("P3 W8 FRAME: the own camera {}; the shoulder now {}", Camera::Compat() ? "is OFF (compatibility mode)" : "is on", Camera::GetFraming().sideTarget > 0 ? "right" : "left"); });
				AddWeaponTo(s, false);
				s.Wait(1.0);
				AddM4a(s, "w8_explore_right", 0, 1, cfg.frameDistExplore);
				AddWeaponTo(s, true);
				AddM4a(s, "w8_drawn_right", 1, 1, cfg.frameDistDrawn);
				s.Do([] { Rig::Tap(Rig::kKeyX, 0.08); });
				s.Wait(0.8);
				AddM4a(s, "w8_drawn_left", 1, -1, cfg.frameDistDrawn);
				s.Do([] { Rig::Tap(Rig::kKeyX, 0.08); });
				s.Wait(0.8);
				AddM4a(s, "w8_drawn_right_again", 1, 1, cfg.frameDistDrawn);
				AddWeaponTo(s, false);
				s.Wait(0.5);
			}
			return Run(s, c.now);
		}

		struct W8Run
		{
			double tPush{ 0.0 }, tState{ -1.0 }, tVar{ -1.0 }, tPosed{ -1.0 }, nextLog{ 0.0 };
			int    sessions0{ 0 };
			float  maxOff{ 0.0f };   // the largest |body angle - view yaw| in degrees between the push and the pose
			float  bodyState{ 0.0f }, viewState{ 0.0f }, wallState{ 0.0f }, bodyPosed{ 0.0f }, wallPosed{ 0.0f };
			bool   haveState{ false };
		};

		// one log line: the body's angle z against the view yaw and against the cover's facing, and the variables that 360's gate reads
		void W8Line(const char* a_label, const W8Run& f, float* a_body = nullptr, float* a_view = nullptr, float* a_wall = nullptr)
		{
			float yaw{ 0.0f };
			const bool yawOk = Camera::ReadViewYaw(yaw);
			const float body = Snap().angle.z * kRad2Deg;
			const auto  ci = Cover::GetInfo();
			float       wall = std::numeric_limits<float>::quiet_NaN();
			if (ci.candidate.kind != Core::CoverKind::None) {
				wall = std::atan2(-ci.candidate.normal.x, -ci.candidate.normal.y) * kRad2Deg;
			}
			const float view = yawOk ? yaw * kRad2Deg : std::numeric_limits<float>::quiet_NaN();
			if (a_body) { *a_body = body; }
			if (a_view) { *a_view = view; }
			if (a_wall) { *a_wall = wall; }
			REX::INFO("P3 W8 {} +{:.2f}: body {:.1f} deg, view {:.1f}, body-view {:+.1f}, the cover's facing {:.1f}, body-facing {:+.1f}; weapon state {}, iSyncRelaxReadySneak {:g}, iSyncIdleLocomotion {:g}, Speed {:g}, Direction {:g}, iSyncActorStance {:g}, cover {}, pose {}, bIsInCover {:g}",
				a_label, Now() - f.tPush, body, view, WrapDeg(body - view), wall, WrapDeg(body - wall), CoverPose::WeaponState(), VarOr("iSyncRelaxReadySneak", -9.0), VarOr("iSyncIdleLocomotion", -9.0), VarOr("Speed", -9.0), VarOr("Direction", -9.0),
				VarOr("iSyncActorStance", -9.0), Core::ToString(ci.state), CoverPose::ToString(CoverPose::GetInfo().state), VarOr("bIsInCover", -9.0));
		}

		// one entry into the user's low wall by walking into it: straight (W) or sideways (the view turned 90 degrees, D: the world direction of the input is the same), weapon sheathed or drawn
		void AddW8Case(Seq& s, const std::shared_ptr<int>& a_n, const std::shared_ptr<int>& a_ok, const std::string& a_name, const bool a_sideways, const bool a_drawn)
		{
			auto f = std::make_shared<W8Run>();
			auto r = std::make_shared<PoseRun>();
			AddWeaponTo(s, a_drawn);
			AddLowPrepare(s, 0);
			if (a_sideways) {
				s.Do([] { s_align = Align{}; });
				s.Until([] { return s_lowSpot < 0 || AlignStep(WrapDeg(LowSpots()[static_cast<std::size_t>(s_lowSpot)].heading - 90.0f)); }, 6.0);
				s.Wait(0.6);
			}
			s.Do([f, a_name, a_sideways] {
				*f = W8Run{};
				Config::Mutable().coverPoses = true;
				Config::Mutable().coverCamera = true;
				Config::Mutable().coverTrigger = 3;
				Cover::ForceReset("p3 W8 case");
				f->sessions0 = CoverPose::GetInfo().sessions;
				f->tPush = Now();
				f->nextLog = Now();
				W8Line(("[" + a_name + "] before the push").c_str(), *f);
				Rig::Tap(a_sideways ? Rig::kKeyD : Rig::kKeyW, 9.0);
			});
			s.Until([f, a_name] {
				const double now = Now();
				float        body{ 0.0f }, view{ 0.0f }, wall{ 0.0f };
				if (now >= f->nextLog) {
					f->nextLog = now + 0.1;
					W8Line(("[" + a_name + "] walk").c_str(), *f, &body, &view, &wall);
					if (std::isfinite(view)) {
						f->maxOff = (std::max)(f->maxOff, std::fabs(WrapDeg(body - view)));
					}
				}
				if (f->tState < 0.0 && Cover::GetInfo().state != Core::CoverState::None) {
					f->tState = now;
					W8Line(("[" + a_name + "] the cover decision").c_str(), *f, &f->bodyState, &f->viewState, &f->wallState);
					f->haveState = true;
				}
				if (f->tVar < 0.0 && VarOr("bIsInCover", 0.0) > 0.5 && CoverPose::GetInfo().sessions > f->sessions0) {
					f->tVar = now;
				}
				const auto pi = CoverPose::GetInfo();
				if (f->tPosed < 0.0 && pi.state == CoverPose::State::Posed && pi.sessions > f->sessions0) {
					f->tPosed = now;
					float view2{ 0.0f };
					W8Line(("[" + a_name + "] posed").c_str(), *f, &f->bodyPosed, &view2, &f->wallPosed);
				}
				return f->tPosed > 0.0 && f->tVar > 0.0;
			}, 8.0);
			s.Do([] { Rig::ReleaseEverything("p3 W8 case"); });
			s.Wait(0.6);
			s.Do([f, a_name, a_n, a_ok, a_drawn, a_sideways] {
				W8Line(("[" + a_name + "] 0.6 s after the pose").c_str(), *f);
				const bool ok = f->tVar > 0.0 && f->tPosed > 0.0;
				Result(std::format("P3 W8 #{} [{}] ({}, {}): the cover decision {:.3f} s after the push, bIsInCover 1 at {:.3f} s, Posed at {:.3f} s; the largest |body - view| between the push and the pose {:.1f} deg; at the decision body {:.1f} deg (facing the wall {:.1f}, difference {:+.1f}); at Posed body {:.1f} (facing {:.1f}, difference {:+.1f}); pose {} -> {}",
					*a_n + 1, a_name, a_drawn ? "weapon drawn" : "weapon sheathed", a_sideways ? "sideways input" : "straight at the wall", f->tState > 0.0 ? f->tState - f->tPush : -1.0, f->tVar > 0.0 ? f->tVar - f->tPush : -1.0, f->tPosed > 0.0 ? f->tPosed - f->tPush : -1.0, f->maxOff,
					f->bodyState, f->wallState, WrapDeg(f->bodyState - f->wallState), f->bodyPosed, f->wallPosed, WrapDeg(f->bodyPosed - f->wallPosed), CoverPose::ToString(CoverPose::GetInfo().state), Verdict(ok)));
				++(*a_n);
				if (ok) {
					++(*a_ok);
				}
			});
			AddPoseLeaveSneak(s, r);
			s.Wait(0.8);
		}

		// the cases of REFERENCE 23.28 at the user's low wall: with 360 on (its default), then F11 and the same with it off (the log of 360 says "toggled off")
		bool StageW8Three60(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto n = std::make_shared<int>(0);
				auto ok = std::make_shared<int>(0);
				s.Do([] {
					const bool has = REX::W32::GetModuleHandleW(L"sf360.dll") != nullptr;
					REX::INFO("P3 W8 360: sf360.dll is {} in this process; the cases run with it on (its default), then F11 is tapped and they run again", has ? "LOADED" : "NOT loaded (the two blocks are then the same)");
				});
				for (int block = 0; block < 2; ++block) {
					const std::string tag = block == 0 ? "on" : "off";
					if (block == 1) {
						s.Do([] {
							if (REX::W32::GetModuleHandleW(L"sf360.dll") != nullptr) {
								REX::INFO("P3 W8 360: F11 is tapped now (360's switch)");
								Rig::Tap(Rig::kKeyF11, 0.12);
							}
						});
						s.Wait(0.6);
					}
					AddW8Case(s, n, ok, tag + "_straight_sheathed_1", false, false);
					AddW8Case(s, n, ok, tag + "_sideways_sheathed_1", true, false);
					AddW8Case(s, n, ok, tag + "_straight_sheathed_2", false, false);
					AddW8Case(s, n, ok, tag + "_sideways_sheathed_2", true, false);
					AddW8Case(s, n, ok, tag + "_straight_drawn", false, true);
					AddW8Case(s, n, ok, tag + "_sideways_drawn", true, true);
				}
				s.Do([n, ok] {
					const bool good = *ok == *n && *n == 12;
					Result(std::format("P3 W8 360 summary: {} of {} entries reached the pose -> {}", *ok, *n, Verdict(good)));
					Summarise("W8 360 low wall cases", good);
				});
			}
			return Run(s, c.now);
		}

		// ---- W9 (REFERENCE 23.32): where the user's save puts the player, and the entries into the user's low wall -------------------------------------------------
		bool StageW9Place(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				s.Do([] {
					const auto& p = Snap().position;
					unsigned long code{ 0 };
					int           interior{ -1 };
					(void)Guarded([&] {
						const auto pl = RE::PlayerCharacter::GetSingleton();
						const auto cell = pl ? pl->parentCell : nullptr;
						interior = cell ? (cell->IsInterior() ? 1 : 0) : -1;
					}, code);
					const auto has = [](const wchar_t* a_name) { return REX::W32::GetModuleHandleW(a_name) != nullptr; };
					Result(std::format("P3 W9 place: the player stands at ({:.2f},{:.2f},{:.2f}), the cell is {}; modules: DDC.dll {}, sf360.dll {}, DyMC.dll {}; iPoseWaitByte {}, iPoseEntryProbe {}, fPoseWaitByteMax {:.2f}; pending byte {}",
						p.x, p.y, p.z, interior == 1 ? "an INTERIOR" : (interior == 0 ? "an exterior" : "unreadable"), has(L"DDC.dll"), has(L"sf360.dll"), has(L"DyMC.dll"), Config::Get().poseWaitByte, Config::Get().poseEntryProbe, Config::Get().poseWaitByteMax,
						CoverPose::PendingByte()));
					Summarise("W9 place", true);
				});
			}
			return Run(s, c.now);
		}

		// 23.35: set 86/88 stand where the save puts the player and walk blind into whatever is ahead (the start is kept and returned to before every entry); set 87 holds the first stance call back
		struct W10Start { RE::NiPoint3 pos{}; float headingDeg{ 0.0f }; bool have{ false }; };
		W10Start g_w10Start;
		bool     g_w9Blind{ false };

		struct W9Case
		{
			double tPush{ -1.0 }, tPosed{ -1.0 };
			int    sessions0{ 0 };
		};
		void AddW9Case(Seq& s, const std::shared_ptr<int>& a_n, const std::shared_ptr<int>& a_ok, const std::shared_ptr<int>& a_first, const float a_headingOffset = 0.0f)
		{
			auto f = std::make_shared<W9Case>();
			auto r = std::make_shared<PoseRun>();
			AddWeaponTo(s, true);
			if (g_w9Blind) {
				AddPrepare(s, 0, g_w10Start.pos, g_w10Start.headingDeg + a_headingOffset);
			} else {
				AddLowPrepare(s, 0);
			}
			s.Do([f] {
				*f = W9Case{};
				Config::Mutable().coverPoses = true;
				Config::Mutable().coverCamera = true;
				Config::Mutable().coverTrigger = 3;
				Cover::ForceReset("p3 W9 case");
				f->sessions0 = CoverPose::GetInfo().sessions;
				f->tPush = Now();
				Rig::Tap(Rig::kKeyW, 9.0);
			});
			s.Until([f] {
				const auto pi = CoverPose::GetInfo();
				if (f->tPosed < 0.0 && pi.state == CoverPose::State::Posed && pi.sessions > f->sessions0) {
					f->tPosed = Now();
				}
				return f->tPosed > 0.0;
			}, 8.0);
			s.Do([] { Rig::ReleaseEverything("p3 W9 case"); });
			s.Wait(0.4);
			s.Do([f, a_n, a_ok, a_first] {
				const auto pi = CoverPose::GetInfo();
				const bool posed = f->tPosed > 0.0;
				const bool began = pi.sessions > f->sessions0;
				const bool first = posed && pi.stanceCalls == 1;
				Result(std::format("P3 W9 #{}: the pose {} ({} session(s) began), {} stance call(s), Posed {:.3f} s after the push; the byte now {} -> {}", *a_n + 1, posed ? "was reached" : "was NOT reached", pi.sessions - f->sessions0, began ? pi.stanceCalls : 0,
					posed ? f->tPosed - f->tPush : -1.0, CoverPose::PendingByte(), Verdict(posed)));
				++(*a_n);
				if (posed) {
					++(*a_ok);
				}
				if (first) {
					++(*a_first);
				}
			});
			AddPoseLeaveSneak(s, r);
			s.Wait(0.8);
		}

		// ten entries in one launch: five with the wait for the pending byte off, five with it on (the stage sets the switch itself, the INI's value is only the start), so that one run answers both questions of 23.32
		// sets 84 and 85 (23.34): the same blocks in two launches (the user's save is a fight and the player may die within a minute): 84 = the wait off, 85 = the wait on
		bool W9Entries(const Ctx& c, int a_firstBlock, int a_lastBlock)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto allOk = std::make_shared<bool>(true);
				for (int block = a_firstBlock; block <= a_lastBlock; ++block) {
					auto n = std::make_shared<int>(0);
					auto ok = std::make_shared<int>(0);
					auto first = std::make_shared<int>(0);
					s.Do([block] {
						Config::Mutable().poseWaitByte = block;
						REX::INFO("P3 W9: block {} of 2: iPoseWaitByte is now {}", block + 1, block);
					});
					const int count = g_w9Blind ? 8 : 5;  // blind: eight headings, 45 degrees apart, from the start heading (the place's walls are not known)
					for (int i = 0; i < count; ++i) {
						AddW9Case(s, n, ok, first, g_w9Blind ? 45.0f * static_cast<float>(i) : 0.0f);
					}
					s.Do([block, n, ok, first, allOk, count] {
						const bool good = *n == count && *ok > 0;
						*allOk = *allOk && good;
						Result(std::format("P3 W9 block {} (iPoseWaitByte {}): {} of {} entries reached the pose, {} of them on the first stance call -> {} (the data is the log: POSE PROBE lines with iPoseEntryProbe 1)", block + 1, block, *ok, *n, *first, Verdict(good)));
					});
				}
				s.Do([allOk] {
					Config::Mutable().poseWaitByte = 0;
					Summarise("W9 entries", *allOk);
				});
			}
			return Run(s, c.now);
		}
		bool StageW9Entries(const Ctx& c) { return W9Entries(c, 0, 1); }
		bool StageW9EntriesOff(const Ctx& c) { return W9Entries(c, 0, 0); }

		bool StageW10Start(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				s.Do([] {
					g_w10Start.pos = Snap().position;
					float yaw{ 0.0f };
					const bool haveYaw = Camera::ReadViewYaw(yaw);
					g_w10Start.headingDeg = haveYaw ? yaw * kRad2Deg : 0.0f;
					g_w10Start.have = true;
					g_w9Blind = true;
					Result(std::format("P3 W10 start: the entries start at ({:.2f},{:.2f},{:.2f}), view heading {:.1f} deg (read {}); blind: the player walks forward into whatever is ahead", g_w10Start.pos.x, g_w10Start.pos.y, g_w10Start.pos.z, g_w10Start.headingDeg, haveYaw));
					Summarise("W10 start", true);
				});
			}
			return Run(s, c.now);
		}
		bool StageW9EntriesOn(const Ctx& c) { return W9Entries(c, 1, 1); }
		// set 89 (23.36): eight blind entries with the INI values as they are (iPoseWaitByte, iPoseNoRepeat): nothing is switched by the stage
		bool StageW9EntriesIni(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				auto n = std::make_shared<int>(0);
				auto ok = std::make_shared<int>(0);
				auto first = std::make_shared<int>(0);
				s.Do([] { REX::INFO("P3 W9: the INI values: iPoseWaitByte {}, iPoseNoRepeat {}, fPoseWaitByteMax {:.2f}", Config::Get().poseWaitByte, Config::Get().poseNoRepeat, Config::Get().poseWaitByteMax); });
				for (int i = 0; i < 8; ++i) {
					AddW9Case(s, n, ok, first, 45.0f * static_cast<float>(i));
				}
				s.Do([n, ok, first] {
					Result(std::format("P3 W9 INI block (iPoseWaitByte {}, iPoseNoRepeat {}): {} of {} entries reached the pose, {} of them on the first stance call -> {}", Config::Get().poseWaitByte, Config::Get().poseNoRepeat, *ok, *n, *first, Verdict(*ok > 0)));
					Summarise("W9 INI entries", *ok > 0);
				});
			}
			return Run(s, c.now);
		}

		// set 87 (23.35): the first stance call held back for a fixed time after the archetype was cleared (fPoseSwitchWait, set by the stage), 0.10, 0.30, 0.55 s; five tall and five low entries each; then the wait for the byte (iPoseWaitByte 1) with no delay
		void AddW10Tall(Seq& s, const std::shared_ptr<int>& a_n, const std::shared_ptr<int>& a_ok, const std::shared_ptr<int>& a_first)
		{
			auto r = std::make_shared<PoseRun>();
			AddWeaponTo(s, true);
			AddPoseEnter(s, r, 0, true);
			s.Do([r, a_n, a_ok, a_first] {
				const auto pi = CoverPose::GetInfo();
				const bool posed = r->tPosed > 0.0;
				const bool began = pi.sessions > r->sessions0;
				Result(std::format("P3 W10 tall #{}: the pose {} ({} session(s) began), {} stance call(s), Posed {:.3f} s after the push; the byte now {} -> {}", *a_n + 1, posed ? "was reached" : "was NOT reached", pi.sessions - r->sessions0, began ? pi.stanceCalls : 0,
					posed ? r->tPosed - r->tPush : -1.0, CoverPose::PendingByte(), Verdict(posed)));
				++(*a_n);
				if (posed) {
					++(*a_ok);
				}
				if (posed && pi.stanceCalls == 1) {
					++(*a_first);
				}
			});
			AddPoseLeaveSneak(s, r);
			s.Wait(0.8);
		}

		bool StageW10Delays(const Ctx& c)
		{
			static Seq s;
			if (c.first) {
				s = Seq{};
				struct Block { float delay; int wait; };
				constexpr std::array<Block, 4> blocks{ { { 0.10f, 0 }, { 0.30f, 0 }, { 0.55f, 0 }, { 0.0f, 1 } } };
				auto allOk = std::make_shared<bool>(true);
				s.Do([] {
					g_tall = true;
					g_autoTall = true;
					g_spot = g_leftSpot;
					Config::Mutable().coverPoses = true;
					Config::Mutable().poseTall = true;
					Config::Mutable().autoCoverTall = 2;
					Config::Mutable().poseCornerHold = 1;
					REX::INFO("P3 W10: the tall wall's spot ({:.2f},{:.2f},{:.2f}) set {}", g_spot.x, g_spot.y, g_spot.z, g_spot.set);
				});
				for (const auto& blk : blocks) {
					const float delay = blk.delay;
					const int   wait = blk.wait;
					s.Do([delay, wait] {
						Config::Mutable().poseSwitchWait = delay;
						Config::Mutable().poseWaitByte = wait;
						REX::INFO("P3 W10: block: fPoseSwitchWait {:.2f}, iPoseWaitByte {}", delay, wait);
					});
					for (int kind = 0; kind < 2; ++kind) {
						auto n = std::make_shared<int>(0);
						auto ok = std::make_shared<int>(0);
						auto first = std::make_shared<int>(0);
						for (int i = 0; i < 5; ++i) {
							if (kind == 0) {
								AddW10Tall(s, n, ok, first);
							} else {
								AddW9Case(s, n, ok, first);
							}
						}
						s.Do([kind, delay, wait, n, ok, first, allOk] {
							const bool good = *n == 5 && *ok == 5;
							*allOk = *allOk && good;
							Result(std::format("P3 W10 block {} (switch wait {:.2f} s, iPoseWaitByte {}): {} of {} entries reached the pose, {} of them on the first stance call -> {}", kind == 0 ? "TALL" : "LOW", delay, wait, *ok, *n, *first, Verdict(good)));
						});
					}
				}
				s.Do([allOk] {
					Config::Mutable().poseSwitchWait = 0.0f;
					Config::Mutable().poseWaitByte = 0;
					g_tall = false;
					g_autoTall = false;
					g_spot = Spot{};
					Summarise("W10 delays: every entry reached the pose", *allOk);
				});
			}
			return Run(s, c.now);
		}

		constexpr std::array<Stage, 8> kSet80{ {
			{ "ready", StageReady, false },
			{ "W8 the chain report", StageW8Chain, false },
			{ "W8 the framing and the shoulder swap", StageW8Frame, true },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "W4 smoke: the tall pose", StageW4SmokeTall, true },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "W4 smoke: the low pose", StageW4SmokeLow, true },
			{ "summary", StageSummary, false },
		} };
		constexpr std::array<Stage, 8> kSet81{ {
			{ "ready", StageReady, false },
			{ "W8 the chain report", StageW8Chain, false },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "W8 360 at the low wall", StageW8Three60, true },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "M4e the user's sequence at the right end", StageTurnReplay, true },
			{ "W4 smoke: the tall pose", StageW4SmokeTall, true },
			{ "summary", StageSummary, false },
		} };
		constexpr std::array<Stage, 7> kSet82{ {
			{ "ready", StageReady, false },
			{ "W8 the chain report", StageW8Chain, false },
			{ "W8 the framing and the shoulder swap", StageW8Frame, true },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "W8 360 at the low wall", StageW8Three60, true },
			{ "W4 smoke: the low pose", StageW4SmokeLow, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 6> kSet83{ {
			{ "ready", StageReady, false },
			{ "W8 the chain report", StageW8Chain, false },
			{ "W9 the place", StageW9Place, false },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "W9 ten entries into the user's low wall", StageW9Entries, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 6> kSet84{ {
			{ "ready", StageReady, false },
			{ "W8 the chain report", StageW8Chain, false },
			{ "W9 the place", StageW9Place, false },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "W9 five entries, the wait off", StageW9EntriesOff, true },
			{ "summary", StageSummary, false },
		} };
		constexpr std::array<Stage, 6> kSet85{ {
			{ "ready", StageReady, false },
			{ "W8 the chain report", StageW8Chain, false },
			{ "W9 the place", StageW9Place, false },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "W9 five entries, the wait on", StageW9EntriesOn, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 6> kSet86{ {
			{ "ready", StageReady, false },
			{ "W8 the chain report", StageW8Chain, false },
			{ "W9 the place", StageW9Place, false },
			{ "W10 the start", StageW10Start, false },
			{ "W9 five blind entries, the wait off", StageW9EntriesOff, true },
			{ "summary", StageSummary, false },
		} };
		constexpr std::array<Stage, 6> kSet88{ {
			{ "ready", StageReady, false },
			{ "W8 the chain report", StageW8Chain, false },
			{ "W9 the place", StageW9Place, false },
			{ "W10 the start", StageW10Start, false },
			{ "W9 five blind entries, the wait on", StageW9EntriesOn, true },
			{ "summary", StageSummary, false },
		} };
		constexpr std::array<Stage, 6> kSet87{ {
			{ "ready", StageReady, false },
			{ "W8 the chain report", StageW8Chain, false },
			{ "R4 find the user's low wall", StageLowSearch, true },
			{ "M4b the straight tall wall's spot (floor)", StageReverseSetup, true },
			{ "W10 the first stance call held back, 0.10 / 0.30 / 0.55 s, then the wait", StageW10Delays, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 6> kSet89{ {
			{ "ready", StageReady, false },
			{ "W8 the chain report", StageW8Chain, false },
			{ "W9 the place", StageW9Place, false },
			{ "W10 the start", StageW10Start, false },
			{ "W9 eight blind entries, the INI values", StageW9EntriesIni, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 4> kSet43{ {
			{ "ready", StageReady, false },
			{ "M4b go to the tall column (the corner card on)", StageGoTall, true },
			{ "M4b the corner at the right end: three start distances, the sweep, the trajectory, the burst", StageCornerRight, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 4> kSet42{ {
			{ "ready", StageReady, false },
			{ "M4b go to the tall column (the corner spike on)", StageGoTall, true },
			{ "M4b the swing measurement with the corner hold and the pivot following the body", StageSwing, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 10> kSet41{ {
			{ "ready", StageReady, false },
			{ "R4 find the kiosk's low wall", StageLowSearch, true },
			{ "M4b the low pose with the weapon drawn, 3 s idle", StagePoseIn, true },
			{ "M4b the first aim press with a shot (low, over)", StagePoseAim, true },
			{ "M4b the exit after a stay, pulling away (the default remedy), eight times", StageShortStay, true },
			{ "M4b blind fire: a hold and a tap", StageBlindTail, true },
			{ "M4b go to the tall column", StageGoTall, true },
			{ "M4b tall cover with the weapon drawn: the automatic entry and the pose", StageTallSmoke, true },
			{ "M4b the swing measurement", StageSwing, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 4> kSet28{ {
			{ "ready", StageReady, false },
			{ "M3g the tall pose on the billboard column of Save32", StageTallPose, true },
			{ "M4a the entry and exit cycles at the billboard column", StageYaw, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 16> kSet38{ {
			{ "ready", StageReady, false },
			{ "R4 find the low wall from either side", StageLowSearch, true },
			{ "M4a the aim at the low wall's end and 0.5 m from it, the framing on and off", StageEndSpot, true },
			{ "the poses off: the Basic cover", StagePosesOff, false },
			{ "T3.4 spike: a Sneak press made by the plugin, ten times", StageMadeUp, true },
			{ "R4 enter low from standing, leave; enter while sneaking, leave", StageLowEnter, true },
			{ "R5 peek over and side peek", StagePeeks, true },
			{ "R2 staying on the low wall: A, D along it, S away", StageStayLow, true },
			{ "T3.5 INI reload on Numpad 9", StageReload, true },
			{ "the poses off again (the reload read the INI)", StagePosesOff, false },
			{ "T3.5 mode 0: the dedicated key", StageMode0, true },
			{ "T3.5 mode 2: the held Sneak control", StageMode2, true },
			{ "M1b R9 the held Sneak press", StageR9, true },
			{ "M1b R10 mode 3 at the low wall", StageR10Low, true },
			{ "M1c T3.8 R12 B while peeking over", StageR12, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 15> kSet39{ {
			{ "ready", StageReady, false },
			{ "the poses off: the Basic cover", StagePosesOff, false },
			{ "T3.3b probe: the game movement vector with W, A, S, D held in turn", StageVectorProbe, true },
			{ "T3.3b write test: vector (0, m) written after the original", StageWriteTest, true },
			{ "direction check: W, A, S against the computed input direction", StageDirection, true },
			{ "R1 enter high (S held, Sneak tapped), R3 leave by Sneak", StageEnterHighSneak, true },
			{ "R1 Sneak tap with no key held: the vanilla toggle", StageVanillaToggle, true },
			{ "S3 queue layout: W, D and the aim button held together", StageQueueLayout, true },
			{ "R2 staying on cover: A, D along the wall, W away", StageStay, true },
			{ "R3 leave by Jump", StageEnterHighJump, true },
			{ "R3 leave by Sprint", StageEnterHighSprint, true },
			{ "R3 leave by forcing first person", StageEnterHighFirstPerson, true },
			{ "R6 open ground and first person", StageOpenGround, true },
			{ "M1b R10 mode 3 at the tall wall", StageR10Tall, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 4> kSet34{ {
			{ "ready", StageReady, false },
			{ "M4a the own camera on the tall wall of Save29: situations, shoulders, aim, tall pose", StageM4aTall, true },
			{ "M4a the entry and exit cycles at the tall wall", StageYaw, true },
			{ "summary", StageSummary, false },
		} };
		constexpr std::array<Stage, 22> kSet35{ {
			{ "ready", StageReady, false },
			{ "R4 find the low wall from either side", StageLowSearch, true },
			{ "M4a the own camera at the low wall of Save30", StageM4aLow, true },
			{ "M4a the pose with the weapon drawn, 3 s idle", StagePoseIn, true },
			{ "M4a the walk both ways", StagePoseWalk, true },
			{ "M4a the first aim press with a shot", StagePoseAim, true },
			{ "M4a out by pulling away and by Sneak", StagePoseOut, true },
			{ "M4a the locks: reload, quick-keys and the holster hold in the pose", StageLocks, true },
			{ "M4a blind fire: a hold and a tap", StageBlindTail, true },
			{ "M4a the entry and exit cycles at the low wall", StageYaw, true },
			{ "T3.4 spike: a Sneak press made by the plugin, ten times", StageMadeUp, true },
			{ "R4 enter low from standing, leave; enter while sneaking, leave", StageLowEnter, true },
			{ "R5 peek over and side peek", StagePeeks, true },
			{ "R2 staying on the low wall: A, D along it, S away", StageStayLow, true },
			{ "T3.5 INI reload on Numpad 9", StageReload, true },
			{ "T3.5 mode 0: the dedicated key", StageMode0, true },
			{ "T3.5 mode 2: the held Sneak control", StageMode2, true },
			{ "M1b R9 the held Sneak press", StageR9, true },
			{ "M1b R10 mode 3 at the low wall", StageR10Low, true },
			{ "M1c T5.3a R11 the cover camera, off and on", StageR11, true },
			{ "M1c T3.8 R12 B while peeking over", StageR12, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 3> kSet29{ {
			{ "ready", StageReady, false },
			{ "M3h the view yaw, the wall push and the way out on the tall wall of Save29", StageYaw, true },
			{ "summary", StageSummary, false },
		} };
		constexpr std::array<Stage, 3> kSet30{ {
			{ "ready", StageReady, false },
			{ "M3h the view yaw, the wall push and the way out on the low wall of Save30", StageYaw, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 10> kSet32{ {
			{ "ready", StageReady, false },
			{ "R4 find the low wall from either side", StageLowSearch, true },
			{ "M3j the pose with the weapon drawn, 3 s idle", StagePoseIn, true },
			{ "M3j the walk both ways", StagePoseWalk, true },
			{ "M3j the first aim press with a shot", StagePoseAim, true },
			{ "M3j out by pulling away and by Sneak", StagePoseOut, true },
			{ "M3j the locks: reload, quick-keys and the holster hold in the pose", StageLocks, true },
			{ "M3j blind fire: a hold and a tap", StageBlindTail, true },
			{ "M3j the entry and exit cycles at the low wall", StageYaw, true },
			{ "summary", StageSummary, false },
		} };
		constexpr std::array<Stage, 3> kSet33{ {
			{ "ready", StageReady, false },
			{ "M3j the entry dive on the tall wall of Save29 (Stride off)", StageYaw, true },
			{ "summary", StageSummary, false },
		} };

		constexpr std::array<Stage, 3> kSet31{ {
			{ "ready", StageReady, false },
			{ "M3i ten ins and outs without the exit clip on the tall wall of Save29", StageYaw, true },
			{ "summary", StageSummary, false },
		} };

		std::span<const Stage> Stages()
		{
			switch (Config::Get().p3Set) {
			case 89: return kSet89;
			case 88: return kSet88;
			case 87: return kSet87;
			case 86: return kSet86;
			case 85: return kSet85;
			case 84: return kSet84;
			case 83: return kSet83;
			case 82: return kSet82;
			case 81: return kSet81;
			case 80: return kSet80;
			case 79: return kSet79;
			case 78: return kSet78;
			case 77: return kSet77;
			case 76: return kSet76;
			case 75: return kSet75;
			case 74: return kSet74;
			case 73: return kSet73;
			case 72: return kSet72;
			case 71: return kSet71;
			case 70: return kSet70;
			case 69: return kSet69;
			case 68: return kSet68;
			case 66: return kSet66;
			case 65: return kSet65;
			case 64: return kSet64;
			case 63: return kSet63;
			case 62: return kSet62;
			case 61: return kSet61;
			case 60: return kSet60;
			case 59: return kSet59;
			case 58: return kSet58;
			case 57: return kSet53;
			case 56: return kSet56;
			case 55: return kSet51;
			case 54: return kSet54;
			case 53: return kSet53;
			case 52: return kSet52;
			case 51: return kSet51;
			case 50: return kSet50;
			case 49: return kSet49;
			case 48: return kSet48;
			case 47: return kSet47;
			case 46: return kSet46;
			case 45: return kSet45;
			case 44: return kSet44;
			case 43: return kSet43;
			case 42: return kSet42;
			case 41: return kSet41;
			case 40: return kSet40;
			case 39: return kSet39;
			case 38: return kSet38;
			case 37: return kSet37;
			case 36: return kSet36;
			case 35: return kSet35;
			case 34: return kSet34;
			case 33: return kSet33;
			case 32: return kSet32;
			case 31: return kSet31;
			case 29: return kSet29;
			case 30: return kSet30;
			case 27: return kSet27;
			case 28: return kSet28;
			case 26: return kSet26;
			case 25: return kSet25;
			case 24: return kSet24;
			case 23: return kSet23;
			case 21: return kSet21;
			case 22: return kSet22;
			case 20: return kSet20;
			case 18: return kSet18;
			case 19: return kSet19;
			case 16: return kSet16;
			case 17: return kSet17;
			case 15: return kSet15;
			case 13: return kSet13;
			case 14: return kSet14;
			case 10: return kSet10;
			case 11: return kSet11;
			case 12: return kSet12;
			case 8: return kSet8;
			case 9: return kSet9;
			case 6: return kSet6;
			case 7: return kSet7;
			case 5: return kSet5;
			case 4: return kSet4;
			case 2: return kSet2;
			case 3: return kSet3;
			default: return kSet1;
			}
		}

		void Finish(const bool a_ok, const char* a_why)
		{
			s_zTrack = false;
			Rig::ReleaseEverything(a_ok ? "script finished" : "script aborted");
			Cover::ForceReset("p3 script end");
			const auto st = WorldProbe::GetStats();
			REX::INFO("RESULT rig p3: {} ({}) | {} passed, {} failed | world probe: {} submitted, {} executed in the Update hook, broken={}", a_ok ? "all stages ran" : "stopped early", a_why, s_pass, s_fail, st.submitted, st.executed, st.broken);
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
		if (s_zTrack) {
			s_zMax = (std::max)(s_zMax, a_snap.position.z);
		}
		const double now = a_snap.time;
		const auto   stages = Stages();

		if (s_stage > 0) {
			const char* abort = nullptr;
			if (!a_snap.valid || a_snap.paused) {
				abort = "the game paused or left the world";
			} else if (stages[s_stage].usesKeys && !Rig::InForeground() && !s_shotWaiting) {
				// the window can lose the foreground for a moment (another program, a notification): the keys are let go, the stage waits up to 15 s
				if (s_fgLostSince < 0.0) {
					s_fgLostSince = now;
					Rig::ReleaseEverything("the game window lost the foreground");
					REX::WARN("S0 rig: p3: the game window is not in the foreground; waiting for it (up to 15 s)");
				}
				if (now - s_fgLostSince > 15.0) {
					abort = "the game window lost the foreground and did not get it back within 15 s";
				} else {
					return;
				}
			} else if (!Cover::GetInfo().enabled) {
				abort = "the cover feature is not enabled (a fault, or the rewrite hook is not working)";
			}
			if (abort) {
				Finish(false, abort);
				return;
			}
		}
		if (s_fgLostSince >= 0.0 && Rig::InForeground()) {
			REX::INFO("S0 rig: p3: the game window has the foreground again after {:.1f} s", now - s_fgLostSince);
			s_fgLostSince = -1.0;
		}
		if (s_stage >= stages.size()) {
			Finish(true, "end of the sequence");
			return;
		}
		if (!s_started) {
			s_started = true;
			s_stageStart = now;
			s_stageFirst = true;
			REX::INFO("S0 rig: p3 set {} stage 1 of {}: {}", Config::Get().p3Set, stages.size(), stages[0].name);
		}
		const Ctx ctx{ a_snap, now, now - s_stageStart, s_stageFirst };
		s_stageFirst = false;
		if (stages[s_stage].fn(ctx)) {
			++s_stage;
			s_stageStart = now;
			s_stageFirst = true;
			if (s_stage < stages.size()) {
				REX::INFO("S0 rig: p3 stage {} of {}: {}", s_stage + 1, stages.size(), stages[s_stage].name);
			}
		}
	}
}
