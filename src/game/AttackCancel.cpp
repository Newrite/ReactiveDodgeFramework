#include "AttackCancel.h"

#include "Settings.h"

namespace rdf::game::attack
{
	namespace
	{
		// "Plugin.esp|0xFormID" (local form id, as xEdit shows it without the load-order byte) or an EditorID.
		RE::BGSKeyword* Lookup(const std::string& text)
		{
			if (text.empty()) {
				return nullptr;
			}
			if (const auto bar = text.find('|'); bar != std::string::npos) {
				const auto plugin = text.substr(0, bar);
				const auto idText = text.substr(bar + 1);
				std::uint32_t id = 0;
				try {
					id = static_cast<std::uint32_t>(std::stoul(idText, nullptr, 16));
				} catch (...) {
					logger::warn("[AttackCancel] '{}': '{}' is not a hex form id", text, idText);
					return nullptr;
				}
				auto* handler = RE::TESDataHandler::GetSingleton();
				auto* form = handler ? handler->LookupForm(id & 0x00FFFFFF, plugin) : nullptr;
				if (!form) {
					form = handler ? handler->LookupForm(id & 0x00000FFF, plugin) : nullptr;  // a light plugin's local id
				}
				return form ? form->As<RE::BGSKeyword>() : nullptr;
			}
			return RE::TESForm::LookupByEditorID<RE::BGSKeyword>(text);
		}

		struct Cached
		{
			std::string     text;
			RE::BGSKeyword* keyword{ nullptr };
			bool            resolved{ false };
		};
		Cached g_light, g_power;

		RE::BGSKeyword* Resolve(Cached& c, const std::string& text)
		{
			if (!c.resolved || c.text != text) {
				c.text = text;
				c.keyword = Lookup(text);
				c.resolved = true;
				if (!text.empty()) {
					if (c.keyword) {
						logger::info("[AttackCancel] keyword '{}' -> {:08X}", text, c.keyword->GetFormID());
					} else {
						logger::warn("[AttackCancel] keyword '{}' not found (no such plugin / form, or not a keyword)", text);
					}
				}
			}
			return c.keyword;
		}

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
	}

	bool AnyTime(RE::Actor* actor, bool power)
	{
		const auto& ac = Settings::Get().attackCancel;
		auto*       keyword = power ? Resolve(g_power, ac.anyTimePowerKeyword) : Resolve(g_light, ac.anyTimeKeyword);
		return keyword && actor && HasKeyword(actor, keyword);
	}
}
