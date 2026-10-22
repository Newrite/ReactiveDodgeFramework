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
* Twitch, real cause (graph event log): `RDF_DodgeEntered` -> `RDF_DodgeClipEnd` -> `RDF_DodgeLeft` -> `MTState` in the
  same millisecond. Each dodge state has its own 16 clip generators; the first time one plays after a load its
  animation is still streaming in, the clip has zero length, and the relative-to-end trigger fires immediately; the
  clip-end exit transition then dropped the dodge. Rare because it needs the first use of one of 32 state/clip pairs.
  Fix: no clip-end trigger and no clip-end exit; the plugin ends every dodge (`RDF_DodgeExit`), IdleForceDefaultState
  stays as the engine's safety exit. (The earlier note that RDF_DodgeClipEnd is a hand-over request is obsolete.)
* Post-dodge speed-up, RE: in controls-driven mode `Character::ApplyMovementDeltaAndFeedback` (0x1405D87F0) moves
  the player by direction * speed * dt from `MovementControllerAI::BuildAggregateStatePayload` (MovementControllerNPC
  vfunc 7, call at 0x1405D8A49; payload speed at +0x0C, read at 0x1405D8AC0 into SetCurrentMovementSpeed
  0x14067CF60 = ID 38841). After animation-driven mode ends that speed restarts low (trace: 317 -> 127 -> 292 over
  ~8 frames). Fix: vtable hook on vfunc 7, player only via the motion snapshot: after a dodge that completed into
  running, the payload speed is floored at the exit speed decaying at `fCarryDecay` (1200 u/s^2). Direction and the
  engine's own acceleration stay untouched. Not yet verified in game.
* Projectile i-frames: `Projectile::Func190` (0x1407521F0, vfunc 190) takes (Projectile*, hkpAllCdPointCollector*)
  (verified in IDA; CommonLib's name "HandleHits(hkpCollidable*)" has the wrong argument). It is shared by Arrow,
  Missile and three other projectile vtables. Hooked on ArrowProjectile and MissileProjectile: while invulnerable and
  a contact root collidable resolves to the player, that step's contacts are dropped and the projectile flies on.
  Beams/flames (continuous) are not covered. `bProjectileIFrames` (default 1). Not yet verified in game.
* Dodge -> attack / block, first in-game run: both work (attack on the first try, graph straight into the attack;
  block starts with `blockStartOut` and lasts while the button is held), but only at ~0.54 s of a 0.70 s dodge: the
  shared 60 % annotation point opened them as late as the chain, and with movement held the exit came first and
  swallowed the press. Now: attack/block may leave a dodge once its exit window is open or the hand-over began, and
  the annotation rule has a threshold per window (attack/block 90 %, capped at 0.35 s; chain/exit 75 %; i-frames end
  60 %). Attack/block now open at 0.20-0.35 s of clip time.
* Block from a dodge, second run: a press 0.10 s into the dodge expired before BlockOpen (0.36 s), and a block that did
  start was dropped by the game after ~0.2 s although the button was held: the game's block handler never saw the
  button-down (it came during the dodge). Now a held button keeps its buffered press alive (`InputBuffer::KeepAlive`),
  and a block the plugin starts is kept (`wantBlocking`) while the button stays down and ended with `blockStop`.
* Dodge -> attack "feels like waiting": the attack transition starts 3-6 ms after the decision (mid-dodge); the first
  hit frame follows 0.52-0.56 s later (the weapon moveset's windup). What the plugin did wrong: it released the motion
  at the cancel, so the character lost all dodge speed in one frame. Now the momentum decays to rest over
  `fActionExitBlend` (0.15 s); during that carry the dodge is "over" for decisions (no chain out of the attack), and no
  post-dodge speed floor follows an attack/block cancel.
* Arm stance layer (graph by the behavior agent, 127/127): each dodge state blends the dodge clip with the current
  weapon's ready-idle pose on the arms only (vanilla RightArm/LeftArm character-property masks, complementary
  NOT_ARMS mask, stance clips selected by iRightHandType / iLeftHandType). `RDF_ArmBlend` is the parametric blend
  parameter, so the plugin writes the stance share directly: `fArmStanceStep` 1.0 for steps, `fArmStanceRoll` 0.0 for
  rolls, 0 when unarmed. Not yet seen in game.
* First external clip set: the user's TK-Dodge OAR set "PlayerDodge* - 2hm" (5 submods; diagonals were picked by a
  key-reading MGEF because TK Dodge has 4 directions). 8 unique spline-compressed clips (0.667 s, forward 0.833 s),
  root motion 197-234 u, step == normal. Rebuilt as an OAR set over RDF's own clip paths (greatsword, no spear;
  the source's BackwardLeft said battleaxe/warhammer, taken as a slip), annotated by `tools/build_clip_set.py`.
