#include "LegacyDodge.h"

#include "Settings.h"

namespace rdf::game::legacy
{
	namespace
	{
		void Zero(std::string_view editorId)
		{
			auto* global = RE::TESForm::LookupByEditorID<RE::TESGlobal>(editorId);
			if (!global) {
				logger::info("legacy dodge: global {} not found, nothing to suppress", editorId);
				return;
			}
			if (global->value == 0.0f) {
				logger::info("legacy dodge: {} is already 0", editorId);
				return;
			}
			logger::info("legacy dodge: {} was {}, set to 0 (restore with `set {} to {}`)", editorId, global->value, editorId, global->value);
			global->value = 0.0f;
		}
	}

	void Suppress()
	{
		if (!Settings::Get().suppressLegacyDodge) {
			return;
		}
		Zero("UDDodgeKey"sv);
		Zero("UDSprintDodge"sv);
	}
}
