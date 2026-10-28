#include "Effects.h"

#include "AttackCancel.h"
#include "Diagnostics.h"
#include "PrecisionAPI.h"
#include "Settings.h"
#include "IFrameHookCode.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rdf::game::effects
{
	namespace
	{
		constexpr std::uint32_t  kMeleeHitFunctionId = 37673;
		constexpr std::ptrdiff_t kHitCallOffset = 0x3C0;

		// Everything here is per actor and keyed by Bethesda handles (native ref handles): a handle that once resolves
		// to null stays null, so dead projectiles / actors are dropped by resolving, and NPC dodgers need nothing new.

		std::uint32_t HandleOf(const RE::TESObjectREFR* ref)
		{
			return ref ? const_cast<RE::TESObjectREFR*>(ref)->GetHandle().native_handle() : 0;
		}

		// ---- actors with active i-frames (main thread writes, hooks on any thread read)
		std::shared_mutex          g_invulnerableLock;
		std::vector<std::uint32_t> g_invulnerable;
		std::atomic<int>           g_invulnerableCount{ 0 };  // lock-free "nobody is invulnerable" fast path

		bool IsInvulnerable(const RE::TESObjectREFR* ref)
		{
			if (!ref || g_invulnerableCount.load(std::memory_order_acquire) == 0) {
				return false;
			}
			const auto       handle = HandleOf(ref);
			std::shared_lock lock(g_invulnerableLock);
			return std::ranges::find(g_invulnerable, handle) != g_invulnerable.end();
		}

		// ---- actors whose perfect window is open (main thread writes, hooks read), as the i-frame list
		std::shared_mutex          g_perfectLock;
		std::vector<std::uint32_t> g_perfect;
		std::atomic<int>           g_perfectCount{ 0 };

		bool IsPerfectOpen(const RE::TESObjectREFR* ref)
		{
			if (!ref || g_perfectCount.load(std::memory_order_acquire) == 0) {
				return false;
			}
			const auto       handle = HandleOf(ref);
			std::shared_lock lock(g_perfectLock);
			return std::ranges::find(g_perfect, handle) != g_perfect.end();
		}

		// The i-frames dodge this hit: the target is invulnerable, and the attacker's rule allows it (a perfect-only
		// attacker only inside the target's perfect window, an undodgeable one never). The attacker's keywords are read
		// here, as the hit meets the i-frames.
		bool Dodges(const RE::TESObjectREFR* target, std::uint32_t attacker)
		{
			if (!IsInvulnerable(target)) {
				return false;
			}
			const auto  ref = attacker ? RE::TESObjectREFR::LookupByHandle(attacker) : RE::NiPointer<RE::TESObjectREFR>{};
			const auto  rule = attack::RuleOf(ref ? ref->As<RE::Actor>() : nullptr);
			switch (rule) {
			case attack::AttackerRule::Undodgeable:
				DevLog("i-frames: {:08X} is undodgeable, its hit on {:08X} lands", ref ? ref->GetFormID() : 0u, target->GetFormID());
				return false;
			case attack::AttackerRule::PerfectOnly:
				if (!IsPerfectOpen(target)) {
					DevLog("i-frames: {:08X} needs a perfect dodge, its hit on {:08X} lands", ref ? ref->GetFormID() : 0u, target->GetFormID());
					return false;
				}
				return true;
			default:
				return true;
			}
		}

		// ---- hits the i-frames dodged, per dodger (any thread), for the perfect-dodge check on the main thread
		std::mutex                                       g_dodgedHitsLock;
		std::unordered_map<std::uint32_t, std::uint32_t> g_dodgedHits;  // dodger handle -> attacker handle (0 = unknown)

		void NoteDodgedHit(const RE::TESObjectREFR* dodger, std::uint32_t attackerHandle)
		{
			std::scoped_lock lock(g_dodgedHitsLock);
			g_dodgedHits.insert_or_assign(HandleOf(dodger), attackerHandle);
		}

		using ProcessHit_t = void (*)(RE::Actor*, RE::HitData*);
		ProcessHit_t g_originalHit = nullptr;

		void HitThunk(RE::Actor* victim, RE::HitData* hit)
		{
			if (const auto attacker = hit ? hit->aggressor.native_handle() : 0u; Dodges(victim, attacker)) {
				NoteDodgedHit(victim, attacker);
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

		// A projectile that passed through a dodger during its i-frames keeps ignoring that dodger for the rest of its
		// life (arrows, bolts, missiles, grenades, cones), so i-frames ending while it still overlaps the dodger's
		// collision do not land a late hit. Streams (beams, flames, barriers) are not remembered: they hurt again once
		// the i-frames are over. Key: projectile handle << 32 | dodger handle; an entry goes once its projectile's
		// handle resolves to null (the projectile is gone, and a dead handle never comes back).
		std::mutex                        g_dodgedLock;
		std::unordered_set<std::uint64_t> g_dodged;
		std::atomic<bool>                 g_anyDodged{ false };

		std::uint64_t DodgedKey(std::uint32_t projectile, std::uint32_t dodger)
		{
			return (static_cast<std::uint64_t>(projectile) << 32) | dodger;
		}

		bool Remembered(std::uint32_t projectile, std::uint32_t dodger)
		{
			if (!g_anyDodged.load(std::memory_order_acquire)) {
				return false;
			}
			std::scoped_lock lock(g_dodgedLock);
			return g_dodged.contains(DodgedKey(projectile, dodger));
		}

		void Remember(const RE::Projectile* p, std::uint32_t projectile, std::uint32_t dodger)
		{
			{
				std::scoped_lock lock(g_dodgedLock);
				if (!g_dodged.insert(DodgedKey(projectile, dodger)).second) {
					return;
				}
				std::erase_if(g_dodged, [](std::uint64_t key) {
					return !RE::TESObjectREFR::LookupByHandle(static_cast<RE::RefHandle>(key >> 32));
				});
				g_anyDodged.store(!g_dodged.empty(), std::memory_order_release);
			}
			DevLog("i-frames: projectile {:08X} passed through {:08X}, ignored by it for the rest of its life", p->GetFormID(), dodger);
		}

		std::atomic<const void*> g_lastStream{ nullptr };

		bool RemembersDodgers(const RE::Projectile* projectile)
		{
			return !projectile->Is(RE::FormType::ProjectileBeam, RE::FormType::ProjectileFlame, RE::FormType::ProjectileBarrier);
		}

		// True: projectile `p` must not hit `target` now. `remember`: once through, it never hits that target again.
		bool Ignore(RE::Projectile* p, const RE::TESObjectREFR* target, bool remember)
		{
			if (!p || !target) {
				return false;
			}
			if (Dodges(target, p->GetProjectileRuntimeData().shooter.native_handle())) {
				const auto dodger = HandleOf(target);
				if (remember) {
					Remember(p, HandleOf(p), dodger);
				} else if (g_lastStream.exchange(p) != p) {
					DevLog("i-frames: stream projectile {:08X} skips {:08X} while invulnerable", p->GetFormID(), target->GetFormID());
				}
				NoteDodgedHit(target, p->GetProjectileRuntimeData().shooter.native_handle());
				return true;
			}
			return remember && g_anyDodged.load(std::memory_order_acquire) && Remembered(HandleOf(p), HandleOf(target));
		}

		// HandleHits: does this contact point touch an actor the projectile must not hit?
		bool PointIgnored(RE::Projectile* p, const RE::hkpRootCdPoint& point, bool remember)
		{
			for (const auto* root : { point.rootCollidableA, point.rootCollidableB }) {
				auto* ref = root ? RE::TESHavokUtilities::FindCollidableRef(*root) : nullptr;
				if (ref && ref->Is(RE::FormType::ActorCharacter) && Ignore(p, ref, remember)) {
					return true;
				}
			}
			return false;
		}

		using FindObject_t = RE::NiAVObject* (*)(const RE::hkpCollidable*);
		FindObject_t g_findRayObject = nullptr;

		RE::NiAVObject* RayCandidateThunk(const RE::hkpCollidable* collidable, RE::Projectile* projectile)
		{
			auto* object = g_findRayObject(collidable);
			if (!object || !projectile ||
				(g_invulnerableCount.load(std::memory_order_acquire) == 0 && !g_anyDodged.load(std::memory_order_acquire))) {
				return object;
			}
			// Use the SAME reference resolver as the ray helper (including player 3D
			// and owner resolution), rather than guessing from a scene-node field.
			using Resolve_t = RE::TESObjectREFR* (*)(RE::NiAVObject*);
			static REL::Relocation<Resolve_t> resolve{ REL::ID(19323) };
			auto* target = resolve(object);
			const bool remember = RemembersDodgers(projectile);
			if (target && target->Is(RE::FormType::ActorCharacter) && Ignore(projectile, target, remember)) {
				DevLog("i-frames: ray candidate projectile {:08X} skips {:08X} before impact processing", projectile->GetFormID(), target->GetFormID());
				return nullptr;  // game's null-object path advances to the next ray hit
			}
			return object;
		}

		void InstallRayCandidateFilter()
		{
			const REL::Relocation<std::uintptr_t> function{ REL::ID(42983) };
			const auto site = function.address() + 0x1BC;
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(site);
			const REL::Relocation<std::uintptr_t> expected{ REL::ID(15644) };
			std::int32_t displacement = 0;
			std::memcpy(&displacement, bytes + 1, sizeof(displacement));
			if (bytes[0] != 0xE8 || site + 5 + displacement != expected.address()) {
				logger::error("projectile early i-frames: unexpected object resolver at {:#x}; not installing", site);
				return;
			}
			code::RayCandidateFilter stub{ reinterpret_cast<std::uintptr_t>(RayCandidateThunk) };
			auto& trampoline = SKSE::GetTrampoline();
			const auto entry = reinterpret_cast<std::uintptr_t>(trampoline.allocate(stub));
			g_findRayObject = reinterpret_cast<FindObject_t>(trampoline.write_call<5>(site, entry));
			logger::info("projectile early i-frames installed at {:#x} (ray candidates before magic/impact feedback)", site);
		}

		// Prefix of MagicHit's per-effect area delivery context, verified against
		// 33686 in IDA. This is not MagicTarget::AddTargetData.
		struct AreaEffectContext
		{
			RE::MagicCaster* caster;          // 00
			RE::MagicTarget* directTarget;    // 08
			RE::Effect* effect;               // 10
			void* unknown18;                 // 18
			RE::MagicItem* magicItem;         // 20
			RE::TESBoundObject* source;       // 28
			RE::TESObjectREFR* attacker;      // 30
		};
		static_assert(offsetof(AreaEffectContext, effect) == 0x10);
		static_assert(offsetof(AreaEffectContext, attacker) == 0x30);
		static_assert(sizeof(AreaEffectContext) == 0x38);

		bool IgnoreAreaTarget(RE::TESObjectREFR* target, const AreaEffectContext* context)
		{
			if (g_invulnerableCount.load(std::memory_order_acquire) == 0 ||
				!context || !target || target == context->attacker || !target->Is(RE::FormType::ActorCharacter)) {
				return false;
			}
			const auto* base = context->effect ? context->effect->baseEffect : nullptr;
			if (!base || (!base->IsHostile() && !base->IsDetrimental())) {
				return false;  // beneficial area spells retain their normal delivery
			}
			const auto attacker = HandleOf(context->attacker);
			if (!Dodges(target, attacker)) {
				return false;
			}
			NoteDodgedHit(target, attacker);
			DevLog("i-frames: magic area spell {:08X} effect {:08X} from {:08X} skips {:08X}",
				context->magicItem ? context->magicItem->GetFormID() : 0u, base->GetFormID(),
				context->attacker ? context->attacker->GetFormID() : 0u, target->GetFormID());
			return true;
		}

		bool InstallAreaTargetFilter()
		{
			const REL::Relocation<std::uintptr_t> function{ REL::ID(33686) };
			const auto site = function.address() + 0x10B;
			constexpr std::uint8_t expected[]{ 0x49, 0x8B, 0x4C, 0x24, 0x10 };
			if (std::memcmp(reinterpret_cast<const void*>(site), expected, sizeof(expected)) != 0) {
				logger::error("magic area i-frames: unexpected delivery entry at {:#x}; not installing", site);
				return false;
			}
			// After ghost/distance/direct-target checks; skip to the common per-target
			// reference release, BEFORE AddTarget and its independent follow-up callback.
			code::AreaTargetFilter stub{ reinterpret_cast<std::uintptr_t>(IgnoreAreaTarget), site + 5, function.address() + 0x2C9 };
			auto& trampoline = SKSE::GetTrampoline();
			const auto entry = reinterpret_cast<std::uintptr_t>(trampoline.allocate(stub));
			trampoline.write_branch<5>(site, entry);
			logger::info("magic area i-frames installed at {:#x} (per hostile/detrimental effect, after range checks)", site);
			return true;
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

			// Only the contacts with actors the projectile must not hit are removed: one arrow can pass through several
			// dodgers and still hit whoever stands behind them in the same step. The collector's points are compacted
			// in place (hits._data / _size, 0x40 per hkpRootCdPoint as HandleHits itself indexes them) for the call and
			// restored afterwards.
			static void HandleHits(RE::Projectile* p, RE::hkpAllCdPointCollector* collector)
			{
				if (!collector || collector->hits._size <= 0 ||
					(g_invulnerableCount.load(std::memory_order_acquire) == 0 && !g_anyDodged.load(std::memory_order_acquire))) {
					handleHits(p, collector);
					return;
				}
				auto&                             hits = collector->hits;
				const std::int32_t                count = hits._size;
				std::vector<RE::hkpRootCdPoint>   saved(hits._data, hits._data + count);
				std::int32_t                      kept = 0;
				for (std::int32_t i = 0; i < count; ++i) {
					if (!PointIgnored(p, saved[i], remember)) {
						hits._data[kept++] = saved[i];
					}
				}
				if (kept == count) {
					handleHits(p, collector);
					return;
				}
				if (kept > 0) {
					hits._size = kept;
					handleHits(p, collector);  // the projectile keeps flying through the dodgers
				}
				std::copy(saved.begin(), saved.end(), hits._data);
				hits._size = count;
			}

			static inline bool         remember = false;
			static inline AddImpact_t  addImpact = nullptr;
			static inline HandleHits_t handleHits = nullptr;
		};

		// `remember`: a projectile that passed through keeps ignoring that dodger (arrows, missiles, grenades, cones:
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
		// sight" for an invulnerable actor skips all three, the same as a wall in between.
		constexpr std::uint32_t  kExplosionApplyId = 42677;
		constexpr std::ptrdiff_t kLineOfSightCall = 0x38C;  // 0x1407399BC in 1.5.97
		constexpr std::uint32_t  kLineOfSightId = 42680;
		using LineOfSight_t = bool (*)(RE::Explosion*, RE::NiPoint3*, RE::Actor*);
		LineOfSight_t g_lineOfSight = nullptr;

		bool LineOfSightThunk(RE::Explosion* explosion, RE::NiPoint3* from, RE::Actor* target)
		{
			const auto owner = explosion ? explosion->GetExplosionRuntimeData().actorOwner.native_handle() : 0u;
			if (Dodges(target, owner)) {
				DevLog("i-frames: explosion {:08X} skips {:08X}", explosion ? explosion->GetFormID() : 0u, target->GetFormID());
				NoteDodgedHit(target, owner);
				return false;
			}
			return g_lineOfSight(explosion, from, target);
		}
	}

	void ChargeCost(RE::Actor* actor, RE::ActorValue resource, float amount)
	{
		if (amount > 0.0f) {
			actor->AsActorValueOwner()->DamageActorValue(resource, amount);
		}
	}

	void FlashMeter(RE::ActorValue value)
	{
		using func_t = void (*)(RE::ActorValue);
		static REL::Relocation<func_t> func{ REL::ID(51907) };
		func(value);
	}

	void DelayRegen(RE::Actor* actor, RE::ActorValue value, float seconds)
	{
		auto* process = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
		if (!process || !process->high || !(seconds > 0.0f)) {
			return;
		}
		using func_t = void (*)(RE::AIProcess*, RE::ActorValue, float);
		static REL::Relocation<func_t> func{ REL::ID(38526) };
		func(process, value, seconds);
	}

	void RefundCost(RE::Actor* actor, RE::ActorValue resource, float amount)
	{
		if (amount > 0.0f) {
			actor->AsActorValueOwner()->RestoreActorValue(resource, amount);
		}
	}

	void InstallProjectileIFrames()
	{
		InstallRayCandidateFilter();
		HookProjectile<0>(RE::VTABLE_ArrowProjectile[0], "ArrowProjectile", true, true);
		HookProjectile<1>(RE::VTABLE_MissileProjectile[0], "MissileProjectile", true, true);
		HookProjectile<2>(RE::VTABLE_GrenadeProjectile[0], "GrenadeProjectile", true, true);
		HookProjectile<3>(RE::VTABLE_ConeProjectile[0], "ConeProjectile", true, false);
		HookProjectile<4>(RE::VTABLE_BeamProjectile[0], "BeamProjectile", false, true);
		HookProjectile<5>(RE::VTABLE_FlameProjectile[0], "FlameProjectile", false, true);
		HookProjectile<6>(RE::VTABLE_BarrierProjectile[0], "BarrierProjectile", false, true);
	}

	bool InstallExplosionIFrames()
	{
		const bool areaInstalled = InstallAreaTargetFilter();
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
		return areaInstalled;
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
			if (Dodges(hit.target, HandleOf(hit.attacker))) {
				ret.bIgnoreHit = true;
				NoteDodgedHit(hit.target, HandleOf(hit.attacker));
				DevLog("i-frames: Precision hit from {:08X} ignored", hit.attacker ? hit.attacker->GetFormID() : 0u);
			}
			return ret;
		});
		logger::info("Precision pre-hit callback: {}", result == PRECISION_API::APIResult::OK ? "registered" : "already registered");
	}

	std::optional<std::uint32_t> TakeDodgedHit(RE::Actor* dodger)
	{
		std::scoped_lock lock(g_dodgedHitsLock);
		const auto       it = g_dodgedHits.find(HandleOf(dodger));
		if (it == g_dodgedHits.end()) {
			return std::nullopt;
		}
		const auto attacker = it->second;
		g_dodgedHits.erase(it);
		return attacker;
	}

	void SetInvulnerable(RE::Actor* actor, bool invulnerable)
	{
		const auto       handle = HandleOf(actor);
		std::unique_lock lock(g_invulnerableLock);
		const auto       it = std::ranges::find(g_invulnerable, handle);
		if (invulnerable && it == g_invulnerable.end() && handle) {
			g_invulnerable.push_back(handle);
		} else if (!invulnerable && it != g_invulnerable.end()) {
			g_invulnerable.erase(it);
		}
		g_invulnerableCount.store(static_cast<int>(g_invulnerable.size()), std::memory_order_release);
	}

	void SetPerfectOpen(RE::Actor* actor, bool open)
	{
		const auto       handle = HandleOf(actor);
		std::unique_lock lock(g_perfectLock);
		const auto       it = std::ranges::find(g_perfect, handle);
		if (open && it == g_perfect.end() && handle) {
			g_perfect.push_back(handle);
		} else if (!open && it != g_perfect.end()) {
			g_perfect.erase(it);
		}
		g_perfectCount.store(static_cast<int>(g_perfect.size()), std::memory_order_release);
	}

	void ClearInvulnerable()
	{
		{
			std::unique_lock lock(g_invulnerableLock);
			g_invulnerable.clear();
			g_invulnerableCount.store(0, std::memory_order_release);
		}
		std::scoped_lock lock(g_dodgedHitsLock);
		g_dodgedHits.clear();
	}

}
