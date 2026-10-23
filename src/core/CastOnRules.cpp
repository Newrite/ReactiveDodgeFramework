#include "CastOnRules.h"

#include <glaze/json.hpp>

#include <algorithm>
#include <format>

// Enums are written by name: glaze maps them through these (the order is the value order).
template <>
struct glz::meta<rdf::caston::Trigger>
{
	using enum rdf::caston::Trigger;
	static constexpr auto value = glz::enumerate(Dodge, PerfectDodge);
};
template <>
struct glz::meta<rdf::caston::Subject>
{
	using enum rdf::caston::Subject;
	static constexpr auto value = glz::enumerate(Dodger, Attacker, CombatTarget);
};
template <>
struct glz::meta<rdf::caston::TargetKind>
{
	using enum rdf::caston::TargetKind;
	static constexpr auto value = glz::enumerate(Self, Dodger, Attacker, CombatTarget, Aim);
};
template <>
struct glz::meta<rdf::caston::CostMode>
{
	using enum rdf::caston::CostMode;
	static constexpr auto value = glz::enumerate(Free, Engine, Custom);
};
template <>
struct glz::meta<rdf::caston::CostValue>
{
	using enum rdf::caston::CostValue;
	static constexpr auto value = glz::enumerate(Magicka, Stamina, Health);
};
template <>
struct glz::meta<rdf::caston::Shortfall>
{
	using enum rdf::caston::Shortfall;
	static constexpr auto value = glz::enumerate(Skip, CastAnyway, Scale);
};
template <>
struct glz::meta<rdf::caston::Compare>
{
	using enum rdf::caston::Compare;
	static constexpr auto value = glz::enumerate(None, Eq, Ne, Lt, Le, Gt, Ge);
};
template <>
struct glz::meta<rdf::caston::Hand>
{
	using enum rdf::caston::Hand;
	static constexpr auto value = glz::enumerate(Right, Left, Either);
};
template <>
struct glz::meta<rdf::caston::ValueMode>
{
	using enum rdf::caston::ValueMode;
	static constexpr auto value = glz::enumerate(Current, Max, Percent, Base);
};
template <>
struct glz::meta<rdf::caston::ConditionKind>
{
	using enum rdf::caston::ConditionKind;
	static constexpr auto value = glz::enumerate(And, Or, HasKeyword, HasEffectKeyword, HasPerk, IsRace, InFaction, Equipped,
		ActorValue, InCombat, Sneaking, Sprinting, Blocking, Attacking, PowerAttacking, Casting, IsPlayer, FirstPerson, Layer,
		Direction, ChainIndex, FromStagger, Random, Preset, HasMagicEffect, Vanilla);
};

namespace rdf::caston::detail
{
	// What a file holds, for writing and for the schema (reading goes through glz::generic: templates merge first).
	struct FileOut
	{
		std::optional<std::string>          schema;  // "$schema"; omitted when unset
		int                                 version{ 1 };
		Presets                             presets;
		std::map<std::string, glz::generic> templates;
		std::vector<Rule>                   rules;
	};
}

template <>
struct glz::meta<rdf::caston::detail::FileOut>
{
	using T = rdf::caston::detail::FileOut;
	static constexpr auto value = glz::object("$schema", &T::schema, "version", &T::version, "presets", &T::presets,
		"templates", &T::templates, "rules", &T::rules);
};

