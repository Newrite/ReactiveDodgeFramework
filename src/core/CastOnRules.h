#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// CastOn rules (docs/CASTON_DESIGN.md): what a designer writes, as text. Engine-free, so the JSON side is unit-tested;
// the game side (game/CastOn) resolves the forms and compiles the conditions.
namespace rdf::caston
{
	enum class Trigger
	{
		Dodge,         // a dodge started (fresh or chained)
		PerfectDodge,  // a hit the i-frames dodged inside the perfect window
	};

	// A participant of the trigger. The attacker is known on PerfectDodge only (on Dodge it is null: its conditions fail).
	enum class Subject
	{
		Dodger,
		Attacker,
		CombatTarget,  // the dodger's combat target
	};

	// Where a cast goes. Self = the cast's own caster; Aim = no target (an aimed spell flies where the caster faces).
	enum class TargetKind
	{
		Self,
		Dodger,
		Attacker,
		CombatTarget,
		Aim,
	};

	enum class CostMode
	{
		Free,    // CastSpellImmediate alone (Papyrus Spell.Cast)
		Engine,  // the engine's cost (CalculateCost: skill, perks, cost mods) through ModActorValue, as a real cast pays
		Custom,  // our own formula on a chosen actor value
	};

	enum class CostValue
	{
		Magicka,
		Stamina,
		Health,
	};

	enum class Shortfall
	{
		Skip,        // not enough: no cast
		CastAnyway,  // pay what there is, cast in full
		Scale,       // pay what there is, effectiveness scaled by the share paid
	};

	enum class Compare
	{
		None,  // no comparison: the condition's own truth (e.g. "has an effect with this keyword")
		Eq,
		Ne,
		Lt,
		Le,
		Gt,
		Ge,
	};

	enum class Hand
	{
		Right,
		Left,
		Either,
	};

	enum class ValueMode
	{
		Current,
		Max,      // permanent value + temporary modifiers
		Percent,  // current / max, 0..100
		Base,
	};

	enum class ConditionKind
	{
		And,
		Or,
		HasKeyword,        // form: keyword on the actor, its race, a worn item or an active magic effect
		HasEffectKeyword,  // form: an active effect with the keyword; with a comparison: the summed magnitudes
		HasPerk,           // form: perk
		IsRace,            // form: race
		InFaction,         // form: faction; with a comparison: the rank
		Equipped,          // text: equipped type (see kEquippedTypes), hand
		ActorValue,        // text: actor value name (as the console / CK name it), mode, comparison
		InCombat,
		Sneaking,
		Sprinting,
		Blocking,
		Attacking,
		PowerAttacking,
		Casting,     // hand: a cast runs in that hand (Either: any, dual included)
		IsPlayer,
		FirstPerson,  // the dodge plays in first person (the dodger)
		Layer,        // text: where the dodge plays (Root, OneHand, Bow, Magic)
		Direction,    // text: the dodge clip (Forward, ForwardRight, ... ForwardLeft, Neutral)
		ChainIndex,   // comparison: 0 = the first dodge of a chain
		FromStagger,  // the dodge cut a stagger short
		Random,       // value: chance in percent
		Preset,       // text: a named condition list ("presets" of any rule file), as if written here (an AND of it)
	};

	struct Condition
	{
		ConditionKind          kind{ ConditionKind::And };
		bool                   negated{ false };
		Subject                subject{ Subject::Dodger };
		std::string            form;  // "Plugin.esp|0xID" or an EditorID
		std::string            text;
		Hand                   hand{ Hand::Either };
		ValueMode              mode{ ValueMode::Current };
		Compare                compare{ Compare::None };
		float                  value{ 0.0f };
		std::vector<Condition> children;  // And / Or
	};

	struct Cast
	{
		std::string spell;
		Subject     caster{ Subject::Dodger };
		TargetKind  target{ TargetKind::Self };
		float       effectiveness{ 1.0f };
		float       magnitude{ 0.0f };  // magnitude override; 0 = the spell's own
		bool        noHitEffectArt{ false };
		// A concentration spell keeps going this long, then the caster is interrupted (seconds; other spells ignore it).
		float       duration{ 1.0f };
	};