* Greatsword set in game: 134/134 dodges played the set's clips (their own SDS dodge sound / MCO_InputBuffer
  annotations fire), windows arrive at the computed clip times / playback speed (e.g. forward step attack 0.28 s,
  chain 0.35 s), 0 rollbacks, 0 dropped dodges.
* Stance jerk for 1-2 frames when a dodge hands over into running (seen with every clip set, and in other dodge
  mods): on the first frame after animation-driven mode ends the engine's speed fell to ~70 (Speed 319 -> 70 -> 284);
  the speed floor was published only once the dodge had finished, one frame after the engine took the movement back.
  The floor is now published from the hand-over on (following its speed), and the payload hook applies it whenever
  set (the payload exists only in controls-driven mode). Attack -> dodge before the end of the attack: user-verified.
* Hand-over jerk, second look (20 hand-overs, some at sgtm 0.2): the floor now arrived in time, but on the dodge's
  last tick it was set from the command's tick-average speed, which spreads the dodge's final milliseconds over the
  whole frame (301.8 -> 45.6 / 124.3 depending on where the end fell), so some hand-overs still dropped to 48-136 for
  a frame. The floor now uses the driver's instantaneous speed (the exit speed at the end).
* Verified: 51/58 hand-overs into running continuous (Speed 316 -> 290 -> 265 -> 260, the engine's own run speed);
  the other 7 had no floor by design (move input released, or sprinting) and show no one-frame dip either.
* One-frame pose step at dodge START (head 8-27 units in one frame), size tracking how different the clip the same
  state played last time was: the SE hkbManualSelectorGenerator reactivates on its previous child and switches to the
  bound index a frame later, unblended. The dodge clip choosers are now state machines with startStateId bound to
  RDF_DodgeClip (TUDM / vanilla pattern), 128/128. The hand-over end itself showed no pose step (<= 0.4 u/frame).
* 2026-10-20: the GraphProbe dev diagnostic (walk hkbBehaviorGraph::activeNodes as hkArray<hkbNodeInfo>, strcmp node
  names) crashed the game in strcmp on a garbage name pointer: CommonLib's hkbNodeInfo layout (0x90 stride, template
  at +0x50) does not hold for every entry. Removed entirely. Rule kept from this: no unverified engine layout is
  dereferenced in game, not even behind bDevLog; check layouts in IDA first or read them with a guarded tool.
* Start-of-dodge micro pop: parked (user: practically invisible). Facts so far, for whoever resumes it:
  - the in-blend is NOT cut: a vtable probe on hkbBlendingTransitionEffect (IDA-verified: duration +0x50, timeRemaining
    +0x80, timeInTransition +0x84; update slot 5 = 0x140A3F820, deactivate slot 7 = 0x140A3F830) showed RDF_DodgeInBlend
    running its full 0.12 s (6 updates) and deactivating at remaining <= 0;
  - in slow motion the pop is a single-frame spike on the second blend update, when the dodge clip's weight is ~0.4 %:
    the change is in the pose being left (or applied on top), an upper-body straightening (head -17 forward, +5 up,
    pelvis ~still);
  - suspect: TDM's lean, hkbTwistModifiers bound to TDM_Pitch / TDM_Roll in 0_master modifier list #2517 (tdmlen), not
    carried into the RDF states; not yet confirmed. The probe was removed (dev tool for this question only);
  - why the earlier GraphProbe crashed: hkbBehaviorGraph::activeNodes (+0x98, 0x90 stride) is reached through a
    template->index map (+0xA0) and entries may redirect (+0x89 flag, +0x68 index), so a linear walk reads junk.
* Arm stance layer with a sheathed weapon / spell pulled the hands toward the centre: iRightHandType / iLeftHandType
  keep reporting the equipped item while sheathed, so the layer forced its ready pose. RDF_ArmBlend is now 0 unless the
  weapon is drawn (and not fists).
* First person (2026-10-20): the 1P graph got the same two dodge states (behavior agent, 205/205). In 1P the state plays
  the current weapon's 1P stance (or MT_Idle when sheathed) instead of a roll clip, so 1P is covered by blending; all
  clips already exist in FirstPerson.hkx, no Blender work needed. Plugin: CommonLib's AddAnimationGraphEventSink only
  registers on graphs.front(), so 1P dodge events never arrived; the sink is now on every graph and counts events only
  from the active graph (tracked each tick). In 1P the dodge never turns the player. bAllowFirstPerson now defaults to 1.
