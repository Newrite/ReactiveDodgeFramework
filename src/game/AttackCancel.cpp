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

		bool HasKeyword(RE::Actor* actor, RE::BGSKeyword* keyword)
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
					const auto* base = effect ? effect->GetBaseObject() : nullptr;
					if (base && !effect->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled) && base->HasKeyword(keyword)) {
						found = true;
						return RE::BSContainer::ForEachResult::kStop;
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			}
			return found;
		}

		// Active, not dispelled magic effects of the actor whose base effect carries the keyword.
		template <class F>
		void ForEffectsWith(RE::Actor* actor, RE::BGSKeyword* keyword, F&& f)
		{
			auto* target = actor ? actor->AsMagicTarget() : nullptr;
			if (!keyword || !target) {
				return;
			}
			target->VisitActiveEffects([&](RE::ActiveEffect* effect) {
				const auto* base = effect ? effect->GetBaseObject() : nullptr;
				if (base && !effect->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled) && base->HasKeyword(keyword)) {
					f(effect->GetMagnitude());
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
	}

	bool ActorHasKeyword(RE::Actor* actor, RE::BGSKeyword* keyword)
	{
		return actor && keyword && HasKeyword(actor, keyword);
	}

	std::optional<float> EffectMagnitudes(RE::Actor* actor, RE::BGSKeyword* keyword)
	{
		std::optional<float> sum;
		ForEffectsWith(actor, keyword, [&](float magnitude) { sum = sum.value_or(0.0f) + magnitude; });
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
