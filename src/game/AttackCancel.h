#pragma once

#include <optional>

namespace rdf::game::attack
{
	// [AttackCancel] sAnyTimeKeyword / sAnyTimePowerKeyword: whether `actor` may cancel its current attack (a power
	// attack when `power`) with a dodge at any moment. The keyword counts on the actor, its race, a worn item or an
	// active magic effect. Resolved from the ini on first use after the data loaded (and again when the menu changes it).
	bool AnyTime(RE::Actor* actor, bool power);
	// [Rules] sNoDodgeKeyword on the actor (same sources): it cannot dodge.
	bool DodgeForbidden(RE::Actor* actor);

	// [Stagger] keywords (same sources as above).
	bool StaggerNoEscape(RE::Actor* actor);
	bool StaggerAnyTime(RE::Actor* actor);
	// The lowest magnitude among the actor's active magic effects with sDelayKeyword (none: no override).
	std::optional<float> StaggerDelayOverride(RE::Actor* actor);
	// The summed magnitudes of the actor's active magic effects with sEscapeCostKeyword.
	float StaggerEscapeCostBonus(RE::Actor* actor);

	// The keyword walk itself (CastOn conditions): the actor, its race, a worn item or an active magic effect.
	// `inactiveEffects`: also effects their spell's conditions switched off, and dispelled ones (the engine's way).
	bool ActorHasKeyword(RE::Actor* actor, RE::BGSKeyword* keyword, bool inactiveEffects = false);

	// [Rules] sPerfectOnlyKeyword / sUndodgeableKeyword: the rules of the actors that may hit `dodger` now (every actor in
	// high process, and the player), handed to the i-frame hooks. Main thread, when the dodger's i-frames begin.
	void RefreshAttackerRules(RE::Actor* dodger);

	// [Casting] keywords (same sources): a dodge keeps the actor's bow draw / its cast going.
	bool DrawDuringDodge(RE::Actor* actor);
	bool CastDuringDodge(RE::Actor* actor);
}
