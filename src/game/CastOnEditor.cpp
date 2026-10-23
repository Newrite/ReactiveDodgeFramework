#include "CastOnEditor.h"

#include "CastOn.h"
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
					FUCK::TableSetupColumn("##list", FUCK::TableColumnFlags::kWidthFixed, 300.0f);
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
			// One table for the whole tree, rows in depth-first order (the order of the live results).
			void DrawConditionTable(std::vector<Condition>& list)
			{
				const auto live = co::LiveResults();
				int        row = 0;
				if (FUCK::BeginTable("##conditions", 7, FUCK::TableFlags::kBordersInnerH | FUCK::TableFlags::kRowBg)) {
					FUCK::TableSetupColumn("##live", FUCK::TableColumnFlags::kWidthFixed, 24.0f);
					FUCK::TableSetupColumn("$RDF_CO_ColNot"_T, FUCK::TableColumnFlags::kWidthFixed, 48.0f);
					FUCK::TableSetupColumn("$RDF_CO_ColKind"_T, FUCK::TableColumnFlags::kWidthStretch, 1.3f);
					FUCK::TableSetupColumn("$RDF_CO_ColWho"_T, FUCK::TableColumnFlags::kWidthFixed, 130.0f);
					FUCK::TableSetupColumn("$RDF_CO_ColWhat"_T, FUCK::TableColumnFlags::kWidthStretch, 2.4f);
					FUCK::TableSetupColumn("$RDF_CO_ColHow"_T, FUCK::TableColumnFlags::kWidthStretch, 1.6f);
					FUCK::TableSetupColumn("##actions", FUCK::TableColumnFlags::kWidthFixed, 100.0f);
					FUCK::TableHeadersRow();
					DrawRows(list, 0, row, live.results);
					FUCK::EndTable();
				}
				AddButtons(list);
			}

			void AddButtons(std::vector<Condition>& list)
			{
				if (FUCK::Button(FUCK::IconText(ICON_FA_PLUS, "$RDF_CO_AddCondition"_T))) {
					list.push_back(Condition{ .kind = ConditionKind::InCombat });
					changed_ = true;
				}
				FUCK::SameLine();
				if (FUCK::Button(FUCK::IconText(ICON_FA_FOLDER_PLUS, "$RDF_CO_AddGroup"_T))) {
					list.push_back(Condition{ .kind = ConditionKind::Or, .children = { Condition{ .kind = ConditionKind::InCombat } } });
					changed_ = true;
				}
			}

			void DrawRows(std::vector<Condition>& list, int depth, int& row, const std::vector<std::int8_t>& live)
			{
				int up = -1, down = -1, remove = -1;
				for (int i = 0; i < static_cast<int>(list.size()); ++i) {
					auto& c = list[i];
					FUCK::PushID(i);
					FUCK::TableNextRow();
					FUCK::TableNextColumn();
					LiveIcon(row < static_cast<int>(live.size()) ? live[row] : -2);
					++row;
					FUCK::TableNextColumn();
					changed_ |= FUCK::Checkbox("##not", &c.negated, false, false);
					FUCK::TableNextColumn();
					if (depth > 0) {
						FUCK::Indent(14.0f * depth);
					}
					int kind = static_cast<int>(c.kind);
					FUCK::SetNextItemWidth(-1.0f);
					if (FUCK::Combo("##kind", &kind, KindNames().data(), static_cast<int>(KindNames().size()))) {
						c.kind = static_cast<ConditionKind>(kind);
						if (Info(c.kind).group && c.children.empty()) {
							c.children.push_back(Condition{ .kind = ConditionKind::InCombat });
						}
						changed_ = true;
					}
					if (depth > 0) {
						FUCK::Unindent(14.0f * depth);
					}
					FUCK::TableNextColumn();
					if (HasSubject(c.kind)) {
						changed_ |= EnumCombo("##who", c.subject, kSubjects, -1.0f);
					}
					FUCK::TableNextColumn();
					DrawWhat(c);
					FUCK::TableNextColumn();
					DrawHow(c);
					FUCK::TableNextColumn();
					if (FUCK::Button(ICON_FA_ARROW_UP "##up") && i > 0) {
						up = i;
					}
					FUCK::SameLine();
					if (FUCK::Button(ICON_FA_ARROW_DOWN "##down") && i + 1 < static_cast<int>(list.size())) {
						down = i;
					}
					FUCK::SameLine();
					if (FUCK::Button(ICON_FA_TRASH "##del")) {
						remove = i;
					}
					if (Info(c.kind).group) {
						DrawRows(c.children, depth + 1, row, live);
					}
					FUCK::PopID();
				}
				if (up > 0) {
					std::swap(list[up], list[up - 1]);
					changed_ = true;
				} else if (down >= 0) {
					std::swap(list[down], list[down + 1]);
					changed_ = true;
				} else if (remove >= 0) {
					list.erase(list.begin() + remove);
					changed_ = true;
				}
			}

			void DrawWhat(Condition& c)
			{
				const auto& info = Info(c.kind);
				if (info.group) {
					if (FUCK::Button(FUCK::IconText(ICON_FA_PLUS, "$RDF_CO_AddCondition"_T))) {
						c.children.push_back(Condition{ .kind = ConditionKind::InCombat });
						changed_ = true;
					}
					FUCK::SameLine();
					if (FUCK::Button(FUCK::IconText(ICON_FA_FOLDER_PLUS, "$RDF_CO_AddGroup"_T))) {
						c.children.push_back(Condition{ .kind = ConditionKind::And, .children = { Condition{ .kind = ConditionKind::InCombat } } });
						changed_ = true;
					}
					return;
				}
				if (info.form) {
					changed_ |= menu::FormInput("##form", c.form, FormTypeOf(c.kind), true);
					return;
				}
				switch (c.kind) {
				case ConditionKind::Layer:
					{
						static const auto names = Names(kLayers);
						changed_ |= TextCombo("##text", c.text, names, -1.0f);
						return;
					}
				case ConditionKind::Direction:
					{
						static const auto names = Names(kDirections);
						changed_ |= TextCombo("##text", c.text, names, -1.0f);
						return;
					}
				case ConditionKind::Equipped:
					{
						static const auto names = Names(kEquippedTypes);
						changed_ |= TextCombo("##text", c.text, names, -1.0f);
						return;
					}
				case ConditionKind::ActorValue:
					changed_ |= Text("##text", c.text, -1.0f);
					if (FUCK::IsItemHovered()) {
						FUCK::SetTooltip("$RDF_CO_AvHint"_T);
					}
					return;
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
						changed_ |= TextCombo("##text", c.text, names, -1.0f);
						return;
					}
				default:
					FUCK::TextDisabled("-");
					return;
				}
			}

			void DrawHow(Condition& c)
			{
				const auto& info = Info(c.kind);
				bool        first = true;
				const auto  next = [&]() {
					if (!first) {
						FUCK::SameLine();
					}
					first = false;
				};
				if (info.hand) {
					next();
					changed_ |= EnumCombo("##hand", c.hand, kHands, 80.0f);
				}
				if (info.mode) {
					next();
					changed_ |= EnumCombo("##mode", c.mode, kModes, 90.0f);
				}
				if (info.compare) {
					next();
					changed_ |= EnumCombo("##cmp", c.compare, kCompares, 56.0f);
					if (c.compare != Compare::None) {
						FUCK::SameLine();
						FUCK::SetNextItemWidth(-1.0f);
						changed_ |= FUCK::DragFloat("##value", &c.value, 0.1f, -1.0e6f, 1.0e6f, "%.2f");
					}
				}
				if (c.kind == ConditionKind::Random) {
					next();
					FUCK::SetNextItemWidth(-1.0f);
					changed_ |= FUCK::DragFloat("##chance", &c.value, 0.5f, 0.0f, 100.0f, "%.0f");
				}
			}

			// ---------------------------------------------------------------------------------------------- casts, cost
			void DrawCasts(std::vector<Cast>& casts)
			{
				int remove = -1;
				if (FUCK::BeginTable("##casts", 8, FUCK::TableFlags::kBordersInnerH | FUCK::TableFlags::kRowBg)) {
					FUCK::TableSetupColumn("$RDF_CO_ColSpell"_T, FUCK::TableColumnFlags::kWidthStretch, 2.6f);
					FUCK::TableSetupColumn("$RDF_CO_ColCaster"_T, FUCK::TableColumnFlags::kWidthFixed, 130.0f);
					FUCK::TableSetupColumn("$RDF_CO_ColTarget"_T, FUCK::TableColumnFlags::kWidthFixed, 130.0f);
					FUCK::TableSetupColumn("$RDF_CO_ColEff"_T, FUCK::TableColumnFlags::kWidthFixed, 80.0f);
					FUCK::TableSetupColumn("$RDF_CO_ColMag"_T, FUCK::TableColumnFlags::kWidthFixed, 80.0f);
					FUCK::TableSetupColumn("$RDF_CO_ColDur"_T, FUCK::TableColumnFlags::kWidthFixed, 80.0f);
					FUCK::TableSetupColumn("$RDF_CO_ColNoArt"_T, FUCK::TableColumnFlags::kWidthFixed, 70.0f);
					FUCK::TableSetupColumn("##del", FUCK::TableColumnFlags::kWidthFixed, 36.0f);
					FUCK::TableHeadersRow();
					for (int i = 0; i < static_cast<int>(casts.size()); ++i) {
						auto& cast = casts[i];
						FUCK::PushID(i);
						FUCK::TableNextRow();
						FUCK::TableNextColumn();
						changed_ |= menu::FormInput("##spell", cast.spell, RE::FormType::Spell, true);
						FUCK::TableNextColumn();
						changed_ |= EnumCombo("##caster", cast.caster, kSubjects, -1.0f);
						FUCK::TableNextColumn();
						changed_ |= EnumCombo("##target", cast.target, kTargets, -1.0f);
						FUCK::TableNextColumn();
						FUCK::SetNextItemWidth(-1.0f);
						changed_ |= FUCK::DragFloat("##eff", &cast.effectiveness, 0.01f, 0.0f, 100.0f, "%.2f");
						FUCK::TableNextColumn();
						FUCK::SetNextItemWidth(-1.0f);
						changed_ |= FUCK::DragFloat("##mag", &cast.magnitude, 0.5f, 0.0f, 100000.0f, "%.1f");
						FUCK::TableNextColumn();
						FUCK::SetNextItemWidth(-1.0f);
						changed_ |= FUCK::DragFloat("##dur", &cast.duration, 0.05f, 0.05f, 60.0f, "%.2f s");
						FUCK::TableNextColumn();
						changed_ |= FUCK::Checkbox("##noart", &cast.noHitEffectArt, false, false);
						FUCK::TableNextColumn();
						if (FUCK::Button(ICON_FA_TRASH "##del")) {
							remove = i;
						}
						FUCK::PopID();
					}
					FUCK::EndTable();
				}
				if (remove >= 0) {
					casts.erase(casts.begin() + remove);
					changed_ = true;
				}
				if (FUCK::Button(FUCK::IconText(ICON_FA_PLUS, "$RDF_CO_AddCast"_T))) {
					casts.push_back({});
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
