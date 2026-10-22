#include "GraphGate.h"

#include "Diagnostics.h"
#include "Settings.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace rdf::game::gate
{
	namespace
	{
		struct Rule
		{
			RE::BSFixedString event;
			RE::BSFixedString cancel;  // empty: none
		};
		std::vector<Rule> g_rules;  // written by Init before Install, read only afterwards

		std::mutex                                                        g_lock;
		std::unordered_map<std::uint32_t, std::vector<RE::BSFixedString>> g_locked;  // locked actors -> held events
		std::atomic<int>                                                  g_lockedCount{ 0 };
		thread_local int                                                  t_bypass = 0;

		const Rule* RuleFor(const RE::BSFixedString& event)
		{
			for (const auto& r : g_rules) {
				if (r.event == event) {
					return &r;
				}
			}
			return nullptr;
		}

		const Rule* RuleCancelledBy(const RE::BSFixedString& event)
		{
			for (const auto& r : g_rules) {
				if (!r.cancel.empty() && r.cancel == event) {
					return &r;
				}
			}
			return nullptr;
		}

		// A sent event of a held name settles the held one (it happened anyway); a cancel event drops it.
		void Settle(std::uint32_t formID, const RE::BSFixedString& name)
		{
			std::scoped_lock lock(g_lock);
			const auto it = g_locked.find(formID);
			if (it != g_locked.end()) {
				std::erase(it->second, name);
			}
		}

		struct Notify
		{
			static bool thunk(RE::IAnimationGraphManagerHolder* holder, const RE::BSFixedString& event)
			{
				if (t_bypass == 0 && g_lockedCount.load(std::memory_order_relaxed) > 0 && holder) {
					auto*      actor = static_cast<RE::Actor*>(holder);
					const auto id = actor->GetFormID();
					if (const auto* rule = RuleFor(event)) {
						std::scoped_lock lock(g_lock);
						if (const auto it = g_locked.find(id); it != g_locked.end()) {
							if (std::find(it->second.begin(), it->second.end(), event) == it->second.end()) {
								it->second.push_back(event);
								DevLog("{:08X}: graph event '{}' held back by the dodge", id, event.c_str());
							}
							return false;
						}
					} else if (const auto* cancelled = RuleCancelledBy(event)) {
						std::scoped_lock lock(g_lock);
						if (const auto it = g_locked.find(id); it != g_locked.end()) {
							const auto before = it->second.size();
							std::erase(it->second, cancelled->event);
							if (it->second.size() != before) {
								DevLog("{:08X}: held '{}' dropped by '{}'", id, cancelled->event.c_str(), event.c_str());
							}
						}
					}
				}
				return func(holder, event);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	Bypass::Bypass() { ++t_bypass; }
	Bypass::~Bypass() { --t_bypass; }

	void Init()
	{
		g_rules.clear();
		for (const auto& spec : Settings::Get().graph.holdEvents) {   // "event" or "event|cancelEvent"
			const auto bar = spec.find('|');
			Rule r;
			r.event = spec.substr(0, bar);
			if (bar != std::string::npos) {
				r.cancel = spec.substr(bar + 1);
			}
			if (!r.event.empty()) {
				g_rules.push_back(r);
			}
		}
	}

	void Install()
	{
		if (g_rules.empty()) {
			logger::info("graph gate: no [Graph] sHoldEvents, not installed");
			return;
		}
		// Slot 1 of the IAnimationGraphManagerHolder vtable; one function serves both classes, so one original.
		REL::Relocation<std::uintptr_t> player{ RE::VTABLE_PlayerCharacter[3] };
		REL::Relocation<std::uintptr_t> character{ RE::VTABLE_Character[3] };
		Notify::func = player.write_vfunc(1, Notify::thunk);
		const auto other = character.write_vfunc(1, Notify::thunk);
		if (other != Notify::func.address()) {
			logger::warn("graph gate: Character and PlayerCharacter NotifyAnimationGraph differ ({:X} / {:X}); NPCs chain to the player's",
				other, Notify::func.address());
		}
		logger::info("graph gate installed ({} held event(s))", g_rules.size());
	}

	void SetLocked(std::uint32_t formID, bool locked)
	{
		std::scoped_lock lock(g_lock);
		if (locked) {
			g_locked.try_emplace(formID);
		} else {
			g_locked.erase(formID);   // whatever is still held was not let go: dropped
		}
		g_lockedCount = static_cast<int>(g_locked.size());
	}

	std::vector<RE::BSFixedString> TakeHeld(std::uint32_t formID)
	{
		std::scoped_lock lock(g_lock);
		const auto it = g_locked.find(formID);
		if (it == g_locked.end()) {
			return {};
		}
		return std::exchange(it->second, {});
	}

	void Forget(std::uint32_t formID)
	{
		SetLocked(formID, false);
	}

	void Clear()
	{
		std::scoped_lock lock(g_lock);
		g_locked.clear();
		g_lockedCount = 0;
	}

	bool Send(RE::Actor* actor, const RE::BSFixedString& event)
	{
		if (!actor) {
			return false;
		}
		Settle(actor->GetFormID(), event);
		Bypass pass;
		return actor->NotifyAnimationGraph(event);
	}
}
