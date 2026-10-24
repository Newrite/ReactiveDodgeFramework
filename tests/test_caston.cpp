#include "check.h"

#include "core/CastOnRules.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace rdf::caston;

namespace
{
	constexpr std::string_view kSample = R"({
		// a designer's comment
		"version": 1,
		"rules": [
			{
				"name": "Flames on a perfect dodge",
				"priority": 10,
				"trigger": "PerfectDodge",
				"conditions": [
					{ "kind": "HasKeyword", "form": "RDF_Perk_Flames" },
					{ "kind": "Or", "children": [
						{ "kind": "Layer", "text": "Magic" },
						{ "kind": "ActorValue", "subject": "Attacker", "text": "Health", "mode": "Percent", "compare": "Lt", "value": 50, "negated": true }
					] }
				],
				"casts": [ { "spell": "Skyrim.esm|0x12FCD", "caster": "Attacker", "target": "Dodger", "effectiveness": 0.5 } ],
				"cost": { "mode": "Engine", "dualCast": true },
				"timing": { "delay": 0.25, "tag": "late" },
				"limits": { "cooldown": 2.0, "chance": 50 }
			}
		]
	})";
}

TEST(caston_reads_a_rule_with_names_comments_and_nested_conditions)
{
	const auto r = ParseRules(kSample);
	CHECK(r.errors.empty());
	CHECK(r.rules.size() == 1);
	if (r.rules.size() != 1) {
		return;
	}
	const auto& rule = r.rules[0];
	CHECK(rule.name == "Flames on a perfect dodge");
	CHECK(rule.enabled);  // absent: the default
	CHECK(rule.priority == 10);
	CHECK(rule.trigger == Trigger::PerfectDodge);
	CHECK(rule.conditions.size() == 2);
	CHECK(rule.conditions[1].kind == ConditionKind::Or);
	CHECK(rule.conditions[1].children.size() == 2);
	const auto& av = rule.conditions[1].children[1];
	CHECK(av.subject == Subject::Attacker && av.mode == ValueMode::Percent && av.compare == Compare::Lt && av.value == 50.0f && av.negated);
	CHECK(rule.casts.size() == 1 && rule.casts[0].caster == Subject::Attacker && rule.casts[0].target == TargetKind::Dodger);
	CHECK(rule.casts[0].effectiveness == 0.5f);
	CHECK(rule.cost.mode == CostMode::Engine && rule.cost.dualCast);
	CHECK(rule.timing.delay == 0.25f && rule.timing.tag == "late" && rule.timing.annotation);
	CHECK(rule.limits.cooldown == 2.0f && rule.limits.chance == 50.0f);
}

TEST(caston_write_then_read_gives_the_same_rule)
{
	const auto first = ParseRules(kSample);
	CHECK(first.rules.size() == 1);
	if (first.rules.empty()) {
		return;
	}
	auto edited = first.rules;
	edited[0].enabled = false;           // a non-default false must survive the round trip
	edited[0].limits.chance = 0.0f;
	const auto text = WriteRules(edited);
	CHECK(!text.empty());
	CHECK(text.find("\"PerfectDodge\"") != std::string::npos);  // enums by name
	const auto again = ParseRules(text);
	CHECK(again.errors.empty());
	CHECK(again.rules.size() == 1);
	if (again.rules.size() == 1) {
		CHECK(!again.rules[0].enabled);
		CHECK(again.rules[0].limits.chance == 0.0f);
		CHECK(again.rules[0].conditions[1].children[1].compare == Compare::Lt);
		CHECK(again.rules[0].casts[0].spell == "Skyrim.esm|0x12FCD");
	}
}

TEST(caston_a_typo_in_a_key_is_an_error_not_silence)
{
	const auto r = ParseRules(R"({ "rules": [ { "name": "x", "contitions": [], "casts": [ { "spell": "a" } ] } ] })");
	CHECK(r.rules.empty());
	CHECK(r.errors.size() == 1);
	CHECK(!r.errors.empty() && r.errors[0].find("contitions") != std::string::npos);
}

TEST(caston_an_unknown_enum_name_is_an_error)
{
	const auto r = ParseRules(R"({ "rules": [ { "trigger": "OnSneeze", "casts": [ { "spell": "a" } ] } ] })");
	CHECK(r.rules.empty());
	CHECK(r.errors.size() == 1);
}