// Descriptions for editors (JsonSchema).
template <>
struct glz::json_schema<rdf::caston::Condition>
{
	schema kind{ .description = "What is checked. And / Or group 'children'; Preset names a list from 'presets' in 'text'." };
	schema negated{ .description = "The opposite. A condition on a participant that is absent fails, negated or not." };
	schema subject{ .description = "Whose: Dodger, Attacker (PerfectDodge only), CombatTarget." };
	schema form{ .description = "Keyword / perk / race / faction: \"Plugin.esp|0xID\" or an EditorID." };
	schema text{ .description = "Equipped type, actor value name, Layer, Direction, the Preset name, or the Vanilla condition function (GetActorValue...)." };
	schema compare{ .description = "None, Eq, Ne, Lt, Le, Gt, Ge (against 'value')." };
	schema args{ .description = "Vanilla: the function's parameters as text: a form (\"Plugin.esp|0xID\" or an EditorID), a number, an actor value name, or Dodger / Attacker / CombatTarget for a reference." };
	schema inactiveEffects{ .description = "Also count magic effects switched off by their spell's conditions, or dispelled (the engine's own functions do)." };
	schema disabled{ .description = "Switched off: the condition (a group with all inside) is as if not written." };
	schema comment{ .description = "A note for people; the editor shows it on the condition." };
};
template <>
struct glz::json_schema<rdf::caston::Cast>
{
	schema spell{ .description = "Spell, scroll, enchantment or potion: \"Plugin.esp|0xID\" or an EditorID." };
	schema caster{ .description = "Who casts: Dodger, Attacker, CombatTarget (Attacker casting on the Dodger = Reflyem's reverse cast)." };
	schema target{ .description = "Self, Dodger, Attacker, CombatTarget, or Aim (no target; an aimed spell flies ahead)." };
	schema duration{ .description = "Seconds a concentration spell runs (others ignore it).", .minimum = 0.0, .maximum = 60.0 };
};
template <>
struct glz::json_schema<rdf::caston::Rule>
{
	schema extends{ .description = "A template of this file's 'templates' to start from." };
	schema trigger{ .description = "Dodge or PerfectDodge." };
	schema priority{ .description = "Higher first; 'exclusive' stops the lower ones of the trigger when this one fires." };
};

namespace rdf::caston
{
	namespace
	{
		constexpr glz::opts kRead{ .comments = true, .error_on_unknown_keys = true };
		constexpr glz::opts kWrite{ .prettify = true };

		// name, form, text, hand, mode, compare, group, effects: by ConditionKind order
		constexpr KindInfo kKinds[]{
			{ "And", false, false, false, false, false, true },
			{ "Or", false, false, false, false, false, true },
			{ "HasKeyword", true, false, false, false, false, false, true },
			{ "HasEffectKeyword", true, false, false, false, true, false, true },
			{ "HasPerk", true },
			{ "IsRace", true },
			{ "InFaction", true, false, false, false, true },
			{ "Equipped", false, true, true },
			{ "ActorValue", false, true, false, true, true },
			{ "InCombat" },
			{ "Sneaking" },
			{ "Sprinting" },
			{ "Blocking" },
			{ "Attacking" },
			{ "PowerAttacking" },
			{ "Casting", false, false, true },
			{ "IsPlayer" },
			{ "FirstPerson" },
			{ "Layer", false, true },
			{ "Direction", false, true },
			{ "ChainIndex", false, false, false, false, true },
			{ "FromStagger" },
			{ "Random", false, false, false, false, false },
			{ "Preset", false, true },
			{ "HasMagicEffect", true, false, false, false, true, false, true },
			{ "Vanilla", false, true, false, false, true, false, true },
		};
		static_assert(std::size(kKinds) == static_cast<std::size_t>(ConditionKind::Vanilla) + 1);

		template <std::size_t N>
		bool OneOf(const std::string& text, const std::string_view (&list)[N])
		{
			return std::find(std::begin(list), std::end(list), text) != std::end(list);
		}

		template <std::size_t N>
		std::string ListOf(const std::string_view (&list)[N])
		{
			std::string out;
			for (const auto& s : list) {
				out += out.empty() ? "" : ", ";
				out += s;
			}
			return out;
		}

