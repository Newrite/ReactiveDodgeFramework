#include "ActionCapture.h"

#include "Diagnostics.h"

#include <algorithm>
#include <array>
#include <optional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// wingdi.h defines GetObject as a macro, which breaks BGSDefaultObjectManager::GetObject.
#undef GetObject

namespace rdf::game::capture
{
	namespace
	{
		constexpr std::uint32_t kPerformActionId = 37996;  // ActorMediator::PerformAction(mediator, BGSActionData*)

		// Every caller of PerformAction (IDA, 1.5.97): function id, offset of the call (E8) or tail jump (E9).
		struct Site
		{
			std::uint32_t id;
			std::ptrdiff_t offset;
		};
		constexpr std::array kSites{
			Site{ 36698, 0xB9 },   // PlayerCharacter (0x1405F9F80)
			Site{ 36699, 0xC6 },   // Character (0x1405FA080)
			Site{ 36764, 0x11C },  // Character (0x1405FE890)
			Site{ 36765, 0x131 },  // Actor (0x1405FEA10)
			Site{ 36766, 0x155 },  // Actor (0x1405FEBA0)
			Site{ 36768, 0x11F },  // Actor (0x1405FF010)
			Site{ 36771, 0x11B },  // Actor (0x1405FF1C0)
			Site{ 37035, 0x208 },  // Actor (0x14060FC60)
			Site{ 37842, 0xD0 },   // 0x1406339B0
			Site{ 37997, 0x36 },   // ActorMediator::Handle (0x14063AFE0)
			Site{ 40551, 0x0A },   // TESActionData::Process (0x1406E6120), tail jump
		};

		using PerformAction_t = bool (*)(void*, RE::BGSActionData*);
		std::array<PerformAction_t, kSites.size()> g_original{};

		thread_local bool g_replaying = false;

		struct Attempt
		{
			bool                         power{ false };
			RE::BGSAction*               action{ nullptr };
			RE::TESIdleForm*             idle{ nullptr };
			RE::ObjectRefHandle          target;
		};
		struct ActorState
		{
			std::vector<CommandKind> fresh;  // not yet taken by the agent
			std::optional<Attempt>   last[2];  // [power]
		};

		std::mutex                                   g_lock;
		std::unordered_set<std::uint32_t>            g_capturing;
		std::unordered_map<std::uint32_t, ActorState> g_actors;

		RE::BGSAction* DefaultAction(RE::DEFAULT_OBJECT object)
		{
			auto* manager = RE::BGSDefaultObjectManager::GetSingleton();
			return manager ? manager->GetObject<RE::BGSAction>(object) : nullptr;
		}

		enum class Kind
		{
			None,
			Attack,
			Power,
		};

		// The vanilla attack actions by name; anything else (idles, framework actions) is an attack when the actor's race
		// has attack data for the animation event it plays, and a power attack when that attack data says so. Killmoves
		// (pa_*) are paired idles, never held back.
		Kind AttackKindOf(RE::Actor* actor, const RE::BSFixedString& animEvent);

		Kind Classify(RE::Actor* actor, const RE::BGSActionData* data)
		{
			using D = RE::DEFAULT_OBJECT;
			static const std::array<RE::BGSAction*, 3> power{ DefaultAction(D::kActionRightPowerAttack),
				DefaultAction(D::kActionLeftPowerAttack), DefaultAction(D::kActionDualPowerAttack) };
			static const std::array<RE::BGSAction*, 3> normal{ DefaultAction(D::kActionRightAttack), DefaultAction(D::kActionLeftAttack),
				DefaultAction(D::kActionDualAttack) };
			if (data->action) {
				if (std::find(power.begin(), power.end(), data->action) != power.end()) {
					return Kind::Power;
				}
				if (std::find(normal.begin(), normal.end(), data->action) != normal.end()) {
					return Kind::Attack;
				}
			}
			return AttackKindOf(actor, data->animEvent);
		}

		// Of an idle that becomes an attack (the actor's race has attack data for its animation event).
		Kind AttackKindOf(RE::Actor* actor, const RE::BSFixedString& animEvent)
		{
			const std::string_view event = animEvent.c_str();
			if (event.empty() || (event.size() >= 3 && _strnicmp(event.data(), "pa_", 3) == 0)) {
				return Kind::None;
			}
			const auto* race = actor->GetRace();
			const auto* map = race ? race->attackDataMap.get() : nullptr;
			if (!map) {
				return Kind::None;
			}
			const auto it = map->attackDataMap.find(animEvent);
			if (it == map->attackDataMap.end() || !it->second) {
				return Kind::None;
			}
			return it->second->data.flags.any(RE::AttackData::AttackFlag::kPowerAttack) ? Kind::Power : Kind::Attack;
		}

