#include "ActionCapture.h"

#include "Diagnostics.h"

#include <algorithm>
#include <array>
#include <optional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

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
			bool                     sprintWanted{ false };  // a sprint the dodge interrupted or held back, not stopped since
		};

		std::mutex                                   g_lock;
		std::unordered_set<std::uint32_t>            g_capturing;
		std::unordered_set<std::uint32_t>            g_holdingSprint;  // sprint starts held back (capturing, or an attack still to show)
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
			SprintStart,
			SprintStop,
		};

		// The vanilla attack actions by name; anything else (idles, framework actions) is an attack when the actor's race
		// has attack data for the animation event it plays, and a power attack when that attack data says so. Killmoves
		// (pa_*) are paired idles, never held back.
		Kind Classify(RE::Actor* actor, const RE::BGSActionData* data)
		{
			using D = RE::DEFAULT_OBJECT;
			static const std::array<RE::BGSAction*, 3> power{ DefaultAction(D::kActionRightPowerAttack),
				DefaultAction(D::kActionLeftPowerAttack), DefaultAction(D::kActionDualPowerAttack) };
			static const std::array<RE::BGSAction*, 3> normal{ DefaultAction(D::kActionRightAttack), DefaultAction(D::kActionLeftAttack),
				DefaultAction(D::kActionDualAttack) };
			static RE::BGSAction* const sprintStart = DefaultAction(D::kActionSprintStart);
			static RE::BGSAction* const sprintStop = DefaultAction(D::kActionSprintStop);
			if (data->action && data->action == sprintStart) {
				return Kind::SprintStart;
			}
			if (data->action && data->action == sprintStop) {
				return Kind::SprintStop;
			}
			if (data->action) {
				if (std::find(power.begin(), power.end(), data->action) != power.end()) {
					return Kind::Power;
				}
				if (std::find(normal.begin(), normal.end(), data->action) != normal.end()) {
					return Kind::Attack;
				}
			}
			const std::string_view event = data->animEvent.c_str();
			if (event.empty() || (event.size() >= 3 && _strnicmp(event.data(), "pa_", 3) == 0)) {
				return Kind::None;
			}
			const auto* race = actor->GetRace();
			const auto* map = race ? race->attackDataMap.get() : nullptr;
			if (!map) {
				return Kind::None;
			}
			const auto it = map->attackDataMap.find(data->animEvent);
			if (it == map->attackDataMap.end() || !it->second) {
				return Kind::None;
			}
			return it->second->data.flags.any(RE::AttackData::AttackFlag::kPowerAttack) ? Kind::Power : Kind::Attack;
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
			bool       capturing = false;
			bool       holdingSprint = false;
			bool       tracked = false;
			{
				std::scoped_lock lock(g_lock);
				capturing = g_capturing.contains(id);
				holdingSprint = capturing || g_holdingSprint.contains(id);
				tracked = g_actors.contains(id);
			}
			if (!capturing && !tracked) {
				return false;
			}
			const auto kind = Classify(actor, data);
			// Sprint (the dodge button is often the sprint button): no sprint starts while the dodge captures; whether
			// the actor still wants one at the dodge's end is followed through the whole dodge, exit included (a stop
			// always passes and forgets it).
			if (kind == Kind::SprintStart || kind == Kind::SprintStop) {
				std::scoped_lock lock(g_lock);
				g_actors[id].sprintWanted = kind == Kind::SprintStart;
				if (kind == Kind::SprintStart && holdingSprint) {
					DevLog("{:08X}: sprint start held back by the dodge", id);
					return true;
				}
				return false;
			}
			if (kind == Kind::None || !capturing) {
				if (capturing) {
					// dev: what else is tried during a dodge (an attack framework's action or idle we do not classify)
					DevLog("{:08X}: action passed during the dodge (action {:08X}, event '{}', idle {:08X})", id,
						data->action ? data->action->GetFormID() : 0u, data->animEvent.c_str(), data->animObjIdle ? data->animObjIdle->GetFormID() : 0u);
				}
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
				if (data->action && data->action == DefaultAction(RE::DEFAULT_OBJECT::kActionSprintStart)) {
					return false;
				}
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

	void HoldSprint(std::uint32_t formID, bool on)
	{
		std::scoped_lock lock(g_lock);
		if (on) {
			g_holdingSprint.insert(formID);
		} else {
			g_holdingSprint.erase(formID);
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

	namespace
	{
		bool PerformDefault(RE::Actor* actor, RE::DEFAULT_OBJECT object)
		{
			auto* action = DefaultAction(object);
			if (!actor || !action) {
				return false;
			}
			const std::unique_ptr<RE::TESActionData> data(RE::TESActionData::Create());
			data->source = RE::NiPointer<RE::TESObjectREFR>(actor);
			data->action = action;
			g_replaying = true;
			const bool ok = data->Process();
			g_replaying = false;
			return ok;
		}
	}

	bool StopSprint(RE::Actor* actor, bool remember)
	{
		if (!actor || !actor->AsActorState()->IsSprinting()) {
			return false;
		}
		const bool ok = PerformDefault(actor, RE::DEFAULT_OBJECT::kActionSprintStop);
		if (remember) {
			std::scoped_lock lock(g_lock);
			g_actors[actor->GetFormID()].sprintWanted = true;
		}
		DevLog("{:08X}: sprint stopped for the dodge: {} (still sprinting: {})", actor->GetFormID(), ok ? "accepted" : "refused",
			actor->AsActorState()->IsSprinting());
		return ok;
	}

	void FinishSprint(RE::Actor* actor, bool resume)
	{
		if (!actor) {
			return;
		}
		bool wanted = false;
		{
			std::scoped_lock lock(g_lock);
			if (const auto it = g_actors.find(actor->GetFormID()); it != g_actors.end()) {
				wanted = std::exchange(it->second.sprintWanted, false);
			}
		}
		if (resume && wanted) {
			const bool ok = PerformDefault(actor, RE::DEFAULT_OBJECT::kActionSprintStart);
			DevLog("{:08X}: sprint resumed after the dodge: {}", actor->GetFormID(), ok ? "accepted" : "refused");
		} else if (actor->AsActorState()->IsSprinting()) {
			// a stop the graph refused while it was in the dodge
			const bool ok = PerformDefault(actor, RE::DEFAULT_OBJECT::kActionSprintStop);
			DevLog("{:08X}: stale sprint stopped after the dodge: {}", actor->GetFormID(), ok ? "accepted" : "refused");
		}
	}

	void Forget(std::uint32_t formID)
	{
		std::scoped_lock lock(g_lock);
		g_capturing.erase(formID);
		g_holdingSprint.erase(formID);
		g_actors.erase(formID);
	}

	void Clear()
	{
		std::scoped_lock lock(g_lock);
		g_capturing.clear();
		g_holdingSprint.clear();
		g_actors.clear();
	}
}
