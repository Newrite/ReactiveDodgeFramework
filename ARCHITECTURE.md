# Reactive Dodge Framework — Architecture

Target: Skyrim SE 1.5.97 only. CommonLibSSE-NG at `S:\Programming\CommonLibSSE-NG`, xmake build.
Evidence for every engine fact is in `REVERSE_ENGINEERING.md` (O = observed in IDA, I = strongly inferred, H = hypothesis).

## 1. The one decision everything hangs on: who moves the character

Skyrim has two displacement sources for an actor (O):

* **Controls-driven** — `MovementHandlerAgentPlayerControls` turns `PlayerControls::data.moveInputVec`
  into a locomotion velocity.
* **Animation-driven** — while the graph variable `bAnimationDriven` is true
  (read by `PlayerCharacter` sub `0x1405D8680`, which switches the movement tweener),
  `MovementTweenerAgentAnimationDriven::BuildOutput` (ID 41160, `0x1407020E0`) calls
  `Character::ProcessMotionData` (ID 31949, `0x1404E6360`) **once per movement update**
  (single call site `0x1407021F1`). That function blends the active clips' motion data
  (animationdatasinglefile MD/RD) into `outTranslation` (actor-local, already scaled by
  refr scale) and `outRotation` (euler delta) for the timestep `dt` it receives in `xmm1`,
  and returns true when it produced motion. The result is fed to the character controller,
  so collision, slopes, stairs and step-up are handled by Havok (I).

**Decision:** a dodge is always an animation-driven graph state (the behavior state sets
`bAnimationDriven = true`, exactly like vanilla attacks/rolls). The plugin replaces the call
at `0x1407021F1` (`write_call<5>`): for an actor whose `MotionDriver` is active, the hook ignores
the clip's motion and writes **our** displacement `v_local * dt` and our yaw delta; otherwise it
forwards to the original. Consequences:

* exactly one authoritative displacement source per frame (no duplicate root motion: the clip's
  motion data is discarded, locomotion velocity is off because the actor is animation-driven);
* the character controller still integrates it, so walls/slopes/stairs behave like any
  vanilla animation-driven move — no position writes, no teleports;
* animations need no motion data at all; if they have it, it can be sampled as an optional
  *speed-profile hint* but never decides direction;
* the hook may run on a movement job thread (H — Precision calls the same per-actor movement
  from `UpdateMovementJob`), so it reads an immutable `MotionCommand` snapshot published by the
  main-thread update through a seqlock; it never touches game objects other than its arguments.

When the graph leaves the dodge state, `bAnimationDriven` drops and locomotion velocity returns.
The handover is smoothed by `MotionDriver` blending its commanded velocity toward the current
locomotion velocity during the recovery phase, so velocity is continuous across the boundary.

## 2. Components

```
InputCollector ──► InputBuffer ──► ActionArbiter ──► ActionController (DodgeAction, …)
 (game thread)      (core)          (core)              │                │
                                                         ▼                ▼
                                                    MotionDriver     GraphCoordinator
                                                   (core + hook)       (game)
                                                         │                │
                                                ProcessMotionData    animation graph
                                                     hook            events / variables
```

Core (pure C++, no CommonLib, unit-tested in `tests/`):

| Unit | Owns | Interface |
|---|---|---|
| `Clock` | game-active timebase: sum of unpaused, timescaled frame dt; frame index | `Advance(dt)`, `Now()` |
| `InputBuffer` | timestamped `Command{kind, t, seq, dir}`; expiry; one-shot consumption; debounce of repeats | `Push`, `Peek(kind, now)`, `Consume(seq)`, `Expire(now)` |
| `ActionPolicy` | data tables: per-action phase timeline and cancel windows (which commands may cancel which phase, earliest time) | plain structs loaded from ini |
| `ActionArbiter` | decides each frame which buffered command (if any) is executed given the current action state and policy; priority order; rejection reasons | `Arbitrate(state, buffer, now) -> Decision` |
| `DodgeAction` | dodge phases `Startup → Active → Recovery → Done` (+`Interrupted`); time in phase; chain count | `Start(dir)`, `Tick(dt, input)`, `Phase()` |
| `Steering` | heading of the trajectory: angular-velocity–limited, phase-dependent strength, response curve, deadzone | `Update(desiredDir, magnitude, dt, phaseGain)` |
| `MotionProfile` | speed(t) curve (startup accel, plateau, decel) scaled to configured distance | `Speed(t)`, `Distance()` |
| `MotionDriver` | composes Steering × MotionProfile → world velocity + yaw rate; velocity continuity at handover; publishes `MotionCommand` | `Tick`, `Snapshot()` |