		void ValidateCondition(const Condition& c, const std::string& path, std::vector<std::string>& errors)
		{
			const auto& info = Info(c.kind);
			const auto  fail = [&](std::string what) { errors.push_back(std::format("{} ({}): {}", path, info.name, what)); };
			if (c.disabled) {
				return;  // switched off: a half-made condition does not stop the rule
			}
			if (info.group) {
				if (c.children.empty()) {
					fail("an empty group");
				}
				for (std::size_t i = 0; i < c.children.size(); ++i) {
					ValidateCondition(c.children[i], std::format("{}.children[{}]", path, i), errors);
				}
				return;
			}
			if (!c.children.empty()) {
				fail("children on a condition that is not a group");
			}
			if (info.form && c.form.empty()) {
				fail("needs a form");
			}
			switch (c.kind) {
			case ConditionKind::Equipped:
				if (!OneOf(c.text, kEquippedTypes)) {
					fail(std::format("text '{}' is not one of {}", c.text, ListOf(kEquippedTypes)));
				}
				break;
			case ConditionKind::Layer:
				if (!OneOf(c.text, kLayers)) {
					fail(std::format("text '{}' is not one of {}", c.text, ListOf(kLayers)));
				}
				break;
			case ConditionKind::Direction:
				if (!OneOf(c.text, kDirections)) {
					fail(std::format("text '{}' is not one of {}", c.text, ListOf(kDirections)));
				}
				break;
			case ConditionKind::ActorValue:
				if (c.text.empty()) {
					fail("needs the actor value's name in text");
				}
				[[fallthrough]];
			case ConditionKind::ChainIndex:
				if (c.compare == Compare::None) {
					fail("needs a compare");
				}
				break;
			case ConditionKind::Random:
				if (c.value < 0.0f || c.value > 100.0f) {
					fail("value is a chance in percent, 0..100");
				}
				break;
			case ConditionKind::Preset:
				if (c.text.empty()) {
					fail("needs the preset's name in text");
				}
				break;
			case ConditionKind::Vanilla:
				if (c.text.empty()) {
					fail("needs the condition function's name in text");
				}
				if (c.args.size() > 2) {
					fail("a condition function takes at most 2 args");
				}
				break;
			default:
				break;
			}
		}
	}

	const KindInfo& Info(ConditionKind kind)
	{
		return kKinds[static_cast<std::size_t>(kind)];
	}

	bool Holds(Compare c, float lhs, float rhs)
	{
		switch (c) {
		case Compare::Eq: return lhs == rhs;
		case Compare::Ne: return lhs != rhs;
		case Compare::Lt: return lhs < rhs;
		case Compare::Le: return lhs <= rhs;
		case Compare::Gt: return lhs > rhs;
		case Compare::Ge: return lhs >= rhs;
		default: return true;
		}
	}

	std::vector<std::string> Validate(const Rule& rule)
	{
		std::vector<std::string> errors;
		for (std::size_t i = 0; i < rule.conditions.size(); ++i) {
			ValidateCondition(rule.conditions[i], std::format("conditions[{}]", i), errors);
		}
		if (rule.casts.empty()) {
			errors.push_back("no casts");
		}
		for (std::size_t i = 0; i < rule.casts.size(); ++i) {
			const auto& cast = rule.casts[i];
			if (cast.spell.empty()) {
				errors.push_back(std::format("casts[{}]: no spell", i));
			}
			if (cast.effectiveness < 0.0f) {
				errors.push_back(std::format("casts[{}]: negative effectiveness", i));
			}
			if (!(cast.duration > 0.0f) || cast.duration > 60.0f) {
				errors.push_back(std::format("casts[{}]: duration is seconds, above 0 and up to 60", i));
			}
		}
		if (rule.timing.delay < 0.0f) {
			errors.push_back("timing.delay is negative");
		}
		if (rule.limits.cooldown < 0.0f) {
			errors.push_back("limits.cooldown is negative");
		}
		if (rule.limits.chance < 0.0f || rule.limits.chance > 100.0f) {
			errors.push_back("limits.chance is a percent, 0..100");
		}
		return errors;
	}

	namespace
	{
		using Object = glz::generic::object_t;
		using Array = glz::generic::array_t;

		const Object* AsObject(const glz::generic& g) { return std::get_if<Object>(&g.data); }
		const std::string* AsString(const glz::generic& g) { return std::get_if<std::string>(&g.data); }

