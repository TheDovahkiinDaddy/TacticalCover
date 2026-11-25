#pragma once

// T3.0: what a press of the game's Sneak control means (PLAN.md section 3.2, "The cover trigger"). A pure function: the input hook calls it
// in the same call that delivers the press, with what is already known (the cached candidate, the stance last read), and swallows or
// passes the press accordingly.
//
//   state None, cover allowed, the movement input pushed beyond fStickMin and a fresh Low or High candidate:
//       Enter. The press carries the stance change: Low cover wants sneaking (1), High standing (0); when the stance already fits the
//       press is swallowed, else it passes and the game toggles
//   state None, anything else: nothing, the press passes (vanilla sneak toggle)
//   state InCover or Peeking: Leave. The press restores the stance held before entering the same way (swallowed when it already fits)
//   state Entering or Exiting: nothing, swallowed (the cover is changing; a second toggle would undo the first)
//   trigger modes other than 1 (T3.5): nothing for now

#include "Core/CoverStateMachine.h"

namespace Core
{
	enum class PressAction
	{
		Nothing,
		Enter,
		Leave,
	};

	struct PressContext
	{
		int        mode{ 1 };                    // [General] iCoverTrigger: 1 = the Sneak control while pushing toward cover
		CoverState state{ CoverState::None };
		bool       allowed{ true };              // no "not allowed" condition now (third person, on the ground, not in a menu ...)
		float      moveMagnitude{ 0.0f };        // the movement input, 0..1 (keys count as 1)
		float      stickMin{ 0.4f };             // [General] fCoverStickMin
		bool       candidateFresh{ false };      // the cache holds an answer no older than fCandidateMaxAge that still points the way the input does
		CoverKind  candidateKind{ CoverKind::None };
		int        stance{ -1 };                 // iSyncActorStance: 0 standing, 1 sneaking, -1 unknown
		int        stanceBefore{ -1 };           // the stance when the cover began (-1 none)
	};

	struct PressDecision
	{
		PressAction action{ PressAction::Nothing };
		bool        swallow{ false };       // true: the game must not see this press
		int         wantedStance{ -1 };     // Enter: what the cover needs; Leave: the stance held before; -1 none
		const char* why{ "" };              // static text for the log
	};

	// Mode 1 as described above. Mode 2 (T3.5, the Sneak control held for fCoverHoldTime): the press itself stays vanilla in state None (the owner takes
	// cover after the hold time, see Engine/Cover), and in cover it leaves like mode 1. Mode 0: the Sneak control is never touched.
	[[nodiscard]] PressDecision DecideSneakPress(const PressContext& a_context) noexcept;

	// Mode 0 (T3.5): the dedicated cover key (user event Unmapped, nothing to swallow). Pressed with the cover allowed and a fresh Low or High candidate it
	// takes cover, no movement input needed; pressed in cover it leaves. The stance is set afterwards by a Sneak press made by the plugin (wantedStance).
	[[nodiscard]] PressDecision DecideCoverKeyPress(const PressContext& a_context) noexcept;
}
