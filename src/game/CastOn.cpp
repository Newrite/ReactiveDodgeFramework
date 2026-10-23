#include "CastOn.h"

#include "AttackCancel.h"
#include "Diagnostics.h"
#include "Forms.h"
#include "Settings.h"

#include <algorithm>
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

		void FlashMeter(RE::ActorValue value)
		{
			using func_t = void (*)(RE::ActorValue);
			static REL::Relocation<func_t> func{ REL::ID(51907) };  // 0x1408D53D0: the HUD meter flash
			func(value);
		}

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
			std::map<std::string, std::pair<fs::path, fs::path>> byName;  // name -> (mod file, user file)
			for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
				if (!it->is_regular_file(ec)) {
					continue;
				}
				auto name = Utf8(it->path().filename());
				std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (name.ends_with(".user.json")) {
					byName[name.substr(0, name.size() - 10)].second = it->path();
				} else if (name.ends_with(".json")) {
					byName[name.substr(0, name.size() - 5)].first = it->path();
				}
			}
			for (const auto& [name, paths] : byName) {
				const auto& path = paths.second.empty() ? paths.first : paths.second;
				std::ifstream in(path, std::ios::binary);
				if (!in) {
					logger::warn("CastOn: cannot read {}", Utf8(path));
					continue;
				}
				out.push_back({ Utf8(path.filename()), std::string(std::istreambuf_iterator<char>(in), {}) });
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
			case ConditionKind::ActorValue: return 2;
			case ConditionKind::HasKeyword:
			case ConditionKind::HasEffectKeyword: return 3;  // the walk over worn items and effects
			default: return 1;
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
				default: break;
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

		// ------------------------------------------------------------------------------------------------ evaluation
		struct Memo  // the keyword walk once per (actor, keyword) and trigger
		{
			struct Entry
			{
				const RE::Actor*      actor;
				const RE::BGSKeyword* keyword;
				bool                  has;
			};
			std::vector<Entry> entries;

			bool Has(RE::Actor* actor, RE::BGSKeyword* keyword)
			{
				for (const auto& e : entries) {
					if (e.actor == actor && e.keyword == keyword) {
						return e.has;
					}
				}
				const bool has = attack::ActorHasKeyword(actor, keyword);
				entries.push_back({ actor, keyword, has });
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

		bool Leaf(const RuleSet& set, const Node& n, RE::Actor* actor, const Context& ctx, Memo& memo)
		{
			switch (n.kind) {
			case ConditionKind::HasKeyword: return memo.Has(actor, n.form->As<RE::BGSKeyword>());
			case ConditionKind::HasEffectKeyword:
				{
					const auto sum = attack::EffectMagnitudes(actor, n.form->As<RE::BGSKeyword>());
					return sum && Holds(n.compare, *sum, n.value);
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
		bool Pay(const Cost& cost, RE::Actor* caster, RE::MagicItem* spell, float& effectiveness, const std::string& rule)
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
				amount = (cost.flat + cost.spellShare * CalculateCost(spell, caster) + cost.maxShare * max) * cost.mult;
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
				float effectiveness = cast.effectiveness;
				if (!Pay(rule.cost, caster, cast.spell, effectiveness, rule.name)) {
					continue;
				}
				magicCaster->CastSpellImmediate(cast.spell, cast.noHitEffectArt, target, effectiveness, false, cast.magnitude, caster);
				if (rule.cost.mode == CostMode::Engine && rule.cost.realCast) {
					// what SpellCast does besides paying (0x140542470): the effects a cast breaks, then the cast event
					const auto keep = cast.spell->HasEffect(RE::EffectArchetypes::ArchetypeID::kInvisibility) ? 11 :
					                  cast.spell->HasEffect(RE::EffectArchetypes::ArchetypeID::kEtherealize)  ? 41 :
					                                                                                              -1;
					RemoveEffectsDueToCast(caster, keep);
					SendSpellCastEvent(caster, cast.spell);
				}
				DevLog("CastOn '{}': {:08X} cast {:08X} '{}' on {:08X} (effectiveness {:.2f})", rule.name, caster->GetFormID(),
					cast.spell->GetFormID(), cast.spell->GetName(), target ? target->GetFormID() : 0u, effectiveness);
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
		for (const auto& [file, json] : ReadFiles()) {
			++files;
			auto parsed = ParseRules(json);
			for (const auto& e : parsed.errors) {
				logger::warn("CastOn {}: {}", file, e);
			}
			for (std::size_t i = 0; i < parsed.rules.size(); ++i) {
				const auto& rule = parsed.rules[i];
				if (!rule.enabled) {
					continue;
				}
				CompiledRule c;
				c.name = rule.name.empty() ? std::format("#{}", i) : rule.name;
				c.file = file;
				c.key = KeyOf(file, rule.name, i);
				c.priority = rule.priority;
				c.exclusive = rule.exclusive;
				c.trigger = rule.trigger;
				c.cost = rule.cost;
				c.timing = rule.timing;
				c.limits = rule.limits;
				std::string error;
				const auto  root = compiler.Conditions(rule.conditions, error);
				for (const auto& cast : rule.casts) {
					if (!error.empty()) {
						break;
					}
					auto* form = ResolveSpell(cast.spell, error);
					auto* spell = form ? form->As<RE::MagicItem>() : nullptr;
					if (spell && spell->GetCastingType() == RE::MagicSystem::CastingType::kConstantEffect) {
						error = std::format("{} is a constant effect (an ability): it cannot be cast", cast.spell);
					} else if (spell) {
						c.casts.push_back({ spell, cast.caster, cast.target, cast.effectiveness, cast.magnitude, cast.noHitEffectArt });
					}
				}
				if (!root || !error.empty()) {
					++dropped;
					logger::warn("CastOn {}: rule '{}' dropped: {}", file, c.name, error);
					continue;
				}
				c.root = *root;
				set->rules.push_back(std::move(c));
			}
		}
		for (std::uint32_t i = 0; i < set->rules.size(); ++i) {
			set->byTrigger[static_cast<int>(set->rules[i].trigger)].push_back(i);
		}
		for (auto& list : set->byTrigger) {
			std::stable_sort(list.begin(), list.end(), [&](std::uint32_t a, std::uint32_t b) { return set->rules[a].priority > set->rules[b].priority; });
		}
		logger::info("CastOn: {} rule(s) from {} file(s) in {} ({} dropped)", set->rules.size(), files, kFolder, dropped);
		std::scoped_lock lock(g_lock);
		g_current = std::move(set);
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