		// The variants an idle's mod chooses between: its siblings in the idle tree, or, for a top-level idle (BFCO's
		// script idles have no parent), the other top-level idles of the same plugin. Attacks of the same kind only.
		// Cached per idle (main thread).
		const std::vector<RE::TESIdleForm*>& Variants(RE::Actor* actor, RE::TESIdleForm* held, Kind kind)
		{
			static std::unordered_map<RE::FormID, std::vector<RE::TESIdleForm*>> cache;
			const auto [it, added] = cache.try_emplace(held->GetFormID());
			if (!added) {
				return it->second;
			}
			auto& out = it->second;
			const auto consider = [&](RE::TESIdleForm* idle) {
				if (idle && idle != held && AttackKindOf(actor, idle->animEventName) == kind) {
					out.push_back(idle);
				}
			};
			if (const auto* siblings = held->parentIdle ? held->parentIdle->childIdles : nullptr) {
				for (std::uint32_t i = 0; i < siblings->size(); ++i) {
					consider((*siblings)[i] ? (*siblings)[i]->As<RE::TESIdleForm>() : nullptr);
				}
			} else if (auto* handler = RE::TESDataHandler::GetSingleton(); handler && held->GetFile(0)) {
				for (auto* idle : handler->GetFormArray<RE::TESIdleForm>()) {
					if (idle && !idle->parentIdle && idle->GetFile(0) == held->GetFile(0)) {
						consider(idle);
					}
				}
			}
			DevLog("{:08X}: idle {:08X} has {} variant(s) of the same kind", actor->GetFormID(), held->GetFormID(), out.size());
			return out;
		}

		// The idle to replay now. The held one was chosen by its mod from the conditions at the press (BFCO picks a
		// directional power attack from the stick and the facing, which the dodge changes); if they no longer hold, the
		// first variant whose conditions hold now wins, as the mod itself would have chosen at this moment.
		RE::TESIdleForm* ChooseIdle(RE::Actor* actor, RE::TESIdleForm* held, bool power)
		{
			const auto holds = [&](RE::TESIdleForm* idle) { return idle && idle->conditions.IsTrue(actor, actor); };
			if (holds(held)) {
				return held;
			}
			for (auto* idle : Variants(actor, held, power ? Kind::Power : Kind::Attack)) {
				if (holds(idle)) {
					DevLog("{:08X}: held-back idle {:08X} no longer applies; variant {:08X} ('{}') does", actor->GetFormID(),
						held->GetFormID(), idle->GetFormID(), idle->animEventName.c_str());
					return idle;
				}
			}
			DevLog("{:08X}: held-back idle {:08X} no longer applies, nor any variant", actor->GetFormID(), held->GetFormID());
			return held;  // the game decides (and will likely refuse)
		}

		// Any thread (the idle callback may run off the main one). True = held back.
		bool HoldBack(RE::BGSActionData* data)
		{
			if (g_replaying || !data) {
				return false;
			}
			auto* ref = data->source.get();
			auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
			if (!actor) {
				return false;
			}
			const auto id = actor->GetFormID();
			{
				std::scoped_lock lock(g_lock);
				if (!g_capturing.contains(id)) {
					return false;
				}
			}
			const auto kind = Classify(actor, data);
			if (kind == Kind::None) {
				return false;
			}
			Attempt attempt;
			attempt.power = kind == Kind::Power;
			attempt.action = data->action;
			attempt.idle = const_cast<RE::TESIdleForm*>(data->animObjIdle);
			if (auto* target = data->target.get()) {
				attempt.target = target->GetHandle();
			}
			std::scoped_lock lock(g_lock);
			auto& s = g_actors[id];
			s.fresh.push_back(attempt.power ? CommandKind::PowerAttack : CommandKind::Attack);
			s.last[attempt.power ? 1 : 0] = attempt;
			return true;
		}

