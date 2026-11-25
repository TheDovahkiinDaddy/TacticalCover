#include "Engine/P5Script.h"

#include <array>
#include <cfloat>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "Config.h"
#include "Engine/AnimRecon.h"
#include "Engine/Camera.h"
#include "Engine/Frame.h"
#include "Engine/Guard.h"
#include "Engine/Input.h"
#include "Engine/Offsets.h"
#include "Engine/PlayerControl.h"
#include "Engine/Rig.h"
#include "Engine/WorldProbe.h"

namespace Engine::P5
{
	namespace
	{
		constexpr float    kPi = 3.14159265358979f;
		constexpr Rig::Key kKeySpace{ 0x20, 0x39 };
		constexpr float    kMaxMove = 1.0f;  // the stop rule: the player farther than this (horizontally) from where a stage began

		[[nodiscard]] const Frame::Snapshot& Snap() { return Frame::Latest(); }
		[[nodiscard]] double                 Now() { return Frame::Now(); }

		bool                s_done{ false };
		bool                s_started{ false };
		bool                s_faulted{ false };
		bool                s_cleaned{ false };
		double              s_fgLostSince{ -1.0 };
		int                 s_pass{ 0 };
		int                 s_fail{ 0 };
		std::vector<std::string> s_summary;

		void Result(const std::string& a_text) { REX::INFO("RESULT {}", a_text); }

		void Summarise(const std::string& a_name, const bool a_ok)
		{
			(a_ok ? s_pass : s_fail) += 1;
			s_summary.push_back(std::format("{}: {}", a_name, a_ok ? "PASS" : "FAIL"));
		}

		// ---- a tiny sequencer, as in the p3 script ---------------------------------------------------------------------------------
		struct Step
		{
			double                    delay{ 0.0 };
			double                    timeout{ 0.0 };
			std::function<bool()>     cond;
			std::function<void(bool)> done;
			bool                      started{ false };
			double                    readyAt{ 0.0 };
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

		bool Run(Seq& a_seq, const double a_now)
		{
			if (!a_seq.started) {
				a_seq.started = true;
				a_seq.last = a_now;
			}
			while (a_seq.i < a_seq.steps.size()) {
				if (s_done) {
					return false;
				}
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

		Seq s_seq;

		// ---- the stage origin and the stop rule ------------------------------------------------------------------------------------
		RE::NiPoint3 s_origin{};
		bool         s_watch{ false };

		float        s_limit{ kMaxMove };

		void BeginStage(const char* a_what, const float a_limit = kMaxMove)
		{
			s_origin = Snap().position;
			s_watch = true;
			s_limit = a_limit;
			REX::INFO("P5 stage origin [{}]: player ({:.3f},{:.3f},{:.3f}), angle z {:.4f} rad ({:.1f} deg)", a_what, s_origin.x, s_origin.y, s_origin.z, Snap().angle.z, Snap().angle.z * 180.0f / kPi);
		}

		[[nodiscard]] float MovedFromOrigin()
		{
			const auto& p = Snap().position;
			return std::hypot(p.x - s_origin.x, p.y - s_origin.y);
		}

		// ---- the process fields (guarded reads and one guarded write) ---------------------------------------------------------------------
		struct Fields
		{
			bool         ok{ false };
			std::int32_t stance{ -1 }, alert{ -1 }, sighted{ -1 }, blind{ -1 }, gunDown{ -1 }, rightLeft{ -1 }, partial{ -1 }, sightedReq{ -1 }, jump{ -1 };
			float        loc[3]{ 0.0f, 0.0f, 0.0f };
			float        heading{ 0.0f };
			bool         aim{ false };
		};

		[[nodiscard]] const std::byte* MiddleHigh()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return nullptr;
			}
			RE::AIProcess* proc{ nullptr };
			if (!SafeRead(&player->currentProcess, proc) || !proc) {
				return nullptr;
			}
			RE::MiddleHighProcessData* mh{ nullptr };
			if (!SafeRead(&proc->middleHigh, mh) || !mh) {
				return nullptr;
			}
			return reinterpret_cast<const std::byte*>(mh);
		}

		bool ReadFields(Fields& a_f)
		{
			a_f = Fields{};
			const auto b = MiddleHigh();
			if (!b) {
				return false;
			}
			bool ok = SafeRead(b + kMhActorStance, a_f.stance);
			ok = SafeRead(b + kMhRelaxedReady, a_f.alert) && ok;
			ok = SafeRead(b + kMhIsSighted, a_f.sighted) && ok;
			ok = SafeRead(b + kMhIsBlindFiring, a_f.blind) && ok;
			ok = SafeRead(b + kMhIsGunDown, a_f.gunDown) && ok;
			ok = SafeRead(b + kMhRightLeft, a_f.rightLeft) && ok;
			ok = SafeRead(b + kMhPartialCover, a_f.partial) && ok;
			ok = SafeRead(b + kMhDesiredLocation, a_f.loc) && ok;
			ok = SafeRead(b + kMhDesiredHeading, a_f.heading) && ok;
			ok = SafeRead(b + kMhSightedRequested, a_f.sightedReq) && ok;
			ok = SafeRead(b + kMhJumpState, a_f.jump) && ok;
			std::uint8_t aim{ 0 };
			ok = SafeRead(b + kMhAimActive, aim) && ok;
			a_f.aim = aim != 0;
			a_f.ok = ok;
			return ok;
		}

		// the one write of the script: the stance field back to what it was (the engine writes this field directly itself)
		bool WriteStanceField(const std::int32_t a_value)
		{
			const auto b = MiddleHigh();
			if (!b) {
				return false;
			}
			unsigned long code{ 0 };
			const bool    ok = Guarded([&] { *reinterpret_cast<std::int32_t*>(const_cast<std::byte*>(b) + kMhActorStance) = a_value; }, code);
			if (!ok) {
				REX::ERROR("P5: exception 0x{:08X} writing the stance field", code);
			}
			return ok;
		}

		[[nodiscard]] std::string GraphVar(const char* a_name)
		{
			const auto v = PlayerControl::ReadVar(a_name);
			return v ? std::format("{}={:g}", a_name, *v) : std::format("{}=unresolved", a_name);
		}

		void Table(const std::string& a_label)
		{
			Fields f;
			const bool ok = ReadFields(f);
			const auto& p = Snap().position;
			REX::INFO("P5 table [{}]: fields {} stance={} alert={} sighted={} blind={} gunDown={} rightLeft={} partial={} loc=({:.2f},{:.2f},{:.2f}) heading={:.3f} sightedReq={} jump={} aim={} | graph {} {} {} {} {} {} {} {} {} {} {} {} | player ({:.3f},{:.3f},{:.3f}) angle z {:.3f} rad, {:.2f} m from the stage origin",
				a_label, ok ? "ok" : "UNREADABLE", f.stance, f.alert, f.sighted, f.blind, f.gunDown, f.rightLeft, f.partial, f.loc[0], f.loc[1], f.loc[2], f.heading, f.sightedReq, f.jump, f.aim,
				GraphVar("bIsInCover"), GraphVar("iSyncActorStance"), GraphVar("iSyncStandingCrouching"), GraphVar("bIsEnteringCover"), GraphVar("bIsExitingCover"), GraphVar("bIsShuffling"), GraphVar("iSyncSighted"),
				GraphVar("iSyncWeaponDrawState"), GraphVar("iSyncCoverStates"), GraphVar("iSyncIdleLocomotion"), GraphVar("Direction"), GraphVar("Speed"), p.x, p.y, p.z, Snap().angle.z, MovedFromOrigin());
		}

		// ---- the engine's stance function --------------------------------------------------------------------------------------------
		[[nodiscard]] bool StanceFnVerified()
		{
			static int s_checked = 0;
			if (s_checked == 0) {
				const auto base = REX::FModule::GetExecutingModule().GetBaseAddress();
				const auto addr = kActorChangeStance.address();
				const auto rva = addr - base;
				std::array<std::uint8_t, 24> bytes{};
				const bool readable = SafeCopy(bytes.data(), reinterpret_cast<const void*>(addr), bytes.size()) == bytes.size();
				const bool ok = rva == kActorChangeStanceRva && readable && bytes == kActorChangeStanceBytes;
				s_checked = ok ? 1 : -1;
				REX::INFO("P5: the stance function, ID {} resolves to RVA 0x{:X} (expected 0x{:X}), the first 24 bytes {} -> {}", kActorChangeStance.id(), rva, kActorChangeStanceRva, readable ? (bytes == kActorChangeStanceBytes ? "match" : "DIFFER") : "UNREADABLE",
					ok ? "OK, the function may be called" : "REFUSED, the function is not called");
			}
			return s_checked == 1;
		}

		bool s_lastRet{ false };
		bool s_lastRan{ false };

