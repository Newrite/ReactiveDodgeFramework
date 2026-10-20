#include "GraphCoordinator.h"

#include "Settings.h"

#include <array>
#include <atomic>
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

		std::atomic<std::uint32_t> g_entered{ 0 };
		std::atomic<int>           g_depth{ 0 };
		std::atomic<const RE::BSTEventSource<RE::BSAnimationGraphEvent>*> g_activeSource{ nullptr };
		std::atomic<bool>          g_logEvents{ false };  // dev: log every graph event (see LogEvents)
		std::atomic<bool>          g_window{ false };
		std::atomic<std::uint32_t> g_clipWindows{ 0 };  // bit per WindowEvent seen since the last drain

		// Clip annotation names (ANIMATION_CONTRACT.md), indexed by WindowEvent.
		constexpr std::array kWindowEventNames{ "RDF_Windows"sv, "RDF_IFrameStart"sv, "RDF_IFrameEnd"sv,
			"RDF_ChainOpen"sv, "RDF_AttackOpen"sv, "RDF_BlockOpen"sv, "RDF_ExitOpen"sv };
		std::array<RE::BSFixedString, kWindowEventNames.size()> g_windowNames;

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

		bool Take(std::atomic<std::uint32_t>& counter)
		{
			return counter.exchange(0, std::memory_order_acq_rel) != 0;
		}

		class Sink final : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* event, RE::BSTEventSource<RE::BSAnimationGraphEvent>* source) override
			{
				// Both the third- and the first-person graph run the dodge states; only the one the player
				// currently uses counts, so a dodge is entered and left once.
				const auto* active = g_activeSource.load(std::memory_order_acquire);
				if (!event || (active && source != active)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				if (g_logEvents.load(std::memory_order_relaxed)) {
					logger::info("graph event {} '{}' (depth {})", event->tag.c_str(), event->payload.c_str(),
						g_depth.load(std::memory_order_relaxed));
				}
				if (event->tag == g_names.entered) {
					g_entered.fetch_add(1, std::memory_order_acq_rel);
					g_depth.fetch_add(1, std::memory_order_acq_rel);
				} else if (event->tag == g_names.left) {
					int depth = g_depth.load(std::memory_order_acquire);
					while (depth > 0 && !g_depth.compare_exchange_weak(depth, depth - 1, std::memory_order_acq_rel)) {
					}
				} else if (const int w = WindowIndex(event->tag); w >= 0) {
					g_clipWindows.fetch_or(1u << w, std::memory_order_acq_rel);
				} else if (Contains(g_names.windowOpen, event->tag)) {
					g_window.store(true, std::memory_order_release);
				} else if (Contains(g_names.windowClose, event->tag)) {
					g_window.store(false, std::memory_order_release);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		Sink g_sink;
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

	void RegisterSink(RE::PlayerCharacter* player)
	{
		if (!player) {
			return;
		}
		// Every graph of the player: the third-person one and the first-person one each send their own
		// dodge events (CommonLib's AddAnimationGraphEventSink only covers the first graph).
		RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
		if (!player->GetAnimationGraphManager(manager) || !manager || manager->graphs.empty()) {
			logger::info("{}", "animation graph sink NOT registered (graph not loaded yet)");
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
		logger::info("animation graph sink registered on {} graph(s)", count);
	}

	bool TrackActiveGraph(RE::PlayerCharacter* player)
	{
		RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
		const RE::BSTEventSource<RE::BSAnimationGraphEvent>* source = nullptr;
		if (player->GetAnimationGraphManager(manager) && manager && manager->activeGraph < manager->graphs.size()) {
			if (const auto& graph = manager->graphs[manager->activeGraph]) {
				source = graph->GetEventSource<RE::BSAnimationGraphEvent>();
			}
		}
		if (g_activeSource.exchange(source, std::memory_order_acq_rel) != source) {
			g_depth = 0;  // a switch between first and third person starts the count afresh
			return true;
		}
		return false;
	}

	bool TakeEntered() { return Take(g_entered); }
	int  Depth() { return g_depth.load(std::memory_order_acquire); }
	void LogEvents(bool on) { g_logEvents.store(on, std::memory_order_relaxed); }
	bool DodgeWindowOpen() { return g_window.load(std::memory_order_acquire); }

	void DrainClipWindows(const std::function<void(WindowEvent)>& sink)
	{
		const auto bits = g_clipWindows.exchange(0, std::memory_order_acq_rel);
		for (std::uint32_t i = 0; i < kWindowEventNames.size(); ++i) {  // enum order: an end after its start
			if (bits & (1u << i)) {
				sink(static_cast<WindowEvent>(i));
			}
		}
	}

	void ClearEvents()
	{
		g_entered = 0;
		g_depth = 0;
		g_window = false;
		g_clipWindows = 0;
	}

	float ArmBlendWeight(float stanceShare)
	{
		// RDF_ArmBlend is the blend parameter of a parametric blend between the dodge (0) and the
		// stance (1), so it is the share itself (BEHAVIOR_INTEGRATION.md section 5).
		return std::clamp(stanceShare, 0.0f, 1.0f);
	}

	bool SendStart(RE::PlayerCharacter* player, DodgeEntry entry, int clip, float animSpeed, float armStance)
	{
		player->SetGraphVariableInt(g_names.clip, clip);
		player->SetGraphVariableFloat(g_names.speed, animSpeed);
		player->SetGraphVariableFloat(g_names.armBlend, ArmBlendWeight(armStance));
		switch (entry) {
		case DodgeEntry::Chain: return player->NotifyAnimationGraph(g_names.chain);
		case DodgeEntry::StartB: return player->NotifyAnimationGraph(g_names.startB);
		default: return player->NotifyAnimationGraph(g_names.start);
		}
	}

	void SendExit(RE::PlayerCharacter* player)
	{
		player->NotifyAnimationGraph(g_names.exit);
	}

	bool GraphIsDodging(const RE::PlayerCharacter* player)
	{
		bool value = false;
		player->GetGraphVariableBool(g_names.isDodging, value);
		return value;
	}
}
