#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The game's own condition functions (the CK's list) for CastOn's Vanilla condition. They are read from the script
// command table (SCRIPT_FUNCTION, 501789 = 0x141DB8910 on SE: 0x50 bytes a command, opcode 0x1000 + the CTDA function
// index), so nothing is hardcoded: a command with a condition function (+0x40) is one. TESConditionItem::IsTrue (29090 =
// 0x1404454C0) calls it as fn(subject, param1, param2, double& result) and compares the result; we call it the same way
// and compare with the rule's own Compare (verified in IDA, docs/CASTON_DESIGN.md 14).
namespace rdf::game::vanilla
{
	// How a parameter is written and stored (the void* slot the function reads).
	enum class Arg
	{
		Unsupported,    // aliases, package data, script variables...: not offered
		Form,           // a form of `formType` (None: any)
		Reference,      // a placed reference, or Dodger / Attacker / CombatTarget
		ActorValue,     // a name as the console knows it
		Integer,        // a number (quest stage, crime type, rank...)
		Float,
		Axis,           // X, Y or Z
		Sex,            // Male or Female
		CastingSource,  // Left, Right, Voice or Instant
	};

	struct Param
	{
		std::string   name;  // as the game names it ("Actor Value", "Magic Effect"...)
		std::uint32_t type{ 0 };  // SCRIPT_PARAM_TYPE
		bool          optional{ false };
		Arg           arg{ Arg::Unsupported };
		RE::FormType  formType{ RE::FormType::None };
	};

	struct Function
	{
		std::uint16_t      index{ 0 };  // the CTDA function index
		std::string        name;
		std::string        help;  // the game's help line, often empty
		bool               reference{ false };  // needs a subject (the engine skips it without one)
		std::vector<Param> params;
		bool               supported{ true };   // every parameter can be written
		RE::SCRIPT_FUNCTION::Condition_t* function{ nullptr };
	};

	// Every condition function, by name. Built on first use; any thread (the table is the game's static data).
	const std::vector<Function>& List();
	// By name, case-insensitive; null when there is none.
	const Function* Find(std::string_view name);

	// The ones CastOn answers itself: the engine's HasMagicEffect (214) and HasMagicEffectKeyword (699) count effects
	// their spell's conditions switched off (MagicTarget::HasMagicEffect 0x1405530D0 compares the base effect only).
	inline constexpr std::uint16_t kHasMagicEffect = 214;
	inline constexpr std::uint16_t kHasMagicEffectKeyword = 699;
}
