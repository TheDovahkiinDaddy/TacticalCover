#include "Engine/TallAim.h"

#include <mutex>

#include "Config.h"
#include "Engine/AnimRecon.h"
#include "Engine/BlindFire.h"
#include "Engine/CoverPose.h"
#include "Engine/Frame.h"

namespace Engine::TallAim
{
	namespace
	{
		constexpr double kIntoTime = 1.50;  // coverstanding Into (22.2)
		constexpr double kOutTime = 1.67;
		constexpr double kTurnTime = 1.00;  // coverstanding_tomirror 1.00 s; iSyncRightLeft flips at its end
		constexpr double kTurnTimeout = 2.5;

		std::mutex  s_mutex;
		Info        s_info;
		Core::TallEnds s_ends;
		bool        s_haveEnds{ false };
		bool        s_aimDown{ false };
		bool        s_startReq{ false };
		bool        s_endReq{ false };
		bool        s_turnReq{ false };
		bool        s_pendingAim{ false };
		bool        s_dropAim{ false };  // the aim press that does nothing (no open end in reach): its repeats and its release are swallowed too, so the engine does not aim by itself either (run m3i-1: it does)
		bool        s_turnToLeft{ false };
		double      s_tOutOver{ 0.0 };
		double      s_tTurnSent{ 0.0 };
		double      s_tAutoTurn{ -10.0 };

		[[nodiscard]] double Now() { return Frame::Now(); }

		[[nodiscard]] bool SideLeft() { return CoverPose::VarValue("iSyncRightLeft") > 0.5; }

		bool Send(const char* a_event)
		{
			bool       accepted{ false };
			const bool ran = AnimRecon::SendEvent(a_event, accepted);
			COVER_DETAIL("TALL AIM: event {} sent {}, accepted {} (iSyncCoverStates {:g}, iIsSighted {:g}, iSyncSighted {:g}, iSyncRightLeft {:g}, bIsInCover {:g})", a_event, ran, accepted, CoverPose::VarValue("iSyncCoverStates"),
				CoverPose::VarValue("iIsSighted"), CoverPose::VarValue("iSyncSighted"), CoverPose::VarValue("iSyncRightLeft"), CoverPose::VarValue("bIsInCover"));
			return ran;
		}

		[[nodiscard]] bool ActiveLocked(const double a_now)
		{
			const auto& cfg = Config::Get();
			if (!cfg.poseTall || !cfg.poseTallAim || !CoverPose::Enabled()) {
				return false;
			}
			const auto p = CoverPose::GetInfo();
			return p.state == CoverPose::State::Posed && p.lastStance == 5 && a_now - p.tPosed >= 0.5 && CoverPose::WeaponState() == 3;
		}

		void ResetLocked(const char* a_why)
		{
			if (s_info.state != State::Idle || s_startReq || s_endReq || s_turnReq) {
				COVER_DETAIL("TALL AIM: reset ({}), state was {}", a_why, ToString(s_info.state));
			}
			s_info.state = State::Idle;
			s_startReq = s_endReq = s_turnReq = s_pendingAim = false;
		}
	}

	const char* ToString(const State a_state) noexcept
	{
		switch (a_state) {
		case State::Idle: return "Idle";
		case State::Turning: return "Turning";
		case State::Into: return "Into";
		case State::Aiming: return "Aiming";
		case State::Out: return "Out";
		}
		return "?";
	}

	Info GetInfo()
	{
		std::lock_guard lock(s_mutex);
		return s_info;
	}

	bool Active()
	{
		std::lock_guard lock(s_mutex);
		return ActiveLocked(Now());
	}

	Core::TallVerb Decide()
	{
		std::lock_guard lock(s_mutex);
		if (!s_haveEnds) {
			return Core::TallVerb::None;
		}
		return Core::DecideTallVerb(s_ends, SideLeft(), Config::Get().edgePeekRange);
	}

	bool Busy()
	{
		std::lock_guard lock(s_mutex);
		return s_info.state != State::Idle && s_info.state != State::Aiming;
	}

