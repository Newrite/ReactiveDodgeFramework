#include "MagicEffects.h"

#include <algorithm>
#include <cmath>

namespace rdf::game::magic
{
	namespace
	{
		bool Matches(RE::ActiveEffect* effect, const Filter& filter)
		{
			const auto* base = effect->GetBaseObject();
			return (!filter.effect || base == filter.effect) && (!filter.keyword || base->HasKeyword(filter.keyword));
		}

		// Every effect of the actor that counts and matches, until `f` returns false.
		template <class F>
		void ForEach(RE::Actor* actor, const Filter& filter, F&& f)
		{
			auto* target = actor ? actor->AsMagicTarget() : nullptr;
			if (!target || (!filter.keyword && !filter.effect)) {
				return;
			}
			target->VisitActiveEffects([&](RE::ActiveEffect* effect) {
				if (Counts(effect, filter.inactive) && Matches(effect, filter) && !f(effect)) {
					return RE::BSContainer::ForEachResult::kStop;
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
	}

	bool Counts(RE::ActiveEffect* effect, bool inactive)
	{
		return effect && effect->GetBaseObject() &&
		       (inactive || !effect->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled));
	}

	float MagnitudeOf(RE::ActiveEffect* effect, Reading reading)
	{
		const float size = std::abs(effect->GetMagnitude());  // 33282: tapered unless the effect recovers
		if (reading == Reading::Applied) {
			return effect->GetBaseObject()->IsDetrimental() ? -size : size;
		}
		// The sign the spell was cast with (the effect item), whatever the value modifier did to the stored one.
		const bool negative = effect->effect && effect->effect->effectItem.magnitude < 0.0f;
		return negative ? -size : size;
	}

	bool Any(RE::Actor* actor, const Filter& filter)
	{
		bool found = false;
		ForEach(actor, filter, [&](RE::ActiveEffect*) {
			found = true;
			return false;
		});
		return found;
	}

	Sum Query(RE::Actor* actor, const Filter& filter, Reading reading)
	{
		Sum sum;
		ForEach(actor, filter, [&](RE::ActiveEffect* effect) {
			const float m = MagnitudeOf(effect, reading);
			sum.lowest = sum.count ? std::min(sum.lowest, m) : m;
			sum.highest = sum.count ? std::max(sum.highest, m) : m;
			sum.total += m;
			++sum.count;
			return true;
		});
		return sum;
	}
}
