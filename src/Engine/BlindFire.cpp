#include "Engine/BlindFire.h"

#include <mutex>

#include "Config.h"
#include "Engine/AnimRecon.h"
#include "Engine/CoverPose.h"
#include "Engine/Input.h"
#include "Engine/TallAim.h"
#include "Engine/Frame.h"

namespace Engine::BlindFire
{
	namespace
	{
		std::mutex s_mutex;
		Info       s_info;
		bool       s_low{ true };
		bool       s_startReq{ false };
		bool       s_endReq{ false };
		double     s_tIntoOver{ 0.0 };
		bool       s_aimDown{ false };
		bool       s_pendingShot{ false };  // the control was let go during Into: one fire press of our own when Into ends
		int        s_dev{ 0 }, s_id{ 0 };
		double     s_shotEnd{ 0.0 };
		bool       s_dropFire{ false };  // T5.2 tall pose: a fire press that does nothing (no open end in reach, a turn instead): its repeats and its release are swallowed too
		double     s_aimUntil{ 0.0 };

		[[nodiscard]] double Now() { return Frame::Now(); }

		// the pose is on and settled, the weapon is drawn and the aim is not held (caller holds the mutex)
		[[nodiscard]] bool Gate(const double a_now)
		{
			const auto& cfg = Config::Get();
			if (!cfg.poseBlindFire || !CoverPose::Enabled()) {
				return false;
			}
			const auto p = CoverPose::GetInfo();
			return p.state == CoverPose::State::Posed && a_now - p.tPosed >= 0.5 && CoverPose::WeaponState() == 3;
		}

		void ResetLocked(const char* a_why)
		{
			if (s_info.state != State::Idle || s_startReq || s_endReq || s_info.aimDeferred) {
				COVER_DETAIL("BLIND FIRE: reset ({}), state was {}", a_why, ToString(s_info.state));
			}
			s_info.state = State::Idle;
			s_info.aimDeferred = false;
			s_startReq = s_endReq = false;
		}

		bool Send(const char* a_event)
		{
			bool       accepted{ false };
			const bool ran = AnimRecon::SendEvent(a_event, accepted);
			COVER_DETAIL("BLIND FIRE: event {} sent {}, accepted {} (iIsBlindFiring {:g}, iSyncCoverStates {:g}, bIsInCover {:g})", a_event, ran, accepted, CoverPose::VarValue("iIsBlindFiring"), CoverPose::VarValue("iSyncCoverStates"), CoverPose::VarValue("bIsInCover"));
			return ran;
		}
	}

	const char* ToString(const State a_state) noexcept
	{
		switch (a_state) {
		case State::Idle: return "Idle";
		case State::Into: return "Into";
		case State::Firing: return "Firing";
		case State::Linger: return "Linger";
		}
		return "?";
	}

	Info GetInfo()
	{
		std::lock_guard lock(s_mutex);
		return s_info;
	}

	Plan OnFire(const bool a_down, const bool a_pressEdge, const int a_device, const int a_id)
	{
		Plan            plan;
		std::lock_guard lock(s_mutex);
		const double    now = Now();
		const auto&     cfg = Config::Get();
		if (s_dropFire) {
			plan.swallow = true;
			if (!a_down) {
				s_dropFire = false;
			}
			return plan;
		}
		if (!Gate(now) || s_aimDown || s_info.aimDeferred) {
			if (s_info.state != State::Idle && !CoverPose::Active()) {
				ResetLocked("the pose is gone");
			}
			return plan;
		}
		auto& st = s_info.state;
		if (a_down && a_pressEdge && st == State::Idle && CoverPose::GetInfo().lastStance == 5) {
			// T5.2 tall pose: the same end rule as the aim (TallAim.cpp)
			if (!cfg.poseTall || !cfg.poseTallBlindFire) {
				return plan;  // the verb is off at tall cover: the press goes to the game as it did before
			}
			if (TallAim::Busy()) {
				plan.swallow = true;
				s_dropFire = true;
				return plan;
			}
			const auto verb = TallAim::Decide();
			if (verb != Core::TallVerb::Here) {
				COVER_DETAIL("BLIND FIRE: tall cover, the fire press at {:.3f}: {} (the end rule of the aim)", now, Core::ToString(verb));
				if (verb == Core::TallVerb::Turn) {
					TallAim::StartTurn("blind fire", false);
				}
				plan.swallow = true;
				s_dropFire = true;
				return plan;
			}
		}
		if (a_down && a_pressEdge) {
			if (st == State::Idle) {
				s_low = CoverPose::GetInfo().lastStance != 5;
				st = State::Into;
				s_info.tPress = now;
				s_tIntoOver = now + static_cast<double>(s_low ? cfg.blindFireIntoLow : cfg.blindFireIntoTall);
				s_startReq = true;
				s_pendingShot = false;
				s_dev = a_device;
				s_id = a_id;
				++s_info.swallowed;
				COVER_DETAIL("BLIND FIRE: the fire press at {:.3f} is swallowed, {} asked for, Into over at +{:.2f} s", now, s_low ? "BlindFireOverStart" : "BlindFireStart", s_tIntoOver - now);
				plan.swallow = true;
			} else if (st == State::Linger) {
				if (now < s_tIntoOver) {
					st = State::Into;  // the Into clip is still playing: the press waits for it
					s_pendingShot = false;
					++s_info.swallowed;
					plan.swallow = true;
				} else {
					st = State::Firing;
					++s_info.passedInLinger;
					COVER_DETAIL("BLIND FIRE: a press {:.2f} s after the release passes straight to the game (the mode is on)", now - s_info.tRelease);
				}
			} else if (st == State::Into) {
				++s_info.swallowed;
				plan.swallow = true;
			}
			return plan;
		}
		if (a_down) {  // a repeat of the held control
			if (st == State::Into) {
				if (now >= s_tIntoOver) {
					st = State::Firing;
					++s_info.freed;
					s_info.tFreed = now;
					plan.resetHeld = true;
					COVER_DETAIL("BLIND FIRE: Into is over, {:.3f} s after the press: the held control is passed to the game as a fresh press (device and id of the real control)", now - s_info.tPress);
				} else {
					++s_info.swallowed;
					plan.swallow = true;
				}
			}
			return plan;
		}
		// the release
		if (st == State::Into) {
			st = State::Linger;
			s_info.tRelease = now;
			++s_info.swallowed;
			plan.swallow = true;
			s_pendingShot = true;
			COVER_DETAIL("BLIND FIRE: the control was let go {:.3f} s after the press, before Into was over: one fire press of the plugin's own follows when Into ends", now - s_info.tPress);
		} else if (st == State::Firing) {
			st = State::Linger;
			s_info.tRelease = now;
			COVER_DETAIL("BLIND FIRE: the release passes to the game; the mode stays for {:.2f} s", cfg.blindFireLinger);
		}
		return plan;
	}

