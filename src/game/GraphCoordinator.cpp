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
			RE::BSFixedString start, startB, chain, exit, entered, left, clip, speed, isDodging;
			std::vector<RE::BSFixedString> windowOpen, windowClose;
		};
		Names g_names;

		std::atomic<std::uint32_t> g_entered{ 0 };
		std::atomic<int>           g_depth{ 0 };
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
			RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* event, RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
			{
				if (!event) {
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
		player->RemoveAnimationGraphEventSink(&g_sink);  // no-op when absent: guards against duplicates
		const bool added = player->AddAnimationGraphEventSink(&g_sink);
		logger::info("animation graph sink {}", added ? "registered" : "NOT registered (graph not loaded yet)");
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

	bool SendStart(RE::PlayerCharacter* player, DodgeEntry entry, int clip, float animSpeed)
	{
		player->SetGraphVariableInt(g_names.clip, clip);
		player->SetGraphVariableFloat(g_names.speed, animSpeed);
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
