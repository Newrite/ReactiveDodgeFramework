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
}