	Plan OnAim(const bool a_down, const bool a_pressEdge)
	{
		Plan            plan;
		std::lock_guard lock(s_mutex);
		const double    now = Now();
		const auto&     cfg = Config::Get();
		if (s_info.aimDeferred) {
			if (!a_down) {
				s_info.aimDeferred = false;
				s_aimDown = false;
				plan.swallow = true;  // the release of a swallowed press
			} else if (now >= s_aimUntil) {
				s_info.aimDeferred = false;
				s_aimDown = true;
				plan.resetHeld = true;
				COVER_DETAIL("BLIND FIRE: the Out clip is over: the held aim is passed to the game as a fresh press");
			} else {
				plan.swallow = true;
			}
			return plan;
		}
		if (a_down && a_pressEdge && s_info.state != State::Idle) {
			s_endReq = true;
			s_info.state = State::Idle;
			s_info.aimDeferred = true;
			s_aimUntil = now + static_cast<double>(s_low ? cfg.blindFireOutLow : cfg.blindFireOutTall);
			COVER_DETAIL("BLIND FIRE: the aim was pressed during blind fire: BlindFireEnd, the aim press waits {:.2f} s for the Out clip", s_aimUntil - now);
			plan.swallow = true;
			return plan;
		}
		s_aimDown = a_down;
		return plan;
	}

	void Tick(const double a_now)
	{
		std::lock_guard lock(s_mutex);
		const bool      posed = CoverPose::GetInfo().state == CoverPose::State::Posed;
		if (!posed || !Config::Get().poseBlindFire) {
			if (s_info.state != State::Idle || s_info.aimDeferred || s_startReq || s_endReq) {
				ResetLocked(posed ? "bPoseBlindFire is off" : "the pose is not Posed");
			}
			return;
		}
		if (s_startReq) {
			s_startReq = false;
			if (Send(s_low ? "BlindFireOverStart" : "BlindFireStart")) {
				++s_info.starts;
				s_info.tStartSent = a_now;
			}
		}
		if (s_endReq) {
			s_endReq = false;
			if (Send("BlindFireEnd")) {
				++s_info.ends;
				s_info.tEndSent = a_now;
			}
		}
		if (s_pendingShot && s_info.state == State::Linger && a_now >= s_tIntoOver) {
			s_pendingShot = false;
			Input::RequestMadeFire(s_dev, s_id, 0.15f);
			++s_info.madeShots;
			s_shotEnd = a_now + 0.25;
			COVER_DETAIL("BLIND FIRE: Into is over {:.3f} s after the press: the lost tap fires once (a fire press of the plugin's own for 0.15 s)", a_now - s_info.tPress);
		}
		if (s_info.state == State::Linger && !s_pendingShot && a_now - s_info.tRelease >= static_cast<double>(Config::Get().blindFireLinger) && a_now >= s_tIntoOver && a_now >= s_shotEnd) {
			s_info.state = State::Idle;
			if (Send("BlindFireEnd")) {
				++s_info.ends;
				s_info.tEndSent = a_now;
			}
		}
	}

	void Reset(const char* a_why)
	{
		std::lock_guard lock(s_mutex);
		ResetLocked(a_why);
	}
}