		// a_heading: a float in the unit and sense of actor angle z (radians, 0 = +y, growing toward +x); -FLT_MAX means "as the actor faces". Returns false when the function did not run.
		bool CallStance(const int a_stance, const bool a_side, const float a_heading, const char* a_label, const RE::NiPoint3* a_loc = nullptr)
		{
			s_lastRan = false;
			s_lastRet = false;
			if (!Config::Get().stanceSpike) {
				REX::WARN("P5: [Anim] bStanceSpike is 0 (or the spike was switched off after a fault): no call");
				return false;
			}
			if (!StanceFnVerified()) {
				return false;
			}
			static const RE::NiPoint3 here{ FLT_MAX, FLT_MAX, FLT_MAX };  // the engine compares the values, not the pointer
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return false;
			}
			const auto& p = Snap().position;
			REX::INFO("P5 call [{}]: f(player, stance {}, side {}, partial false, {}, heading {}) at ({:.3f},{:.3f},{:.3f}), actor angle z {:.4f} rad ({:.1f} deg)", a_label, a_stance, a_side,
				a_loc ? std::format("location ({:.3f},{:.3f},{:.3f})", a_loc->x, a_loc->y, a_loc->z) : std::string("here"),
				a_heading <= -FLT_MAX ? std::string("as I face (-FLT_MAX)") : std::format("{:.4f} rad ({:.1f} deg)", a_heading, a_heading * 180.0f / kPi), p.x, p.y, p.z, Snap().angle.z, Snap().angle.z * 180.0f / kPi);
			unsigned long code{ 0 };
			bool          ret{ false };
			const bool    ok = Guarded(
                [&] {
                    using func_t = bool (*)(RE::Actor*, std::int32_t, bool, bool, const RE::NiPoint3*, float);
                    const auto fn = reinterpret_cast<func_t>(kActorChangeStance.address());
                    ret = fn(player, a_stance, a_side, false, a_loc ? a_loc : &here, a_heading);
                },
                code);
			if (!ok) {
				REX::ERROR("P5: exception 0x{:08X} in the stance function; the spike is switched off for this session", code);
				Config::Mutable().stanceSpike = false;
				Abort("exception in the stance function", true);
				return false;
			}
			s_lastRan = true;
			s_lastRet = ret;
			REX::INFO("P5 call [{}]: returned {}", a_label, ret);
			return true;
		}

		// ---- screenshots by the hand-off of the g2 script (CLAUDE.md): SCREENSHOT NOW <name>, the ack file ------------------------------
		std::filesystem::path AckPath()
		{
			const auto& f = Config::Get().shotAckFile;
			return f.empty() ? Config::Directory() / "g2_shot_ack.txt" : std::filesystem::path(f);
		}
		double s_shotStart{ 0.0 };
		bool   s_shotWaiting{ false };

		void ShotBegin(const char* a_name)
		{
			std::error_code ec;
			std::filesystem::remove(AckPath(), ec);
			s_shotStart = Now();
			s_shotWaiting = true;
			REX::INFO("SCREENSHOT NOW {} (the script waits for the ack file, at most 25 s)", a_name);
		}

		bool ShotDone()
		{
			std::error_code ec;
			const bool      acked = std::filesystem::exists(AckPath(), ec);
			if (acked || Now() - s_shotStart > 25.0) {
				std::filesystem::remove(AckPath(), ec);
				s_shotWaiting = false;
				REX::INFO("P5: screenshot {} after {:.1f} s; the script goes on", acked ? "acknowledged" : "timed out", Now() - s_shotStart);
				return true;
			}
			return false;
		}

		// events at times counted from the moment the first step runs; a screenshot event waits for its hand-off before the next event is timed
		struct Ev
		{
			double                t;
			std::function<void()> fn;
			std::string           shot;
		};

		void AddTimeline(Seq& s, std::vector<Ev> a_evs)
		{
			double prev = 0.0;
			for (auto& e : a_evs) {
				s.Do(std::move(e.fn), e.t - prev);
				prev = e.t;
				if (!e.shot.empty()) {
					s.Do([name = e.shot] { ShotBegin(name.c_str()); });
					s.Until(ShotDone, 26.0);
				}
			}
		}

		Ev TableAt(const double a_t, const std::string& a_label, const std::string& a_shot = {})
		{
			return { a_t, [a_label] { Table(a_label); }, a_shot };
		}

		// ---- moving the player (eased SetPosition at 60 Hz) and a floor cast ---------------------------------------------------------
		struct Mv
		{
			bool         active{ false };
			RE::NiPoint3 from{}, to{};
			double       start{ 0.0 }, seconds{ 1.0 }, next{ 0.0 }, holdUntil{ 0.0 };
		};
		Mv s_mv;

		void WritePos(const RE::NiPoint3& a_target)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}
			unsigned long code{ 0 };
			if (!Guarded([&] { player->SetPosition(a_target, true); }, code)) {
				REX::ERROR("P5: exception 0x{:08X} in SetPosition", code);
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
			s_watch = false;
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
			WritePos({ s_mv.from.x + (s_mv.to.x - s_mv.from.x) * u, s_mv.from.y + (s_mv.to.y - s_mv.from.y) * u, s_mv.from.z + (s_mv.to.z - s_mv.from.z) * u });
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

		// ---- turning the view (as the p3 script does: the body turns to the camera when the player moves) -------------------------------------
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
			const float err = WrapDeg(a_headingDeg - yaw * 180.0f / kPi);
			if (std::fabs(err) < 3.0f) {
				return true;
			}
			if (s_align.haveLast && std::fabs(err) > std::fabs(s_align.lastErr) + 1.0f) {
				s_align.sign = -s_align.sign;
			}
			s_align.lastErr = err;
			s_align.haveLast = true;
			if (++s_align.tries > 12) {
				REX::WARN("P5: the view could not be turned to heading {:.0f} (still {:.1f} degrees off); going on", a_headingDeg, err);
				return true;
			}
			Rig::MouseMove(static_cast<int>(std::clamp(s_align.sign * err / 0.34f, -400.0f, 400.0f)), 0);
			s_align.next = now + 0.35;
			return false;
		}

		WorldProbe::Ticket s_floorTicket{ 0 };
		bool               s_floorHave{ false };
		float              s_floorZ{ 0.0f };

		// ---- readiness and the two places ---------------------------------------------------------------------------------------------
		bool         s_haveAnchor{ false };
		RE::NiPoint3 s_anchor{};
		double       s_stillSince{ 0.0 };
		RE::NiPoint3 s_spawn{};

		bool Ready(const Frame::Snapshot& a_snap)
		{
			if (!a_snap.valid || a_snap.paused || !Camera::GetStats().installed) {
				s_haveAnchor = false;
				return false;
			}
			if (!s_haveAnchor || std::fabs(a_snap.position.x - s_anchor.x) > 0.02f || std::fabs(a_snap.position.y - s_anchor.y) > 0.02f || std::fabs(a_snap.position.z - s_anchor.z) > 0.02f) {
				s_anchor = a_snap.position;
				s_haveAnchor = true;
				s_stillSince = a_snap.time;
				return false;
			}
			if (a_snap.time - s_stillSince < 3.0 || !Rig::InForeground()) {
				return false;
			}
			if (a_snap.cameraIndex != RE::CameraStates::kThirdPerson) {
				static double s_lastForce = -10.0;
				if (a_snap.time - s_lastForce > 2.5) {
					s_lastForce = a_snap.time;
					REX::INFO("S0 rig: p5: the camera is not third person (index {}); asking for third person", a_snap.cameraIndex);
					(void)Camera::ForceThirdPerson();
				}
				return false;
			}
			if (!WorldProbe::Available()) {
				return false;
			}
			s_spawn = a_snap.position;
			REX::INFO("S0 rig: script 'p5' mode {} starting at ({:.2f},{:.2f},{:.2f}), third person, game window in front, cast queue available", Config::Get().p5Mode, a_snap.position.x, a_snap.position.y, a_snap.position.z);
			return true;
		}

		[[nodiscard]] bool IsSave29() { return s_spawn.x < -650.0f; }

		// ---- stage 1: the probe, no writes ----------------------------------------------------------------------------------------------
		struct ProbeData
		{
			std::uint64_t n{ 0 };
			std::uint64_t unreadable{ 0 };
			std::uint64_t badStance{ 0 }, badAlert{ 0 }, badJump{ 0 };
			std::uint64_t noGraph{ 0 };
			std::set<int> seenStance, seenAlert, seenJump;
			double        t0{ 0.0 };
		};

