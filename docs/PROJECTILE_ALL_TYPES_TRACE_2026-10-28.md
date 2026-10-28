# Projectile impact follow-up: all seven types (SE 1.5.97)

Release status: the user reported that the seven additional delivery/feedback
call-site filters did not remove the observed impact. Those experimental filters
and verbose per-impact diagnostics were removed for v0.3.0. The trace below is
a historical record of that attempt, not a description of the released hooks.
Ray-candidate, ordinary/stream contact, AddImpact and area/explosion protection
remain. See CIF_IFRAMES_ORDER_AUDIT_2026-10-28.md for a possible external route;
the exact reported interaction remains unverified.

Follow-up to the user's `F:/Downloads/ReactiveDodgeFramework (2).log` and report:
explosion damage is now blocked, but a Fireball / magic-arrow variant still emits
fire impact VFX and sound. Inspection used the existing SkyrimSE IDB and local
`E:/Reverse/SkyrimSE.exe`; no game launch was performed.

## What the log establishes

Both new hooks installed successfully at startup. With development logging later
enabled, MagicHit area effects and explosion targets are visibly skipped. There
are remembered projectile pass-through messages, but no ray-candidate messages
in the supplied log. It does not identify those projectile types or which sound
route fired. A particular remaining fireball impact is therefore not attributed
to a specific route by this log alone.

## Primary vtable trace

All addresses below are SkyrimSE image-relative RVAs, NOT hardcoded live addresses.
172 = ProcessImpacts, 183 = ray/line collision, 189 = AddImpact, 190 = HandleHits.

| Type | 172 | 183 | 189 | 190 |
|---|---|---|---|---|
| Arrow | 731FE0 | 74ED10 | 732400 | 7521F0 |
| Missile | 7454F0 | 74ED10 | 747810 | 7521F0 |
| Grenade | 74C710 | 74ED10 | 73FC10 | 7521F0 |
| Cone | 7356A0 | 74ED10 | 735AA0 | 735B70 |
| Beam | 74C710 | 74ED10 | 734170 | 7521F0 |
| Flame | 74C710 | 73DD40 | 73E5F0 | 7521F0 |
| Barrier | 733350 | 74ED10 | 751C30 | 7521F0 |

Arrow's ProcessImpacts chains into Missile's, and Missile chains into base
ProcessImpacts. Cone and Barrier also call base ProcessImpacts. Flame's separate
183 implementation calls the same 74F650 ray helper. The earlier ray gate thus
covers that shared helper but does not cover physics contacts or deferred hits.

### Physics contacts

7521F0 calls magic contact helper 752620 at 752441 BEFORE virtual AddImpact at
7524BF. The helper performs magic delivery and BGSImpactData feedback. Previously
Beam/Flame/Barrier only hooked AddImpact, so they had the same early-feedback gap
that the previous fix had closed for ray candidates. Their HandleHits collectors
are now filtered too, retaining other contacts and restoring the collector.

Collector filtering now takes each type's `remember` policy. Streams pass false;
they must not inherit the previously hardcoded true used for single projectiles.
Cone retains its specialized HandleHits: actor delivery checks AddImpact's result
before it invokes magic contact. Non-actor physics/impulse paths are preserved.

### Barrier overlaps

BarrierPhantomCallbackShape at 7327B0 calls 7334D0. That routine gets the reference
from the collidable, maintains the engine's overlap counters, calls AddImpact,
and invokes 752620 only if an impact was returned. Its magic-contact call is
independently guarded as well. Engine-owned overlap counters are not rewritten.

### Deferred impact feedback

Base ProcessImpacts 74C710 calls 7531C0 at 74CB20, after processing queued impact
records. This is a separate feedback caller from the ray/physics magic helper.
It is now guarded using the target reference and the same Ignore policy.

### Cone ground feedback

Cone helper 7369B0 traces downward and calls 7531C0 at 736E29 with a NULL target.
This is ground feedback rather than an actor hit. Its caller is covered by the
feedback wrapper, but a null/world target is allowed. Silencing this just because
someone nearby dodges would remove legitimate ground VFX/sound.

## All direct native call sites guarded

These are ALL direct code xrefs to the two helpers in this EXE. Each installation
requires E8 with the expected original destination; unexpected patches log an
error and are left alone. Per-site original functions are stored independently.

| Caller ID + offset | RVA | Callee | Route |
|---|---|---|---|
| 42567 + 13A | 73360A | 43014 / 752620 | Barrier overlap magic contact |
| 42634 + 727 | 736297 | 43014 / 752620 | Cone actor magic contact |
| 42983 + 395 | 74F9E5 | 43014 / 752620 | Ray magic contact |
| 43013 + 251 | 752441 | 43014 / 752620 | Physics magic contact |
| 42638 + 479 | 736E29 | 43022 / 7531C0 | Cone ground feedback |
| 42943 + 410 | 74CB20 | 43022 / 7531C0 | Deferred impact feedback |
| 43014 + 1EA | 75280A | 43022 / 7531C0 | Magic contact feedback |

Note on the earlier audit: Hex-Rays attached a semantic call line to 74F9BD in
one branch; the actual single CALL instruction is at 74F9E5, as raw disassembly
confirms. This implementation patches the instruction, not the pseudocode line.

43014 receives seven arguments; the last is a one-byte output flag. Rejected
contacts clear that flag and return false. 43022 receives six arguments; the
wrapper rejects ignored actor targets before the function sets flag 100000,
sends hit/detection events, creates particles/decals/hazards or plays sound.
The original function is called unchanged for other targets. Attacker rules,
perfect-only eligibility and remembered projectile exclusions remain shared.

## Other sound/effect paths

- The 74F650 sound branch ending at 74FC8A reads BGSProjectile +B8, which is
  DATA +38 = activeSoundLoop. It is projectile flight/active audio, not the
  BGSImpactData sound1/sound2 (+70/+78). It remains enabled.
- 753AB0 applies a physical impulse after deferred feedback; it is not an impact
  audio dispatcher. Existing contact/impact rejection remains the upstream gate.
- 753D40 handles explosions, with impact-list/proximity/timer branches. Grenade
  slot 191 only sets flag 40000000. Timer/proximity explosions are not globally
  suppressed; the existing per-target explosion/AoE i-frames still protect actors.
- 7503B0 is also invoked from ActorMagicCaster's launch code for initial decal
  placement, independently of hitting an actor. It is not the hit-sound funnel.

External plugins can call engine helpers directly or spawn their own effects;
native EXE call-site coverage does not prove coverage of arbitrary plugin code.

## Verification and next game check

SE DLL build passed; all 129 existing C++ tests pass. The two generated adapters
are unchanged. All seven new call-site IDs, offsets, E8 bytes and destinations
were independently checked against the local PE image using the Address Library.
Evidence: `_work/projectile-all-routes-sites-20261028.txt` and
`_work/projectile-all-routes-tests-20261028.log`.

New development logs show rejected AddImpact and physics contacts (type and
remember policy), magic-contact routes, and EVERY feedback dispatch, including
allowed world hits: route, projectile/base IDs, target/type, material, ignored
flag and position. Runtime type IDs: 63 Missile, 64 Arrow, 65 Grenade, 66 Beam,
67 Flame, 68 Cone, 69 Barrier; actor target type is 62.

Retest the same Fireball variant through a dodger with open space behind, then
with a wall/bystander behind. Keep bDevLog=1 for a short reproduction. An
`ignored=true` actor dispatch should produce no feedback; an allowed wall/ground
dispatch can legitimately produce an effect after pass-through. Actual behavior
of the reported variant remains for the user to verify in game.
