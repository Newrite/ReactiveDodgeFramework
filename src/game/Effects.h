#pragma once

namespace rdf::game::effects
{
	// Spend / give back the configured dodge cost resource (no-op when the dodge is free).
	void ChargeCost(RE::PlayerCharacter* player, float cost);
	void RefundCost(RE::PlayerCharacter* player, float cost);

	// Melee hit hook (ID 37673 + 0x3C0): while invulnerable, hits on the player are dropped.
	bool InstallIFrames();
	// Every projectile type. While invulnerable a projectile never records a hit on the player (AddImpact, vfunc
	// 189); single ones (arrow, missile, grenade) also drop their contact step (vfunc 190) and fly on, and once one
	// has passed through it ignores the player for the rest of its life. Beams, flames, cones and barriers only
	// skip the player while the i-frames last.
	void InstallProjectileIFrames();
	// Explosions: the per-actor line-of-sight check in Explosion::ApplyToTargets (42677 + 0x38C) says "blocked" for
	// an invulnerable player, which skips the explosion's damage, spell and knockdown.
	bool InstallExplosionIFrames();
	// Precision's pre-hit callback (API V4): its hits on an invulnerable player are ignored before
	// any hitstop or impact. Call once Precision is loaded (kPostPostLoad or later).
	void InstallPrecisionIFrames();
	// Main thread; read by the hit hook.
	void SetInvulnerable(bool invulnerable);
}
