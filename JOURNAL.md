# Journal

## 2026-10-19 — C++ plugin implementation

Deviations from / refinements of ARCHITECTURE.md, with reasons.

* **Two-phase arbitration.** `ReactiveCore::Evaluate` is pure; `Apply(decision, accepted)` consumes the
  buffered press and changes state only when the game layer's side effect succeeded (graph accepted the
  event, `TESActionData::Process` returned true). A refused effect leaves the press buffered until it
  expires, which is the retry the architecture asks for, without a rollback path in the core.
  `PreviewStartAngleDeg` lets the game layer ask the graph before the core commits.
* **Mashed presses.** Accepting a command consumes every pending command of that kind pressed at or
  before "now" (`ConsumeKindUpTo`); a fresh (non-chain) dodge also consumes pending attack/block presses,
  because they belong to the action that preceded the dodge.
* **Velocity published = average over the tick** (`distance(t1)-distance(t0)) / dt`), not the
  instantaneous speed, so the hook integrating `v * dt` covers exactly the configured distance at any frame
  rate (tests: 30..240 fps, spikes up to 0.25 s). The hook converts to actor-local using the heading at the
  *middle* of the yaw step because the engine applies translation and yaw in the same step (order inside
  `ProcessMotionData`'s consumer unverified).
* **Final contract (coordinator message):** the plugin sets only `RDF_DodgeClip` (int) and
  `RDF_DodgeSpeed` before `RDF_DodgeStart/Chain`; `RDF_DodgeAngle`, `bIsDodging`, `DF_IsDodging`,
  `RDF_Phase` from the first draft are NOT written. `RDF_IsDodging` is read for diagnostics only. FaceMotion
  always sends clip 0 and the character snap-turns (`fStartupYawRate`, 3600 deg/s); KeepFacing picks the
  8-way index from the angle between the path and the facing (+8 for sidestep, `bSidestepWhenKeepFacing`).
  `Left` + `Entered` in the same tick is a chain, not an exit; a `Left` while waiting for `Entered`
  belongs to the state being left; `RDF_DodgeClipEnd` ends ownership.
* **Cancel into attack/block** sends `RDF_DodgeExit` and runs the action in the same tick. If the action
  fails the dodge is ended in the core (motion released), the press stays buffered and the controller retries
  every tick until it expires (`Controller::RetryDeferred`).
* **Attack = `TESActionData` + `Process()` (vtable slot 5 = Address Library ID 40551, checked against the
  vtable in SkyrimSE.exe), actions `kActionRightAttack` / `kActionRightPowerAttack`.** Left/dual attacks
  are not issued. Block = `actorState2.wantBlocking = 1` + graph event `blockStart` (no clean block action
  record). The `TESActionData` is released with the default delete exactly like Input Support does in this
  modlist; if that ever shows heap corruption, switch to `~TESActionData` + `RE::free`.
* **Motion ownership starts when the graph accepts the start event**, before `RDF_DodgeEntered`. If Entered
  does not arrive within `iEnterTimeoutFrames` the dodge is rolled back and the stamina refunded.
* **maxCarrySpeed** caps only the locomotion speed carried into a fresh dodge, not the carry of a chain
  (a chain legitimately starts at the previous peak speed).
* **Legacy dodge.** STB.dll's Left Alt dodge is silenced by zeroing the TUDM globals `UDDodgeKey` and
  `UDSprintDodge` on kPostLoadGame/kNewGame (`bSuppressLegacyDodge`); original values are logged with the
  console command to restore them.
* **Stamina** uses `ActorValueOwner::DamageActorValue/RestoreActorValue` (CommonLib has no
  `RestoreActorValue(modifier, ...)` overload on Actor in this tree).
* **Trampoline** is allocated through `SKSE::Init(..., {.trampoline = true, .trampolineSize = 64})`
  (the old `AllocTrampoline` is deprecated).
