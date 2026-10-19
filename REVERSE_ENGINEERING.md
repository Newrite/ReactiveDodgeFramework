# Reverse-engineering findings — SkyrimSE.exe 1.5.97

Database: `E:\Reverse\SkyrimSE.exe.i64` (imagebase 0x140000000). Address Library:
`version-1-5-97-0.bin` from the SKSE64 mod; IDs resolved with `tools/addrlib.py`.
Confidence: **O** observed in disassembly, **I** strongly inferred, **H** hypothesis to verify in game.

## Root motion / animation-driven movement

| What | ID | Address | Conf. |
|---|---|---|---|
| `MovementTweenerAgentAnimationDriven` vtable (secondary, `IMovementTweenerAgent`) | — | `0x14166C758` | O |
| `MovementTweenerAgentAnimationDriven::BuildOutput` (vtbl+0x18 of the secondary vtable) | 41160 | `0x1407020E0` | O |
| `Character::ProcessMotionData(Actor*, float dt, NiPoint3* outT, NiPoint3* outR, bool* outFlag)` | 31949 | `0x1404E6360` | O |
| the only code call site of 31949 (`41160 + 0x111`, bytes `E8 6A 41 DE FF`) | — | `0x1407021F1` | O |
| `Character::SetAnimationDrivenTweenerEnabled(Actor*, bool)` → `currentProcess->high + 0x46D` | 36910 | `0x140607C70` | O |
| `PlayerCharacter` sub reading graph bools `bAnimationDriven` (FixedStrings+0x568) and `bAllowRotation` (+0x570) and sending the matching internal graph event to the actor's own sink | — | `0x1405D8680` | O |

Details (O unless marked):
* `BuildOutput` first resets the output transform to identity, resolves the actor handle,
  requires `Actor::boolFlags & kShouldAnimGraphUpdate (0x100)` (Actor+0x1FC), then calls
  31949 with `rcx=actor`, `xmm1=[stage+0x1C]` (dt), `r8=outTranslation` (= the output packet's
  first 12 bytes), `r9=&rotation` (stack NiPoint3), `[rsp+20h]=&bool`.
* If it returns true the translation is optionally rotated by the movement controller yaw at
  `controller+0x24C` (only when global `byte_141DF1618` is set), and the rotation becomes a
  Z-rotation matrix when its X/Y are zero, otherwise a full euler matrix.
* 31949 iterates the active clips of the actor's graph(s), looks up each clip's motion data in
  `AnimationClipDataSingleton` (i.e. animationdatasinglefile translation/rotation keys),
  computes the delta between `localTime - dt*playbackSpeed` and `localTime`, weights it by
  clip weight, normalises by total weight and multiplies the translation by the reference
  scale. Returns false when no clip with weight > 0 contributes. So `outT` is a per-`dt`
  displacement; frame: actor-local (I — derived from the per-clip rotation by the clip's root
  rotation; to verify in game with the diagnostics trace).
* When `stage->state == 1` and the flag returned false, `BuildOutput` disables the tweener
  for that actor via 36910.

Implication used by the design: owning this call = owning the actor's animation-driven
displacement, with the character controller still doing collision (I).

## Controls-driven movement and the post-dodge speed

| What | ID | Address | Conf. |
|---|---|---|---|
| `Character::ApplyMovementDeltaAndFeedback` (player movement delta, feeds the speed graph vars) | — | `0x1405D87F0` | O |
| call of `MovementControllerAI::BuildAggregateStatePayload` (MovementControllerNPC vfunc 7, `call [rax+38h]`) | — | `0x1405D8A49` | O |
| payload layout: direction NiPoint3 at +0x00, **speed float at +0x0C** (read at `0x1405D8AC0`), vector at +0x10 | — | — | O |
| `AIProcess::SetCurrentMovementSpeed` (middleHigh +0x2A8) | 38841 | `0x14067CF60` | O |
| `PlayerControls` speed update: target from input, only deceleration limited | — | `0x1407072D0` | O |

In controls-driven mode the player moves by direction * speed * dt from that payload; after animation-driven mode
ends the speed restarts low and climbs back (traced 317 -> 127 -> 292 u/s over ~8 frames). RDF floors it right after
a dodge (vfunc 7 hook).

## Projectiles

`Projectile::Func190` (`0x1407521F0`, vfunc 190) takes `(Projectile*, hkpAllCdPointCollector*)` and turns contacts
into hits (O; CommonLib's `HandleHits(hkpCollidable*)` name has the wrong argument). Shared by ArrowProjectile
(vtable ID 263776), MissileProjectile (263942) and three other projectile vtables.

## Behavior graph behaviour observed in game

* A state's exit notify fires when its out-transition **starts**; the out-blend keeps the state's modifiers (and so
  `bAnimationDriven`) active until it ends (O, traces).
* A wildcard into the state that is currently blending out is entered and then lost: the actor stays
  animation-driven by the dying blend and the clip never plays (O). RDF alternates two identical states.
* A clip played for the first time after a load is still streaming: zero length, so a relative-to-end clip trigger
  fires on entry (O). RDF has no clip-end exit.
* Clip annotations reach `BSAnimationGraphEvent` sinks only when their text is a registered graph event (O).
* TDM: `GetDirectionalMovementState()` is true in Vanilla Style too; `GetDirectionalMovementMode()` distinguishes;
  `GetActualMovementInput()` holds only the last input event's axis; in 360 mode TDM rewrites
  `PlayerControls::moveInputVec` to character-forward (O, traces).

## Hook sites used by other installed mods (from their sources, not yet re-checked here)

| Mod | Site | Purpose |
|---|---|---|
| Precision / TDM | 35565 + 0x748 | per-frame update |
| Precision | 36359 + 0xF0 | per-actor `ApplyMovement(Actor*, dt)` inside the movement job |
| AMR | 31812 / 31813 (inside 31949) | rewrite clip translation / rotation data |
| The Ultimate Dodge Mod | 37673 + 0x3C0 | melee `ProcessHit` (i-frames) |
| BDI | 62640 + 0x22A | graph symbol-id map injection |

Compatibility notes: AMR rewrites inside 31949; our hook replaces the *call* to 31949 only while
a dodge owns the actor, so AMR still applies to every other animation. Precision's dt scaling at
36359 happens upstream and is honoured because we multiply by the `dt` 31949 receives.

## Other engine facts used

* `Actor::Update` vfunc 0xAD (CommonLib `Actor.h`), `PlayerControlsData::moveInputVec` at +0x00
  (CommonLib), `HighProcessData +0x46D` animation-driven flag / `+0x46F allowRotation` (CommonLib
  field names `unk46D` / `allowRotation`, write site 36910 — O).
* `Actor::boolFlags` bit 8 = `kShouldAnimGraphUpdate` gates motion processing (O at `0x1407021C1`).
