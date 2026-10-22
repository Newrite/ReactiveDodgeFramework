#include "Effects.h"

#include "Diagnostics.h"
#include "PrecisionAPI.h"
#include "Settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

namespace rdf::game::effects
{
	namespace
	{
		constexpr std::uint32_t  kMeleeHitFunctionId = 37673;
		constexpr std::ptrdiff_t kHitCallOffset = 0x3C0;

		std::atomic<bool> g_invulnerable{ false };

		// The last hit the i-frames dodged (any thread), for the perfect-dodge check on the main thread.
		std::atomic<bool>          g_dodgedHit{ false };
		std::atomic<std::uint32_t> g_dodgedHitAttacker{ 0 };  // ref handle, 0 = unknown

		void NoteDodgedHit(std::uint32_t attackerHandle)
		{
			g_dodgedHitAttacker.store(attackerHandle, std::memory_order_relaxed);
			g_dodgedHit.store(true, std::memory_order_release);
		}

		using ProcessHit_t = void (*)(RE::Actor*, RE::HitData*);
		ProcessHit_t g_originalHit = nullptr;

		void HitThunk(RE::Actor* victim, RE::HitData* hit)
		{
			if (victim && victim->IsPlayerRef() && g_invulnerable.load(std::memory_order_acquire)) {
				NoteDodgedHit(hit ? hit->aggressor.native_handle() : 0);
				return;
			}
			g_originalHit(victim, hit);
		}

		// ---- projectiles (vtable slots verified in IDA 1.5.97, REVERSE_ENGINEERING.md "Projectiles")
		// 189 AddImpact(ref, loc, vel, collidable, shapeKey, flag) -> ImpactData*: records a hit on `ref`; every type
		//     but Barrier overrides it, and an impact's spell / damage is applied later from ProcessImpacts (172).
		// 190 HandleHits(hkpAllCdPointCollector*): per physics contact; the base version (Arrow, Missile, Grenade,
		//     Beam, Flame, Barrier) also applies an arrow's weapon damage itself and kills the projectile, so for single
		//     projectiles the whole step is dropped. Cone has its own and goes through AddImpact per target.
		constexpr std::size_t kAddImpactVfunc = 189;
		constexpr std::size_t kHandleHitsVfunc = 190;
		using AddImpact_t = RE::Projectile::ImpactData* (*)(RE::Projectile*, RE::TESObjectREFR*, const RE::NiPoint3*,
			const RE::NiPoint3*, RE::hkpCollidable*, std::int32_t, std::uint32_t);
		using HandleHits_t = void (*)(RE::Projectile*, RE::hkpAllCdPointCollector*);

		// A single projectile (arrow, bolt, fireball, grenade) that passed through the player during the i-frames keeps
		// ignoring the player for the rest of its life, so i-frames ending while it still overlaps the player's
		// collision do not land a late hit. Continuous ones (beams, flames, cones, barriers) are not remembered: they
		// hurt again once the i-frames are over, as a stream should.
		struct Dodged
		{
			const RE::Projectile*                 projectile;
			RE::FormID                            formID;  // the pointer alone could be a new projectile later
			std::chrono::steady_clock::time_point until;
		};
		constexpr auto      kRemember = std::chrono::seconds(30);  // longer than any projectile's flight
		std::mutex          g_dodgedLock;
		std::vector<Dodged> g_dodged;

		bool Remembered(const RE::Projectile* p)
		{
			const auto       now = std::chrono::steady_clock::now();
			std::scoped_lock lock(g_dodgedLock);
			std::erase_if(g_dodged, [&](const Dodged& d) { return d.until < now; });
			return std::ranges::any_of(g_dodged, [&](const Dodged& d) { return d.projectile == p && d.formID == p->GetFormID(); });
		}

		void Remember(const RE::Projectile* p)
		{
			{
				std::scoped_lock lock(g_dodgedLock);
				for (const auto& d : g_dodged) {
					if (d.projectile == p && d.formID == p->GetFormID()) {
						return;
					}
				}
				g_dodged.push_back({ p, p->GetFormID(), std::chrono::steady_clock::now() + kRemember });
			}
			DevLog("i-frames: projectile {:08X} passed through the player, ignored for the rest of its life", p->GetFormID());
		}

