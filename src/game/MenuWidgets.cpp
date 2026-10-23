#include "MenuWidgets.h"

#include "Forms.h"

#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>

#include "FUCK_API.h"
#include "IconsFontAwesome6.h"

#include <algorithm>
#include <cctype>

namespace rdf::game::menu
{
	namespace
	{
		void Picker(std::string& text, RE::FormType type, bool& changed)
		{
			if (!FUCK::BeginPopup("pick")) {
				return;
			}
			static std::string filter;
			FUCK::InputText("$RDF_Form_Filter"_T, &filter);
			std::string needle = filter;
			std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			FUCK::BeginChild("list", ImVec2(560.0f, 320.0f), true);
			if (const auto catalog = forms::CatalogOf(type); !catalog) {
				FUCK::TextDisabled("%s", "$RDF_Form_Loading"_T);
			} else {
				int shown = 0;
				for (const auto& e : *catalog) {
					if (!needle.empty() && e.search.find(needle) == std::string::npos) {
						continue;
					}
					if (++shown > 400) {
						FUCK::TextDisabled("%s", "$RDF_Form_More"_T);
						break;
					}
					if (FUCK::Selectable(e.label.c_str(), e.text == text)) {
						text = e.text;
						changed = true;
						FUCK::CloseCurrentPopup();
					}
				}
				if (shown == 0) {
					FUCK::TextDisabled("%s", "$RDF_Form_NoMatch"_T);
				}
			}
			FUCK::EndChild();
			FUCK::EndPopup();
		}

		void Buttons(std::string& text, bool& changed)
		{
			if (FUCK::Button(ICON_FA_MAGNIFYING_GLASS "##pick")) {
				FUCK::OpenPopup("pick");
			}
			FUCK::SameLine();
			if (FUCK::Button(ICON_FA_XMARK "##clear") && !text.empty()) {
				text.clear();
				changed = true;
			}
		}
	}

	bool FormInput(const char* label, std::string& text, RE::FormType type, bool compact)
	{
		bool changed = false;
		if (compact) {
			const auto r = forms::Peek(text, type);
			const bool found = r.status == forms::Status::Found;
			const bool bad = !found && r.status != forms::Status::Empty && r.status != forms::Status::Pending;
			FUCK::TextColored(found ? ImVec4(0.5f, 0.9f, 0.5f, 1.0f) : bad ? ImVec4(1.0f, 0.55f, 0.35f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
				"%s", found ? ICON_FA_CHECK : bad ? ICON_FA_TRIANGLE_EXCLAMATION : ICON_FA_MINUS);
			FUCK::SameLine();
			Buttons(text, changed);
			FUCK::SameLine();
			FUCK::SetNextItemWidth(-1.0f);
			FUCK::InputText(label, &text);
			changed |= FUCK::IsItemDeactivatedAfterEdit();
			if (FUCK::IsItemHovered() && !r.detail.empty()) {
				FUCK::SetTooltip(r.detail.c_str());  // the host formats tooltips printf-style: a detail never holds '%'
			}
			Picker(text, type, changed);
			return changed;
		}
		// The host lays a labelled widget out as label | field stretched to the row's end (item width ignored), so the
		// buttons go on the line below, in front of the resolution.
		FUCK::InputText(label, &text);
		const bool editing = FUCK::IsItemActive();
		changed |= FUCK::IsItemDeactivatedAfterEdit();
		if (FUCK::IsItemHovered()) {
			FUCK::SetTooltip("$RDF_Form_Hint"_T);  // no % in the text: the host formats it printf-style
		}
		Buttons(text, changed);
		FUCK::SameLine();
		Picker(text, type, changed);
		if (editing) {
			FUCK::TextDisabled("%s", "$RDF_Form_Editing"_T);
		} else {
			const auto r = forms::Peek(text, type);
			switch (r.status) {
			case forms::Status::Empty: FUCK::TextDisabled("%s", "$RDF_Form_None"_T); break;
			case forms::Status::Pending: FUCK::TextDisabled("%s", "$RDF_Form_Pending"_T); break;
			case forms::Status::Found: FUCK::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "%s %s", ICON_FA_CHECK, r.detail.c_str()); break;
			default: FUCK::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s %s", ICON_FA_TRIANGLE_EXCLAMATION, r.detail.c_str()); break;
			}
		}
		return changed;
	}
}
