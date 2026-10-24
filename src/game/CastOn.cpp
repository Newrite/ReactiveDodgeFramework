#include "CastOn.h"

#include "AttackCancel.h"
#include "Diagnostics.h"
#include "Effects.h"
#include "Forms.h"
#include "MagicEffects.h"
#include "Settings.h"
#include "VanillaConditions.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <charconv>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <random>

namespace rdf::game::caston
{
	using namespace rdf::caston;

	namespace
	{
		constexpr auto kFolder = "Data/SKSE/Plugins/ReactiveDodgeFramework/CastOn";

		// Engine functions (SE 1.5.97, checked in IDA; docs/CASTON_DESIGN.md 1). CommonLib keeps CalculateCost protected.
		float CalculateCost(const RE::MagicItem* spell, RE::Actor* caster)
		{
			using func_t = float (*)(const RE::MagicItem*, RE::Actor*);
			static REL::Relocation<func_t> func{ REL::ID(11213) };  // 0x140101A30, what CheckCast / SpellCast charge
			return func(spell, caster);
		}

		RE::ActorValue ActorValueForCost(RE::MagicItem* spell, RE::MagicSystem::CastingSource source)
		{
			using func_t = RE::ActorValue (*)(RE::MagicItem*, RE::MagicSystem::CastingSource);
			static REL::Relocation<func_t> func{ REL::ID(33817) };  // 0x140556780: magicka for spells, item charge for staves
			return func(spell, source);
		}

		void RemoveEffectsDueToCast(RE::Actor* actor, std::int32_t keepArchetype)
		{
			using func_t = bool (*)(RE::Actor*, std::int32_t);
			static REL::Relocation<func_t> func{ REL::ID(37864) };  // 0x140634B80: dispels invisibility / ethereal unless kept
			func(actor, keepArchetype);
		}

		void FlashMeter(RE::ActorValue value) { effects::FlashMeter(value); }  // 51907 = 0x1408D53D0

		float GameSetting(const char* name, float fallback)
		{
			auto* collection = RE::GameSettingCollection::GetSingleton();
			auto* setting = collection ? collection->GetSetting(name) : nullptr;
			return setting ? setting->GetFloat() : fallback;
		}

		// ------------------------------------------------------------------------------------------------ compiled form
		struct Node
		{
			ConditionKind  kind{ ConditionKind::And };
			bool           negated{ false };
			Subject        subject{ Subject::Dodger };
			RE::TESForm*   form{ nullptr };
			int            index{ -1 };  // layer / direction / equipped type
			RE::ActorValue av{ RE::ActorValue::kNone };
			ValueMode      mode{ ValueMode::Current };
			Compare        compare{ Compare::None };
			float          value{ 0.0f };
			Hand           hand{ Hand::Either };
			bool           inactiveEffects{ false };
			magic::Reading reading{ magic::Reading::Authored };
			// Vanilla: the function, its stored parameters, and the parameters that name a participant (filled in per check)
			const vanilla::Function* function{ nullptr };
			void*                    params[2]{ nullptr, nullptr };
			std::int8_t              participant[2]{ -1, -1 };  // a Subject, or kOther
			std::uint32_t  first{ 0 };  // children: nodes[first .. first + count)
			std::uint32_t  count{ 0 };
			int            cost{ 0 };   // for ordering the children of an AND: cheap first
		};

		struct CompiledCast
		{
			RE::MagicItem* spell{ nullptr };
			Subject        caster{ Subject::Dodger };
			TargetKind     target{ TargetKind::Self };
			float          effectiveness{ 1.0f };
			float          magnitude{ 0.0f };
			bool           noHitEffectArt{ false };
			float          duration{ 1.0f };  // a concentration spell's
		};

		struct CompiledRule
		{
			std::string               name;
			std::string               file;
			std::uint64_t             key{ 0 };  // cooldown / once-per-chain identity
			int                       priority{ 0 };
			bool                      exclusive{ false };
			Trigger                   trigger{ Trigger::Dodge };
			std::uint32_t             root{ 0 };
			std::vector<CompiledCast> casts;
			rdf::caston::Cost         cost;
			rdf::caston::Timing       timing;  // not rdf::Timing (the dodge's phases)
			rdf::caston::Limits       limits;
		};
	}

	struct RuleSet
	{
		std::vector<CompiledRule>  rules;
		std::vector<Node>          nodes;
		std::vector<std::uint32_t> byTrigger[2];  // rule indices, priority order
		RE::BGSKeyword*            warhammer{ nullptr };
	};

	namespace
	{
		std::mutex g_lock;  // Current() may be read by the menu thread later
		RuleSetPtr g_current;

		// Concentration spells CastOn started: CastSpellImmediate leaves the instant caster casting them and nothing else
		// releases it (Character::Update 0x140632E50 updates all four casters each frame; ActorMagicCaster::Update keeps a
		// concentration spell going). Module-wide, not per agent: an NPC's agent is dropped a second after its dodge.
		struct Channel
		{
			RE::ObjectRefHandle caster;
			RE::MagicItem*      spell{ nullptr };
			float               left{ 0.0f };                      // game seconds
			float               duration{ 0.0f };
			RE::ActorValue      drained{ RE::ActorValue::kNone };  // Engine: the value we drain per second; stops at 0
			float               perSecond{ 0.0f };                 // ... and how much (CalculateCost of the spell)
			std::string         rule;
		};
		std::vector<Channel> g_channels;  // main thread

		void StartChannel(RE::Actor* caster, RE::MagicItem* spell, float duration, RE::ActorValue drained, float perSecond,
			const std::string& rule)
		{
			const auto handle = caster->GetHandle();
			std::erase_if(g_channels, [&](const Channel& c) { return c.caster == handle; });  // its instant caster now casts this
			g_channels.push_back({ handle, spell, duration, duration, drained, perSecond, rule });
		}

		std::mt19937& Rng()
		{
			static std::mt19937 rng{ std::random_device{}() };
			return rng;
		}

		bool Roll(float percent)
		{
			if (percent >= 100.0f) {
				return true;
			}
			if (percent <= 0.0f) {
				return false;
			}
			return std::uniform_real_distribution<float>(0.0f, 100.0f)(Rng()) < percent;
		}

		// ------------------------------------------------------------------------------------------------ files
		struct RuleText
		{
			std::string file;  // as named in the log
			std::string json;
			std::string name;  // <name> of <name>.json / <name>.user.json, as the mod spelled it
			bool        user{ false };
		};

		std::string Utf8(const std::filesystem::path& p)
		{
			const auto u8 = p.generic_u8string();  // no throwing conversion
			return std::string(u8.begin(), u8.end());
		}