	struct Cost
	{
		CostMode  mode{ CostMode::Free };
		// Engine
		bool      dualCast{ false };  // x fMagicDualCastingCostMult + fMagicDualCastingCostBase
		bool      realCast{ false };  // also what a real cast does: TESSpellCastEvent, breaking invisibility
		// Custom: (flat + spellShare x the spell's cost + maxShare x the value's max) x mult
		CostValue value{ CostValue::Magicka };
		float     flat{ 0.0f };
		float     spellShare{ 1.0f };
		float     maxShare{ 0.0f };
		float     mult{ 1.0f };
		// Engine and Custom
		Shortfall shortfall{ Shortfall::Skip };
		bool      feedback{ true };  // the player: HUD meter flash and the fail sound when refused
	};

	struct Timing
	{
		float       delay{ 0.0f };  // seconds after the trigger; past the dodge's end = at its end
		std::string tag;            // RDF_CastOnDodge.<tag> / RDF_CastOnPerfectDodge.<tag> annotations fire it; untagged ones fire every rule
		bool        annotation{ true };  // a clip annotation of the trigger overrides the delay
	};

	struct Limits
	{
		float cooldown{ 0.0f };     // seconds between firings, per actor
		bool  oncePerChain{ false };
		float chance{ 100.0f };
	};

	struct Rule
	{
		std::string       name;
		std::string       extends;  // the template the rule was built from (already applied when read)
		bool              enabled{ true };
		int               priority{ 0 };  // higher first
		bool              exclusive{ false };  // fired: the lower rules of the trigger are not evaluated
		Trigger           trigger{ Trigger::Dodge };
		std::vector<Condition> conditions;  // all must hold (an implicit AND)
		std::vector<Cast> casts;
		Cost              cost;
		Timing            timing;
		Limits            limits;
	};

	// Named condition lists: a file's "presets"; every file's presets are visible to every rule (a modpack's shared file).
	using Presets = std::map<std::string, std::vector<Condition>, std::less<>>;

	struct ParseResult
	{
		std::vector<Rule>        rules;    // the valid ones (templates applied; Preset conditions kept, see ExpandPresets)
		Presets                  presets;  // the file's own
		std::vector<std::string> errors;   // the file's syntax error (then nothing else), or "rules[2] 'Name': ..." for a dropped rule
	};

	// A rule file: SKSE/Plugins/ReactiveDodgeFramework/CastOn/<name>.json, or <name>.user.json beside it, which replaces it
	// whole (OAR's user.json way). JSONC: comments allowed; an unknown key is an error (a typo must not pass silently).
	// Keys: "$schema" (for editors), "version", "presets", "templates", "rules". A rule with "extends" starts from that
	// template (a template may extend another): the rule's keys replace the template's, the cost / timing / limits
	// objects merge key by key, and the conditions add up (the template's and the rule's must all hold).
	ParseResult ParseRules(std::string_view json);
	// Rules are written whole: a rule read from a template already holds the merged values, so "extends" is not written.
	std::string WriteRules(const std::vector<Rule>& rules, const Presets& presets = {});

	// Replaces every Preset condition by an AND of the named list (nested presets too). False, with the reasons in
	// `errors`, when a name is unknown or presets name each other in a circle.
	bool ExpandPresets(std::vector<Condition>& conditions, const Presets& presets, std::vector<std::string>& errors);

	// The JSON Schema of a rule file, for editors (VS Code: "json.schemas", or "$schema" in the file).
	std::string JsonSchema();

	// Problems that make a rule unusable (a condition without its form, an unknown layer name...). Empty = valid.
	std::vector<std::string> Validate(const Rule& rule);

	// Condition parameters the kind reads (for the writer and the menu).
	struct KindInfo
	{
		std::string_view name;
		bool             form{ false };
		bool             text{ false };
		bool             hand{ false };
		bool             mode{ false };
		bool             compare{ false };   // takes compare / value
		bool             group{ false };
	};
	const KindInfo& Info(ConditionKind kind);

	inline constexpr std::string_view kLayers[]{ "Root", "OneHand", "Bow", "Magic" };
	inline constexpr std::string_view kDirections[]{ "Forward", "ForwardRight", "Right", "BackwardRight", "Backward",
		"BackwardLeft", "Left", "ForwardLeft", "Neutral" };
	inline constexpr std::string_view kEquippedTypes[]{ "Fists", "Sword", "Dagger", "Axe", "Mace", "Greatsword",
		"Battleaxe", "Warhammer", "Bow", "Crossbow", "Staff", "Spell", "Shield", "Torch", "Any" };

	bool Holds(Compare c, float lhs, float rhs);
}
