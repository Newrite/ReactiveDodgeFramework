#include "CastOnEditor.h"

#include "CastOn.h"
#include "Forms.h"
#include "MenuWidgets.h"

#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>

#include "FUCK_API.h"
#include "IconsFontAwesome6.h"

#include <algorithm>
#include <atomic>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace rdf::game::caston_editor
{
	namespace
	{
		using namespace rdf::caston;
		namespace co = rdf::game::caston;

		// Names as the JSON writes them (enum order).
		constexpr const char* kTriggers[]{ "Dodge", "PerfectDodge" };
		constexpr const char* kSubjects[]{ "Dodger", "Attacker", "CombatTarget" };
		constexpr const char* kTargets[]{ "Self", "Dodger", "Attacker", "CombatTarget", "Aim" };
		constexpr const char* kCostModes[]{ "Free", "Engine", "Custom" };
		constexpr const char* kCostValues[]{ "Magicka", "Stamina", "Health" };
		constexpr const char* kShortfalls[]{ "Skip", "CastAnyway", "Scale" };
		constexpr const char* kCompares[]{ "-", "=", "!=", "<", "<=", ">", ">=" };
		constexpr const char* kHands[]{ "Right", "Left", "Either" };
		constexpr const char* kModes[]{ "Current", "Max", "Percent", "Base" };
		static_assert(std::size(kTriggers) == static_cast<std::size_t>(Trigger::PerfectDodge) + 1);
		static_assert(std::size(kSubjects) == static_cast<std::size_t>(Subject::CombatTarget) + 1);
		static_assert(std::size(kTargets) == static_cast<std::size_t>(TargetKind::Aim) + 1);
		static_assert(std::size(kCostModes) == static_cast<std::size_t>(CostMode::Custom) + 1);
		static_assert(std::size(kCostValues) == static_cast<std::size_t>(CostValue::Health) + 1);
		static_assert(std::size(kShortfalls) == static_cast<std::size_t>(Shortfall::Scale) + 1);
		static_assert(std::size(kCompares) == static_cast<std::size_t>(Compare::Ge) + 1);
		static_assert(std::size(kHands) == static_cast<std::size_t>(Hand::Either) + 1);
		static_assert(std::size(kModes) == static_cast<std::size_t>(ValueMode::Base) + 1);

		const std::vector<const char*>& KindNames()
		{
			static const std::vector<const char*> names = [] {
				std::vector<const char*> out;
				for (int k = 0; k <= static_cast<int>(ConditionKind::Preset); ++k) {
					out.push_back(Info(static_cast<ConditionKind>(k)).name.data());  // literals: null-terminated
				}
				return out;
			}();
			return names;
		}

		template <std::size_t N>
		std::vector<const char*> Names(const std::string_view (&list)[N])
		{
			std::vector<const char*> out;
			for (const auto& s : list) {
				out.push_back(s.data());
			}
			return out;
		}

		RE::FormType FormTypeOf(ConditionKind kind)
		{
			switch (kind) {
			case ConditionKind::HasKeyword:
			case ConditionKind::HasEffectKeyword: return RE::FormType::Keyword;
			case ConditionKind::HasPerk: return RE::FormType::Perk;
			case ConditionKind::IsRace: return RE::FormType::Race;
			case ConditionKind::InFaction: return RE::FormType::Faction;
			default: return RE::FormType::None;
			}
		}

		// The conditions about the dodge itself have no participant.
		bool HasSubject(ConditionKind kind)
		{
			switch (kind) {
			case ConditionKind::Layer:
			case ConditionKind::Direction:
			case ConditionKind::ChainIndex:
			case ConditionKind::FromStagger:
			case ConditionKind::Random:
			case ConditionKind::Preset: return false;
			default: return !Info(kind).group;
			}
		}

		// One line per kind for the editor (OAR shows a description per condition).
		const char* Describe(ConditionKind kind)
		{
			switch (kind) {
			case ConditionKind::And: return "$RDF_CK_And"_T;
			case ConditionKind::Or: return "$RDF_CK_Or"_T;
			case ConditionKind::HasKeyword: return "$RDF_CK_HasKeyword"_T;
			case ConditionKind::HasEffectKeyword: return "$RDF_CK_HasEffectKeyword"_T;
			case ConditionKind::HasPerk: return "$RDF_CK_HasPerk"_T;
			case ConditionKind::IsRace: return "$RDF_CK_IsRace"_T;
			case ConditionKind::InFaction: return "$RDF_CK_InFaction"_T;
			case ConditionKind::Equipped: return "$RDF_CK_Equipped"_T;
			case ConditionKind::ActorValue: return "$RDF_CK_ActorValue"_T;
			case ConditionKind::InCombat: return "$RDF_CK_InCombat"_T;
			case ConditionKind::Sneaking: return "$RDF_CK_Sneaking"_T;
			case ConditionKind::Sprinting: return "$RDF_CK_Sprinting"_T;
			case ConditionKind::Blocking: return "$RDF_CK_Blocking"_T;
			case ConditionKind::Attacking: return "$RDF_CK_Attacking"_T;
			case ConditionKind::PowerAttacking: return "$RDF_CK_PowerAttacking"_T;
			case ConditionKind::Casting: return "$RDF_CK_Casting"_T;
			case ConditionKind::IsPlayer: return "$RDF_CK_IsPlayer"_T;
			case ConditionKind::FirstPerson: return "$RDF_CK_FirstPerson"_T;
			case ConditionKind::Layer: return "$RDF_CK_Layer"_T;
			case ConditionKind::Direction: return "$RDF_CK_Direction"_T;
			case ConditionKind::ChainIndex: return "$RDF_CK_ChainIndex"_T;
			case ConditionKind::FromStagger: return "$RDF_CK_FromStagger"_T;
			case ConditionKind::Random: return "$RDF_CK_Random"_T;
			case ConditionKind::Preset: return "$RDF_CK_Preset"_T;
			default: return "";
			}
		}

		template <class E, std::size_t N>
		bool EnumCombo(const char* id, E& value, const char* const (&names)[N], float width)
		{
			int i = static_cast<int>(value);
			FUCK::SetNextItemWidth(width);
			if (FUCK::Combo(id, &i, names, static_cast<int>(N))) {
				value = static_cast<E>(i);
				return true;
			}
			return false;
		}

		bool TextCombo(const char* id, std::string& text, const std::vector<const char*>& names, float width)
		{
			int i = -1;
			for (int n = 0; n < static_cast<int>(names.size()); ++n) {
				if (text == names[n]) {
					i = n;
				}
			}
			FUCK::SetNextItemWidth(width);
			if (FUCK::Combo(id, &i, names.data(), static_cast<int>(names.size())) && i >= 0) {
				text = names[i];
				return true;
			}
			return false;
		}

		// A text box whose edit counts when it ends (no half-typed values reach the rules).
		bool Text(const char* id, std::string& text, float width)
		{
			FUCK::SetNextItemWidth(width);
			FUCK::InputText(id, &text);
			return FUCK::IsItemDeactivatedAfterEdit();
		}

		void LiveIcon(std::int8_t v)
		{
			switch (v) {
			case 1: FUCK::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f), "%s", ICON_FA_CHECK); break;
			case 0: FUCK::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", ICON_FA_XMARK); break;
			case -1: FUCK::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", ICON_FA_QUESTION); break;
			default: FUCK::TextDisabled(" "); break;
			}
			if (v == -1 && FUCK::IsItemHovered()) {
				FUCK::SetTooltip("$RDF_CO_LiveUnknown"_T);
			}
		}

		struct EditDoc
		{
			std::string              name;
			bool                     user{ false };
			std::vector<Rule>        rules;
			Presets                  presets;
			std::vector<std::string> loadErrors;
			bool                     dirty{ false };
		};

		class Editor final : public FUCK::IWindow
		{
		public:
			const char*       Id() const override { return "CastOnRules"; }
			const char*       Title() const override { return "$RDF_CO_Title"_T; }
			bool              IsOpen() const override { return open_.load(); }
			FUCK::WindowFlags GetFlags() const override { return FUCK::WindowFlags::kNone; }
			ImVec2            GetDefaultSize() const override { return ImVec2(1250.0f, 780.0f); }

			void SetOpen(bool open) override
			{
				open_ = open;
				if (!open) {
					co::WatchRule(std::nullopt, {});
				} else {
					watched_ = false;  // send what is selected again
				}
			}

			void Draw() override
			{
				if (!loaded_) {
					ReloadFiles();
				}
				changed_ = false;
				DrawToolbar();
				if (FUCK::BeginTable("##layout", 2, FUCK::TableFlags::kResizable | FUCK::TableFlags::kBordersInnerV)) {
					FUCK::TableSetupColumn("##list", FUCK::TableColumnFlags::kWidthFixed, 380.0f);
					FUCK::TableSetupColumn("##edit", FUCK::TableColumnFlags::kWidthStretch);
					FUCK::TableNextRow();
					FUCK::TableNextColumn();
					FUCK::BeginChild("##left", ImVec2(0.0f, 0.0f));
					DrawList();
					FUCK::EndChild();
					FUCK::TableNextColumn();
					FUCK::BeginChild("##right", ImVec2(0.0f, 0.0f));
					DrawSelection();
					FUCK::EndChild();
					FUCK::EndTable();
				}
				if (changed_ && doc_ >= 0 && doc_ < static_cast<int>(docs_.size())) {
					docs_[doc_].dirty = true;
					watched_ = false;
				}
				if (!watched_) {
					SendWatch();
				}
				co::PollLive();
			}

		private:
			enum class Op  // what a condition node asked for, applied after its list is drawn
			{
				None,
				Up,
				Down,
				Duplicate,
				Delete,
				PasteBelow,
			};

			enum class Selected
			{
				None,
				Rule,
				Preset,
			};

			// ---------------------------------------------------------------------------------------------- data
			void ReloadFiles()
			{
				docs_.clear();
				for (auto& d : co::ReadDocuments()) {
					docs_.push_back({ d.name, d.user, std::move(d.parsed.rules), std::move(d.parsed.presets), std::move(d.parsed.errors), false });
				}
				loaded_ = true;
				if (doc_ >= static_cast<int>(docs_.size())) {
					Select(-1, Selected::None, -1, {});
				}
				watched_ = false;
			}

			Presets AllPresets() const  // as the game sees them: the first file by name wins a name
			{
				Presets out;
				for (const auto& d : docs_) {
					for (const auto& [name, list] : d.presets) {
						out.emplace(name, list);
					}
				}
				return out;
			}

			void Select(int doc, Selected what, int rule, std::string preset)
			{
				doc_ = doc;
				what_ = what;
				rule_ = rule;
				preset_ = std::move(preset);
				watched_ = false;
			}

			Rule* SelectedRule()
			{
				if (what_ != Selected::Rule || doc_ < 0 || doc_ >= static_cast<int>(docs_.size())) {
					return nullptr;
				}
				auto& rules = docs_[doc_].rules;
				return rule_ >= 0 && rule_ < static_cast<int>(rules.size()) ? &rules[rule_] : nullptr;
			}

			std::vector<Condition>* SelectedPreset()
			{
				if (what_ != Selected::Preset || doc_ < 0 || doc_ >= static_cast<int>(docs_.size())) {
					return nullptr;
				}
				auto& presets = docs_[doc_].presets;
				const auto it = presets.find(preset_);
				return it == presets.end() ? nullptr : &it->second;
			}

			// What the live results answer: the selected rule, or a preset as a rule of its conditions.
			void SendWatch()
			{
				watched_ = true;
				if (const auto* rule = SelectedRule()) {
					co::WatchRule(*rule, AllPresets());
				} else if (const auto* list = SelectedPreset()) {
					Rule probe;
					probe.conditions = *list;
					co::WatchRule(probe, AllPresets());
				} else {
					co::WatchRule(std::nullopt, {});
				}
			}

			void Save(EditDoc& doc)
			{
				std::string error;
				if (co::SaveUserDocument(doc.name, doc.rules, doc.presets, error)) {
					doc.user = true;
					doc.dirty = false;
					status_ = std::format("{} {}.user.json", "$RDF_CO_Saved"_T, doc.name);
					co::ReloadInGame();
				} else {
					status_ = error;
				}
			}

			// ---------------------------------------------------------------------------------------------- toolbar
			void DrawToolbar()
			{
				EditDoc* doc = doc_ >= 0 && doc_ < static_cast<int>(docs_.size()) ? &docs_[doc_] : nullptr;
				FUCK::BeginDisabled(!doc);
				if (FUCK::Button(FUCK::IconText(ICON_FA_FLOPPY_DISK, "$RDF_CO_Save"_T)) && doc) {
					Save(*doc);
				}
				if (FUCK::IsItemHovered()) {
					FUCK::SetTooltip("$RDF_CO_SaveTip"_T);
				}
				FUCK::EndDisabled();
				FUCK::SameLine();
				if (FUCK::Button(FUCK::IconText(ICON_FA_ARROWS_ROTATE, "$RDF_CO_ReloadFiles"_T))) {
					ReloadFiles();
					status_ = "$RDF_CO_Reloaded"_T;
				}
				if (FUCK::IsItemHovered()) {
					FUCK::SetTooltip("$RDF_CO_ReloadTip"_T);
				}
				FUCK::SameLine();
				if (FUCK::Button(FUCK::IconText(ICON_FA_BOLT, "$RDF_CO_Apply"_T))) {
					co::ReloadInGame();
					status_ = "$RDF_CO_Applied"_T;
				}
				if (FUCK::IsItemHovered()) {
					FUCK::SetTooltip("$RDF_CO_ApplyTip"_T);
				}
				FUCK::SameLine();
				FUCK::BeginDisabled(!doc || !doc->user);
				if (FUCK::Button(FUCK::IconText(ICON_FA_TRASH, "$RDF_CO_DeleteUser"_T)) && doc) {
					std::string error;
					if (co::DeleteUserDocument(doc->name, error)) {
						ReloadFiles();
						co::ReloadInGame();
						status_ = "$RDF_CO_DeletedUser"_T;
					} else {
						status_ = error;
					}
				}
				if (FUCK::IsItemHovered()) {
					FUCK::SetTooltip("$RDF_CO_DeleteUserTip"_T);
				}
				FUCK::EndDisabled();
				FUCK::SameLine();
				if (FUCK::Button(FUCK::IconText(ICON_FA_FILE_CIRCLE_PLUS, "$RDF_CO_NewFile"_T))) {
					newName_.clear();
					FUCK::OpenPopup("##newfile");
				}
				DrawNewFilePopup();
				if (!status_.empty()) {
					FUCK::SameLine();
					FUCK::TextDisabled("%s", status_.c_str());
				}
				FUCK::Separator();
			}

			void DrawNewFilePopup()
			{
				if (!FUCK::BeginPopup("##newfile")) {
					return;
				}
				FUCK::Text("%s", "$RDF_CO_FileName"_T);
				FUCK::SetNextItemWidth(260.0f);
				FUCK::InputText("##name", &newName_);
				const bool valid = !newName_.empty() && newName_.find_first_of("\\/:*?\"<>|.") == std::string::npos &&
				                   std::none_of(docs_.begin(), docs_.end(), [&](const EditDoc& d) { return _stricmp(d.name.c_str(), newName_.c_str()) == 0; });
				FUCK::BeginDisabled(!valid);
				if (FUCK::Button("$RDF_CO_Create"_T)) {
					docs_.push_back({ newName_, true, {}, {}, {}, true });
					Select(static_cast<int>(docs_.size()) - 1, Selected::None, -1, {});
					FUCK::CloseCurrentPopup();
				}
				FUCK::EndDisabled();
				FUCK::EndPopup();
			}

			// ---------------------------------------------------------------------------------------------- left
			void DrawList()
			{
				if (docs_.empty()) {
					FUCK::TextDisabledWrapped("%s", "$RDF_CO_NoFiles"_T);
				}
				for (int d = 0; d < static_cast<int>(docs_.size()); ++d) {
					auto& doc = docs_[d];
					FUCK::PushID(d);
					const auto header = std::format("{}{}{}###doc", doc.name, doc.user ? " (user)" : "", doc.dirty ? " *" : "");
					if (FUCK::CollapsingHeader(header.c_str(), 1 << 5 /* DefaultOpen */)) {
						for (const auto& e : doc.loadErrors) {
							FUCK::TextColoredWrapped(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", e.c_str());
						}
						for (int r = 0; r < static_cast<int>(doc.rules.size()); ++r) {
							auto& rule = doc.rules[r];
							FUCK::PushID(r);
							if (FUCK::Checkbox("##on", &rule.enabled, false, false)) {
								doc.dirty = true;
								watched_ = false;
							}
							FUCK::SameLine();
							const auto label = std::format("{}  [{}{}]###rule", rule.name.empty() ? std::format("#{}", r) : rule.name,
								rule.trigger == Trigger::Dodge ? "Dodge" : "Perfect", rule.priority ? std::format(" {}", rule.priority) : std::string());
							if (FUCK::Selectable(label.c_str(), doc_ == d && what_ == Selected::Rule && rule_ == r)) {
								Select(d, Selected::Rule, r, {});
							}
							FUCK::PopID();
						}
						for (const auto& [name, list] : doc.presets) {
							const auto label = std::format("{} {} ({})###p{}", ICON_FA_LIST, name, list.size(), name);
							if (FUCK::Selectable(label.c_str(), doc_ == d && what_ == Selected::Preset && preset_ == name)) {
								Select(d, Selected::Preset, -1, name);
							}
						}
						if (FUCK::Button(FUCK::IconText(ICON_FA_PLUS, "$RDF_CO_AddRule"_T))) {
							Rule rule;
							rule.name = std::format("New rule {}", doc.rules.size() + 1);
							rule.casts.push_back({});
							doc.rules.push_back(std::move(rule));
							doc.dirty = true;
							Select(d, Selected::Rule, static_cast<int>(doc.rules.size()) - 1, {});
						}
						FUCK::SameLine();
						if (FUCK::Button(FUCK::IconText(ICON_FA_PLUS, "$RDF_CO_AddPreset"_T))) {
							std::string name;
							for (int n = 1; name.empty() || doc.presets.contains(name); ++n) {
								name = std::format("Preset{}", n);
							}
							doc.presets[name] = { Condition{ .kind = ConditionKind::InCombat } };
							doc.dirty = true;
							Select(d, Selected::Preset, -1, name);
						}
					}
					FUCK::PopID();
				}
			}

			// ---------------------------------------------------------------------------------------------- right
			void DrawSelection()
			{
				if (auto* rule = SelectedRule()) {
					DrawRule(*rule);
				} else if (auto* list = SelectedPreset()) {
					DrawPreset(*list);
				} else {
					FUCK::TextDisabledWrapped("%s", "$RDF_CO_SelectHint"_T);
				}
			}

			void DrawLiveHeader()
			{
				const auto live = co::LiveResults();
				FUCK::TextDisabled("%s %s", "$RDF_CO_Live"_T, live.participants.empty() ? "$RDF_CO_LiveNone"_T : live.participants.c_str());
			}

			void DrawPreset(std::vector<Condition>& list)
			{
				auto& doc = docs_[doc_];
				if (rename_.empty() || renameFor_ != preset_) {
					rename_ = preset_;
					renameFor_ = preset_;
				}
				FUCK::SetNextItemWidth(300.0f);
				FUCK::InputText("$RDF_CO_PresetName"_T, &rename_);
				if (FUCK::IsItemDeactivatedAfterEdit() && !rename_.empty() && rename_ != preset_ && !doc.presets.contains(rename_)) {
					auto node = doc.presets.extract(preset_);
					node.key() = rename_;
					doc.presets.insert(std::move(node));
					preset_ = rename_;
					renameFor_ = rename_;
					changed_ = true;
					return;  // `list` belonged to the old node
				}
				FUCK::TextDisabledWrapped("%s", "$RDF_CO_PresetNote"_T);
				DrawLiveHeader();
				DrawConditionTable(list);
				FUCK::Spacing();
				if (FUCK::Button(FUCK::IconText(ICON_FA_TRASH, "$RDF_CO_DeletePreset"_T))) {
					doc.presets.erase(preset_);
					Select(doc_, Selected::None, -1, {});
					doc.dirty = true;
				}
			}

			void DrawRule(Rule& rule)
			{
				changed_ |= Text("$RDF_CO_Name"_T, rule.name, -1.0f);
				changed_ |= FUCK::Checkbox("$RDF_CO_Enabled"_T, &rule.enabled);
				changed_ |= EnumCombo("$RDF_CO_Trigger"_T, rule.trigger, kTriggers, -1.0f);
				changed_ |= FUCK::DragInt("$RDF_CO_Priority"_T, &rule.priority, 1.0f, -100000, 100000);
				changed_ |= FUCK::Checkbox("$RDF_CO_Exclusive"_T, &rule.exclusive);
				if (!rule.extends.empty()) {
					FUCK::TextDisabledWrapped("%s %s", "$RDF_CO_FromTemplate"_T, rule.extends.c_str());
				}

				FUCK::SeparatorText("$RDF_CO_Conditions"_T);
				DrawLiveHeader();
				DrawConditionTable(rule.conditions);

				FUCK::SeparatorText("$RDF_CO_Casts"_T);
				DrawCasts(rule.casts);

				FUCK::SeparatorText("$RDF_CO_Cost"_T);
				DrawCost(rule.cost);

				FUCK::SeparatorText("$RDF_CO_Timing"_T);
				changed_ |= FUCK::DragFloat("$RDF_CO_Delay"_T, &rule.timing.delay, 0.01f, 0.0f, 10.0f, "%.2f s");
				changed_ |= Text("$RDF_CO_Tag"_T, rule.timing.tag, -1.0f);
				changed_ |= FUCK::Checkbox("$RDF_CO_Annotation"_T, &rule.timing.annotation);

				FUCK::SeparatorText("$RDF_CO_Limits"_T);
				changed_ |= FUCK::DragFloat("$RDF_CO_Cooldown"_T, &rule.limits.cooldown, 0.05f, 0.0f, 600.0f, "%.2f s");
				changed_ |= FUCK::Checkbox("$RDF_CO_OncePerChain"_T, &rule.limits.oncePerChain);
				changed_ |= FUCK::DragFloat("$RDF_CO_Chance"_T, &rule.limits.chance, 0.5f, 0.0f, 100.0f, "%.0f");

				if (const auto problems = Validate(rule); !problems.empty()) {
					FUCK::SeparatorText("$RDF_CO_Problems"_T);
					for (const auto& p : problems) {
						FUCK::TextColoredWrapped(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", p.c_str());
					}
				}
				FUCK::Spacing();
				FUCK::Separator();
				if (FUCK::Button(FUCK::IconText(ICON_FA_PLAY, "$RDF_CO_FireNow"_T))) {
					co::FireNow(rule, AllPresets());
				}
				if (FUCK::IsItemHovered()) {
					FUCK::SetTooltip("$RDF_CO_FireNowTip"_T);
				}
				FUCK::SameLine();
				auto& doc = docs_[doc_];
				if (FUCK::Button(FUCK::IconText(ICON_FA_COPY, "$RDF_CO_Duplicate"_T))) {
					Rule copy = rule;
					copy.name += " (copy)";
					doc.rules.insert(doc.rules.begin() + rule_ + 1, std::move(copy));
					doc.dirty = true;
					Select(doc_, Selected::Rule, rule_ + 1, {});
					return;
				}
				FUCK::SameLine();
				if (FUCK::Button(FUCK::IconText(ICON_FA_TRASH, "$RDF_CO_Delete"_T))) {
					doc.rules.erase(doc.rules.begin() + rule_);
					doc.dirty = true;
					Select(doc_, Selected::None, -1, {});
				}
			}

			// ---------------------------------------------------------------------------------------------- conditions
			// OAR's layout (UIMain::DrawCondition): every condition a small bordered block whose header is one line (open
			// arrow, live result, NOT and the name, a dim summary of its arguments); opened, its type, participant and each
			// argument on a row of their own; a group draws its children the same way inside. Right click: copy, paste,
			// duplicate, delete. Live results come in depth-first order over the whole tree, collapsed parts included.
			void DrawConditionTable(std::vector<Condition>& list)
			{
				const auto live = co::LiveResults();
				int        row = 0;
				DrawConditionList(list, row, live.results);
			}

			static int RowsOf(const Condition& c)
			{
				int n = 1;
				for (const auto& child : c.children) {
					n += RowsOf(child);
				}
				return n;
			}

			void AddButtons(std::vector<Condition>& list)
			{
				if (FUCK::Button(FUCK::IconText(ICON_FA_PLUS, "$RDF_CO_AddCondition"_T))) {
					list.push_back(Condition{ .kind = lastKind_ });
					OpenNext(list, list.size() - 1);
				}
				FUCK::SameLine();
				if (FUCK::Button(FUCK::IconText(ICON_FA_FOLDER_PLUS, "$RDF_CO_AddGroup"_T))) {
					list.push_back(Condition{ .kind = ConditionKind::Or, .children = { Condition{ .kind = lastKind_ } } });
					OpenNext(list, list.size() - 1);
				}
				FUCK::SameLine();
				FUCK::BeginDisabled(!clipboard_);
				if (FUCK::Button(FUCK::IconText(ICON_FA_PASTE, "$RDF_CO_Paste"_T)) && clipboard_) {
					list.push_back(*clipboard_);
					OpenNext(list, list.size() - 1);
				}
				FUCK::EndDisabled();
			}

			void OpenNext(const std::vector<Condition>& list, std::size_t index)
			{
				openList_ = &list;
				openIndex_ = index;
				changed_ = true;
			}

			void DrawConditionList(std::vector<Condition>& list, int& row, const std::vector<std::int8_t>& live)
			{
				Op  op = Op::None;
				int at = -1;
				for (int i = 0; i < static_cast<int>(list.size()); ++i) {
					auto&      c = list[i];
					const int  myRow = row;
					const auto result = myRow < static_cast<int>(live.size()) ? live[myRow] : static_cast<std::int8_t>(-2);
					FUCK::PushID(i);
					if (FUCK::BeginTable("##node", 1, FUCK::TableFlags::kBordersOuterH | FUCK::TableFlags::kBordersOuterV)) {
						FUCK::TableNextRow();
						FUCK::TableNextColumn();
						if (openList_ == &list && openIndex_ == static_cast<std::size_t>(i)) {
							FUCK::SetNextItemOpen(true, 1 /* Always */);
							openList_ = nullptr;
						}
						const bool open = FUCK::TreeNode("##open", kNodeFlags);
						if (FUCK::BeginPopupContextItem("##menu")) {
							if (FUCK::Selectable(FUCK::IconText(ICON_FA_COPY, "$RDF_CO_Copy"_T))) {
								clipboard_ = c;
							}
							if (FUCK::Selectable(FUCK::IconText(ICON_FA_PASTE, "$RDF_CO_PasteBelow"_T), false, clipboard_ ? 0 : (1 << 3) /* Disabled */)) {
								op = Op::PasteBelow;
								at = i;
							}
							if (FUCK::Selectable(FUCK::IconText(ICON_FA_CLONE, "$RDF_CO_Duplicate"_T))) {
								op = Op::Duplicate;
								at = i;
							}
							if (FUCK::Selectable(FUCK::IconText(ICON_FA_TRASH, "$RDF_CO_Delete"_T))) {
								op = Op::Delete;
								at = i;
							}
							FUCK::EndPopup();
						}
						FUCK::SameLine();
						LiveIcon(result);
						FUCK::SameLine();
						const auto title = std::format("{}{}", c.negated ? "NOT " : "", Info(c.kind).name);
						if (c.kind == ConditionKind::Preset) {
							FUCK::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "%s", title.c_str());
						} else if (Info(c.kind).group) {
							FUCK::TextColored(ImVec4(0.95f, 0.8f, 0.45f, 1.0f), "%s", title.c_str());
						} else {
							FUCK::Text("%s", title.c_str());
						}
						if (const auto summary = Summary(c); !summary.empty()) {
							FUCK::SameLine(0.0f, 24.0f);
							FUCK::TextDisabled("%s", summary.c_str());
						}
						++row;
						if (open) {
							DrawConditionBody(c, i, static_cast<int>(list.size()), op, at, row, live);
							FUCK::TreePop();
						} else {
							row = myRow + RowsOf(c);  // the closed subtree's rows still count
						}
						FUCK::EndTable();
					}
					FUCK::PopID();
				}
				switch (op) {
				case Op::Up:
					std::swap(list[at], list[at - 1]);
					changed_ = true;
					break;
				case Op::Down:
					std::swap(list[at], list[at + 1]);
					changed_ = true;
					break;
				case Op::Duplicate:
					list.insert(list.begin() + at + 1, list[at]);
					changed_ = true;
					break;
				case Op::PasteBelow:
					if (clipboard_) {
						list.insert(list.begin() + at + 1, *clipboard_);
						OpenNext(list, at + 1);
					}
					break;
				case Op::Delete:
					list.erase(list.begin() + at);
					changed_ = true;
					break;
				default:
					break;
				}
				AddButtons(list);
			}

			void DrawConditionBody(Condition& c, int i, int count, Op& op, int& at, int& row, const std::vector<std::int8_t>& live)
			{
				changed_ |= FUCK::Checkbox("$RDF_CO_Negate"_T, &c.negated);
				int kind = static_cast<int>(c.kind);
				if (FUCK::ComboWithFilter("$RDF_CO_ColKind"_T, &kind, KindNames().data(), static_cast<int>(KindNames().size()), 16)) {
					c.kind = static_cast<ConditionKind>(kind);
					lastKind_ = Info(c.kind).group ? lastKind_ : c.kind;
					if (Info(c.kind).group && c.children.empty()) {
						c.children.push_back(Condition{ .kind = lastKind_ });
					}
					changed_ = true;
				}
				FUCK::TextDisabledWrapped("%s", Describe(c.kind));
				if (HasSubject(c.kind)) {
					changed_ |= EnumCombo("$RDF_CO_ColWho"_T, c.subject, kSubjects, -1.0f);
				}
				DrawArguments(c);
				if (Info(c.kind).group) {
					FUCK::Spacing();
					DrawConditionList(c.children, row, live);
				}
				FUCK::Spacing();
				FUCK::BeginDisabled(i == 0);
				if (FUCK::Button(ICON_FA_ARROW_UP "##up")) {
					op = Op::Up;
					at = i;
				}
				FUCK::EndDisabled();
				FUCK::SameLine();
				FUCK::BeginDisabled(i + 1 >= count);
				if (FUCK::Button(ICON_FA_ARROW_DOWN "##down")) {
					op = Op::Down;
					at = i;
				}
				FUCK::EndDisabled();
				FUCK::SameLine();
				if (FUCK::Button(FUCK::IconText(ICON_FA_COPY, "$RDF_CO_Copy"_T))) {
					clipboard_ = c;
				}
				FUCK::SameLine();
				if (FUCK::Button(FUCK::IconText(ICON_FA_TRASH, "$RDF_CO_DeleteCondition"_T))) {
					op = Op::Delete;
					at = i;
				}
			}

			void DrawArguments(Condition& c)
			{
				const auto& info = Info(c.kind);
				if (info.form) {
					changed_ |= menu::FormInput(FormLabel(c.kind), c.form, FormTypeOf(c.kind));
				}
				switch (c.kind) {
				case ConditionKind::Layer:
					{
						static const auto names = Names(kLayers);
						changed_ |= TextCombo("$RDF_CO_Layer"_T, c.text, names, -1.0f);
						break;
					}
				case ConditionKind::Direction:
					{
						static const auto names = Names(kDirections);
						changed_ |= TextCombo("$RDF_CO_Direction"_T, c.text, names, -1.0f);
						break;
					}
				case ConditionKind::Equipped:
					{
						static const auto names = Names(kEquippedTypes);
						changed_ |= TextCombo("$RDF_CO_EquippedType"_T, c.text, names, -1.0f);
						break;
					}
				case ConditionKind::ActorValue:
					changed_ |= Text("$RDF_CO_AvName"_T, c.text, -1.0f);
					if (FUCK::IsItemHovered()) {
						FUCK::SetTooltip("$RDF_CO_AvHint"_T);
					}
					break;
				case ConditionKind::Preset:
					{
						const auto               presets = AllPresets();
						std::vector<std::string> keep;
						for (const auto& [name, list] : presets) {
							keep.push_back(name);
						}
						std::vector<const char*> names;
						for (const auto& n : keep) {
							names.push_back(n.c_str());
						}
						changed_ |= TextCombo("$RDF_CO_PresetRef"_T, c.text, names, -1.0f);
						break;
					}
				default:
					break;
				}
				if (info.hand) {
					changed_ |= EnumCombo("$RDF_CO_Hand"_T, c.hand, kHands, -1.0f);
				}
				if (info.mode) {
					changed_ |= EnumCombo("$RDF_CO_AvMode"_T, c.mode, kModes, -1.0f);
				}
				if (info.compare) {
					changed_ |= EnumCombo("$RDF_CO_Compare"_T, c.compare, kCompares, -1.0f);
					if (c.compare != Compare::None) {
						changed_ |= FUCK::DragFloat("$RDF_CO_Number"_T, &c.value, 0.1f, -1.0e6f, 1.0e6f, "%.2f");
					}
				}
				if (c.kind == ConditionKind::Random) {
					changed_ |= FUCK::DragFloat("$RDF_CO_ChanceValue"_T, &c.value, 0.5f, 0.0f, 100.0f, "%.0f");
				}
			}

			static const char* FormLabel(ConditionKind kind)
			{
				switch (kind) {
				case ConditionKind::HasPerk: return "$RDF_CO_Perk"_T;
				case ConditionKind::IsRace: return "$RDF_CO_Race"_T;
				case ConditionKind::InFaction: return "$RDF_CO_Faction"_T;
				default: return "$RDF_CO_Keyword"_T;
				}
			}

			// The header's dim summary, as OAR's argument column: the participant, then the arguments.
			static std::string Summary(const Condition& c)
			{
				const auto& info = Info(c.kind);
				if (info.group) {
					return std::format("{} {}", c.children.size(), "$RDF_CO_Children"_T);
				}
				std::string out;
				const auto  add = [&](std::string_view part) {
					if (!part.empty()) {
						out += out.empty() ? "" : "  ";
						out += part;
					}
				};
				if (HasSubject(c.kind)) {
					add(kSubjects[static_cast<int>(c.subject)]);
				}
				add(c.form);
				add(c.text);
				if (info.hand) {
					add(kHands[static_cast<int>(c.hand)]);
				}
				if (info.mode && c.mode != ValueMode::Current) {
					add(kModes[static_cast<int>(c.mode)]);
				}
				if (info.compare && c.compare != Compare::None) {
					add(std::format("{} {:g}", kCompares[static_cast<int>(c.compare)], c.value));
				}
				if (c.kind == ConditionKind::Random) {
					add(std::format("{:g} %", c.value));
				}
				return out;
			}

			// ---------------------------------------------------------------------------------------------- casts
			void DrawCasts(std::vector<Cast>& casts)
			{
				int remove = -1;
				for (int i = 0; i < static_cast<int>(casts.size()); ++i) {
					auto& cast = casts[i];
					FUCK::PushID(i);
					if (FUCK::BeginTable("##cast", 1, FUCK::TableFlags::kBordersOuterH | FUCK::TableFlags::kBordersOuterV)) {
						FUCK::TableNextRow();
						FUCK::TableNextColumn();
						if (openCast_ == i) {
							FUCK::SetNextItemOpen(true, 1 /* Always */);
							openCast_ = -1;
						}
						const bool open = FUCK::TreeNode("##open", kNodeFlags);
						FUCK::SameLine();
						const auto r = forms::Peek(cast.spell, RE::FormType::Spell);
						const auto name = cast.spell.empty() ? std::string("$RDF_CO_NoSpell"_T) : cast.spell;
						FUCK::TextColored(r.status == forms::Status::Found ? ImVec4(0.85f, 0.85f, 0.85f, 1.0f) : ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s %s",
							ICON_FA_WAND_MAGIC_SPARKLES, name.c_str());
						FUCK::SameLine(0.0f, 24.0f);
						FUCK::TextDisabled("%s > %s%s", kSubjects[static_cast<int>(cast.caster)], kTargets[static_cast<int>(cast.target)],
							cast.effectiveness != 1.0f ? std::format("  x{:g}", cast.effectiveness).c_str() : "");
						if (open) {
							changed_ |= menu::FormInput("$RDF_CO_ColSpell"_T, cast.spell, RE::FormType::Spell);
							changed_ |= EnumCombo("$RDF_CO_ColCaster"_T, cast.caster, kSubjects, -1.0f);
							changed_ |= EnumCombo("$RDF_CO_ColTarget"_T, cast.target, kTargets, -1.0f);
							changed_ |= FUCK::DragFloat("$RDF_CO_Effectiveness"_T, &cast.effectiveness, 0.01f, 0.0f, 100.0f, "%.2f");
							changed_ |= FUCK::DragFloat("$RDF_CO_Magnitude"_T, &cast.magnitude, 0.5f, 0.0f, 100000.0f, "%.1f");
							changed_ |= FUCK::DragFloat("$RDF_CO_Duration"_T, &cast.duration, 0.05f, 0.05f, 60.0f, "%.2f s");
							changed_ |= FUCK::Checkbox("$RDF_CO_NoArt"_T, &cast.noHitEffectArt);
							if (FUCK::Button(FUCK::IconText(ICON_FA_TRASH, "$RDF_CO_DeleteCast"_T))) {
								remove = i;
							}
							FUCK::TreePop();
						}
						FUCK::EndTable();
					}
					FUCK::PopID();
				}
				if (remove >= 0) {
					casts.erase(casts.begin() + remove);
					changed_ = true;
				}
				if (FUCK::Button(FUCK::IconText(ICON_FA_PLUS, "$RDF_CO_AddCast"_T))) {
					casts.push_back({});
					openCast_ = static_cast<int>(casts.size()) - 1;
					changed_ = true;
				}
				FUCK::TextDisabledWrapped("%s", "$RDF_CO_CastsNote"_T);
			}

			void DrawCost(Cost& cost)
			{
				changed_ |= EnumCombo("$RDF_CO_Mode"_T, cost.mode, kCostModes, -1.0f);
				switch (cost.mode) {
				case CostMode::Engine:
					changed_ |= FUCK::Checkbox("$RDF_CO_DualCast"_T, &cost.dualCast);
					changed_ |= FUCK::Checkbox("$RDF_CO_RealCast"_T, &cost.realCast);
					break;
				case CostMode::Custom:
					changed_ |= EnumCombo("$RDF_CO_Value"_T, cost.value, kCostValues, -1.0f);
					changed_ |= FUCK::DragFloat("$RDF_CO_Flat"_T, &cost.flat, 0.5f, 0.0f, 10000.0f, "%.1f");
					changed_ |= FUCK::DragFloat("$RDF_CO_SpellShare"_T, &cost.spellShare, 0.01f, 0.0f, 10.0f, "%.2f");
					changed_ |= FUCK::DragFloat("$RDF_CO_MaxShare"_T, &cost.maxShare, 0.005f, 0.0f, 1.0f, "%.3f");
					changed_ |= FUCK::DragFloat("$RDF_CO_Mult"_T, &cost.mult, 0.01f, 0.0f, 10.0f, "%.2f");
					break;
				default:
					break;
				}
				if (cost.mode != CostMode::Free) {
					changed_ |= EnumCombo("$RDF_CO_Shortfall"_T, cost.shortfall, kShortfalls, -1.0f);
					changed_ |= FUCK::Checkbox("$RDF_CO_Feedback"_T, &cost.feedback);
				}
			}

			std::atomic<bool>    open_{ false };
			bool                 loaded_{ false };
			bool                 watched_{ false };
			bool                 changed_{ false };
			std::vector<EditDoc> docs_;
			int                  doc_{ -1 };
			Selected             what_{ Selected::None };
			int                  rule_{ -1 };
			std::string          preset_;
			std::string          rename_;
			std::string          renameFor_;
			std::string          newName_;
			std::string          status_;
			std::optional<Condition>       clipboard_;
			ConditionKind                  lastKind_{ ConditionKind::InCombat };  // what "+ Condition" adds (OAR's way)
			const std::vector<Condition>*  openList_{ nullptr };                  // a node just added opens
			std::size_t                    openIndex_{ 0 };
			int                            openCast_{ -1 };
			static constexpr int           kNodeFlags = (1 << 2) | (1 << 10) | (1 << 11);  // AllowOverlap | FramePadding | SpanAvailWidth
		};

		Editor& Window()
		{
			static Editor editor;  // FLICK keeps the pointer: it lives as long as the plugin
			return editor;
		}
	}

	void Install()
	{
		FUCK::RegisterWindow(&Window());
	}

	void Open()
	{
		Window().SetOpen(true);
	}
}
