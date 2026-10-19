#include "Effects.h"

#include "Diagnostics.h"
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

		// Projectile::Func190 (0x1407521F0), shared by ArrowProjectile and MissileProjectile.
		constexpr std::size_t kProjectileCollisionVfunc = 190;
		using Collision_t = void (*)(RE::Projectile*, RE::hkpAllCdPointCollector*);

		bool TouchesPlayer(const RE::hkpAllCdPointCollector& collector)
		{
			for (const auto& hit : collector.hits) {
				for (const auto* root : { hit.rootCollidableA, hit.rootCollidableB }) {
					const auto* ref = root ? RE::TESHavokUtilities::FindCollidableRef(*root) : nullptr;
					if (ref && ref->IsPlayerRef()) {
						return true;
					}
				}
			}
			return false;
		}

		template <int Kind>
		struct CollisionHook
		{
			static void Thunk(RE::Projectile* projectile, RE::hkpAllCdPointCollector* collector)
			{
				if (collector && g_invulnerable.load(std::memory_order_acquire) && TouchesPlayer(*collector)) {
					DevLog("i-frames: projectile {:08X} passed through the player", projectile ? projectile->GetFormID() : 0u);
					return;  // this step's contacts are dropped; the projectile keeps flying
				}
				original(projectile, collector);
			}
			static inline Collision_t original = nullptr;
		};

		template <int Kind>
		void HookCollision(REL::VariantID vtableId, const char* name)
		{
			REL::Relocation<std::uintptr_t> vtbl{ vtableId };
			CollisionHook<Kind>::original = reinterpret_cast<Collision_t>(vtbl.write_vfunc(kProjectileCollisionVfunc, CollisionHook<Kind>::Thunk));
			logger::info("projectile i-frame hook installed ({})", name);
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

	void InstallProjectileIFrames()
	{
		HookCollision<0>(RE::VTABLE_ArrowProjectile[0], "ArrowProjectile");
		HookCollision<1>(RE::VTABLE_MissileProjectile[0], "MissileProjectile");
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
