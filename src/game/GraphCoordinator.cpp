#include "GraphCoordinator.h"

#include "Settings.h"

#include <atomic>
#include <vector>

namespace rdf::game::graph
{
	namespace
	{
		struct Names
		{
			RE::BSFixedString start, chain, exit, entered, left, clipEnd, clip, speed, isDodging;
			std::vector<RE::BSFixedString> windowOpen, windowClose;
		};
		Names g_names;

		std::atomic<std::uint32_t> g_entered{ 0 };
		std::atomic<int>           g_depth{ 0 };
		std::atomic<std::uint32_t> g_clipEnd{ 0 };
		std::atomic<bool>          g_window{ false };

		bool Contains(const std::vector<RE::BSFixedString>& list, const RE::BSFixedString& tag)
		{
			for (const auto& n : list) {
				if (n == tag) {
					return true;
				}
			}
			return false;
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
				if (event->tag == g_names.entered) {
					g_entered.fetch_add(1, std::memory_order_acq_rel);
					g_depth.fetch_add(1, std::memory_order_acq_rel);
				} else if (event->tag == g_names.left) {
					int depth = g_depth.load(std::memory_order_acquire);
					while (depth > 0 && !g_depth.compare_exchange_weak(depth, depth - 1, std::memory_order_acq_rel)) {
					}
				} else if (event->tag == g_names.clipEnd) {
					g_clipEnd.fetch_add(1, std::memory_order_acq_rel);
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
		g_names.chain = g.chain;
		g_names.exit = g.exit;
		g_names.entered = g.entered;
		g_names.left = g.left;
		g_names.clipEnd = g.clipEnd;
		g_names.clip = g.clip;
		g_names.speed = g.speed;
		g_names.isDodging = g.isDodging;
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
	bool TakeClipEnd() { return Take(g_clipEnd); }
	bool DodgeWindowOpen() { return g_window.load(std::memory_order_acquire); }

	void ClearEvents()
	{
		g_entered = 0;
		g_depth = 0;
		g_clipEnd = 0;
		g_window = false;
	}

	bool SendStart(RE::PlayerCharacter* player, bool chain, int clip, float animSpeed)
	{
		player->SetGraphVariableInt(g_names.clip, clip);
		player->SetGraphVariableFloat(g_names.speed, animSpeed);
		return player->NotifyAnimationGraph(chain ? g_names.chain : g_names.start);
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
