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

		// --- dodge keys: one FLICK hotkey, keyboard/mouse and gamepad bound separately, no modifiers.
		// FLICK codes: keyboard = scan code, mouse = 256 + button, gamepad = 266 + SKSE gamepad code.
		// The ini keeps the game's own: iDodgeKeyboard scan code, iDodgeMouse button, iDodgeGamepad XInput mask.
		std::uint32_t ToFlickKbm(const Settings& s)
		{
			if (s.dodgeKeyboard >= 0) {
				return static_cast<std::uint32_t>(s.dodgeKeyboard);
			}
			return s.dodgeMouse >= 0 ? FUCK::Keys::kMBBase + static_cast<std::uint32_t>(s.dodgeMouse) : 0;
		}

		std::uint32_t ToFlickPad(const Settings& s)
		{
			if (s.dodgeGamepad <= 0) {
				return 0;
			}
			const auto code = SKSE::InputMap::GamepadMaskToKeycode(static_cast<std::uint32_t>(s.dodgeGamepad));
			return code >= FUCK::Keys::kGPBase && code < SKSE::InputMap::kMaxMacros ? FUCK::Keys::FromSKSE(code) : 0;
		}

		void FromFlick(std::uint32_t kbm, std::uint32_t pad, Settings& s)
		{
			s.dodgeKeyboard = kbm > 0 && kbm < FUCK::Keys::kMBBase ? static_cast<int>(kbm) : -1;
			s.dodgeMouse = kbm >= FUCK::Keys::kMBBase && kbm < FUCK::Keys::kGPBase ? static_cast<int>(kbm - FUCK::Keys::kMBBase) : -1;
			s.dodgeGamepad = pad >= 2 * FUCK::Keys::kGPBase && pad - FUCK::Keys::kGPBase < SKSE::InputMap::kMaxMacros ?
			                     static_cast<int>(SKSE::InputMap::GamepadKeycodeToMask(pad - FUCK::Keys::kGPBase)) :
			                     -1;
		}

		class DodgeKey
		{
		public:
			static DodgeKey& Get()
			{
				static DodgeKey k;
				return k;
			}

			void Draw()
			{
				auto& s = State::Get().Draft();
				if (!hotkey_.isBinding) {
					hotkey_.kKey = ToFlickKbm(s);
					hotkey_.gKey = ToFlickPad(s);
					hotkey_.kMod1 = hotkey_.kMod2 = hotkey_.gMod1 = hotkey_.gMod2 = -1;
				}
				hotkey_.disallowModifiers = true;
				FUCK::DrawManagedHotkey("$RDF_DodgeKey"_T, hotkey_, FUCK::HotkeyFlags::kNoModifiers);
				if (FUCK::IsItemHovered()) {
					FUCK::SetTooltip("$RDF_DodgeKey_Desc"_T);
				}
				if (!hotkey_.isBinding && (hotkey_.kKey != ToFlickKbm(s) || hotkey_.gKey != ToFlickPad(s))) {
					FromFlick(hotkey_.kKey, hotkey_.gKey, s);
					State::Get().Commit();
				}
			}

			// While binding, FLICK reads the next press from the game's input stream.
			bool OnInput(const void* e) { return hotkey_.isBinding && FUCK::UpdateManagedHotkey(e, hotkey_); }

		private:
			FUCK::ManagedHotkey hotkey_;
		};

		class PageTool final : public FUCK::ITool
		{
		public:
			PageTool(Page page, const char* nameKey, const char* icon, const char* noteKey) :
				page_(page), nameKey_(nameKey), icon_(icon), noteKey_(noteKey) {}

			bool OnAsyncInput(const void* e) override { return page_ == Page::Input && DodgeKey::Get().OnInput(e); }

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
				if (page_ == Page::Input) {
					FUCK::SeparatorText("$RDF_Section_Keys"_T);
					DodgeKey::Get().Draw();
				}
				const auto  fields = schema::Fields();
				const auto& texts = Texts();
				const char* section = nullptr;
				for (std::size_t n = 0; n < fields.size(); ++n) {
					if (fields[n].page != page_ || fields[n].custom) {
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
				{ Page::Input, "$RDF_Page_Input", ICON_FA_KEYBOARD, "$RDF_Note_Input" },
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
