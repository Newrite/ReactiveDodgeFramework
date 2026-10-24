#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace rdf::game::effects
{
	// Spend / give back the configured dodge cost resource (no-op when the dodge is free).
	void ChargeCost(RE::Actor* actor, float cost);
	void RefundCost(RE::Actor* actor, float cost);

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
	// I-frames are per actor (player or NPC): the hooks drop hits on an actor while it is marked invulnerable.
	// Main thread; read by the hooks on any thread.
	void SetInvulnerable(RE::Actor* actor, bool invulnerable);
	void ClearInvulnerable();  // every actor (load / reset)
	// The actor's perfect-dodge window is open now (main thread, every tick, beside SetInvulnerable): what a perfect-only
	// attacker's hit needs to be dodged.
	void SetPerfectOpen(RE::Actor* actor, bool open);

	// [Rules] what the i-frames may do against an attacker (sPerfectOnlyKeyword / sUndodgeableKeyword). The keyword walk
	// runs on the main thread (attack::RefreshAttackerRules); the hooks, on any thread, only read the result by handle.
	enum class AttackerRule : std::uint8_t
	{
		Normal,
		PerfectOnly,  // dodged only inside the dodger's perfect window
		Undodgeable,  // never dodged: the i-frames let the hit through
	};
	// Main thread: the rules of these attackers (handle, rule) replace what was known of them; Normal ones are forgotten.
	void UpdateAttackerRules(const std::vector<std::pair<std::uint32_t, AttackerRule>>& rules);
	void ClearAttackerRules();
	// The last hit the i-frames dodged for `dodger` since the previous call (melee, Precision, projectile, explosion),
	// as the attacker's ref handle (0 = unknown); nullopt when there was none. A projectile ignored because it already
	// passed through does not count. Main thread.
	std::optional<std::uint32_t> TakeDodgedHit(RE::Actor* dodger);
}