		// <name>.json and <name>.user.json pairs: the user file wins whole.
		std::vector<RuleText> ReadFiles()
		{
			namespace fs = std::filesystem;
			std::vector<RuleText> out;
			std::error_code       ec;
			const fs::path        folder(kFolder);
			if (!fs::is_directory(folder, ec)) {
				return out;
			}
			std::map<std::string, std::pair<fs::path, fs::path>> byName;  // lower-case name -> (mod file, user file)
			for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
				if (!it->is_regular_file(ec)) {
					continue;
				}
				auto name = Utf8(it->path().filename());
				std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (name.ends_with(".schema.json")) {
					continue;  // rdf_caston.schema.json: for editors
				}
				if (name.ends_with(".user.json")) {
					byName[name.substr(0, name.size() - 10)].second = it->path();
				} else if (name.ends_with(".json")) {
					byName[name.substr(0, name.size() - 5)].first = it->path();
				}
			}
			for (const auto& [name, paths] : byName) {
				const bool  user = !paths.second.empty();
				const auto& path = user ? paths.second : paths.first;
				std::ifstream in(path, std::ios::binary);
				if (!in) {
					logger::warn("CastOn: cannot read {}", Utf8(path));
					continue;
				}
				auto stem = Utf8(path.filename());  // the spelling on disk, without .json / .user.json
				stem.resize(stem.size() - (user ? 10 : 5));
				out.push_back({ Utf8(path.filename()), std::string(std::istreambuf_iterator<char>(in), {}), stem, user });
			}
			return out;
		}

		// ------------------------------------------------------------------------------------------------ compiler
		RE::TESForm* ResolveSpell(const std::string& text, std::string& error)
		{
			// any magic item: a spell, a scroll, an enchantment, a potion
			for (const auto type : { RE::FormType::Spell, RE::FormType::Scroll, RE::FormType::Enchantment, RE::FormType::AlchemyItem }) {
				const auto r = forms::Resolve(text, type);
				if (r.status == forms::Status::Found) {
					return r.form;
				}
				if (r.status != forms::Status::WrongType) {
					error = r.detail;
					return nullptr;
				}
			}
			error = std::format("'{}' is not a spell, scroll, enchantment or potion", text);
			return nullptr;
		}

		RE::FormType FormTypeOf(ConditionKind kind)
		{
			switch (kind) {
			case ConditionKind::HasKeyword:
			case ConditionKind::HasEffectKeyword: return RE::FormType::Keyword;
			case ConditionKind::HasMagicEffect: return RE::FormType::MagicEffect;
			case ConditionKind::HasPerk: return RE::FormType::Perk;
			case ConditionKind::IsRace: return RE::FormType::Race;
			case ConditionKind::InFaction: return RE::FormType::Faction;
			default: return RE::FormType::None;
			}
		}

		int CostOf(ConditionKind kind)
		{
			switch (kind) {
			case ConditionKind::Layer:
			case ConditionKind::Direction:
			case ConditionKind::ChainIndex:
			case ConditionKind::FromStagger:
			case ConditionKind::FirstPerson:
			case ConditionKind::IsPlayer:
			case ConditionKind::Random: return 0;
			case ConditionKind::HasPerk:
			case ConditionKind::InFaction:
			case ConditionKind::ActorValue:
			case ConditionKind::Vanilla: return 2;
			case ConditionKind::HasKeyword:
			case ConditionKind::HasEffectKeyword:
			case ConditionKind::HasMagicEffect: return 3;  // the walk over worn items and effects
			default: return 1;
			}
		}

		constexpr std::int8_t kOther = 3;  // a Vanilla reference parameter left empty: the other participant

