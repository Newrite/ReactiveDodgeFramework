#include "GraphCoordinator.h"

#include "GraphGate.h"
#include "Settings.h"

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace rdf::game::graph
{
	namespace
	{
		struct Names
		{
			RE::BSFixedString start, startB, start1H, startB1H, chain, exit, entered, left, clip, speed, armBlend, isDodging, chainIndex, attackFromDodge, attackFromDodgeClip;
			RE::BSFixedString startBow, startBowB, exitBow, startMagic, startMagicB, exitMagic, layer;
			std::vector<RE::BSFixedString> windowOpen, windowClose;
			// [AttackCancel]
			std::vector<RE::BSFixedString> recovery, recoveryEnd, attackStart;
			RE::BSFixedString              cancelOpen{ "RDF_DodgeCancelOpen" }, cancelClose{ "RDF_DodgeCancelClose" };
			// [Stagger]
			std::vector<RE::BSFixedString> staggerOpen, staggerClose, staggerStart;
		};
		Names g_names;

		// Clip annotation names (ANIMATION_CONTRACT.md), indexed by WindowEvent.
		// Attack-start events closer together than this belong to one attack.
		constexpr auto kAttackStartBurst = std::chrono::milliseconds(150);

		constexpr std::array kWindowEventNames{ "RDF_Windows"sv, "RDF_IFrameStart"sv, "RDF_IFrameEnd"sv,
			"RDF_ChainOpen"sv, "RDF_AttackOpen"sv, "RDF_BlockOpen"sv, "RDF_ExitOpen"sv, "RDF_PerfectStart"sv, "RDF_PerfectEnd"sv };
		std::array<RE::BSFixedString, kWindowEventNames.size()> g_windowNames;

		struct ActorGraph
		{
			std::uint32_t entered{ 0 };
			int           depth{ 0 };
			bool          window{ false };
			std::uint32_t clipWindows{ 0 };  // bit per WindowEvent seen since the last drain
			// the actor's current attack (attack frameworks, attack clip annotations); start over with each attack
			bool                attackRecovery{ false };
			std::optional<bool> attackCancel;
			std::uint32_t       attackStarts{ 0 };  // attacks started (a new attack after a known one)
			std::chrono::steady_clock::time_point lastAttackStart{};
			bool                                  staggerRecovery{ false };  // the stagger's recovery window is open
			// Several graphs (the player's first / third person): only this one counts; null = every graph.
			const RE::BSTEventSource<RE::BSAnimationGraphEvent>* activeSource{ nullptr };
			std::vector<std::pair<int, std::string>>              castOn;  // CastOn annotations since the last drain
			std::vector<std::pair<int, std::string>>              motion;  // RDF_Motion (0) / ClipLength (1) / Speed (2) payloads
		};

		// "RDF_CastOnDodge" with payload "tag", or the whole "RDF_CastOnDodge.tag" as the tag when the graph did not split it.
		constexpr std::array kCastOnNames{ "RDF_CastOnDodge"sv, "RDF_CastOnPerfectDodge"sv, "RDF_CastOnClip"sv };
		// The clip's motion (ANIMATION_CONTRACT.md 5): "RDF_Motion.<numbers>", "RDF_ClipLength.<seconds>", "RDF_Speed.<x>",
		// split the same way.
		constexpr std::array kMotionNames{ "RDF_Motion"sv, "RDF_ClipLength"sv, "RDF_Speed"sv };

		template <std::size_t N>
		bool PayloadEvent(const std::array<std::string_view, N>& names, std::string_view tag, std::string_view payload, int& kind,
			std::string& out)
		{
			for (int i = 0; i < static_cast<int>(names.size()); ++i) {
				const auto name = names[i];
				if (tag.size() >= name.size() && _strnicmp(tag.data(), name.data(), name.size()) == 0 &&
					(tag.size() == name.size() || tag[name.size()] == '.')) {
					kind = i;
					out = tag.size() > name.size() ? std::string(tag.substr(name.size() + 1)) : std::string(payload);
					return true;
				}
			}
			return false;
		}

		// Graph events come from the threads that update the graphs; the main thread reads. Keyed by ref handle.
		std::mutex                                    g_lock;
		std::unordered_map<std::uint32_t, ActorGraph> g_actors;
		std::atomic<bool>                             g_logEvents{ false };  // dev: log every graph event

		std::uint32_t HandleOf(const RE::TESObjectREFR* ref)
		{
			return ref ? const_cast<RE::TESObjectREFR*>(ref)->GetHandle().native_handle() : 0;
		}

		bool Contains(const std::vector<RE::BSFixedString>& list, const RE::BSFixedString& tag)
		{
			for (const auto& n : list) {
				if (n == tag) {
					return true;
				}
			}
			return false;
		}

		int WindowIndex(const RE::BSFixedString& tag)
		{
			for (std::size_t i = 0; i < g_windowNames.size(); ++i) {
				if (g_windowNames[i] == tag) {
					return static_cast<int>(i);
				}
			}
			return -1;
		}

		class Sink final : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* event, RE::BSTEventSource<RE::BSAnimationGraphEvent>* source) override
			{
				if (!event || !event->holder) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto       handle = HandleOf(event->holder);
				std::scoped_lock lock(g_lock);
				const auto       it = g_actors.find(handle);
				if (it == g_actors.end()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto& g = it->second;
				// The player's third- and first-person graphs both run the dodge states; only the one in use
				// counts, so a dodge is entered and left once.
				if (g.activeSource && source != g.activeSource) {
					return RE::BSEventNotifyControl::kContinue;
				}
				if (g_logEvents.load(std::memory_order_relaxed)) {
					logger::info("graph event {:08X} {} '{}' (depth {})", event->holder->GetFormID(), event->tag.c_str(),
						event->payload.c_str(), g.depth);
				}
				if (Contains(g_names.attackStart, event->tag)) {
					g.attackRecovery = false;
					g.attackCancel.reset();
					// One attack can send its start event more than once (BFCO's Bfco_AttackStartFX twice, ~20 ms apart):
					// a burst counts as one start.
					const auto now = std::chrono::steady_clock::now();
					if (now - g.lastAttackStart > kAttackStartBurst) {
						++g.attackStarts;
					}
					g.lastAttackStart = now;
				} else if (Contains(g_names.recovery, event->tag)) {
					g.attackRecovery = true;
				} else if (Contains(g_names.recoveryEnd, event->tag)) {
					g.attackRecovery = false;
				} else if (event->tag == g_names.cancelOpen) {
					g.attackCancel = true;
				} else if (event->tag == g_names.cancelClose) {
					g.attackCancel = false;
				}
				if (Contains(g_names.staggerStart, event->tag) || Contains(g_names.staggerClose, event->tag)) {
					g.staggerRecovery = false;
				} else if (Contains(g_names.staggerOpen, event->tag)) {
					g.staggerRecovery = true;
				}
				int         castOnKind = 0;
				std::string castOnTag;
				if (PayloadEvent(kCastOnNames, event->tag.c_str(), event->payload.c_str(), castOnKind, castOnTag)) {
					g.castOn.emplace_back(castOnKind, std::move(castOnTag));
				} else if (PayloadEvent(kMotionNames, event->tag.c_str(), event->payload.c_str(), castOnKind, castOnTag)) {
					g.motion.emplace_back(castOnKind, std::move(castOnTag));
				}
				if (event->tag == g_names.entered) {
					++g.entered;
					++g.depth;
				} else if (event->tag == g_names.left) {
					g.depth = std::max(0, g.depth - 1);
				} else if (const int w = WindowIndex(event->tag); w >= 0) {
					g.clipWindows |= 1u << w;
				} else if (Contains(g_names.windowOpen, event->tag)) {
					g.window = true;
				} else if (Contains(g_names.windowClose, event->tag)) {
					g.window = false;
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		Sink g_sink;

		template <class F>
		auto WithActor(RE::Actor* actor, F&& f)
		{
			std::scoped_lock lock(g_lock);
			return f(g_actors[HandleOf(actor)]);
		}
	}

	void Init()
	{
		const auto& g = Settings::Get().graph;
		g_names.start = g.start;
		g_names.startB = g.startB;
		g_names.start1H = g.start1H;
		g_names.startB1H = g.startB1H;
		g_names.startBow = g.startBow;
		g_names.startBowB = g.startBowB;
		g_names.exitBow = g.exitBow;
		g_names.startMagic = g.startMagic;
		g_names.startMagicB = g.startMagicB;
		g_names.exitMagic = g.exitMagic;
		g_names.layer = g.layer;
		g_names.chain = g.chain;
		g_names.exit = g.exit;
		g_names.entered = g.entered;
		g_names.left = g.left;
		g_names.clip = g.clip;
		g_names.speed = g.speed;
		g_names.armBlend = g.armBlend;
		g_names.isDodging = g.isDodging;
		g_names.chainIndex = g.chainIndex;
		g_names.attackFromDodge = g.attackFromDodge;
		g_names.attackFromDodgeClip = g.attackFromDodgeClip;
		for (std::size_t i = 0; i < kWindowEventNames.size(); ++i) {
			g_windowNames[i] = kWindowEventNames[i];
		}
		for (const auto& n : g.windowOpen) {
			g_names.windowOpen.emplace_back(n);
		}
		for (const auto& n : g.windowClose) {
			g_names.windowClose.emplace_back(n);
		}
		const auto& ac = Settings::Get().attackCancel;
		for (const auto& n : ac.recoveryEvents) {
			g_names.recovery.emplace_back(n);
		}
		for (const auto& n : ac.recoveryEndEvents) {
			g_names.recoveryEnd.emplace_back(n);
		}
		const auto& st = Settings::Get().stagger;
		g_names.staggerOpen.clear();
		g_names.staggerClose.clear();
		g_names.staggerStart.clear();
		for (const auto& n : st.recoveryOpenEvents) {
			g_names.staggerOpen.emplace_back(n);
		}
		for (const auto& n : st.recoveryCloseEvents) {
			g_names.staggerClose.emplace_back(n);
		}
		for (const auto& n : st.staggerStartEvents) {
			g_names.staggerStart.emplace_back(n);
		}
		for (const auto& n : ac.attackStartEvents) {
			g_names.attackStart.emplace_back(n);
		}
	}

	void RegisterSink(RE::Actor* actor)
	{
		if (!actor) {
			return;
		}
		{
			std::scoped_lock lock(g_lock);
			g_actors.try_emplace(HandleOf(actor));
		}
		// Every graph of the actor: the player's third- and first-person ones each send their own dodge events
		// (CommonLib's AddAnimationGraphEventSink only covers the first graph).
		RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
		if (!actor->GetAnimationGraphManager(manager) || !manager || manager->graphs.empty()) {
			logger::info("animation graph sink NOT registered on {:08X} (graph not loaded yet)", actor->GetFormID());
			return;
		}
		std::uint32_t count = 0;
		for (const auto& graph : manager->graphs) {
			if (!graph) {
				continue;
			}
			auto* source = graph->GetEventSource<RE::BSAnimationGraphEvent>();
			source->RemoveEventSink(&g_sink);  // no-op when absent: guards against duplicates
			source->AddEventSink(&g_sink);
			++count;
		}
		logger::info("animation graph sink registered on {:08X}: {} graph(s)", actor->GetFormID(), count);
	}

	void UnregisterSink(RE::Actor* actor)
	{
		if (!actor) {
			return;
		}
		RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
		if (actor->GetAnimationGraphManager(manager) && manager) {
			for (const auto& graph : manager->graphs) {
				if (graph) {
					graph->GetEventSource<RE::BSAnimationGraphEvent>()->RemoveEventSink(&g_sink);
				}
			}
		}
		std::scoped_lock lock(g_lock);
		g_actors.erase(HandleOf(actor));
	}

	bool TrackActiveGraph(RE::Actor* actor)
	{
		RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
		const RE::BSTEventSource<RE::BSAnimationGraphEvent>* source = nullptr;
		if (actor->GetAnimationGraphManager(manager) && manager && manager->activeGraph < manager->graphs.size()) {
			if (const auto& graph = manager->graphs[manager->activeGraph]) {
				source = graph->GetEventSource<RE::BSAnimationGraphEvent>();
			}
		}
		return WithActor(actor, [&](ActorGraph& g) {
			if (g.activeSource == source) {
				return false;
			}
			const bool switched = g.activeSource != nullptr;
			g.activeSource = source;
			g.depth = 0;  // a switch between first and third person starts the count afresh
			return switched;
		});
	}

	bool TakeEntered(RE::Actor* actor)
	{
		return WithActor(actor, [](ActorGraph& g) { return std::exchange(g.entered, 0u) != 0; });
	}

	int Depth(RE::Actor* actor)
	{
		return WithActor(actor, [](ActorGraph& g) { return g.depth; });
	}

	void LogEvents(bool on) { g_logEvents.store(on, std::memory_order_relaxed); }

	bool DodgeWindowOpen(RE::Actor* actor)
	{
		return WithActor(actor, [](ActorGraph& g) { return g.window; });
	}

	void DrainClipWindows(RE::Actor* actor, const std::function<void(WindowEvent)>& sink)
	{
		const auto bits = WithActor(actor, [](ActorGraph& g) { return std::exchange(g.clipWindows, 0u); });
		for (std::uint32_t i = 0; i < kWindowEventNames.size(); ++i) {  // enum order: an end after its start
			if (bits & (1u << i)) {
				sink(static_cast<WindowEvent>(i));
			}
		}
	}

	void DrainCastOn(RE::Actor* actor, const std::function<void(int kind, std::string_view tag)>& sink)
	{
		const auto events = WithActor(actor, [](ActorGraph& g) { return std::exchange(g.castOn, {}); });
		for (const auto& [kind, tag] : events) {
			sink(kind, tag);
		}
	}

	void DrainClipMotion(RE::Actor* actor, const std::function<void(int kind, std::string_view payload)>& sink)
	{
		const auto events = WithActor(actor, [](ActorGraph& g) { return std::exchange(g.motion, {}); });
		for (const auto& [kind, payload] : events) {
			sink(kind, payload);
		}
	}

	void SetPlaybackSpeed(RE::Actor* actor, float speed)
	{
		actor->SetGraphVariableFloat(g_names.speed, speed);
	}

	void ClearEvents(RE::Actor* actor)
	{
		WithActor(actor, [](ActorGraph& g) {
			const auto source = g.activeSource;
			g = ActorGraph{};
			g.activeSource = source;
			return 0;
		});
	}

	AttackWindows ReadAttack(RE::Actor* actor)
	{
		return WithActor(actor, [](ActorGraph& g) { return AttackWindows{ g.attackRecovery, g.attackCancel, g.attackStarts }; });
	}

	bool StaggerRecoveryOpen(RE::Actor* actor)
	{
		if (WithActor(actor, [](ActorGraph& g) { return g.staggerRecovery ? 1 : 0; })) {
			return true;
		}
		for (const auto& name : Settings::Get().stagger.recoveryVariables) {
			bool open = false;
			if (actor->GetGraphVariableBool(name, open) && open) {
				return true;
			}
		}
		return false;
	}

	void ResetAttack(RE::Actor* actor)
	{
		WithActor(actor, [](ActorGraph& g) {
			g.attackRecovery = false;
			g.attackCancel.reset();
			return 0;
		});
	}

	void ClearAll()
	{
		std::scoped_lock lock(g_lock);
		g_actors.clear();
	}

	float ArmBlendWeight(float stanceShare)
	{
		// RDF_ArmBlend is the blend parameter of a parametric blend between the dodge (0) and the
		// stance (1), so it is the share itself (BEHAVIOR_INTEGRATION.md section 5).
		return std::clamp(stanceShare, 0.0f, 1.0f);
	}

	const char* ToString(Layer layer)
	{
		switch (layer) {
		case Layer::OneHand: return "1hm";
		case Layer::Bow: return "bow";
		case Layer::Magic: return "magic";
		default: return "root";
		}
	}

	bool SendStart(RE::Actor* actor, DodgeEntry entry, int clip, float animSpeed, float armStance, int chainIndex, Layer layer)
	{
		actor->SetGraphVariableInt(g_names.clip, clip);
		actor->SetGraphVariableInt(g_names.chainIndex, chainIndex);
		actor->SetGraphVariableFloat(g_names.speed, animSpeed);
		actor->SetGraphVariableFloat(g_names.armBlend, ArmBlendWeight(armStance));
		actor->SetGraphVariableInt(g_names.layer, static_cast<std::int32_t>(layer));
		const bool b = entry == DodgeEntry::StartB;
		switch (layer) {   // outside the root a chain is the start of the other state (the caller passes StartA / StartB)
		case Layer::OneHand: return gate::Send(actor, b ? g_names.startB1H : g_names.start1H);
		case Layer::Bow: return gate::Send(actor, b ? g_names.startBowB : g_names.startBow);
		case Layer::Magic: return gate::Send(actor, b ? g_names.startMagicB : g_names.startMagic);
		default: break;
		}
		switch (entry) {
		case DodgeEntry::Chain: return actor->NotifyAnimationGraph(g_names.chain);
		case DodgeEntry::StartB: return actor->NotifyAnimationGraph(g_names.startB);
		default: return actor->NotifyAnimationGraph(g_names.start);
		}
	}

	void SetChainIndex(RE::Actor* actor, int chainIndex)
	{
		actor->SetGraphVariableInt(g_names.chainIndex, chainIndex);
	}

	void SetAttackFromDodge(RE::Actor* actor, int kind, int clip)
	{
		actor->SetGraphVariableInt(g_names.attackFromDodge, kind);
		actor->SetGraphVariableInt(g_names.attackFromDodgeClip, clip);
	}

	void SendExit(RE::Actor* actor, Layer layer)
	{
		switch (layer) {
		case Layer::Bow: actor->NotifyAnimationGraph(g_names.exitBow); break;
		case Layer::Magic: actor->NotifyAnimationGraph(g_names.exitMagic); break;
		default: actor->NotifyAnimationGraph(g_names.exit); break;
		}
	}

	void SendAllExits(RE::Actor* actor)
	{
		actor->NotifyAnimationGraph(g_names.exit);
		actor->NotifyAnimationGraph(g_names.exitBow);
		actor->NotifyAnimationGraph(g_names.exitMagic);
	}

	void SetLayer(RE::Actor* actor, Layer layer)
	{
		actor->SetGraphVariableInt(g_names.layer, static_cast<std::int32_t>(layer));
	}

	bool GraphIsDodging(RE::Actor* actor)
	{
		bool value = false;
		actor->GetGraphVariableBool(g_names.isDodging, value);
		return value;
	}
}
