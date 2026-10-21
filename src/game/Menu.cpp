#include "Menu.h"

#include "Settings.h"
#include "SettingsSchema.h"

#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>

// FLICK's API: ImGui types from the same powerof3/imgui revision FLICK is built with (src/third_party),
// every call goes through FLICK's own interface, so nothing of ImGui is linked here.
#include "FUCK_API.h"
#include "IconsFontAwesome6.h"

#include <array>
#include <atomic>
#include <string>
#include <vector>

namespace rdf::game::menu
{
	namespace
	{
		constexpr auto kPluginName = "ReactiveDodgeFramework";

		using schema::Field;
		using schema::Kind;
		using schema::Page;

		// Translation keys per field, built once: "$RDF_<key>", "$RDF_<key>_Desc", "$RDF_<key>_<choice>".
		struct FieldText
		{
			std::string              label;
			std::string              desc;
			std::vector<std::string> choices;
		};

		const std::vector<FieldText>& Texts()
		{
			static const std::vector<FieldText> texts = [] {
				std::vector<FieldText> out;
				for (const auto& f : schema::Fields()) {
					FieldText t{ std::string("$RDF_") + f.key, std::string("$RDF_") + f.key + "_Desc", {} };
					for (const auto* c : f.choices) {
						t.choices.push_back(std::string("$RDF_") + f.key + "_" + c);
					}
					out.push_back(std::move(t));
				}
				return out;
			}();
			return texts;
		}

		// The menu edits a copy on the render thread; every change is committed to the live settings on
		// the main thread (SKSE task), where the controller hands it to the core between dodges.
		class State
		{
		public:
			static State& Get()
			{
				static State s;
				return s;
			}

			Settings& Draft() { return draft_; }

			void Open()
			{
				if (resync_.exchange(false) || !opened_) {
					draft_ = Settings::Snapshot();
					opened_ = true;
				}
			}

			void Commit()
			{
				unsaved_ = true;
				SKSE::GetTaskInterface()->AddTask([s = draft_]() { Settings::Commit(s); });
			}

			void Save()
			{
				SKSE::GetTaskInterface()->AddTask([s = draft_, this]() {
					Settings::Commit(s);
					std::string error;
					if (schema::SaveToIni(s, &error)) {
						logger::info("menu: settings saved to the ini");
						unsaved_ = false;
					} else {
						logger::error("menu: saving the ini failed: {}", error);
					}
				});
			}

			void Reload()
			{
				SKSE::GetTaskInterface()->AddTask([this]() {
					Settings::Load();
					unsaved_ = false;
					resync_ = true;
				});
			}

			bool Unsaved() const { return unsaved_; }
			void Resync()
			{
				if (resync_.exchange(false)) {
					draft_ = Settings::Snapshot();
				}
			}

		private:
			Settings          draft_;
			bool              opened_{ false };
			std::atomic<bool> unsaved_{ false };
			std::atomic<bool> resync_{ false };
		};

		void DrawField(const Field& f, const FieldText& t)
		{
			auto&       s = State::Get().Draft();
			const char* label = FUCK::Translate(t.label.c_str());
			bool        changed = false;
			switch (f.kind) {
			case Kind::Float:
				changed = FUCK::SliderFloat(label, f.f(s), f.min, f.max, f.format);
				break;
			case Kind::Int:
				changed = FUCK::SliderInt(label, f.i(s), static_cast<int>(f.min), static_cast<int>(f.max));
				break;
			case Kind::Bool:
				changed = FUCK::Checkbox(label, f.b(s));
				break;
			case Kind::Choice:
				{
					std::array<const char*, 8> names{};
					const int                  count = static_cast<int>(std::min(t.choices.size(), names.size()));
					for (int n = 0; n < count; ++n) {
						names[n] = FUCK::Translate(t.choices[n].c_str());
					}
					int current = f.getChoice(s);
					if (FUCK::Combo(label, &current, names.data(), count)) {
						f.setChoice(s, current);
						changed = true;
					}
					break;
				}
			}
			if (FUCK::IsItemHovered()) {
				const char* desc = FUCK::Translate(t.desc.c_str());
				if (desc && desc[0] != '$') {
					FUCK::SetTooltip(desc);  // descriptions carry no '%': the host formats them printf-style
				}
			}
			if (changed) {
				State::Get().Commit();
			}
		}