		void AddProbe(Seq& s, const double a_seconds)
		{
			auto d = std::make_shared<ProbeData>();
			const double k = a_seconds / 6.0;
			s.Do([] {
				BeginStage("stage 1, probe");
				s_watch = false;  // the probe's jump carries the player about a metre (run p5-1 stopped there): the stop rule belongs to the calls
				Table("probe, start");
			});
			s.Do([d] {
				Rig::ReleaseEverything("p5 probe");
				Rig::Tap(Rig::kKeyShift, 0.05);  // a real button event passes the hook: the template of the made-up Sneak press
				Rig::MouseRight(true);           // the aim button held: a queue with an event in it every frame
			});
			s.Wait(0.5);
			s.Do([d] { d->t0 = Now(); });
			// the rig's own events during the probe: sneak twice, a jump, the weapon drawn
			auto events = std::make_shared<std::vector<std::pair<double, std::function<void()>>>>();
			events->push_back({ 0.3 * k, [] { REX::INFO("P5 probe: a made-up Sneak press (1)"); Input::RequestSneakPress(); } });
			events->push_back({ 1.3 * k, [] { REX::INFO("P5 probe: a made-up Sneak press (2)"); Input::RequestSneakPress(); } });
			events->push_back({ 2.5 * k, [] { REX::INFO("P5 probe: a jump"); Rig::Tap(kKeySpace, 0.1); } });
			events->push_back({ 4.0 * k, [] { REX::INFO("P5 probe: DrawWeaponMagicHands(true)"); (void)AnimRecon::DrawWeapon(true); } });
			auto next = std::make_shared<std::size_t>(0);
			s.Until([d, events, next, a_seconds] {
				const double t = Now() - d->t0;
				while (*next < events->size() && t >= (*events)[*next].first) {
					(*events)[*next].second();
					++*next;
				}
				Fields f;
				++d->n;
				if (!ReadFields(f)) {
					++d->unreadable;
				} else {
					const auto g1 = PlayerControl::ReadVar("iSyncActorStance");
					const auto g2 = PlayerControl::ReadVar("iSyncRelaxedReadyAlert");
					const auto g3 = PlayerControl::ReadVar("iSyncJumpState");
					d->seenStance.insert(f.stance);
					d->seenAlert.insert(f.alert);
					d->seenJump.insert(f.jump);
					if (!g1 || !g2 || !g3) {
						++d->noGraph;
					}
					d->badStance += (g1 && static_cast<int>(std::lround(*g1)) != f.stance) ? 1 : 0;
					d->badAlert += (g2 && static_cast<int>(std::lround(*g2)) != f.alert) ? 1 : 0;
					d->badJump += (g3 && static_cast<int>(std::lround(*g3)) != f.jump) ? 1 : 0;
				}
				return t >= a_seconds;
			}, a_seconds + 3.0);
			s.Do([d] {
				Rig::MouseRight(false);
				Rig::ReleaseEverything("p5 probe done");
				Input::ClearMadeUpRequests();
				auto list = [](const std::set<int>& a_s) {
					std::string out;
					for (const int v : a_s) {
						out += std::format(" {}", v);
					}
					return out;
				};
				// the stance and the alert fields are what the calls rely on: no disagreement allowed. The jump state changes while the field and the graph variable are read a moment apart
					// (run m1c-2: one sample of 1331 differed during the probe's own jump): up to 3 are allowed.
					const bool ok = d->n > 100 && d->unreadable == 0 && d->noGraph == 0 && d->badStance == 0 && d->badAlert == 0 && d->badJump <= 3;
				Result(std::format("P5 T5.0 probe (no writes): {} samples, {} unreadable, {} with an unresolved graph variable; disagreements between the process field and the graph variable: stance {} (values seen:{}), relaxed/ready alert {} (values seen:{}), jump state {} (values seen:{}) -> {}",
					d->n, d->unreadable, d->noGraph, d->badStance, list(d->seenStance), d->badAlert, list(d->seenAlert), d->badJump, list(d->seenJump), ok ? "PASS" : "FAIL"));
				Summarise("T5.0 probe: the fields agree with the graph", ok);
				Table("probe, end");
				if (!ok) {
					Abort("the probe failed: no call may follow");
				}
			});
			s.Wait(0.5);
		}

		// ---- stage 2 (and 4's second half): the call on whatever graph the player has ----------------------------------------------------
		std::int32_t s_stanceBefore{ -1 };

		void AddEnterCall(Seq& s, const std::string& a_tag, const int a_stance, const bool a_side, const float a_heading, const std::string& a_shot1, const std::string& a_shot2 = {})
		{
			s.Do([a_tag, a_stance, a_side, a_heading] {
				BeginStage(a_tag.c_str());
				Table(a_tag + " before");
				(void)CallStance(a_stance, a_side, a_heading, (a_tag + " enter").c_str());
			});
			std::vector<Ev> evs;
			evs.push_back(TableAt(0.2, a_tag + " +0.2 s"));
			evs.push_back(TableAt(0.5, a_tag + " +0.5 s"));
			evs.push_back(TableAt(1.0, a_tag + " +1.0 s", a_shot1));
			evs.push_back(TableAt(2.0, a_tag + " +2.0 s", a_shot2));
			AddTimeline(s, std::move(evs));
		}

		void AddExitCall(Seq& s, const std::string& a_tag, const std::string& a_shot = {}, const std::vector<double> a_times = { 0.5, 1.0 })
		{
			s.Do([a_tag] {
				BeginStage(a_tag.c_str());
				(void)CallStance(0, false, -FLT_MAX, (a_tag + " exit").c_str());
			});
			std::vector<Ev> evs;
			for (const double t : a_times) {
				evs.push_back(TableAt(t, std::format("{} +{:.1f} s", a_tag, t), t >= 1.0 ? a_shot : std::string()));
			}
			AddTimeline(s, std::move(evs));
		}

		void AddStage2(Seq& s)
		{
			s.Do([] {
				Fields f;
				(void)ReadFields(f);
				s_stanceBefore = f.stance;
				REX::INFO("P5 stage 2: the stance field reads {} before the call", s_stanceBefore);
			});
			AddEnterCall(s, "stage 2 own graph, stance 3", 3, false, -FLT_MAX, "own_3");
			AddExitCall(s, "stage 2 own graph, exit");
			s.Wait(0.3);
			s.Do([] {
				Fields f;
				(void)ReadFields(f);
				if (f.ok && f.stance != 0 && f.stance != 1) {
					REX::WARN("P5 stage 2: the stance field reads {} one second after the exit call (not 0 or 1): the value it had before ({}) is written back", f.stance, s_stanceBefore);
					Result(std::format("P5 T5.0 stage 2: the stance field read {} one second after the exit call; it was written back to {}", f.stance, s_stanceBefore));
					(void)WriteStanceField(s_stanceBefore);
				} else {
					Result(std::format("P5 T5.0 stage 2: the stance field reads {} one second after the exit call (0 or 1: nothing written back)", f.stance));
				}
				Table("stage 2 end");
			});
		}

		// ---- stage 3: the borrowed graph ---------------------------------------------------------------------------------------------------
		void AddLeg(Seq& s, const char* a_name, const Rig::Key a_key, const std::string& a_tag)
		{
			auto p0 = std::make_shared<RE::NiPoint3>();
			s.Do([a_name, a_key, a_tag, p0] {
				BeginStage(a_tag.c_str());
				*p0 = Snap().position;
				Table(std::string(a_tag) + " before");
				REX::INFO("P5: {} held for 1 s", a_name);
				Rig::Tap(a_key, 1.0);
			});
			s.Wait(1.2);
			s.Do([a_name, a_tag, p0] {
				Rig::ReleaseEverything("p5 leg");
				const auto& p = Snap().position;
				Table(a_tag + " after");
				Result(std::format("P5 T5.0 {}: the player moved ({:+.3f},{:+.3f}) m, {:.3f} m in all, from ({:.3f},{:.3f})", a_name, p.x - p0->x, p.y - p0->y, std::hypot(p.x - p0->x, p.y - p0->y), p0->x, p0->y));
			});
		}

