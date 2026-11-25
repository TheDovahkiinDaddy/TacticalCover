#include "Core/CoverStateMachine.h"

namespace Core
{
	const char* ToString(const CoverState a_state) noexcept
	{
		switch (a_state) {
		case CoverState::None: return "None";
		case CoverState::Entering: return "Entering";
		case CoverState::InCover: return "InCover";
		case CoverState::Peeking: return "Peeking";
		case CoverState::Exiting: return "Exiting";
		}
		return "?";
	}

	const char* ToString(const PeekKind a_kind) noexcept
	{
		switch (a_kind) {
		case PeekKind::None: return "none";
		case PeekKind::Left: return "left";
		case PeekKind::Right: return "right";
		case PeekKind::Over: return "over";
		}
		return "?";
	}

	const char* ToString(const ExitReason a_reason) noexcept
	{
		switch (a_reason) {
		case ExitReason::None: return "none";
		case ExitReason::Trigger: return "the cover trigger";
		case ExitReason::HoldAway: return "the input held away from the wall";
		case ExitReason::Jump: return "jump";
		case ExitReason::Sprint: return "sprint";
		case ExitReason::Lost: return "the cover was lost";
		case ExitReason::Displaced: return "displaced from the cover line";
		}
		return "?";
	}

	const char* ToString(const MachineEvent a_event) noexcept
	{
		switch (a_event) {
		case MachineEvent::None: return "none";
		case MachineEvent::Reached: return "reached the stand point";
		case MachineEvent::EnterBlocked: return "the path to the stand point is blocked";
		case MachineEvent::PeekStarted: return "peek started";
		case MachineEvent::PeekEnded: return "peek ended";
		case MachineEvent::ExitStarted: return "exit started";
		case MachineEvent::ExitDone: return "exit done";
		case MachineEvent::HardReset: return "hard reset";
		}
		return "?";
	}

	void CoverStateMachine::ToState(const CoverState a_state, const double a_now) noexcept
	{
		m_state = a_state;
		m_since = a_now;
		m_awaySince = -1.0;
		if (a_state != CoverState::Peeking) {
			m_peek = PeekKind::None;
		}
	}

	bool CoverStateMachine::RequestEnter(const double a_now, const CoverCandidate& a_candidate, const int a_stanceBefore)
	{
		if (m_state != CoverState::None || a_candidate.kind == CoverKind::None) {
			return false;
		}
		m_candidate = a_candidate;
		m_stanceBefore = a_stanceBefore;
		m_exit = ExitReason::None;
		ToState(CoverState::Entering, a_now);
		return true;
	}

	bool CoverStateMachine::RequestLeave(const double a_now, const ExitReason a_reason)
	{
		if (m_state != CoverState::Entering && m_state != CoverState::InCover && m_state != CoverState::Peeking) {
			return false;
		}
		m_exit = a_reason;
		ToState(CoverState::Exiting, a_now);
		return true;
	}

	bool CoverStateMachine::Reset() noexcept
	{
		const bool changed = m_state != CoverState::None;
		m_state = CoverState::None;
		m_peek = PeekKind::None;
		m_awaySince = -1.0;
		return changed;
	}

	MachineResult CoverStateMachine::Update(const MachineInputs& a_in)
	{
		MachineResult out;

		// any -> None
		if (m_state != CoverState::None && (a_in.worldReset || !a_in.allowed)) {
			out.event = MachineEvent::HardReset;
			out.why = a_in.worldReset ? "the world changed (load, cell change, teleport or death)" : "cover is not allowed now";
			Reset();
			return out;
		}

		switch (m_state) {
		case CoverState::None:
			break;

		case CoverState::Entering:
			if (a_in.pathBlocked) {
				out.event = MachineEvent::EnterBlocked;
				ToState(CoverState::None, a_in.now);
			} else if (a_in.reachedStand) {
				out.event = MachineEvent::Reached;
				ToState(CoverState::InCover, a_in.now);
			} else if (a_in.now - m_since >= static_cast<double>(m_cfg.enterMaxTime) - 1e-6) {
				out.event = MachineEvent::Reached;
				out.snap = true;
				ToState(CoverState::InCover, a_in.now);
			}
			break;

		case CoverState::InCover:
		case CoverState::Peeking: {
			// leaving
			ExitReason reason = ExitReason::None;
			if (a_in.lostCount >= m_cfg.lostLeaveCount) {
				reason = ExitReason::Lost;
			} else if (a_in.shoved || a_in.deviation > m_cfg.maxDeviation) {
				reason = ExitReason::Displaced;
			} else if (a_in.awayInput >= m_cfg.exitInputThreshold) {
				if (m_awaySince < 0.0) {
					m_awaySince = a_in.now;
				}
				if (a_in.now - m_awaySince >= static_cast<double>(m_cfg.exitHoldTime) - 1e-6) {
					reason = ExitReason::HoldAway;
				}
			} else {
				m_awaySince = -1.0;
			}
			if (reason != ExitReason::None) {
				m_exit = reason;
				out.event = MachineEvent::ExitStarted;
				out.exitReason = reason;
				ToState(CoverState::Exiting, a_in.now);
				break;
			}

			// peeking
			if (m_state == CoverState::InCover) {
				if (a_in.aimHeld && a_in.peekSpot != PeekKind::None) {
					m_state = CoverState::Peeking;
					m_peek = a_in.peekSpot;
					m_since = a_in.now;
					out.event = MachineEvent::PeekStarted;
					out.peek = a_in.peekSpot;
				}
			} else if (!a_in.aimHeld || a_in.peekSpot == PeekKind::None) {
				out.event = MachineEvent::PeekEnded;
				out.peek = m_peek;
				m_state = CoverState::InCover;
				m_peek = PeekKind::None;
				m_since = a_in.now;
			}
			break;
		}

		case CoverState::Exiting:
			if (a_in.now - m_since >= static_cast<double>(m_cfg.exitTime) - 1e-6) {
				out.event = MachineEvent::ExitDone;
				ToState(CoverState::None, a_in.now);
			}
			break;
		}
		return out;
	}
}
