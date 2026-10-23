#include "AttackCancel.h"

#include "Forms.h"
#include "Settings.h"

namespace rdf::game::attack
{
	namespace
	{
		// The engine's own worn-item walk (InventoryChanges::VisitWornItems, 15856 = 0x1401E51D0, the one the WornHasKeyword
		// condition uses): it calls vfunc 1 (Visit) for every worn entry and stops on anything but kContinue.
		class WornHasKeyword final : public RE::InventoryChanges::IItemChangeVisitor
		{
		public:
			explicit WornHasKeyword(const RE::BGSKeyword* keyword) :
				keyword_(keyword) {}

			RE::BSContainer::ForEachResult Visit(RE::InventoryEntryData* entry) override
			{
				const auto* form = entry && entry->object ? entry->object->As<RE::BGSKeywordForm>() : nullptr;
				if (form && form->HasKeyword(keyword_)) {
					found = true;
					return RE::BSContainer::ForEachResult::kStop;
				}
				return RE::BSContainer::ForEachResult::kContinue;
			}

			bool found{ false };

		private:
			const RE::BGSKeyword* keyword_;
		};

		// An effect that counts: active and not dispelled (kInactive: its spell's conditions switched it off), unless
		// `inactive` asks for every effect as the engine's own condition functions see them.
		bool Counts(RE::ActiveEffect* effect, bool inactive)
		{
			return effect && effect->GetBaseObject() &&
			       (inactive || !effect->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled));
		}

		bool HasKeyword(RE::Actor* actor, RE::BGSKeyword* keyword, bool inactive = false)
		{
			if (actor->HasKeyword(keyword)) {
				return true;
			}
			if (const auto* race = actor->GetRace(); race && race->HasKeyword(keyword)) {
				return true;
			}
			if (auto* changes = actor->GetInventoryChanges()) {
				WornHasKeyword worn(keyword);
				changes->VisitWornItems(worn);
				if (worn.found) {
					return true;
				}
			}
			// The engine's own active-effect walk (MagicTarget::VisitEffects): the same on SE, AE and VR.
			bool found = false;
			if (auto* target = actor->AsMagicTarget()) {
				target->VisitActiveEffects([&](RE::ActiveEffect* effect) {
					if (Counts(effect, inactive) && effect->GetBaseObject()->HasKeyword(keyword)) {
						found = true;
						return RE::BSContainer::ForEachResult::kStop;
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			}
			return found;
		}

		// The actor's magic effects that count (Counts) and match: a base effect with the keyword, or that base effect.
		template <class Match, class F>
		void ForEffects(RE::Actor* actor, bool inactive, Match&& match, F&& f)
		{
			auto* target = actor ? actor->AsMagicTarget() : nullptr;
			if (!target) {
				return;
			}
			target->VisitActiveEffects([&](RE::ActiveEffect* effect) {
				if (Counts(effect, inactive) && match(effect->GetBaseObject())) {
					f(effect->GetMagnitude());
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}

		template <class F>
		void ForEffectsWith(RE::Actor* actor, RE::BGSKeyword* keyword, F&& f, bool inactive = false)
		{
			if (keyword) {
				ForEffects(actor, inactive, [&](const RE::EffectSetting* base) { return base->HasKeyword(keyword); }, f);
			}
		}
	}

	bool ActorHasKeyword(RE::Actor* actor, RE::BGSKeyword* keyword, bool inactiveEffects)
	{
		return actor && keyword && HasKeyword(actor, keyword, inactiveEffects);
	}

	std::optional<float> EffectMagnitudes(RE::Actor* actor, RE::BGSKeyword* keyword, bool inactiveEffects)
	{
		std::optional<float> sum;
		ForEffectsWith(actor, keyword, [&](float magnitude) { sum = sum.value_or(0.0f) + magnitude; }, inactiveEffects);
		return sum;
	}

	std::optional<float> EffectMagnitudes(RE::Actor* actor, const RE::EffectSetting* effect, bool inactiveEffects)
	{
		std::optional<float> sum;
		if (effect) {
			ForEffects(actor, inactiveEffects, [&](const RE::EffectSetting* base) { return base == effect; },
				[&](float magnitude) { sum = sum.value_or(0.0f) + magnitude; });
		}
		return sum;
	}

	bool AnyTime(RE::Actor* actor, bool power)
	{
		const auto& ac = Settings::Get().attackCancel;
		auto*       keyword = power ? forms::Get<RE::BGSKeyword>(ac.anyTimePowerKeyword) : forms::Get<RE::BGSKeyword>(ac.anyTimeKeyword);
		return keyword && actor && HasKeyword(actor, keyword);
	}

	bool DodgeForbidden(RE::Actor* actor)
	{
		auto* keyword = forms::Get<RE::BGSKeyword>(Settings::Get().attackCancel.noDodgeKeyword);
		return keyword && actor && HasKeyword(actor, keyword);
	}

	bool StaggerNoEscape(RE::Actor* actor)
	{
		auto* keyword = forms::Get<RE::BGSKeyword>(Settings::Get().stagger.noEscapeKeyword);
		return keyword && actor && HasKeyword(actor, keyword);
	}

	bool StaggerAnyTime(RE::Actor* actor)
	{
		auto* keyword = forms::Get<RE::BGSKeyword>(Settings::Get().stagger.anyTimeKeyword);
		return keyword && actor && HasKeyword(actor, keyword);
	}

	std::optional<float> StaggerDelayOverride(RE::Actor* actor)
	{
		std::optional<float> lowest;
		ForEffectsWith(actor, forms::Get<RE::BGSKeyword>(Settings::Get().stagger.delayKeyword), [&](float magnitude) {
			lowest = lowest ? std::min(*lowest, magnitude) : magnitude;
		});
		return lowest;
	}

	float StaggerEscapeCostBonus(RE::Actor* actor)
	{
		float sum = 0.0f;
		ForEffectsWith(actor, forms::Get<RE::BGSKeyword>(Settings::Get().stagger.escapeCostKeyword), [&](float magnitude) { sum += magnitude; });
		return sum;
	}

	bool DrawDuringDodge(RE::Actor* actor)
	{
		auto* keyword = forms::Get<RE::BGSKeyword>(Settings::Get().casting.drawKeyword);
		return keyword && actor && HasKeyword(actor, keyword);
	}

	bool CastDuringDodge(RE::Actor* actor)
	{
		auto* keyword = forms::Get<RE::BGSKeyword>(Settings::Get().casting.castKeyword);
		return keyword && actor && HasKeyword(actor, keyword);
	}
}