		void AddStage3(Seq& s, const int a_stance, const float a_heading, const std::string& a_prefix)
		{
			const std::string st = std::to_string(a_stance);
			// the weapon drawn
			s.Do([] {
				BeginStage("stage 3, the weapon");
				const auto w = PlayerControl::ReadVar("iSyncWeaponDrawState");
				REX::INFO("P5 stage 3: iSyncWeaponDrawState reads {}", w ? std::format("{:g}", *w) : std::string("unresolved"));
				if (!w || *w < 0.5) {
					REX::INFO("P5 stage 3: the weapon is not drawn: DrawWeaponMagicHands(true)");
					(void)AnimRecon::DrawWeapon(true);
				}
			});
			s.Wait(1.5);
			s.Do([] {
				BeginStage("stage 3, the archetype");
				bool ret{ false };
				const bool ran = AnimRecon::ChangeArchetype(0, ret);
				REX::INFO("P5 stage 3: ChangeAnimArchetype(player, null): ran {}, returned {}", ran, ret);
				if (!ran) {
					Abort("ChangeAnimArchetype did not run (refused or faulted)");
				}
			});
			s.Wait(1.0);
			s.Do([] { Table("stage 3 after ChangeAnimArchetype(null), +1.0 s"); });
			AddEnterCall(s, "stage 3 borrowed graph, stance " + st + ", side 0", a_stance, false, a_heading, a_prefix + st + "_side0_a", a_prefix + st + "_side0_b");
			AddLeg(s, "A held 1 s in cover", Rig::kKeyA, "stage 3 leg A");
			AddLeg(s, "D held 1 s in cover", Rig::kKeyD, "stage 3 leg D");
			AddLeg(s, "W held 1 s in cover", Rig::kKeyW, "stage 3 leg W");
			// aim 2 s, a picture, one shot
			s.Do([] {
				BeginStage("stage 3, aim");
				Table("stage 3 aim before");
				Rig::MouseRight(true);
			});
			{
				std::vector<Ev> evs;
				evs.push_back(TableAt(1.0, "stage 3 aim +1.0 s", a_prefix + st + "_aim"));
				AddTimeline(s, std::move(evs));
			}
			s.Do([] {
				REX::INFO("P5: one shot (left mouse button tapped) while aiming");
				Rig::MouseLeft(true);
			});
			s.Wait(0.12);
			s.Do([] { Rig::MouseLeft(false); });
			s.Wait(0.6);
			s.Do([] { Table("stage 3 after the shot"); });
			s.Wait(0.4);
			s.Do([] {
				Rig::MouseRight(false);
				Table("stage 3 aim released");
			});
			s.Wait(0.5);
			AddExitCall(s, "stage 3 exit, side 0", a_prefix + "exit");
			s.Wait(0.5);
			AddEnterCall(s, "stage 3 borrowed graph, stance " + st + ", side 1", a_stance, true, a_heading, a_prefix + st + "_side1");
			AddExitCall(s, "stage 3 exit, side 1", {});
		}

		// ---- stage 4: the restore and the test that counts ------------------------------------------------------------------------------------
		void AddStage4(Seq& s, const int a_stance, const float a_heading, const std::string& a_restoredName)
		{
			s.Do([] {
				BeginStage("stage 4, restore");
				bool       ret{ false };
				const bool ran = AnimRecon::RestoreArchetype(ret);
				REX::INFO("P5 stage 4: ChangeAnimArchetype(player, AnimArchetypePlayer): ran {}, returned {}", ran, ret);
				(void)AnimRecon::DrawWeapon(false);
			});
			s.Wait(1.0);
			s.Do([] { (void)AnimRecon::DrawWeapon(true); });
			s.Wait(2.0);
			s.Do([] { Table("stage 4 restored, holstered and drawn, +2.0 s"); });
			AddEnterCall(s, "stage 4 restored, the stage 2 call again, stance " + std::to_string(a_stance), a_stance, false, a_heading, a_restoredName);
			AddExitCall(s, "stage 4 restored, exit");
			s.Wait(0.3);
		}

		// ---- T5.0b: the exit function, the camera rig sample, modes 4 and 5 ---------------------------------------------------------------------
		[[nodiscard]] bool ExitFnVerified()
		{
			static int s_checked = 0;
			if (s_checked == 0) {
				const auto base = REX::FModule::GetExecutingModule().GetBaseAddress();
				const auto addr = kActorExitCover.address();
				const auto rva = addr - base;
				std::array<std::uint8_t, 26> bytes{};
				const bool readable = SafeCopy(bytes.data(), reinterpret_cast<const void*>(addr), bytes.size()) == bytes.size();
				const bool ok = rva == kActorExitCoverRva && readable && bytes == kActorExitCoverBytes;
				s_checked = ok ? 1 : -1;
				REX::INFO("P5: the exit function, ID {} resolves to RVA 0x{:X} (expected 0x{:X}), the first 26 bytes {} -> {}", kActorExitCover.id(), rva, kActorExitCoverRva, readable ? (bytes == kActorExitCoverBytes ? "match" : "DIFFER") : "UNREADABLE",
					ok ? "OK, the function may be called" : "REFUSED, the function is not called");
			}
			return s_checked == 1;
		}