		template <std::size_t N>
		bool Thunk(void* mediator, RE::BGSActionData* data)
		{
			if (HoldBack(data)) {
				const auto* ref = data->source.get();
				DevLog("{:08X}: attack attempt held back by the dodge (action {:08X}, event '{}', idle {:08X})", ref ? ref->GetFormID() : 0u,
					data->action ? data->action->GetFormID() : 0u, data->animEvent.c_str(), data->animObjIdle ? data->animObjIdle->GetFormID() : 0u);
				return false;
			}
			return g_original[N](mediator, data);
		}

		template <std::size_t N>
		void InstallOne(std::uintptr_t vanilla)
		{
			const auto site = REL::Relocation<std::uintptr_t>{ REL::ID(kSites[N].id) }.address() + kSites[N].offset;
			const auto opcode = *reinterpret_cast<const std::uint8_t*>(site);
			auto&      trampoline = SKSE::GetTrampoline();
			if (opcode == 0xE8) {
				g_original[N] = reinterpret_cast<PerformAction_t>(trampoline.write_call<5>(site, Thunk<N>));
			} else if (opcode == 0xE9) {
				g_original[N] = reinterpret_cast<PerformAction_t>(trampoline.write_branch<5>(site, Thunk<N>));
			} else {
				logger::error("action capture: site {} (id {} +{:#x}) is not a call ({:02X}); attempts through it are not held back", N,
					kSites[N].id, kSites[N].offset, opcode);
				return;
			}
			if (reinterpret_cast<std::uintptr_t>(g_original[N]) != vanilla) {
				logger::info("action capture: site {} chains to another plugin's hook {:#x}", N, reinterpret_cast<std::uintptr_t>(g_original[N]));
			}
		}

		template <std::size_t... I>
		void InstallAll(std::uintptr_t vanilla, std::index_sequence<I...>)
		{
			(InstallOne<I>(vanilla), ...);
		}
	}

	void Install()
	{
		const REL::Relocation<std::uintptr_t> performAction{ REL::ID(kPerformActionId) };
		InstallAll(performAction.address(), std::make_index_sequence<kSites.size()>{});
		logger::info("action capture installed on {} callers of ActorMediator::PerformAction", kSites.size());
	}

	void SetCapturing(std::uint32_t formID, bool on)
	{
		std::scoped_lock lock(g_lock);
		if (on) {
			g_capturing.insert(formID);
		} else {
			g_capturing.erase(formID);
		}
	}

	std::vector<CommandKind> TakeNew(std::uint32_t formID)
	{
		std::scoped_lock lock(g_lock);
		const auto it = g_actors.find(formID);
		return it == g_actors.end() ? std::vector<CommandKind>{} : std::exchange(it->second.fresh, {});
	}

	bool Replay(RE::Actor* actor, bool power)
	{
		if (!actor) {
			return false;
		}
		std::optional<Attempt> attempt;
		{
			std::scoped_lock lock(g_lock);
			const auto it = g_actors.find(actor->GetFormID());
			if (it == g_actors.end()) {
				return false;
			}
			auto& s = it->second;
			attempt = s.last[power ? 1 : 0] ? s.last[power ? 1 : 0] : s.last[power ? 0 : 1];
			s.last[0].reset();
			s.last[1].reset();
		}
		if (!attempt) {
			return false;
		}
		const auto target = attempt->target.get();
		bool       ok = false;
		g_replaying = true;
		if (attempt->idle) {
			if (auto* process = actor->GetActorRuntimeData().currentProcess) {
				attempt->idle = ChooseIdle(actor, attempt->idle, attempt->power);
				ok = process->PlayIdle(actor, attempt->idle, target.get());
			}
		} else if (attempt->action) {
			const std::unique_ptr<RE::TESActionData> data(RE::TESActionData::Create());
			data->source = RE::NiPointer<RE::TESObjectREFR>(actor);
			data->target = target;
			data->action = attempt->action;
			ok = data->Process();
		}
		g_replaying = false;
		DevLog("{:08X}: replayed the held-back {} ({} {:08X}): {}", actor->GetFormID(), attempt->power ? "power attack" : "attack",
			attempt->idle ? "idle" : "action", attempt->idle ? attempt->idle->GetFormID() : (attempt->action ? attempt->action->GetFormID() : 0u),
			ok ? "accepted" : "refused");
		return ok;
	}

	void Forget(std::uint32_t formID)
	{
		std::scoped_lock lock(g_lock);
		g_capturing.erase(formID);
		g_actors.erase(formID);
	}

	void Clear()
	{
		std::scoped_lock lock(g_lock);
		g_capturing.clear();
		g_actors.clear();
	}
}