		// `over` on top of `base`: keys replace, objects merge key by key, "conditions" arrays add up (base first).
		glz::generic Merge(const glz::generic& base, const glz::generic& over)
		{
			const auto* b = AsObject(base);
			const auto* o = AsObject(over);
			if (!b || !o) {
				return over;
			}
			Object out = *b;
			for (const auto& [key, value] : *o) {
				const auto it = out.find(key);
				if (it == out.end()) {
					out.emplace(key, value);
					continue;
				}
				const auto* baseArray = std::get_if<Array>(&it->second.data);
				const auto* overArray = std::get_if<Array>(&value.data);
				if (key == "conditions" && baseArray && overArray) {
					Array both = *baseArray;
					both.insert(both.end(), overArray->begin(), overArray->end());
					it->second.data = std::move(both);
				} else if (AsObject(it->second) && AsObject(value)) {
					it->second = Merge(it->second, value);
				} else {
					it->second = value;
				}
			}
			glz::generic result;
			result.data = std::move(out);
			return result;
		}

		// A template with its own "extends" chain applied; nullopt with `error` for a missing one or a circle.
		std::optional<glz::generic> Resolve(const Object& templates, const std::string& name, std::vector<std::string>& chain,
			std::string& error)
		{
			if (std::find(chain.begin(), chain.end(), name) != chain.end()) {
				std::string path;
				for (const auto& n : chain) {
					path += n + " -> ";
				}
				error = std::format("templates extend each other: {}{}", path, name);
				return std::nullopt;
			}
			const auto it = templates.find(name);
			if (it == templates.end() || !AsObject(it->second)) {
				error = std::format("no template '{}'", name);
				return std::nullopt;
			}
			const auto* t = AsObject(it->second);
			const auto  parent = t->find("extends");
			if (parent == t->end()) {
				return it->second;
			}
			const auto* parentName = AsString(parent->second);
			if (!parentName) {
				error = std::format("template '{}': extends is not a name", name);
				return std::nullopt;
			}
			chain.push_back(name);
			auto base = Resolve(templates, *parentName, chain, error);
			chain.pop_back();
			if (!base) {
				return std::nullopt;
			}
			return Merge(*base, it->second);
		}

		template <class T>
		std::string ReadPart(T& out, const glz::generic& value)
		{
			std::string text;
			if (glz::write_json(value, text)) {
				return "cannot re-read the value";
			}
			if (const auto error = glz::read<kRead>(out, text)) {
				return glz::format_error(error, text);
			}
			return {};
		}

		bool Expand(std::vector<Condition>& list, const Presets& presets, std::vector<std::string>& chain, std::vector<std::string>& errors)
		{
			bool ok = true;
			for (auto& c : list) {
				if (c.kind != ConditionKind::Preset) {
					if (Info(c.kind).group) {
						ok = Expand(c.children, presets, chain, errors) && ok;
					}
					continue;
				}
				if (std::find(chain.begin(), chain.end(), c.text) != chain.end()) {
					errors.push_back(std::format("presets name each other: {} -> {}", chain.empty() ? std::string() : chain.back(), c.text));
					ok = false;
					continue;
				}
				const auto it = presets.find(c.text);
				if (it == presets.end()) {
					errors.push_back(std::format("no preset '{}'", c.text));
					ok = false;
					continue;
				}
				Condition group;
				group.kind = ConditionKind::And;
				group.negated = c.negated;
				group.children = it->second;
				chain.push_back(c.text);
				ok = Expand(group.children, presets, chain, errors) && ok;
				chain.pop_back();
				c = std::move(group);
			}
			return ok;
		}
	}

	bool ExpandPresets(std::vector<Condition>& conditions, const Presets& presets, std::vector<std::string>& errors)
	{
		std::vector<std::string> chain;
		return Expand(conditions, presets, chain, errors);
	}

	void DropDisabled(std::vector<Condition>& conditions)
	{
		std::erase_if(conditions, [](Condition& c) {
			if (c.disabled) {
				return true;
			}
			if (!Info(c.kind).group) {
				return false;
			}
			DropDisabled(c.children);
			return c.children.empty();
		});
	}

