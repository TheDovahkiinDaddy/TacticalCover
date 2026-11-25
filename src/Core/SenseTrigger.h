#pragma once

// Sense on stop (PLAN.md Phase 2 follow-up, 2026-11-17): decides when the plugin should look for cover by itself, so that the test card
// needs no key. Engine-free and unit-tested. One query per trigger; the caller says whether one is already running.
//
//   Stopped: the player has stood still for fStillSeconds after having moved at least fMinMove since the last sense.
//   Turned:  the view has turned more than fTurnDeg since the last sense and has been steady for fStillSeconds.
//
// "Still" means the position did not change by more than kMoveEps since the still period began; "steady" means the yaw did not
// change by more than kYawEps. No trigger comes sooner than fMinInterval after the last sense (a limit on how often a player who
// looks around can cause a sense).

#include "Core/Geometry.h"

namespace Core
{
	struct SenseTriggerSettings
	{
		float stillSeconds{ 0.4f };
		float minMove{ 0.5f };       // metres since the last sense for the Stopped trigger
		float turnDeg{ 30.0f };      // degrees since the last sense for the Turned trigger
		float minInterval{ 1.0f };   // seconds between two senses
		float moveEps{ 0.02f };      // metres: a position change above this is movement
		float yawEps{ 1.0f };        // degrees: a yaw change above this is turning
	};

	enum class SenseReason
	{
		None,
		Stopped,
		Turned,
	};
	[[nodiscard]] const char* ToString(SenseReason a_reason) noexcept;

	class SenseTrigger
	{
	public:
		explicit SenseTrigger(const SenseTriggerSettings& a_settings = {}) :
			m_cfg(a_settings) {}

		void SetSettings(const SenseTriggerSettings& a_settings) { m_cfg = a_settings; }

		// The game is not in a state to sense (not third person, paused, no player): forget everything, so that the first valid frame
		// afterwards starts from the present position and view and triggers nothing.
		void Reset() noexcept { m_started = false; }

		// Call every valid frame. a_busy: a query is running (a trigger waits, it is not lost). Returns why a sense should start now.
		// The caller then starts it and calls Sensed().
		[[nodiscard]] SenseReason Update(double a_now, Vec2 a_position, float a_yawRadians, bool a_busy);

		// A query started now (by a trigger or by the key): the "since the last sense" references move here.
		void Sensed(double a_now, Vec2 a_position, float a_yawRadians) noexcept;

	private:
		SenseTriggerSettings m_cfg;
		bool                 m_started{ false };
		Vec2                 m_stillAnchor{};
		double               m_stillSince{ 0.0 };
		float                m_yawAnchor{ 0.0f };
		double               m_yawSince{ 0.0 };
		Vec2                 m_lastPos{};
		float                m_lastYaw{ 0.0f };
		double               m_lastTime{ -1e9 };
	};
}