		void DrawFooter(Page page)
		{
			FUCK::Spacing(2);
			FUCK::Separator();
			FUCK::Spacing();
			auto& state = State::Get();
			if (FUCK::Button(FUCK::IconText(ICON_FA_FLOPPY_DISK, "$RDF_Save"_T))) {
				state.Save();
			}
			FUCK::SameLine();
			if (FUCK::Button(FUCK::IconText(ICON_FA_ARROWS_ROTATE, "$RDF_Reload"_T))) {
				state.Reload();
			}
			FUCK::SameLine();
			if (FUCK::Button(FUCK::IconText(ICON_FA_ROTATE_LEFT, "$RDF_Defaults"_T))) {
				schema::CopyPage(page, Settings{}, state.Draft());
				state.Commit();
			}
			if (state.Unsaved()) {
				FUCK::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", "$RDF_Unsaved"_T);
			} else {
				FUCK::TextDisabled("%s", "$RDF_Live"_T);
			}
		}

		class PageTool final : public FUCK::ITool
		{
		public:
			PageTool(Page page, const char* nameKey, const char* icon, const char* noteKey) :
				page_(page), nameKey_(nameKey), icon_(icon), noteKey_(noteKey) {}

			const char* Name() const override { return FUCK::Translate(nameKey_); }
			const char* Group() const override { return "$RDF_Group"_T; }

			void OnOpen() override { State::Get().Open(); }

			void Draw() override
			{
				auto& state = State::Get();
				state.Resync();
				FUCK::TextColored(ImVec4(0.55f, 0.8f, 1.0f, 1.0f), "%s", FUCK::IconText(icon_, Name()));
				if (noteKey_) {
					FUCK::TextDisabledWrapped("%s", FUCK::Translate(noteKey_));
				}
				FUCK::Separator();
				FUCK::Spacing();
				const auto  fields = schema::Fields();
				const auto& texts = Texts();
				const char* section = nullptr;
				for (std::size_t n = 0; n < fields.size(); ++n) {
					if (fields[n].page != page_) {
						continue;
					}
					if (!section || std::strcmp(section, fields[n].section) != 0) {
						section = fields[n].section;
						const std::string key = std::string("$RDF_Section_") + section;
						FUCK::SeparatorText(FUCK::Translate(key.c_str()));
					}
					FUCK::PushID(fields[n].key);
					DrawField(fields[n], texts[n]);
					FUCK::PopID();
				}
				DrawFooter(page_);
			}

		private:
			Page        page_;
			const char* nameKey_;
			const char* icon_;
			const char* noteKey_;
		};

		// Tools must outlive the session (FLICK keeps the pointers).
		std::vector<PageTool>& Tools()
		{
			static std::vector<PageTool> tools{
				{ Page::Timing, "$RDF_Page_Timing", ICON_FA_STOPWATCH, "$RDF_Note_Timing" },
				{ Page::Motion, "$RDF_Page_Motion", ICON_FA_PERSON_RUNNING, "$RDF_Note_Motion" },
				{ Page::Steering, "$RDF_Page_Steering", ICON_FA_COMPASS, "$RDF_Note_Steering" },
				{ Page::Animation, "$RDF_Page_Animation", ICON_FA_FILM, "$RDF_Note_Animation" },
				{ Page::Rules, "$RDF_Page_Rules", ICON_FA_SCALE_BALANCED, nullptr },
				{ Page::Debug, "$RDF_Page_Debug", ICON_FA_BUG, "$RDF_Note_Debug" },
			};
			return tools;
		}
	}

	void Install()
	{
		if (!GetModuleHandleW(L"FUCK.dll")) {
			logger::info("menu: FLICK not loaded, in-game settings menu disabled");
			return;
		}
		if (!FUCK::Connect(kPluginName)) {
			logger::warn("menu: FLICK found but the API connection failed (version mismatch?)");
			return;
		}
		for (auto& tool : Tools()) {
			FUCK::RegisterTool(&tool);
		}
		logger::info("menu: {} pages registered with FLICK", Tools().size());
	}
}
