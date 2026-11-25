#include "Engine/Cover.h"

#include <array>
#include <atomic>
#include <mutex>
#include <optional>

#include "Config.h"
#include "Core/AutoCover.h"
#include "Engine/AnimRecon.h"
#include "Core/CandidateCache.h"
#include "Core/CoverMovement.h"
#include "Engine/Camera.h"
#include "Engine/BlindFire.h"
#include "Engine/CoverPose.h"
#include "Engine/GraphEvents.h"
#include "Engine/ScopeAim.h"
#include "Engine/TallAim.h"
#include "Engine/CoverSense.h"
#include "Engine/Frame.h"
#include "Engine/FootIK.h"
#include "Engine/WeaponDraw.h"
#include "Engine/Guard.h"
#include "Engine/Input.h"
#include "Engine/PlayerControl.h"
#include "Engine/WorldProbe.h"

namespace Engine::Cover
{
	namespace
	{
		constexpr double      kLogicStep = 1.0 / 60.0;   // rule 5: the SFSE task runs 600 to 800 times a second, so the logic has its own timer
		constexpr double      kAllowedStep = 0.05;       // the "not allowed" readings are about six graph reads: 20 Hz is enough
		constexpr double      kFactsMaxAge = 0.30;       // the hook ignores tick facts older than this (the tick stopped: vanilla behaviour)
		constexpr float       kTeleportDistance = 1.5f;  // more than this between two logic ticks (16 ms) is a teleport, not movement
		constexpr float       kReachedDistance = 0.05f;
		constexpr std::size_t kMaxTransitions = 400;

		// ---- shared between the hook, the tick and the rig; one mutex, held briefly -------------------------------------------------
		std::mutex              s_mutex;
		Core::CoverStateMachine s_machine;
		Core::CandidateCache    s_cache;
		Core::InputState        s_in;  // the movement input as the player gave it (before any rewrite)

		bool         s_allowed{ false };  // facts of the tick, for the hook
		double       s_allowedAt{ -1.0 };
		bool         s_third{ false };
		int          s_stance{ -1 };  // iSyncActorStance: 0 standing, 1 sneaking
		double       s_stanceAt{ -1.0 };
		float        s_yaw{ 0.0f };
		bool         s_yawOk{ false };
		RE::NiPoint3 s_pos{};
		double       s_posAt{ -1.0 };

		// a swallowed press is swallowed to its release (the game never sees half of a press)
		struct Swallow
		{
			bool         active{ false };
			int          device{ 0 };
			std::int32_t id{ 0 };
		};
		std::array<Swallow, 4> s_swallow;
		std::uint64_t          s_lastPressId{ 0 };
		double                 s_lastPressAt{ -10.0 };
		bool                   s_lastPressSwallowed{ false };

		// the stance check 0.3 s after a press that should (or should not) change it
		bool        s_checkPending{ false };
		double      s_checkAt{ 0.0 };
		int         s_checkWanted{ -1 };
		int         s_checkBefore{ -1 };
		std::string s_checkLabel;

		std::vector<Transition> s_transitions;
		std::uint32_t           s_presses{ 0 };
		std::uint32_t           s_entered{ 0 };
		std::uint32_t           s_swallowed{ 0 };
		std::uint32_t           s_senses{ 0 };

		// T3.3: what the hook needs to filter the movement while the player is in cover; published by the tick (60 Hz), used only while fresh
		struct FilterData
		{
			bool             valid{ false };
			double           at{ -1.0 };
			float            yaw{ 0.0f };
			Core::Vec2       tangent{};
			Core::Vec2       normal{};
			Core::MoveLimits limits{};
			bool             stop{ false };  // Track says Lost or Corner: all movement stops
		};
		FilterData    s_filter;
		std::uint32_t s_coverId{ 0 };   // counts the covers taken; a Track answer for an older one is dropped
		int           s_lostCount{ 0 };  // consecutive Track answers of Lost or Unavailable (T3.3d: a Corner does not count)
		Core::TrackFilter s_tf;           // T3.3d: what the Track answers did to the cover (s_mutex)
		// T3.3f: the settle hold after the stand point is reached (fEnterSettleTime; tick thread, set under the mutex)
		bool          s_settleActive{ false };
		int           s_settleWrites{ 0 };
		double        s_settleUntil{ 0.0 };
		RE::NiPoint3  s_settlePos{};
		int           s_lastSide{ 0 };   // +1 or -1: the side along the wall the game movement vector pointed to last (s_mutex)
		std::uint32_t s_trackCorner{ 0 };
		bool          s_trackStop{ false };
		// T3.4: peeking (tick thread only, except s_aimHeld)
		struct PeekPlan
		{
			enum class Phase { Idle, Checking, Ready } phase{ Phase::Idle };
			Core::PeekKind    kind{ Core::PeekKind::None };
			RE::NiPoint3      spot{};
			WorldProbe::Batch batch;
			double            started{ 0.0 };
			double            retryAt{ 0.0 };
		};
		PeekPlan      s_peek;
		struct Move2
		{
			bool         active{ false };
			RE::NiPoint3 from{};
			RE::NiPoint3 to{};
			double       start{ 0.0 };
			double       seconds{ 0.15 };
		};
		Move2        s_move2;
		std::atomic<int> s_peeking{ 0 };  // the peek in progress, a Core::PeekKind (tick thread writes; the hook reads it at an aim release)
		double           s_crouchSentAt{ -10.0 };  // when the hook sent the crouch press of a peek over (under the mutex)
		double           s_lastStatus{ -10.0 };
		RE::NiPoint3 s_peekHome{};
		// a Sneak press made by the plugin whenever the stance is not what it should be (peek over, leaving by pulling away); at most two tries
		struct StanceFix
		{
			bool        active{ false };
			int         wanted{ -1 };
			double      nextAt{ 0.0 };
			int         tries{ 0 };
			const char* label{ "" };
		};
		StanceFix     s_fix;
		std::uint32_t s_tracks{ 0 };
		std::uint32_t s_trackLost{ 0 };
		float         s_lastPerp{ 0.0f };
		float         s_lastRemainPlus{ 0.0f };
		float         s_lastRemainMinus{ 0.0f };

		// T3.5, iCoverTrigger 2: the Sneak control held for fCoverHoldTime takes cover. The press itself passes to the game (a tap stays vanilla).
		struct HoldPress
		{
			bool         active{ false };
			bool         consumed{ false };
			int          device{ 0 };
			std::int32_t id{ 0 };
			double       since{ 0.0 };
			int          stanceAtPress{ -1 };
		};
		HoldPress         s_hold;
		Core::AutoCover   s_auto;  // M1b, mode 3 (used with the mutex held)
		bool              s_notifyFaulted{ false };
		int               s_fixRequest{ -2 };  // a stance for the tick to set with a made-up press (-2 none); written by the hook
		std::uint64_t     s_lastKeyId{ 0 };
		double            s_lastKeyAt{ -10.0 };
		float             s_gameVecX{ 0.0f };  // T3.3b: the movement vector the game computed in the last call of the hook (before our write)
		float             s_gameVecY{ 0.0f };
		double            s_gameVecAt{ -10.0 };
		std::uint32_t     s_dbgWrites{ 0 }, s_dbgWritesBlocked{ 0 };  // A3
		Core::TallEnds    s_tallEnds{};
		int               s_poseStancePending{ -1 };  // T5.2: a stance asked for while a pose is busy, made when the own graph is back
		double            s_reachedAt{ -10.0 };
		bool              s_aimHeld{ false };  // SecondaryAttack beyond 0.3, from the input (the only reading of third-person aiming, REFERENCE 15)
		std::atomic<bool> s_faulted{ false };

		// ---- tick thread only ------------------------------------------------------------------------------------------------------
		bool                             s_wasEnabled{ false };
		double                           s_nextLogic{ 0.0 };
		double                           s_nextAllowed{ 0.0 };
		std::vector<std::string>         s_notAllowed;
		const void*                      s_lastCell{ nullptr };
		bool                             s_haveLastPos{ false };
		RE::NiPoint3                     s_lastPos{};
		bool                             s_expectJump{ false };
		CoverSense::ProbeAdapter         s_adapter;
		std::optional<Core::CoverSensor> s_sensor;
		double                           s_queryStart{ 0.0 };
		bool                             s_queryIsTrack{ false };
		std::uint32_t                    s_queryCover{ 0 };
		double                           s_nextTrack{ 0.0 };
		RE::NiPoint3                     s_lastTrackPos{};
		double                           s_lastTickTime{ 0.0 };
		double                           s_lastTrackLog{ -10.0 };
		double                           s_nextFind{ 0.0 };
		Core::Vec2                       s_queryIntent{};
		Core::Vec3                       s_queryFeet{};
		std::string                      s_lastBgKey;
		double                           s_lastBgLog{ -10.0 };

		struct Mover
		{
			bool         active{ false };
			RE::NiPoint3 from{};
			Core::Vec3   stand{};
			double       start{ 0.0 };
			int          steps{ 0 };
			int          bad{ 0 };
			RE::NiPoint3 last{};
		};
		Mover s_mover;

		// ---- helpers -----------------------------------------------------------------------------------------------------------------
		[[nodiscard]] double Now() noexcept { return Frame::Now(); }

		[[nodiscard]] float Smooth(const float a_t) noexcept
		{
			const float c = std::clamp(a_t, 0.0f, 1.0f);
			return c * c * (3.0f - 2.0f * c);
		}

		[[nodiscard]] float Horizontal(const RE::NiPoint3& a, const RE::NiPoint3& b) noexcept
		{
			return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
		}

		// forward = (sin h, cos h), right = (cos h, -sin h); input x is to the right, y forward (S4b convention A)
		[[nodiscard]] Core::Vec2 WorldDir(const float a_yaw, const float a_x, const float a_y) noexcept
		{
			const float s = std::sin(a_yaw);
			const float c = std::cos(a_yaw);
			return { c * a_x + s * a_y, -s * a_x + c * a_y };
		}

		// Caller holds the mutex.
		void Note(const double a_now, const Core::CoverState a_from, const Core::CoverState a_to, std::string a_why)
		{
			if (s_transitions.size() < kMaxTransitions) {
				s_transitions.push_back({ a_now, a_from, a_to, std::move(a_why) });
			}
		}

		// Caller holds the mutex. The movement input as a world direction and its magnitude (direction (0,0) when the yaw is unknown).
		[[nodiscard]] Core::Vec2 CurrentIntent(float& a_magnitude)
		{
			s_in.ExpireStick(Now(), static_cast<double>(Config::Get().stickExpireTime));  // a stick at rest may send no event: no event for 0.25 s = centred
			a_magnitude = s_in.MoveMagnitude();
			if (!s_yawOk || a_magnitude < 0.01f) {
				return {};
			}
			return Core::Normalized(WorldDir(s_yaw, s_in.MoveX(), s_in.MoveY()));
		}

		// Caller holds the mutex.
		void ClearHooks() noexcept
		{
			s_swallow = {};
			s_lastPressId = 0;
			s_checkPending = false;
		}

		[[nodiscard]] int FindSwallow(const int a_device, const std::int32_t a_id) noexcept
		{
			for (std::size_t i = 0; i < s_swallow.size(); ++i) {
				if (s_swallow[i].active && s_swallow[i].device == a_device && s_swallow[i].id == a_id) {
					return static_cast<int>(i);
				}
			}
			return -1;
		}

		void AddSwallow(const int a_device, const std::int32_t a_id) noexcept
		{
			if (FindSwallow(a_device, a_id) >= 0) {
				return;
			}
			for (auto& s : s_swallow) {
				if (!s.active) {
					s = { true, a_device, a_id };
					return;
				}
			}
			s_swallow[0] = { true, a_device, a_id };
		}

		// Caller holds the mutex. The facts of the tick are only good while the tick keeps running.
		[[nodiscard]] bool FactsFresh(const double a_now) noexcept
		{
			return s_allowedAt >= 0.0 && a_now - s_allowedAt <= kFactsMaxAge;
		}

		[[nodiscard]] std::string JoinReasons(const std::vector<std::string>& a_reasons)
		{
			std::string out;
			for (const auto& r : a_reasons) {
				out += (out.empty() ? "" : "; ") + r;
			}
			return out.empty() ? std::string("none") : out;
		}

