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

## 2026-10-19 — review fixes

* **Graph depth instead of Left events.** Havok fires the to-state's enter notify when a transition begins
  and the from-state's exit notify when the blend ends. A chain A->B therefore reports `Entered(B)` first and
  `Left(A)` about one blend later, so a bare "Left means the dodge is over" would kill a healthy chain.
  The sink now keeps `depth = #Entered - #Left` (clamped at 0): start 0->1, chain 1->2->1, real exit 1->0.
  Any `Entered` completes the start/chain handshake; ownership ends only on the 1->0 transition. Depth is
  reset on load/new game (`ClearEvents`). While waiting for the handshake, a lowering depth is ignored.
* **`RDF_DodgeClipEnd` is a handover request, not an interrupt.** The graph returns to Default_State by
  itself, so we run the same path as the locomotion exit (blend-out of the speed profile, `exitBlend`) without
  sending `RDF_DodgeExit`. Velocity stays continuous; ownership ends when depth reaches 0 or the core completes,
  whichever is first. Note `fExitBlend` (0.15 s) is shorter than the graph's 0.25 s out-blend; raise it in the
  ini if the tail should match.
* **Clip/timeline sync.** `RDF_DodgeSpeed = fAnimSpeed * fClipLength / (fStartup+fActive+fRecovery)`
  (`Timing::PlaybackSpeed`, unit tested), default `fClipLength=0.667`.

## 2026-10-19 — first in-game traces
* Axes confirmed: requested vs actual displacement match per frame, heading error ~0 while nothing else rotates the
  actor. The motion hook runs on several job threads (seqlock snapshot holds up).
* Yaw fight: in TDM 360 mode TDM turned the player back while the dodge turned it; the internal facing then drifted
  from the real yaw (heading errors up to 180 deg). Fixes: facing is closed-loop on the actor's real yaw each step;
  FaceMotion dodges take TDM yaw control (`RequestYawControl`, released when the dodge stops).
* In TDM "Vanilla Style" (or without TDM) the camera owns the facing, so `sOrientation=Auto` now keeps facing and
  picks the directional clip unless TDM reports free (360) movement; no yaw is commanded in that mode.
* Pause after a dodge with input held: the engine only sends `moveStart` when input begins, so locomotion sat idle.
  The plugin sends `moveStart` on the locomotion handover when move input is held.
* Second in-game run: yaw tracking holds (mean heading error 0.3-7 deg, actor ends facing its path). But TDM's
  `GetDirectionalMovementState()` is true in "Vanilla Style" as well, so every dodge took the FaceMotion path and
  snapped the character around before backward dodges. Free rotation now means
  `GetDirectionalMovementMode() == kDirectional` (API V3).
* `moveStart` sent in the exit tick was refused: the dodge state is still active then. It is now sent when the graph
  has left every dodge state (depth 1 -> 0) and move input is still held.
* Third run: mode detection right (Vanilla Style -> directional clips 9/11/13/15, 360 -> clip 0 + turn), 45
  dodge->dodge chains, direction never stuck. `moveStart` is refused even after the graph left the dodge: locomotion
  is already running, so the end-of-dodge stop is the fixture clip settling into a full stance. Removed the
  moveStart path; `fExitFrom` default 0.45 -> 0.34 so the hand-over to running starts before that pose.
* 360 mode spun the character 180 deg before backward dodges. New `fFaceMotionMaxAngle` (100): wider dodges keep
  facing and play the backward/diagonal-back clip. The facing decision is made once per dodge (per-frame flags
  could flip it mid-dodge). Lock-on yaw removed from the core: TDM turns toward the target itself.

## 2026-10-19 — clip annotations drive the windows
* serde-hkx reads the fixture clips: uncompressed (`hkaInterleavedUncompressedAnimation`, 99 tracks x 20 frames), root
  motion only as `animmotion` annotations. roll_* and step_* are identical; each direction has its own speed curve
  (forward peaks at 0.40 s, left at 0.10 s), so one ini timeline cannot fit all 8.
* XML round trip is stable (xml -> hkx -> xml identical); hkx bytes differ only in float low bits (6-decimal text).
* `RDF_*` annotation events registered in 0_master; plugin switches a dodge to annotation windows on `RDF_Windows`.
* Validation: once `rdf` has a Pandora priority slot, mods after it append their events after ours and get renumbered.
  `validate_rdf.py` now compares objects with event/variable ids replaced by names (87/87 against a fresh baseline).
* In-game: annotations fire (91 window events / 29 dodges, `RDF_Windows` at clip start every time).
* 360-mode backward dodges curved forward: TDM rewrites `PlayerControls::moveInputVec` to "forward" relative to the
  character it turns, so steering chased forward at 260 deg/s. Input now comes from TDM's
  `GetActualMovementInput()` (raw stick) when TDM is present.
* The stop at the end persists although displacement continues (~280 u/s after the exit). Dev trace now follows 30
  ticks past each dodge with the locomotion graph floats `Speed` / `SpeedSampled` / `Direction` to find what restarts.
* Diagonals broke with TDM's `GetActualMovementInput()`: TDM fills it per input event, so with W+D held it holds only
  the last key's axis (every diagonal became 0/90/180). The plugin now tracks the movement keys (via the control map)
  and the left stick in its own input sink; PlayerControls only gates "moving or not".
* The end-of-dodge stop, measured: the dodge exit notify fires when the out-transition STARTS (not at blend end as
  assumed for chains), the plugin released motion there, and the graph stayed animation-driven for the 0.25 s blend
  with the clip's zero end motion -> ~13 frames planted. Then locomotion `Speed` restarts at 0 and ramps to 292 in
  ~0.17 s. Fix 1: an exit we/clip-end started keeps driving the hand-over through the blend. The ramp is logged
  (graph Speed vs process currentMovementSpeed / pathingCurrentMovementSpeed) before deciding what to seed.
* "Press -> twitch, no dodge" (rare since the start): a dodge pressed during the previous dodge's 0.25 s out-blend sent
  RDF_DodgeStart into RDF_Dodge_A, the state being left. Havok reported Entered, the dying blend kept the actor
  animation-driven for ~0.28 s (plugin moved it), then the graph settled in Default_State: the clip never played.
  Fix: a second root wildcard `RDF_DodgeStartB` -> RDF_Dodge_B; the plugin alternates A/B for every new dodge, and a
  chain decided after the graph already left the dodge states enters as a start into the other state (4 chains were
  refused for that reason in the same log). validate_rdf: 90/90.
* Twitch seen again on the first dodge after a load: Entered and Left in the same tick, no `RDF_Windows`, while the
  graph stayed animation-driven ~0.3 s (out-blend) and the plugin moved the actor. Something transitions out of the
  dodge state immediately; not the out-blend case. Dev log now records every graph event (tag, payload, depth) from
  each dodge start until the trace tail ends, plus `bInJumpState` and depth in the trace.
