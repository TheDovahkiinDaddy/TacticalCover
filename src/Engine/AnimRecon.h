#pragma once

// S6: animation reconnaissance, driven from the plugin instead of the console (the console cannot be read by the
// plugin, the plugin can read the graph variables). Sends the cover events of cover.agx to the player's graph and
// logs the cover-related graph variables before and after; inspects where the keyword that gates the player's graph
// (AnimArchetypePlayer, found by tools/esm_subgraphs.py) lives; and, as S6c, removes it from the player's actor base
// for a few seconds and re-draws the weapon to see whether the graph changes. Everything is guarded, nothing is
// saved by the plugin, and the removal is undone at the end of the stage. [AnimRecon] bAnimRecon=0 keeps it out.

namespace Engine::AnimRecon
{
	// The cover-related graph variables of cover.agx and gunactions.agx.
	[[nodiscard]] std::span<const char* const> CoverVars();

	// "label: name=value ..." with unresolved names after it. One log line.
	void LogCoverVars(const char* a_label);

	// Sends a_event to the player's graph through IAnimationGraphManagerHolder::NotifyAnimationGraphImpl. a_accepted is
	// the function's return value. Returns false when the call faulted.
	[[nodiscard]] bool SendEvent(const char* a_event, bool& a_accepted);

	// Where does AnimArchetypePlayer (0x6B508 in Starfield.esm) live? Logs HasKeyword on the player reference, on the
	// actor base and on the race. Returns a one-line summary for the RESULT line.
	[[nodiscard]] std::string InspectArchetypeKeyword();

	// S6c: remove the keyword from the player's actor base / put it back. Return true when the array changed.
	[[nodiscard]] bool RemoveArchetypeFromBase();
	[[nodiscard]] bool RestoreArchetypeOnBase();

	// ActorUtils::ChangeAnimArchetype(player, keyword) through our own Address Library ID (Engine/Offsets.h). a_keywordFormId 0 passes
	// a null keyword (clear the archetype). a_returned is the function's result. Returns false when the function is refused (its
	// ID does not resolve to the verified RVA) or faulted. The first successful call with a keyword other than AnimArchetypePlayer
	// marks the player as changed; ArchetypeChanged() says whether it must be restored.
	[[nodiscard]] bool ChangeArchetype(RE::TESFormID a_keywordFormId, bool& a_returned);
	[[nodiscard]] bool ArchetypeChanged() noexcept;
	// Restores AnimArchetypePlayer (0006B508). True when the call ran (a_returned = its result).
	[[nodiscard]] bool RestoreArchetype(bool& a_returned);

	// Actor::DrawWeaponMagicHands(a_draw), guarded.
	[[nodiscard]] bool DrawWeapon(bool a_draw);

	// Starting from the wrapper of the same name in the player's graph: the position the player is at, for before/after.
	[[nodiscard]] RE::NiPoint3 PlayerPosition();
}