	void StartTurn(const char* a_why, const bool a_aimWanted)
	{
		std::lock_guard lock(s_mutex);
		if (s_info.state != State::Idle) {
			return;
		}
		s_turnToLeft = !SideLeft();
		s_info.state = State::Turning;
		s_info.tTurn = Now();
		s_info.lastTurnToLeft = s_turnToLeft;
		s_turnReq = true;
		s_pendingAim = a_aimWanted;
		COVER_DETAIL("TALL AIM: {}: only the {} end is open and in reach: the actor turns to the {} first", a_why, s_turnToLeft ? "left" : "right", s_turnToLeft ? "left" : "right");
	}

	Plan OnAim(const bool a_down, const bool a_pressEdge)
	{
		Plan            plan;
		std::lock_guard lock(s_mutex);
		const double    now = Now();
		if (s_dropAim) {
			plan.swallow = true;
			s_aimDown = a_down;
			if (!a_down) {
				s_dropAim = false;
				s_pendingAim = false;  // let go during the turn: no aim afterwards
				if (s_info.state == State::Into || s_info.state == State::Aiming) {
					s_endReq = true;  // the aim the plugin started after the turn ends with the control (run m4b4-2: without this the aim stayed on)
				}
			}
			return plan;
		}
		s_aimDown = a_down;
		if (!ActiveLocked(now)) {
			if (s_info.state != State::Idle && !CoverPose::Active()) {
				ResetLocked("the pose is gone");
			}
			return plan;
		}
		if (a_down && a_pressEdge) {
			if (s_info.state == State::Turning) {
				// card M4b: a press that arrives during a turn (the player's own or the automatic one) is swallowed to its release (the engine's own aim would stop the turn,
				// REFERENCE 23.8); the plugin starts the aim when the turn is over if the control is still held
				s_pendingAim = true;
				s_dropAim = true;
				COVER_DETAIL("TALL AIM: the aim press at {:.3f} arrives during the turn: swallowed, the aim starts when the turn is over if it is still held", now);
				plan.swallow = true;
				return plan;
			}
			if (s_info.state != State::Idle) {
				return plan;  // a clip is running: the press waits for nothing, the player presses again
			}
			if (!s_haveEnds) {
				++s_info.refused;
				return plan;
			}
			const auto verb = Core::DecideTallVerb(s_ends, SideLeft(), Config::Get().edgePeekRange);
			COVER_DETAIL("TALL AIM: the aim press at {:.3f}: side {}, ends left {:.2f} m ({}) right {:.2f} m ({}), range {:.2f} m: {}", now, SideLeft() ? "Left" : "Right", s_ends.left, Core::ToString(s_ends.leftKind), s_ends.right, Core::ToString(s_ends.rightKind),
				Config::Get().edgePeekRange, Core::ToString(verb));
			if (verb == Core::TallVerb::Here) {
				s_info.state = State::Into;
				s_info.tStart = now;
				s_info.tIntoOver = now + kIntoTime;
				s_startReq = true;
			} else if (verb == Core::TallVerb::Turn) {
				s_turnToLeft = !SideLeft();
				s_info.state = State::Turning;
				s_info.tTurn = now;
				s_info.lastTurnToLeft = s_turnToLeft;
				s_turnReq = true;
				s_pendingAim = true;
				s_dropAim = true;  // card M4b: the real press is held back during the turn and to its release
			} else {
				++s_info.refused;
				s_dropAim = true;
			}
			plan.swallow = verb == Core::TallVerb::None || verb == Core::TallVerb::Turn || Config::Get().poseTallAimSwallow != 0;
		} else if (!a_down) {
			s_pendingAim = false;
			if (s_info.state == State::Into || s_info.state == State::Aiming) {
				s_endReq = true;  // sent by the tick, never before Into is over
			}
		}
		return plan;
	}

