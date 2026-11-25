#pragma once

// W7 (REFERENCE 23.25): the engine's "an equip animation is owed". After an equip the player's process data keeps a byte at middleHigh + 0x586 (1: the Equip event is owed, 2: another action); the
// player's update pays it about 0.3 s later for the Orion (ID 101325 -> 101326, the sender of the Equip event stack of 23.24; build\dis\equipsend_101325.txt, equipgate_104383.txt, equipqueue_102324.txt).
//   [Cover] iEquipFieldProbe 1 (rig only): every change of the fields around it is logged (EQUIPFIELDS: the bytes middleHigh + 0x584, 0x585, 0x586, the dwords + 0x2D0, 0x2F0, 0x3B0, 0x3D0, the bytes
//     of the high data + 0x566, 0x57E, 0x583, with bIsInCover, the weapon state, bIsEquipping, the pose state and the weapon key), at the tick rate of the frame tick.
//   [Cover] iPoseEquipPending 1: while the pose is on (Switching, Entering, Posed) a byte at middleHigh + 0x586 that reads 1 is written 0 (2 is left alone); one log line per write.
// Reads and the one write are in __try; a fault turns both off for the session. Nothing happens when both switches are 0 (the play state).

#include <cstdint>

namespace Engine::EquipPending
{
	struct Fields
	{
		bool          ok{ false };
		std::uint8_t  b584{ 0 }, b585{ 0 }, b586{ 0 };
		std::uint32_t d2D0{ 0 }, d2F0{ 0 }, d3B0{ 0 }, d3D0{ 0 };
		std::uint8_t  h566{ 0 }, h57E{ 0 }, h583{ 0 };
	};
	[[nodiscard]] Fields Read();

	struct Info
	{
		int  writes{ 0 };
		int  readsOne{ 0 };
		bool faulted{ false };
	};
	[[nodiscard]] Info GetInfo();

	// every frame tick, unthrottled
	void OnTick(double a_now);
}
