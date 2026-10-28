# Projectile impact feedback during i-frames — 2026-10-28

Implementation follow-up: ray candidates are now filtered before early impact
processing. See [correction details](IFRAMES_DELIVERY_FIXES_2026-10-28.md). The
audit below describes the pre-fix source; gameplay validation remains open.

Static verification in the existing Skyrim SE 1.5.97.0 IDB. No DLL edits or
in-game reproduction performed.

## Finding

RDF can reject a magic projectile's AddImpact and allow it to continue flying
after the engine has already dispatched impact feedback. This is a separate
path from the hkpAllCdPointCollector HandleHits hook.

Verified call sequence:

1. Projectile vfunc 183, `0x14074ED10`, calls the ray-hit processing helper
   `0x14074F650` at `0x14074F549`. Arrow and Missile share this vfunc in their
   primary vtables; Flame has an override that also calls this helper.
2. For a resolved hit reference and a projectile with `spell != nullptr`
   (projectile +0x158), the helper calls `0x140752620` at `0x14074F9BD` or
   `0x14074F9E5`.
3. That helper can dispatch magic delivery through `0x1407528E0` and impact
   feedback through `0x1407531C0` (call at `0x14075280A`). These branches have
   collision-layer, spell and ward-related conditions; not every contact
   necessarily plays a sound.
4. `0x1407531C0` resolves a BGSImpactData from the weapon/effect impact set and
   material. It can send a hit event at `0x14075354D`, create visual effects,
   and dispatch impact sound at `0x14075388D` through `0x1405A2930`.
5. The latter selects sound descriptors at BGSImpactData +0x70 / +0x78 and
   calls `0x1405A48B0`, which creates/positions the sound and calls
   `BSSoundHandle::Play` at `0x1405A493B`.
6. Only AFTER the early helper returns does `0x14074F650` call virtual
   AddImpact (slot 189 / vtable +0x5E8), at `0x14074FA14`.
7. RDF returns nullptr for an ignored actor. The engine tests that result and
   continues searching hits (`0x14074FA25`), but previously dispatched effects
   and sounds are not undone.

Thus a missing Projectile::ImpactData entry does not mean that no BGSImpactData
feedback occurred. They are different objects/stages.

## Why ordinary arrows differ

The early call in step 2 is conditional on the projectile's spell pointer.
Ordinary arrows with no projectile spell skip it and reach RDF's AddImpact
filter first. Their physics contacts are also filtered by RDF's HandleHits.
This explains the observed asymmetry without assuming different vfunc 183
implementations. Modded arrows carrying a projectile spell need separate testing.

## Other implications

- `0x1407528E0` calls MagicCaster::MagicHit at `0x140752AF6`; the missing early
  filter can therefore affect more than sound. This extends the separate AoE
  coverage gap documented in EXPLOSION_IFRAMES_AUDIT_2026-10-28.md. It does not
  establish that every observed pass-through applies damage.
- The ray-hit helper also has a separate sound branch ending in Play at
  `0x14074FC8A`, independent of successful AddImpact. Its precise sound role
  was not established here; do not indiscriminately silence all projectile audio.
- `0x1407531C0` sets projectile flag 0x100000 before feedback processing.
  Rejecting the hit only afterwards can also change later feedback behavior.

## Correction direction

Reject ignored actor candidates before the ray-hit helper's early spell/impact
processing, while continuing traversal to later targets. Reuse RDF's attacker
rules and projectile/dodger memory. Merely muting sound or returning nullptr
from AddImpact cannot undo the earlier side effects. Keep the existing contact
filter and independently cover area targets when a projectile hits the ground.

Runtime verification: magic missile through a dodger into a wall; into another
actor; ordinary arrow; repeated overlap after i-frames end; streams/cones;
legitimate impact sounds and flight audio. Instrument the early helper and
AddImpact with projectile/target IDs to correlate the audible event.