	void Tick(const double a_now, const Core::TallEnds& a_ends, const double a_stillFor)
	{
		std::lock_guard lock(s_mutex);
		s_ends = a_ends;
		s_haveEnds = true;
		{
			// card M4b: idle in the tall pose, the stick still, the only (or clearly the nearer) open end within range on the other side: turn to it without a press
			const auto& cfg = Config::Get();
			if (cfg.tallAutoTurn && s_info.state == State::Idle && !s_aimDown && !s_startReq && !s_endReq && !s_turnReq && ActiveLocked(a_now) && a_stillFor >= static_cast<double>(cfg.tallAutoTurnStill) &&
				CoverPose::VarValue("iIsSighted") < 0.5 && CoverPose::VarValue("iSyncSighted") < 0.5 && BlindFire::GetInfo().state == BlindFire::State::Idle && a_now - s_tAutoTurn > 2.0 &&
				Core::DecideAutoTurn(a_ends, SideLeft(), cfg.edgePeekRange, cfg.tallAutoTurnMargin)) {
				s_tAutoTurn = a_now;
				s_turnToLeft = !SideLeft();
				s_info.state = State::Turning;
				s_info.tTurn = a_now;
				s_info.lastTurnToLeft = s_turnToLeft;
				s_turnReq = true;
				s_pendingAim = false;
				COVER_DETAIL("TALL AIM: the actor stands still {:.2f} s on the {} side with the {} end open and within {:.2f} m (left {:.2f} m, right {:.2f} m): it turns to the {} by itself", a_stillFor, SideLeft() ? "Left" : "Right", s_turnToLeft ? "left" : "right", cfg.edgePeekRange, a_ends.left, a_ends.right, s_turnToLeft ? "left" : "right");
			}
		}
		if (!ActiveLocked(a_now) && !(CoverPose::GetInfo().state == CoverPose::State::Posed && s_info.state != State::Idle)) {
			if (s_info.state != State::Idle || s_startReq || s_endReq || s_turnReq) {
				ResetLocked(CoverPose::GetInfo().state != CoverPose::State::Posed ? "the pose is not Posed" : "the tall aim is off or the weapon is not drawn");
			}
			return;
		}
		if (s_turnReq) {
			s_turnReq = false;
			if (Send(s_turnToLeft ? "EnterCoverStandingLeft" : "EnterCoverStandingRight")) {
				++s_info.turns;
				s_tTurnSent = a_now;
			}
		}
		if (s_info.state == State::Turning && s_tTurnSent > 0.0) {
			const bool flipped = SideLeft() == s_turnToLeft;
			if ((flipped && a_now - s_tTurnSent >= 0.3) || a_now - s_tTurnSent > kTurnTimeout) {
				s_info.tTurnDone = a_now;
				COVER_DETAIL("TALL AIM: the turn is over {:.2f} s after the event: iSyncRightLeft {:g} ({})", a_now - s_tTurnSent, CoverPose::VarValue("iSyncRightLeft"), flipped ? "turned" : "NOT turned, timeout");
				s_info.state = State::Idle;
				s_tTurnSent = 0.0;
				if (s_pendingAim && s_aimDown && flipped) {
					s_pendingAim = false;
					s_info.state = State::Into;
					s_info.tStart = a_now;
					s_info.tIntoOver = a_now + kIntoTime;
					s_startReq = true;
				}
			}
		}
		if (s_startReq) {
			s_startReq = false;
			if (Send("SightedStart")) {
				++s_info.aimStarts;
			}
		}
		if (s_info.state == State::Into && a_now >= s_info.tIntoOver) {
			s_info.state = State::Aiming;
		}
		if (s_endReq && a_now >= s_info.tIntoOver && (s_info.state == State::Into || s_info.state == State::Aiming)) {
			s_endReq = false;
			if (Send("SightedEnd")) {
				++s_info.aimEnds;
				s_info.tEnd = a_now;
			}
			s_info.state = State::Out;
			s_tOutOver = a_now + kOutTime;
		}
		if (s_info.state == State::Out && a_now >= s_tOutOver) {
			s_info.state = State::Idle;
			COVER_DETAIL("TALL AIM: the Out clip is over: iIsSighted {:g}, iSyncSighted {:g}, iSyncCoverStates {:g}", CoverPose::VarValue("iIsSighted"), CoverPose::VarValue("iSyncSighted"), CoverPose::VarValue("iSyncCoverStates"));
		}
	}

	void Reset(const char* a_why)
	{
		std::lock_guard lock(s_mutex);
		ResetLocked(a_why);
	}

	void RequestEnd(const char* a_why)
	{
		std::lock_guard lock(s_mutex);
		if (s_info.state == State::Into || s_info.state == State::Aiming) {
			s_endReq = true;
			s_pendingAim = false;
			COVER_DETAIL("TALL AIM: the aim is ended by the plugin ({}); state {}", a_why, ToString(s_info.state));
		} else {
			COVER_DETAIL("TALL AIM: an end was asked for ({}) but no aim clip is running (state {})", a_why, ToString(s_info.state));
		}
	}
}
