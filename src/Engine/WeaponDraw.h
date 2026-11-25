#pragma once

// Spike W2 (REFERENCE 23.19, 23.20): after an equip inside the cover pose the new weapon's model hangs on a holster node and no `WeaponDraw` mark is ever sent (no cover clip set has an
// equip clip). The mark's handler (WeaponDrawHandler, Address Library ID 103947) calls one function with the actor: Address Library ID 100439 (RVA 0x18D7050 on 1.16.244). While the pose
// is Posed and the weapon in the hands changed, this module waits until the `Weapon` node has lost its model (two children left) and iPoseWeaponDrawDelay has passed, then calls that
// function once with the player. Rig only: [Cover] iPoseWeaponDraw 0 (default, the play state) does nothing and installs nothing.

namespace Engine::WeaponDraw
{
	struct Info
	{
		bool available{ false };   // the function was found and its first bytes are the verified ones
		bool faulted{ false };
		int  calls{ 0 };
		int  giveUps{ 0 };
	};

	// Called from the cover tick every tick. a_posed: the pose is Posed.
	void Tick(double a_now, bool a_posed);

	[[nodiscard]] Info GetInfo();

	// W4: the full state line (the draw snapshot) with a label, for the rig
	void LogState(const char* a_label);

	// W5: an Equip event was kept from the player's graph while the pose is Posed (GraphEvents). If the engine had set the weapon state to 2 for it, the draw work brings it to 3 (the ordinary job
	// of the handler of the WeaponDraw mark): looked at 0.05 s later, at most three calls 0.15 s apart, one log line each
	void NoteEquipBlocked(double a_now);
}
