#include "Core/SwapChord.h"

namespace Core
{
	void SwapChord::OnTrigger(const float a_value) noexcept
	{
		m_triggerHeld = m_cfg.enabled && a_value >= m_cfg.triggerThreshold;
	}

	ChordVerdict SwapChord::OnAButton(const bool a_down, const bool a_pressEdge, const double a_now) noexcept
	{
		if (!m_cfg.enabled) {
			m_captured = false;
			return ChordVerdict::Pass;
		}
		if (a_pressEdge) {
			if (!m_triggerHeld) {
				m_captured = false;
				return ChordVerdict::Pass;
			}
			m_captured = true;
			if (a_now - m_lastKeySwap < m_cfg.dedupeWindow) {
				return ChordVerdict::Swallow;  // the keyboard path swapped for this very press already
			}
			m_lastPadSwap = a_now;
			return ChordVerdict::SwapAndSwallow;
		}
		if (!m_captured) {
			return ChordVerdict::Pass;
		}
		if (!a_down) {
			m_captured = false;  // the release of a chord press: swallowed, and the chord is over
		}
		return ChordVerdict::Swallow;
	}

	bool SwapChord::AllowKeySwap(const double a_now) noexcept
	{
		if (m_cfg.enabled && a_now - m_lastPadSwap < m_cfg.dedupeWindow) {
			return false;
		}
		m_lastKeySwap = a_now;
		return true;
	}

	void SwapChord::Reset() noexcept
	{
		m_triggerHeld = false;
		m_captured = false;
		m_lastPadSwap = -1.0e9;
		m_lastKeySwap = -1.0e9;
	}
}
