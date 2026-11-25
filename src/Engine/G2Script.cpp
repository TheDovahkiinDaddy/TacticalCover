#include "Engine/G2Script.h"

#include <functional>

#include "Config.h"
#include "Engine/AnimRecon.h"
#include "Engine/Camera.h"
#include "Engine/CameraSettings.h"
#include "Engine/Frame.h"
#include "Engine/Guard.h"
#include "Engine/Input.h"
#include "Engine/PlayerControl.h"
#include "Engine/Rig.h"
#include "Engine/WorldProbe.h"

namespace Engine::G2
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
		};

		constexpr float kPi = 3.14159265358979f;

		bool        s_done{ false };
		bool        s_started{ false };
		std::size_t s_stage{ 0 };
		double      s_stageStart{ 0.0 };
		bool        s_stageFirst{ true };

		// ---- shared helpers ------------------------------------------------------------------------------
		void Result(const std::string& a_text)
		{
			REX::INFO("RESULT {}", a_text);
		}
		// A picture is taken by whoever watches the run (computer use). The script stops its own clock and waits until the
		// file g2_shot_ack.txt appears next to the DLL (it is deleted again), at most 25 s.
		std::filesystem::path AckPath()
		{
			const auto& f = Config::Get().shotAckFile;
			return f.empty() ? Config::Directory() / "g2_shot_ack.txt" : std::filesystem::path(f);
		}
		bool   s_shotPending{ false };
		double s_shotStart{ 0.0 };
		double s_shotNow{ 0.0 };
		void Screenshot(const char* a_name, const double)
		{
			std::error_code ec;
			std::filesystem::remove(AckPath(), ec);
			s_shotPending = true;
			s_shotStart = s_shotNow;
			REX::INFO("SCREENSHOT NOW {} (the script waits for g2_shot_ack.txt, at most 25 s)", a_name);
		}

		struct Basis
		{
			RE::NiPoint3 root{};
			RE::NiPoint3 right{};
			bool         ok{ false };
		};
		Basis MakeBasis(const Camera::Sample& a_s)
		{
			Basis b;
			if (!a_s.ok || a_s.distance < 0.05f) {
				return b;
			}
			const float fx = (a_s.player.x - a_s.rootWorld.x) / a_s.distance;
			const float fy = (a_s.player.y - a_s.rootWorld.y) / a_s.distance;
			b.root = a_s.rootWorld;
			b.right = { fy, -fx, 0.0f };
			b.ok = true;
			return b;
		}
		float Lateral(const Basis& a_b, const Camera::Sample& a_s)
		{
			return (a_s.rootWorld.x - a_b.root.x) * a_b.right.x + (a_s.rootWorld.y - a_b.root.y) * a_b.right.y;
		}

		std::string Reasons(const Frame::Snapshot& a_snap)
		{
			const auto why = PlayerControl::NotAllowed(a_snap);
			if (why.empty()) {
				return "cover allowed";
			}
			std::string s = "not allowed:";
			for (const auto& w : why) {
				s += " [" + w + "]";
			}
			return s;
		}

		// ---- stage: wait until the game is ready -----------------------------------------------------------------
		bool        s_haveAnchor{ false };
		RE::NiPoint3 s_anchor{};
		double      s_stillSince{ 0.0 };
		RE::NiPoint3 s_startPos{};

		bool StageReady(const Ctx& c)
		{
			const auto& snap = c.snap;
			if (!snap.valid || snap.paused || !Input::HookInstalled() || !Camera::GetStats().installed) {
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
					REX::INFO("S0 rig: g2: the camera is not third person (index {}); asking for third person", snap.cameraIndex);
					(void)Camera::ForceThirdPerson();
				}
				return false;
			}
			s_startPos = snap.position;
			REX::INFO("S0 rig: script 'g2' starting at ({:.2f},{:.2f},{:.2f}), third person, game window in the foreground", snap.position.x, snap.position.y, snap.position.z);
			REX::INFO("S5 state at the start: {} | {}", PlayerControl::DescribeVars(PlayerControl::WatchedVars()), PlayerControl::ActorFlags());
			return true;
		}

		// ---- S4 ----------------------------------------------------------------------------------------------
		Basis                 s_base;
		Camera::Sample        s_baseSample;
		Camera::Stats         s_statsAtStart;
		double                s_vis0{ 0 }, s_vis1{ 0 }, s_visN{ 0 }, s_latSum{ 0 };
		float                 s_latMax{ 0.0f };
		std::uint64_t         s_applyAtStart{ 0 };

		bool StageS4Baseline(const Ctx& c)
		{
			if (c.first) {
				s_baseSample = Camera::SampleRoot();
				s_base = MakeBasis(s_baseSample);
				s_statsAtStart = Camera::GetStats();
				REX::INFO("S4 baseline: root world=({:.3f},{:.3f},{:.3f}) local=({:.3f},{:.3f},{:.3f}) parent={} player=({:.3f},{:.3f},{:.3f}) camera-to-player {:.3f} m horizontal",
					s_baseSample.rootWorld.x, s_baseSample.rootWorld.y, s_baseSample.rootWorld.z, s_baseSample.rootLocal.x, s_baseSample.rootLocal.y, s_baseSample.rootLocal.z,
					s_baseSample.hasParent, s_baseSample.player.x, s_baseSample.player.y, s_baseSample.player.z, s_baseSample.distance);
			}
			if (c.t < 3.0) {
				return false;
			}
			const auto st = Camera::GetStats();
			const double dt = c.t;
			Result(std::format("S4 hook: installed={} faulted={} | in {:.1f} s: {} calls on cameraStates[third] ({:.1f}/s), {} calls in all, {} calls on other objects | thread count of the hook see log 'S4 hook: first call'",
				st.installed, st.faulted, dt, st.callsThird - s_statsAtStart.callsThird, static_cast<double>(st.callsThird - s_statsAtStart.callsThird) / dt, st.calls - s_statsAtStart.calls,
				st.callsOther - s_statsAtStart.callsOther));
			s_statsAtStart = st;
			return true;
		}

		void ResetVis()
		{
			s_vis0 = s_vis1 = s_visN = s_latSum = 0;
			s_latMax = 0.0f;
		}
		void SampleLateral(const double a_applied)
		{
			if (!s_base.ok) {
				return;
			}
			const auto  s = Camera::SampleRoot();
			if (!s.ok) {
				return;
			}
			const float lat = Lateral(s_base, s);
			++s_visN;
			s_latSum += lat;
			s_latMax = (std::max)(s_latMax, std::fabs(lat));
			const double mag = std::fabs(a_applied);
			if (std::fabs(lat) > 0.8 * mag && mag > 0.05) {
				++s_vis1;
			} else if (std::fabs(lat) < 0.2 * mag) {
				++s_vis0;
			}
		}
		bool StageS4Side(const Ctx& c, const float a_target, const char* a_name, const double a_sampleFrom, const double a_screenshotAt, const double a_end)
		{
			if (c.first) {
				ResetVis();
				if (a_name[0] == 'r') {
					Camera::SetOffsetTarget(a_target);
				}
			}
			if (c.t >= a_sampleFrom && c.t < a_screenshotAt) {
				SampleLateral(Camera::GetStats().applied);
			}
			static bool shot = false;
			if (c.first) {
				shot = false;
			}
			if (!shot && c.t >= a_screenshotAt) {
				shot = true;
				Screenshot(a_name, a_end - a_screenshotAt);
			}
			if (c.t < a_end) {
				return false;
			}
			const auto st = Camera::GetStats();
			Result(std::format("S4 offset {} (target {:+.2f}, applied {:+.3f}, collision limit {:.2f}): of {:.0f} samples read after the hook, {:.0f} show the offset in full, {:.0f} show none; mean lateral {:+.3f} m, largest {:.3f} m | hook calls that moved the node {}",
				a_name, Camera::OffsetTarget(), st.applied, st.allowed, s_visN, s_vis1, s_vis0, s_visN > 0 ? s_latSum / s_visN : 0.0, s_latMax, st.appliedCalls - s_applyAtStart));
			return true;
		}
		bool StageS4Right(const Ctx& c)
		{
			if (c.first) {
				s_applyAtStart = Camera::GetStats().appliedCalls;
			}
			return StageS4Side(c, 0.45f, "right", 1.5, 3.0, 10.0);
		}
		bool StageS4Left(const Ctx& c)
		{
			if (c.first) {
				s_applyAtStart = Camera::GetStats().appliedCalls;
				Camera::Flip();  // the flip key's code path: +0.45 -> -0.45
			}
			return StageS4Side(c, -0.45f, "left", 1.5, 3.0, 7.0);
		}
		bool StageS4Restore(const Ctx& c)
		{
			if (c.first) {
				Camera::SetOffsetTarget(0.0f);
				ResetVis();
			}
			if (c.t >= 2.5 && c.t < 4.0) {
				SampleLateral(0.0);
			}
			static bool shotDone = false;
			if (c.first) {
				shotDone = false;
			}
			if (!shotDone && c.t >= 3.0) {
				shotDone = true;
				Screenshot("zero", 0.0);
			}
			if (c.t < 4.0) {
				return false;
			}
			const auto s = Camera::SampleRoot();
			const float dx = s.rootWorld.x - s_baseSample.rootWorld.x;
			const float dy = s.rootWorld.y - s_baseSample.rootWorld.y;
			const float dz = s.rootWorld.z - s_baseSample.rootWorld.z;
			Result(std::format("S4 restore: target 0, applied {:+.4f}; camera root differs from the baseline by ({:+.4f},{:+.4f},{:+.4f}) = {:.4f} m; largest lateral deviation seen in the last 1.5 s {:.4f} m",
				Camera::GetStats().applied, dx, dy, dz, std::sqrt(dx * dx + dy * dy + dz * dz), s_latMax));
			return true;
		}
		std::uint64_t s_thirdAtFp{ 0 };
		bool StageS4FirstPerson(const Ctx& c)
		{
			if (c.first) {
				Camera::SetOffsetTarget(0.45f);
				(void)Camera::ForceFirstPerson();
			}
			if (c.t < 1.0) {
				return false;  // let the tween finish
			}
			static bool marked = false;
			if (c.first) {
				marked = false;
			}
			if (!marked) {
				marked = true;
				s_thirdAtFp = Camera::GetStats().callsThird;
				REX::INFO("S4 first person: camera index {}, {} | {}", c.snap.cameraIndex, Reasons(c.snap), PlayerControl::DescribeVars(PlayerControl::WatchedVars()));
			}
			if (c.t < 4.0) {
				return false;
			}
			const auto st = Camera::GetStats();
			Result(std::format("S4 first person: camera index {}; offset target {:+.2f} was set; hook calls on the third-person state in the last 3 s: {} (0 = not called outside third person); applied offset {:+.3f}; {}",
				c.snap.cameraIndex, Camera::OffsetTarget(), st.callsThird - s_thirdAtFp, st.applied, Reasons(c.snap)));
			return true;
		}
		bool StageS4BackToThird(const Ctx& c)
		{
			if (c.first) {
				Camera::SetOffsetTarget(0.0f);
				s_thirdAtFp = Camera::GetStats().callsThird;
				(void)Camera::ForceThirdPerson();
			}
			if (c.t < 3.0) {
				return false;
			}
			const auto st = Camera::GetStats();
			Result(std::format("S4 back to third person: camera index {}; hook calls on the third-person state in the last 3 s: {}; applied offset {:+.3f}; {}", c.snap.cameraIndex,
				st.callsThird - s_thirdAtFp, st.applied, Reasons(c.snap)));
			return true;
		}

		// ---- S5: eased move ---------------------------------------------------------------------------------
		PlayerControl::EasedMove s_move;
		int                      s_moveStep{ 0 };
		RE::NiPoint3             s_moveDir{};  // unit, horizontal
		bool                     s_moveDirOk{ false };
		bool                     s_moveBegun{ false };
		double                   s_movePause{ 0.0 };
		std::array<std::string, 4> s_moveDescr;

		// T2.0: the casts are queued and answered a frame later, so the choice is a batch plus an evaluation once it is done.
		WorldProbe::Batch s_dirBatch;
		bool              s_dirPending{ false };
		double            s_dirStarted{ 0.0 };
		RE::NiPoint3      s_dirPos{};
		float             s_dirHeading{ 0.0f };

		void StartChooseDirection(const Frame::Snapshot& a_snap, const double a_now)
		{
			s_dirBatch.Clear();
			s_dirPending = true;
			s_dirStarted = a_now;
			s_dirPos = a_snap.position;
			s_dirHeading = a_snap.angle.z;
			const float h = s_dirHeading;
			const RE::NiPoint3 fwd{ std::sin(h), std::cos(h), 0.0f };
			const RE::NiPoint3 candidates[] = { { fwd.y, -fwd.x, 0.0f }, { -fwd.y, fwd.x, 0.0f }, { -fwd.x, -fwd.y, 0.0f } };
			for (const auto& d : candidates) {
				const RE::NiPoint3 from{ s_dirPos.x, s_dirPos.y, s_dirPos.z + 1.2f };
				const RE::NiPoint3 low{ s_dirPos.x, s_dirPos.y, s_dirPos.z + 0.4f };
				s_dirBatch.Add(from, { from.x + d.x * 1.6f, from.y + d.y * 1.6f, from.z }, 0.1f);
				s_dirBatch.Add(low, { low.x + d.x * 1.6f, low.y + d.y * 1.6f, low.z }, 0.1f);
				const RE::NiPoint3 above{ s_dirPos.x + d.x * 0.7f, s_dirPos.y + d.y * 0.7f, s_dirPos.z + 1.0f };
				s_dirBatch.Add(above, { above.x, above.y, above.z - 1.5f }, 0.1f);
			}
		}

		void FinishChooseDirection()
		{
			const float h = s_dirHeading;
			const RE::NiPoint3 fwd{ std::sin(h), std::cos(h), 0.0f };
			const RE::NiPoint3 candidates[] = { { fwd.y, -fwd.x, 0.0f }, { -fwd.y, fwd.x, 0.0f }, { -fwd.x, -fwd.y, 0.0f } };
			const char*        names[] = { "right", "left", "back" };
			s_moveDirOk = false;
			for (std::size_t i = 0; i < 3; ++i) {
				const auto& d = candidates[i];
				const auto& chest = s_dirBatch[i * 3];
				const auto& knee = s_dirBatch[i * 3 + 1];
				const auto& floor = s_dirBatch[i * 3 + 2];
				const bool floorOk = floor.ok && floor.hit && std::fabs((s_dirPos.z + 1.0f - 1.0f + 0.1f) - floor.safePoint.z) < 0.35f;
				REX::INFO("S5 direction {}: chest ray hit={} knee ray hit={} floor under target ok={}", names[i], chest.ok && chest.hit, knee.ok && knee.hit, floorOk);
				if (chest.ok && knee.ok && !chest.hit && !knee.hit && floorOk) {
					s_moveDir = d;
					s_moveDirOk = true;
					REX::INFO("S5 eased move: moving {} ({:.2f},{:.2f}) and back", names[i], d.x, d.y);
					return;
				}
			}
			REX::WARN("S5 eased move: no direction is clear for 1.6 m with floor under it; the test is skipped");
		}

		bool StageS5Move(const Ctx& c)
		{
			const auto& cfg = Config::Get();
			if (!cfg.playerControl) {
				return true;
			}
			if (c.first) {
				s_moveStep = 0;
				s_moveBegun = false;
				StartChooseDirection(c.snap, c.now);
			}
			if (s_dirPending) {
				if (!s_dirBatch.Done()) {
					if (c.now - s_dirStarted > 3.0) {
						s_dirPending = false;
						REX::WARN("S5 eased move: the direction casts were not answered within 3 s; the test is skipped");
						return true;
					}
					return false;
				}
				s_dirPending = false;
				FinishChooseDirection();
				if (!s_moveDirOk) {
					return true;
				}
				s_movePause = c.now + 1.0;
			}
			if (c.now < s_movePause) {
				return false;
			}
			if (!s_moveBegun) {
				const bool  ctl = s_moveStep < 2;  // steps 0 and 1: updateCharController = true; 2 and 3: false
				const bool  out = (s_moveStep % 2) == 0;
				const float sign = out ? 1.0f : -1.0f;
				s_move.Begin({ s_moveDir.x * 0.6f * sign, s_moveDir.y * 0.6f * sign, 0.0f }, 0.4f, ctl);
				s_moveBegun = true;
			}
			if (!s_move.Step(c.now)) {
				return false;
			}
			s_moveDescr[static_cast<std::size_t>(s_moveStep)] = s_move.Describe();
			Result(std::format("S5 eased move #{} ({} 0.6 m in 0.4 s): {}", s_moveStep + 1, (s_moveStep % 2) == 0 ? "out" : "back", s_moveDescr[static_cast<std::size_t>(s_moveStep)]));
			++s_moveStep;
			s_moveBegun = false;
			s_movePause = c.now + 0.8;
			if (s_moveStep < 4) {
				return false;
			}
			const auto p = c.snap.position;
			Result(std::format("S5 eased move: back at the start within {:.3f} m horizontally, {:+.3f} m vertically", std::sqrt((p.x - s_startPos.x) * (p.x - s_startPos.x) + (p.y - s_startPos.y) * (p.y - s_startPos.y)), p.z - s_startPos.z));
			return true;
		}

		// ---- S5: sneak -----------------------------------------------------------------------------------------
		struct SneakCandidate
		{
			Rig::Key    key;
			const char* name;
		};
		constexpr std::array<SneakCandidate, 2> kSneakKeys{ { { Rig::kKeyC, "C" }, { Rig::kKeyLCtrl, "Left Ctrl" } } };
		enum class SneakPhase { Tap, WaitChange, Hold, TapBack, WaitBack, Done };
		SneakPhase s_sneakPhase{ SneakPhase::Tap };
		std::size_t s_sneakIdx{ 0 };
		double      s_sneakT{ 0.0 };
		double      s_stance0{ 0.0 };
		std::uint32_t s_flags0{ 0 };
		std::string s_sneakVerdict;

		double Stance()
		{
			return PlayerControl::ReadVar("iSyncActorStance").value_or(-1.0);
		}
		std::uint32_t Flags2()
		{
			const auto p = RE::PlayerCharacter::GetSingleton();
			return p ? p->actorState2 : 0u;
		}

		bool StageS5Sneak(const Ctx& c)
		{
			if (!Config::Get().playerControl) {
				return true;
			}
			if (c.first) {
				s_sneakPhase = SneakPhase::Tap;
				s_sneakIdx = 0;
				s_sneakVerdict.clear();
				REX::INFO("S5 sneak: starting state {} | {}", PlayerControl::DescribeVars(PlayerControl::WatchedVars()), PlayerControl::ActorFlags());
			}
			switch (s_sneakPhase) {
			case SneakPhase::Tap:
				s_stance0 = Stance();
				s_flags0 = Flags2();
				REX::INFO("S5 sneak: tapping {} (stance {:g}, actorState2 0x{:08X})", kSneakKeys[s_sneakIdx].name, s_stance0, s_flags0);
				Rig::Tap(kSneakKeys[s_sneakIdx].key, 0.1);
				s_sneakT = c.now;
				s_sneakPhase = SneakPhase::WaitChange;
				return false;
			case SneakPhase::WaitChange: {
				const bool changed = Stance() != s_stance0 || Flags2() != s_flags0;
				if (changed) {
					const double delay = c.now - s_sneakT;
					REX::INFO("S5 sneak: state changed {:.3f} s after the {} press: stance {:g} -> {:g}, actorState2 0x{:08X} -> 0x{:08X} | {}", delay, kSneakKeys[s_sneakIdx].name, s_stance0, Stance(),
						s_flags0, Flags2(), PlayerControl::DescribeVars(PlayerControl::WatchedVars()));
					s_sneakVerdict = std::format("keyboard {} toggles sneak, first change after {:.3f} s (stance {:g} -> {:g})", kSneakKeys[s_sneakIdx].name, delay, s_stance0, Stance());
					s_sneakT = c.now;
					s_sneakPhase = SneakPhase::Hold;
					return false;
				}
				if (c.now - s_sneakT > 2.0) {
					REX::INFO("S5 sneak: no state change within 2 s of tapping {}", kSneakKeys[s_sneakIdx].name);
					if (++s_sneakIdx >= kSneakKeys.size()) {
						Result("S5 sneak: no keyboard key of C, Left Ctrl changed the stance; see the 'S2 new input' lines for what those keys mean to the game");
						return true;
					}
					s_sneakPhase = SneakPhase::Tap;
				}
				return false;
			}
			case SneakPhase::Hold:
				if (c.now - s_sneakT >= 1.0) {
					s_sneakPhase = SneakPhase::TapBack;
				}
				return false;
			case SneakPhase::TapBack:
				s_stance0 = Stance();
				s_flags0 = Flags2();
				Rig::Tap(kSneakKeys[s_sneakIdx].key, 0.1);
				s_sneakT = c.now;
				s_sneakPhase = SneakPhase::WaitBack;
				return false;
			case SneakPhase::WaitBack: {
				const bool changed = Stance() != s_stance0 || Flags2() != s_flags0;
				if (changed || c.now - s_sneakT > 2.5) {
					const double delay = c.now - s_sneakT;
					Result(std::format("S5 sneak: {}; second press {} after {:.3f} s (stance {:g} -> {:g}); sneak is {} by emulating the key",
						s_sneakVerdict, changed ? "changed it back" : "did NOT change it", delay, s_stance0, Stance(), changed ? "restorable" : "NOT restorable"));
					s_sneakPhase = SneakPhase::Done;
					return true;
				}
				return false;
			}
			default:
				return true;
			}
		}

		// ---- S5: weapon drawn and aiming ----------------------------------------------------------------------------
		enum class WeaponPhase { Check, WaitDraw, Settle, Aim, Release, Done };
		WeaponPhase s_weaponPhase{ WeaponPhase::Check };
		double      s_weaponT{ 0.0 };
		double      s_ready0{ 0.0 };
		std::uint32_t s_wFlags0{ 0 };
		std::string s_aimFirst;
		double      s_aimInput{ -1 }, s_aimCam{ -1 }, s_aimSighted{ -1 }, s_aimFlags{ -1 };
		int         s_cam0{ -1 };
		double      s_sighted0{ 0 };
		std::string s_drawVerdict;

		bool StageS5Weapon(const Ctx& c)
		{
			if (!Config::Get().playerControl) {
				return true;
			}
			if (c.first) {
				s_weaponPhase = WeaponPhase::Check;
			}
			switch (s_weaponPhase) {
			case WeaponPhase::Check:
				s_ready0 = PlayerControl::ReadVar("iSyncRelaxedReadyAlert").value_or(-1.0);
				s_wFlags0 = Flags2();
				if (s_ready0 >= 1.0) {
					s_drawVerdict = "the weapon was already drawn at the start of the stage";
					REX::INFO("S5 weapon: already drawn (iSyncRelaxedReadyAlert={:g})", s_ready0);
					s_weaponPhase = WeaponPhase::Settle;
					s_weaponT = c.now;
				} else {
					REX::INFO("S5 weapon: holstered (iSyncRelaxedReadyAlert={:g}, actorState2 0x{:08X}); tapping R", s_ready0, s_wFlags0);
					Rig::Tap(Rig::kKeyR, 0.1);
					s_weaponT = c.now;
					s_weaponPhase = WeaponPhase::WaitDraw;
				}
				return false;
			case WeaponPhase::WaitDraw: {
				const double ready = PlayerControl::ReadVar("iSyncRelaxedReadyAlert").value_or(-1.0);
				if (ready != s_ready0 || Flags2() != s_wFlags0) {
					s_drawVerdict = std::format("key R draws the weapon: iSyncRelaxedReadyAlert {:g} -> {:g} and actorState2 0x{:08X} -> 0x{:08X} after {:.3f} s", s_ready0, ready, s_wFlags0, Flags2(), c.now - s_weaponT);
					s_weaponPhase = WeaponPhase::Settle;
					s_weaponT = c.now;
				} else if (c.now - s_weaponT > 3.0) {
					s_drawVerdict = "key R did not change the weapon state within 3 s";
					s_weaponPhase = WeaponPhase::Settle;
					s_weaponT = c.now;
				}
				return false;
			}
			case WeaponPhase::Settle:
				if (c.now - s_weaponT < 1.5) {
					return false;
				}
				s_cam0 = c.snap.cameraIndex;
				s_sighted0 = PlayerControl::ReadVar("iSyncSighted").value_or(-1.0);
				s_wFlags0 = Flags2();
				s_aimInput = s_aimCam = s_aimSighted = s_aimFlags = -1;
				REX::INFO("S5 aim: before: camera {} aimHeld={} flags 0x{:08X} | {}", s_cam0, Input::State().AimHeld(), s_wFlags0, PlayerControl::DescribeVars(PlayerControl::WatchedVars()));
				Rig::MouseRight(true);
				s_weaponT = c.now;
				s_weaponPhase = WeaponPhase::Aim;
				return false;
			case WeaponPhase::Aim: {
				const double t = c.now - s_weaponT;
				if (s_aimInput < 0 && Input::State().AimHeld()) {
					s_aimInput = t;
				}
				if (s_aimCam < 0 && c.snap.cameraIndex != s_cam0) {
					s_aimCam = t;
				}
				if (s_aimSighted < 0 && PlayerControl::ReadVar("iSyncSighted").value_or(0.0) != s_sighted0) {
					s_aimSighted = t;
				}
				if (s_aimFlags < 0 && Flags2() != s_wFlags0) {
					s_aimFlags = t;
				}
				if (t < 1.5) {
					return false;
				}
				REX::INFO("S5 aim: while held: camera {} aimHeld={} flags 0x{:08X} | {}", c.snap.cameraIndex, Input::State().AimHeld(), Flags2(), PlayerControl::DescribeVars(PlayerControl::WatchedVars()));
				Result(std::format("S5 aim (synthetic right mouse button held 1.5 s; -1 = never): {}; InputState.AimHeld after {:.3f} s; camera index changed from {} after {:.3f} s (now {}); iSyncSighted changed after {:.3f} s; actorState2 changed after {:.3f} s (0x{:08X} -> 0x{:08X})",
					s_drawVerdict, s_aimInput, s_cam0, s_aimCam, c.snap.cameraIndex, s_aimSighted, s_aimFlags, s_wFlags0, Flags2()));
				Rig::MouseRight(false);
				s_weaponT = c.now;
				s_weaponPhase = WeaponPhase::Release;
				return false;
			}
			case WeaponPhase::Release:
				if (c.now - s_weaponT < 1.2) {
					return false;
				}
				Result(std::format("S5 aim released: camera index {} aimHeld={} flags 0x{:08X} | {}", c.snap.cameraIndex, Input::State().AimHeld(), Flags2(), PlayerControl::DescribeVars(PlayerControl::WatchedVars())));
				s_weaponPhase = WeaponPhase::Done;
				return true;
			default:
				return true;
			}
		}

		bool StageS5Conditions(const Ctx& c)
		{
			if (!Config::Get().playerControl) {
				return true;
			}
			Result(std::format("S5 conditions now (third person, weapon out, standing): {} | detectable: {}", Reasons(c.snap), PlayerControl::DetectableConditions()));
			return true;
		}

		// ---- S6a: the cover events on the player's own graph ----------------------------------------------------------
		RE::NiPoint3 s_pos0{};
		float        s_maxMove{ 0.0f };
		int          s_s6Step{ 0 };
		bool         s_s6Accepted[2]{};

		float Dist(const RE::NiPoint3& a, const RE::NiPoint3& b)
		{
			return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
		}

		// Sends EnterCoverCrouchingRight at t=0.5, samples, ExitCover at t=6.5, samples; a picture at t=2.5.
		bool CoverEventRun(const Ctx& c, const char* a_label, const char* a_shotName)
		{
			if (c.first) {
				s_s6Step = 0;
				s_maxMove = 0.0f;
				s_pos0 = AnimRecon::PlayerPosition();
				AnimRecon::LogCoverVars(std::format("{} baseline", a_label).c_str());
			}
			s_maxMove = (std::max)(s_maxMove, Dist(AnimRecon::PlayerPosition(), s_pos0));
			const auto step = [&](const int a_n, const double a_at) { return s_s6Step == a_n && c.t >= a_at; };
			if (step(0, 0.5)) {
				bool accepted{ false };
				const bool ok = AnimRecon::SendEvent("EnterCoverCrouchingRight", accepted);
				REX::INFO("S6 {}: NotifyAnimationGraphImpl('EnterCoverCrouchingRight') call ok={} returned {}", a_label, ok, accepted);
				s_s6Accepted[0] = ok && accepted;
				s_s6Step = 1;
			} else if (step(1, 0.8)) {
				AnimRecon::LogCoverVars(std::format("{} +0.3s after Enter", a_label).c_str());
				s_s6Step = 2;
			} else if (step(2, 2.5)) {
				AnimRecon::LogCoverVars(std::format("{} +2.0s after Enter", a_label).c_str());
				Screenshot(a_shotName, 4.0);
				s_s6Step = 3;
			} else if (step(3, 6.5)) {
				bool accepted{ false };
				const bool ok = AnimRecon::SendEvent("ExitCover", accepted);
				REX::INFO("S6 {}: NotifyAnimationGraphImpl('ExitCover') call ok={} returned {}", a_label, ok, accepted);
				s_s6Accepted[1] = ok && accepted;
				s_s6Step = 4;
			} else if (step(4, 8.0)) {
				AnimRecon::LogCoverVars(std::format("{} +1.5s after Exit", a_label).c_str());
				Result(std::format("S6 {}: Enter event accepted={} Exit event accepted={}; the player moved at most {:.3f} m from where they stood; final position moved {:.3f} m; {}", a_label, s_s6Accepted[0], s_s6Accepted[1],
					s_maxMove, Dist(AnimRecon::PlayerPosition(), s_pos0), Reasons(c.snap)));
				return true;
			}
			return false;
		}

		bool StageS6a(const Ctx& c)
		{
			if (!Config::Get().animRecon) {
				return true;
			}
			return CoverEventRun(c, "S6a (player graph as shipped)", "s6a-enter-cover");
		}

		// ---- S6c: the player on the NPC gun graph --------------------------------------------------------------------
		int  s_s6cStep{ 0 };
		bool s_removed{ false };

		bool StageS6cPrepare(const Ctx& c)
		{
			if (!Config::Get().animRecon) {
				return true;
			}
			const auto step = [&](const int a_n, const double a_at) { return s_s6cStep == a_n && c.t >= a_at; };
			if (c.first) {
				s_s6cStep = 0;
				s_removed = false;
				Result(std::format("S6c keyword: {}", AnimRecon::InspectArchetypeKeyword()));
				AnimRecon::LogCoverVars("S6c before");
			}
			if (step(0, 0.5)) {
				s_removed = AnimRecon::RemoveArchetypeFromBase();
				REX::INFO("S6c: RemoveKeyword(AnimArchetypePlayer) on the player's actor base returned {}", s_removed);
				REX::INFO("S6c after removal: {}", AnimRecon::InspectArchetypeKeyword());
				(void)AnimRecon::DrawWeapon(false);
				s_s6cStep = 1;
			} else if (step(1, 2.0)) {
				(void)AnimRecon::DrawWeapon(true);
				s_s6cStep = 2;
			} else if (step(2, 5.0)) {
				AnimRecon::LogCoverVars("S6c keyword removed, weapon re-drawn");
				return true;
			}
			return false;
		}

		bool StageS6cEvents(const Ctx& c)
		{
			if (!Config::Get().animRecon) {
				return true;
			}
			return CoverEventRun(c, "S6c (keyword removed, weapon re-drawn)", "s6c-enter-cover");
		}

		bool StageS6cRestore(const Ctx& c)
		{
			if (!Config::Get().animRecon) {
				return true;
			}
			if (c.first) {
				s_removed = false;
				const bool back = AnimRecon::RestoreArchetypeOnBase();
				Result(std::format("S6c: the keyword was put back on the actor base: {} | {}", back, AnimRecon::InspectArchetypeKeyword()));
				(void)AnimRecon::DrawWeapon(false);
			}
			static bool redrawn = false;
			if (c.first) {
				redrawn = false;
			}
			if (!redrawn && c.t >= 2.0) {
				redrawn = true;
				(void)AnimRecon::DrawWeapon(true);
			}
			if (c.t < 5.0) {
				return false;
			}
			AnimRecon::LogCoverVars("S6c keyword restored, weapon re-drawn");
			return true;
		}


		// ---- console typing and route A (planning session's request, 2026-11-16) ------------------------------------
		bool        s_allowPause{ false };  // the console pauses the game; the pause check is off while the route A stage runs
		std::string s_typeText;
		std::size_t s_typeIdx{ 0 };
		double      s_typeNext{ 0.0 };
		bool        s_typing{ false };

		void StartTyping(const std::string& a_text)
		{
			s_typeText = a_text;
			s_typeIdx = 0;
			s_typeNext = 0.0;
			s_typing = true;
			REX::INFO("S6 console: typing '{}'", a_text);
		}
		void PumpTyping(const double a_now)
		{
			if (!s_typing || a_now < s_typeNext) {
				return;
			}
			if (s_typeIdx >= s_typeText.size()) {
				s_typing = false;
				return;
			}
			const char c = s_typeText[s_typeIdx++];
			Rig::Key   key{};
			bool       shift{ false };
			if (!Rig::KeyForChar(c, key, shift)) {
				REX::WARN("S6 console: no key for character '{}', skipped", c);
				return;
			}
			if (shift) {
				Rig::Tap(Rig::kKeyShift, 0.12);
			}
			Rig::Tap(key, 0.1);
			s_typeNext = a_now + (shift ? 0.28 : 0.18);
		}

		struct RStep
		{
			double                delay;       // seconds after the previous step ran (and typing finished)
			std::function<void()> fn;
			bool                  waitTyping;  // not used by the stage loop (typing always finishes first); kept for reading
		};
		std::vector<RStep> s_rSteps;
		std::size_t        s_rIdx{ 0 };
		double             s_rNextAt{ 0.0 };
		RE::NiPoint3       s_rPos0{};

		void ResolveReport(const char* a_label)
		{
			static constexpr std::array<const char*, 6> names{ "iSyncStandingCrouching", "iSyncCoverStates", "bIsEnteringCover", "bIsExitingCover", "bIsShuffling", "iIsPlayer" };
			std::string s;
			int         resolved = 0;
			for (const char* n : names) {
				if (const auto v = PlayerControl::ReadVar(n)) {
					s += std::format(" {}={:g}", n, *v);
					if (std::string_view(n) != "iIsPlayer") {
						++resolved;
					}
				} else {
					s += std::format(" {}=<unresolved>", n);
				}
			}
			Result(std::format("S6 route A [{}]: {} of the 5 cover variables resolve;{}", a_label, resolved, s));
		}

		void AddVariant(const char* a_label, const std::string& a_arg, const bool a_withHelp)
		{
			const std::string label = a_label;
			const auto        add = [](const double a_delay, std::function<void()> a_fn, const bool a_wait = false) { s_rSteps.push_back({ a_delay, std::move(a_fn), a_wait }); };
			add(0.5, [] { Rig::Tap(Rig::kKeyGrave, 0.1); });  // open the console
			if (a_withHelp) {
				add(1.0, [] { StartTyping("help ChangeAnimArchetype"); });
				add(0.3, [] { Rig::Tap(Rig::kKeyEnter, 0.1); }, true);
			add(0.8, [] { Rig::Tap(Rig::kKeyE, 0.1); });  // accepts the "console commands disable achievements" dialog
				add(1.2, [] { Screenshot("console-help", 0.0); });
			}
			add(0.8, [a_arg] { StartTyping("player.cf \"Actor.ChangeAnimArchetype\" " + a_arg); });
			add(0.3, [] { Rig::Tap(Rig::kKeyEnter, 0.1); }, true);
			add(0.8, [] { Rig::Tap(Rig::kKeyE, 0.1); });  // accepts the "console commands disable achievements" dialog
			add(1.8, [label] { Screenshot(("console-" + label).c_str(), 0.0); });
			add(0.3, [] { Rig::Tap(Rig::kKeyGrave, 0.1); });  // close the console
			add(1.5, [label] {
				AnimRecon::LogCoverVars(("route A [" + label + "] after the console call").c_str());
				(void)AnimRecon::DrawWeapon(false);
			});
			add(1.5, [] { (void)AnimRecon::DrawWeapon(true); });
			add(3.0, [label] {
				AnimRecon::LogCoverVars(("route A [" + label + "] weapon re-drawn").c_str());
				ResolveReport(label.c_str());
				s_rPos0 = AnimRecon::PlayerPosition();
				bool       accepted{ false };
				const bool ok = AnimRecon::SendEvent("EnterCoverCrouchingRight", accepted);
				REX::INFO("S6 route A [{}]: EnterCoverCrouchingRight call ok={} returned {}", label, ok, accepted);
			});
			add(1.3, [label] { AnimRecon::LogCoverVars(("route A [" + label + "] 1.3 s after Enter").c_str()); });
			add(0.3, [label] {
				if (PlayerControl::ReadVar("iSyncStandingCrouching")) {  // only when the cover graph is really there: fewer pauses for pictures
					Screenshot((label + "-enter-cover").c_str(), 0.0);
				}
			});
			add(0.5, [label] {
				bool       accepted{ false };
				const bool ok = AnimRecon::SendEvent("ExitCover", accepted);
				REX::INFO("S6 route A [{}]: ExitCover call ok={} returned {}; the player moved {:.3f} m since Enter", label, ok, accepted, Dist(AnimRecon::PlayerPosition(), s_rPos0));
			});
			add(1.5, [label] {
				AnimRecon::LogCoverVars(("route A [" + label + "] after ExitCover").c_str());
				s_rPos0 = AnimRecon::PlayerPosition();
				Rig::Tap(Rig::kKeyW, 0.5);
			});
			add(1.5, [label] {
				Result(std::format("S6 route A [{}]: W held 0.5 s moved the player {:.2f} m (0 = cannot move)", label, Dist(AnimRecon::PlayerPosition(), s_rPos0)));
				Rig::MouseRight(true);
			});
			add(1.2, [label] {
				Result(std::format("S6 route A [{}]: right mouse held 1.2 s: InputState.AimHeld={} actorState2 0x{:08X} | {}", label, Input::State().AimHeld(), Flags2(),
					PlayerControl::DescribeVars(PlayerControl::WatchedVars())));
				Rig::MouseRight(false);
			});
			// restore AnimArchetypePlayer
			add(1.0, [] { Rig::Tap(Rig::kKeyGrave, 0.1); });
			add(0.8, [] { StartTyping("player.cf \"Actor.ChangeAnimArchetype\" 0006B508"); });
			add(0.3, [] { Rig::Tap(Rig::kKeyEnter, 0.1); }, true);
			add(0.8, [] { Rig::Tap(Rig::kKeyE, 0.1); });  // accepts the "console commands disable achievements" dialog
			add(1.2, [] { Rig::Tap(Rig::kKeyGrave, 0.1); });
			add(1.5, [] { (void)AnimRecon::DrawWeapon(false); });
			add(1.5, [] { (void)AnimRecon::DrawWeapon(true); });
			add(3.0, [label] {
				AnimRecon::LogCoverVars(("route A [" + label + "] after restoring AnimArchetypePlayer").c_str());
				ResolveReport((label + " restored").c_str());
			});
		}

		bool StageRouteA(const Ctx& c)
		{
			if (!Config::Get().animRecon) {
				return true;
			}
			if (c.first) {
				s_allowPause = true;
				s_rSteps.clear();
				s_rIdx = 0;
				AddVariant("pirate", "0003818E", false);  // AnimArchetypePirate
				AddVariant("none", "None", false);
				s_rNextAt = s_rSteps[0].delay;
				AnimRecon::LogCoverVars("route A baseline");
			}
			if (s_typing) {
				return false;
			}
			if (s_rIdx >= s_rSteps.size()) {
				s_allowPause = false;
				return true;
			}
			if (c.t < s_rNextAt) {
				return false;
			}
			s_rSteps[s_rIdx].fn();
			++s_rIdx;
			if (s_rIdx < s_rSteps.size()) {
				s_rNextAt = c.t + s_rSteps[s_rIdx].delay;
			}
			return false;
		}


		// ---- route A, direct call of the engine's ChangeAnimArchetype (planning session, 2026-11-16) -------------------
		struct DStep
		{
			double                delay;
			std::function<void()> fn;
		};
		std::vector<DStep> s_dSteps;
		std::size_t        s_dIdx{ 0 };
		double             s_dNextAt{ 0.0 };
		RE::NiPoint3       s_dPos0{};
		std::string        s_dBaseline;

		bool CoverVarsResolve()
		{
			return PlayerControl::ReadVar("iSyncStandingCrouching").has_value() || PlayerControl::ReadVar("iSyncCoverStates").has_value();
		}

		void DAdd(const double a_delay, std::function<void()> a_fn)
		{
			s_dSteps.push_back({ a_delay, std::move(a_fn) });
		}

		void ChangeStep(const std::string& a_label, const RE::TESFormID a_id)
		{
			bool       returned{ false };
			const bool ran = AnimRecon::ChangeArchetype(a_id, returned);
			REX::INFO("S6 route A [{}]: ChangeAnimArchetype({:08X}) ran={} returned {}", a_label, a_id, ran, returned);
			Result(std::format("S6 route A [{}]: call ran={} returned {}", a_label, ran, returned));
		}

		void AddArchetypeTest(const std::string& a_label, const RE::TESFormID a_id)
		{
			DAdd(1.0, [=] { ChangeStep(a_label, a_id); });
			DAdd(1.0, [=] {
				AnimRecon::LogCoverVars(("route A [" + a_label + "] 1 s after the call").c_str());
				ResolveReport((a_label + " +1s").c_str());
				if (!CoverVarsResolve()) {
					(void)AnimRecon::DrawWeapon(false);
				}
			});
			DAdd(1.5, [=] {
				if (!CoverVarsResolve()) {
					(void)AnimRecon::DrawWeapon(true);
				}
			});
			DAdd(2.5, [=] {
				AnimRecon::LogCoverVars(("route A [" + a_label + "] after the optional holster/draw").c_str());
				ResolveReport((a_label + " settled").c_str());
				s_dPos0 = AnimRecon::PlayerPosition();
				bool       accepted{ false };
				const bool ok = AnimRecon::SendEvent("EnterCoverCrouchingRight", accepted);
				REX::INFO("S6 route A [{}]: EnterCoverCrouchingRight call ok={} returned {} (cover variables resolve: {})", a_label, ok, accepted, CoverVarsResolve());
			});
			DAdd(0.5, [=] { AnimRecon::LogCoverVars(("route A [" + a_label + "] 0.5 s after Enter").c_str()); });
			DAdd(1.0, [=] {
				AnimRecon::LogCoverVars(("route A [" + a_label + "] 1.5 s after Enter").c_str());
				if (CoverVarsResolve()) {
					Screenshot((a_label + "-enter-cover").c_str(), 0.0);
				}
			});
			DAdd(1.0, [=] {
				AnimRecon::LogCoverVars(("route A [" + a_label + "] 2.5 s after Enter").c_str());
				bool       accepted{ false };
				const bool ok = AnimRecon::SendEvent("ExitCover", accepted);
				REX::INFO("S6 route A [{}]: ExitCover call ok={} returned {}; the player moved {:.3f} m since Enter", a_label, ok, accepted, Dist(AnimRecon::PlayerPosition(), s_dPos0));
			});
			DAdd(1.5, [=] {
				AnimRecon::LogCoverVars(("route A [" + a_label + "] 1.5 s after ExitCover").c_str());
				s_dPos0 = AnimRecon::PlayerPosition();
				Rig::Tap(Rig::kKeyW, 0.5);
			});
			DAdd(1.5, [=] {
				Result(std::format("S6 route A [{}]: W held 0.5 s moved the player {:.2f} m (0 = cannot move)", a_label, Dist(AnimRecon::PlayerPosition(), s_dPos0)));
				Rig::MouseRight(true);
			});
			DAdd(1.2, [=] {
				Result(std::format("S6 route A [{}]: right mouse held 1.2 s: InputState.AimHeld={} actorState2 0x{:08X} | {}", a_label, Input::State().AimHeld(), Flags2(),
					PlayerControl::DescribeVars(PlayerControl::WatchedVars())));
				Rig::MouseRight(false);
			});
		}

		bool StageRouteAD(const Ctx& c)
		{
			if (!Config::Get().animRecon) {
				return true;
			}
			if (c.first) {
				s_dSteps.clear();
				s_dIdx = 0;
				// draw the weapon first when it is holstered
				DAdd(0.5, [] {
					if (PlayerControl::ReadVar("iSyncRelaxedReadyAlert").value_or(0.0) < 1.0) {
						Rig::Tap(Rig::kKeyR, 0.1);
					}
				});
				DAdd(3.0, [] {
					AnimRecon::LogCoverVars("route A baseline (weapon drawn)");
					ResolveReport("baseline");
					REX::INFO("S6 route A baseline: {} | {}", PlayerControl::DescribeVars(PlayerControl::WatchedVars()), PlayerControl::ActorFlags());
				});
				AddArchetypeTest("pirate", 0x0003818E);  // AnimArchetypePirate
				AddArchetypeTest("null", 0);             // clear the archetype
				DAdd(1.0, [] {
					bool       returned{ false };
					const bool ran = AnimRecon::RestoreArchetype(returned);
					REX::INFO("S6 route A: restore with AnimArchetypePlayer ran={} returned {}", ran, returned);
					if (!ran || !returned) {
						REX::ERROR("S6 route A: RESTORE OF ANIMARCHETYPEPLAYER FAILED (ran={} returned={}): the next run must start from a clean load", ran, returned);
					}
				});
				DAdd(1.0, [] { (void)AnimRecon::DrawWeapon(false); });
				DAdd(1.5, [] { (void)AnimRecon::DrawWeapon(true); });
				DAdd(2.5, [] {
					AnimRecon::LogCoverVars("route A after the restore");
					ResolveReport("restored");
					Result(std::format("S6 route A restore check: {}", AnimRecon::InspectArchetypeKeyword()));
				});
				s_dNextAt = s_dSteps[0].delay;
			}
			if (s_dIdx >= s_dSteps.size()) {
				return true;
			}
			if (c.t < s_dNextAt) {
				return false;
			}
			s_dSteps[s_dIdx].fn();
			++s_dIdx;
			if (s_dIdx < s_dSteps.size()) {
				s_dNextAt = c.t + s_dSteps[s_dIdx].delay;
			}
			return false;
		}


		// ---- S4 settings route: the engine's own over-the-shoulder settings, measured by numbers (planning session) -----------
		// The camera root's offset from the player is split into sideways (along the baseline camera's right vector), back (distance
		// behind the player along the baseline view direction, horizontal) and up. Static player, no camera input.
		std::vector<DStep> s_cSteps;
		std::size_t        s_cIdx{ 0 };
		double             s_cNextAt{ 0.0 };
		Basis              s_cBase;
		bool               s_cHaveBase{ false };
		float              s_cPosX0{ 0.0f }, s_cCombatX0{ 0.0f };
		bool               s_cHaveBases{ false };
		bool               s_cLeft0{ false };

		void CMeasure(const char* a_label)
		{
			const auto s = Camera::SampleRoot();
			if (!s.ok) {
				Result(std::format("S4 settings [{}]: camera root not readable", a_label));
				return;
			}
			if (!s_cHaveBase) {
				s_cBase = MakeBasis(s);
				s_cHaveBase = true;
			}
			float posX{ 0.0f }, combatX{ 0.0f }, posZ{ 0.0f };
			bool  left{ false };
			(void)CameraSettings::GetFloat("fOverShoulderPosX:Camera", posX);
			(void)CameraSettings::GetFloat("fOverShoulderCombatPosX:Camera", combatX);
			(void)CameraSettings::GetFloat("fOverShoulderPosZ:Camera", posZ);
			(void)CameraSettings::GetBool("bTestLeftSideCameras:Camera", left);
			const float dx = s.rootWorld.x - s.player.x;
			const float dy = s.rootWorld.y - s.player.y;
			const float dz = s.rootWorld.z - s.player.z;
			float       side = 0.0f, back = 0.0f;
			if (s_cBase.ok) {
				const float fx = -s_cBase.right.y;  // forward from the right vector: right = (fy, -fx)
				const float fy = s_cBase.right.x;
				side = dx * s_cBase.right.x + dy * s_cBase.right.y;
				back = -(dx * fx + dy * fy);
			}
			Result(std::format("S4 settings [{}]: sideways {:+.3f} back {:.3f} up {:+.3f} | raw root-player ({:+.3f},{:+.3f},{:+.3f}) | weapon {} | PosX {:+.3f} CombatPosX {:+.3f} PosZ {:+.3f} TestLeft {}",
				a_label, side, back, dz, dx, dy, dz, (Flags2() & 2u) ? "drawn" : "holstered", posX, combatX, posZ, left));
		}

		void CSet(const float a_dx)
		{
			const bool a = CameraSettings::SetFloat("fOverShoulderPosX:Camera", s_cPosX0 + a_dx);
			const bool b = CameraSettings::SetFloat("fOverShoulderCombatPosX:Camera", s_cCombatX0 + a_dx);
			REX::INFO("S4 settings: PosX and CombatPosX set to base {:+.2f} (writes ok: {} {})", a_dx, a, b);
		}

		void CAddPass(const char* a_state)
		{
			const std::string st = a_state;
			const auto        add = [](const double a_delay, std::function<void()> a_fn) { s_cSteps.push_back({ a_delay, std::move(a_fn) }); };
			for (const float step : { 0.4f, -0.4f }) {
				add(0.3, [step] { CSet(step); });
				add(0.5, [st, step] { CMeasure((st + " PosX/CombatPosX base" + (step > 0 ? "+0.4" : "-0.4") + " after 0.5 s").c_str()); });
				add(1.5, [st, step] { CMeasure((st + " PosX/CombatPosX base" + (step > 0 ? "+0.4" : "-0.4") + " after 2 s").c_str()); });
				// does a camera state change make the engine read the settings again?
				add(0.3, [] { (void)Camera::ForceFirstPerson(); });
				add(1.5, [] { (void)Camera::ForceThirdPerson(); });
				add(3.0, [st, step] { CMeasure((st + " PosX/CombatPosX base" + (step > 0 ? "+0.4" : "-0.4") + " after a first/third person toggle").c_str()); });
			}
			add(0.3, [] { CSet(0.0f); });
			add(0.5, [st] { CMeasure((st + " restored to base after 0.5 s").c_str()); });
			add(1.5, [st] { CMeasure((st + " restored to base after 2 s").c_str()); });
			add(0.3, [] { (void)CameraSettings::SetBool("bTestLeftSideCameras:Camera", true); REX::INFO("S4 settings: bTestLeftSideCameras set true"); });
			add(2.0, [st] { CMeasure((st + " bTestLeftSideCameras on after 2 s").c_str()); });
			add(0.3, [] { (void)Camera::ForceFirstPerson(); });
			add(1.5, [] { (void)Camera::ForceThirdPerson(); });
			add(3.0, [st] { CMeasure((st + " bTestLeftSideCameras on after a first/third person toggle").c_str()); });
			add(0.3, [] { (void)CameraSettings::SetBool("bTestLeftSideCameras:Camera", false); REX::INFO("S4 settings: bTestLeftSideCameras set false"); });
			add(2.0, [st] { CMeasure((st + " bTestLeftSideCameras off after 2 s").c_str()); });
		}

		bool StageS4Settings(const Ctx& c)
		{
			if (!Config::Get().cameraSettings) {
				return true;
			}
			if (c.first) {
				s_cSteps.clear();
				s_cIdx = 0;
				s_cHaveBase = false;
				s_cHaveBases = false;
				Camera::SetOffsetTarget(0.0f);
				Camera::SetMode(0);
				const auto add = [](const double a_delay, std::function<void()> a_fn) { s_cSteps.push_back({ a_delay, std::move(a_fn) }); };
				add(1.0, [] {
					s_cHaveBases = CameraSettings::GetFloat("fOverShoulderPosX:Camera", s_cPosX0) && CameraSettings::GetFloat("fOverShoulderCombatPosX:Camera", s_cCombatX0);
					(void)CameraSettings::GetBool("bTestLeftSideCameras:Camera", s_cLeft0);
					REX::INFO("S4 settings: base PosX {:+.3f} CombatPosX {:+.3f} TestLeft {} (readable: {})", s_cPosX0, s_cCombatX0, s_cLeft0, s_cHaveBases);
					if (!s_cHaveBases) {
						Result("S4 settings: fOverShoulderPosX / fOverShoulderCombatPosX are not readable as float settings; stage skipped");
					}
					CMeasure("holstered baseline");
					if (PlayerControl::ReadVar("iSyncRelaxedReadyAlert").value_or(0.0) >= 1.0) {
						(void)AnimRecon::DrawWeapon(false);  // start from holstered
					}
				});
				add(3.0, [] { CMeasure("holstered baseline (second reading)"); });
				CAddPass("holstered");
				add(0.3, [] { Rig::Tap(Rig::kKeyR, 0.1); });
				add(3.0, [] { CMeasure("drawn baseline"); });
				CAddPass("drawn");
				add(0.5, [] {
					(void)CameraSettings::SetFloat("fOverShoulderPosX:Camera", s_cPosX0);
					(void)CameraSettings::SetFloat("fOverShoulderCombatPosX:Camera", s_cCombatX0);
					(void)CameraSettings::SetBool("bTestLeftSideCameras:Camera", s_cLeft0);
					REX::INFO("S4 settings: every value written back to its starting value");
				});
				add(2.5, [] { CMeasure("drawn, all values restored"); });
				s_cNextAt = s_cSteps[0].delay;
			}
			if (s_cIdx >= s_cSteps.size()) {
				return true;
			}
			if (c.t < s_cNextAt) {
				return false;
			}
			if (!s_cHaveBases && s_cIdx > 0) {
				return true;
			}
			s_cSteps[s_cIdx].fn();
			++s_cIdx;
			if (s_cIdx < s_cSteps.size()) {
				s_cNextAt = c.t + s_cSteps[s_cIdx].delay;
			}
			return false;
		}


		// ---- S4 call-site hook: sideways / back / up at +0.45, -0.45 and 0, holstered and drawn (planning session) -----------
		std::vector<DStep> s_sSteps;
		std::size_t        s_sIdx{ 0 };
		double             s_sNextAt{ 0.0 };

		void SAddState(const std::string& a_state)
		{
			const auto add = [](const double a_delay, std::function<void()> a_fn) { s_sSteps.push_back({ a_delay, std::move(a_fn) }); };
			for (const float off : { 0.45f, -0.45f, 0.0f }) {
				const std::string label = std::format("{} offset {:+.2f}", a_state, off);
				add(0.3, [off] {
					Camera::SetMode(3);
					Camera::SetOffsetTarget(off);
				});
				add(2.5, [label] {
					const auto st = Camera::GetStats();
					CMeasure((label + " (call-site hook)").c_str());
					REX::INFO("S4 call site counters: site calls {} applied {} | applied offset {:+.3f} | collision limit {:.2f}", st.siteCalls, st.siteApplied, st.applied, st.allowed);
					Screenshot(label.c_str(), 0.0);
				});
			}
			// the hook switched off while the offset is still requested: the numbers must be the baseline again
			add(0.3, [] {
				Camera::SetOffsetTarget(0.45f);
				Camera::SetMode(0);
			});
			add(2.5, [a_state] { CMeasure((a_state + " hook switched off with +0.45 requested").c_str()); });
			add(0.3, [] { Camera::SetOffsetTarget(0.0f); });
		}

		bool StageS4CallSite(const Ctx& c)
		{
			if (c.first) {
				s_sSteps.clear();
				s_sIdx = 0;
				s_cHaveBase = false;
				Camera::SetOffsetTarget(0.0f);
				Camera::SetMode(3);
				const auto add = [](const double a_delay, std::function<void()> a_fn) { s_sSteps.push_back({ a_delay, std::move(a_fn) }); };
				add(1.0, [] {
					const auto st = Camera::GetStats();
					REX::INFO("S4 call site: installed={} calls so far {}", st.siteInstalled, st.siteCalls);
					if (Flags2() & 2u) {
						(void)AnimRecon::DrawWeapon(false);
					}
				});
				add(3.0, [] { CMeasure("holstered baseline (no offset)"); });
				SAddState("holstered");
				add(0.3, [] { Rig::Tap(Rig::kKeyR, 0.1); });
				add(3.0, [] { CMeasure("drawn baseline (no offset)"); });
				SAddState("drawn");
				add(0.5, [] {
					Camera::SetOffsetTarget(0.0f);
					Camera::SetMode(0);
				});
				add(2.5, [] { CMeasure("drawn, everything off again"); });
				s_sNextAt = s_sSteps[0].delay;
			}
			if (s_sIdx >= s_sSteps.size()) {
				return true;
			}
			if (c.t < s_sNextAt) {
				return false;
			}
			s_sSteps[s_sIdx].fn();
			++s_sIdx;
			if (s_sIdx < s_sSteps.size()) {
				s_sNextAt = c.t + s_sSteps[s_sIdx].delay;
			}
			return false;
		}

		// ---- S4b: pivot shift (mode 4) and its probe, the same nine cases for mode 3 (the present hook) and mode 4 ---------------
		// Cases: weapon away, drawn (R), aimed (right mouse held until iSyncSighted reads 1), each at offset 0, +0.45 and -0.45.
		// Every case logs one `RESULT S4b probe` line (from the Update hook), one `S4 settings` line (camera root against the first
		// baseline) and one `S4b counters` line.
		struct BStep
		{
			double                 delay;
			std::function<void()>  fn;
			std::function<bool()>  gate;     // when set, the step also waits for this to become true (at most maxWait s extra)
			double                 maxWait{ 0.0 };
		};
		std::vector<BStep>         s_bSteps;
		std::size_t                s_bIdx{ 0 };
		double                     s_bNextAt{ 0.0 };
		float                      s_bAimSide0{ 0.0f };
		bool                       s_bHaveAimSide0{ false };
		bool                       s_bMoveActive{ false };
		PlayerControl::EasedMove   s_bMove;

		void BAdd(const double a_delay, std::function<void()> a_fn, std::function<bool()> a_gate = {}, const double a_maxWait = 0.0)
		{
			s_bSteps.push_back({ a_delay, std::move(a_fn), std::move(a_gate), a_maxWait });
		}

		bool BSighted()
		{
			return PlayerControl::ReadVar("iSyncSighted").value_or(0.0) >= 1.0;
		}

		void BRequestProbe(const std::string& a_label)
		{
			const double sighted = PlayerControl::ReadVar("iSyncSighted").value_or(-1.0);
			const double isSighted = PlayerControl::ReadVar("iIsSighted").value_or(-1.0);
			Camera::ProbeRequest(std::format("{} | iSyncSighted {:g} iIsSighted {:g} weapon {}", a_label, sighted, isSighted, (Flags2() & 2u) ? "drawn" : "holstered"));
		}

		void BCounters(const std::string& a_label)
		{
			const auto st = Camera::GetStats();
			Result(std::format("S4b counters [{}]: mode {} | pivot writes {} restored {} changed {} null {} | call-site calls {} applied {} | applied offset {:+.3f} target {:+.3f}", a_label, Camera::Mode(), st.pivotWrites, st.pivotRestored, st.pivotChanged, st.pivotNull,
				st.siteCalls, st.siteApplied, st.applied, Camera::OffsetTarget()));
		}

		void BAddUnaimed(const std::string& a_state)
		{
			for (const float off : { 0.0f, 0.45f, -0.45f }) {
				const std::string label = std::format("{} offset {:+.2f}", a_state, off);
				BAdd(0.3, [off] { Camera::SetOffsetTarget(off); });
				BAdd(2.5, [label] { BRequestProbe(label); });
				BAdd(0.4, [label] {
					CMeasure(label.c_str());
					BCounters(label);
				});
			}
		}

		// One aimed case: the target is set while not aiming, then the button goes down, the step waits for iSyncSighted = 1 (at most
		// 3.5 s extra), the probe is requested, and the button comes up again afterwards (a_shot: a picture is taken while it is held).
		void BAddAimedCase(const std::string& a_label, const float a_off, const bool a_shot, const bool a_storeSide, const std::function<void()>& a_before = {})
		{
			BAdd(0.3, [a_off, a_before] {
				Camera::SetOffsetTarget(a_off);
				if (a_before) {
					a_before();
				}
			});
			BAdd(0.4, [] { Rig::MouseRight(true); });
			BAdd(0.8, [a_label] { BRequestProbe(a_label); }, [] { return BSighted(); }, 3.5);
			BAdd(0.6, [a_label, a_storeSide] {
				const auto p = Camera::LastProbe();
				if (a_storeSide && p.ok) {
					s_bAimSide0 = p.side;
					s_bHaveAimSide0 = true;
					REX::INFO("S4b: aimed, offset 0: the camera is {:+.3f} m sideways in the yaw frame (the left-side picture uses -2 x this)", p.side);
				}
				CMeasure(a_label.c_str());
				BCounters(a_label);
				if (!p.ok) {
					REX::WARN("S4b: no probe result for [{}]", a_label);
				}
			});
			if (a_shot) {
				BAdd(0.1, [a_label] { Screenshot(a_label.c_str(), 0.0); });
			}
			BAdd(0.1, [] { Rig::MouseRight(false); });
			BAdd(1.6, [] {});
		}

		void BBegin(const int a_mode, const bool a_cases)
		{
			s_bSteps.clear();
			s_bIdx = 0;
			s_bMoveActive = false;
			s_cHaveBase = false;
			s_bHaveAimSide0 = false;
			Camera::SetOffsetTarget(0.0f);
			Camera::SetMode(a_mode);
			BAdd(1.0, [a_mode] {
				const auto st = Camera::GetStats();
				REX::INFO("S4b: mode {} hook installed {} call site installed {} | start state: weapon {}", a_mode, st.installed, st.siteInstalled, (Flags2() & 2u) ? "drawn" : "holstered");
				if (Flags2() & 2u) {
					(void)AnimRecon::DrawWeapon(false);
				}
			});
			BAdd(3.0, [] {});
			if (!a_cases) {
				return;
			}
			BAddUnaimed("away");
			BAdd(0.3, [] { Rig::Tap(Rig::kKeyR, 0.1); });
			BAdd(3.0, [] {});
			BAddUnaimed("drawn");
			for (const float off : { 0.0f, 0.45f, -0.45f }) {
				BAddAimedCase(std::format("aimed offset {:+.2f}", off), off, false, off == 0.0f);
			}
			// the hook switched off while +0.45 is still requested: must read like the aimed baseline
			BAddAimedCase("aimed hook off with +0.45 requested", 0.45f, false, false, [] { Camera::SetMode(0); });
			BAdd(0.3, [] {
				Camera::SetOffsetTarget(0.0f);
				Camera::SetMode(0);
			});
			BAdd(2.0, [] { CMeasure("everything off again"); });
		}

		bool BRun(const Ctx& c)
		{
			if (s_bMoveActive && s_bMove.Step(c.now)) {
				s_bMoveActive = false;
			}
			if (s_bIdx >= s_bSteps.size()) {
				return !s_bMoveActive;
			}
			if (s_bIdx == 0 && c.t < s_bSteps[0].delay) {
				return false;
			}
			if (s_bIdx > 0 && c.t < s_bNextAt) {
				return false;
			}
			const auto& st = s_bSteps[s_bIdx];
			if (st.gate && !st.gate() && c.t < s_bNextAt + st.maxWait) {
				return false;
			}
			st.fn();
			++s_bIdx;
			if (s_bIdx < s_bSteps.size()) {
				s_bNextAt = c.t + s_bSteps[s_bIdx].delay;
			}
			return false;
		}

		bool StageS4bProbe(const Ctx& c)
		{
			if (c.first) {
				BBegin(3, true);
				s_bNextAt = s_bSteps[0].delay;
			}
			return BRun(c);
		}

		bool StageS4bPivot(const Ctx& c)
		{
			if (c.first) {
				BBegin(4, true);
				s_bNextAt = s_bSteps[0].delay;
			}
			return BRun(c);
		}

		// Pictures, mode 4: aimed at offset 0, +0.45 and the left side, then the player's back about 1 m from a wall.
		void BPicture(const std::string& a_label, const float a_off, const bool a_storeSide)
		{
			BAddAimedCase(a_label, a_off, true, a_storeSide);
		}

		bool s_bStop{ false };

		void BLogCollision(const char* a_what)
		{
			const auto cs = Camera::ReadCollision();
			Result(std::format("S4b collision state [{}]: factor {:.4f} timer {:.4f} wanted-to-final distance {:.3f} m (read {})", a_what, cs.factor, cs.timer, cs.wantedToFinal, cs.ok));
		}

		// The engine's collision has let go of the camera: factor 1.000 and the wanted and collided positions together.
		bool BCameraFree()
		{
			const auto cs = Camera::ReadCollision();
			return cs.ok && std::fabs(cs.factor - 1.0f) < 0.0015f && cs.wantedToFinal < 0.01f;
		}

		// Second round of S4b: the save loads with a wall 1.3 m behind the player and the engine's collision has pulled the camera in; it
		// lets go only while it sees the player move or the camera turn. The player walks away with real input (W held 4 s), then the
		// stage waits for factor 1.000. No cast of ours runs in these measurements (docs/REFERENCE.md section 14.8, "Hazard").
		bool StageS4bWalkAway(const Ctx& c)
		{
			if (c.first) {
				s_bSteps.clear();
				s_bIdx = 0;
				s_bMoveActive = false;
				s_bStop = false;
				Camera::SetOffsetTarget(0.0f);
				Camera::SetMode(0);
				BAdd(1.0, [] {
					BLogCollision("start, before walking");
					if (Flags2() & 2u) {
						(void)AnimRecon::DrawWeapon(false);
					}
				});
				BAdd(2.0, [] { BLogCollision("start, weapon holstered"); });
				BAdd(0.3, [] { Rig::Tap(Rig::kKeyW, 4.0); });
				BAdd(4.5, [] { BLogCollision("after holding W for 4 s"); });
				BAdd(0.5, [] {}, [] { return BCameraFree(); }, 20.0);
				BAdd(0.2, [] {
					BLogCollision("end of the wait");
					const auto s = Camera::SampleRoot();
					Result(std::format("S4b walk: player at ({:.2f},{:.2f},{:.2f}) | camera free: {}", s.player.x, s.player.y, s.player.z, BCameraFree()));
					if (!BCameraFree()) {
						s_bStop = true;
						Result("S4b walk: the collision factor did not reach 1.000 with the two positions together within 20 s after real walking: STOP (contradicts REFERENCE section 14.8)");
					}
				});
				s_bNextAt = s_bSteps[0].delay;
			}
			if (s_bStop) {
				Abort("the collision factor did not recover after walking");
				return true;
			}
			return BRun(c);
		}

		// Pictures, mode 4, in the open: drawn at +0.45, then aimed at offset 0, at +0.45 and on the left side.
		bool StageS4bPictures(const Ctx& c)
		{
			if (c.first) {
				BBegin(4, false);
				BAdd(0.3, [] { Rig::Tap(Rig::kKeyR, 0.1); });  // weapon out
				BAdd(3.0, [] {});
				BAdd(0.3, [] { Camera::SetOffsetTarget(0.45f); });
				BAdd(2.5, [] { BRequestProbe("drawn offset +0.45 (picture)"); });
				BAdd(0.4, [] {
					BCounters("drawn offset +0.45 (picture)");
					Screenshot("drawn offset +0.45", 0.0);
				});
				BAdd(0.3, [] { Camera::SetOffsetTarget(0.0f); });
				BPicture("aimed offset +0.00", 0.0f, true);
				BPicture("aimed offset +0.45", 0.45f, false);
				BAdd(0.1, [] {});
				BAddAimedCase("aimed left side", 0.0f, true, false, [] {
					const float off = s_bHaveAimSide0 ? -2.0f * s_bAimSide0 : -0.9f;
					REX::INFO("S4b: left-side picture: offset {:+.3f} (= -2 x {:+.3f}{})", off, s_bAimSide0, s_bHaveAimSide0 ? "" : ", fallback: no aimed measurement");
					Camera::SetOffsetTarget(off);
				});
				BAdd(0.3, [] {
					Camera::SetOffsetTarget(0.0f);
					Camera::SetMode(0);
				});
				BAdd(1.0, [] {});
				s_bNextAt = s_bSteps[0].delay;
			}
			return BRun(c);
		}

		// ---- the sequence -------------------------------------------------------------------------------------------
		constexpr std::array<Stage, 23> kStages{ {
			{ "ready", StageReady },
			{ "S4b walk away from the wall (real input)", StageS4bWalkAway },
			{ "S4b pivot shift (mode 4)", StageS4bPivot },
			{ "S4b pictures (mode 4)", StageS4bPictures },
			{ "S4b probe (mode 3, the present call-site hook)", StageS4bProbe },
			{ "S4 call-site hook (numbers and pictures)", StageS4CallSite },
			{ "S4 camera INI settings (numbers)", StageS4Settings },
			{ "S6 route A (direct call)", StageRouteAD },
			{ "S4 baseline", StageS4Baseline },
			{ "S4 offset right", StageS4Right },
			{ "S4 offset left (flip)", StageS4Left },
			{ "S4 restore", StageS4Restore },
			{ "S4 first person", StageS4FirstPerson },
			{ "S4 back to third person", StageS4BackToThird },
			{ "S5 eased move", StageS5Move },
			{ "S5 sneak", StageS5Sneak },
			{ "S5 weapon and aim", StageS5Weapon },
			{ "S5 conditions", StageS5Conditions },
			{ "S6a cover events", StageS6a },
			{ "S6c prepare", StageS6cPrepare },
			{ "S6c cover events", StageS6cEvents },
			{ "S6c restore", StageS6cRestore },
			{ "S6 route A (console, superseded)", StageRouteA },
		} };
		// Order: S5 and S6 first, the camera stages last (a graphics-driver reset ended the second run during S4).

		void Finish(const bool a_ok, const char* a_why)
		{
			Rig::ReleaseEverything(a_ok ? "script finished" : "script aborted");
			if (s_removed) {
				// Whatever happened, the player's actor base must get its keyword back before the game goes on.
				const bool back = AnimRecon::RestoreArchetypeOnBase();
				REX::INFO("S6c: keyword restored at the end of the script: {}", back);
				s_removed = false;
			}
			if (AnimRecon::ArchetypeChanged()) {
				bool       returned{ false };
				const bool ran = AnimRecon::RestoreArchetype(returned);
				REX::INFO("S6 route A: archetype restored at the end of the script: ran={} returned {}", ran, returned);
				if (!ran || !returned) {
					REX::ERROR("S6 route A: RESTORE OF ANIMARCHETYPEPLAYER FAILED at the end of the script");
				}
			}
			if (s_cHaveBases) {
				(void)CameraSettings::SetFloat("fOverShoulderPosX:Camera", s_cPosX0);
				(void)CameraSettings::SetFloat("fOverShoulderCombatPosX:Camera", s_cCombatX0);
				(void)CameraSettings::SetBool("bTestLeftSideCameras:Camera", s_cLeft0);
			}
			Camera::SetOffsetTarget(0.0f);
			REX::INFO("RESULT rig g2: {} ({})", a_ok ? "all stages ran" : "stopped early", a_why);
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
		s_shotNow = now;
		constexpr std::size_t kTotal = kStages.size();

		if (s_stage > 0) {
			// Safety for every stage after the first: the world must be there, unpaused, with the game in front.
			const char* abort = nullptr;
			if (!a_snap.valid || (a_snap.paused && !s_allowPause)) {
				abort = "the game paused or left the world";
			} else if (!Rig::InForeground()) {
				abort = "the game window lost the foreground";
			}
			if (abort) {
				Finish(false, abort);
				return;
			}
		}

		if (s_shotPending) {
			std::error_code ec;
			const auto      ack = AckPath();
			const bool      acked = std::filesystem::exists(ack, ec);
			if (acked || now - s_shotStart > 25.0) {
				std::filesystem::remove(ack, ec);
				s_shotPending = false;
				s_stageStart += now - s_shotStart;  // the stage's own clock does not run while somebody takes a picture
				REX::INFO("S0 rig: screenshot {} after {:.1f} s; the script goes on", acked ? "acknowledged" : "timed out", now - s_shotStart);
			}
			return;
		}
		if (s_stage >= kTotal || (s_stage > 0 && static_cast<int>(s_stage) >= Config::Get().g2EndStage)) {
			Finish(true, "end of the sequence");
			return;
		}
		if (!s_started) {
			s_started = true;
			s_stageStart = now;
			s_stageFirst = true;
			REX::INFO("S0 rig: g2 stage 1 of {}: {}", kTotal, kStages[0].name);
		}
		PumpTyping(now);
		const Ctx ctx{ a_snap, now, now - s_stageStart, s_stageFirst };
		s_stageFirst = false;
		const bool finished = kStages[s_stage].fn(ctx);
		if (finished) {
			++s_stage;
			if (s_stage == 1) {
				s_stage = (std::min)(static_cast<std::size_t>(Config::Get().g2StartStage), kTotal - 1);
			}
			s_stageStart = now;
			s_stageFirst = true;
			if (s_stage < kTotal) {
				REX::INFO("S0 rig: g2 stage {} of {}: {}", s_stage + 1, kTotal, kStages[s_stage].name);
			}
		}
	}
}
