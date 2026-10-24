#include "PerfectGhost.h"

#include "Diagnostics.h"
#include "Settings.h"

#include "third_party/ghosttrails/GhostTrailsAPI.h"

#include <algorithm>
#include <vector>

namespace rdf::game::ghost
{
	namespace
	{
		namespace gt = GhostTrailsAPI;

		gt::IVGhostTrails1* g_api = nullptr;

		// The requests this plugin owns. The provider keeps each one's status until Release, and Release also removes
		// silhouettes still on screen: a spawn is released once it is over (a terminal state) and its last silhouette had
		// its whole lifetime; a Prepare at once when it is over.
		struct Owned
		{
			gt::RequestID id{ 0 };
			bool          prepare{ false };
			float         linger{ 0.0f };  // game seconds left after the request ended (spawns)
			bool          ended{ false };
		};
		std::vector<Owned> g_owned;
		constexpr std::size_t kMaxOwned = 32;  // past this the oldest is released (its silhouettes go with it)

		bool Terminal(gt::State s)
		{
			return s == gt::State::Completed || s == gt::State::Partial || s == gt::State::Failed || s == gt::State::Canceled;
		}

		void Own(gt::RequestID id, bool prepare, float linger)
		{
			if (g_owned.size() >= kMaxOwned) {
				g_api->Release(g_owned.front().id);
				g_owned.erase(g_owned.begin());
			}
			g_owned.push_back({ id, prepare, linger, false });
		}

		// The actor gets silhouettes: the switch, a provider, and NPCs only when allowed.
		bool Wanted(RE::Actor* actor)
		{
			const auto& p = Settings::Get().perfect;
			return g_api && actor && p.enabled && p.ghost && (actor->IsPlayerRef() || p.ghostNPC);
		}

		const char* ResultName(gt::Result r)
		{
			switch (r) {
			case gt::Result::Accepted: return "accepted";
			case gt::Result::InvalidArgument: return "invalid argument";
			case gt::Result::NotReady: return "not ready";
			case gt::Result::Capacity: return "capacity";
			case gt::Result::NotFound: return "not found";
			default: return "finished";
			}
		}
	}

	void Init()
	{
		g_api = gt::RequestPluginAPI();
		logger::info("perfect-dodge ghost: GhostTrails API v1 {}", g_api ? "found" : "not found (no silhouettes)");
	}

	void OnDodgeStart(RE::Actor* actor)
	{
		if (!Wanted(actor)) {
			return;
		}
		const auto     slots = static_cast<std::uint32_t>(std::max(Settings::Get().perfect.ghostCount, 1));
		gt::RequestID  id{};
		const auto     r = g_api->Prepare(actor->GetHandle().native_handle(), slots, &id);
		if (r == gt::Result::Accepted) {
			Own(id, true, 0.0f);
		} else {
			DevLog("perfect-dodge ghost: warming {:08X} refused: {}", actor->GetFormID(), ResultName(r));
		}
	}

	void OnPerfectDodge(RE::Actor* actor)
	{
		if (!Wanted(actor)) {
			return;
		}
		const auto&       p = Settings::Get().perfect;
		gt::SpawnOptions  o;
		o.appearance = { p.ghostColor[0], p.ghostColor[1], p.ghostColor[2], p.ghostColor[3], p.ghostIntensity, p.ghostRimPower };
		o.count = static_cast<std::uint32_t>(std::max(p.ghostCount, 1));
		o.interval = p.ghostInterval;
		o.lifetime = p.ghostLifetime;
		o.retryWindow = 0.15f;  // a silhouette late by more than this would not stand where the dodge was
		o.spawnWindow = 0.0f;
		gt::RequestID id{};
		const auto    r = g_api->Spawn(actor->GetHandle().native_handle(), &o, &id);
		if (r == gt::Result::Accepted) {
			Own(id, false, p.ghostLifetime + 0.1f);
			DevLog("perfect-dodge ghost: {} silhouette(s) of {:08X} asked (request {})", o.count, actor->GetFormID(), id);
		} else {
			DevLog("perfect-dodge ghost: {:08X} refused: {}", actor->GetFormID(), ResultName(r));
		}
	}

	void Update(float dt)
	{
		if (!g_api || g_owned.empty()) {
			return;
		}
		std::erase_if(g_owned, [&](Owned& o) {
			if (!o.ended) {
				gt::Status status;
				if (g_api->GetStatus(o.id, &status) != gt::Result::Accepted) {
					return true;  // unknown to the provider now: nothing left to release
				}
				o.ended = Terminal(status.state);
				if (!o.ended) {
					return false;
				}
			}
			o.linger -= dt;
			if (o.prepare || o.linger <= 0.0f) {
				g_api->Release(o.id);
				return true;
			}
			return false;
		});
	}

	void Reset()
	{
		g_owned.clear();  // a session reset made every ID unknown to the provider
	}
}
