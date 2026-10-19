#include "Effects.h"

#include "Settings.h"

#include <atomic>
#include <cstring>

namespace rdf::game::effects
{
	namespace
	{
		constexpr std::uint32_t  kMeleeHitFunctionId = 37673;
		constexpr std::ptrdiff_t kHitCallOffset = 0x3C0;

		std::atomic<bool> g_invulnerable{ false };

		using ProcessHit_t = void (*)(RE::Actor*, RE::HitData*);
		ProcessHit_t g_originalHit = nullptr;

		void HitThunk(RE::Actor* victim, RE::HitData* hit)
		{
			if (victim && victim->IsPlayerRef() && g_invulnerable.load(std::memory_order_acquire)) {
				return;
			}
			g_originalHit(victim, hit);
		}
	}

	void ChargeCost(RE::PlayerCharacter* player, float cost)
	{
		const auto resource = Settings::Get().costResource;
		if (resource && cost > 0.0f) {
			player->AsActorValueOwner()->DamageActorValue(*resource, cost);
		}
	}

	void RefundCost(RE::PlayerCharacter* player, float cost)
	{
		const auto resource = Settings::Get().costResource;
		if (resource && cost > 0.0f) {
			player->AsActorValueOwner()->RestoreActorValue(*resource, cost);
		}
	}

	bool InstallIFrames()
	{
		const REL::Relocation<std::uintptr_t> function{ REL::ID(kMeleeHitFunctionId) };
		const auto                           site = function.address() + kHitCallOffset;
		const auto*                          bytes = reinterpret_cast<const std::uint8_t*>(site);
		logger::info("i-frame hook site {:#x}: {:02X} {:02X} {:02X} {:02X} {:02X}", site, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);
		if (bytes[0] != 0xE8) {
			logger::error("i-frame hook: expected CALL (E8) at the site, not installing");
			return false;
		}
		REL::Relocation<std::uintptr_t> callSite{ site };
		g_originalHit = reinterpret_cast<ProcessHit_t>(callSite.write_call<5>(HitThunk));
		logger::info("i-frame hook installed (original {:#x})", reinterpret_cast<std::uintptr_t>(g_originalHit));
		return true;
	}

	void SetInvulnerable(bool invulnerable)
	{
		g_invulnerable.store(invulnerable, std::memory_order_release);
	}
}
