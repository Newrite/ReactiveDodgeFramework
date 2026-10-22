#include "GraphCoordinator.h"

#include "Settings.h"

#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace rdf::game::graph
{
	namespace
	{
		struct Names
		{
			RE::BSFixedString start, startB, chain, exit, entered, left, clip, speed, armBlend, isDodging;
			std::vector<RE::BSFixedString> windowOpen, windowClose;
		};
		Names g_names;

		// Clip annotation names (ANIMATION_CONTRACT.md), indexed by WindowEvent.
		constexpr std::array kWindowEventNames{ "RDF_Windows"sv, "RDF_IFrameStart"sv, "RDF_IFrameEnd"sv,
			"RDF_ChainOpen"sv, "RDF_AttackOpen"sv, "RDF_BlockOpen"sv, "RDF_ExitOpen"sv };
		std::array<RE::BSFixedString, kWindowEventNames.size()> g_windowNames;

		struct ActorGraph
		{
			std::uint32_t entered{ 0 };
			int           depth{ 0 };
			bool          window{ false };
			std::uint32_t clipWindows{ 0 };  // bit per WindowEvent seen since the last drain
			// Several graphs (the player's first / third person): only this one counts; null = every graph.
			const RE::BSTEventSource<RE::BSAnimationGraphEvent>* activeSource{ nullptr };
		};

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
		g_names.chain = g.chain;
		g_names.exit = g.exit;
		g_names.entered = g.entered;
		g_names.left = g.left;
		g_names.clip = g.clip;
		g_names.speed = g.speed;
		g_names.armBlend = g.armBlend;
		g_names.isDodging = g.isDodging;
		for (std::size_t i = 0; i < kWindowEventNames.size(); ++i) {
			g_windowNames[i] = kWindowEventNames[i];
		}
		for (const auto& n : g.windowOpen) {
			g_names.windowOpen.emplace_back(n);
		}
		for (const auto& n : g.windowClose) {
			g_names.windowClose.emplace_back(n);
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

	void ClearEvents(RE::Actor* actor)
	{
		WithActor(actor, [](ActorGraph& g) {
			const auto source = g.activeSource;
			g = ActorGraph{};
			g.activeSource = source;
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

	bool SendStart(RE::Actor* actor, DodgeEntry entry, int clip, float animSpeed, float armStance)
	{
		actor->SetGraphVariableInt(g_names.clip, clip);
		actor->SetGraphVariableFloat(g_names.speed, animSpeed);
		actor->SetGraphVariableFloat(g_names.armBlend, ArmBlendWeight(armStance));
		switch (entry) {
		case DodgeEntry::Chain: return actor->NotifyAnimationGraph(g_names.chain);
		case DodgeEntry::StartB: return actor->NotifyAnimationGraph(g_names.startB);
		default: return actor->NotifyAnimationGraph(g_names.start);
		}
	}

	void SendExit(RE::Actor* actor)
	{
		actor->NotifyAnimationGraph(g_names.exit);
	}

	bool GraphIsDodging(RE::Actor* actor)
	{
		bool value = false;
		actor->GetGraphVariableBool(g_names.isDodging, value);
		return value;
	}
}