* First person, motion (2026-10-20): the stance-only 1P dodge felt static. TUDM's 16 1P clips turned out to be byte-identical copies of
  the vanilla 1P sneak roll, and Motion Sensitive Fix SE (active) strips camera motion from many 1P clips. Vanilla does ship real 1P
  dodges (`DodgeLeft/Right/Back`, 0.9 s, already in FirstPerson.hkx). The 1P state now blends them with the stance on RDF_ArmBlend
  (fArmStanceFirstPerson 0.35); forward uses the sneak roll. Validator 213/213. Camera switch mid-dodge now sends RDF_DodgeExit, plus a
  0.6 s watchdog for a graph left in a dodge state the plugin does not own.
* Cast stuck after a dodge (2026-10-20): dodging out of a charging spell left the cast state on until sheathing. Vanilla states
  that take over from casting (Stagger_State, jump/land, swim, ragdoll, bleedout...) send the graph event `InterruptCast` on enter;
  the engine's InterruptCastHandler calls Actor::InterruptCast (37808, verified in IDA: InterruptCast on all 4 casters, a no-op
  without a current spell). The RDF states did not, so the casters stayed charging. Both dodge states (3P and 1P) now send
  RDF_DodgeEntered + InterruptCast on enter. Not STB's partial-cast mechanic; vanilla would behave the same.
* Jump during a dodge, dodge out of stagger (2026-10-20): the root jump wildcards cut any dodge short. JumpHandler::ProcessButton
  (vtable 263065 slot 4 = 0x1407090A0, verified in IDA) calls Actor::Jump with no knowledge of a dodge; a vtable hook (JumpGate)
  swallows the button while a dodge runs and its exit window is closed ([Jump] bBlockDuringDodge=1). Dodge out of stagger:
  [Stagger] bDodgeFromStagger (default 0, meant to be unlocked later) and fDodgeDelay. While staggered the arbiter sees no engine
  block; on a successful start the plugin clears actorState2.staggered the way StaggeredStopHandler (0x140721280: bit 0x2000 at
  +0xC4) does, since the graph leaves the stagger state through our wildcard and its staggerStop never fires. During our own
  dodge only the actor flag counts as stagger, not the graph's IsStaggering (still true while the stagger blends out).
* 1P view locked during a dodge (2026-10-20): the mouse turn of the player is a rotation rate emitted by
  MovementHandlerAgentPlayerControls::BuildDirectControlOutput (0x1406FBA80: lookInputVec.x * movement-type float +0x24 =
  rotateInPlaceRun). While bAnimationDriven the tweener builds the output instead (our ProcessMotionData hook), so the turn is
  dropped; in 3P only the camera orbits, in 1P the camera yaw is the actor's. The hook now adds lookInputVec.x * rate * dt in
  1P (rate read each tick via ActorState vfunc 8 -> Character 0x1406082B0, which writes 56 bytes; fFirstPersonLookScale).
  Pitch goes through PlayerCamera 0x14084D3C0 -> ModifyRotationX and is not animation-driven.
* Precision i-frames (2026-10-21): Precision (active in STB) runs its own hit pipeline (hitstop, impacts, recoil) before the game's
  hit code, so the 37673+0x3C0 hook only dropped the damage. Following vinymayan/Dodge, the plugin now registers a Precision V4
  pre-hit callback at kPostPostLoad that sets bIgnoreHit while the player is invulnerable ([Compat] bUsePrecision). Header copied
  unchanged from ersh1/Precision. vinymayan/Dodge reviewed: its i-frames and "perfect dodge" are wall-clock windows from the dodge
  start (perfect = the first perfectDodgeWindow seconds), time stop is only graph events for another mod to act on.
* 1P stance and TDM mode (2026-10-21): with a whole-pose blend the vanilla 1P dodge clips (a one-handed arm pose) pulled a
  two-hander's grip forward. The 1P state now uses the 3P layout with bone masks on the vanilla 99-bone 1P skeleton: clip on
  torso/head/camera, arms = Blend(RDF_ArmBlend){clip, stance}; fArmStanceFirstPerson default 1.0. Validator 219/219.
  "Diagonal dodge in TDM 360": every dodge in the log had freeRotation=false; the STB TDM MCM ini has uDirectionalMovementDrawn
  = uDirectionalMovementSheathed = 1 (TDM: 1 = Vanilla style, 2 = 360). TDM also reports kVanillaStyle in 360 while a
  face-crosshair reason is active (attacking, blocking, casting, aiming, shouting, bFaceCrosshairWhileAttacking = 1 in STB).
  The dodge start line now names the TDM mode.
* Steering limits (2026-10-21, animator feedback): input could bend a running dodge anywhere (even reverse it), the move input
  was read against the live camera so turning the camera steered the dodge, and in keep-facing dodges (Vanilla-style TDM) TDM
  turned the actor toward the camera, allowing a 180 mid-dodge. New [Steering] keys, defaults = previous behavior:
  fMaxDeviation (path stays within +-X deg of its start heading; the desired heading is clamped, so there is no snap),
  fInputCone (input farther than X deg from the current path is ignored), bInputFromStartCamera (stick read against the camera
  yaw at dodge start unless locked on), bHoldFacing (keep-facing dodges hold the start yaw and take TDM yaw control; not with a
  locked target or in first person). 5 new tests, 59/59.
