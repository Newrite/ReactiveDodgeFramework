# Explosion i-frame audit — 2026-10-28

Implementation follow-up: the missing per-target area filter is now implemented.
See [correction details](IFRAMES_DELIVERY_FIXES_2026-10-28.md). The audit below
describes the source state before that correction; gameplay validation remains open.

## Scope and conclusion

Read-only investigation of the current source and Skyrim SE 1.5.97.0 in IDA
(`E:\Reverse\SkyrimSE.exe.i64`, image base `0x140000000`). No gameplay reproduction
or DLL change was performed. The reported attack/mod and the reporter's runtime
are not yet known.

The existing Explosion hook has the correct address and signature for this
runtime. However, it does not cover the independent area-effect delivery in
`MagicCaster::MagicHit`. A projectile striking the environment can deliver its
spell to nearby actors through this path without an impact on those actors and
without passing through `Explosion::HasLineOfSight`. This is a confirmed coverage
gap, and a plausible explanation for the reported failure; it is not yet a
confirmed reproduction of the reporter's particular attack.

## Verified engine paths

### Physical explosion and the explosion's own spell

- `0x140739630`: `Explosion::ApplyToTargets`, Address Library ID 42677.
- `0x1407399BC` (`+0x38C`): calls `0x14073B0E0`, ID 42680. Arguments are
  RCX = explosion, RDX = mutable origin point, R8 = target actor.
- `0x1407399C1`: `test al, al`; `0x1407399C3`: `jz 0x140739B87`.
- Returning false skips the HitData application at `0x140739A0C`, the
  explosion's spell/enchantment cast at `0x140739AB1`, and knockdown at
  `0x140739B5A`.
- Image-space effects and a script-event dispatch (`0x1407398EB`) happen
  before this gate. A visual reaction alone therefore does not prove HP damage;
  script-driven consequences of the earlier event are not covered by this gate.
- The Ignore LOS flag is checked *inside* `HasLineOfSight` at `0x14073B137`.
  It does not bypass RDF's call-site thunk.
- The observed caller at `0x14073AFFF` gates ApplyToTargets on runtime flags
  `(flags & 6) == 2`; ApplyToTargets sets bit 4. This path does not establish a
  repeated damage retry after i-frames expire. The separate `0x140739C40` path
  handles Havok impulse processing.

### Projectile spell area effects — missing protection

- `0x14074C710`: projectile ProcessImpacts.
- `0x14074CA2C`: calls `MagicCaster::MagicHit` (`0x14054DC90`) with the impact
  position, projectile, and collided reference.
- `0x14054DEF3`: MagicHit gathers area targets around that position.
- `0x14054E132`: calls `0x140551B80` for each eligible spell effect.
- `0x140551B80`: iterates the gathered references, checks caster/direct-target
  exclusion, ghost state and distance, constructs the effect delivery data.
- `0x140551DCA`: virtual `MagicTarget::AddTarget` call applies the effect to
  each area target. No call to Explosion::HasLineOfSight is on this path.

RDF's AddImpact/HandleHits hooks filter actual projectile contacts. If the
contact is with the ground or another reference, the nearby dodger is not that
contact. The subsequent area-effect iteration is not intercepted anywhere in
the current `src` tree. The melee HitData call-site hook cannot cover magic
effect delivery either.

## Other relevant behavior

- Explosions currently check the active i-frame window at application time;
  they do not inherit the remembered projectile/dodger exclusion. A projectile
  avoided earlier can explode nearby after protection expires.
- Default time window is 0.05–0.35 seconds, subject to settings, keywords and
  authored animation windows. Undodgeable/perfect-only attacker rules also apply.
- The inspected local RDF log reports successful installation of the explosion
  hook, chained directly to the game's LOS check. This is evidence about the
  local run only, not the reporter's installation or later hook overwrites.

## Recommended correction and validation

Intercept area-effect delivery per target and apply the existing `Dodges` /
`NoteDodgedHit` policy. Preserve delivery to other actors. Scope the filter to
appropriate hostile incoming effects; a global rejection of all AddTarget calls
would also reject healing, buffs and RDF's own cast-on effects. Verify engine
types, return semantics and any post-delivery callbacks before implementing.

In-game acceptance cases: projectile strikes ground beside a dodging actor;
same strike with a non-dodging bystander; direct hit; standalone explosion;
hits outside the i-frame window; healing/buffs; undodgeable/perfect-only rules;
already-running damage-over-time. Log the delivery route, spell/effect,
projectile, target, attacker and protection state to distinguish these cases.