		std::atomic<const void*> g_lastStream{ nullptr };

		// True: the projectile must not hit `target` now. `remember`: once through, it never hits the player again.
		bool Ignore(RE::Projectile* p, const RE::TESObjectREFR* target, bool remember)
		{
			if (!p || !target || !target->IsPlayerRef()) {
				return false;
			}
			if (g_invulnerable.load(std::memory_order_acquire)) {
				if (remember) {
					Remember(p);
				} else if (g_lastStream.exchange(p) != p) {
					DevLog("i-frames: stream projectile {:08X} skips the player while invulnerable", p->GetFormID());
				}
				NoteDodgedHit(p->GetProjectileRuntimeData().shooter.native_handle());
				return true;
			}
			return remember && Remembered(p);
		}

		RE::TESObjectREFR* PlayerIn(const RE::hkpAllCdPointCollector& collector)
		{
			for (const auto& hit : collector.hits) {
				for (const auto* root : { hit.rootCollidableA, hit.rootCollidableB }) {
					auto* ref = root ? RE::TESHavokUtilities::FindCollidableRef(*root) : nullptr;
					if (ref && ref->IsPlayerRef()) {
						return ref;
					}
				}
			}
			return nullptr;
		}

		template <int Kind>
		struct ProjectileHooks
		{
			static RE::Projectile::ImpactData* AddImpact(RE::Projectile* p, RE::TESObjectREFR* ref, const RE::NiPoint3* loc,
				const RE::NiPoint3* vel, RE::hkpCollidable* collidable, std::int32_t shapeKey, std::uint32_t flag)
			{
				if (Ignore(p, ref, remember)) {
					return nullptr;
				}
				return addImpact(p, ref, loc, vel, collidable, shapeKey, flag);
			}

			static void HandleHits(RE::Projectile* p, RE::hkpAllCdPointCollector* collector)
			{
				if (collector) {
					if (auto* player = PlayerIn(*collector); player && Ignore(p, player, true)) {
						return;  // this step's contacts are dropped; the projectile keeps flying
					}
				}
				handleHits(p, collector);
			}

			static inline bool         remember = false;
			static inline AddImpact_t  addImpact = nullptr;
			static inline HandleHits_t handleHits = nullptr;
		};

		// `remember`: a projectile that passed through keeps ignoring the player (arrows, missiles, grenades, cones:
		// a shout or breath cone outlives any i-frames). `contacts`: also drop the contact step (single-target types
		// whose base HandleHits applies damage itself; never for cones, which hit several actors per step).
		template <int Kind>
		void HookProjectile(REL::VariantID vtableId, const char* name, bool remember, bool contacts)
		{
			REL::Relocation<std::uintptr_t> vtbl{ vtableId };
			ProjectileHooks<Kind>::remember = remember;
			ProjectileHooks<Kind>::addImpact = reinterpret_cast<AddImpact_t>(vtbl.write_vfunc(kAddImpactVfunc, ProjectileHooks<Kind>::AddImpact));
			if (contacts) {
				ProjectileHooks<Kind>::handleHits = reinterpret_cast<HandleHits_t>(vtbl.write_vfunc(kHandleHitsVfunc, ProjectileHooks<Kind>::HandleHits));
			}
			logger::info("projectile i-frames: {} (AddImpact{}{})", name, contacts ? " + HandleHits" : "", remember ? ", remembered" : ", while invulnerable");
		}

		// ---- explosions: Explosion::ApplyToTargets (42677) asks HasLineOfSight (42680) per actor before it applies
		// the explosion's HitData, casts its spell / enchantment on the actor and knocks it down. Answering "no line of
		// sight" for an invulnerable player skips all three, the same as a wall in between.
		constexpr std::uint32_t  kExplosionApplyId = 42677;
		constexpr std::ptrdiff_t kLineOfSightCall = 0x38C;  // 0x1407399BC in 1.5.97
		constexpr std::uint32_t  kLineOfSightId = 42680;
		using LineOfSight_t = bool (*)(RE::Explosion*, RE::NiPoint3*, RE::Actor*);
		LineOfSight_t g_lineOfSight = nullptr;

