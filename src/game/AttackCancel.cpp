#include "AttackCancel.h"

#include "Forms.h"
#include "MagicEffects.h"
#include "Settings.h"

#include <utility>
#include <vector>

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
			if (actor->HasKeyword(keyword)) {  // the race's keywords included
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

	// The delay is a time (the lowest one wins): read as the spell gives it (Authored).
	std::optional<float> StaggerDelayOverride(RE::Actor* actor)
	{
		const auto sum = magic::Query(actor, { .keyword = forms::Get<RE::BGSKeyword>(Settings::Get().stagger.delayKeyword), .recoverOnly = true },
			magic::Reading::Authored);
		return sum.count ? std::optional<float>(sum.lowest) : std::nullopt;
	}

	float StaggerEscapeCostBonus(RE::Actor* actor)
	{
		return EffectAmount(actor, Settings::Get().stagger.escapeCostKeyword);
	}

	void WarnDispellingEffects()
	{
		const auto& s = Settings::Get();
		std::vector<std::pair<const char*, RE::BGSKeyword*>> keywords;
		for (const auto& [key, text] : { std::pair{ "[Cost] sCostKeyword", &s.costKeyword }, { "[Cost] sCostMultKeyword", &s.costMultKeyword },
				 { "[Cost] sChainCostKeyword", &s.chainCostKeyword }, { "[Timing] sChainDodgesKeyword", &s.chainDodgesKeyword },
				 { "[IFrames] sIFrameKeyword", &s.iFrameKeyword }, { "[Perfect] sPerfectWindowKeyword", &s.perfect.windowKeyword },
				 { "[Stagger] sEscapeCostKeyword", &s.stagger.escapeCostKeyword },
				 { "[Stagger] sEscapeCostMultKeyword", &s.stagger.escapeCostMultKeyword }, { "[Stagger] sDelayKeyword", &s.stagger.delayKeyword } }) {
			if (auto* kw = text->empty() ? nullptr : forms::Get<RE::BGSKeyword>(*text)) {
				keywords.emplace_back(key, kw);
			}
		}
		auto* data = RE::TESDataHandler::GetSingleton();
		if (keywords.empty() || !data) {
			return;
		}
		using Flag = RE::EffectSetting::EffectSettingData::Flag;
		for (auto* effect : data->GetFormArray<RE::EffectSetting>()) {
			if (!effect) {
				continue;
			}
			for (const auto& [key, kw] : keywords) {
				if (!effect->HasKeyword(kw)) {
					continue;
				}
				if (effect->data.flags.none(Flag::kRecover)) {
					logger::warn("magic effect {} ({:08X}) has {} but not Recover: its magnitude is not counted",
						effect->GetFullName(), effect->GetFormID(), key);
				}
				if (effect->data.flags.any(Flag::kDispelWithKeywords)) {
					logger::warn("magic effect {} ({:08X}) has {} and 'Dispel Effects with these Keywords': each new one dispels "
					             "the others from other spells, so their magnitudes never add up",
						effect->GetFullName(), effect->GetFormID(), key);
				}
			}
		}
	}

	float EffectAmount(RE::Actor* actor, const std::string& keyword)
	{
		auto* kw = keyword.empty() ? nullptr : forms::Get<RE::BGSKeyword>(keyword);
		return kw ? magic::Query(actor, { .keyword = kw, .recoverOnly = true }, magic::Reading::Applied).total : 0.0f;
	}

	AttackerRule RuleOf(RE::Actor* attacker)
	{
		if (!attacker) {
			return AttackerRule::Normal;
		}
		// Peek: the hooks run on any thread; the keywords are resolved on the main thread (settings load / the menu).
		const auto& ac = Settings::Get().attackCancel;
		auto*       undodgeable = ac.undodgeableKeyword.empty() ? nullptr : forms::Peek(ac.undodgeableKeyword, RE::FormType::Keyword).form;
		auto*       perfectOnly = ac.perfectOnlyKeyword.empty() ? nullptr : forms::Peek(ac.perfectOnlyKeyword, RE::FormType::Keyword).form;
		if (undodgeable && HasKeyword(attacker, undodgeable->As<RE::BGSKeyword>())) {
			return AttackerRule::Undodgeable;
		}
		if (perfectOnly && HasKeyword(attacker, perfectOnly->As<RE::BGSKeyword>())) {
			return AttackerRule::PerfectOnly;
		}
		return AttackerRule::Normal;
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
