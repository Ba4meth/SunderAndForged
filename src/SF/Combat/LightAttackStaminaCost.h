#pragma once

namespace SF::Combat
{
	// Adds stamina cost to *normal/light* attacks (vanilla light attacks cost 0 stamina).
	//
	// Formula:
	//  - Unarmed: Base
	//  - Weapon : Base + (weaponWeight * WeightMult)
	//
	// The final cost is additionally passed through the vanilla perk entry point
	// BGSEntryPoint::kModPowerAttackStamina so that perks (and mods like
	// "Perk Entry Point Extender") that already affect power attack stamina cost
	// can also affect light attacks.
	class LightAttackStaminaCost
	{
	public:
		static void Install();
	};
}
