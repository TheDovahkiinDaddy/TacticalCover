#pragma once

// The shoulder swap on the controller chord LT + A, read by the plugin itself (release 1.0.0, REFERENCE 24). Engine-free and unit-tested.
//
// The game hands the plugin every gamepad button event with its XInput mask as the idCode: the left trigger is 0x09 with an analogue value (REFERENCE 12.2), the A
// button 0x1000 (the logs of the user's plays: the user events 'Activate', 'Accept', 'QCAButton', 'SelectTarget' all carry 0x1000, one event per press). While
// the left trigger is held, a press of A swaps the shoulder, and that press, its repeats and its release are kept from the game (so nothing is activated: A is the
// game's Activate). An A press that began without the trigger passes untouched to its release, even if the trigger is pulled in between.
//
// The user's own Steam layout turns LT + A into the keyboard key X (the old binding, which stays): the game then sees no A at all. Should a layout ever send
// both, the two paths would swap twice and cancel out, so a swap from one path is not made within dedupeWindow seconds of a swap from the other.

namespace Core
{
	struct SwapChordSettings
	{
		bool   enabled{ false };         // bShoulderPadChord
		float  triggerThreshold{ 0.5f };  // fShoulderPadTrigger: the left trigger counts as held from this value on (0..1)
		double dedupeWindow{ 0.25 };      // seconds: a swap from the other path within this window is not made again
	};

	enum class ChordVerdict
	{
		Pass,           // the event goes to the game untouched
		Swallow,        // the event is kept from the game, nothing else happens
		SwapAndSwallow  // the shoulder swaps and the event is kept from the game
	};

	class SwapChord
	{
	public:
		void SetSettings(const SwapChordSettings& a_settings) noexcept { m_cfg = a_settings; }

		// The left trigger's event (value 0..1; the release comes as 0).
		void OnTrigger(float a_value) noexcept;

		// An A button event: down = value > 0, pressEdge = down with no time held yet.
		[[nodiscard]] ChordVerdict OnAButton(bool a_down, bool a_pressEdge, double a_now) noexcept;

		// The keyboard path (iShoulderKey): true when its swap may be made now (and then it is counted).
		[[nodiscard]] bool AllowKeySwap(double a_now) noexcept;

		// A load, a fault, the feature switched off: forget everything held.
		void Reset() noexcept;

		[[nodiscard]] bool TriggerHeld() const noexcept { return m_triggerHeld; }
		[[nodiscard]] bool Captured() const noexcept { return m_captured; }

	private:
		SwapChordSettings m_cfg{};
		bool              m_triggerHeld{ false };
		bool              m_captured{ false };  // the current A press belongs to the chord: its repeats and its release are swallowed
		double            m_lastPadSwap{ -1.0e9 };
		double            m_lastKeySwap{ -1.0e9 };
	};
}
