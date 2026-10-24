#pragma once

#include <cstdint>
#include <optional>
#include <string>

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

	// The keyword walk itself (CastOn conditions): the actor (its race's keywords included), a worn item or an active
	// magic effect.
	// `inactiveEffects`: also effects their spell's conditions switched off, and dispelled ones (the engine's way).
	bool ActorHasKeyword(RE::Actor* actor, RE::BGSKeyword* keyword, bool inactiveEffects = false);
	// The summed magnitudes of the actor's effects with the keyword (ini text; empty or unknown = 0), as an amount:
	// only effects whose base effect has Recover count (one without it keeps changing a value rather than holding an
	// amount), a Detrimental effect counts negative, inactive and dispelled ones not at all (magic::Reading::Applied).
	// The cost, chain, i-frame and perfect-window keywords all read this way; sDelayKeyword too, as Authored.
	float EffectAmount(RE::Actor* actor, const std::string& keyword);
	// kDataLoaded: warns in the log of every magic effect that carries one of the amount keywords but lacks Recover (it
	// is not counted), or has "Dispel Effects with these Keywords" (0x100): adding it dispels the other effects with that
	// keyword from other spells (KeywordDispelFunctor, 0x140555910 in IDA), so they never add up.
	void WarnDispellingEffects();

	// [Rules] what the i-frames may do against an attacker (sUndodgeableKeyword first, then sPerfectOnlyKeyword), read by
	// the i-frame hooks when a hit meets them.
	enum class AttackerRule : std::uint8_t
	{
		Normal,
		PerfectOnly,  // dodged only inside the dodger's perfect window
		Undodgeable,  // never dodged: the i-frames let the hit through
	};
	AttackerRule RuleOf(RE::Actor* attacker);

	// [Casting] keywords (same sources): a dodge keeps the actor's bow draw / its cast going.
	bool DrawDuringDodge(RE::Actor* actor);
	bool CastDuringDodge(RE::Actor* actor);
}
