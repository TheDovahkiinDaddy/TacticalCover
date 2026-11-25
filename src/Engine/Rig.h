#pragma once

// S0 test rig: a script the plugin runs by itself so that a test run needs no hands (PLAN.md rule 6, section 5 S0).
//
// [General] sTestScript=s2b starts, once a save has loaded, the game window is in the foreground and the player
// has stood still for 3 s: synthetic Space taps (the S2b jump rotation), then W held for 4 s, a 2 s pause, S held
// for 4 s (the S2b movement sequence triggers on its own). It ends with a "RESULT rig" line and "TEST COMPLETE".
//
// Synthetic keys go through SendInput with scan codes (the virtual-key code is filled in as well), only while
// this process owns the foreground window. Every key the rig holds is tracked and released on completion, on
// pause or menu, when the window loses the foreground, after 5 s at the latest, and on any fault.

namespace Engine::Frame
{
	struct Snapshot;
}

namespace Engine::Rig
{
	void OnTick(const Frame::Snapshot& a_snap);

	// ---- key and mouse plumbing for the scripts (S0 rig, g2 script) -------------------------------------------
	struct Key
	{
		std::uint16_t vk;
		std::uint16_t scan;
	};
	inline constexpr Key kKeyR{ 0x52, 0x13 };
	inline constexpr Key kKeyC{ 0x43, 0x2E };
	inline constexpr Key kKeyLCtrl{ 0xA2, 0x1D };
	inline constexpr Key kKeyW{ 0x57, 0x11 };
	inline constexpr Key kKeyA{ 0x41, 0x1E };
	inline constexpr Key kKeyS{ 0x53, 0x1F };
	inline constexpr Key kKeyD{ 0x44, 0x20 };
	inline constexpr Key kKeyE{ 0x45, 0x12 };
	inline constexpr Key kKeyGrave{ 0xC0, 0x29 };  // opens and closes the console
	inline constexpr Key kKeyEnter{ 0x0D, 0x1C };
	inline constexpr Key kKeyShift{ 0xA0, 0x2A };
	inline constexpr Key kKeyF11{ 0x7A, 0x57 };    // 360 Directional Movement's switch (VK_F11 = 122)
	inline constexpr Key kKeyX{ 0x58, 0x2D };      // Stride's shoulder swap (iShoulderKey 0x58)
	inline constexpr Key kKey1{ 0x31, 0x02 };
	inline constexpr Key kKey2{ 0x32, 0x03 };
	inline constexpr Key kKey3{ 0x33, 0x04 };
	inline constexpr Key kKey4{ 0x34, 0x05 };
	inline constexpr Key kKey5{ 0x35, 0x06 };
	inline constexpr Key kKey6{ 0x36, 0x07 };
	inline constexpr Key kKey7{ 0x37, 0x08 };
	inline constexpr Key kKey8{ 0x38, 0x09 };
	inline constexpr Key kKey9{ 0x39, 0x0A };
	inline constexpr Key kKey0{ 0x30, 0x0B };      // favourite slot 10
	inline constexpr Key kKeyMinus{ 0xBD, 0x0C };  // favourite slot 11
	inline constexpr Key kKeyEquals{ 0xBB, 0x0D }; // favourite slot 12
	inline constexpr Key kKeyNum5{ 0x65, 0x4C };  // Numpad 5: the dedicated cover key of iCoverTrigger 0 (no vanilla binding)
	inline constexpr Key kKeyNum9{ 0x69, 0x49 };  // Numpad 9: re-reads the INI

	// US-layout key (and whether Shift is needed) for one typed character; false for characters the table lacks.
	[[nodiscard]] bool KeyForChar(char a_char, Key& a_key, bool& a_shift);

	// Press now, release after a_seconds (released by the rig's own tick). Only sent while the game owns the foreground window.
	void Tap(Key a_key, double a_seconds = 0.08);
	// Right mouse button down or up (aim). Tracked like a held key: released on any abort.
	void MouseRight(bool a_down);
	void MouseLeft(bool a_down);
	// A relative mouse movement (dx, dy counts), the way the mouse turns the camera. Only while the game window is in front.
	void MouseMove(int a_dx, int a_dy);
	// Releases everything the rig holds. Safe to call at any time.
	void ReleaseEverything(const char* a_why);
	[[nodiscard]] bool InForeground();

	// True once a script has finished or aborted.
	[[nodiscard]] bool Finished() noexcept;
}
