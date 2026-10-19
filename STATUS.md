# Status (2026-10-19)

## Done and unit tested (`xmake build rdf_tests`, 45 cases)
Clock, Vec2/angle math, InputBuffer, ActionPolicy, ActionArbiter (+ reject reasons), DodgeAction, Steering,
MotionProfile, MotionDriver, Seqlock, ReactiveCore orchestration.

## Done, compiles, NOT yet exercised in game
* Hooks: `ProcessMotionData` call at `0x1407021F1` (bytes `E8 6A 41 DE FF`, target `0x1404E6360` verified
  against SkyrimSE.exe and re-verified at runtime), melee-hit call at ID 37673 + 0x3C0 (`E8 1B D4 FF FF` in the
  exe), `PlayerCharacter::Update` vfunc 0xAD.
* Input sink, graph sink, engine-state queries, stamina, i-frames (melee only), TDM lock-on provider,
  legacy-dodge suppression, lifecycle reset.

## Unverified / to check first in game (set `bDevLog=1`)
1. Thread that runs the motion hook (`trace ... thread=first/last` vs the main thread).
2. Local axes of `outTranslation` (x right, y forward assumed) and the sign of the yaw delta (`iYawSign`):
   compare `requested`, `actual`, `err` in the trace; err should stay near 0.
3. That `bAnimationDriven` is true for the whole dodge (the hook is only called while it is).
4. `TESActionData::Process` from the dodge-exit tick actually starts an attack/block; deferred retry.
5. Block path (`wantBlocking` + `blockStart`) and the dodge-window events (`MCO_DodgeOpen/Close`).
6. First-person dodge is blocked (`bAllowFirstPerson=0`); no 1P graph support.
7. Gamepad default (right-stick click, 0x80) conflicts with nothing in the user's setup.

## TODO
* Projectile i-frames (only melee hits are filtered).
* Left-hand / dual attacks from a dodge.
* Hold-to-block continuation after a dodge when the block button was already down.