TEST(caston_invalid_rules_are_dropped_with_reasons_and_valid_ones_kept)
{
	const auto r = ParseRules(R"({ "rules": [
		{ "name": "no form", "conditions": [ { "kind": "HasKeyword" } ], "casts": [ { "spell": "a" } ] },
		{ "name": "bad layer", "conditions": [ { "kind": "Layer", "text": "Sideways" } ], "casts": [ { "spell": "a" } ] },
		{ "name": "no casts" },
		{ "name": "empty group", "conditions": [ { "kind": "And" } ], "casts": [ { "spell": "a" } ] },
		{ "name": "ok", "conditions": [ { "kind": "ChainIndex", "compare": "Ge", "value": 1 } ], "casts": [ { "spell": "a" } ] }
	] })");
	CHECK(r.rules.size() == 1);
	CHECK(!r.rules.empty() && r.rules[0].name == "ok");
	CHECK(r.errors.size() == 4);
	CHECK(r.errors.size() == 4 && r.errors[0].find("needs a form") != std::string::npos);
	CHECK(r.errors.size() == 4 && r.errors[1].find("Sideways") != std::string::npos);
}

TEST(caston_a_cast_duration_reads_and_is_checked)
{
	const auto r = ParseRules(R"({ "rules": [
		{ "name": "ok", "casts": [ { "spell": "a", "duration": 2.5 } ] },
		{ "name": "zero", "casts": [ { "spell": "a", "duration": 0 } ] },
		{ "name": "long", "casts": [ { "spell": "a", "duration": 120 } ] }
	] })");
	CHECK(r.rules.size() == 1 && r.rules[0].casts[0].duration == 2.5f);
	CHECK(r.errors.size() == 2);
	const auto d = ParseRules(R"({ "rules": [ { "casts": [ { "spell": "a" } ] } ] })");
	CHECK(d.rules.size() == 1 && d.rules[0].casts[0].duration == 1.0f);  // the default
}

TEST(caston_presets_are_read_and_expand_into_and_groups)
{
	const auto r = ParseRules(R"({
		"presets": {
			"Caster": [ { "kind": "Equipped", "text": "Spell" } ],
			"CasterInCombat": [ { "kind": "Preset", "text": "Caster" }, { "kind": "InCombat" } ]
		},
		"rules": [ { "name": "r", "conditions": [ { "kind": "Preset", "text": "CasterInCombat", "negated": true } ], "casts": [ { "spell": "a" } ] } ]
	})");
	CHECK(r.errors.empty());
	CHECK(r.presets.size() == 2);
	CHECK(r.rules.size() == 1);
	if (r.rules.size() != 1) {
		return;
	}
	auto                     conditions = r.rules[0].conditions;
	std::vector<std::string> errors;
	CHECK(conditions[0].kind == ConditionKind::Preset);  // kept as written until expanded
	CHECK(ExpandPresets(conditions, r.presets, errors));
	CHECK(conditions.size() == 1 && conditions[0].kind == ConditionKind::And && conditions[0].negated);
	CHECK(conditions[0].children.size() == 2);
	CHECK(conditions[0].children[0].kind == ConditionKind::And);  // the nested preset
	CHECK(conditions[0].children[0].children.size() == 1 && conditions[0].children[0].children[0].kind == ConditionKind::Equipped);
}

TEST(caston_unknown_and_circular_presets_fail_with_reasons)
{
	Presets presets;
	presets["A"] = { Condition{ .kind = ConditionKind::Preset, .text = "B" } };
	presets["B"] = { Condition{ .kind = ConditionKind::Preset, .text = "A" } };
	std::vector<Condition>   circle{ Condition{ .kind = ConditionKind::Preset, .text = "A" } };
	std::vector<std::string> errors;
	CHECK(!ExpandPresets(circle, presets, errors));
	CHECK(!errors.empty() && errors[0].find("name each other") != std::string::npos);
	std::vector<Condition> missing{ Condition{ .kind = ConditionKind::Preset, .text = "Nope" } };
	errors.clear();
	CHECK(!ExpandPresets(missing, presets, errors));
	CHECK(errors.size() == 1 && errors[0].find("Nope") != std::string::npos);
}

