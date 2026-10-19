#pragma once

namespace rdf::game::effects
{
	// Spend / give back the configured dodge cost resource (no-op when the dodge is free).
	void ChargeCost(RE::PlayerCharacter* player, float cost);
	void RefundCost(RE::PlayerCharacter* player, float cost);

	// Melee hit hook (ID 37673 + 0x3C0): while invulnerable, hits on the player are dropped.
	bool InstallIFrames();
	// Arrow and missile projectiles (shared Projectile vfunc 190, which turns contacts into hits):
	// while invulnerable, a projectile touching the player flies on instead of hitting.
	void InstallProjectileIFrames();
	// Main thread; read by the hit hook.
	void SetInvulnerable(bool invulnerable);
}
