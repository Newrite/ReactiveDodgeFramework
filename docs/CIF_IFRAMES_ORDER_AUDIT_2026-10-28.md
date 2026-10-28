# CIF impact feedback and RDF hook ordering

## Evidence and limits

- RDF HEAD: 3c987a9. The area, ray and seven delivery-call fixes are currently
  uncommitted worktree changes, not recent commits. They remain intact.
- Public CIF source inspected at 562ac4d231fb28db718a43e9301a2cd1f572624f
  (xmake version 2.0.9), cloned into `_work/CIF-source`.
- Local houseCARL profile Vanilla resolves CIF 2.0.9; local CIF log confirms
  that version. Local Mu/Object Impact Framework inventory is NOT evidence
  that either is in the reporter's different installation.
- Downloaded `F:/Downloads/skse64.log` dated October 27 instead reports CIF
  version 01020050 (1.2.5.0), AccurateProjectileCollision and NewProjectilesTMP.
  That historical log does not establish the reporter's current DLL versions.
- `ReactiveDodgeFramework (2).log` predates the latest seven-call-site fixes:
  it has the ray/area hooks but not the new route installation lines, and
  Beam/Flame/Barrier still say AddImpact only. It cannot validate the final
  current worktree build.
- No live Skyrim process was available for runtime chain inspection. The
  reporter's CIF 1.2.5 binary was not available at its logged F:/SDS path.

## Concrete conflict in CIF 2.0.9 source

RDF installs its projectile vtable hooks during SKSEPlugin_Load (src/main.cpp).
CIF installs its hooks during kDataLoaded: Plugin.cpp -> PreLoadData ->
MainEvent::InstallHitHook (Initialization.hpp:60). Thus CIF's Arrow slot-190
wrapper becomes the outer wrapper and stores RDF as its previous function.

The local logs corroborate the timing: RDF projectile hooks at 03:18:28;
CIF ArrowCollision/MissileCollision hooks at 03:19:35.

Main.hpp:129, OnArrowCollisionTemplate:

1. Reads actor contact from the unfiltered collector.
2. SetDeferredHit(projectile, victim, false).
3. ProcessHitNormalized(victim, nullopt).
4. Only THEN calls _ArrowCollission, which can now be RDF's HandleHits.

DeferredHit.hpp:195 calls ImpactEffect::PlayImpactEffect without first requiring
runtimeReady. ImpactEffect.hpp:170-195 spawns particles for magic, extra effects
when configured, and plays impact sounds directly via BGSImpactManager. RDF's
engine call-site hooks do not intercept these direct plugin calls.

DeferredHit.hpp:121-129 distinguishes ordinary arrow vs magic by ammoSource:
an ArrowProjectile with no ammoSource is treated as magic and gets magic VFX.
This is a concrete mechanism for a magic-arrow variant producing a flash and
sound while RDF subsequently removes the contact and lets it fly onward.
Ordinary arrows can also receive CIF sounds depending on their impact mapping.

Main.hpp:103 OnMissileCollisionTemplate does NOT have that general pre-feedback
behavior. It normally just chains; its special branch concerns blood-spray
projectiles and a configurable workaround. Therefore the Arrow result must not
be generalized to every MissileProjectile or called a proven reproduction of
the reporter's exact attack.

CIF also listens for TESHitEvent and processes newly added ActiveEffects.
Those are additional entry points, not proof they fired in this reproduction.
Its ProcessHitTemplate similarly runs CIF feedback before chaining the melee
hit handler, so load-phase ordering affects more than projectile audio.

## Why previous changes did not cover this

The ray/contact/delivery filters address vanilla engine routes. In this chain,
the sequence is CIF feedback -> RDF filter -> vanilla processing. Filtering the
vanilla work succeeds but cannot undo earlier CIF side effects.

## Correction direction

Ensure RDF's contact filter is outside CIF's collision wrapper after CIF has
installed, so CIF sees only allowed contacts. Prefer one deliberate late
installation with captured original handlers and module-owner diagnostics.
Do NOT blindly reinstall the same thunk and overwrite its stored original:
if CIF already stored that thunk, this creates RDF -> CIF -> RDF recursion.
A late outer wrapper would need separate original storage, or initial vtable
installation must be moved to a verified later lifecycle point.

For the inspected source, the public pre-hit callbacks return void and do not
provide a documented cancel-feedback result. Zeroing damage is not an adequate
substitute: PlayImpactEffect is not conditional on positive damage.

Before claiming the reporter's issue fixed, obtain the current RDF/CIF logs
and actual runtime projectile type (64 Arrow vs 63 Missile), then verify the
final slot owner and original handler module. Test pass-through into open air,
into a wall and into a non-dodging actor. The working explosion/area protection
does not need to be removed for this compatibility correction.

Source: https://github.com/Seb263/SkyrimSE_CoreImpactFramework/tree/562ac4d231fb28db718a43e9301a2cd1f572624f
