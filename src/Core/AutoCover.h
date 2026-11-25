#pragma once

// M1b, iCoverTrigger 3: taking cover without a press. A pure decision (engine-free, tested): the owner calls Update() every tick of the cover logic with what is
// known, and it says "enter now" when ALL of the following has held without a break for fAutoCoverTime:
//   the state is None, cover is allowed, no sprint, the movement input is at least fCoverStickMin, a fresh Low or High candidate for the direction of the input,
//   the input points into the cover (the angle between it and the direction to the wall is at most fAutoCoverAngleDeg), the stand point is at most
//   fAutoCoverDistance away, and for HIGH cover (iAutoCoverTall): 0 never by itself, 1 only with an open end within fAutoCoverTallEdge (a wall in the middle of a corridor does
//   not grab the player; 0 for the edge distance = never), 2 anywhere, but only with the weapon drawn.
// The hold time (card M4b): with the weapon drawn fAutoCoverTimeDrawn (low and tall: the user wants cover at once; what is left is the freshness of the 10 Hz sensor answer),
// with the weapon away fAutoCoverTime (tall cover in mode 1 too).
// Re-arm: any state other than None switches the automatic entry off; it comes back once the "into cover" condition has been false for fAutoCoverRearm.

#include "Core/CoverSensor.h"
#include "Core/CoverStateMachine.h"

namespace Core
{
	struct AutoCoverSettings
	{
		float time{ 0.30f };       // fAutoCoverTime: the condition must hold this long
		float rearm{ 0.30f };      // fAutoCoverRearm: after an exit the condition must be false this long before the next automatic entry
		float angleDeg{ 35.0f };   // fAutoCoverAngleDeg
		float distance{ 0.60f };   // fAutoCoverDistance: the stand point at most this far (horizontal)
		float tallEdge{ 1.2f };    // fAutoCoverTallEdge: HIGH cover needs an open end this near (0 = never; mode 1)
		int   tallMode{ 1 };       // iAutoCoverTall: 0 tall cover never by itself, 1 only near an open end (tallEdge), 2 anywhere with the weapon drawn
		float timeTall{ 0.45f };   // fAutoCoverTimeTall: the hold time for HIGH cover (tall mode 2 only used it with the weapon drawn; since card M4b timeDrawn replaces it there)
		float timeDrawn{ 0.05f };  // fAutoCoverTimeDrawn (card M4b): the hold time, low and tall, while the weapon is drawn; the weapon away keeps time (low) as before
	};

	struct AutoCoverInput
	{
		double     now{ 0.0 };
		CoverState state{ CoverState::None };
		bool       allowed{ true };
		bool       sprinting{ false };
		float      moveMagnitude{ 0.0f };
		float      stickMin{ 0.4f };
		bool       candidateFresh{ false };
		CoverKind  kind{ CoverKind::None };
		float      angleDeg{ 180.0f };        // input direction against the direction to the wall
		float      standDistance{ 1e9f };     // horizontal, player to stand point
		float      openEndDistance{ 1e9f };   // from the foot to the nearest Open end (1e9: none)
		bool       weaponDrawn{ true };       // the weapon is drawn (tall cover mode 2)
	};

	struct AutoCoverResult
	{
		bool        enter{ false };
		bool        into{ false };      // the condition holds now
		float       heldFor{ 0.0f };    // enter: how long it held
		const char* why{ "" };          // static text for the log (why not "into")
	};

	// The angle between the movement direction (world, unit; (0,0) = no input: 180) and the direction from the player to the wall (the negated normal).
	[[nodiscard]] float AngleToWallDeg(Vec2 a_inputDir, Vec2 a_wallNormal) noexcept;

	// The distance from the foot to the nearest Open end of the candidate (1e9 when no end is open).
	[[nodiscard]] float NearestOpenEnd(const CoverCandidate& a_candidate) noexcept;

	class AutoCover
	{
	public:
		[[nodiscard]] AutoCoverResult Update(const AutoCoverSettings& a_settings, const AutoCoverInput& a_in) noexcept;
		void                          Reset() noexcept;
		[[nodiscard]] bool            Armed() const noexcept { return m_armed; }

	private:
		bool   m_armed{ true };
		double m_heldSince{ -1.0 };
		double m_falseSince{ -1.0 };
	};
}
