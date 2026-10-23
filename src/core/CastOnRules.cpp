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
		Direction, ChainIndex, FromStagger, Random);
};

namespace rdf::caston
{
	namespace
	{
		constexpr glz::opts kRead{ .comments = true, .error_on_unknown_keys = true };
		constexpr glz::opts kWrite{ .prettify = true };

		// name, form, text, hand, mode, compare, group: by ConditionKind order
		constexpr KindInfo kKinds[]{
			{ "And", false, false, false, false, false, true },
			{ "Or", false, false, false, false, false, true },
			{ "HasKeyword", true },
			{ "HasEffectKeyword", true, false, false, false, true },
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
		};
		static_assert(std::size(kKinds) == static_cast<std::size_t>(ConditionKind::Random) + 1);

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

	ParseResult ParseRules(std::string_view json)
	{
		ParseResult result;
		RuleFile    file;
		// glaze reads from a null-terminated buffer by default: copy the view into a string
		const std::string buffer(json);
		if (const auto error = glz::read<kRead>(file, buffer)) {
			result.errors.push_back(glz::format_error(error, buffer));
			return result;
		}
		for (std::size_t i = 0; i < file.rules.size(); ++i) {
			auto& rule = file.rules[i];
			if (const auto problems = Validate(rule); !problems.empty()) {
				for (const auto& p : problems) {
					result.errors.push_back(std::format("rules[{}] '{}': {}", i, rule.name, p));
				}
				continue;
			}
			result.rules.push_back(std::move(rule));
		}
		return result;
	}

	std::string WriteRules(const std::vector<Rule>& rules)
	{
		const RuleFile file{ 1, rules };
		std::string    out;
		if (const auto error = glz::write<kWrite>(file, out)) {
			return {};
		}
		return out;
	}
}