* Direction commit window and orientation warp (2026-10-21): the dodge path was always continuous, only the clip is chosen in
  45-degree steps, and keep-facing dodges (lock-on, camera-facing) ran a clip up to 22.5 deg off the path. Souls-likes cover
  the gap by turning the body (lock-on rolls left/right are their own clips there, per a stream and the game files a friend
  looked at). fDirectionCommitTime (0.06): in the first seconds the live input re-aims the dodge outright. fOrientationWarpMax
  (22.5): keep-facing dodges turn the body by the rest of the angle so the clip runs along the path, eased back over the
  recovery or over the exit / action blend when cut short; the dodge then owns the yaw (TDM yaw control). Not in first person.
  A 4-direction set would need 45 (later per set, via the clip descriptor). 5 new tests, 64/64.
* Gamepad / 1P / TDM follow-ups (2026-10-21): (1) diagonal chains after a forward dodge lost the arm blend: TDM reported 360
  mid-dodge (its _bHasMovementInput comes from the controls-driven movement handler, idle while the dodge drives the motion),
  so the chain picked the forward roll (fArmStanceRoll 0). The TDM mode is now sampled only while no dodge runs and kept for a
  chain. (2) 1P forward played the vanilla sneak roll, whose camera drop left the stance arms out of view; forward now keeps
  MT_Idle on the body. (3) A held / warped facing is now handed back to the camera yaw as the dodge eases out (recovery or the
  exit/action blend), so TDM's face-crosshair-on-attack no longer snaps the actor at the first attack. Gamepad: iDodgeGamepad
  0x80 (RS click) is TDM's target lock / camera reset, which explains the slide. 65/65 tests, 219/219 checks.
* FLICK settings menu (2026-10-21): FLICK (FUCK.dll 1.7.2, API v7; Fuzzlesz/FUCK_API, template Fuzzlesz/FUCK-Template)
  exports only RequestFUCK; FUCK::Connect at kDataLoaded, six ITool pages in the group "Reactive Dodge" of its F7 menu.
  The menu edits a copy on the render thread and commits each change on the main thread (SKSE task -> Settings::Commit,
  generation counter); the controller hands the new core Config to ReactiveCore::Reconfigure between dodges. One schema
  table (SettingsSchema.cpp) drives the widgets and the ini writer; rdf::ini::Update (core, 7 tests) rewrites keys in place.
  Translations EN/RU are UTF-16LE "$KEY<TAB>text" (FLICK's parser splits at the first whitespace). 72/72 tests.
* Nemesis patch and FOMOD (2026-10-21): Nemesis format = per behavior folder (0_master, defaultmale, defaultfemale,
  _1stperson/0_master), edited nodes as their full original text with <!-- MOD_CODE ~rdfm~ OPEN --> ... <!-- CLOSE --> blocks,
  new nodes as #rdfm$N.txt; $eventID/$variableID are shared syntax. Nemesis' original text is not an hkxcmd dump: no
  SERIALIZE_IGNORED comments, one pointer per line in vanilla pointer arrays, one vector component per line (quadVariableValues);
  the generator builds it from the vanilla dumps with those rules and requires an installed Nemesis mod (tudm, flinch, jpbo,
  zcbe, aaaaa, abeef, itmuse...) to agree, numelements aside (mods update counts, Nemesis recounts). Mod code rdfm: every
  installed Nemesis mod uses 4+ characters. check_rdf_nemesis.py: 33/33 (applied offline, serde-hkx round trip, hkxcheck 0
  new errors in 3P and 1P). Pandora never added our clips to animationdata either, so neither patch does. FOMOD validated
  against ModConfig 5.0 (GandaG/fomod-schema, one stray space in its xsd fixed locally).
* Every projectile type and explosions (2026-10-22): IDA showed AddImpact (vfunc 189) as the per-target hit record for
  all types (each overrides it but Barrier) and HandleHits (190) as the contact handler that also applies an arrow's
  damage itself, so: AddImpact hooked on all seven vtables, HandleHits only for arrows / missiles / grenades. Single
  projectiles that passed through during i-frames are remembered (pointer + FormID, 30 s) and ignored afterwards, which
  closes the late-hit case; streams are not remembered. Explosions: the HasLineOfSight call inside
  Explosion::ApplyToTargets (42677+0x38C) answers "blocked" for an invulnerable player. IDA db annotated (13 names).
  Side note: the plugin's VTABLE_*[0] are the primary vtables; the last VariantID of CommonLib's arrays is the +0x38
  subobject's vtable (a lookup mistake of mine while checking, not a plugin bug).