		[[nodiscard]] std::string StandText(const Core::CoverCandidate& a_c)
		{
			return std::format("{} cover {:.2f} m high, stand point ({:.2f},{:.2f},{:.2f}), wall normal ({:+.2f},{:+.2f})", a_c.kind == Core::CoverKind::Low ? "LOW" : "HIGH", a_c.height,
				a_c.standPoint.x, a_c.standPoint.y, a_c.standPoint.z, a_c.normal.x, a_c.normal.y);
		}
	}

	bool Enabled() noexcept
	{
		const auto& c = Config::Get();
		return c.enabled && c.cover && c.inputRewrite && c.rewriteAtControls && !s_faulted.load() && Input::ControlsHookWorking();
	}

	// ======================================= hook side =======================================================================================

	ButtonPlan OnButton(const Core::Action a_action, const bool a_down, const bool a_pressEdge, const int a_deviceType, const std::int32_t a_idCode, const std::uint64_t a_eventId)
	{
		ButtonPlan plan;
		if (!Enabled()) {
			return plan;
		}
		const auto&  cfg = Config::Get();
		const double now = Now();

		if (a_action == Core::Action::Aim) {
			std::lock_guard lock(s_mutex);
			if (const auto sp = ScopeAim::OnAim(a_down, a_pressEdge); sp.swallow) {
				plan.swallow = true;  // W4: a scoped weapon in the pose: the aim control is held back, no aim of ours, the machine is not told (REFERENCE 23.22)
				return plan;
			}
			auto bfPlan = BlindFire::OnAim(a_down, a_pressEdge);  // T5.4: an aim press during blind fire waits for the Out clip
			if (BlindFire::GetInfo().state == BlindFire::State::Idle && !bfPlan.swallow) {
				const auto taPlan = TallAim::OnAim(a_down, a_pressEdge);  // T5.2 tall pose: the side aim by the plugin's own events
				bfPlan.swallow = taPlan.swallow;
				bfPlan.resetHeld = taPlan.resetHeld;
			}
			if (a_down && (cfg.poseDiagMask & 1) != 0 && CoverPose::Busy()) {
				// A3 switch b: the machine is not told about the aim while a pose is on
			} else if (a_down && bfPlan.swallow) {
				// the press is held back: the machine does not see it yet
			} else {
				s_aimHeld = a_down;
			}
			plan.swallow = bfPlan.swallow;
			plan.resetHeld = bfPlan.resetHeld;
			// T3.8: only while the machine is still Peeking: after a leave (B, the cover key, Jump, Sprint) the aim release must not make a crouch press
			if (!a_down && s_peeking.load() == static_cast<int>(Core::PeekKind::Over) && s_machine.State() == Core::CoverState::Peeking && s_stance == 0 && cfg.peekOver) {
				// the aim was let go while peeking over low cover: crouch again with a press made right here, in this call (a request made later waits for the next
				// input event, and none may come while the player stands still)
				Input::RequestSneakPress();
				s_crouchSentAt = now;
				COVER_DETAIL("COVER peek over: the aim was released: the crouch press is made in the same call as the release");
			}
			return plan;
		}

		if (a_action == Core::Action::Sneak) {
			std::lock_guard lock(s_mutex);
			const int       slot = FindSwallow(a_deviceType, a_idCode);
			if (!a_pressEdge) {
				if (!a_down && s_hold.active && s_hold.device == a_deviceType && s_hold.id == a_idCode) {
					s_hold.active = false;  // the control was let go before the hold time: a tap
				}
				// repeats and the release of a press that was swallowed are swallowed too
				if (slot >= 0) {
					plan.swallow = true;
					if (!a_down) {
						s_swallow[static_cast<std::size_t>(slot)].active = false;
					}
				}
				return plan;
			}
			if (slot >= 0) {
				s_swallow[static_cast<std::size_t>(slot)].active = false;
			}
			if (a_eventId != 0 && a_eventId == s_lastPressId && now - s_lastPressAt < 0.1) {
				// the same press seen twice (the receiver may run more than once per frame): the same decision, nothing counted
				if (s_lastPressSwallowed) {
					AddSwallow(a_deviceType, a_idCode);
					plan.swallow = true;
				}
				return plan;
			}
			s_lastPressId = a_eventId;
			s_lastPressAt = now;

			float                  magnitude = 0.0f;
			const Core::Vec2       dir = CurrentIntent(magnitude);
			const auto             fresh = s_cache.Fresh(now, cfg.candidateMaxAge, dir, cfg.candidateMaxTurnDeg);
			float                  standDistance = 0.0f;
			Core::PressContext     ctx;
			ctx.mode = cfg.coverTrigger;
			ctx.state = s_machine.State();
			ctx.allowed = FactsFresh(now) && s_allowed && s_third;
			ctx.moveMagnitude = magnitude;
			ctx.stickMin = cfg.coverStickMin;
			ctx.candidateFresh = fresh.has_value();
			ctx.candidateKind = fresh ? fresh->candidate.kind : Core::CoverKind::None;
			if (fresh && fresh->candidate.kind != Core::CoverKind::None && s_posAt >= 0.0) {
				const auto& sp = fresh->candidate.standPoint;
				standDistance = std::sqrt((sp.x - s_pos.x) * (sp.x - s_pos.x) + (sp.y - s_pos.y) * (sp.y - s_pos.y));
				if (standDistance > cfg.enterMaxDistance) {
					ctx.candidateFresh = false;  // too far to slide in
				}
			}
			ctx.stance = (s_stanceAt >= 0.0 && now - s_stanceAt <= kFactsMaxAge) ? s_stance : -1;
			ctx.stanceBefore = s_machine.StanceBefore();

			const auto decision = Core::DecideSneakPress(ctx);
			++s_presses;
			plan.swallow = decision.swallow;
			if (decision.swallow) {
				++s_swallowed;
				AddSwallow(a_deviceType, a_idCode);
			}
			s_lastPressSwallowed = decision.swallow;

			const auto from = s_machine.State();
			if (decision.action == Core::PressAction::Enter && fresh && s_machine.RequestEnter(now, fresh->candidate, ctx.stance)) {
				++s_entered;
				++s_coverId;
				s_lostCount = 0;
				s_tf = Core::TrackFilter{};
				s_lastSide = 0;
				s_filter = FilterData{};
				Note(now, from, Core::CoverState::Entering, "the Sneak press");
				s_cache.Clear();
				s_checkPending = true;
				s_checkAt = now + static_cast<double>(cfg.stanceCheckTime) + 0.9;  // the stance is made by a press of the plugin (M1): it needs the tick, the press and the game
				s_checkWanted = decision.wantedStance;
				s_checkBefore = ctx.stance;
				s_checkLabel = "enter";
				if (decision.wantedStance >= 0 && ctx.stance != decision.wantedStance) {
					s_fixRequest = decision.wantedStance;  // M1: the real press is swallowed, the plugin makes the stance
				}
			} else if (decision.action == Core::PressAction::Leave && s_machine.RequestLeave(now, Core::ExitReason::Trigger)) {
				Note(now, from, Core::CoverState::Exiting, "the Sneak press");
				s_peeking = 0;  // T3.8: the peek is over with the leave
				if (decision.wantedStance >= 0) {
					s_checkPending = true;
					s_checkAt = now + static_cast<double>(cfg.stanceCheckTime) + 0.9;
					s_checkWanted = decision.wantedStance;
					s_checkBefore = ctx.stance;
					s_checkLabel = "leave";
					if (ctx.stance != decision.wantedStance) {
						s_fixRequest = decision.wantedStance;  // M1: the real press is swallowed, the plugin restores the stance
					}
				}
			} else if (decision.action != Core::PressAction::Nothing) {
				plan.swallow = false;  // the machine refused: leave the press to the game
			}
			if (cfg.coverTrigger == 2 && decision.action == Core::PressAction::Nothing && !decision.swallow && ctx.state == Core::CoverState::None) {
				s_hold = { true, false, a_deviceType, a_idCode, now, ctx.stance };
			}
			COVER_DETAIL("COVER press #{} [{}]: {} | state {}, input {:.2f} toward ({:+.2f},{:+.2f}), sensor {}{}, stance {} (before {}, wanted {}) | the press {}", s_presses,
				a_deviceType == 0 ? "keyboard" : (a_deviceType == 2 ? "gamepad" : "device"), decision.why, Core::ToString(from), magnitude, dir.x, dir.y,
				fresh ? (fresh->candidate.kind == Core::CoverKind::None ? "found nothing" : (fresh->candidate.kind == Core::CoverKind::Low ? "LOW" : "HIGH")) : "no fresh answer",
				fresh && fresh->candidate.kind != Core::CoverKind::None ? std::format(" (stand point {:.2f} m away, answer {:.2f} s old)", standDistance, now - fresh->time) : std::string(), ctx.stance, ctx.stanceBefore,
				decision.wantedStance, plan.swallow ? "is swallowed" : "passes to the game");
			return plan;
		}

		if ((a_action == Core::Action::Jump || a_action == Core::Action::Sprint) && a_pressEdge) {
			std::lock_guard lock(s_mutex);
			const auto      from = s_machine.State();
			const auto      reason = a_action == Core::Action::Jump ? Core::ExitReason::Jump : Core::ExitReason::Sprint;
			if (s_machine.RequestLeave(now, reason)) {
				Note(now, from, Core::CoverState::Exiting, Core::ToString(reason));
				s_peeking = 0;  // T3.8
				COVER_DETAIL("COVER leaving by {}: the press passes to the game (state was {})", Core::ToString(reason), Core::ToString(from));
			}
		}
		return plan;
	}

	// T3.5 mode 0: the dedicated cover key (a keyboard code, [Input] iCoverKey; it has no vanilla binding, the log shows its user event). Same call path as a
	// Sneak press, but nothing to swallow and no movement input needed: the sensor looks along the view.
	void OnCoverKey(const bool a_down, const bool a_pressEdge, const std::uint64_t a_eventId)
	{
		if (!Enabled() || !a_pressEdge || !a_down) {
			return;
		}
		const auto&     cfg = Config::Get();
		const double    now = Now();
		std::lock_guard lock(s_mutex);
		if (a_eventId != 0 && a_eventId == s_lastKeyId && now - s_lastKeyAt < 0.1) {
			return;
		}
		s_lastKeyId = a_eventId;
		s_lastKeyAt = now;

		const auto         fresh = s_cache.Fresh(now, cfg.candidateMaxAge, Core::Vec2{}, 0.0f);  // age only: the view may have turned a little
		Core::PressContext ctx;
		ctx.mode = cfg.coverTrigger;
		ctx.state = s_machine.State();
		ctx.allowed = FactsFresh(now) && s_allowed && s_third;
		ctx.candidateFresh = fresh.has_value();
		ctx.candidateKind = fresh ? fresh->candidate.kind : Core::CoverKind::None;
		float standDistance = 0.0f;
		if (fresh && fresh->candidate.kind != Core::CoverKind::None && s_posAt >= 0.0) {
			const auto& sp = fresh->candidate.standPoint;
			standDistance = std::sqrt((sp.x - s_pos.x) * (sp.x - s_pos.x) + (sp.y - s_pos.y) * (sp.y - s_pos.y));
			if (standDistance > cfg.enterMaxDistance) {
				ctx.candidateFresh = false;
			}
		}
		ctx.stance = (s_stanceAt >= 0.0 && now - s_stanceAt <= kFactsMaxAge) ? s_stance : -1;
		ctx.stanceBefore = s_machine.StanceBefore();
		const auto decision = Core::DecideCoverKeyPress(ctx);
		const auto from = s_machine.State();
		if (decision.action == Core::PressAction::Enter && fresh && s_machine.RequestEnter(now, fresh->candidate, ctx.stance)) {
			++s_entered;
			++s_coverId;
			s_lostCount = 0;
			s_tf = Core::TrackFilter{};
			s_lastSide = 0;
			s_filter = FilterData{};
			Note(now, from, Core::CoverState::Entering, "the cover key");
			s_cache.Clear();
			s_fixRequest = decision.wantedStance;
		} else if (decision.action == Core::PressAction::Leave && s_machine.RequestLeave(now, Core::ExitReason::Trigger)) {
			Note(now, from, Core::CoverState::Exiting, "the cover key");
			s_peeking = 0;  // T3.8
			if (decision.wantedStance >= 0) {
				s_fixRequest = decision.wantedStance;
			}
		}
		COVER_DETAIL("COVER key press [mode 0]: {} | state {}, sensor {}{}, stance {} (before {}, wanted {})", decision.why, Core::ToString(from),
			fresh ? (fresh->candidate.kind == Core::CoverKind::None ? "found nothing" : (fresh->candidate.kind == Core::CoverKind::Low ? "LOW" : "HIGH")) : "no fresh answer",
			fresh && fresh->candidate.kind != Core::CoverKind::None ? std::format(" (stand point {:.2f} m away, answer {:.2f} s old)", standDistance, now - fresh->time) : std::string(), ctx.stance, ctx.stanceBefore, decision.wantedStance);
	}

