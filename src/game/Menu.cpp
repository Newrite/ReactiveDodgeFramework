#include "Menu.h"

#include "Forms.h"
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

#include <algorithm>
#include <array>
#include <cctype>
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
					const std::string stem = f.textKey ? f.textKey : f.key;
					FieldText t{ "$RDF_" + stem, "$RDF_" + stem + "_Desc", {} };
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

		// A form named by text: the edit box (committed when the edit ends, so half-typed text is never resolved), what the
		// committed text resolves to (forms.h: Peek, resolved on the main thread), a picker over every form of the type and a
		// clear button. The game keeps running with whatever the text resolves to, null included.
		bool DrawForm(const Field& f, const char* label)
		{
			auto&        s = State::Get().Draft();
			std::string& text = *f.str(s);
			bool         changed = false;
			// The host lays a labelled widget out as label | field stretched to the row's end (item width ignored), so the
			// buttons go on the line below, in front of the resolution.
			FUCK::InputText(label, &text);
			const bool editing = FUCK::IsItemActive();
			changed |= FUCK::IsItemDeactivatedAfterEdit();
			if (FUCK::IsItemHovered()) {
				FUCK::SetTooltip("$RDF_Form_Hint"_T);  // no % in the text: the host formats it printf-style
			}
			if (FUCK::Button(ICON_FA_MAGNIFYING_GLASS "##pick")) {
				FUCK::OpenPopup("pick");
			}
			FUCK::SameLine();
			if (FUCK::Button(ICON_FA_XMARK "##clear") && !text.empty()) {
				text.clear();
				changed = true;
			}
			FUCK::SameLine();
			if (FUCK::BeginPopup("pick")) {
				static std::string filter;
				FUCK::InputText("$RDF_Form_Filter"_T, &filter);
				std::string needle = filter;
				std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				FUCK::BeginChild("list", ImVec2(560.0f, 320.0f), true);
				if (const auto catalog = forms::CatalogOf(f.formType); !catalog) {
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
			// what the text resolves to
			if (editing) {
				FUCK::TextDisabled("%s", "$RDF_Form_Editing"_T);
			} else {
				const auto r = forms::Peek(text, f.formType);
				switch (r.status) {
				case forms::Status::Empty: FUCK::TextDisabled("%s", "$RDF_Form_None"_T); break;
				case forms::Status::Pending: FUCK::TextDisabled("%s", "$RDF_Form_Pending"_T); break;
				case forms::Status::Found: FUCK::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "%s %s", ICON_FA_CHECK, r.detail.c_str()); break;
				default: FUCK::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s %s", ICON_FA_TRIANGLE_EXCLAMATION, r.detail.c_str()); break;
				}
			}
			return changed;
		}

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
			case Kind::Form:
				changed = DrawForm(f, label);
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

		// Two rows, keyboard/mouse and gamepad, each showing its own key whatever device is in use (FLICK's managed hotkey
		// shows only the active device's half). FLICK reads the press on the input thread (OnAsyncInput); the result is
		// handed to the drawing thread through atomics and applied to the draft there.
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
				if (const auto kbm = boundKbm_.exchange(kNothing); kbm != kNothing) {
					FromFlick(kbm, ToFlickPad(s), s);
					State::Get().Commit();
				}
				if (const auto pad = boundPad_.exchange(kNothing); pad != kNothing) {
					FromFlick(ToFlickKbm(s), pad, s);
					State::Get().Commit();
				}
				if (slot_.load() != Slot::None && !FUCK::IsBinding()) {
					slot_ = Slot::None;  // FLICK ended it (another widget, the menu closed)
				}
				Row("$RDF_DodgeKeyKbm"_T, "$RDF_DodgeKeyKbm_Desc"_T, Slot::Kbm, ToFlickKbm(s));
				Row("$RDF_DodgeKeyPad"_T, "$RDF_DodgeKeyPad_Desc"_T, Slot::Pad, ToFlickPad(s));
			}

			// Input thread. While one of our rows binds, every press is ours.
			bool OnInput(const void* e)
			{
				const auto slot = slot_.load();
				if (slot == Slot::None) {
					return false;
				}
				std::uint32_t key = 0;
				std::int32_t  mod1 = -1, mod2 = -1;
				switch (FUCK::UpdateBinding(e, &key, &mod1, &mod2)) {
				case FUCK::BindResult::kBound:
					if ((slot == Slot::Pad) == (key >= FUCK::Keys::kGPBase)) {
						(slot == Slot::Pad ? boundPad_ : boundKbm_).store(key);
						slot_ = Slot::None;
					} else {
						FUCK::StartBinding(0, -1, -1, true);  // the other device's button: keep waiting for this one
					}
					break;
				case FUCK::BindResult::kCancelled:
					slot_ = Slot::None;
					break;
				default:
					break;
				}
				return true;
			}

		private:
			enum class Slot : std::uint8_t
			{
				None,
				Kbm,
				Pad,
			};
			static constexpr std::uint32_t kNothing = 0xFFFFFFFF;

			void Row(const char* label, const char* tooltip, Slot slot, std::uint32_t key)
			{
				const bool binding = slot_.load() == slot;
				const std::string text = binding ? std::string("$RDF_PressKey"_T) + "###" + label : std::string(label) + "###" + label;
				const bool busy = !binding && FUCK::IsBinding();
				if (busy) {
					FUCK::BeginDisabled();
				}
				if (FUCK::Hotkey(text.c_str(), binding ? 0 : key, -1, -1, true, true, binding) && !binding) {
					slot_ = slot;
					FUCK::StartBinding(0, -1, -1, true);
				}
				if (busy) {
					FUCK::EndDisabled();
				}
				if (FUCK::IsItemHovered()) {
					FUCK::SetTooltip(tooltip);
				}
			}

			std::atomic<Slot>          slot_{ Slot::None };
			std::atomic<std::uint32_t> boundKbm_{ kNothing };
			std::atomic<std::uint32_t> boundPad_{ kNothing };
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
					FUCK::PushID(fields[n].section);
					FUCK::PushID(fields[n].key);
					DrawField(fields[n], texts[n]);
					FUCK::PopID();
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