		bool LineOfSightThunk(RE::Explosion* explosion, RE::NiPoint3* from, RE::Actor* target)
		{
			if (target && target->IsPlayerRef() && g_invulnerable.load(std::memory_order_acquire)) {
				DevLog("i-frames: explosion {:08X} skips the player", explosion ? explosion->GetFormID() : 0u);
				NoteDodgedHit(explosion ? explosion->GetExplosionRuntimeData().actorOwner.native_handle() : 0);
				return false;
			}
			return g_lineOfSight(explosion, from, target);
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
		HookProjectile<0>(RE::VTABLE_ArrowProjectile[0], "ArrowProjectile", true, true);
		HookProjectile<1>(RE::VTABLE_MissileProjectile[0], "MissileProjectile", true, true);
		HookProjectile<2>(RE::VTABLE_GrenadeProjectile[0], "GrenadeProjectile", true, true);
		HookProjectile<3>(RE::VTABLE_ConeProjectile[0], "ConeProjectile", true, false);
		HookProjectile<4>(RE::VTABLE_BeamProjectile[0], "BeamProjectile", false, false);
		HookProjectile<5>(RE::VTABLE_FlameProjectile[0], "FlameProjectile", false, false);
		HookProjectile<6>(RE::VTABLE_BarrierProjectile[0], "BarrierProjectile", false, false);
	}

	bool InstallExplosionIFrames()
	{
		const REL::Relocation<std::uintptr_t> function{ REL::ID(kExplosionApplyId) };
		const auto                           site = function.address() + kLineOfSightCall;
		const auto*                          bytes = reinterpret_cast<const std::uint8_t*>(site);
		if (bytes[0] != 0xE8) {
			logger::error("explosion i-frames: expected CALL (E8) at {:#x}, not installing", site);
			return false;
		}
		REL::Relocation<std::uintptr_t> callSite{ site };
		g_lineOfSight = reinterpret_cast<LineOfSight_t>(callSite.write_call<5>(LineOfSightThunk));
		const REL::Relocation<std::uintptr_t> vanilla{ REL::ID(kLineOfSightId) };
		logger::info("explosion i-frames installed at {:#x} ({})", site,
			reinterpret_cast<std::uintptr_t>(g_lineOfSight) == vanilla.address() ? "game's line-of-sight check" : "chained to another hook");
		return true;
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

	// Precision runs its own hit pipeline (hitstop, impact effects, recoil) before the game's hit code,
	// so dropping the hit at 37673 alone still lets those play. Its pre-hit callback can ignore the hit
	// outright, which is what i-frames mean.
	void InstallPrecisionIFrames()
	{
		auto* api = static_cast<PRECISION_API::IVPrecision4*>(PRECISION_API::RequestPluginAPI(PRECISION_API::InterfaceVersion::V4));
		if (!api) {
			logger::info("Precision not found; melee i-frames use the game's hit hook only");
			return;
		}
		const auto result = api->AddPreHitCallback(SKSE::GetPluginHandle(), [](const PRECISION_API::PrecisionHitData& hit) {
			PRECISION_API::PreHitCallbackReturn ret;
			if (hit.target && hit.target->IsPlayerRef() && g_invulnerable.load(std::memory_order_acquire)) {
				ret.bIgnoreHit = true;
				NoteDodgedHit(hit.attacker ? hit.attacker->GetHandle().native_handle() : 0);
				DevLog("i-frames: Precision hit from {:08X} ignored", hit.attacker ? hit.attacker->GetFormID() : 0u);
			}
			return ret;
		});
		logger::info("Precision pre-hit callback: {}", result == PRECISION_API::APIResult::OK ? "registered" : "already registered");
	}

	std::optional<std::uint32_t> TakeDodgedHit()
	{
		if (!g_dodgedHit.exchange(false, std::memory_order_acq_rel)) {
			return std::nullopt;
		}
		return g_dodgedHitAttacker.load(std::memory_order_relaxed);
	}

	void SetInvulnerable(bool invulnerable)
	{
		g_invulnerable.store(invulnerable, std::memory_order_release);
	}
}
