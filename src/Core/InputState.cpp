#include "Core/InputState.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace Core
{
	Action ClassifyUserEvent(const std::string_view a_name) noexcept
	{
		if (a_name == "Forward") return Action::MoveForward;
		if (a_name == "Back") return Action::MoveBack;
		if (a_name == "StrafeLeft") return Action::MoveLeft;
		if (a_name == "StrafeRight") return Action::MoveRight;
		if (a_name == "Sprint") return Action::Sprint;
		if (a_name == "Jump") return Action::Jump;
		if (a_name == "SecondaryAttack") return Action::Aim;  // assumption: confirmed or corrected by the first test run
		if (a_name == "PrimaryAttack") return Action::Fire;
		if (a_name == "Sneak" || a_name == "Crouch") return Action::Sneak;
		return Action::None;
	}

	std::string_view ActionName(const Action a_action) noexcept
	{
		switch (a_action) {
		case Action::MoveForward: return "Forward";
		case Action::MoveBack: return "Back";
		case Action::MoveLeft: return "Left";
		case Action::MoveRight: return "Right";
		case Action::Sprint: return "Sprint";
		case Action::Jump: return "Jump";
		case Action::Aim: return "Aim";
		case Action::Fire: return "Fire";
		case Action::Sneak: return "Sneak";
		case Action::None: break;
		}
		return "None";
	}

	bool IsMovementAction(const Action a_action) noexcept
	{
		return a_action == Action::MoveForward || a_action == Action::MoveBack || a_action == Action::MoveLeft || a_action == Action::MoveRight;
	}

	void InputState::ApplyAction(const Action a_action, const bool a_down) noexcept
	{
		switch (a_action) {
		case Action::MoveForward: m_forward = a_down; break;
		case Action::MoveBack: m_back = a_down; break;
		case Action::MoveLeft: m_left = a_down; break;
		case Action::MoveRight: m_right = a_down; break;
		case Action::Sprint: m_sprint = a_down; break;
		case Action::Jump:
			if (a_down && !m_jump) {
				m_jumpEdge = true;
			}
			m_jump = a_down;
			break;
		case Action::Aim: m_aim = a_down; break;
		case Action::Fire: m_fire = a_down; break;
		case Action::Sneak: m_sneak = a_down; break;
		case Action::None: break;
		}
	}

	void InputState::ApplyCoverKey(const bool a_down) noexcept
	{
		if (a_down && !m_cover) {
			m_coverEdge = true;
		}
		m_cover = a_down;
	}

	void InputState::SetStick(const float a_x, const float a_y) noexcept
	{
		m_stickX = std::clamp(a_x, -1.0f, 1.0f);
		m_stickY = std::clamp(a_y, -1.0f, 1.0f);
	}

	void InputState::SetStickAt(const float a_x, const float a_y, const double a_now) noexcept
	{
		SetStick(a_x, a_y);
		m_stickTime = a_now;
	}

	void InputState::ExpireStick(const double a_now, const double a_maxAge) noexcept
	{
		if ((m_stickX != 0.0f || m_stickY != 0.0f) && a_now - m_stickTime > a_maxAge) {
			m_stickX = 0.0f;
			m_stickY = 0.0f;
		}
	}

	namespace
	{
		struct Vec2
		{
			float x;
			float y;
		};

		// Keys and stick summed, then scaled onto the unit disc if longer than 1.
		Vec2 Combine(const float a_stickX, const float a_stickY, const bool a_forward, const bool a_back, const bool a_left, const bool a_right) noexcept
		{
			const float x = a_stickX + (a_right ? 1.0f : 0.0f) - (a_left ? 1.0f : 0.0f);
			const float y = a_stickY + (a_forward ? 1.0f : 0.0f) - (a_back ? 1.0f : 0.0f);
			const float len = std::sqrt(x * x + y * y);
			return len > 1.0f ? Vec2{ x / len, y / len } : Vec2{ x, y };
		}
	}

	float InputState::MoveX() const noexcept
	{
		return Combine(m_stickX, m_stickY, m_forward, m_back, m_left, m_right).x;
	}

	float InputState::MoveY() const noexcept
	{
		return Combine(m_stickX, m_stickY, m_forward, m_back, m_left, m_right).y;
	}

	float InputState::MoveMagnitude() const noexcept
	{
		const float x = MoveX();
		const float y = MoveY();
		return std::sqrt(x * x + y * y);
	}

	bool InputState::ConsumeJumpPressed() noexcept
	{
		return std::exchange(m_jumpEdge, false);
	}

	bool InputState::ConsumeCoverPressed() noexcept
	{
		return std::exchange(m_coverEdge, false);
	}

	std::string InputState::Describe() const
	{
		std::string out;
		const auto  add = [&](const bool a_on, const char* a_name) {
            if (a_on) {
                if (!out.empty()) {
                    out += '+';
                }
                out += a_name;
            }
		};
		add(m_forward, "Forward");
		add(m_back, "Back");
		add(m_left, "Left");
		add(m_right, "Right");
		add(m_sprint, "Sprint");
		add(m_jump, "Jump");
		add(m_aim, "Aim");
		add(m_fire, "Fire");
		add(m_sneak, "Sneak");
		add(m_cover, "Cover");
		return out.empty() ? "-" : out;
	}
}
