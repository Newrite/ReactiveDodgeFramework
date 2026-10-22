#pragma once

namespace rdf::game::attack
{
	// [AttackCancel] sAnyTimeKeyword / sAnyTimePowerKeyword: whether `actor` may cancel its current attack (a power
	// attack when `power`) with a dodge at any moment. The keyword counts on the actor, its race, a worn item or an
	// active magic effect. Resolved from the ini on first use after the data loaded (and again when the menu changes it).
	bool AnyTime(RE::Actor* actor, bool power);
	// [Rules] sNoDodgeKeyword on the actor (same sources): it cannot dodge.
	bool DodgeForbidden(RE::Actor* actor);
}
