#include "AttackCancel.h"

#include "Forms.h"
#include "MagicEffects.h"
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
			return magic::Any(actor, { .keyword = keyword, .inactive = inactive });  // effects that count (MagicEffects.h)
		}
	}

	bool ActorHasKeyword(RE::Actor* actor, RE::BGSKeyword* keyword, bool inactiveEffects)
	{
		return actor && keyword && HasKeyword(actor, keyword, inactiveEffects);
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

	// Both carry a number: read as the spell gives it (Authored), so a detrimental effect does not flip its sign.
	std::optional<float> StaggerDelayOverride(RE::Actor* actor)
	{
		const auto sum = magic::Query(actor, { .keyword = forms::Get<RE::BGSKeyword>(Settings::Get().stagger.delayKeyword) }, magic::Reading::Authored);
		return sum.count ? std::optional<float>(sum.lowest) : std::nullopt;
	}

	float StaggerEscapeCostBonus(RE::Actor* actor)
	{
		return magic::Query(actor, { .keyword = forms::Get<RE::BGSKeyword>(Settings::Get().stagger.escapeCostKeyword) }, magic::Reading::Authored).total;
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
