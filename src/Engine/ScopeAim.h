#pragma once

// W4 (REFERENCE 23.22): the aim is locked in the cover pose for a weapon with a scope. The engine's scope view takes the camera out of third person and the engine itself leaves the cover when
// that happens (23.21), and in low cover the scope looks into the wall; nothing the mod does may take the character out of cover, so while the pose is on and the weapon in the hands is
// scoped the aim control does not reach the game and the plugin starts no aim of its own. [Cover] iPoseScopeAim 2 (0 = off). Hook side: OnAim; tick side: Tick.
// How a weapon is known to have a scope (iScopeSource, bit 1 and bit 2): the graph variable bHasScope read by name while the weapon is in the hands (cached per weapon key, because the
// pose's own graph may not have the variable), and learning in memory: the camera leaving third person while the aim control is held marks the weapon key as scoped for the session.

#include <cstdint>
#include <vector>

namespace Engine::ScopeAim
{
	struct Plan
	{
		bool swallow{ false };
	};

	struct Info
	{
		std::uint32_t key{ 0 };
		bool          scoped{ false };
		bool          byVar{ false };
		bool          byLearn{ false };
		bool          varReadable{ false };
		double        hasScope{ -1.0 };
		double        inScopes{ -1.0 };
		int           locks{ 0 };      // aim presses held back
		int           notices{ 0 };    // HUD lines asked for
		int           learned{ 0 };
		int           ends{ 0 };       // aims ended at a switch to a scoped weapon
	};
	[[nodiscard]] Info GetInfo();

	// the pose is on (Switching, Entering, Posed) and the weapon in the hands is scoped, iPoseScopeAim 2
	[[nodiscard]] bool Locked();

	// the aim control: a press while locked is held back to its release; a press that began before the lock is not touched (its release must reach the game)
	[[nodiscard]] Plan OnAim(bool a_down, bool a_pressEdge);

	// a_third: the camera is third person; a_aimHeld: the aim control is held (the hook's reading)
	void Tick(double a_now, bool a_third, bool a_aimHeld);

	// true once per pose after a press was held back: the caller shows the HUD line
	[[nodiscard]] bool TakeNotice();

	// the rig: forget what was learned and cached
	void Forget();
	// the rig: forget only what was learned (the bHasScope cache stays)
	void ForgetLearned();
	// the rig: mark a weapon key as scoped as if the camera had left third person under an aim in the open (the play of an earlier hour)
	void LearnForRig(std::uint32_t a_key, bool a_persist = false);
	// the scoped weapons kept between sessions (23.23): the cache file CoverSF.reloadsound.txt has a line "s <form id>" per weapon; AudioProbe reads and writes it
	void LoadPersisted(std::uint32_t a_key);
	[[nodiscard]] std::vector<std::uint32_t> PersistedKeys();
	// the rig: the cached or learned answer for a weapon key
	[[nodiscard]] bool KnownScoped(std::uint32_t a_key);
}