	ParseResult ParseRules(std::string_view json)
	{
		ParseResult result;
		// glaze reads from a null-terminated buffer by default: copy the view into a string
		const std::string buffer(json);
		glz::generic      doc;
		if (const auto error = glz::read<kRead>(doc, buffer)) {
			result.errors.push_back(glz::format_error(error, buffer));
			return result;
		}
		const auto* top = AsObject(doc);
		if (!top) {
			result.errors.push_back("the file is not a JSON object");
			return result;
		}
		for (const auto& [key, value] : *top) {
			if (key != "$schema" && key != "version" && key != "presets" && key != "templates" && key != "rules") {
				result.errors.push_back(std::format("unknown key '{}' (expected $schema, version, presets, templates, rules)", key));
			}
		}
		if (const auto it = top->find("presets"); it != top->end()) {
			if (const auto error = ReadPart(result.presets, it->second); !error.empty()) {
				result.errors.push_back("presets: " + error);
				result.presets.clear();
			}
			for (const auto& [name, list] : result.presets) {
				Rule probe;  // a preset's conditions get the rule checks
				probe.conditions = list;
				probe.casts.push_back({ "-" });
				for (const auto& p : Validate(probe)) {
					result.errors.push_back(std::format("presets['{}']: {}", name, p));
				}
			}
		}
		static const Object kNoTemplates;
		const Object*       templates = &kNoTemplates;
		if (const auto it = top->find("templates"); it != top->end()) {
			templates = AsObject(it->second);
			if (!templates) {
				result.errors.push_back("templates: not an object of named templates");
				templates = &kNoTemplates;
			}
		}
		const auto rulesIt = top->find("rules");
		const auto* rules = rulesIt == top->end() ? nullptr : std::get_if<Array>(&rulesIt->second.data);
		if (rulesIt != top->end() && !rules) {
			result.errors.push_back("rules: not an array");
		}
		for (std::size_t i = 0; rules && i < rules->size(); ++i) {
			const auto& raw = (*rules)[i];
			const auto* object = AsObject(raw);
			std::string name;
			if (object) {
				if (const auto n = object->find("name"); n != object->end() && AsString(n->second)) {
					name = *AsString(n->second);
				}
			}
			const auto fail = [&](const std::string& what) { result.errors.push_back(std::format("rules[{}] '{}': {}", i, name, what)); };
			glz::generic merged = raw;
			if (object) {
				if (const auto e = object->find("extends"); e != object->end() && !(AsString(e->second) && AsString(e->second)->empty())) {
					const auto* templateName = AsString(e->second);
					if (!templateName) {
						fail("extends is not a name");
						continue;
					}
					std::vector<std::string> chain;
					std::string              error;
					const auto               base = Resolve(*templates, *templateName, chain, error);
					if (!base) {
						fail(error);
						continue;
					}
					merged = Merge(*base, raw);
				}
			}
			Rule rule;
			if (const auto error = ReadPart(rule, merged); !error.empty()) {
				fail(error);
				continue;
			}
			if (const auto problems = Validate(rule); !problems.empty()) {
				for (const auto& p : problems) {
					fail(p);
				}
				continue;
			}
			result.rules.push_back(std::move(rule));
		}
		return result;
	}

	std::string WriteRules(const std::vector<Rule>& rules, const Presets& presets)
	{
		detail::FileOut file;
		file.presets = presets;
		file.rules = rules;
		for (auto& rule : file.rules) {
			rule.extends.clear();  // a rule read from a template holds the merged values: written whole, not on top of it again
		}
		std::string out;
		if (const auto error = glz::write<kWrite>(file, out)) {
			return {};
		}
		return out;
	}

	std::string JsonSchema()
	{
		auto schema = glz::write_json_schema<detail::FileOut, glz::opts{ .prettify = true }>();
		return schema ? *schema : std::string();
	}
}