TEST(caston_templates_merge_objects_add_conditions_and_let_the_rule_win)
{
	const auto r = ParseRules(R"({
		"templates": {
			"Base": { "trigger": "PerfectDodge", "conditions": [ { "kind": "InCombat" } ],
			          "cost": { "mode": "Custom", "value": "Stamina", "flat": 10 }, "limits": { "cooldown": 2 } },
			"Hot": { "extends": "Base", "conditions": [ { "kind": "IsPlayer" } ], "casts": [ { "spell": "fire" } ] }
		},
		"rules": [
			{ "name": "a", "extends": "Hot", "conditions": [ { "kind": "Sneaking" } ], "cost": { "flat": 25 }, "priority": 3 },
			{ "name": "b", "extends": "Hot", "casts": [ { "spell": "ice" } ], "trigger": "Dodge" }
		]
	})");
	CHECK(r.errors.empty());
	for (const auto& e : r.errors) {
		std::printf("    %s\n", e.c_str());
	}
	CHECK(r.rules.size() == 2);
	if (r.rules.size() != 2) {
		return;
	}
	const auto& a = r.rules[0];
	CHECK(a.extends == "Hot");
	CHECK(a.trigger == Trigger::PerfectDodge);  // from Base through Hot
	CHECK(a.conditions.size() == 3);            // Base's, Hot's, then its own
	CHECK(a.conditions.size() == 3 && a.conditions[0].kind == ConditionKind::InCombat && a.conditions[2].kind == ConditionKind::Sneaking);
	CHECK(a.cost.mode == CostMode::Custom && a.cost.value == CostValue::Stamina && a.cost.flat == 25.0f);  // merged key by key
	CHECK(a.limits.cooldown == 2.0f && a.priority == 3);
	CHECK(a.casts.size() == 1 && a.casts[0].spell == "fire");
	const auto& b = r.rules[1];
	CHECK(b.trigger == Trigger::Dodge && b.casts.size() == 1 && b.casts[0].spell == "ice");  // replaced
	CHECK(b.conditions.size() == 2);
}

TEST(caston_template_problems_drop_only_their_rules)
{
	const auto r = ParseRules(R"({
		"templates": { "X": { "extends": "Y" }, "Y": { "extends": "X" } },
		"rules": [
			{ "name": "circle", "extends": "X", "casts": [ { "spell": "a" } ] },
			{ "name": "missing", "extends": "Z", "casts": [ { "spell": "a" } ] },
			{ "name": "typo in a template field", "extends": "T", "casts": [ { "spell": "a" } ] },
			{ "name": "fine", "casts": [ { "spell": "a" } ] }
		]
	})");
	CHECK(r.rules.size() == 1 && r.rules[0].name == "fine");
	CHECK(r.errors.size() == 3);
	CHECK(r.errors.size() == 3 && r.errors[0].find("extend each other") != std::string::npos);
	CHECK(r.errors.size() == 3 && r.errors[1].find("no template 'Z'") != std::string::npos);
}

TEST(caston_schema_key_is_accepted_and_unknown_top_keys_are_not)
{
	const auto ok = ParseRules(R"({ "$schema": "./rdf_caston.schema.json", "version": 1, "rules": [] })");
	CHECK(ok.errors.empty());
	const auto bad = ParseRules(R"({ "rulez": [] })");
	CHECK(bad.errors.size() == 1);
}

TEST(caston_write_keeps_presets)
{
	Presets presets;
	presets["Caster"] = { Condition{ .kind = ConditionKind::Equipped, .text = "Spell" } };
	Rule rule;
	rule.name = "r";
	rule.casts.push_back({ "a" });
	rule.conditions.push_back(Condition{ .kind = ConditionKind::Preset, .text = "Caster" });
	const auto text = WriteRules({ rule }, presets);
	const auto again = ParseRules(text);
	CHECK(again.errors.empty());
	for (const auto& e : again.errors) {
		std::printf("    %s\n%s\n", e.c_str(), text.substr(0, 300).c_str());
	}
	CHECK(again.presets.size() == 1 && again.rules.size() == 1);
	CHECK(again.rules.size() == 1 && again.rules[0].conditions[0].kind == ConditionKind::Preset);
}

TEST(caston_json_schema_names_the_condition_kinds)
{
	const auto schema = JsonSchema();
	CHECK(!schema.empty());
	CHECK(schema.find("HasKeyword") != std::string::npos);
	CHECK(schema.find("PerfectDodge") != std::string::npos);
	CHECK(schema.find("\"$schema\"") != std::string::npos);
}

TEST(caston_compare_holds)
{
	CHECK(Holds(Compare::Ge, 2.0f, 2.0f));
	CHECK(!Holds(Compare::Lt, 2.0f, 2.0f));
	CHECK(Holds(Compare::None, 0.0f, 5.0f));
	CHECK(Holds(Compare::Ne, 1.0f, 2.0f));
}

TEST(caston_the_shipped_examples_parse_and_validate)
{
	const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "dist" / "CastOn" / "RDF_Examples.json";
	std::ifstream in(path, std::ios::binary);
	CHECK(static_cast<bool>(in));
	const std::string json((std::istreambuf_iterator<char>(in)), {});
	const auto r = ParseRules(json);
	CHECK(r.errors.empty());
	for (const auto& e : r.errors) {
		std::printf("    %s\n", e.c_str());
	}
	CHECK(r.rules.size() == 3);
	for (const auto& rule : r.rules) {  // every Preset it names exists
		auto                     conditions = rule.conditions;
		std::vector<std::string> errors;
		CHECK(ExpandPresets(conditions, r.presets, errors));
	}
}

