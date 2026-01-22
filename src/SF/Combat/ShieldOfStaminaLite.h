#pragma once

namespace SF::Combat
{
	// Simplified "Shield of Stamina" behavior:
	// - On a blocked hit, health damage is paid from stamina first.
	// - If stamina is insufficient, the remaining damage stays on health.
	class ShieldOfStaminaLite
	{
	public:
		static void Install();
	};
}