		std::string_view Trim(std::string_view s)
		{
			while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
				s.remove_prefix(1);
			}
			while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
				s.remove_suffix(1);
			}
			return s;
		}

		void* AsParam(std::int32_t v) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(static_cast<std::uint32_t>(v))); }

		// One of `names` (case-insensitive) or a number; its index or the number.
		std::optional<std::int32_t> NamedOrNumber(const std::string& text, std::initializer_list<std::string_view> names)
		{
			std::int32_t i = 0;
			for (const auto n : names) {
				if (n.size() == text.size() && _strnicmp(n.data(), text.c_str(), n.size()) == 0) {
					return i;
				}
				++i;
			}
			std::int32_t v = 0;
			const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
			if (ec != std::errc() || end != text.data() + text.size()) {
				return std::nullopt;
			}
			return v;
		}

		// A Vanilla parameter from its text: the void* slot the function reads, or a participant to fill in per check.
		bool ParseArg(const vanilla::Param& p, const std::string& text, void*& slot, std::int8_t& participant, std::string& why)
		{
			using A = vanilla::Arg;
			switch (p.arg) {
			case A::Form:
				{
					const auto r = forms::Resolve(text, p.formType);
					if (!r.form) {
						why = r.detail;
						return false;
					}
					slot = r.form;
					return true;
				}
			case A::Reference:
				{
					if (const auto who = NamedOrNumber(text, { "Dodger", "Attacker", "CombatTarget" }); who && *who >= 0 && *who <= 2 &&
						!std::isdigit(static_cast<unsigned char>(text.front()))) {
						participant = static_cast<std::int8_t>(*who);
						return true;
					}
					const auto r = forms::Resolve(text, RE::FormType::None);
					auto*      ref = r.form ? r.form->As<RE::TESObjectREFR>() : nullptr;
					if (!ref) {
						why = r.form ? std::format("{} is not a placed reference (or write Dodger, Attacker, CombatTarget)", r.detail) : r.detail;
						return false;
					}
					slot = ref;
					return true;
				}
			case A::ActorValue:
				{
					const auto av = RE::ActorValueList::LookupActorValueByName(text.c_str());
					if (av == RE::ActorValue::kNone) {
						why = std::format("no actor value '{}'", text);
						return false;
					}
					slot = AsParam(static_cast<std::int32_t>(av));
					return true;
				}
			case A::Integer:
				if (const auto v = NamedOrNumber(text, {})) {
					slot = AsParam(*v);
					return true;
				}
				why = std::format("'{}' is not a whole number", text);
				return false;
			case A::Float:
				{
					float      v = 0.0f;
					const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
					if (ec != std::errc() || end != text.data() + text.size()) {
						why = std::format("'{}' is not a number", text);
						return false;
					}
					slot = AsParam(std::bit_cast<std::int32_t>(v));
					return true;
				}
			case A::Axis:
				if (text.size() == 1 && std::strchr("XYZxyz", text[0])) {
					slot = AsParam(std::toupper(static_cast<unsigned char>(text[0])));
					return true;
				}
				why = "X, Y or Z";
				return false;
			case A::Sex:
				if (const auto v = NamedOrNumber(text, { "Male", "Female" })) {
					slot = AsParam(*v);
					return true;
				}
				why = "Male or Female";
				return false;
			case A::CastingSource:
				if (const auto v = NamedOrNumber(text, { "Left", "Right", "Voice", "Instant" })) {
					slot = AsParam(*v);
					return true;
				}
				why = "Left, Right, Voice or Instant";
				return false;
			default:
				why = "a parameter of this kind cannot be written";
				return false;
			}
		}

		template <std::size_t N>
		int IndexOf(const std::string& text, const std::string_view (&list)[N])
		{
			const auto it = std::find(std::begin(list), std::end(list), text);
			return it == std::end(list) ? -1 : static_cast<int>(it - std::begin(list));
		}

		class Compiler
		{
		public:
			explicit Compiler(RuleSet& set) :
				set_(set) {}

			// The rule's conditions as one AND node; its index, or nullopt with `error` set.
			std::optional<std::uint32_t> Conditions(const std::vector<Condition>& conditions, std::string& error)
			{
				Condition root;
				root.kind = ConditionKind::And;
				root.children = conditions;
				const auto at = static_cast<std::uint32_t>(set_.nodes.size());
				set_.nodes.emplace_back();
				Node node;
				if (!Compile(root, node, "conditions", error)) {
					return std::nullopt;
				}
				set_.nodes[at] = node;
				return at;
			}

		private:
			bool Compile(const Condition& c, Node& out, const std::string& path, std::string& error)
			{
				out.kind = c.kind;
				out.negated = c.negated;
				out.subject = c.subject;
				out.mode = c.mode;
				out.compare = c.compare;
				out.value = c.value;
				out.hand = c.hand;
				out.inactiveEffects = c.inactiveEffects;
				out.reading = c.reading == Reading::Applied ? magic::Reading::Applied : magic::Reading::Authored;
				out.cost = CostOf(c.kind);
				if (Info(c.kind).group) {
					// children contiguous: reserve the block, then compile each into it (grandchildren go after)
					out.first = static_cast<std::uint32_t>(set_.nodes.size());
					out.count = static_cast<std::uint32_t>(c.children.size());
					set_.nodes.resize(set_.nodes.size() + c.children.size());
					std::vector<Node> children(c.children.size());
					for (std::size_t i = 0; i < c.children.size(); ++i) {
						if (!Compile(c.children[i], children[i], std::format("{}[{}]", path, i), error)) {
							return false;
						}
					}
					if (c.kind == ConditionKind::And) {  // cheap checks first; an OR keeps the author's order
						std::stable_sort(children.begin(), children.end(), [](const Node& a, const Node& b) { return a.cost < b.cost; });
					}
					int cost = 0;
					for (std::size_t i = 0; i < children.size(); ++i) {
						cost = std::max(cost, children[i].cost);
						set_.nodes[out.first + i] = children[i];
					}
					out.cost = cost;
					return true;
				}
				if (const auto type = FormTypeOf(c.kind); type != RE::FormType::None) {
					const auto r = forms::Resolve(c.form, type);
					if (!r.form) {
						error = std::format("{}: {}", path, r.detail);
						return false;
					}
					out.form = r.form;
				}
				switch (c.kind) {
				case ConditionKind::Layer: out.index = IndexOf(c.text, kLayers); break;
				case ConditionKind::Direction: out.index = IndexOf(c.text, kDirections); break;
				case ConditionKind::Equipped: out.index = IndexOf(c.text, kEquippedTypes); break;
				case ConditionKind::ActorValue:
					out.av = RE::ActorValueList::LookupActorValueByName(c.text.c_str());
					if (out.av == RE::ActorValue::kNone) {
						error = std::format("{}: no actor value '{}'", path, c.text);
						return false;
					}
					break;
				case ConditionKind::Vanilla: return CompileVanilla(c, out, path, error);
				default: break;
				}
				return true;
			}

			bool CompileVanilla(const Condition& c, Node& out, const std::string& path, std::string& error)
			{
				const auto* f = vanilla::Find(c.text);
				if (!f) {
					error = std::format("{}: no condition function '{}'", path, c.text);
					return false;
				}
				if (!f->supported) {
					error = std::format("{}: {} takes a parameter CastOn cannot write (an alias, package data...)", path, f->name);
					return false;
				}
				if (c.args.size() > f->params.size()) {
					error = std::format("{}: {} takes {} parameter(s), not {}", path, f->name, f->params.size(), c.args.size());
					return false;
				}
				out.function = f;
				for (std::size_t k = 0; k < f->params.size(); ++k) {
					const auto& p = f->params[k];
					const auto  text = k < c.args.size() ? std::string(Trim(c.args[k])) : std::string();
					if (text.empty()) {
						if (k == 0 && p.arg == vanilla::Arg::Reference) {
							out.participant[k] = kOther;  // the engine's way: an empty first reference is the target
						} else if (!p.optional) {
							error = std::format("{}: {} needs '{}'", path, f->name, p.name);
							return false;
						}
						continue;
					}
					std::string why;
					if (!ParseArg(p, text, out.params[k], out.participant[k], why)) {
						error = std::format("{}: {} '{}': {}", path, f->name, p.name, why);
						return false;
					}
				}
				return true;
			}

			RuleSet& set_;
		};

		std::uint64_t KeyOf(const std::string& file, const std::string& name, std::size_t index)
		{
			std::uint64_t h = 1469598103934665603ull;  // FNV-1a
			for (const auto& part : { file, name, std::to_string(index) }) {
				for (const unsigned char c : part) {
					h = (h ^ c) * 1099511628211ull;
				}
				h = (h ^ 0x1f) * 1099511628211ull;
			}
			return h;
		}

		// One rule against the loaded forms: presets expanded, conditions compiled into `set`, casts resolved. False with
		// `error` when something is missing (a form, a preset, a spell) or a spell cannot be cast (an ability).
		bool CompileRule(RuleSet& set, Compiler& compiler, const Rule& rule, const Presets& presets, const std::string& file,
			std::size_t index, CompiledRule& c, std::string& error)
		{
			c.name = rule.name.empty() ? std::format("#{}", index) : rule.name;
			c.file = file;
			c.key = KeyOf(file, rule.name, index);
			c.priority = rule.priority;
			c.exclusive = rule.exclusive;
			c.trigger = rule.trigger;
			c.cost = rule.cost;
			c.timing = rule.timing;
			c.limits = rule.limits;
			auto                     conditions = rule.conditions;
			std::vector<std::string> presetErrors;
			DropDisabled(conditions);  // before: a switched-off reference to a missing preset is no error
			if (!ExpandPresets(conditions, presets, presetErrors)) {
				error = presetErrors.empty() ? std::string("a preset failed") : presetErrors.front();
				return false;
			}
			DropDisabled(conditions);  // after: what the presets switched off
			const auto root = compiler.Conditions(conditions, error);
			if (!root) {
				return false;
			}
			c.root = *root;
			for (const auto& cast : rule.casts) {
				auto* form = ResolveSpell(cast.spell, error);
				auto* spell = form ? form->As<RE::MagicItem>() : nullptr;
				if (!spell) {
					if (error.empty()) {
						error = std::format("{} is not a magic item", cast.spell);
					}
					return false;
				}
				if (spell->GetCastingType() == RE::MagicSystem::CastingType::kConstantEffect) {
					error = std::format("{} is a constant effect (an ability): it cannot be cast", cast.spell);
					return false;
				}
				c.casts.push_back({ spell, cast.caster, cast.target, cast.effectiveness, cast.magnitude, cast.noHitEffectArt, cast.duration });
			}
			(void)set;
			return true;
		}

		// ------------------------------------------------------------------------------------------------ evaluation
		struct Memo  // the keyword walk once per (actor, keyword) and trigger
		{
			struct Entry
			{
				const RE::Actor*      actor;
				const RE::BGSKeyword* keyword;
				bool                  inactive;
				bool                  has;
			};
			std::vector<Entry> entries;

			bool Has(RE::Actor* actor, RE::BGSKeyword* keyword, bool inactive)
			{
				for (const auto& e : entries) {
					if (e.actor == actor && e.keyword == keyword && e.inactive == inactive) {
						return e.has;
					}
				}
				const bool has = attack::ActorHasKeyword(actor, keyword, inactive);
				entries.push_back({ actor, keyword, inactive, has });
				return has;
			}
		};

		RE::Actor* SubjectOf(Subject s, const Context& ctx)
		{
			switch (s) {
			case Subject::Attacker: return ctx.attacker;
			case Subject::CombatTarget: return ctx.combatTarget;
			default: return ctx.dodger;
			}
		}

		// Index into kEquippedTypes of what the hand holds, -1 for something else.
		int EquippedType(RE::Actor* actor, bool left, const RE::BGSKeyword* warhammer)
		{
			const auto* form = actor->GetEquippedObject(left);
			if (!form) {
				return 0;  // fists
			}
			if (const auto* weapon = form->As<RE::TESObjectWEAP>()) {
				using T = RE::WEAPON_TYPE;
				switch (weapon->GetWeaponType()) {
				case T::kHandToHandMelee: return 0;
				case T::kOneHandSword: return 1;
				case T::kOneHandDagger: return 2;
				case T::kOneHandAxe: return 3;
				case T::kOneHandMace: return 4;
				case T::kTwoHandSword: return 5;
				case T::kTwoHandAxe: return warhammer && weapon->HasKeyword(warhammer) ? 7 : 6;
				case T::kBow: return 8;
				case T::kCrossbow: return 9;
				case T::kStaff: return 10;
				default: return -1;
				}
			}
			if (form->Is(RE::FormType::Spell)) {
				return 11;
			}
			if (form->Is(RE::FormType::Armor)) {
				return 12;
			}
			if (form->Is(RE::FormType::Light)) {
				return 13;
			}
			return -1;
		}

		bool GraphBool(RE::Actor* actor, std::string_view name)
		{
			bool value = false;
			return actor->GetGraphVariableBool(name, value) && value;
		}

		// The participant a check does not run on: the dodger's is the attacker, else the combat target; anyone else's the
		// dodger (what TESConditionItem calls the target).
		RE::Actor* OtherOf(RE::Actor* actor, const Context& ctx)
		{
			return actor == ctx.dodger ? (ctx.attacker ? ctx.attacker : ctx.combatTarget) : ctx.dodger;
		}

		// A Vanilla condition's function result. False when the function refuses (or a named participant is absent).
		bool VanillaResult(const Node& n, RE::Actor* actor, const Context& ctx, double& result)
		{
			void* params[2]{ n.params[0], n.params[1] };
			for (int k = 0; k < 2; ++k) {
				if (n.participant[k] < 0) {
					continue;
				}
				RE::Actor* who = n.participant[k] == kOther ? OtherOf(actor, ctx) : SubjectOf(static_cast<Subject>(n.participant[k]), ctx);
				if (!who && n.participant[k] != kOther) {
					return false;
				}
				params[k] = static_cast<RE::TESObjectREFR*>(who);
			}
			const auto formOf = [&](int k) { return params[k] ? static_cast<RE::TESForm*>(params[k]) : nullptr; };
			switch (n.function->index) {
			case vanilla::kHasMagicEffect:
				{
					const auto* effect = formOf(0) ? formOf(0)->As<RE::EffectSetting>() : nullptr;
					result = effect && magic::Any(actor, { .effect = effect, .inactive = n.inactiveEffects }) ? 1.0 : 0.0;
					return true;
				}
			case vanilla::kHasMagicEffectKeyword:
				{
					const auto* keyword = formOf(0) ? formOf(0)->As<RE::BGSKeyword>() : nullptr;
					result = keyword && magic::Any(actor, { .keyword = keyword, .inactive = n.inactiveEffects }) ? 1.0 : 0.0;
					return true;
				}
			default:
				if (n.function->reference && !actor) {
					return false;
				}
				return n.function->function(actor, params[0], params[1], result);
			}
		}

		bool Leaf(const RuleSet& set, const Node& n, RE::Actor* actor, const Context& ctx, Memo& memo)
		{
			switch (n.kind) {
			case ConditionKind::HasKeyword: return memo.Has(actor, n.form->As<RE::BGSKeyword>(), n.inactiveEffects);
			case ConditionKind::HasEffectKeyword:
				{
					const auto sum = magic::Query(actor, { .keyword = n.form->As<RE::BGSKeyword>(), .inactive = n.inactiveEffects }, n.reading);
					return sum.count > 0 && Holds(n.compare, sum.total, n.value);
				}
			case ConditionKind::HasMagicEffect:
				{
					const auto sum = magic::Query(actor, { .effect = n.form->As<RE::EffectSetting>(), .inactive = n.inactiveEffects }, n.reading);
					return sum.count > 0 && Holds(n.compare, sum.total, n.value);
				}
			case ConditionKind::Vanilla:
				{
					double result = 0.0;
					if (!VanillaResult(n, actor, ctx, result)) {
						return false;
					}
					return n.compare == Compare::None ? result != 0.0 : Holds(n.compare, static_cast<float>(result), n.value);
				}
			case ConditionKind::HasPerk: return actor->HasPerk(n.form->As<RE::BGSPerk>());
			case ConditionKind::IsRace: return actor->GetRace() == n.form;
			case ConditionKind::InFaction:
				{
					auto* faction = n.form->As<RE::TESFaction>();
					if (!actor->IsInFaction(faction)) {
						return false;
					}
					return n.compare == Compare::None || Holds(n.compare, static_cast<float>(actor->GetFactionRank(faction, actor->IsPlayerRef())), n.value);
				}
			case ConditionKind::Equipped:
				{
					const auto matches = [&](bool left) {
						const int type = EquippedType(actor, left, set.warhammer);
						return n.index == 14 ? type > 0 : type == n.index;  // Any: anything but fists
					};
					switch (n.hand) {
					case Hand::Right: return matches(false);
					case Hand::Left: return matches(true);
					default: return matches(false) || matches(true);
					}
				}
			case ConditionKind::ActorValue:
				{
					auto*       owner = actor->AsActorValueOwner();
					const float current = owner->GetActorValue(n.av);
					const float max = owner->GetPermanentActorValue(n.av) + actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, n.av);
					float       v = current;
					switch (n.mode) {
					case ValueMode::Max: v = max; break;
					case ValueMode::Percent: v = max > 0.0f ? current / max * 100.0f : 0.0f; break;
					case ValueMode::Base: v = owner->GetBaseActorValue(n.av); break;
					default: break;
					}
					return Holds(n.compare, v, n.value);
				}
			case ConditionKind::InCombat: return actor->IsInCombat();
			case ConditionKind::Sneaking: return actor->IsSneaking();
			case ConditionKind::Sprinting: return actor->AsActorState()->IsSprinting();
			case ConditionKind::Blocking: return actor->IsBlocking();
			case ConditionKind::Attacking: return actor->AsActorState()->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone;
			case ConditionKind::PowerAttacking: return actor->IsPowerAttacking();
			case ConditionKind::Casting:
				switch (n.hand) {
				case Hand::Right: return GraphBool(actor, "IsCastingRight"sv) || GraphBool(actor, "IsCastingDual"sv);
				case Hand::Left: return GraphBool(actor, "IsCastingLeft"sv) || GraphBool(actor, "IsCastingDual"sv);
				default: return GraphBool(actor, "IsCastingRight"sv) || GraphBool(actor, "IsCastingLeft"sv) || GraphBool(actor, "IsCastingDual"sv);
				}
			case ConditionKind::IsPlayer: return actor->IsPlayerRef();
			case ConditionKind::FirstPerson: return ctx.firstPerson && actor == ctx.dodger;
			case ConditionKind::Layer: return static_cast<int>(ctx.layer) == n.index;
			case ConditionKind::Direction: return ctx.clip == n.index;
			case ConditionKind::ChainIndex: return Holds(n.compare, static_cast<float>(ctx.chainIndex), n.value);
			case ConditionKind::FromStagger: return ctx.fromStagger;
			case ConditionKind::Random: return Roll(n.value);
			default: return false;
			}
		}

		bool Eval(const RuleSet& set, std::uint32_t index, const Context& ctx, Memo& memo)
		{
			const auto& n = set.nodes[index];
			bool        result = false;
			if (n.kind == ConditionKind::And || n.kind == ConditionKind::Or) {
				const bool all = n.kind == ConditionKind::And;
				result = all;
				for (std::uint32_t i = 0; i < n.count; ++i) {
					if (Eval(set, n.first + i, ctx, memo) != all) {
						result = !all;
						break;
					}
				}
			} else {
				auto* actor = SubjectOf(n.subject, ctx);
				if (!actor) {
					return false;  // no such participant: the condition fails, negated or not
				}
				result = Leaf(set, n, actor, ctx, memo);
			}
			return n.negated ? !result : result;
		}

		// ------------------------------------------------------------------------------------------------ casting
		// Pays the rule's cost for one cast; false = refused. Scale may lower the effectiveness.
		// A concentration spell (`duration` seconds): Engine pays nothing up front (the engine drains its cost per second while
		// it runs, as for a real cast; refused at 0), Custom's spell share is the cost per second x the duration.
		bool Pay(const Cost& cost, RE::Actor* caster, RE::MagicItem* spell, bool concentration, float duration, float& effectiveness,
			const std::string& rule)
		{
			if (cost.mode == CostMode::Free) {
				return true;
			}
			RE::ActorValue value = RE::ActorValue::kNone;
			float          amount = 0.0f;
			if (cost.mode == CostMode::Engine) {
				value = ActorValueForCost(spell, RE::MagicSystem::CastingSource::kInstant);
				if (value == RE::ActorValue::kNone) {
					return true;  // a power, a potion...: the engine charges nothing either
				}
				if (concentration) {
					if (caster->AsActorValueOwner()->GetActorValue(value) > 0.0f || (caster->IsPlayerRef() && RE::PlayerCharacter::IsGodMode())) {
						return true;
					}
					if (cost.feedback && caster->IsPlayerRef()) {
						FlashMeter(value);
					}
					DevLog("CastOn '{}': {:08X} has no {} left for a concentration spell", rule, caster->GetFormID(), static_cast<int>(value));
					return false;
				}
				amount = CalculateCost(spell, caster);
				if (cost.dualCast) {
					amount = amount * GameSetting("fMagicDualCastingCostMult", 2.8f) + GameSetting("fMagicDualCastingCostBase", 0.0f);
				}
				if (caster->IsPlayerRef() && RE::PlayerCharacter::IsGodMode()) {
					amount = 0.0f;
				}
			} else {
				value = cost.value == CostValue::Stamina ? RE::ActorValue::kStamina :
				        cost.value == CostValue::Health  ? RE::ActorValue::kHealth :
				                                           RE::ActorValue::kMagicka;
				auto*       owner = caster->AsActorValueOwner();
				const float max = owner->GetPermanentActorValue(value) + caster->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, value);
				const float spellCost = CalculateCost(spell, caster) * (concentration ? duration : 1.0f);  // per second for concentration
				amount = (cost.flat + cost.spellShare * spellCost + cost.maxShare * max) * cost.mult;
			}
			if (amount <= 0.0f) {
				return true;
			}
			auto*       owner = caster->AsActorValueOwner();
			const float available = std::max(0.0f, owner->GetActorValue(value));
			float       paid = amount;
			if (available < amount) {
				if (cost.shortfall == Shortfall::Skip || (cost.shortfall == Shortfall::Scale && available <= 0.0f)) {
					if (cost.feedback && caster->IsPlayerRef() && value != RE::ActorValue::kRightItemCharge && value != RE::ActorValue::kLeftItemCharge) {
						FlashMeter(value);
					}
					DevLog("CastOn '{}': {:08X} cannot pay {:.1f} (has {:.1f})", rule, caster->GetFormID(), amount, available);
					return false;
				}
				paid = available;
				if (cost.shortfall == Shortfall::Scale) {
					effectiveness *= available / amount;
				}
			}
			owner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, value, -paid);  // vfunc 6, as SpellCast pays
			DevLog("CastOn '{}': {:08X} paid {:.1f} of {:.1f} (actor value {})", rule, caster->GetFormID(), paid, amount, static_cast<int>(value));
			return true;
		}

		void SendSpellCastEvent(RE::Actor* caster, RE::MagicItem* spell)
		{
			auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
			if (!holder) {
				return;
			}
			RE::TESSpellCastEvent event{ RE::NiPointer<RE::TESObjectREFR>(caster), spell->GetFormID() };
			holder->GetEventSource<RE::TESSpellCastEvent>()->SendEvent(&event);
		}

		RE::TESObjectREFR* TargetOf(TargetKind kind, RE::Actor* caster, const Context& ctx)
		{
			switch (kind) {
			case TargetKind::Self: return caster;
			case TargetKind::Dodger: return ctx.dodger;
			case TargetKind::Attacker: return ctx.attacker;
			case TargetKind::CombatTarget: return ctx.combatTarget;
			default: return nullptr;  // Aim
			}
		}

		void Execute(const CompiledRule& rule, const Context& ctx)
		{
			for (const auto& cast : rule.casts) {
				auto* caster = SubjectOf(cast.caster, ctx);
				if (!caster || caster->IsDead()) {
					DevLog("CastOn '{}': no caster ({}) for {:08X}", rule.name, static_cast<int>(cast.caster), cast.spell->GetFormID());
					continue;
				}
				auto* target = TargetOf(cast.target, caster, ctx);
				if (cast.target != TargetKind::Aim && !target) {
					DevLog("CastOn '{}': no target ({}) for {:08X}", rule.name, static_cast<int>(cast.target), cast.spell->GetFormID());
					continue;
				}
				if (cast.spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf) {
					target = caster;
				}
				auto* magicCaster = caster->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
				if (!magicCaster) {
					continue;
				}
				const bool concentration = cast.spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
				float      effectiveness = cast.effectiveness;
				if (!Pay(rule.cost, caster, cast.spell, concentration, cast.duration, effectiveness, rule.name)) {
					continue;
				}
				magicCaster->CastSpellImmediate(cast.spell, cast.noHitEffectArt, target, effectiveness, false, cast.magnitude, caster);
				if (concentration) {
					// kSkipCheckCast (vfunc 0x13 sets it at 0x140543620; InterruptCastImpl clears it again), every mode. Without it
					// MagicCaster::Fire (0x14054C950), run each interval of a concentration cast, asks CheckCast first and interrupts
					// the cast when it refuses, and for the instant caster it does: one burst, then nothing (in game 2026-10-24).
					// The flag also stops ActorMagicCaster::Update's per-second drain, so Engine drains here instead (UpdateChannels).
					magicCaster->SetSkipCheckCast();
					auto  drained = RE::ActorValue::kNone;
					float perSecond = 0.0f;
					if (rule.cost.mode == CostMode::Engine) {
						drained = ActorValueForCost(cast.spell, RE::MagicSystem::CastingSource::kInstant);
						perSecond = drained == RE::ActorValue::kNone ? 0.0f : CalculateCost(cast.spell, caster);
					}
					StartChannel(caster, cast.spell, cast.duration, drained, perSecond, rule.name);
					DevLog("CastOn '{}': instant caster after the cast: holds {:08X}, state {}", rule.name,
						magicCaster->currentSpell ? magicCaster->currentSpell->GetFormID() : 0u, static_cast<int>(magicCaster->state.underlying()));
				}
				if (rule.cost.mode == CostMode::Engine && rule.cost.realCast) {
					// what SpellCast does besides paying (0x140542470): the effects a cast breaks, then the cast event
					const auto keep = cast.spell->HasEffect(RE::EffectArchetypes::ArchetypeID::kInvisibility) ? 11 :
					                  cast.spell->HasEffect(RE::EffectArchetypes::ArchetypeID::kEtherealize)  ? 41 :
					                                                                                              -1;
					RemoveEffectsDueToCast(caster, keep);
					SendSpellCastEvent(caster, cast.spell);
				}
				DevLog("CastOn '{}': {:08X} cast {:08X} '{}' on {:08X} (effectiveness {:.2f}{})", rule.name, caster->GetFormID(),
					cast.spell->GetFormID(), cast.spell->GetName(), target ? target->GetFormID() : 0u, effectiveness,
					concentration ? std::format(", concentration for {:.2f}s", cast.duration) : std::string());
			}
		}

		bool TagMatches(const std::string& ruleTag, std::string_view annotationTag)
		{
			return ruleTag == annotationTag;
		}
	}

	// ---------------------------------------------------------------------------------------------------- loading
	void Load()
	{
		auto set = std::make_shared<RuleSet>();
		if (const auto r = forms::Resolve("WeapTypeWarhammer", RE::FormType::Keyword); r.form) {
			set->warhammer = r.form->As<RE::BGSKeyword>();
		}
		Compiler   compiler(*set);
		std::size_t files = 0, dropped = 0;
		// Two passes: every file's presets first (any rule may name any of them), then the rules.
		std::vector<std::pair<std::string, ParseResult>> parsedFiles;  // (file name as logged, its rules)
		Presets                                          presets;
		std::map<std::string, std::string, std::less<>>  presetFile;
		for (const auto& text : ReadFiles()) {
			++files;
			const auto& file = text.file;
			auto        parsed = ParseRules(text.json);
			for (const auto& e : parsed.errors) {
				logger::warn("CastOn {}: {}", file, e);
			}
			for (auto& [name, list] : parsed.presets) {
				if (const auto it = presetFile.find(name); it != presetFile.end()) {
					logger::warn("CastOn {}: preset '{}' is also in {}; that one is used", file, name, it->second);
					continue;
				}
				presetFile.emplace(name, file);
				presets.emplace(name, list);
			}
			parsedFiles.emplace_back(file, std::move(parsed));
		}
		for (const auto& [file, parsed] : parsedFiles) {
			for (std::size_t i = 0; i < parsed.rules.size(); ++i) {
				const auto& rule = parsed.rules[i];
				if (!rule.enabled) {
					continue;
				}
				CompiledRule c;
				std::string  error;
				if (!CompileRule(*set, compiler, rule, presets, file, i, c, error)) {
					++dropped;
					logger::warn("CastOn {}: rule '{}' dropped: {}", file, c.name, error);
					continue;
				}
				set->rules.push_back(std::move(c));
			}
		}
		for (std::uint32_t i = 0; i < set->rules.size(); ++i) {
			set->byTrigger[static_cast<int>(set->rules[i].trigger)].push_back(i);
		}
		for (auto& list : set->byTrigger) {
			std::stable_sort(list.begin(), list.end(), [&](std::uint32_t a, std::uint32_t b) { return set->rules[a].priority > set->rules[b].priority; });
		}
		logger::info("CastOn: {} rule(s) and {} preset(s) from {} file(s) in {} ({} dropped)", set->rules.size(), presets.size(), files,
			kFolder, dropped);
		std::scoped_lock lock(g_lock);
		g_current = std::move(set);
	}

	// ---------------------------------------------------------------------------------------------------- the editor
	std::vector<Document> ReadDocuments()
	{
		std::vector<Document> out;
		for (auto& text : ReadFiles()) {
			out.push_back({ text.name, text.user, ParseRules(text.json) });
		}
		return out;
	}

	bool SaveUserDocument(const std::string& name, const std::vector<Rule>& rules, const Presets& presets, std::string& error)
	{
		namespace fs = std::filesystem;
		std::error_code ec;
		fs::create_directories(fs::path(kFolder), ec);
		const auto text = WriteRules(rules, presets);
		if (text.empty()) {
			error = "the rules could not be written as JSON";
			return false;
		}
		const auto    path = fs::path(kFolder) / fs::path(reinterpret_cast<const char8_t*>((name + ".user.json").c_str()));
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << text;
		if (!out) {
			error = std::format("cannot write {}", Utf8(path));
			return false;
		}
		logger::info("CastOn: saved {} ({} rule(s), {} preset(s))", Utf8(path), rules.size(), presets.size());
		return true;
	}

	bool DeleteUserDocument(const std::string& name, std::string& error)
	{
		namespace fs = std::filesystem;
		std::error_code ec;
		const auto      path = fs::path(kFolder) / fs::path(reinterpret_cast<const char8_t*>((name + ".user.json").c_str()));
		if (!fs::remove(path, ec) || ec) {
			error = std::format("cannot delete {}", Utf8(path));
			return false;
		}
		logger::info("CastOn: deleted {}", Utf8(path));
		return true;
	}

	namespace
	{
		struct Watch
		{
			std::mutex                 lock;
			std::optional<Rule>        rule;  // what the editor shows; nullopt = nothing
			Presets                    presets;
			std::uint32_t              requested{ 0 };
			Live                       result;
		};
		std::atomic<bool>                     g_liveQueued{ false };
		std::chrono::steady_clock::time_point g_liveLast{};
		Watch g_watch;

		// The player as the dodger; its combat target as the attacker and the combat target (the editor has no dodge).
		Context PlayerContext(RE::NiPointer<RE::Actor>& hold)
		{
			Context ctx;
			auto*   player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return ctx;
			}
			hold = player->GetActorRuntimeData().currentCombatTarget.get();
			ctx.dodger = player;
			ctx.attacker = hold.get();
			ctx.combatTarget = hold.get();
			const auto* camera = RE::PlayerCamera::GetSingleton();
			ctx.firstPerson = camera && camera->IsInFirstPerson();
			return ctx;
		}

		bool NeedsActor(ConditionKind kind)
		{
			switch (kind) {
			case ConditionKind::Layer:
			case ConditionKind::Direction:
			case ConditionKind::ChainIndex:
			case ConditionKind::FromStagger:
			case ConditionKind::FirstPerson:
			case ConditionKind::Random:
			case ConditionKind::Preset: return false;
			default: return true;
			}
		}

		// Rows of a switched-off subtree: no result (-2), as many rows as the editor draws.
		void SkipRows(const Condition& c, std::vector<std::int8_t>& out)
		{
			out.push_back(-2);
			for (const auto& child : c.children) {
				SkipRows(child, out);
			}
		}

		// One row of the editor's condition tree, then its children (depth-first, as the editor draws them). -2 = the
		// condition is switched off; a group decides on the rest.
		std::int8_t LiveRow(const Condition& c, const Presets& presets, const Context& ctx, const RuleSet& base, Memo& memo,
			std::vector<std::int8_t>& out)
		{
			if (c.disabled) {
				SkipRows(c, out);
				return -2;
			}
			const auto at = out.size();
			out.push_back(-1);
			std::int8_t result = -1;
			if (Info(c.kind).group) {
				bool decided = false, unknown = false, any = false;
				for (const auto& child : c.children) {
					const auto v = LiveRow(child, presets, ctx, base, memo, out);
					if (v == -2) {
						continue;
					}
					any = true;
					unknown = unknown || v < 0;
					if (!decided && v >= 0 && (v == 1) == (c.kind == ConditionKind::Or)) {
						decided = true;  // an Or with a true child, an And with a false one
					}
				}
				result = decided ? (c.kind == ConditionKind::Or ? 1 : 0) : unknown ? -1 : (c.kind == ConditionKind::Or ? 0 : 1);
				if (!any) {
					result = 1;  // everything inside switched off: the group is dropped (DropDisabled), it holds
				} else if (result >= 0 && c.negated) {
					result = static_cast<std::int8_t>(1 - result);
				}
			} else if (!NeedsActor(c.kind) || SubjectOf(c.subject, ctx)) {
				RuleSet  one;
				Compiler compiler(one);
				one.warhammer = base.warhammer;
				std::vector<Condition>   list{ c };
				std::vector<std::string> errors;
				std::string              error;
				if (ExpandPresets(list, presets, errors)) {
					DropDisabled(list);  // a preset's switched-off conditions
					if (const auto root = compiler.Conditions(list, error)) {
						result = Eval(one, *root, ctx, memo) ? 1 : 0;
					}
				}
			}
			out[at] = result;
			return result;
		}
	}

	void WatchRule(std::optional<Rule> rule, Presets presets)
	{
		std::scoped_lock lock(g_watch.lock);
		g_watch.rule = std::move(rule);
		g_watch.presets = std::move(presets);
		++g_watch.requested;
		g_liveLast = {};  // at once
	}

	Live LiveResults()
	{
		std::scoped_lock lock(g_watch.lock);
		return g_watch.result;
	}

	// Main thread. A task the editor posts (PollLive): the game is usually paused while the menu is open, so the frame
	// tick (UpdateChannels) does not run, but SKSE tasks do.
	void EvaluateLive()
	{
		std::optional<Rule> rule;
		Presets             presets;
		std::uint32_t       generation = 0;
		{
			std::scoped_lock lock(g_watch.lock);
			if (!g_watch.rule) {
				return;
			}
			rule = g_watch.rule;
			presets = g_watch.presets;
			generation = g_watch.requested;
		}
		RE::NiPointer<RE::Actor> hold;
		const auto               ctx = PlayerContext(hold);
		Live                     live;
		live.generation = generation;
		if (ctx.dodger) {
			RuleSet base;
			if (const auto set = Current()) {
				base.warhammer = set->warhammer;
			}
			Memo memo;
			for (const auto& c : rule->conditions) {
				LiveRow(c, presets, ctx, base, memo, live.results);
			}
			live.participants = ctx.attacker ? std::format("{:08X} '{}'", ctx.attacker->GetFormID(), ctx.attacker->GetName()) : std::string();
		}
		std::scoped_lock lock(g_watch.lock);
		g_watch.result = std::move(live);
	}

	void PollLive()
	{
		const auto now = std::chrono::steady_clock::now();
		if (g_liveQueued.load() || now - g_liveLast < std::chrono::milliseconds(250)) {
			return;
		}
		g_liveLast = now;
		g_liveQueued = true;
		SKSE::GetTaskInterface()->AddTask([] {
			EvaluateLive();
			g_liveQueued = false;
		});
	}

	void FireNow(Rule rule, Presets presets)
	{
		SKSE::GetTaskInterface()->AddTask([rule = std::move(rule), presets = std::move(presets)]() {
			RuleSet      set;
			Compiler     compiler(set);
			CompiledRule c;
			std::string  error;
			if (!CompileRule(set, compiler, rule, presets, "(editor)", 0, c, error)) {
				logger::warn("CastOn: Fire now '{}': {}", rule.name, error);
				return;
			}
			RE::NiPointer<RE::Actor> hold;
			const auto               ctx = PlayerContext(hold);
			if (!ctx.dodger) {
				return;
			}
			Memo       memo;
			const bool holds = Eval(set, c.root, ctx, memo);
			logger::info("CastOn: Fire now '{}' (its conditions {} now)", c.name, holds ? "hold" : "do not hold");
			Execute(c, ctx);
		});
	}

	void ReloadInGame()
	{
		SKSE::GetTaskInterface()->AddTask([] { Load(); });
	}

	void UpdateChannels(float dt)
	{
		std::erase_if(g_channels, [&](Channel& c) {
			const auto ref = c.caster.get();
			auto*      actor = ref ? ref->As<RE::Actor>() : nullptr;
			auto*      magicCaster = actor ? actor->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant) : nullptr;
			if (!actor || actor->IsDead() || !magicCaster) {
				DevLog("CastOn '{}': concentration {:08X} forgotten: its caster is gone or dead", c.rule, c.spell->GetFormID());
				return true;
			}
			if (magicCaster->currentSpell != c.spell) {  // its instant caster went on to something else: not ours to stop
				DevLog("CastOn '{}': {:08X} concentration {:08X} forgotten after {:.2f}s: the instant caster now holds {:08X} (state {})",
					c.rule, actor->GetFormID(), c.spell->GetFormID(), c.duration - c.left,
					magicCaster->currentSpell ? magicCaster->currentSpell->GetFormID() : 0u, static_cast<int>(magicCaster->state.underlying()));
				return true;
			}
			c.left -= dt;
			bool spent = false;
			if (c.drained != RE::ActorValue::kNone && !(actor->IsPlayerRef() && RE::PlayerCharacter::IsGodMode())) {
				// Engine: what ActorMagicCaster::Update would drain (cost per second x dt, clamped to what is left)
				auto*       owner = actor->AsActorValueOwner();
				const float available = owner->GetActorValue(c.drained);
				const float amount = std::min(c.perSecond * dt, std::max(0.0f, available));
				if (amount > 0.0f) {
					owner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, c.drained, -amount);
				}
				spent = available - amount <= 0.0f;
			}
			if (c.left > 0.0f && !spent) {
				return false;
			}
			magicCaster->InterruptCast(false);
			DevLog("CastOn '{}': {:08X} concentration {:08X} stopped ({})", c.rule, actor->GetFormID(), c.spell->GetFormID(),
				spent ? "nothing left to pay" : "its duration is over");
			return true;
		});
	}

	void ClearChannels()
	{
		g_channels.clear();
	}

	RuleSetPtr Current()
	{
		std::scoped_lock lock(g_lock);
		return g_current;
	}

	// ---------------------------------------------------------------------------------------------------- scheduler
	void Scheduler::OnTrigger(Trigger trigger, const Context& ctx, double now)
	{
		if (trigger == Trigger::Dodge) {
			seen_.clear();
			clipPlaces_ = false;
		}
		const auto set = Current();
		if (!set || !Settings::Get().castOn || !ctx.dodger) {
			return;
		}
		const auto& list = set->byTrigger[static_cast<int>(trigger)];
		if (list.empty()) {
			return;
		}
		const auto start = std::chrono::steady_clock::now();
		Memo       memo;
		int        passed = 0;
		for (const auto index : list) {
			const auto& rule = set->rules[index];
			if (const auto it = lastFired_.find(rule.key); rule.limits.cooldown > 0.0f && it != lastFired_.end() && now - it->second < rule.limits.cooldown) {
				continue;
			}
			if (rule.limits.oncePerChain && firedInChain_.contains(rule.key)) {
				continue;
			}
			if (!Eval(*set, rule.root, ctx, memo) || !Roll(rule.limits.chance)) {
				continue;
			}
			++passed;
			lastFired_[rule.key] = now;
			firedInChain_.insert(rule.key);
			Pending p;
			p.set = set;
			p.rule = index;
			p.trigger = trigger;
			p.dodger = ctx.dodger->GetHandle();
			p.attacker = ctx.attacker ? ctx.attacker->GetHandle() : RE::ObjectRefHandle{};
			p.combatTarget = ctx.combatTarget ? ctx.combatTarget->GetHandle() : RE::ObjectRefHandle{};
			p.facts = ctx;
			p.triggeredAt = now;
			if (trigger == Trigger::PerfectDodge && rule.timing.annotation) {
				for (const auto& [t, tag] : seen_) {  // the clip's moment for it came before the hit: fire at the hit
					if (t == trigger && TagMatches(rule.timing.tag, tag)) {
						p.annotationArrived = true;
					}
				}
			}
			pending_.push_back(std::move(p));
			DevLog("CastOn: '{}' ({}) passed on {} of {:08X}", rule.name, rule.file, trigger == Trigger::Dodge ? "Dodge" : "PerfectDodge",
				ctx.dodger->GetFormID());
			if (rule.exclusive) {
				break;
			}
		}
		const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
		DevLog("CastOn: {} rule(s) of {} evaluated in {} us, {} passed", list.size(), trigger == Trigger::Dodge ? "Dodge" : "PerfectDodge", us, passed);
	}

	void Scheduler::OnAnnotation(Trigger trigger, std::string_view tag)
	{
		seen_.emplace_back(trigger, std::string(tag));
		for (auto& p : pending_) {
			const auto& rule = p.set->rules[p.rule];
			if (p.trigger == trigger && rule.timing.annotation && TagMatches(rule.timing.tag, tag)) {
				p.annotationArrived = true;
			}
		}
	}

	bool Scheduler::Waits(const Pending& p, double now, bool entered) const
	{
		if (p.annotationArrived) {
			return false;
		}
		const auto& timing = p.set->rules[p.rule].timing;
		if (timing.annotation) {
			if (p.trigger == Trigger::Dodge && !entered) {
				return true;  // the clip's RDF_CastOnClip declaration is not known yet
			}
			if (clipPlaces_) {
				return true;  // the clip places the moment: its annotation (or the dodge's end)
			}
		}
		return now < p.triggeredAt + timing.delay;
	}

	void Scheduler::Update(RE::Actor*, double now, bool entered)
	{
		if (pending_.empty()) {
			return;
		}
		std::vector<Pending> due;
		std::erase_if(pending_, [&](Pending& p) {
			if (Waits(p, now, entered)) {
				return false;
			}
			due.push_back(std::move(p));
			return true;
		});
		for (const auto& p : due) {
			Fire(p);
		}
	}

	void Scheduler::OnDodgeEnd(RE::Actor*)
	{
		auto due = std::exchange(pending_, {});
		for (const auto& p : due) {
			Fire(p);
		}
		clipPlaces_ = false;
	}

	void Scheduler::CancelDodge()
	{
		std::erase_if(pending_, [&](const Pending& p) {
			if (p.trigger != Trigger::Dodge) {
				return false;
			}
			const auto key = p.set->rules[p.rule].key;
			lastFired_.erase(key);
			firedInChain_.erase(key);
			return true;
		});
	}

	void Scheduler::Clear()
	{
		pending_.clear();
		lastFired_.clear();
		firedInChain_.clear();
		seen_.clear();
		clipPlaces_ = false;
	}

	void Scheduler::Fire(const Pending& p)
	{
		// the participants again, from their handles: one may have been unloaded since the trigger
		const auto dodger = p.dodger.get();
		const auto attacker = p.attacker.get();
		const auto target = p.combatTarget.get();
		Context    ctx = p.facts;
		ctx.dodger = dodger ? dodger->As<RE::Actor>() : nullptr;
		ctx.attacker = attacker ? attacker->As<RE::Actor>() : nullptr;
		ctx.combatTarget = target ? target->As<RE::Actor>() : nullptr;
		Execute(p.set->rules[p.rule], ctx);
	}
}