	namespace
	{
		// Caller holds the mutex. The hook-side filters (zeroing while entering, the movement along the wall in cover) apply only while the tick is alive
		// (FactsFresh): a stopped tick means vanilla input. Entering zeroes the movement for enterMaxTime plus 0.3 s at the most, measured here.
		enum class FilterMode
		{
			None,
			Zero,   // all movement
			Wall,   // movement along the wall (T3.3)
		};

		std::atomic<double> s_revUntil{ 0.0 };  // card M4b: until then the movement input is written as zero (a reversal against the pose's side waits for the character to stand)

		[[nodiscard]] FilterMode ModeNow(const double a_now)
		{
			const auto& cfg = Config::Get();
			if (!cfg.coverFilterInput || !FactsFresh(a_now)) {
				return FilterMode::None;
			}
			if ((cfg.poseDiagMask & 4) != 0 && CoverPose::Active()) {
				return FilterMode::None;  // A3 switch d
			}
			switch (s_machine.State()) {
			case Core::CoverState::Entering:
				return a_now - s_machine.StateSince() <= static_cast<double>(cfg.machineCfg.enterMaxTime) + 0.3 ? FilterMode::Zero : FilterMode::None;
			case Core::CoverState::Peeking:
				return FilterMode::Zero;
			case Core::CoverState::InCover:
				if (a_now < s_revUntil.load()) {
					return FilterMode::Zero;
				}
				if (CoverPose::CornerActive() && s_filter.valid && Core::AwayInput(s_in.MoveX(), s_in.MoveY(), s_filter.yaw, s_filter.normal) < cfg.machineCfg.exitInputThreshold) {
					return FilterMode::Zero;  // card M4b: no walk during the corner aim
				}
				return (s_filter.valid && a_now - s_filter.at <= kFactsMaxAge) ? (s_filter.stop ? FilterMode::Zero : FilterMode::Wall) : FilterMode::None;
			default:
				return FilterMode::None;
			}
		}
	}

	namespace
	{
		// T5.2c: the movement input the game's walk clip would step on the spot with: the pose is on and the along-the-wall part of the whole input (keys and stick) is below fCoverAlongMin
		// (the wall's normal part is no walk at all); called with s_mutex held
		std::atomic<int> s_zeroCount{ 0 };
		// the pose's stick log (bPoseStickLog): the raw stick, the stick as the game was given it, the vector the plugin gave back
		float  s_logRawX{ 0.0f }, s_logRawY{ 0.0f }, s_logSentX{ 0.0f }, s_logSentY{ 0.0f }, s_logOutX{ 0.0f }, s_logOutY{ 0.0f };
		bool   s_logOutSet{ false };
		double s_nextStickLog{ 0.0 };

		[[nodiscard]] bool PoseZeroInputLocked(const double a_now)
		{
			const auto& cfg = Config::Get();
			if (cfg.poseExitTurn == 3) {
				const auto ps = CoverPose::GetInfo().state;
				if (ps == CoverPose::State::Leaving || ps == CoverPose::State::Restoring) {
					s_zeroCount.fetch_add(1);
					return true;  // the way out: no movement input for the game until the own graph is back (the exit clip turns the actor toward the input otherwise)
				}
			}
			if (!cfg.poseZeroInput || !CoverPose::Active() || !s_filter.valid || a_now - s_filter.at > kFactsMaxAge) {
				return false;
			}
			// pulling away from the wall is the way out: that input goes to the game as it always did (a zeroed key also reads as released afterwards, the machine would never see it held; run m3l-1)
			if (Core::AwayInput(s_in.MoveX(), s_in.MoveY(), s_filter.yaw, s_filter.normal) >= cfg.machineCfg.exitInputThreshold) {
				return false;
			}
			if (CoverPose::CornerActive()) {
				s_zeroCount.fetch_add(1);
				return true;  // card M4b: the side aim has no walk: the input along the wall is swallowed until the aim is let go
			}
			const Core::Vec2 w = Core::InputToWorld(s_filter.yaw, s_in.MoveX(), s_in.MoveY());
			if (std::fabs(Core::Dot(w, s_filter.tangent)) < cfg.coverAlongMin) {
				s_zeroCount.fetch_add(1);
				return true;
			}
			return false;
		}
	}

	int ZeroInputCount() noexcept
	{
		return s_zeroCount.load();
	}

	void OnMoveKey(const Core::Action a_action, const bool a_down, float& a_value)
	{
		if (!Enabled()) {
			return;
		}
		std::lock_guard lock(s_mutex);
		s_in.ApplyAction(a_action, a_down);  // the raw input: the leave by pulling away keeps reading it
		if (a_down && PoseZeroInputLocked(Now())) {
			a_value = 0.0f;
		}
	}

	void OnMoveStick(float& a_x, float& a_y)
	{
		if (!Enabled()) {
			return;
		}
		const double    now = Now();
		std::lock_guard lock(s_mutex);
		s_in.SetStickAt(a_x, a_y, now);
		s_logRawX = a_x;
		s_logRawY = a_y;
		if (PoseZeroInputLocked(now)) {
			a_x = 0.0f;
			a_y = 0.0f;
		}
		s_logSentX = a_x;
		s_logSentY = a_y;
	}

	VectorFilter FilterMovementVector(const float a_gameX, const float a_gameY)
	{
		VectorFilter out;
		if (!Enabled()) {
			return out;
		}
		const double    now = Now();
		std::lock_guard lock(s_mutex);
		s_gameVecX = a_gameX;
		s_gameVecY = a_gameY;
		s_gameVecAt = now;
		s_logOutSet = false;
		switch (ModeNow(now)) {
		case FilterMode::Zero:
			out = { true, 0.0f, 0.0f };
			break;
		case FilterMode::Wall: {
			const auto r = Core::RewriteStick(a_gameX, a_gameY, s_filter.yaw, s_filter.tangent, s_filter.normal, s_filter.limits, Config::Get().coverMoveScale);
			if (std::fabs(r.along) > 0.05f) {
				s_lastSide = r.along > 0.0f ? 1 : -1;  // the side the player is moving toward along the wall (for a Corner answer of the track)
			}
			out = { true, r.x, r.y };
			break;
		}
		case FilterMode::None:
			break;
		}
		if (out.write) {
			s_logOutX = out.x;
			s_logOutY = out.y;
			s_logOutSet = true;
		}
		return out;
	}

	// ======================================= tick side =======================================================================================
	namespace
	{
		// Caller must NOT hold the mutex.
		void HardResetNow(const double a_now, const std::string& a_why)
		{
			CoverPose::HardReset(a_why.c_str());  // T5.2: the archetype, the hold and the camera offset back in this call
			s_move2.active = false;
			s_peek.phase = PeekPlan::Phase::Idle;
			std::lock_guard lock(s_mutex);
			const auto      from = s_machine.State();
			if (s_machine.Reset()) {
				Note(a_now, from, Core::CoverState::None, "hard reset: " + a_why);
				REX::INFO("COVER hard reset from {}: {}", Core::ToString(from), a_why);
			}
			s_cache.Clear();
			ClearHooks();
			s_mover.active = false;
			s_peeking = 0;
			Camera::CoverCamReset();  // T5.3a: a hard reset takes the cover camera offset out at once
		}

