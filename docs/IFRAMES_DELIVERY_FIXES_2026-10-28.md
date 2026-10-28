# Area spell and projectile impact i-frame corrections

Follow-up after the user's runtime test: explosion/AoE damage is reported fixed,
but impact feedback remained. Additional all-type contact and feedback coverage
is documented in [the full projectile trace](PROJECTILE_ALL_TYPES_TRACE_2026-10-28.md).

Runtime target: Skyrim SE 1.5.97. Both original audits were checked against the
open SkyrimSE IDB before implementing these corrections.

## Projectile candidates

The ray helper (ID 42983 / 0x14074F650) resolves each collidable to a scene object
at +0x1BC (0x14074F80C). A call adapter passes its existing RCX collidable and
RBX projectile into RayCandidateThunk. The thunk calls the original object
resolver, and uses the same reference resolver as the engine (19323).

If that reference is an actor ignored by the existing Ignore policy, return
nullptr instead of the object. The engine's null-object branch advances to the
next ray candidate. This precedes early spell delivery, impact events, VFX,
sound, impact flags and AddImpact. A wall or another actor behind the dodger can
still be hit. Flight/near-miss sound processing outside this contact branch is
not silenced. Existing HandleHits and AddImpact filters remain.

Remembered projectile exclusions and attacker rules are shared with AddImpact.
Arrows, missiles, grenades and cones retain their exclusions; beam/flame/barrier
streams only skip during active protection. A remembered pass-through does not
count as a new perfect-dodge hit after the window closes.

Installation requires the expected E8 call to resolver 15644, otherwise it logs
an error and leaves that site alone. Controlled by bIFrames + bProjectileIFrames.

## Per-target area magic

ID 33686 / 0x140551B80 is MagicHit's independent area-effect loop. At +0x10B
(0x140551C8B), the engine has already excluded caster/direct target, checked ghost
state and checked distance. R13 is the reference; R12 points to a per-effect
context (Effect* at +0x10, MagicItem* +0x20, caster reference +0x30).

A generated branch calls IgnoreAreaTarget. It rejects only actor targets of an
incoming effect whose base setting is hostile OR detrimental, while Dodges says
the attack can be avoided. It uses the same undodgeable/perfect-only attacker
policy and records the dodged hit for perfect-dodge handling. Null base effects,
beneficial effects and self delivery are left to the engine.

Rejected targets jump to +0x2C9 (0x140551E49), the common reference-release path.
This skips BOTH MagicTarget::AddTarget and the independent follow-up at
0x140551DF0 / 0x140551E33, which a false AddTarget return alone would not skip.
Other targets continue through the engine's loop normally. Allowed targets replay
the displaced five-byte `mov rcx,[r12+10h]`, then resume at 0x140551C90.

The stub allocates 0x20 bytes of shadow space with the existing call-aligned
engine stack; live nonvolatile registers and XMM6+ follow the Windows x64 ABI.
The original five bytes are checked before installation. Controlled by bIFrames
+ bExplosionIFrames. The existing physical explosion LOS hook remains, including
its original limitation: pre-gate script/image-space events are outside this fix.
Existing running active effects are not dispelled; a later nearby explosion is
evaluated against the active window, independently of remembered direct contact.

## Verification and game checks

Two native offline regression tests execute the ACTUAL generated adapter bytes
inside synthetic engine frames. They exercise both area branches, restore the
frame/return path, verify the displaced effect pointer, and verify the ray
adapter retains both arguments and null/non-null return values.

Gameplay checks remain manual: ground-hit AoE beside a dodger and an unprotected
bystander; physical explosion; direct magic projectile through a dodger into a
wall/second actor; ordinary and enchanted arrows; beam/flame/cone; outside-window
hits; healing and buffs; undodgeable/perfect-only rules; existing damage-over-time.

With bDevLog=1, look for `ray candidate projectile ... before impact processing`
and `magic area spell ... effect ... from ... skips ...`. Installation logs name
both new sites. Compilation/offline frame tests do not establish gameplay success.