TEST(caston_disabled_conditions_drop_and_skip_validation)
{
	const auto r = ParseRules(R"({ "rules": [ { "name": "a", "casts": [ { "spell": "X" } ], "conditions": [
		{ "kind": "InCombat", "comment": "kept" },
		{ "kind": "HasPerk", "disabled": true },
		{ "kind": "Or", "children": [ { "kind": "Sneaking", "disabled": true }, { "kind": "Random", "value": 10, "disabled": true } ] },
		{ "kind": "Or", "children": [ { "kind": "Sneaking" }, { "kind": "Blocking", "disabled": true } ] } ] } ] })");
	CHECK(r.errors.empty());  // the switched-off HasPerk has no form: no error
	CHECK(r.rules.size() == 1);
	auto conditions = r.rules[0].conditions;
	CHECK(conditions.size() == 4);
	CHECK(conditions[0].comment == "kept");
	DropDisabled(conditions);
	CHECK(conditions.size() == 2);  // the HasPerk and the group with nothing left are gone
	CHECK(conditions[0].kind == ConditionKind::InCombat);
	CHECK(conditions[1].children.size() == 1);
	CHECK(conditions[1].children[0].kind == ConditionKind::Sneaking);
	// written back, read again: the flags and the comment survive
	const auto again = ParseRules(WriteRules(r.rules));
	CHECK(again.rules.size() == 1 && again.rules[0].conditions[1].disabled && again.rules[0].conditions[0].comment == "kept");
}

TEST(caston_vanilla_and_magic_effect_conditions)
{
	const auto r = ParseRules(R"({ "rules": [ { "name": "v", "casts": [ { "spell": "X" } ], "conditions": [
		{ "kind": "Vanilla", "text": "GetActorValue", "args": [ "Health" ], "compare": "Ge", "value": 50 },
		{ "kind": "HasMagicEffect", "form": "Skyrim.esm|0x1EA6E", "inactiveEffects": true } ] } ] })");
	CHECK(r.errors.empty());
	CHECK(r.rules.size() == 1);
	const auto& c = r.rules[0].conditions;
	CHECK(c[0].kind == ConditionKind::Vanilla && c[0].args.size() == 1 && c[0].args[0] == "Health");
	CHECK(c[1].kind == ConditionKind::HasMagicEffect && c[1].inactiveEffects);
	CHECK(Info(ConditionKind::HasMagicEffect).effects && Info(ConditionKind::Vanilla).effects && !Info(ConditionKind::IsRace).effects);
	const auto again = ParseRules(WriteRules(r.rules));
	CHECK(again.rules.size() == 1 && again.rules[0].conditions[0].args == c[0].args && again.rules[0].conditions[1].inactiveEffects);

	const auto bad = ParseRules(R"({ "rules": [ { "name": "b", "casts": [ { "spell": "X" } ], "conditions": [
		{ "kind": "Vanilla" }, { "kind": "Vanilla", "text": "GetDistance", "args": [ "a", "b", "c" ] }, { "kind": "HasMagicEffect" } ] } ] })");
	CHECK(bad.rules.empty());
	CHECK(bad.errors.size() == 3);  // no function, three args, no form
}

TEST(caston_effect_reading_round_trips)
{
	const auto r = ParseRules(R"({ "rules": [ { "name": "r", "casts": [ { "spell": "X" } ], "conditions": [
		{ "kind": "HasEffectKeyword", "form": "K", "compare": "Gt", "value": 10, "reading": "Applied" } ] } ] })");
	CHECK(r.errors.empty() && r.rules.size() == 1);
	CHECK(r.rules[0].conditions[0].reading == Reading::Applied);
	const auto again = ParseRules(WriteRules(r.rules));
	CHECK(again.rules.size() == 1 && again.rules[0].conditions[0].reading == Reading::Applied);
	CHECK(ParseRules(R"({ "rules": [ { "name": "r", "casts": [ { "spell": "X" } ], "conditions": [ { "kind": "InCombat" } ] } ] })").rules[0].conditions[0].reading == Reading::Authored);
}

TEST(caston_the_russian_guide_example_parses)
{
	const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "docs" / "ru" / "CastOn.md";
	std::ifstream in(path, std::ios::binary);
	CHECK(static_cast<bool>(in));
	const std::string text((std::istreambuf_iterator<char>(in)), {});
	const auto        from = text.find("```json\n");
	const auto        to = from == std::string::npos ? std::string::npos : text.find("```", from + 8);
	CHECK(from != std::string::npos && to != std::string::npos);
	const auto r = ParseRules(std::string_view(text).substr(from + 8, to - from - 8));
	for (const auto& e : r.errors) {
		std::printf("    %s\n", e.c_str());
	}
	CHECK(r.errors.empty() && r.rules.size() == 1 && r.presets.size() == 1);
}