		bool WritePosition(const RE::NiPoint3& a_target)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return false;
			}
			if ((Config::Get().poseDiagMask & 2) != 0 && CoverPose::Busy()) {
				++s_dbgWritesBlocked;  // A3 switch c
				return false;
			}
			++s_dbgWrites;
			unsigned long code{ 0 };
			if (!Guarded([&] { player->SetPosition(a_target, true); }, code)) {
				s_faulted = true;
				REX::ERROR("COVER: exception 0x{:08X} in Actor::SetPosition; the cover feature is off for this session", code);
				return false;
			}
			return true;
		}

		void BeginMover(const Frame::Snapshot& a_snap, const Core::CoverCandidate& a_cand, const double a_now)
		{
			s_mover = {};
			s_mover.active = true;
			s_mover.from = a_snap.position;
			s_mover.stand = a_cand.standPoint;
			s_mover.start = a_now;
			s_mover.last = a_snap.position;
			COVER_DETAIL("COVER enter move: from ({:.2f},{:.2f},{:.2f}) to the stand point ({:.2f},{:.2f},{:.2f}), {:.2f} m, over {:.2f} s (snap after {:.2f} s)", a_snap.position.x, a_snap.position.y, a_snap.position.z,
				a_cand.standPoint.x, a_cand.standPoint.y, a_cand.standPoint.z, Horizontal(a_snap.position, { a_cand.standPoint.x, a_cand.standPoint.y, a_cand.standPoint.z }), Config::Get().machineCfg.enterTime,
				Config::Get().machineCfg.enterMaxTime);
		}

		// One step of the eased move at the 60 Hz timer; fills in what the state machine needs to know.
		void StepMover(const Frame::Snapshot& a_snap, const double a_now, Core::MachineInputs& a_in)
		{
			const auto&        cfg = Config::Get();
			const RE::NiPoint3 stand{ s_mover.stand.x, s_mover.stand.y, s_mover.stand.z };
			const double       t = a_now - s_mover.start;

			if (s_mover.steps > 0) {
				// the game left the player this far from where we put them one step ago: a wall or an object is in the way
				s_mover.bad = Horizontal(a_snap.position, s_mover.last) > cfg.pathBlockedError ? s_mover.bad + 1 : 0;
				if (s_mover.bad >= 4) {
					a_in.pathBlocked = true;
					return;
				}
			}
			if (Horizontal(a_snap.position, stand) <= kReachedDistance) {
				a_in.reachedStand = true;
				return;
			}
			if (!cfg.coverMove) {
				return;
			}
			const float        u = Smooth(static_cast<float>(t / (std::max)(static_cast<double>(cfg.machineCfg.enterTime), 0.05)));
			const RE::NiPoint3 target{ s_mover.from.x + (stand.x - s_mover.from.x) * u, s_mover.from.y + (stand.y - s_mover.from.y) * u, s_mover.from.z + (stand.z - s_mover.from.z) * u };
			if (!WritePosition(target)) {
				a_in.pathBlocked = true;
				return;
			}
			s_mover.last = target;
			s_expectJump = true;
			++s_mover.steps;
		}

		void LogBackground(const double a_now, const Core::SensorResult& a_result)
		{
			const auto& c = a_result.candidate;
			const auto  key = std::format("{}|{}|{}|{:.0f}|{:.0f}", Core::ToString(c.kind), Core::ToString(c.why), c.usedFallback, std::round(c.standPoint.x * 2.0f), std::round(c.standPoint.y * 2.0f));
			if (key == s_lastBgKey || a_now - s_lastBgLog < 0.5) {
				return;
			}
			s_lastBgKey = key;
			s_lastBgLog = a_now;
			COVER_DETAIL("COVER sense: {}", CoverSense::Describe(a_result));
		}

		void BeginMove2(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, const double a_now, const double a_seconds)
		{
			s_move2 = { true, a_from, a_to, a_now, (std::max)(a_seconds, 0.05) };
		}

		// one step of the eased peek move (60 Hz); every step is an own write
		void StepMove2(const double a_now)
		{
			if (!s_move2.active) {
				return;
			}
			const float u = Smooth(static_cast<float>((a_now - s_move2.start) / s_move2.seconds));
			const RE::NiPoint3 target{ s_move2.from.x + (s_move2.to.x - s_move2.from.x) * u, s_move2.from.y + (s_move2.to.y - s_move2.from.y) * u, s_move2.from.z + (s_move2.to.z - s_move2.from.z) * u };
			if (WritePosition(target)) {
				s_expectJump = true;
			} else {
				s_move2.active = false;
				return;
			}
			if (a_now - s_move2.start >= s_move2.seconds) {
				s_move2.active = false;
			}
		}

		// M1b, bCoverNotify: a HUD line, as the game's own notification event (RE::ShowHUDMessageEvent, its event source ID from CommonLibSF). A test aid: guarded, switched
		// off for the session after a fault. Called from the tick only.
		void Notify(const char* a_text)
		{
			if (!Config::Get().coverNotify || s_notifyFaulted) {
				return;
			}
			unsigned long code{ 0 };
			if (!Guarded([&] {
					auto* source = RE::ShowHUDMessageEvent::GetEventSource();
					if (source) {
						const RE::ShowHUDMessageEvent ev{ RE::BSFixedString(a_text), RE::BSFixedString(""), false, false };
						source->Notify(ev);
					}
				},
					code)) {
				s_notifyFaulted = true;
				REX::ERROR("COVER notify: exception 0x{:08X} while showing a HUD line; bCoverNotify is off for this session", code);
			} else {
				COVER_DETAIL("COVER notify: HUD line \"{}\"", a_text);
			}
		}

		void WantStance(const int a_wanted, const double a_now, const char* a_label)
		{
			if (CoverPose::Busy()) {
				s_poseStancePending = a_wanted;  // T5.2: the stance is made after the pose, on the own graph
				return;
			}
			s_fix = { true, a_wanted, a_now + 0.05, 0, a_label };
		}

		// the stance fix: a Sneak press made by the plugin, when the stance is not the wanted one; waits 0.6 s for each press to take effect
		void StepStanceFix(const double a_now, const int a_stance)
		{
			if (CoverPose::Busy()) {
				s_fix.active = false;  // T5.2: no Sneak press while the pose has the stance
				return;
			}
			if (!s_fix.active) {
				return;
			}
			if (a_stance == s_fix.wanted || a_stance < 0) {
				if (a_stance == s_fix.wanted) {
					COVER_DETAIL("COVER stance fix [{}]: the stance is {} as wanted after {} press(es) made by the plugin", s_fix.label, a_stance, s_fix.tries);
				}
				s_fix.active = false;
				return;
			}
			if (a_now < s_fix.nextAt) {
				return;
			}
			if (s_fix.tries >= 2) {
				REX::WARN("COVER stance fix [{}]: the stance is {}, wanted {}, after {} presses made by the plugin; giving up", s_fix.label, a_stance, s_fix.wanted, s_fix.tries);
				s_fix.active = false;
				return;
			}
			++s_fix.tries;
			s_fix.nextAt = a_now + 0.6;
			Input::RequestSneakPress();
			COVER_DETAIL("COVER stance fix [{}]: stance {}, wanted {}: a Sneak press is made by the plugin (try {})", s_fix.label, a_stance, s_fix.wanted, s_fix.tries);
		}

		// Side peek or peek over for this tick: which one is possible now (None: neither). Side peeks need a validated spot (floor, clear path), checked through
		// the cast queue; the answer arrives a few frames later and is kept while the aim stays held.
		Core::PeekKind PeekSpotNow(const Frame::Snapshot& a_snap, const double a_now, const Core::CoverState a_state, const Core::CoverCandidate& a_cand, const Core::MoveLimits& a_limits, const bool a_aim)
		{
			const auto& cfg = Config::Get();
			if (!cfg.peek || CoverPose::Busy() || (a_state != Core::CoverState::InCover && a_state != Core::CoverState::Peeking)) {
				s_peek.phase = PeekPlan::Phase::Idle;
				return Core::PeekKind::None;
			}
			if (a_state == Core::CoverState::Peeking) {
				// stays as long as the aim is held (the machine ends it); a side peek keeps the spot it was validated for
				return a_aim ? static_cast<Core::PeekKind>(s_peeking.load()) : Core::PeekKind::None;
			}
			if (!a_aim) {
				s_peek.phase = PeekPlan::Phase::Idle;
				return Core::PeekKind::None;
			}
			// which open end is near enough
			const float margin = cfg.sensorCfg.edgeMargin;
			float       distPlus = 1e9f;
			float       distMinus = 1e9f;
			if (a_cand.edgeRightKind == Core::EdgeKind::Open) {
				distPlus = a_limits.remainPlus + margin;
			}
			if (a_cand.edgeLeftKind == Core::EdgeKind::Open) {
				distMinus = a_limits.remainMinus + margin;
			}
			Core::PeekKind want = Core::PeekKind::None;
			if (distPlus <= cfg.edgePeekRange && distPlus <= distMinus) {
				want = Core::PeekKind::Right;
			} else if (distMinus <= cfg.edgePeekRange) {
				want = Core::PeekKind::Left;
			} else if (cfg.peekOver && a_cand.kind == Core::CoverKind::Low && a_cand.canPeekOver && Input::MadeUpPressStats().haveTemplate) {
				s_peek.phase = PeekPlan::Phase::Idle;
				return Core::PeekKind::Over;
			}
			if (want == Core::PeekKind::None) {
				s_peek.phase = PeekPlan::Phase::Idle;
				return Core::PeekKind::None;
			}

			if (s_peek.phase == PeekPlan::Phase::Idle || s_peek.kind != want) {
				if (a_now < s_peek.retryAt || !WorldProbe::Available()) {
					return Core::PeekKind::None;
				}
				// the spot: on the stand line, fSidePeekDistance past the end (the end is edgeRight or edgeLeft from the foot of the cover)
				const float edgeAlong = want == Core::PeekKind::Right ? a_cand.edgeRight : -a_cand.edgeLeft;
				const float along = edgeAlong + (want == Core::PeekKind::Right ? cfg.sidePeekDistance : -cfg.sidePeekDistance);
				const float sx = a_cand.standPoint.x + a_cand.tangent.x * along;
				const float sy = a_cand.standPoint.y + a_cand.tangent.y * along;
				s_peek.kind = want;
				s_peek.spot = { sx, sy, a_cand.floorZ };
				s_peek.batch.Clear();
				s_peek.started = a_now;
				const float        r = Config::Get().probeRadius;
				const auto&        p = a_snap.position;
				s_peek.batch.Add({ sx, sy, a_cand.floorZ + 1.0f }, { sx, sy, a_cand.floorZ - 3.0f }, r);                  // 0 floor under the spot
				s_peek.batch.Add({ p.x, p.y, p.z + 0.4f }, { sx, sy, p.z + 0.4f }, r);                                      // 1 the way there, knee height
				s_peek.batch.Add({ p.x, p.y, p.z + 1.0f }, { sx, sy, p.z + 1.0f }, r);                                      // 2 and chest height
				s_peek.phase = PeekPlan::Phase::Checking;
				return Core::PeekKind::None;
			}
			if (s_peek.phase == PeekPlan::Phase::Checking) {
				if (a_now - s_peek.started > 0.6) {
					s_peek.phase = PeekPlan::Phase::Idle;
					s_peek.retryAt = a_now + 0.5;
					COVER_DETAIL("COVER peek: the spot check was not answered within 0.6 s");
					return Core::PeekKind::None;
				}
				if (!s_peek.batch.Done()) {
					return Core::PeekKind::None;
				}
				const auto& fl = s_peek.batch[0];
				const bool  floorOk = fl.ok && fl.hit && std::fabs((fl.point.z - 0.1f) - a_cand.floorZ) <= 0.35f;
				const bool  pathOk = s_peek.batch[1].ok && !s_peek.batch[1].hit && s_peek.batch[2].ok && !s_peek.batch[2].hit;
				COVER_DETAIL("COVER peek spot {} ({:.2f},{:.2f}): floor {} (end point z {:.2f}, cover floor {:.2f}), path knee {} chest {} -> {}", Core::ToString(s_peek.kind), s_peek.spot.x, s_peek.spot.y, floorOk ? "ok" : "NONE", fl.point.z - 0.1f, a_cand.floorZ,
					s_peek.batch[1].hit ? "BLOCKED" : "clear", s_peek.batch[2].hit ? "BLOCKED" : "clear", floorOk && pathOk ? "valid" : "invalid");
				if (floorOk && pathOk) {
					s_peek.phase = PeekPlan::Phase::Ready;
				} else {
					s_peek.phase = PeekPlan::Phase::Idle;
					s_peek.retryAt = a_now + 0.5;
					return Core::PeekKind::None;
				}
			}
			return s_peek.kind;
		}

		// Every tick (the SFSE task runs 600 times a second): lets a running query advance and files its answer, so that an answer is not delayed
		// by the 60 Hz timer of the rest of the logic.
		void PumpSensor(const double a_now)
		{
			if (!s_sensor) {
				return;
			}
			auto& sensor = *s_sensor;
			sensor.Update();
			if (!sensor.HasResult()) {
				return;
			}
			const auto r = sensor.TakeResult();
			if (r.isTrack) {
				std::lock_guard lock(s_mutex);
				if (s_queryCover != s_coverId || s_machine.State() != Core::CoverState::InCover) {
					return;  // an answer for a cover that is already left
				}
				// T3.3d: Ok replaces the candidate; a Corner keeps the last good one, is not counted and stops only the side of the last movement; Lost and Unavailable count and stop everything
				const auto dec = Core::DecideTrack(r.status, s_tf, s_lastSide);
				s_tf = dec.next;
				s_lostCount = s_tf.lostCount;
				s_trackStop = s_tf.stopAll;
				if (dec.useCandidate) {
					s_machine.SetCandidate(r.candidate);
					++s_tracks;
				} else {
					if (r.status == Core::TrackStatus::Corner) {
						++s_trackCorner;
					} else {
						++s_trackLost;
					}
					if (a_now - s_lastTrackLog > 0.3) {
						s_lastTrackLog = a_now;
						if (r.status == Core::TrackStatus::Corner) {
							COVER_DETAIL("COVER track: corner (not counted), normal turned {:.1f} deg: the last good candidate stays, movement toward side {:+d} stops until the next Ok answer", r.turnDeg, s_tf.blockedSide);
						} else {
							COVER_DETAIL("COVER track: {} ({} in a row; {} leave the cover), normal turned {:.1f} deg: movement stops{}{}", Core::ToString(r.status), s_lostCount, Config::Get().machineCfg.lostLeaveCount, r.turnDeg, r.why.empty() ? "" : "; why: ", r.why);
						}
					}
				}
				return;
			}
			{
				std::lock_guard lock(s_mutex);
				if (s_machine.State() == Core::CoverState::None) {
					s_cache.Store(a_now, r.candidate, s_queryIntent, s_queryFeet);
					++s_senses;
				}
			}
			LogBackground(a_now, r);
		}

		void TickImpl(const Frame::Snapshot& a_snap)
		{
			const auto&  cfg = Config::Get();
			const double now = a_snap.time;
			if (now < s_nextLogic) {
				PumpSensor(now);
				return;
			}
			s_nextLogic = now + kLogicStep;
			if (now > s_nextLogic + 0.1) {
				s_nextLogic = now + kLogicStep;  // no catching up after a stall
			}
			if (!s_sensor) {
				s_sensor.emplace(s_adapter, CoverSense::Settings());
			}

			const auto player = RE::PlayerCharacter::GetSingleton();
			const bool valid = a_snap.valid && !a_snap.paused && player != nullptr;
			const void* cell = player ? static_cast<const void*>(player->parentCell) : nullptr;
			if (!valid) {
				HardResetNow(now, "no player in a loaded cell, or the game is paused");
				{
					std::lock_guard lock(s_mutex);
					s_in = {};
					s_aimHeld = false;
					s_allowed = false;
					s_third = false;
				}
				if (s_sensor && s_sensor->Busy()) {
					s_sensor->Cancel();
				}
				s_haveLastPos = false;
				s_lastCell = cell;
				return;
			}
			const bool third = a_snap.cameraIndex == RE::CameraStates::kThirdPerson;

			// the world changed under us: a load, a cell change, a teleport (our own snap is announced by s_expectJump)
			bool        worldReset = false;
			const char* worldWhy = "";
			if (cell != s_lastCell) {
				worldReset = true;
				worldWhy = "the parent cell changed";
			}
			s_lastCell = cell;
			const float  moved = s_haveLastPos ? Horizontal(a_snap.position, s_lastPos) : 0.0f;
			const double dtLast = now - s_lastTickTime;
			if (s_haveLastPos && !s_expectJump && moved > kTeleportDistance) {
				worldReset = true;
				worldWhy = "the player moved more than 1.5 m between two ticks (teleport)";
			}
			// moved more than 0.15 m (or what 8 m/s covers when the tick was late) by something other than our own write: a hit reaction, an
			// explosion, a knockdown. Cover is left at once, nothing is corrected (T3.3).
			// 23.18: not in the first moments after the plugin's own stance call: the cover entry clip sets the actor's position (07:40:01 of the user's play: 0.15 m along the line at a walk, 0.02 s after the call, "shoved")
			const double tStanceCall = CoverPose::GetInfo().tCall;
			const bool   afterOwnCall = cfg.shoveGrace > 0.0f && tStanceCall > 0.0 && now >= tStanceCall && now - tStanceCall < static_cast<double>(cfg.shoveGrace);
			const bool shovedNow = s_haveLastPos && !s_expectJump && !worldReset && !afterOwnCall && moved > (std::max)(cfg.shoveDistance, static_cast<float>(8.0 * dtLast));
			s_expectJump = false;
			s_lastPos = a_snap.position;
			s_lastTickTime = now;
			s_haveLastPos = true;

			if (now >= s_nextAllowed) {
				s_nextAllowed = now + kAllowedStep;
				s_notAllowed = PlayerControl::NotAllowed(a_snap);
			}
			// 23.21 iPoseScopeAim: a scope view (the camera state leaves third person) while the pose is Posed and the aim control is held does not end the cover; the guard applies again when the
			// camera is still not third person 1.0 s after the aim control went up. (s_notAllowed itself stays as it was read: the exemption is made here, per tick.)
			static double s_scopeLastAim = -10.0;
			bool          scopeView = false;
			if (cfg.poseScopeAim == 1 && !third && CoverPose::GetInfo().state == CoverPose::State::Posed) {
				bool onlyCamera = true;  // the cached reasons may be empty (read while the camera was still third person): then the camera is the only reason
				for (const auto& w : s_notAllowed) {
					if (w != "first person" && w != "ironsights camera") {
						onlyCamera = false;
					}
				}
				if (s_aimHeld || CoverPose::VarValue("iIsSighted") > 0.5) {
					s_scopeLastAim = now;
				}
				if (onlyCamera && now - s_scopeLastAim <= 1.0) {
					scopeView = true;
					static double s_scopeLog = -10.0;
					if (now - s_scopeLog > 1.0) {
						s_scopeLog = now;
						COVER_DETAIL("COVER scope view: the camera is not third person (camera state {}) while the aim control is held: the cover stays (iPoseScopeAim 1)", a_snap.cameraIndex);
					}
				}
			}
			const bool allowedNow = (third && s_notAllowed.empty()) || scopeView;
			if (!third && !scopeView && s_notAllowed.empty()) {
				s_notAllowed.emplace_back("camera is not third person");
			}
			const auto stanceRead = PlayerControl::ReadVar("iSyncActorStance");
			const int  stance = stanceRead ? static_cast<int>(std::lround(*stanceRead)) : -1;
			float      yaw{ 0.0f };
			const bool yawOk = Camera::ReadViewYaw(yaw);

			Core::CoverState     state{};
			Core::CoverCandidate cand;
			{
				std::lock_guard lock(s_mutex);
				s_allowed = allowedNow;
				s_allowedAt = now;
				s_third = third;
				s_stance = stance;
				s_stanceAt = now;
				s_yaw = yaw;
				s_yawOk = yawOk;
				s_pos = a_snap.position;
				s_posAt = now;
				if (worldReset) {
					s_cache.Clear();
				}
				state = s_machine.State();
				cand = s_machine.Candidate();
			}

			// ---- the machine's facts ----
			Core::MachineInputs in;
			in.now = now;
			in.allowed = allowedNow;
			in.worldReset = worldReset;
			if (state == Core::CoverState::Entering && allowedNow && !worldReset) {
				if (!s_mover.active) {
					BeginMover(a_snap, cand, now);
				}
				StepMover(a_snap, now, in);
			} else {
				s_mover.active = false;
			}
			Core::Correction correction{};
			if (state == Core::CoverState::InCover || state == Core::CoverState::Peeking) {
				// T3.3: where the player is against the tracked cover: off the line, past an end, the part of the input that pulls away from the wall
				const Core::Vec2 p{ a_snap.position.x, a_snap.position.y };
				auto             limits = Core::LimitsFor(cand, p, cfg.sensorCfg.edgeMargin);
				// low cover and a crouched player: the low pair (the crouched walk is slower than the standing one, see fCoverMaxInputLow in the INI)
				const bool lowCrouched = cand.kind == Core::CoverKind::Low && (stance == 1 || (stance >= 2 && stance <= 4));  // stances 2 to 4 are the pose's crouch
				limits.brakeDistance = lowCrouched ? cfg.edgeBrakeDistanceLow : cfg.edgeBrakeDistance;
				limits.maxInput = lowCrouched ? cfg.coverMaxInputLow : cfg.coverMaxInput;
				limits.stopDistance = lowCrouched ? cfg.edgeStopDistanceLow : cfg.edgeStopDistance;
				limits.alongMin = cfg.coverAlongMin;
				const auto limitsRaw = limits;  // the peeks measure the distance to an end from these (no blocked side)
				{
					std::lock_guard lock(s_mutex);
					Core::ApplyBlockedSide(limits, s_tf.blockedSide);  // T3.3d: after a Corner answer, no movement toward the side of the last movement until the next Ok
				}
				correction = Core::DriftCorrection(cand, p, limits, cfg.holdTolerance, cfg.holdMaxStep);
				in.deviation = std::fabs(correction.perp);
				in.shoved = shovedNow;
				bool aim = false;
				{
					std::lock_guard lock(s_mutex);
					aim = s_aimHeld;
				}
				in.aimHeld = aim;
				in.peekSpot = PeekSpotNow(a_snap, now, state, cand, limitsRaw, aim);
				{
					std::lock_guard lock(s_mutex);
					in.lostCount = s_lostCount;
					s_in.ExpireStick(now, static_cast<double>(cfg.stickExpireTime));
					// the player real input is the vector the game computed (read before our write); with no recent call of the hook, the tracked keys and stick
					const bool gameFresh = now - s_gameVecAt <= 0.25;
					in.awayInput = yawOk ? (gameFresh ? Core::AwayInput(s_gameVecX, s_gameVecY, yaw, cand.normal) : Core::AwayInput(s_in.MoveX(), s_in.MoveY(), yaw, cand.normal)) : 0.0f;
					s_filter = { true, now, yaw, cand.tangent, cand.normal, limits, s_trackStop };
					s_lastPerp = correction.perp;
					if (now - s_lastStatus >= 0.25) {
						s_lastStatus = now;
						const auto m = ModeNow(now);
						COVER_DETAIL("COVER status: {} at ({:.2f},{:.2f}), {:.3f} m off the line, room {:+.2f} / {:+.2f} m, filter {}{}, tracks {} (lost {}), aim {}, stance {}", Core::ToString(state), a_snap.position.x, a_snap.position.y, correction.perp,
							limits.remainPlus, limits.remainMinus, m == FilterMode::Wall ? "wall" : (m == FilterMode::Zero ? "zero" : "off"), s_trackStop ? " (track lost: stopped)" : "", s_tracks, s_trackLost, s_aimHeld, stance);
					}
					s_lastRemainPlus = limits.remainPlus;
					s_lastRemainMinus = limits.remainMinus;
				}
			} else {
				std::lock_guard lock(s_mutex);
				s_filter = FilterData{};
				s_trackStop = false;
			}

			bool snapNow = false;
			{
				std::lock_guard lock(s_mutex);
				const auto      before = s_machine.State();
				s_machine.SetSettings(cfg.machineCfg);  // hot reload (T3.5)
				const auto      r = s_machine.Update(in);
				const auto      after = s_machine.State();
				switch (r.event) {
				case Core::MachineEvent::Reached: {
					snapNow = r.snap;
						s_reachedAt = now;
					Note(now, before, after, r.snap ? "snapped to the stand point" : "reached the stand point");
					const float err = Horizontal(a_snap.position, { cand.standPoint.x, cand.standPoint.y, cand.standPoint.z });
					if (cfg.enterSettleTime > 0.0f) {
						s_settleActive = true;
						s_settleWrites = 0;
						s_settleUntil = now + static_cast<double>(cfg.enterSettleTime);
						s_settlePos = r.snap ? RE::NiPoint3{ cand.standPoint.x, cand.standPoint.y, a_snap.position.z } : a_snap.position;
					}
					Notify(std::format("{}: in cover ({})", Config::DisplayName(), cand.kind == Core::CoverKind::Low ? "low" : "tall").c_str());
					REX::INFO("COVER in cover ({}): {:.2f} s after the press, player ({:.2f},{:.2f},{:.2f}), {:.3f} m from the stand point, {}, stance {} (before {})", r.snap ? "snapped after the time limit" : "reached",
						now - s_mover.start, a_snap.position.x, a_snap.position.y, a_snap.position.z, err, StandText(cand), stance, s_machine.StanceBefore());
					break;
				}
				case Core::MachineEvent::EnterBlocked:
					Note(now, before, after, "the path to the stand point is blocked");
					COVER_DETAIL("COVER enter aborted: the path to the stand point is blocked (the player stays {:.2f} m behind the eased move); back to None", Horizontal(a_snap.position, s_mover.last));
					break;
				case Core::MachineEvent::ExitStarted:
					Note(now, before, after, Core::ToString(r.exitReason));
					COVER_DETAIL("COVER leaving cover: {} (player ({:.2f},{:.2f}), {:.3f} m off the line, away input {:.2f}, lost {} in a row, shoved {})", Core::ToString(r.exitReason), a_snap.position.x, a_snap.position.y, in.deviation,
						in.awayInput, in.lostCount, in.shoved);
					s_filter = FilterData{};
					s_move2.active = false;
					s_peeking = 0;
					break;
				case Core::MachineEvent::ExitDone:
					// T3.8: whatever the reason the stance held before the cover comes back, except after a Jump or a Sprint (the game's own action decides the stance there:
					// a press made now would crouch a player who is sprinting). A stance that already is the wanted one costs no press.
					if (s_machine.LastExit() != Core::ExitReason::Jump && s_machine.LastExit() != Core::ExitReason::Sprint && s_machine.StanceBefore() >= 0) {
						WantStance(s_machine.StanceBefore(), now, "left: stance as before the cover");
					}
					Notify(std::format("{}: left cover", Config::DisplayName()).c_str());
					Note(now, before, after, "exit done");
					REX::INFO("COVER left cover ({}): stance {} (before the cover {}), input unfiltered", Core::ToString(s_machine.LastExit()), stance, s_machine.StanceBefore());
					break;
				case Core::MachineEvent::HardReset:
					Note(now, before, after, std::string("hard reset: ") + r.why);
					REX::INFO("COVER hard reset from {}: {} (not allowed now: {}{}{})", Core::ToString(before), r.why, JoinReasons(s_notAllowed), worldReset ? "; " : "", worldReset ? worldWhy : "");
					ClearHooks();
					s_cache.Clear();
					s_move2.active = false;
					s_peek.phase = PeekPlan::Phase::Idle;
					s_peeking = 0;
					Camera::CoverCamReset();  // T5.3a: at once, no ease
						CoverPose::HardReset(std::string(r.why).c_str());
					break;
				case Core::MachineEvent::PeekStarted:
					Note(now, before, after, std::string("peek ") + Core::ToString(r.peek));
					s_peeking = static_cast<int>(r.peek);
					if (r.peek == Core::PeekKind::Over) {
						COVER_DETAIL("COVER peek over: aim held, the cover is {:.2f} m high: stand up (stance {} now)", cand.height, stance);
						WantStance(0, now, "peek over: stand up");
					} else {
						s_peekHome = a_snap.position;
						const float outTime = (std::max)(cfg.peekTime, Horizontal(a_snap.position, s_peek.spot) / cfg.peekSpeed);
						BeginMove2(a_snap.position, s_peek.spot, now, outTime);
						COVER_DETAIL("COVER peek {}: from ({:.2f},{:.2f}) to the spot ({:.2f},{:.2f}) over {:.2f} s", Core::ToString(r.peek), a_snap.position.x, a_snap.position.y, s_peek.spot.x, s_peek.spot.y, outTime);
					}
					break;
				case Core::MachineEvent::PeekEnded:
					Note(now, before, after, std::string("peek ended ") + Core::ToString(r.peek));
					s_peeking = 0;
					if (r.peek == Core::PeekKind::Over) {
						COVER_DETAIL("COVER peek over ended: crouch again (stance {} now, the cover wants {})", stance, s_machine.WantedStance());
						WantStance(s_machine.WantedStance(), now, "peek over ended: crouch");
						if (now - s_crouchSentAt < 1.0) {
							s_fix.nextAt = (std::max)(s_fix.nextAt, s_crouchSentAt + 0.7);  // the press of the hook is on its way: look again after it took effect
						}
					} else {
						const float backTime = (std::max)(cfg.peekTime, Horizontal(a_snap.position, s_peekHome) / cfg.peekSpeed);
						BeginMove2(a_snap.position, s_peekHome, now, backTime);
						s_nextTrack = now + backTime + 0.25;  // no Track while the player moves back from past the end of the cover
						COVER_DETAIL("COVER peek {} ended: back to ({:.2f},{:.2f}) over {:.2f} s", Core::ToString(r.peek), s_peekHome.x, s_peekHome.y, backTime);
					}
					break;
				case Core::MachineEvent::None:
					break;
				}
				if (s_checkPending && now >= s_checkAt) {
					s_checkPending = false;
					// card M4b: while a pose runs it owns the stance (field 3 or 5); the machine's wish is for the plain cover, so a difference is no mismatch
					const bool poseOwns = stance != s_checkWanted && s_checkWanted >= 0 && CoverPose::Busy();
					COVER_DETAIL("COVER stance check [{}]: wanted {}, now {} (before the press {}) -> {}", s_checkLabel, s_checkWanted, stance, s_checkBefore,
						stance == s_checkWanted ? "OK" : (poseOwns ? "the pose owns the stance (not checked)" : (s_checkWanted < 0 ? "no stance wanted" : "MISMATCH")));
				}
			}
			if (snapNow && cfg.coverMove) {
				const RE::NiPoint3 stand{ cand.standPoint.x, cand.standPoint.y, cand.standPoint.z };
				if (WritePosition(stand)) {
					s_expectJump = true;
				}
			}
			if (s_mover.active && s_machine.State() != Core::CoverState::Entering) {
				s_mover.active = false;
			}

			// T5.2: the cover poses
			if (CoverPose::Enabled() || CoverPose::Busy()) {
				Core::CoverCandidate pc;
				Core::CoverState     ps{};
				{
					std::lock_guard lock(s_mutex);
					ps = s_machine.State();
					pc = s_machine.Candidate();
				}
				s_tallEnds = { pc.edgeLeft, pc.edgeRight, pc.edgeLeftKind, pc.edgeRightKind };  // T5.2 tall pose: the tracked ends for the end rule
				const bool           wasBusy = CoverPose::Busy();
				CoverPose::Inputs    pin;
				pin.now = now;
				pin.machine = ps;
				pin.candidate = &pc;
				pin.playerZ = a_snap.position.z;
				if (ps == Core::CoverState::InCover && (cfg.poseDiagMask & 8) != 0 && stance == 1 && !wasBusy) {
					if (!s_fix.active) {
						WantStance(0, now, "A3 switch e: stand up before the pose");
					}
				}
				pin.canStart = ps == Core::CoverState::InCover && ((cfg.poseDiagMask & 8) == 0 || stance != 1) && !s_aimHeld && allowedNow && !shovedNow && !worldReset;  // T5.2b: no stance to wait for: the stance call is made from whatever stance the player has
				const auto pe = CoverPose::Tick(pin);
				if (!wasBusy && CoverPose::Busy()) {
					s_poseStancePending = -1;
					s_fix.active = false;
				}
				if (pe == CoverPose::Event::Restored && s_poseStancePending >= 0) {
					const int pending = s_poseStancePending;
					s_poseStancePending = -1;
					WantStance(pending, now, "left the pose: stance as before the cover");
				}
			}

			// T5.4 and T5.3: blind fire events, and the own-graph camera rig sets
			if (CoverPose::Enabled() || CoverPose::Busy()) {
				BlindFire::Tick(now);
				CoverPose::CornerHoldTick(now);
				{
					int slideSide{ 0 };
					if (CoverPose::TakeCornerSlide(slideSide) && cfg.poseCornerEdge > 0.0f && state == Core::CoverState::InCover) {
						const auto& cd = s_machine.Candidate();
						const float edge = slideSide > 0 ? cd.edgeRight : cd.edgeLeft;
						const auto  kind = slideSide > 0 ? cd.edgeRightKind : cd.edgeLeftKind;
						if (kind == Core::EdgeKind::Open && edge < 4.0f) {
							const float delta = edge - cfg.poseCornerEdge;  // + toward the end, - away from it
							const float tl = std::sqrt(cd.tangent.x * cd.tangent.x + cd.tangent.y * cd.tangent.y);
							if (std::fabs(delta) > 0.02f && tl > 0.5f) {
								const float        k = static_cast<float>(slideSide) * delta / tl;
								const RE::NiPoint3 to{ a_snap.position.x + cd.tangent.x * k, a_snap.position.y + cd.tangent.y * k, a_snap.position.z };
								const double       secs = (std::min)(0.60, 0.25 + 0.5 * std::fabs(static_cast<double>(delta)));
								BeginMove2(a_snap.position, to, now, secs);
								COVER_DETAIL("COVER corner aim: the open end is {:.2f} m away on the {}; the actor slides {:+.2f} m along the wall over {:.2f} s to stand {:.2f} m from it", edge, slideSide > 0 ? "right" : "left", delta, secs, cfg.poseCornerEdge);
							} else {
								COVER_DETAIL("COVER corner aim: the open end is {:.2f} m away on the {}: already at the distance ({:.2f} m)", edge, slideSide > 0 ? "right" : "left", cfg.poseCornerEdge);
							}
						} else {
							COVER_DETAIL("COVER corner aim: no open end on the {} within 4 m (edge {:.2f} m, kind {}): no slide", slideSide > 0 ? "right" : "left", edge, Core::ToString(kind));
						}
					}
				}
				if (Config::Get().poseStickLog && CoverPose::GetInfo().state == CoverPose::State::Posed && now >= s_nextStickLog) {
					s_nextStickLog = now + 0.2;
					float rx, ry, sx, sy, ox, oy, gx, gy;
					bool  os;
					{
						std::lock_guard lock(s_mutex);
						rx = s_logRawX; ry = s_logRawY; sx = s_logSentX; sy = s_logSentY; ox = s_logOutX; oy = s_logOutY; os = s_logOutSet; gx = s_gameVecX; gy = s_gameVecY;
					}
					COVER_DETAIL("POSE STICK raw=({:+.2f},{:+.2f}) given=({:+.2f},{:+.2f}) game_vec=({:+.2f},{:+.2f}) our_vec={} iSyncIdleLocomotion={:g} Speed={:g} Direction={:g} side={:g}", rx, ry, sx, sy, gx, gy,
						os ? std::format("({:+.2f},{:+.2f})", ox, oy) : std::string("-"), CoverPose::VarValue("iSyncIdleLocomotion"), CoverPose::VarValue("Speed"), CoverPose::VarValue("Direction"), CoverPose::VarValue("iSyncRightLeft"));
				}
				{
					// the first 1.8 s after a leave: what the game's movement and the weapon and stance fields do (the body stands about 0.8 s before it turns on the new exit, REFERENCE 22.20)
					static double s_nextExitLog = 0.0;
					const auto    pi = CoverPose::GetInfo();
					if (Config::Get().poseStickLog && pi.tLeave > 0.0 && now - pi.tLeave < 1.8 && pi.state != CoverPose::State::Posed && now >= s_nextExitLog) {
						s_nextExitLog = now + 0.05;
						float gx, gy;
						double ga;
						{
							std::lock_guard lock(s_mutex);
							gx = s_gameVecX; gy = s_gameVecY; ga = s_gameVecAt;
						}
						float ang = 0.0f;
						if (const auto pl = RE::PlayerCharacter::GetSingleton()) {
							ang = pl->data.angle.z;
						}
						COVER_DETAIL("EXIT WALK +{:.2f}: game_vec=({:+.2f},{:+.2f}) age {:.2f} s, our zeroing so far {}, pose {}, cover {}, weapon {}, stance field {}, iSyncActorStance {:g}, idle {:g}, Speed {:g}, Direction {:g}, bIsExitingCover {:g}, bIsInCover {:g}, angle z {:.1f} deg, input stick ({:+.2f},{:+.2f})",
							now - pi.tLeave, gx, gy, now - ga, ZeroInputCount(), CoverPose::ToString(pi.state), Core::ToString(s_machine.State()), CoverPose::WeaponState(), CoverPose::StanceField(), CoverPose::VarValue("iSyncActorStance"), CoverPose::VarValue("iSyncIdleLocomotion"),
							CoverPose::VarValue("Speed"), CoverPose::VarValue("Direction"), CoverPose::VarValue("bIsExitingCover"), CoverPose::VarValue("bIsInCover"), ang * 57.29578f, s_logRawX, s_logRawY);
					}
				}
				{
					Core::TallEnds te;
					te = s_tallEnds;
					static double s_lastMoveInput = 0.0;
					{
						float            mg = 0.0f;
						std::lock_guard  lk(s_mutex);
						(void)CurrentIntent(mg);
						if (mg > 0.2f || (Config::Get().turnLock && now < s_revUntil.load())) {
							s_lastMoveInput = now;  // 23.16: the time the reversal rule holds the input at zero is the player's push, not a stand
						}
					}
					// 23.16: the automatic turn never works against the player. After a turn the player asked for (the reversal rule's, or the game's own turn from a push) there is none until the
					// player has walked 0.5 m, aimed, fired or left; the stand-still time that triggers it is counted from the end of the push.
					static bool         s_ptlOn = false;
					static RE::NiPoint3 s_ptlPos{};
					static bool         s_sideKnown = false;
					static bool         s_sideLeftPrev = false;
					double              stillFor = now - s_lastMoveInput;
					{
						const bool posedNow = CoverPose::GetInfo().state == CoverPose::State::Posed;
						const bool sideLeft = CoverPose::VarValue("iSyncRightLeft") > 0.5;
						const bool aimingNow = CoverPose::VarValue("iIsSighted") > 0.5 || CoverPose::VarValue("iSyncSighted") > 0.5 || BlindFire::GetInfo().state != BlindFire::State::Idle;
						const auto  here = Frame::Latest().position;
						if (!posedNow || aimingNow) {
							if (s_ptlOn) {
								s_ptlOn = false;
								COVER_DETAIL("COVER turn lock released: {}", !posedNow ? "the pose ended" : "aim or fire");
							}
							s_sideKnown = false;
						} else {
							const bool ownAutoTurn = TallAim::GetInfo().state == TallAim::State::Turning || now - TallAim::GetInfo().tTurn < 2.0;
							const bool revNow = now < s_revUntil.load();
							if (Config::Get().turnLock && !s_ptlOn && (revNow || (s_sideKnown && sideLeft != s_sideLeftPrev && !ownAutoTurn))) {
								s_ptlOn = true;
								s_ptlPos = here;
								COVER_DETAIL("COVER turn lock set: the player asked for a turn ({}): no automatic turn until the player has walked 0.5 m, aimed, fired or left", revNow ? "the reversal rule" : "the game's own turn from a push");
							}
							s_sideLeftPrev = sideLeft;
							s_sideKnown = true;
							if (s_ptlOn && std::hypot(here.x - s_ptlPos.x, here.y - s_ptlPos.y) >= 0.5f) {
								s_ptlOn = false;
								COVER_DETAIL("COVER turn lock released: the player has walked 0.5 m");
							}
						}
					}
					TallAim::Tick(now, te, s_ptlOn ? 0.0 : stillFor);
					{
						// 23.16: the game's foot IK in the pose (iPoseFootIK / iPoseFootIKLow: 0 left alone, 1 off while the pose stands on the mirrored side, 2 off for the whole pose). The switch is global:
						// it is changed only when the wish changes (one log line each) and put back on every way out: the pose ending, a hard reset, no player (a load), a menu pause.
						static bool s_fikOff = false;
						const auto& cf = Config::Get();
						const auto  pi = CoverPose::GetInfo();
						const bool  posedF = pi.state == CoverPose::State::Posed;
						const int   mode = pi.lastStance == 5 ? cf.poseFootIK : cf.poseFootIKLow;
						const bool  sideL = CoverPose::VarValue("iSyncRightLeft") > 0.5;
						const auto& fs = Frame::Latest();
						const bool  wantOff = posedF && fs.valid && !fs.paused && (mode == 2 || (mode == 1 && sideL)) && s_machine.State() == Core::CoverState::InCover;
						ScopeAim::Tick(now, fs.cameraIndex == RE::CameraStates::kThirdPerson, s_aimHeld);  // W4 (iPoseScopeAim 2)
						if (ScopeAim::TakeNotice()) {
							Notify(std::format("{}: scoped weapon, no aim from cover", Config::DisplayName()).c_str());
						}
						GraphEvents::Tick(now, posedF);  // W5 (iGraphEventProbe / iPoseEquipBlock; 0 = nothing installed)
						WeaponDraw::Tick(now, posedF);  // spike W2 (rig only: iPoseWeaponDraw 0 does nothing)
						if (wantOff && !s_fikOff) {
							if (FootIK::Set(false, sideL ? "the pose stands on the mirrored side" : "the pose")) {
								s_fikOff = true;
							}
						} else if (!wantOff && s_fikOff) {
							FootIK::Restore(!posedF ? "the pose ended" : (!fs.valid || fs.paused ? "no player or a menu pause" : (mode == 1 && !sideL ? "the pose is on the open side again" : "the wish changed")));
							s_fikOff = false;
						}
					}
					{
						// card M4b (REFERENCE 23.11): the direction is reversed and the character walks against the pose's side (the walk machine turns the side round only on some pushes from rest;
						// the rest it walks on, backwards). The input is written as zero, the Enter event of the other side is sent (the one the automatic turn sends) and the input goes to the game
						// again when iSyncRightLeft has flipped (at most fPoseReverseTimeout seconds).
						static bool   s_revActive = false;
						static bool   s_revWantLeft = false;
						static double s_revStart = 0.0;
						static double s_mismatchSince = -1.0;
						static int    s_revCount = 0;
						const auto&   cr = Config::Get();
						const bool    lowPose = CoverPose::GetInfo().lastStance != 5;
						const bool    applies = cr.poseReverse >= 2 || (cr.poseReverse == 1 && !lowPose);
						const bool    poseOn = CoverPose::GetInfo().state == CoverPose::State::Posed && s_machine.State() == Core::CoverState::InCover;
						const bool    aiming = CoverPose::VarValue("iIsSighted") > 0.5 || CoverPose::VarValue("iSyncSighted") > 0.5 || BlindFire::GetInfo().state != BlindFire::State::Idle || TallAim::GetInfo().state != TallAim::State::Idle;
						if (applies && poseOn && !aiming) {
							float            mg = 0.0f;
							Core::Vec2       adir;
							{
								std::lock_guard lk(s_mutex);
								adir = CurrentIntent(mg);
							}
							const float along = Core::Dot(adir, cand.tangent) * mg;
							const bool  leftSide = CoverPose::VarValue("iSyncRightLeft") > 0.5;
							const bool  mismatch = std::fabs(along) >= cr.coverAlongMin && (along < 0.0f) != leftSide;
							if (!s_revActive) {
								if (!mismatch) {
									s_mismatchSince = -1.0;
								} else {
									if (s_mismatchSince < 0.0) {
										s_mismatchSince = now;
									}
									if (now - s_mismatchSince >= static_cast<double>(cr.poseReverseHold)) {
										s_revActive = true;
										s_revWantLeft = along < 0.0f;
										s_revStart = now;
										++s_revCount;
										s_revUntil.store(now + static_cast<double>(cr.poseReverseRest));
										bool       acc{ false };
										const bool tall = !lowPose;
										const bool sent = AnimRecon::SendEvent(s_revWantLeft ? (tall ? "EnterCoverStandingLeft" : "EnterCoverCrouchingLeft") : (tall ? "EnterCoverStandingRight" : "EnterCoverCrouchingRight"), acc);
										COVER_DETAIL("COVER reversal #{}: the input along the wall ({:+.2f}) has gone against the pose's side ({}) for {:.2f} s (the character {}): the input is written as zero and the Enter event of the {} side is sent ({} accepted {}); it goes to the game when the side has turned (at most {:.1f} s)", s_revCount, along, leftSide ? "Left" : "Right", now - s_mismatchSince,
											CoverPose::VarValue("iSyncIdleLocomotion") > 0.5 ? "walks" : "stands", s_revWantLeft ? "Left" : "Right", sent, acc, cr.poseReverseRest);
										s_mismatchSince = -1.0;
									}
								}
							} else {
								const bool flipped = leftSide == s_revWantLeft;
								if ((flipped && now - s_revStart >= 0.25) || now - s_revStart >= static_cast<double>(cr.poseReverseRest)) {
									s_revActive = false;
									s_revUntil.store(0.0);
									COVER_DETAIL("COVER reversal: over after {:.2f} s ({}), the input goes to the game again (side {}, walking {:g})", now - s_revStart, flipped ? "turned" : "NOT turned, timeout", leftSide ? "Left" : "Right", CoverPose::VarValue("iSyncIdleLocomotion"));
								} else {
									s_revUntil.store(now + 0.2);  // kept up to date while it waits
								}
							}
						} else {
							s_mismatchSince = -1.0;
							if (s_revActive) {
								s_revActive = false;
								s_revUntil.store(0.0);
							}
						}
					}
				}
				const auto cc = Camera::GetCoverCam();
				const bool elig = third && allowedNow && state == Core::CoverState::None && !CoverPose::Busy() && cc.backNow < 0.005f && cc.backTarget < 0.005f;
				CoverPose::SampleOwnRig(now, elig, s_aimHeld || CoverPose::VarValue("iIsSighted") > 0.5, stance == 1);
			}

			// T3.5: a stance asked for by the hook (modes 0 and 2), and the hold of the Sneak control (mode 2)
			{
				int req = -2;
				{
					std::lock_guard lock(s_mutex);
					req = s_fixRequest;
					s_fixRequest = -2;
				}
				if (req == 1 && CoverPose::Enabled() && ((cfg.poseDiagMask & 8) != 0 || CoverPose::WillPose(s_machine.Candidate().kind))) {
					req = -2;  // T5.2b: no crouch under the pose (the pose itself crouches the character); with no pose to come the Basic cover still crouches
				}
				if (req >= 0) {
					WantStance(req, now, "the cover needs this stance");
				}
				if (cfg.coverTrigger == 2) {
					std::lock_guard lock(s_mutex);
					if (s_hold.active && !s_hold.consumed && now - s_hold.since >= static_cast<double>(cfg.coverHoldTime) && s_machine.State() == Core::CoverState::None) {
						s_hold.consumed = true;
						float            mag = 0.0f;
						const Core::Vec2 hdir = CurrentIntent(mag);
						const auto       fresh = s_cache.Fresh(now, cfg.candidateMaxAge, hdir, cfg.candidateMaxTurnDeg);
						float            sd = 0.0f;
						if (fresh && fresh->candidate.kind != Core::CoverKind::None) {
							const auto& sp = fresh->candidate.standPoint;
							sd = std::sqrt((sp.x - a_snap.position.x) * (sp.x - a_snap.position.x) + (sp.y - a_snap.position.y) * (sp.y - a_snap.position.y));
						}
						if (allowedNow && mag >= cfg.coverStickMin && fresh && fresh->candidate.kind != Core::CoverKind::None && sd <= cfg.enterMaxDistance
							&& s_machine.RequestEnter(now, fresh->candidate, s_hold.stanceAtPress)) {
							++s_entered;
							++s_coverId;
							s_lostCount = 0;
							s_tf = Core::TrackFilter{};
							s_lastSide = 0;
							s_filter = FilterData{};
							Note(now, Core::CoverState::None, Core::CoverState::Entering, "the Sneak control held");
							s_cache.Clear();
							s_fixRequest = s_machine.WantedStance();
							COVER_DETAIL("COVER hold [mode 2]: the Sneak control was held {:.2f} s with the input pushed toward cover: {} cover, stand point {:.2f} m away; stance {} before the press, the cover wants {}", now - s_hold.since,
								s_machine.Candidate().kind == Core::CoverKind::Low ? "LOW" : "HIGH", sd, s_hold.stanceAtPress, s_machine.WantedStance());
						} else {
							COVER_DETAIL("COVER hold [mode 2]: the Sneak control was held {:.2f} s but there is no cover to take (input {:.2f}, sensor {}, allowed {}): it stays the vanilla toggle", now - s_hold.since, mag,
								fresh ? (fresh->candidate.kind == Core::CoverKind::None ? "found nothing" : "cover") : "no fresh answer", allowedNow);
						}
					}
				}
			}

			// M1b, mode 3: cover without a press
			if (cfg.coverTrigger == 3) {
				std::lock_guard      lock(s_mutex);
				float                mag = 0.0f;
				const Core::Vec2     adir = CurrentIntent(mag);
				const auto           fresh = s_cache.Fresh(now, cfg.candidateMaxAge, adir, cfg.candidateMaxTurnDeg);
				Core::AutoCoverInput ai;
				ai.now = now;
				ai.state = s_machine.State();
				ai.allowed = allowedNow && yawOk;
				ai.sprinting = s_in.SprintHeld();
				ai.moveMagnitude = mag;
				ai.stickMin = cfg.coverStickMin;
				ai.candidateFresh = fresh.has_value();
				ai.kind = fresh ? fresh->candidate.kind : Core::CoverKind::None;
				if (fresh && fresh->candidate.kind != Core::CoverKind::None) {
					const auto& sp = fresh->candidate.standPoint;
					ai.standDistance = std::sqrt((sp.x - a_snap.position.x) * (sp.x - a_snap.position.x) + (sp.y - a_snap.position.y) * (sp.y - a_snap.position.y));
					ai.angleDeg = Core::AngleToWallDeg(adir, fresh->candidate.normal);
					ai.openEndDistance = Core::NearestOpenEnd(fresh->candidate);
				}
				ai.weaponDrawn = CoverPose::WeaponState() == 3;
				Core::AutoCoverSettings as;
				as.time = cfg.autoCoverTime;
				as.rearm = cfg.autoCoverRearm;
				as.angleDeg = cfg.autoCoverAngleDeg;
				as.distance = cfg.autoCoverDistance;
				as.tallEdge = cfg.autoCoverTallEdge;
				as.tallMode = cfg.autoCoverTall;
				as.timeTall = cfg.autoCoverTimeTall;
				as.timeDrawn = cfg.autoCoverTimeDrawn;
				const auto ar = s_auto.Update(as, ai);
				if (ar.enter) {
					if (fresh && ai.standDistance <= cfg.enterMaxDistance && s_machine.RequestEnter(now, fresh->candidate, stance)) {
						++s_entered;
						++s_coverId;
						s_lostCount = 0;
						s_tf = Core::TrackFilter{};
						s_lastSide = 0;
						s_filter = FilterData{};
						Note(now, Core::CoverState::None, Core::CoverState::Entering, "automatic cover");
						s_cache.Clear();
						s_fixRequest = s_machine.WantedStance();
						s_checkPending = true;
						s_checkAt = now + static_cast<double>(cfg.stanceCheckTime) + 0.9;
						s_checkWanted = s_machine.WantedStance();
						s_checkBefore = stance;
						s_checkLabel = "enter (automatic)";
						COVER_DETAIL("COVER automatic [mode 3]: {} cover, stand point {:.2f} m away, the input {:.0f} deg off the direction to the wall, nearest open end {}, the condition held {:.2f} s; stance {} now, the cover wants {}",
							s_machine.Candidate().kind == Core::CoverKind::Low ? "LOW" : "HIGH", ai.standDistance, ai.angleDeg,
							ai.openEndDistance > 1e8f ? std::string("none") : std::format("{:.2f} m", ai.openEndDistance), ar.heldFor, stance, s_machine.WantedStance());
					} else {
						COVER_DETAIL("COVER automatic [mode 3]: the machine did not take the cover (stand point {:.2f} m away)", ai.standDistance);
					}
				}
			}

			// the drift correction: removes small drift that movement input caused. Not a leash: only while the player is on the ground (any "not allowed"
			// reading, airborne among them, has reset the state already), only below fMaxDeviation, never in the tick of a shove
			if (cfg.coverMove && correction.needed && !s_move2.active && !shovedNow && !(CoverPose::Busy() && CoverPose::GetInfo().state != CoverPose::State::Posed) && state == Core::CoverState::InCover && s_machine.State() == Core::CoverState::InCover && allowedNow && in.deviation <= cfg.machineCfg.maxDeviation) {
				const RE::NiPoint3 target{ a_snap.position.x + correction.delta.x, a_snap.position.y + correction.delta.y, a_snap.position.z };
				if (WritePosition(target)) {
					s_expectJump = true;
				}
			}

			// T3.3f: the settle hold. Only the first fEnterSettleTime seconds after the stand point was reached and only while the along part of the input is below
			// fCoverAlongMin: a player more than 0.03 m from where it was reached is brought back (at most 0.10 m a tick). Not a leash.
			if (s_settleActive) {
				if (s_machine.State() != Core::CoverState::InCover || now >= s_settleUntil || cfg.enterSettleTime <= 0.0f || CoverPose::Busy()) {
					s_settleActive = false;
					COVER_DETAIL("COVER settle hold ended: {} ({} corrections)", s_machine.State() != Core::CoverState::InCover ? "the state is no longer InCover" : (now >= s_settleUntil ? "the time is up" : "switched off"), s_settleWrites);
				} else if (cfg.coverMove && allowedNow && !shovedNow && !s_move2.active && yawOk) {
					float along = 0.0f;
					{
						std::lock_guard lock(s_mutex);
						const bool       gameFresh = now - s_gameVecAt <= 0.25;
						const Core::Vec2 w = gameFresh ? Core::InputToWorld(yaw, s_gameVecX, s_gameVecY) : Core::InputToWorld(yaw, s_in.MoveX(), s_in.MoveY());
						along = Core::Dot(w, cand.tangent);
					}
					if (std::fabs(along) >= cfg.coverAlongMin) {
						s_settleActive = false;
						COVER_DETAIL("COVER settle hold ended: the along part of the input is {:+.2f} ({} corrections)", along, s_settleWrites);
					} else {
						const float dist = Horizontal(a_snap.position, s_settlePos);
						if (dist > 0.03f) {
							const float step = (std::min)(dist, 0.10f);
							const RE::NiPoint3 target{ a_snap.position.x + (s_settlePos.x - a_snap.position.x) / dist * step, a_snap.position.y + (s_settlePos.y - a_snap.position.y) / dist * step, a_snap.position.z };
							if (WritePosition(target)) {
								s_expectJump = true;
								if (++s_settleWrites == 1) {
									COVER_DETAIL("COVER settle hold: the player is {:.3f} m from where it was reached: brought back ({:.3f} m this tick)", dist, step);
								}
							}
						}
					}
				}
			}

			// T3.4: the eased peek move and the stance service
			StepMove2(now);
			if (s_machine.State() == Core::CoverState::None && s_move2.active) {
				s_move2.active = false;
			}
			StepStanceFix(now, stance);

			// T5.3a: the cover camera's target for this tick (the hook site eases toward it and adds it to the rig nodes)
			{
				float back = 0.0f;
				float up = 0.0f;
				{
					int             kind = 0;
					std::lock_guard lock(s_mutex);
					const auto      st = s_machine.State();
					if (st == Core::CoverState::InCover || st == Core::CoverState::Peeking) {
						kind = s_machine.Candidate().kind == Core::CoverKind::Low ? 1 : 2;
					}
					Camera::SetCoverKind(kind, s_aimHeld);
				}
				if (cfg.coverCamera) {
					std::lock_guard lock(s_mutex);
					const auto      st = s_machine.State();
					if (st == Core::CoverState::InCover || st == Core::CoverState::Peeking) {
						const bool  low = s_machine.Candidate().kind == Core::CoverKind::Low;
						const float share = s_aimHeld ? cfg.coverCamAimShare : 1.0f;
						back = (low ? cfg.coverCamBackLow : cfg.coverCamBackTall) * share;
						up = (low ? cfg.coverCamUpLow : cfg.coverCamUpTall) * share;
					}
				}
				Camera::SetCoverCam(back, up);
			}

			// ---- background sensing: while the movement input points somewhere and nothing is taken ----
			auto& sensor = *s_sensor;
			PumpSensor(now);
			Core::CoverState nowState{};
			float            magnitude = 0.0f;
			Core::Vec2       dir{};
			{
				std::lock_guard lock(s_mutex);
				nowState = s_machine.State();
				dir = CurrentIntent(magnitude);
			}
			if (sensor.Busy() && (s_queryIsTrack ? (nowState != Core::CoverState::InCover || now - s_queryStart > 1.0) : (nowState != Core::CoverState::None || now - s_queryStart > 3.0))) {
				sensor.Cancel();
			}
			// T3.3: Track at fTrackRateHz while the player moves along the cover (and every 0.5 s while standing, so a wall that vanishes is noticed)
			if (nowState == Core::CoverState::InCover && allowedNow && WorldProbe::Available() && !sensor.Busy() && now >= s_nextTrack) {
				const float movedSinceTrack = Horizontal(a_snap.position, s_lastTrackPos);
				if (movedSinceTrack >= 0.03f || now - s_queryStart >= 0.5) {
					Core::CoverCandidate current;
					{
						std::lock_guard lock(s_mutex);
						current = s_machine.Candidate();
					}
					sensor.SetSettings(CoverSense::Settings());
					s_queryIsTrack = true;
					s_queryCover = s_coverId;
					s_queryStart = now;
					s_lastTrackPos = a_snap.position;
					s_nextTrack = now + 1.0 / static_cast<double>((std::max)(cfg.sensorCfg.trackRateHz, 1.0f));
					(void)sensor.Track(current, { a_snap.position.x, a_snap.position.y, a_snap.position.z });
				}
			}
			const bool       pushed = magnitude >= cfg.coverStickMin;
			const bool       senseNow = ((cfg.coverTrigger == 1 || cfg.coverTrigger == 3) && pushed) || ((cfg.coverTrigger == 0 || cfg.coverTrigger == 2) && yawOk);
			const Core::Vec2 senseDir = (pushed || cfg.coverTrigger == 1 || cfg.coverTrigger == 3) ? dir : Core::Vec2{ std::sin(yaw), std::cos(yaw) };  // modes 0 and 2: along the view when nothing is pushed
			if (senseNow && nowState == Core::CoverState::None && allowedNow && yawOk && WorldProbe::Available() && !sensor.Busy() && now >= s_nextFind) {
				sensor.SetSettings(CoverSense::Settings());
				s_queryIsTrack = false;
				s_queryIntent = senseDir;
				s_queryFeet = { a_snap.position.x, a_snap.position.y, a_snap.position.z };
				s_queryStart = now;
				s_nextFind = now + 1.0 / static_cast<double>((std::max)(cfg.senseRateHz, 0.5f));
				(void)sensor.Find(s_queryFeet, senseDir);
			}
		}
	}

	void OnTick(const Frame::Snapshot& a_snap)
	{
		if (!Enabled()) {
			if (s_wasEnabled) {
				s_wasEnabled = false;
				{
					std::lock_guard lock(s_mutex);
					const auto      from = s_machine.State();
					(void)s_machine.Reset();
					s_cache.Clear();
					ClearHooks();
					s_in = {};
					Note(a_snap.time, from, Core::CoverState::None, "the feature was switched off");
				}
				s_mover.active = false;
				if (s_sensor && s_sensor->Busy()) {
					s_sensor->Cancel();
				}
				REX::INFO("COVER: switched off ({}); state reset to None, every input filter removed", s_faulted.load() ? "after a fault" : "bCover=0 or the rewrite hook is not working");
			}
			return;
		}
		if (!s_wasEnabled) {
			s_wasEnabled = true;
			REX::INFO("COVER: the cover feature is ON (trigger mode {}, stick minimum {:.2f}, candidate age limit {:.2f} s, sense rate {:.0f} Hz)", Config::Get().coverTrigger, Config::Get().coverStickMin,
				Config::Get().candidateMaxAge, Config::Get().senseRateHz);
		}
		unsigned long code{ 0 };
		if (!Guarded([&] { TickImpl(a_snap); }, code)) {
			s_faulted = true;
			REX::ERROR("COVER: exception 0x{:08X} in the cover tick; the cover feature is off for this session", code);
		}
	}

	// ======================================= rig and log =====================================================================================
	Info GetInfo()
	{
		Info            i;
		const double    now = Now();
		std::lock_guard lock(s_mutex);
		i.enabled = Enabled();
		i.faulted = s_faulted.load();
		i.state = s_machine.State();
		i.peek = s_machine.Peek();
		i.lastExit = s_machine.LastExit();
		i.candidate = s_machine.Candidate();
		i.stance = s_stance;
		i.stanceBefore = s_machine.StanceBefore();
		if (const auto& latest = s_cache.Latest()) {
			i.haveCached = true;
			i.cachedKind = latest->candidate.kind;
			i.cachedAge = now - latest->time;
		}
		i.presses = s_presses;
		i.entered = s_entered;
		i.swallowed = s_swallowed;
		i.senses = s_senses;
		i.tracks = s_tracks;
		i.trackLost = s_trackLost;
		i.lostCount = s_lostCount;
		i.perp = s_lastPerp;
		i.remainPlus = s_lastRemainPlus;
		i.remainMinus = s_lastRemainMinus;
		{
			const auto fm = ModeNow(now);
			i.filterMode = fm == FilterMode::Zero ? 1 : (fm == FilterMode::Wall ? 2 : 0);
		}
		i.positionWrites = s_dbgWrites;
		i.positionWritesBlocked = s_dbgWritesBlocked;
		i.aimHeldSeen = s_aimHeld;
		return i;
	}

	InputDir GetInputDir()
	{
		InputDir        d;
		std::lock_guard lock(s_mutex);
		d.magnitude = s_in.MoveMagnitude();
		d.valid = s_yawOk;
		d.yaw = s_yaw;
		if (s_yawOk && d.magnitude > 0.01f) {
			d.dir = Core::Normalized(WorldDir(s_yaw, s_in.MoveX(), s_in.MoveY()));
		}
		return d;
	}

	std::vector<Transition> Transitions(const std::size_t a_from)
	{
		std::lock_guard lock(s_mutex);
		if (a_from >= s_transitions.size()) {
			return {};
		}
		return { s_transitions.begin() + static_cast<std::ptrdiff_t>(a_from), s_transitions.end() };
	}

	std::size_t TransitionCount()
	{
		std::lock_guard lock(s_mutex);
		return s_transitions.size();
	}

	void ForceReset(const char* a_why)
	{
		HardResetNow(Now(), a_why);
	}
}
