#pragma once

#include <optional>

namespace rdf::game::effects
{
	// Spend / give back `amount` of a dodge's cost resource.
	void ChargeCost(RE::Actor* actor, RE::ActorValue resource, float amount);
	void RefundCost(RE::Actor* actor, RE::ActorValue resource, float amount);
	// The HUD meter of a value flashes (51907 = 0x1408D53D0, the engine's "not enough" flash; the player's HUD).
	void FlashMeter(RE::ActorValue value);
	// The engine's AIProcess::SetRegenerationDelay (38526 = 0x14065A940, IDA): only lengthens the actor's delay for Health
	// / Magicka / Stamina, capped at fHealth / fMagicka / fStaminaRegenDelayMax. Needs a high process.
	void DelayRegen(RE::Actor* actor, RE::ActorValue value, float seconds);

	// Melee hit hook (ID 37673 + 0x3C0): while invulnerable, hits on the player are dropped.
	bool InstallIFrames();
	// Every projectile type. Ray candidates are rejected before spell/impact feedback;
	// while invulnerable a projectile never records a hit on the actor (AddImpact, vfunc
	// 189); single ones (arrow, missile, grenade) also drop their contact step (vfunc 190) and fly on, and once one
	// has passed through it ignores the actor for the rest of its life (also cones).
	// Beams, flames and barriers only skip the actor while the i-frames last.
	// Stream HandleHits filters use transient protection rather than remembering.
	void InstallProjectileIFrames();
	// Explosions: the per-actor line-of-sight check in Explosion::ApplyToTargets (42677 + 0x38C) says "blocked" for
	// an invulnerable actor, which skips the explosion's damage, spell and knockdown.
	// Also filters MagicHit's separate per-target hostile/detrimental area delivery,
	// before AddTarget and its independent callbacks. Beneficial effects are preserved.
	bool InstallExplosionIFrames();
	// Precision's pre-hit callback (API V4): its hits on an invulnerable player are ignored before
	// any hitstop or impact. Call once Precision is loaded (kPostPostLoad or later).
	void InstallPrecisionIFrames();
	// I-frames are per actor (player or NPC): the hooks drop hits on an actor while it is marked invulnerable.
	// Main thread; read by the hooks on any thread.
	void SetInvulnerable(RE::Actor* actor, bool invulnerable);
	void ClearInvulnerable();  // every actor (load / reset)
	// The actor's perfect-dodge window is open now (main thread, every tick, beside SetInvulnerable): what a perfect-only
	// attacker's hit needs to be dodged.
	void SetPerfectOpen(RE::Actor* actor, bool open);

	// The last hit the i-frames dodged for `dodger` since the previous call (melee, Precision, projectile, explosion),
	// as the attacker's ref handle (0 = unknown); nullopt when there was none. A projectile ignored because it already
	// passed through does not count. Main thread.
	std::optional<std::uint32_t> TakeDodgedHit(RE::Actor* dodger);
}