		// g(player): the engine's way out of a cover stance. Returns false when it did not run.
		bool CallExit(const char* a_label)
		{
			s_lastRan = false;
			s_lastRet = false;
			if (!Config::Get().stanceSpike || !ExitFnVerified()) {
				return false;
			}
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return false;
			}
			const auto& p = Snap().position;
			Fields before;
			(void)ReadFields(before);
			REX::INFO("P5 call [{}]: g(player) at ({:.3f},{:.3f},{:.3f}), the stance field reads {} before", a_label, p.x, p.y, p.z, before.stance);
			unsigned long code{ 0 };
			bool          ret{ false };
			const bool    ok = Guarded(
				[&] {
					using func_t = bool (*)(RE::Actor*);
					const auto fn = reinterpret_cast<func_t>(kActorExitCover.address());
					ret = fn(player);
				},
				code);
			if (!ok) {
				REX::ERROR("P5: exception 0x{:08X} in the exit function; the spike is switched off for this session", code);
				Config::Mutable().stanceSpike = false;
				Abort("exception in the exit function", true);
				return false;
			}
			Fields after;
			(void)ReadFields(after);
			s_lastRan = true;
			s_lastRet = ret;
			REX::INFO("P5 call [{}]: returned {}; the stance field reads {} right after", a_label, ret, after.stance);
			return true;
		}

		struct OwnRig
		{
			bool         have{ false };
			RE::NiPoint3 boom{}, pivot{};
		};
		OwnRig s_ownRig;

		void RigSample(const std::string& a_label, const bool a_store = false)
		{
			const auto r = Camera::ReadRig();
			REX::INFO("{}", Camera::DescribeRig(a_label.c_str(), r));
			if (a_store && r.ok && r.n1F8.present && r.n200.present) {
				s_ownRig = { true, r.n1F8.t, r.n200.t };
				REX::INFO("P5: the own-graph rig stored for the hold: boom ({:+.4f},{:+.4f},{:+.4f}), pivot ({:+.4f},{:+.4f},{:+.4f})", r.n1F8.t.x, r.n1F8.t.y, r.n1F8.t.z, r.n200.t.x, r.n200.t.y, r.n200.t.z);
			}
		}

		void AddShot(Seq& s, const std::string& a_name)
		{
			s.Do([a_name] { ShotBegin(a_name.c_str()); });
			s.Until(ShotDone, 26.0);
		}

		// events at times counted from the first step; a shot follows the event that names it
		Ev TableRigAt(const double a_t, const std::string& a_label, const std::string& a_shot = {})
		{
			return { a_t, [a_label] { Table(a_label); RigSample(a_label); }, a_shot };
		}

		void AddExitFn(Seq& s, const std::string& a_tag, const std::string& a_shot = {})
		{
			s.Do([a_tag] {
				BeginStage(a_tag.c_str());
				(void)CallExit(a_tag.c_str());
			});
			std::vector<Ev> evs;
			evs.push_back(TableAt(0.2, a_tag + " +0.2 s"));
			evs.push_back(TableAt(0.5, a_tag + " +0.5 s"));
			if (a_shot.empty()) {
				evs.push_back(TableAt(1.0, a_tag + " +1.0 s"));
			} else {
				evs.push_back(TableRigAt(1.0, a_tag + " +1.0 s", a_shot));
			}
			AddTimeline(s, std::move(evs));
		}

		// the own-graph samples (three, 0.5 s apart, the last one stored for the hold), the weapon drawn, the archetype cleared
		void AddBorrow(Seq& s)
		{
			s.Do([] {
				BeginStage("rig samples, own graph");
				RigSample("own #1");
			});
			s.Wait(0.5);
			s.Do([] { RigSample("own #2"); });
			s.Wait(0.5);
			s.Do([] {
				RigSample("own #3", true);
				Table("own graph, before the archetype change");
			});
			s.Do([] {
				BeginStage("the weapon");
				const auto w = PlayerControl::ReadVar("iSyncWeaponDrawState");
				REX::INFO("P5: iSyncWeaponDrawState reads {}", w ? std::format("{:g}", *w) : std::string("unresolved"));
				if (!w || *w < 0.5) {
					REX::INFO("P5: the weapon is not drawn: DrawWeaponMagicHands(true)");
					(void)AnimRecon::DrawWeapon(true);
				}
			});
			s.Wait(1.5);
			s.Do([] {
				BeginStage("the archetype");
				bool       ret{ false };
				const bool ran = AnimRecon::ChangeArchetype(0, ret);
				REX::INFO("P5: ChangeAnimArchetype(player, null): ran {}, returned {}", ran, ret);
				if (!ran) {
					Abort("ChangeAnimArchetype did not run (refused or faulted)");
				}
			});
			s.Wait(1.0);
		}

		void AddRestore(Seq& s, const std::string& a_shotName)
		{
			s.Do([] {
				Camera::SetRigHold(false, {}, {});
				BeginStage("restore");
				bool       ret{ false };
				const bool ran = AnimRecon::RestoreArchetype(ret);
				REX::INFO("P5: ChangeAnimArchetype(player, AnimArchetypePlayer): ran {}, returned {}", ran, ret);
				(void)AnimRecon::DrawWeapon(false);
			});
			s.Wait(1.0);
			s.Do([] { (void)AnimRecon::DrawWeapon(true); });
			s.Wait(2.0);
			s.Do([] {
				Table("own restored");
				RigSample("own restored");
			});
			AddShot(s, a_shotName);
		}

		// mode 4: the camera rig on the borrowed graph, without and with the hold, the exit function, side true, the second stance
		void AddMode4(Seq& s, const int a_stanceA, const int a_stanceB, const std::string& a_p)
		{
			const std::string sa = std::to_string(a_stanceA);
			const std::string sb = std::to_string(a_stanceB);
			AddBorrow(s);
			s.Do([a_p] { Table("npc standing"); RigSample("npc standing"); });
			AddShot(s, a_p + "_npc_standing");
			// d. the cover, no hold
			s.Do([sa, a_stanceA] {
				BeginStage("cover, no hold");
				Table("before the enter call, stance " + sa);
				(void)CallStance(a_stanceA, false, -FLT_MAX, "cover, no hold");
			});
			{
				std::vector<Ev> evs;
				evs.push_back(TableRigAt(1.0, "npc cover, no hold", a_p + "_cover_nohold"));
				AddTimeline(s, std::move(evs));
			}
			// e. the hold on
			s.Do([] {
				BeginStage("cover, hold");
				Camera::SetRigHold(true, s_ownRig.boom, s_ownRig.pivot);
			});
			{
				std::vector<Ev> evs;
				evs.push_back(TableRigAt(0.5, "npc cover, hold", a_p + "_cover_hold_a"));
				evs.push_back(TableAt(2.0, "npc cover, hold +1.5 s after the first picture", a_p + "_cover_hold_b"));
				AddTimeline(s, std::move(evs));
			}
			// f. aim 2 s
			s.Do([] {
				BeginStage("cover, hold, aim");
				Rig::MouseRight(true);
			});
			{
				std::vector<Ev> evs;
				evs.push_back(TableRigAt(1.0, "npc cover, hold, aim +1.0 s", a_p + "_cover_hold_aim"));
				AddTimeline(s, std::move(evs));
			}
			s.Do([] { Rig::MouseRight(false); });
			s.Wait(1.0);
			// g. the exit function
			AddExitFn(s, "exit function g(player), hold on", a_p + "_after_exit");
			s.Wait(0.5);
			// h. side true on a clean state
			s.Do([sa, a_stanceA] {
				BeginStage("side true");
				Fields f;
				(void)ReadFields(f);
				REX::INFO("P5: iSyncRightLeft reads {} before the enter call with side true (stance field {})", f.rightLeft, f.stance);
				Table("before side true");
				(void)CallStance(a_stanceA, true, -FLT_MAX, "side true");
			});
			{
				std::vector<Ev> evs;
				evs.push_back(TableAt(1.0, "side true +1.0 s", a_p + "_cover_side1"));
				AddTimeline(s, std::move(evs));
			}
			AddExitFn(s, "exit function after side true");
			s.Wait(0.5);
			// i. the second stance
			s.Do([sb, a_stanceB] {
				BeginStage("second stance");
				Table("before stance " + sb);
				(void)CallStance(a_stanceB, false, -FLT_MAX, "second stance");
			});
			{
				std::vector<Ev> evs;
				evs.push_back(TableAt(1.0, "stance " + sb + " +1.0 s", a_p + "_cover_stance" + sb));
				AddTimeline(s, std::move(evs));
			}
			AddExitFn(s, "exit function after the second stance");
			s.Wait(0.5);
			// j. back to the player's own graph
			AddRestore(s, a_p + "_restored");
		}

		// mode 5: the cover's own verbs through NotifyAnimationGraphImpl
		void AddVerb(Seq& s, const std::string& a_event, const std::string& a_end, const bool a_shoot, const float a_limit)
		{
			auto p0 = std::make_shared<RE::NiPoint3>();
			s.Do([a_event, a_limit, p0] {
				BeginStage(a_event.c_str(), a_limit);
				*p0 = Snap().position;
				Table(a_event + " before");
				bool       accepted{ false };
				const bool ran = AnimRecon::SendEvent(a_event.c_str(), accepted);
				REX::INFO("P5 event [{}]: sent {}, accepted {}", a_event, ran, accepted);
				Result(std::format("P5 T5.0b event {}: ran {}, accepted {}", a_event, ran, accepted));
			});
			std::vector<Ev> evs;
			evs.push_back({ 1.0, [a_event, a_shoot] {
				Table(a_event + " +1.0 s");
				if (a_shoot) {
					REX::INFO("P5: one shot (left mouse button) in {}", a_event);
					Rig::MouseLeft(true);
				}
			}, {} });
			evs.push_back({ 1.12, [] { Rig::MouseLeft(false); }, {} });
			evs.push_back({ 1.5, [a_event, p0] {
				const auto& p = Snap().position;
				Table(a_event + " +1.5 s");
				Result(std::format("P5 T5.0b event {}: the player moved ({:+.3f},{:+.3f},{:+.3f}) m in 1.5 s, {:.3f} m horizontally", a_event, p.x - p0->x, p.y - p0->y, p.z - p0->z, std::hypot(p.x - p0->x, p.y - p0->y)));
			}, "v_" + a_event });
			AddTimeline(s, std::move(evs));
			if (!a_end.empty()) {
				s.Do([a_end] {
					bool       accepted{ false };
					const bool ran = AnimRecon::SendEvent(a_end.c_str(), accepted);
					REX::INFO("P5 event [{}]: sent {}, accepted {}", a_end, ran, accepted);
					Result(std::format("P5 T5.0b event {}: ran {}, accepted {}", a_end, ran, accepted));
				});
				s.Wait(1.0);
				s.Do([a_end] { Table(a_end + " +1.0 s"); });
			}
			s.Wait(0.3);
		}

		void AddMode5(Seq& s, const int a_stance, const std::string& a_p)
		{
			AddBorrow(s);
			s.Do([a_stance] {
				BeginStage("cover, hold on from the start");
				Camera::SetRigHold(true, s_ownRig.boom, s_ownRig.pivot);
				Table("before the enter call");
				(void)CallStance(a_stance, false, -FLT_MAX, "mode 5 enter");
			});
			{
				std::vector<Ev> evs;
				evs.push_back(TableRigAt(1.0, "mode 5 cover +1.0 s"));
				AddTimeline(s, std::move(evs));
			}
			AddVerb(s, "ShuffleForward", {}, false, 3.0f);
			AddVerb(s, "ShuffleBackward", {}, false, 3.0f);
			AddVerb(s, "SightedOverLowStart", "SightedEnd", true, 1.0f);
			AddVerb(s, "BlindFireOverStart", "BlindFireEnd", true, 1.0f);
			AddVerb(s, "SightedStart", "SightedEnd", false, 1.0f);
			AddExitFn(s, "exit function g(player), hold on");
			s.Wait(0.5);
			AddRestore(s, a_p + "_restored");
		}

		// ---- stage 5 and the end --------------------------------------------------------------------------------------------------------------
		void Cleanup(const bool a_fault)
		{
			if (s_cleaned) {
				return;
			}
			s_cleaned = true;
			Camera::SetRigHold(false, {}, {});
			unsigned long ignored{ 0 };
			(void)Guarded([] { Rig::ReleaseEverything("p5 end"); }, ignored);
			if (AnimRecon::ArchetypeChanged()) {
				bool       ret{ false };
				const bool ran = AnimRecon::RestoreArchetype(ret);
				REX::INFO("P5 stage 5: the archetype is put back (AnimArchetypePlayer): ran {}, returned {}", ran, ret);
			}
			Fields f;
			if (ReadFields(f)) {
				REX::INFO("P5 stage 5: the stance field reads {}", f.stance);
				if (f.stance >= 2 && !a_fault && Config::Get().stanceSpike) {
					REX::INFO("P5 stage 5: the player is still in a cover stance: the exit function g(player)");
					(void)CallExit("stage 5 exit");
					if (ReadFields(f) && f.stance != 0 && f.stance != 1) {
						REX::WARN("P5 stage 5: the stance field still reads {}; 0 is written back", f.stance);
						(void)WriteStanceField(0);
					}
				}
			}
			Table("stage 5, the end");
		}

		void Finish(const bool a_ok, const char* a_why)
		{
			if (s_done) {
				return;
			}
			Cleanup(s_faulted);
			Rig::ReleaseEverything(a_ok ? "p5 finished" : "p5 aborted");
			REX::INFO("RESULT rig p5: {} ({}) | {} passed, {} failed", a_ok ? "all stages ran" : "stopped early", a_why, s_pass, s_fail);
			for (const auto& l : s_summary) {
				REX::INFO("RESULT P5 summary {}", l);
			}
			REX::INFO("TEST COMPLETE{}", a_ok ? "" : " (aborted)");
			s_done = true;
			if (Config::Get().quitAfterTest) {
				if (const auto main = RE::Main::GetSingleton()) {
					main->quitGame = true;
					REX::INFO("S0 rig: Main::quitGame set; the game should close now");
				}
			}
		}

		// a floor cast at (x, y), then the eased move there at about 3 m/s
		void AddMoveTo(Seq& s, const float a_x, const float a_y)
		{
			auto target = std::make_shared<RE::NiPoint3>(a_x, a_y, 0.0f);
			s.Do([target] {
				Rig::ReleaseEverything("p5 move");
				s_floorTicket = WorldProbe::Submit({ target->x, target->y, Snap().position.z + 1.0f }, { target->x, target->y, Snap().position.z - 3.0f }, 0.1f);
			});
			s.Until([] {
				const auto r = WorldProbe::Poll(s_floorTicket);
				if (!r) {
					return false;
				}
				s_floorHave = r->ok && r->hit;
				s_floorZ = r->point.z - 0.1f;
				return true;
			}, 3.0);
			s.Do([target] {
				if (!s_floorHave || std::fabs(s_floorZ - Snap().position.z) > 1.0f) {
					Abort("no floor within 1 m of the player's height at the target of an eased move");
					return;
				}
				target->z = s_floorZ;
				const float dist = std::hypot(target->x - Snap().position.x, target->y - Snap().position.y);
				REX::INFO("P5: the eased move to ({:.2f},{:.2f},{:.2f}), {:.2f} m", target->x, target->y, target->z, dist);
				MoveBegin(*target, (std::max)(1.0, static_cast<double>(dist) / 3.0));
			});
			s.Until(MoveStep, 15.0);
			s.Wait(0.8);
		}

		// ---- M1c, modes 6 and 7: the first four pose runs of REFERENCE 22.5 (runs 1, 2, 8 and 3), Save30, the low wall -----------------------------------------------------
		// a background sampler: the player's position every 0.02 s (so a time after an event can be looked up whatever the script was waiting for), and an optional logger
		struct TraceSample
		{
			double       t;
			RE::NiPoint3 p;
			float        angle;
		};
		std::vector<TraceSample> s_trace;
		double                   s_traceNext{ 0.0 };
		std::function<void()>    s_bg;

		void TraceTick()
		{
			const double now = Now();
			if (now >= s_traceNext && Snap().valid) {
				s_traceNext = now + 0.02;
				s_trace.push_back({ now, Snap().position, Snap().angle.z });
				if (s_trace.size() > 20000) {
					s_trace.erase(s_trace.begin(), s_trace.begin() + 10000);
				}
			}
			if (s_bg) {
				s_bg();
			}
		}

		[[nodiscard]] TraceSample TraceAt(const double a_t)
		{
			TraceSample best{ a_t, Snap().position, Snap().angle.z };
			double      bestD = 1e9;
			for (auto it = s_trace.rbegin(); it != s_trace.rend(); ++it) {
				const double d = std::fabs(it->t - a_t);
				if (d < bestD) {
					bestD = d;
					best = *it;
				} else if (it->t < a_t - 0.5) {
					break;
				}
			}
			return best;
		}

		[[nodiscard]] float AngleDiffDeg(const float a_from, const float a_to) { return WrapDeg((a_to - a_from) * 180.0f / kPi); }

		bool WriteDesiredLocation(const RE::NiPoint3& a_to)
		{
			const auto b = MiddleHigh();
			if (!b) {
				return false;
			}
			unsigned long code{ 0 };
			const bool    ok = Guarded(
				[&] {
					auto* f = reinterpret_cast<float*>(const_cast<std::byte*>(b) + kMhDesiredLocation);
					f[0] = a_to.x;
					f[1] = a_to.y;
					f[2] = a_to.z;
				},
				code);
			if (!ok) {
				REX::ERROR("P5: exception 0x{:08X} writing DesiredLocation", code);
			}
			return ok;
		}

		struct Basis
		{
			float fx, fy, rx, ry;
		};
		[[nodiscard]] Basis BasisOf(const float a_angle) { return { std::sin(a_angle), std::cos(a_angle), std::cos(a_angle), -std::sin(a_angle) }; }  // forward and right of an actor angle z

		[[nodiscard]] std::string CoverVars()
		{
			return std::format("{} {} {} {} {}", GraphVar("iSyncCoverStates"), GraphVar("iSyncIdleLocomotion"), GraphVar("Direction"), GraphVar("Speed"), GraphVar("iSyncRightLeft"));
		}

		// the common start of modes 6 and 7: own-graph samples, the weapon, the archetype cleared, the hold on
		void AddBorrowAndHold(Seq& s)
		{
			AddBorrow(s);
			s.Do([] {
				BeginStage("hold on");
				Camera::SetRigHold(true, s_ownRig.boom, s_ownRig.pivot);
				s_trace.clear();
			});
		}

		void AddCleanEnd(Seq& s, const std::string& a_p)
		{
			s.Do([] { s_bg = nullptr; Rig::ReleaseEverything("p5 pose runs"); });
			AddExitFn(s, "exit function g(player), end of the runs");
			s.Wait(0.5);
			AddRestore(s, a_p + "_restored");
		}

		// ---- run 1: the shuffle with DesiredLocation moved -------------------------------------------------------------------------------------------------------------------
		struct ShuffleCase
		{
			RE::NiPoint3 p0{}, target{};
			double       t0{ 0.0 };
			float        angle0{ 0.0f };
		};

		void AddShuffle(Seq& s, const std::string& a_tag, const char* a_event, const float a_right, const float a_forward, const std::string& a_shot)
		{
			auto d = std::make_shared<ShuffleCase>();
			s.Do([d, a_tag, a_event, a_right, a_forward] {
				BeginStage(a_tag.c_str(), 3.0f);
				d->p0 = Snap().position;
				d->angle0 = Snap().angle.z;
				const auto b = BasisOf(d->angle0);
				d->target = { d->p0.x + b.rx * a_right + b.fx * a_forward, d->p0.y + b.ry * a_right + b.fy * a_forward, d->p0.z };
				Table(a_tag + " before");
				const bool wrote = WriteDesiredLocation(d->target);
				bool       accepted{ false };
				d->t0 = Now();
				const bool ran = AnimRecon::SendEvent(a_event, accepted);
				REX::INFO("P5 run1 [{}]: DesiredLocation written {} to ({:.3f},{:.3f},{:.3f}) = the player ({:.3f},{:.3f}) {:+.2f} m to the actor's right and {:+.2f} m forward (actor angle z {:.3f} rad); event {} sent {}, accepted {}", a_tag, wrote, d->target.x, d->target.y, d->target.z, d->p0.x, d->p0.y, a_right, a_forward, d->angle0, a_event, ran, accepted);
			});
			s.Wait(1.6);
			s.Do([d, a_tag, a_right, a_forward] {
				std::string line;
				float       err09 = 99.0f;
				for (const double t : { 0.3, 0.6, 0.9, 1.5 }) {
					const auto  p = TraceAt(d->t0 + t);
					const float e = std::hypot(p.p.x - d->target.x, p.p.y - d->target.y);
					const float moved = std::hypot(p.p.x - d->p0.x, p.p.y - d->p0.y);
					if (t == 0.9) {
						err09 = e;
					}
					line += std::format(" +{:.1f} s: moved {:.3f} m, {:.3f} m from the target;", t, moved, e);
				}
				const bool ok = err09 <= 0.05f;
				Result(std::format("P5 run1 {} (the target is {:+.2f} m right, {:+.2f} m forward = {:.2f} m from the start):{} on the target within 0.05 m by +0.9 s -> {}", a_tag, a_right, a_forward, std::hypot(a_right, a_forward), line, ok ? "PASS" : "FAIL"));
				Summarise("run1 " + a_tag, ok);
				Table(a_tag + " +1.6 s");
			});
			if (!a_shot.empty()) {
				AddShot(s, a_shot);
			}
			s.Wait(0.4);
		}

		void AddRun1(Seq& s)
		{
			AddShuffle(s, "run1a ShuffleForward 0.60 m to the right", "ShuffleForward", 0.60f, 0.0f, "r1_a");
			AddShuffle(s, "run1b ShuffleBackward 0.60 m the other way", "ShuffleBackward", -0.60f, 0.0f, "r1_b");
			AddShuffle(s, "run1c ShuffleForward 1.50 m to the right", "ShuffleForward", 1.50f, 0.0f, "r1_c");
			AddShuffle(s, "run1d ShuffleBackward 1.50 m back", "ShuffleBackward", -1.50f, 0.0f, {});
		}

		// ---- run 2: can the walk be used -------------------------------------------------------------------------------------------------------------------------------------
		struct WalkData
		{
			double       t0{ 0.0 };
			double       nextLog{ 0.0 };
			RE::NiPoint3 p0{};
			float        angle0{ 0.0f };
			bool         mover{ false };
			float        amp{ 1.6f };
			double       nextMove{ 0.0 };
			float        speed{ 0.84f };
			std::vector<std::string> lines;
		};

		void AddWalk(Seq& s, const std::string& a_tag, const bool a_mover, const std::string& a_shot)
		{
			auto d = std::make_shared<WalkData>();
			s.Do([d, a_tag, a_mover] {
				BeginStage(a_tag.c_str(), 3.0f);
				s_watch = false;
				d->p0 = Snap().position;
				d->angle0 = Snap().angle.z;
				d->mover = a_mover;
				d->t0 = Now();
				d->nextLog = d->t0;
				Table(a_tag + " before");
				Rig::Tap(Rig::kKeyD, 30.0);  // held until the script lets it go
				REX::INFO("P5 run2 [{}]: D held{}", a_tag, a_mover ? "; the plugin moves the player to its right at 0.84 m/s (back and forth over 1.6 m)" : "");
				s_bg = [d, a_tag] {
					const double now = Now();
					if (d->mover && now >= d->nextMove) {
						// a triangle wave along the actor's right at 0.84 m/s: 0 to 1.6 m out and back, from the start point, written at 60 Hz
						d->nextMove = now + 1.0 / 60.0;
						const double dist = (now - d->t0) * d->speed;
						const double m = std::fmod(dist, 2.0 * d->amp);
						const double off = m < d->amp ? m : 2.0 * d->amp - m;
						const auto   b = BasisOf(d->angle0);
						WritePos({ d->p0.x + b.rx * static_cast<float>(off), d->p0.y + b.ry * static_cast<float>(off), d->p0.z });
					}
					if (now >= d->nextLog) {
						d->nextLog = now + 0.1;
						const auto& p = Snap().position;
						REX::INFO("P5 run2 [{}] t={:+.2f} s: {} | moved ({:+.3f},{:+.3f}) m from the start", a_tag, now - d->t0, CoverVars(), p.x - d->p0.x, p.y - d->p0.y);
					}
				};
			});
			s.Wait(1.0);
			if (!a_shot.empty()) {
				AddShot(s, a_shot);
			}
			s.Until([d] { return Now() - d->t0 >= 2.2; }, 30.0);
			s.Do([d, a_tag] {
				s_bg = nullptr;
				Rig::ReleaseEverything("p5 run2");
				const auto& p = Snap().position;
				Result(std::format("P5 run2 {}: D held and read every 0.1 s for {:.1f} s (the lines 'P5 run2 [..]' above), the player ended ({:+.3f},{:+.3f}) m from where it began", a_tag, Now() - d->t0, p.x - d->p0.x, p.y - d->p0.y));
				Table(a_tag + " end");
			});
			s.Wait(1.0);
			if (a_mover) {
				// the mover left the player somewhere along the wall: back to the start point
				s.Do([d] { MoveBegin(d->p0, 1.0); });
				s.Until(MoveStep, 3.0);
				s.Wait(0.5);
			}
		}

		void AddRun2(Seq& s)
		{
			AddWalk(s, "run2a D held, the engine moves nothing", false, "r2_key");
			AddWalk(s, "run2b D held, the plugin moves the player at 0.84 m/s", true, "r2_moved");
		}

		// ---- run 8: the stance call with the real stand point and the wall's heading, from 0.26 m short -----------------------------------------------------------------------------
		constexpr float kStandX = -614.96f;
		constexpr float kStandY = 2116.00f;
		constexpr float kWallHeading = 3.0f * kPi / 2.0f;  // toward the wall of Save30 (-x): actor angle z 4.7124 rad

		void AddRun8Case(Seq& s, const bool a_real, const std::string& a_shot)
		{
			auto d = std::make_shared<ShuffleCase>();
			AddMoveTo(s, kStandX + 0.26f, kStandY);
			s.Do([d, a_real] {
				BeginStage(a_real ? "run8 real call" : "run8 here call", 2.0f);
				d->p0 = Snap().position;
				d->angle0 = Snap().angle.z;
				d->target = { kStandX, kStandY, d->p0.z };
				Table(std::string(a_real ? "run8 real" : "run8 here") + " before");
				d->t0 = Now();
				if (a_real) {
					(void)CallStance(3, false, kWallHeading, "run8 real: the stand point and the wall's heading", &d->target);
				} else {
					(void)CallStance(3, false, -FLT_MAX, "run8 here: as the stance call of 21");
				}
			});
			s.Wait(1.3);
			s.Do([d, a_real] {
				std::string line;
				for (const double t : { 0.2, 0.53, 1.0 }) {
					const auto  p = TraceAt(d->t0 + t);
					const float e = std::hypot(p.p.x - d->target.x, p.p.y - d->target.y);
					const float moved = std::hypot(p.p.x - d->p0.x, p.p.y - d->p0.y);
					line += std::format(" +{:.2f} s: ({:.3f},{:.3f}), {:.3f} m from the stand point, moved {:.3f} m, angle z {:.3f} rad ({:+.1f} deg from the start);", t, p.p.x, p.p.y, e, moved, p.angle, AngleDiffDeg(d->angle0, p.angle));
				}
				const auto  p1 = TraceAt(d->t0 + 1.0);
				const float e1 = std::hypot(p1.p.x - d->target.x, p1.p.y - d->target.y);
				Result(std::format("P5 run8 {} from 0.26 m short of the stand point ({:.2f},{:.2f}), the wall at {:.4f} rad:{} {}", a_real ? "the stand point and the wall's heading" : "the 'here' call", d->target.x, d->target.y, kWallHeading, line,
					a_real ? std::format("the real call: {:.3f} m from the stand point at +1.0 s (the 'here' call of 21 took the step back to 0.01 m of where it was called)", e1) : std::format("the here call: {:.3f} m from the stand point at +1.0 s", e1)));
				Table(std::string(a_real ? "run8 real" : "run8 here") + " +1.3 s");
			});
			AddShot(s, a_shot);
			AddExitFn(s, a_real ? "run8 exit after the real call" : "run8 exit after the here call");
			s.Wait(1.2);
		}

		void AddRun8(Seq& s)
		{
			AddRun8Case(s, true, "r8_real");
			AddRun8Case(s, false, "r8_here");
		}

		void AddRun3(Seq& s);

		void AddMode6(Seq& s, const std::string& a_p)
		{
			AddBorrowAndHold(s);
			s.Do([] {
				BeginStage("cover, hold on, stance 3");
				Table("before the enter call");
				(void)CallStance(3, false, -FLT_MAX, "modes 6 enter, as I face");
			});
			s.Wait(1.4);
			s.Do([] { Table("in cover +1.4 s"); RigSample("mode 6 cover +1.4 s"); });
			AddRun1(s);
			AddRun2(s);
			AddExitFn(s, "exit function g(player) before run 8");
			s.Wait(1.2);
			AddRun8(s);
			AddRun3(s);
			AddCleanEnd(s, a_p);
		}

		// ---- run 3: which mode the engine's own aim input chooses, and which exit follows from each state ----------------------------------------------------------------------------
		struct ExitCase
		{
			RE::NiPoint3 p0{};
			float        angle0{ 0.0f };
			double       tg{ 0.0 };
		};

		void AddExitFrom(Seq& s, const std::string& a_tag, std::function<void()> a_into, const double a_settle, const bool a_releaseAimFirst)
		{
			auto d = std::make_shared<ExitCase>();
			AddMoveTo(s, kStandX, kStandY);
			s.Do([a_tag] {
				BeginStage((a_tag + " enter").c_str(), 2.0f);
				Table(a_tag + " before the enter call");
				RE::NiPoint3 st{ kStandX, kStandY, Snap().position.z };
				(void)CallStance(3, false, kWallHeading, (a_tag + " enter").c_str(), &st);
			});
			s.Wait(1.3);
			s.Do([a_into] { a_into(); });
			s.Wait(a_settle);
			if (a_releaseAimFirst) {
				s.Do([] { Rig::MouseRight(false); });
				s.Wait(0.3);
			}
			s.Do([d, a_tag] {
				Table(a_tag + " just before g");
				REX::INFO("P5 run3 [{}] before g: {}", a_tag, CoverVars());
				d->p0 = Snap().position;
				d->angle0 = Snap().angle.z;
				d->tg = Now();
				BeginStage((a_tag + " exit").c_str(), 3.0f);
				(void)CallExit((a_tag + " g").c_str());
			});
			s.Wait(1.2);
			s.Do([d, a_tag] {
				const auto  p = TraceAt(d->tg + 1.0);
				const auto  b = BasisOf(d->angle0);
				const float dx = p.p.x - d->p0.x;
				const float dy = p.p.y - d->p0.y;
				const float right = dx * b.rx + dy * b.ry;
				const float fwd = dx * b.fx + dy * b.fy;
				REX::INFO("P5 run3 [{}] +1.0 s after g: {}", a_tag, CoverVars());
				Result(std::format("P5 run3 {}: g(player) {}: at +1.0 s the player moved {:+.3f} m to its right and {:+.3f} m forward (22.3: exit from idle 0.00 and 0.00; exit_sighted 0.76 / 0.04 (low), 0.90 / -0.37 (standing); exit_sightedover 0.01 / -0.22; exit_sightedoverlow 0, 0; blindfireover 0, 0) and turned {:+.1f} deg (positive = to the right; the exits from idle turn about 71)", a_tag, "called", right, fwd, AngleDiffDeg(d->angle0, p.angle)));
				Table(a_tag + " +1.2 s after g");
			});
			s.Do([] { Rig::ReleaseEverything("p5 run3 case end"); });
			s.Wait(0.6);
		}

		void AddRun3(Seq& s)
		{
			AddExitFrom(s, "run3a from plain idle", [] {}, 0.8, false);
			AddExitFrom(s, "run3b the aim button held 1.5 s", [] { Rig::MouseRight(true); }, 1.5, false);
			AddExitFrom(s, "run3c the aim button let go 0.3 s before g", [] { Rig::MouseRight(true); }, 1.5, true);
			AddExitFrom(s, "run3d in aim over (SightedOverStart)", [] {
				bool accepted{ false };
				const bool ran = AnimRecon::SendEvent("SightedOverStart", accepted);
				REX::INFO("P5 run3: event SightedOverStart sent {}, accepted {}", ran, accepted);
			}, 2.2, false);
			AddExitFrom(s, "run3e in aim over low (SightedOverLowStart)", [] {
				bool accepted{ false };
				const bool ran = AnimRecon::SendEvent("SightedOverLowStart", accepted);
				REX::INFO("P5 run3: event SightedOverLowStart sent {}, accepted {}", ran, accepted);
			}, 2.2, false);
			AddExitFrom(s, "run3f in blind fire over (BlindFireOverStart)", [] {
				bool accepted{ false };
				const bool ran = AnimRecon::SendEvent("BlindFireOverStart", accepted);
				REX::INFO("P5 run3: event BlindFireOverStart sent {}, accepted {}", ran, accepted);
			}, 1.6, false);
		}

		void AddMode7(Seq& s, const std::string& a_p)
		{
			AddBorrowAndHold(s);
			AddRun3(s);
			AddCleanEnd(s, a_p);
		}

		void Build(const Frame::Snapshot&)
		{
			const int mode = Config::Get().p5Mode;
			const bool tall = IsSave29();
			if ((mode == 6 || mode == 7) && tall) {
				Abort("modes 6 and 7 are written for the low wall of Save30");
				return;
			}
			const int  stance = mode == 3 ? 5 : 3;
			const float heading = mode == 3 ? kPi / 2.0f : -FLT_MAX;  // the tall wall of Save29 lies toward heading 90 degrees (east); actor angle z is radians, 0 = +y, growing toward +x
			const std::string prefix = mode == 3 ? "npc_" : "npc_";
			REX::INFO("P5: mode {} on {}: stance {}, heading {}", mode, tall ? "Save29 (the tall wall)" : "Save30 (the low wall)", stance, mode == 3 ? std::format("{:.4f} rad = {:.1f} deg toward the wall (east)", heading, heading * 180.0f / kPi) : std::string("as I face"));

			// to the stand point of the wall; the body is first turned toward it (the camera turned to the heading of the wall, then W for 0.3 s from 2.7 m away: the body turns to the
			// camera when the player moves), because the call's "as I face" heading is the actor's own. Mode 3 gives an explicit heading instead.
			const float sx = tall ? -713.63f : -614.96f;
			const float sy = tall ? 1937.40f : 2116.00f;
			const bool  face = mode != 3;
			const float awayX = tall ? -2.7f : 2.7f;
			const float faceDeg = tall ? 90.0f : 270.0f;
			if (face) {
				AddMoveTo(s_seq, sx + awayX, sy);
				s_seq.Do([] { s_align = Align{}; });
				s_seq.Until([faceDeg] { return AlignStep(faceDeg); }, 8.0);
				s_seq.Wait(0.6);
				s_seq.Do([] { Rig::Tap(Rig::kKeyW, 0.3); });
				s_seq.Wait(1.3);
				s_seq.Do([faceDeg] { REX::INFO("P5: after the W tap the actor angle z is {:.4f} rad ({:.1f} deg); the camera is turned to heading {:.0f} (toward the wall)", Snap().angle.z, Snap().angle.z * 180.0f / kPi, faceDeg); });
			}
			AddMoveTo(s_seq, sx, sy);

			if (mode != 3) {
				AddProbe(s_seq, mode == 1 ? 6.0 : 2.0);
				AddMoveTo(s_seq, sx, sy);  // the probe's jump moved the player
				// the aim field of the process data stayed true about 5 s after the aim button was released (run p5-2: the first call came while it read true): wait until it reads false
				s_seq.Until([] { Fields f; return ReadFields(f) && !f.aim; }, 8.0, [](bool ok) { REX::INFO("P5: the aim field {} before the first call", ok ? "reads false" : "still reads true after 8 s"); });
				s_seq.Wait(1.0);
			}
			if (mode == 1 || mode == 2) {
				AddStage2(s_seq);
			}
			if (mode == 4) {
				AddMode4(s_seq, tall ? 5 : 3, tall ? 4 : 2, tall ? "c" : "b");
			}
			if (mode == 5) {
				AddMode5(s_seq, tall ? 5 : 3, "v");
			}
			if (mode == 6) {
				AddMode6(s_seq, "m6");
			}
			if (mode == 7) {
				AddMode7(s_seq, "m7");
			}
			if (mode == 2 || mode == 3) {
				AddStage3(s_seq, stance, heading, prefix);
				AddStage4(s_seq, stance, heading, std::format("restored_{}", stance));
			}
		}
	}

	void Abort(const char* a_why, const bool a_fault)
	{
		if (s_done) {
			return;
		}
		if (a_fault) {
			s_faulted = true;
		}
		REX::INFO("P5: stopped: {}", a_why);
		Finish(false, a_why);
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
		if (!Config::Get().stanceSpike) {
			REX::WARN("S0 rig: p5: [Anim] bStanceSpike is 0: the script does not run");
			s_done = true;
			return;
		}
		if (!s_started) {
			if (!Ready(a_snap)) {
				return;
			}
			s_started = true;
			Build(a_snap);
		}
		if (!a_snap.valid || a_snap.paused) {
			Abort("the game paused or left the world");
			return;
		}
		if (!Rig::InForeground() && !s_shotWaiting) {
			if (s_fgLostSince < 0.0) {
				s_fgLostSince = a_snap.time;
				Rig::ReleaseEverything("the game window lost the foreground");
				REX::WARN("S0 rig: p5: the game window is not in the foreground; waiting for it (up to 15 s)");
			}
			if (a_snap.time - s_fgLostSince > 15.0) {
				Abort("the game window lost the foreground and did not get it back within 15 s");
			}
			return;
		}
		if (s_fgLostSince >= 0.0 && Rig::InForeground()) {
			REX::INFO("S0 rig: p5: the game window has the foreground again after {:.1f} s", a_snap.time - s_fgLostSince);
			s_fgLostSince = -1.0;
		}
		if (s_watch && MovedFromOrigin() > s_limit) {
			Result(std::format("P5 T5.0 stop rule: the player is {:.2f} m from where the stage began (limit {:.1f} m): the script stops, the cleanup runs", MovedFromOrigin(), s_limit));
			Table("stop rule");
			Abort("the player moved farther than the limit from where a stage began");
			return;
		}
		TraceTick();
		if (Run(s_seq, a_snap.time)) {
			Finish(true, "end of the sequence");
		}
	}
}
