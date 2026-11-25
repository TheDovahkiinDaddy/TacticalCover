#pragma once

// Spike 23.13 (REFERENCE 23.13): the reload sound in the cover pose.
//
// Why it is silent: the player's own reload clip carries `<audio><foley>` annotations (the weapon's Wwise events: for the Grendel
// WPNHandRifleGrendelReload1MagOut at frame 1, 2MagIn at 45, 3BoltClose at 86, each with a _WwiseMotion twin); the engine posts
// them through AK::SoundEngine::PostEvent when the clip passes the frame. The cover reload clips (covercrouching_reload,
// coverstanding_reload) carry no `<audio>` entries, so the engine posts nothing: measured with the probe below (a plain reload:
// six posts on game object 2; the same reload in the pose: none).
//
// The probe: the five places that call PostEvent (by id, by name) are hooked as call sites, every call is forwarded unchanged and
// logged (iAudioProbe 1 while the rig arms it, 2 always, rate limited). Off by default: nothing is hooked.
//
// The cure (iPoseReloadSound 1, off by default): the sequence the engine posts during an ordinary reload (event ids, times) is
// learned per weapon; when a reload starts in the pose the same events are posted on the same game object at the same fractions of
// the reload, only while the reload runs and the pose holds.

namespace Engine::AudioProbe
{
	struct Info
	{
		bool          installed{ false };
		int           sites{ 0 };
		std::uint64_t calls{ 0 };       // calls seen since the hook went in
		std::uint64_t logged{ 0 };
		int           learned{ 0 };     // weapons whose reload sequence is known
		int           replays{ 0 };     // pose reloads that were voiced
		int           posted{ 0 };      // events the plugin posted
		int           cancelled{ 0 };   // replays cut short
		int           ended{ 0 };       // end-of-event callbacks for the plugin's own posts
		bool          faulted{ false };
		std::uint32_t equippedKey{ 0 };   // the weapon of the last equip event
		int           equipEvents{ 0 };
	};

	void Tick();                    // installs once, from the frame tick, when iAudioProbe or iPoseReloadSound is not 0
	void OnTick(double a_now);      // learn and replay (iPoseReloadSound)
	void Arm(bool a_on, const char* a_label);
	// probe (rig only): looks through the player's process data for pointers to weapon forms and logs where they are (spike 23.14, the weapon key)
	void ScanWeapon(const char* a_label);
	[[nodiscard]] std::uint32_t CurrentWeapon();   // the weapon in the hands (process data), else the last equip event, else 0
	void FlushCache();              // writes the cache file now (the scoped weapons of ScopeAim are in it)
	void DebugForget();             // rig only: clear the memory and read the file again, as a fresh start does
	[[nodiscard]] Info GetInfo() noexcept;
}