Game layer (`src/game/`, CommonLib):

| Unit | Owns |
|---|---|
| `InputCollector` | `BSTEventSink<InputEvent*>` on `BSInputDeviceManager` for the dodge / attack / block buttons (keyboard, mouse, gamepad); samples `PlayerControls::data.moveInputVec` (analog) every frame; converts to world space with the camera yaw (or TDM target when locked on) |
| `GraphCoordinator` | sends graph events, sets graph variables (direction, speed hint, phase), listens to `BSAnimationGraphEvent` for the acknowledgements the dodge state emits, detects that the graph really entered/left the dodge state |
| `MotionHook` | the `0x1407021F1` call hook, seqlock reader |
| `UpdateHook` | per-frame tick on the main thread (`PlayerCharacter::Update`, vfunc 0xAD) — the only place core state changes |
| `EngineState` | read-only queries: attacking, blocking, staggered, ragdoll, dead, in killmove, swimming, mounted, sitting, in menu, weapon state, stamina |
| `Effects` | stamina cost, optional i-frames (melee hit / projectile filter during the invulnerability window only) |
| `Lifecycle` | reset on load/new game/death/cell change/3D reset; re-register graph sinks |
| `Settings` | `ReactiveDodge.ini` |
| `Diagnostics` | structured dev log behind `bDevLog`; per-dodge trace (requested vs actual displacement, angle error) |

## 3. Frame order (main thread, inside `PlayerCharacter::Update`)

1. `Clock.Advance(dt)` (dt = 0 when paused → nothing expires during menus).
2. `InputCollector` drains the presses it queued from the input sink into `InputBuffer` with the
   timestamp of the frame they arrived in (presses arriving while paused are dropped).
3. `EngineState` snapshot → forced interruptions (stagger/ragdoll/death/killmove) end the action
   immediately (`Interrupted`) and clear motion ownership.
4. `ActionArbiter` → at most one decision: start dodge, chain dodge, cancel into attack/block/
   locomotion, or nothing. Accepted commands are consumed exactly once.
5. `DodgeAction.Tick` + `MotionDriver.Tick` with the current analog input → publish snapshot.
6. `GraphCoordinator` emits what the decision requires (events/vars). If the graph rejects the
   start event (`NotifyAnimationGraph` returns false) the action is rolled back the same frame,
   no stamina is charged and the command is retried until it expires.

The movement update (later in the same frame) runs the hook, which consumes the snapshot.

## 4. Behavior contract (summary — full text in `ANIMATION_CONTRACT.md` / `BEHAVIOR_INTEGRATION.md`)

* Events in: `RDF_DodgeStart` (enter dodge), `RDF_DodgeChain` (re-enter, self transition with blend),
  `RDF_DodgeExit` (blend back to locomotion/default state). Attack/block use the vanilla
  `attackStart`/`blockStart` paths, which must have transitions out of the dodge state.
* Variables in: `RDF_DodgeAngle` (float, deg, relative to facing at start: selects directional
  clip / blend), `RDF_DodgeSpeed` (playback hint), plus `bIsDodging`, `DF_IsDodging` (TDM cooperation).
* Events out (annotations or state enter/exit notify): `RDF_DodgeEntered`, `RDF_DodgeLeft`.
* The dodge state sets `bAnimationDriven = true` while active.

## 5. Threading & lifecycle invariants

* Core state is mutated only on the main thread in the update hook.
* The motion hook only reads the published snapshot (seqlock, no locks, no allocation).
* A snapshot carries the actor handle and a generation counter; after load/death/reset the
  generation changes and stale snapshots are ignored.
* Ownership of motion ends whenever: the action ends, the graph reports the dodge state left,
  a forced interruption is detected, or the actor stops being animation-driven.
