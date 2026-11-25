#include "Core/CoverDecision.h"

namespace Core
{
	PressDecision DecideSneakPress(const PressContext& a_ctx) noexcept
	{
		PressDecision d;
		if (a_ctx.mode != 1 && a_ctx.mode != 2 && a_ctx.mode != 3) {
			d.why = "the trigger mode is not 1, 2 or 3: the Sneak control stays vanilla";
			return d;
		}

		switch (a_ctx.state) {
		case CoverState::Entering:
		case CoverState::Exiting:
			d.swallow = true;
			d.why = "the cover is changing: the press is ignored";
			return d;

		case CoverState::InCover:
		case CoverState::Peeking:
			d.action = PressAction::Leave;
			d.wantedStance = a_ctx.stanceBefore;
			// M1: the game toggles sneak for the gamepad's B on release and only for a press held under about 0.4 s, so the stance change never rides on the real
			// press: it is always swallowed and the owner restores the stance held before with a press of its own
			d.swallow = true;
			d.why = "leave: the press is swallowed, the stance held before is restored by a press made by the plugin";
			return d;

		case CoverState::None:
			break;
		}

		if (a_ctx.mode == 2) {
			d.why = "mode 2: a press is vanilla; holding the control takes cover";
			return d;
		}
		if (!a_ctx.allowed) {
			d.why = "cover is not allowed now: vanilla sneak";
			return d;
		}
		if (a_ctx.stance < 0) {
			d.why = "the stance cannot be read: vanilla sneak";
			return d;
		}
		if (a_ctx.moveMagnitude < a_ctx.stickMin) {
			d.why = "the movement input is not pushed toward anything: vanilla sneak";
			return d;
		}
		if (!a_ctx.candidateFresh) {
			d.why = "no recent answer from the sensor for this direction: vanilla sneak";
			return d;
		}
		if (a_ctx.candidateKind == CoverKind::None) {
			d.why = "the sensor found no cover in this direction: vanilla sneak";
			return d;
		}
		d.action = PressAction::Enter;
		d.wantedStance = a_ctx.candidateKind == CoverKind::Low ? 1 : 0;
		d.swallow = true;
		d.why = "enter: the press is swallowed, the stance the cover needs is made by a press made by the plugin";
		return d;
	}

	PressDecision DecideCoverKeyPress(const PressContext& a_ctx) noexcept
	{
		PressDecision d;
		if (a_ctx.mode != 0) {
			d.why = "the trigger mode is not 0: the dedicated key does nothing";
			return d;
		}
		switch (a_ctx.state) {
		case CoverState::Entering:
		case CoverState::Exiting:
			d.why = "the cover is changing: the key is ignored";
			return d;
		case CoverState::InCover:
		case CoverState::Peeking:
			d.action = PressAction::Leave;
			d.wantedStance = a_ctx.stanceBefore;
			d.why = "leave: the stance before the cover is restored by a press made by the plugin";
			return d;
		case CoverState::None:
			break;
		}
		if (!a_ctx.allowed) {
			d.why = "cover is not allowed now";
			return d;
		}
		if (a_ctx.stance < 0) {
			d.why = "the stance cannot be read";
			return d;
		}
		if (!a_ctx.candidateFresh || a_ctx.candidateKind == CoverKind::None) {
			d.why = "no recent cover in the direction of the view";
			return d;
		}
		d.action = PressAction::Enter;
		d.wantedStance = a_ctx.candidateKind == CoverKind::Low ? 1 : 0;
		d.why = "enter: the stance is set afterwards by a press made by the plugin";
		return d;
	}
}
