#pragma once

// W5 (REFERENCE 23.23): every animation event that enters the player's graph. A hook on slot 1 of the vtable of the player's IAnimationGraphManagerHolder base (PlayerCharacter, sub-object
// +0x60, Address Library ID 452463, slot 1 = NotifyAnimationGraphImpl, RVA 0xF09F60 on 1.16.244; the plugin's own sends go through the same slot). Two uses, each behind its own switch:
//   [Cover] iGraphEventProbe 1: a ring of the last second of events (time, name, what the graph answered, own or the engine's); at every weapon key change the ring and the next 1.5 s are written
//     to the log, nothing else is logged (REFERENCE 23.23, the observation).
//   [Cover] iPoseEquipBlock 1 with sPoseEquipBlockEvents: while the pose is Posed an event of that list is not passed to the graph (the hook returns false: "not accepted", which is what the
//     graph answers for an event it has no transition for, so every caller has a path for it).
// Nothing is installed when both are 0 (the play state of m4b8).

#include <cstdint>

namespace Engine::GraphEvents
{
	struct Info
	{
		bool          installed{ false };
		bool          faulted{ false };
		std::uint32_t seen{ 0 };
		std::uint32_t blocked{ 0 };
		std::uint32_t windows{ 0 };
	};
	[[nodiscard]] Info GetInfo();

	// From the cover tick, every tick. Installs the hook when a switch asks for it; a_posed: the pose is Posed.
	void Tick(double a_now, bool a_posed);

	// The plugin's own sends are tagged (AnimRecon::SendEvent).
	void SetOwn(bool a_own) noexcept;
}
