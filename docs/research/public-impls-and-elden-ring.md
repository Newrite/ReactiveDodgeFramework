# Public dodge/movement implementations (Skyrim SE) and Elden Ring principles

Scope: research for ReactiveDodgeFramework (SE 1.5.97). Source clones (read only, not built) are in
`S:\AI Agent Skyrim\ReactiveDodgeFramework\_ext\` (TrueDirectionalMovement, Precision, DodgeFramework,
TK_Dodge_RE, TKDodgeAddon, AnimationMotionRevolution, BehaviorDataInjector, EldenRingHKS).
Convention: every `RELOCATION_ID(a, b)` below is quoted as **SE id** (a); AE id (b) is omitted. Address in
comment = SE 1.5.97 RVA as printed by the mod authors. IDs come from the mods' source, NOT re-verified in IDA
by me. "Offset" = byte offset inside that function for a `write_call<5>`.

Labels: [V] verified from public source with citation, [I] inference, [R] recommendation.

---------------------------------------------------------------------------------------------------

## PART 1 - Skyrim

### 1.1 Who does what (one table)

| Mod | Repo | Moves the character how | Direction | I-frames | Cancel |
|---|---|---|---|---|---|
| ersh1 Dodge Framework (DF, MIT, base of DMCO) | github.com/ersh1/DodgeFramework | does NOT move it. Only sends graph events | 8-way snap of `playerControls->data.prevMoveVec` at press time | none in DLL (behavior/anim) | none in DLL |
| TK Dodge RE / Addon | github.com/max-su-2019/TK_Dodge_RE, github.com/tavvfiq/TKDodgeAddon | animation root motion (vanilla path) | 8-way (event name suffix) at press time | graph var `TKDR_IframeDuration` set by DLL, consumed by behavior [V TKRE.cpp:109] | `bIsDodging` graph bool gates re-dodge, optional attack cancel |
| True Directional Movement (TDM) | github.com/ersh1/TrueDirectionalMovement | rotation only (SetHeading + AI rotation speed); locomotion stays vanilla | camera-relative, continuous | n/a | n/a |
| Precision | github.com/ersh1/Precision | collision only (but hooks the movement job: hitstop) | n/a | skips contact when `IsGhost()` [V ContactListener.cpp:386] | n/a |
| Animation Motion Revolution (AMR) | github.com/alexsylex/AnimationMotionRevolution | replaces the root-motion source (per-clip annotations) | n/a | n/a | n/a |
| Behavior Data Injector (BDI) | github.com/max-su-2019/BehaviorDataInjector | adds graph variables/events without patching the behavior | n/a | n/a | n/a |

### 1.2 Dodge Framework (ersh1) [V, src/Events.cpp, src/Hooks.cpp, 593 LOC total]
- Input: BSInputDeviceManager event sink for the bound key; plus a vtable hook on `SprintHandler::ProcessButton`
  (`Hooks.cpp:21`) to get tap-sprint = dodge: if sprint release with `HeldDuration() < fSprintHoldDuration` call
  `Events::Dodge()`; otherwise zero `heldDownSecs` so sprint starts normally. Trick to reuse: `bStoppingSprint` flag
  so a tap that was meant to END a sprint does not dodge.
- `Dodge()` (Events.cpp:70-): guards = `UI::GameIsPaused`, `ControlMap::IsMovementControlsEnabled/IsLookingControlsEnabled`,
  Dialogue Menu, `GetSitSleepState != kNormal`, stamina <= 0.
- Direction: `Vec2Normalize(playerControls->data.prevMoveVec)`, angle vs (0,1), bucketed into 8 sectors with PI/8 edges
  -> `SetGraphVariableFloat("Dodge_Angle")`, `SetGraphVariableInt("Dodge_Direction")`, then
  `NotifyAnimationGraph("Dodge_F|RF|R|RB|B|LB|L|LF|N")` followed by `NotifyAnimationGraph("Dodge")`.
  Neutral stick sends `Dodge_Angle = PI` + `Dodge_N`.
  => **Direction is fixed at the instant of the key press and snapped to 8 sectors; `prevMoveVec` (last non-zero move
  vector, in character-local space) is used, not camera space.** [V]
- Everything else (i-frames, stamina, root motion, cancels) is in the DMCO behavior/animation set, not DLL.
- Public contract TDM reads from DF/DMCO graphs: bool vars `DF_IsDodging`, `DF_UnlockRotation`, `DF_UnlockRotationFull`,
  event `DF_DodgeStart` [V TDM DirectionalMovementHandler.cpp:19-21, 188, 631-633]. These are the de-facto
  "dodge state" protocol: reuse the same names to get TDM rotation cooperation for free.

### 1.3 TK Dodge RE [V TK_Dodge_RE/src/TKRE.cpp, AnimationEvents.cpp]
- `canDodge` (TKRE.cpp:84): graph bool `bIsDodging` must be false (it is set true by the behavior during a dodge),
  attack state none OR `enableDodgeAttackCancel`, not sprinting (if sprint-key dodge), controls enabled, not sit/sleep/
  knocked/flying/swimming/jumping (`bInJumpState`), not killmove, stamina > 0.
- `dodge()` (:99): optional `iStep=2`, `SetGraphVariableFloat("TKDR_IframeDuration", 0.3)` (default `iFrameDuration=0.3` s,
  TKRE.h:38), then `NotifyAnimationGraph(event)`. I-frame is therefore authored in the behavior using that variable
  (a timer node / Ghost-on-timer; the actual ghost mechanism is inside the behavior files, not visible in the DLL) [I].
- Stamina: `AnimationEvents.cpp:23` sinks `BSAnimationGraphEvent` by hooking the vfunc table of the graph's event sink
  (`fnHash` of vtable ptr -> original ProcessEvent) and on tag `TKDR_DodgeStart` calls `RestoreActorValue(kDamage, kStamina, -cost)`.
  => cost is charged on the anim event, not on input, so a rejected dodge costs nothing. Good pattern.
- Input = same SprintHandler tap trick as DF.

### 1.4 True Directional Movement [V, src/Hooks.h / Hooks.cpp / DirectionalMovementHandler.cpp]
Hook inventory (SE id, offset where stated):

| Hook | Where | What it does |
|---|---|---|
| `MovementHandler` vtable idx 2 `ProcessThumbstick`, idx 4 `ProcessButton` (+2 shift only on SE 1.7.99+, not 1.5.97) | `VTABLE_MovementHandler[0]` | Calls original, then saves/restores `PlayerControlsData::moveInputVec/prevMoveVec/autoMove` and re-derives them (Hooks.cpp:126-). Lets other plugins hook the same vfunc first. |
| `PlayerControls` vtable idx 1 `Handle` | `VTABLE_PlayerControls[0]` | Resets per-frame state, remembers the `InputEvent*` chain (to replay look input during target lock). |
| `GamepadHook` | id 67497 (0xC1AB40) +0x13F call | gamepad ProcessInput interception. |
| `PlayerCharacter` vtable idx 0x7D `UpdateAnimation`, 0xAD `Update`, 0x122 `ProcessTracking`; `Character` 0xAD `Update` | | per-frame logic and headtracking. |
| `PlayerCharacter_ActorState` vtable idx 4 `GetAngle` | | subtracts `GetYawDelta()` from returned angle.z (tween-menu yaw compensation) |
| `UpdateSprintState` call | id 39375 (0x69E580) + 0xDAE | override sprint decision using the *true* input direction |
| PlayerControls checks | id 41288 (0x706AF0) +0xD8, +0x99, +0xB9 | sync-anim / sprint checks (3 call sites) |
| `AIProcess::SetRotationSpeedZ` | id 36365 (0x5D87F0) +0x356, +0x5E4; id 41293 (0x707210) +0x49 (branch) | replaces the AI-process rotation speed so the *graph* gets a matching turn speed |
| `Actor::SetRotationZ/X` call sites | id 32042 (0x4EC300) +0x4DC (X), id 36365 +0x9C7 (Z), id 38613 (0x664C80) +0x59A (Z) | keeps ThirdPersonState.freeRotation in sync when anything else rotates the actor |
| Headtracking | ids 36984 +0x592, 36376 +0xA3, 36220 +0x45C/+0x5BF, 36540 +0x17E | `SetHeadtrackTarget` call sites |
| Main update | id 35565 (0x5B2FF0) +0x748 (a nullsub call) | TDM's per-frame `Update()` (Precision uses the same spot) |
| Misc | 43030 projectile launch, 42496/49960/43009 horse aim, 49852/49883 menu camera state | not relevant |

How it gets input direction: `ProcessThumbstick`/`ProcessButton` build `inputDirection` (stick xValue,yValue, or WASD
`_pressedDirections`), then `cameraRelativeInputDirection = Vec2Rotate(input, -currentCameraRotOffset)` and
`worldRelativeInputDirection = Vec2Rotate(camRel, currentCharacterRot)` (DirectionalMovementHandler.cpp:1018-1030).
It then sets `moveInputVec` so vanilla locomotion believes the character is always pressing "forward".

How it rotates the character: computes `angleDelta` toward desired yaw, clamps to `rotationSpeedMult * dt`, calls
`SetDesiredAIProcessRotationSpeed(angleDelta/dt)` and `playerCharacter->SetHeading(data.angle.z + angleDelta)`
(`Actor::SetHeading` = id 36248 per CommonLibSSE-NG Actor.cpp:1312), and counter-rotates `ThirdPersonState::freeRotation.x`
so the camera does not swing (cpp:1262-1271). Skips when `IsPlayerAIDriven()` (aiControlledPackage or
`movementController->controlsDriven == false`), `IsPlayerAnimationDriven()` (`Actor::IsAnimationDriven`), plugin owns yaw.
Rotation speed has a dodge-specific multiplier `fDodgeUnlockedRotationSpeedMult` when `DF_IsDodging && DF_UnlockRotation && !DF_UnlockRotationFull` (cpp:1237).
When locked on during a dodge it stops rotating (`HasDodgeRotationLock`, h:249, cpp:1033-1040).
`OnDodge()` (cpp:721): when the DF API is present, resets the face-crosshair timer and calls `UpdateRotation(true)` to turn
instantly toward input before the dodge animation starts (responsiveness).
Graph variables it writes: `TDM_VelocityX/Y`, `TDM_Pitch/Roll`, `TDM_SpineTurn`, `TDM_TargetLock`, `TDM_HorseAimTurn_*`;
it reads `TDM_Dodge` (legacy), `TDM_LockRotation`; sends `TDM_Turn_180`, `TDM_AttackStart/Mid/End/Stop` handlers.

Lock-on heading: `SetDesiredAngleToTarget(player, targetHandle)` (cpp:1075) every frame, also per-attack magnetism
`SetDesiredAngleToMagnetismTarget` (cpp:2122) gated by `TDM_AttackStart/Mid/End` anim events.

Public API `TrueDirectionalMovementAPI.h` (src/, interface V1..V5, `RequestPluginAPI(InterfaceVersion)`), returns
`APIResult {OK, NotOwner, MustKeep, AlreadyGiven, AlreadyTaken, BadThread}` [V]:
- V1: `GetTDMThreadId`, `GetDirectionalMovementState`, `GetTargetLockState`, `GetCurrentTarget() -> ActorHandle`,
  `RequestDisableDirectionalMovement/ReleaseDisableDirectionalMovement/GetDisableDirectionalMovementOwner`,
  `RequestDisableHeadtracking/Release.../GetDisableHeadtrackingOwner`.
- V2: **`RequestYawControl(handle, yawRotationSpeedMultiplier /*0 = instant*/)`, `SetPlayerYaw(handle, desiredYaw)`, `ReleaseYawControl(handle)`** (ownership model: first requester wins, others get AlreadyTaken).
- V3: `GetDirectionalMovementMode()` (kDisabled/kVanillaStyle/kDirectional/kTargetLock), `GetActualMovementInput()` (raw pre-TDM stick vector).
- V4: `IsTargetLockBehindTarget`. V5: `RequestDisableTargetLock/Release/GetOwner`.
Must be called from TDM's thread (`GetTDMThreadId`, BadThread otherwise).
=> For us: `GetActualMovementInput` + `GetDirectionalMovementMode` + `GetCurrentTarget` give camera/lock-aware desired dodge
direction; `RequestYawControl/SetPlayerYaw` lets us rotate the actor to the dodge heading without fighting TDM. [R] Make all of this optional (soft dependency, `RequestPluginAPI` null = fallback).

### 1.5 Precision [V, src/Hooks.h/cpp, ContactListener.cpp]
- `UpdateMovementJob`-internal function id 36359 (0x5D8170): `write_call<5>` at +0xF0 replaces the call to the per-actor
  `ApplyMovement(Actor*, float dt)` (orig target 0x5D8260). Precision multiplies `dt` (hitstop). [V Hooks.cpp:46-53]
  This is the cleanest *public* example of a choke point where per-actor movement is applied with a dt we can scale or
  intercept. [I] Our driven-displacement option can run in the same call.
- `Character`/`PlayerCharacter` vtable idx 0x79 `ModifyAnimationUpdateData(BSAnimationUpdateData&)` (animation update dt/ data).
- `RunOneActorAnimationUpdateJob` id 40436 (0x6E1990); main update 35565 +0x748; Job_Ragdoll_post_physics 38112.
- Hit pipeline: `Actor::Hit`-chain ids 37650, `HitActor` 37673, `CalculateCurrentHitTargetForWeaponSwing` 37674,
  TESObjectREFR::Hit callee 36682, `PopulateHitData` 42832, death force 37675.
- Collision filter hooks: 76181, 76676-76680, 77228 (hkpCollidableCollidableFilter::isCollisionEnabled etc.).
- I-frames: contact listener disables the contact point if `targetActor->IsGhost()` (Actor::IsGhost id 36286);
  also "jump i-frames" when `charController->context.currentState` is `kJumping`/`kInAir` (PrecisionHandler.cpp:1314) -
  i.e. i-frames are a *state-derived predicate*, no graph variable. No `bIsDodging` check exists in Precision itself.
- Precision API (V1..V4): `AddPreHitCallback`, `AddPostHitCallback`, `AddPrePhysicsStepCallback`,
  `AddCollisionFilterComparisonCallback`, `AddContactListenerCallback`, `IsActorActive`, `ToggleDisableActor`, ... (PrecisionAPI.h:146-365).
  [R] Pre-hit callback is the optional integration point for "this swing is dodged" (return value can discard the hit)
  and for perfect-dodge detection (see 1.8).

### 1.6 Root motion: where the engine applies animation motion [V AMR include/Hooks.h]
AMR is the only public source I found that hooks the true root-motion path. All ids SE (first value). Source repo notes:
a comment says no register/offset change between SE and AE for the hkb hooks.

| Item | SE id (addr) | Hook | Meaning |
|---|---|---|---|
| `Character::ProcessMotionData` | 31949 | call sites at +0x28D, +0x2A1 (translation) and +0x355, +0x368 (rotation); thunk passes `r13=Character*`, `rbx=BSFixedString*` (clip name) | The function that reads per-clip `MotionDataContainer` and produces this frame's translation delta + rotation quaternion. This is the **root motion producer** |
| `MotionDataContainer::ProcessTranslationData` | 31812 | original called as fallback | sample translation spline at `motionTime` (`NiPoint3&`) |
| `MotionDataContainer::ProcessRotationData` | 31813 | fallback | sample rotation (`NiQuaternion&`) |
| `InterpolateRotation` | 69459 | | quaternion slerp helper |
| `hkbClipGenerator::Activate` | 58602 | +0x66E: replace start-time computation (`mov rdx,r15`; r15=`hkbContext*`) | knows the clip being entered -> registers annotations |
| `hkbClipGenerator::Deactivate` | 58604 | +0x1A (`rdx=r14` = hkbContext) | clip left |
| `bhkCharacterController::UpdateStateAndMovement` | 76436 (AMR passes 32703 as AE id - looks like a copy-paste from ProcessMotionData; do not trust that AE id) | +0x7D1: replace `hkpCharacterContext::getState` call | forces the "swimming" state when the motion has no Z, so vertical root motion is not eaten by ground snapping; flag in `bhkCharacterStateOnGround::unk10` |
| `bhkCharacterStateOnGround` vtable idx 8 `SimulateStatePhysics` | `VTABLE_bhkCharacterStateOnGround[0]` | full replacement (AMR reimplements the ground state step) | shows how `velocityMod`, `forwardVec`, `outVelocity`, `supportNorm`, `surfaceInfo.surfaceVelocity` are combined through `hkpCharacterMovementUtil::calculateMovement` (SE id 78988, desired velocity = `velocityMod` with **x/y swapped**, gain 1.0, maxVelocityDelta 500) and gravity added. (Hooks.cpp:359-455) |
| `bhkWorld::PickObject`-like | 76005 | | ground raycast |

AMR's mechanism: motion is authored as **annotations** on the clip: text `animmotion <x> <y> <z>` and `animrotation <yawDeg>`
at a given time (Motion.h), parsed in `hkbClipGenerator::Activate`; `ProcessTranslationData_Hook` interpolates piecewise-linear
between those keyframes (cumulative from clip start), falling back to the vanilla spline when no annotation exists.
=> The vanilla pipeline is: clip's embedded motion track -> `Character::ProcessMotionData` (id 31949) -> character controller
(`bhkCharacterController::velocityMod`/`outVelocity`) -> `hkpCharacterContext` state step. [V for chain endpoints, I for exact velocityMod handoff].

CommonLibSSE-NG relevant layouts (local `S:\Programming\CommonLibSSE-NG\include\RE\B\bhkCharacterController.h`) [V]:
`forwardVec 0x70`, `outVelocity 0x90`, `initialVelocity 0xA0`, `velocityMod 0xB0`, `direction 0xC0`, `rotCenter 0xD0`, `pushDelta 0xE0`,
`up 0x100`, `supportNorm 0x110`, `deltaPos 0x180`, `flags 0x218` (CHARACTER_FLAGS: `kAnimAngleMod` bit6, `kSupport` bit8, `kJumping` bit13, `kNoSim` bit17),
`velocityTime 0x220`, `rotMod 0x224`, `rotModTime 0x228`, `fallTime 0x244`, `gravity 0x248`, `speedPct 0x264`.
Virtuals: `GetLinearVelocityImpl` idx 6, `SetLinearVelocityImpl` idx 7, `SetPositionImpl` idx 3, `RotateImpl` idx 0x13.
`MovementControllerNPC::controlsDriven` @ 0x1C5 (TDM uses it for "AI driven"). `Actor::IsGhost` id 36286, `Actor::SetHeading` id 36248.
[I] `velocityTime`/`rotMod`/`rotModTime` look like the engine's own "apply this velocity/rotation for N seconds" fields - candidate for a
non-animation-driven dash; untested.

### 1.7 BDI [V]
`BehaviorDataInjector/src/Hook.h:15`: id 62640 (`CreateSymbolIdMap`) +0x22A write_call; injects extra variable/event names into the
behavior symbol map from JSON so a DLL can `SetGraphVariable*`/`NotifyAnimationGraph` names that the stock behavior lacks.
Useful because graph-variable/event names for a *new* dodge could be registered without a Nemesis/behavior patch, but the graph
still needs states that *use* them (a fully behavior-less dodge would need either a graph state we repurpose or a driven mover - see 1.8).

Not examined (no public source found): "Payload Interpreter", "SCAR", "Animated Interactions", "SmoothCam" (SmoothCam source
exists publicly but is camera-only). DMCO/Dodge MCO itself is an animation+behavior pack (DLL = ersh1 DodgeFramework). TK Dodge RE repo has no root-motion hook.

### 1.8 Problems observed per mod and reusable pieces

Problems (the three the brief names):
1. **Direction fixed at start** - DF/TK Dodge choose 8-way event at key press from `prevMoveVec` (local space); the clip then has the
   direction baked into its root motion. Stick changes during startup are ignored; analog direction is lost; camera/lock-on is ignored
   (TDM compensates by rotating the actor toward the input *before* the event, `OnDodge()`).
2. **Root motion owned by the animation** - displacement = clip data via `Character::ProcessMotionData`; only AMR lets you change it, via
   annotations on the clip (static per clip, not per dodge). Distance/speed cannot adapt to the situation (obstacle, slope, reactive trigger).
3. **No real cancel** - cancel is whatever the behavior's transitions allow; DLL only exposes `bIsDodging` as a gate. No input buffer; attack-from-dodge is
   behavior-authored (`enableDodgeAttackCancel` only lifts the DLL gate).
4. I-frames are authored in the graph timeline (TK: `TKDR_IframeDuration`) or derived from state (Precision jump i-frames), not from a shared, queryable API.

Reusable as optional integrations [R]:
- TDM API V3/V2: raw input, mode, locked target, yaw ownership (`RequestYawControl`). Also read `DF_IsDodging/DF_UnlockRotation[Full]` graph names for compatibility, and set them in our own graph/own state so TDM freezes rotation correctly.
- Precision API V1+: `AddPreHitCallback` (discard hits during i-frames, detect perfect-dodge window), `ToggleDisableActor`; and respect that Precision already treats `Actor::IsGhost()` as untouchable - so Ghost is the one i-frame predicate recognized by the ecosystem (contacts disabled, not just damage). [I] Setting ghost via a flag vs. `SetGhost` papyrus: not verified in these sources.
- Hook-point reuse: id 35565 +0x748 (frame update; shared with TDM & Precision - use the same call-site pattern, chain via `write_call` return original), id 36359 +0xF0 (per-actor ApplyMovement with dt), `MovementHandler`/`SprintHandler` vtable hooks (call original first so DF/TDM still work).
- DF-compatible input: tap-sprint with `bStoppingSprint` guard.

---------------------------------------------------------------------------------------------------

## PART 2 - Elden Ring engineering principles

### 2.0 What exists locally (no tooling installed)
`F:\SteamLibrary\steamapps\common\ELDEN RING\Game`: `regulation.bin`, `Data0..3.bhd/.bdt` (12G/13G/20G/2.7G), `DLC.bhd/.bdt`, `sd\sd.*`,
`eldenring.exe`, `start_protected_game.exe`, `EasyAntiCheat`, `oo2core_6_win64.dll`, `movie\`. No loose `action\script\c0000.hks`.
To read the player action script (`action/script/c0000.hks`, compiled Lua in `scriptbnd`) and animation TAE (`chr/c0000.anibnd.dcx`,
`c0000_*.anibnd`) you would need: an unpacker that carries the BHD5 RSA keys (UXM Selective Unpacker / Smithbox / Yabber-derived tools),
then WitchyBND to unpack `.anibnd.dcx`/`.luabnd.dcx` (needs `oo2core_6_win64.dll`, already present), then DSAnimStudio (TAE viewer/editor) and a
Lua decompiler/HKS tool (community "ividyon/EldenRingHKS" is already a decompile). Not done here (instruction: no decryption tooling).
Do not copy proprietary assets; only principles are used below.

### 2.1 Verified from public sources [V]

Source A: public decompile of c0000.hks, github.com/ividyon/EldenRingHKS (c0000.hks, 26,667 lines; clone in `_ext/EldenRingHKS`).
Line refs are to that file. I read it as analysis; no code is copied into our project.
- **Input is polled each tick by the *current* state, in a fixed priority list.** `EvasionCommonFunction` (l.6843-6885), called from `Rolling_onUpdate` (l.8172) and the backstep/other states, tries in order:
  passive actions (damage), jump, hand change, guard-on, weapon change, **ExecEvasion (roll again)**, item, magic, art stance, **ExecAttack (attack from roll, with dedicated variants like `W_AttackRightLightStep`)**,
  **MoveStartonCancelTiming** (resume locomotion), then `IsAnimEnd -> W_Idle` (l.8184-8190).
  => cancel windows are not graph transitions; they are state-local "can I start X now" predicates evaluated every frame.
- **Request/buffer object**: `env(ActionRequest, ACTION_ARM_ROLLING)` is an engine-side latched request (set on button event, consumed by `ResetRequest()`); the script calls `ResetRequest()` on failure paths such as zero stamina (l.4492-4600) so a stale press does not fire later. The vendor's combat guide says a pressed action is queued for a short time and performed at the next available chance (bandainamcoent.com/news/elden-ring-introduction-part-4-combat-guide). The exact queue length is engine-side and not in the script.
- **Cancel gating by event flags / SpEffects**: roll chaining after hits is gated by `env(GetEventEzStateFlag, 2..5)` chosen by `DamageCount` (l.4519-4545) and attack/move cancels by `env(IsMoveCancelPossible)` (l.651-662), `env(IsSpecialTransitionPossible)` etc. These flags are driven by time-based animation events (TAE) - consistent with "TAE event type 0 JumpTable" semantics, but I could not retrieve a public page documenting jump-table IDs (soulsmodding wiki URLs returned 404/empty), so the exact IDs are [I].
- **Roll direction**: engine supplies `env(GetRollAngle)` (an analog angle; script multiplies by 0.01, l.18368) relative to camera. Free camera: roll anim is always the forward clip (`RollingDirectionIndex 0`) and the character is turned to the stick angle (`act(FaceDirection, angle)`) *at roll start*. Lock-on: angle is bucketed by four thresholds into forward/right/left/back clips (`RollingDirectionIndex 0..3`, `RollingAngleThreshold*Test` vars) and the body is turned with `TurnToLockonTargetImmediately(turn_target_angle)` (l.4600-4655).
- **Rotation during roll**: `Rolling_onUpdate` calls `act(DisallowAdditiveTurning, TRUE)` and `SetRollingTurnCondition` (l.295-312): during the roll turn speed is forced to 0 for large `TurnAngleReal`/set angle, and 360 deg/s for 90-180 deg lock-on cases. => rotation is mostly applied *before/at the start* and then locked; limited residual turning only in lock-on.
- **Chaining**: roll-from-roll uses a separate event `W_Rolling_Selftrans` with its own variables (`RollingDirectionIndex_SelfTrans`, `RollingAngleRealSelftrans`), triggered when node `Rolling_CMSG` is already active (l.4531-4545). Stamina is charged once at request time: `AddStamina(STAMINA_REDUCE_ROLLING)` unless SpEffect 102360 is active (l.4566).
- **Gating**: `GetEvasionRequest` rejects if stamina < `STAMINA_MINIMUM`; `GetFallHeight > 150` rejects; overweight is a separate weight index (`EVASION_WEIGHT_INDEX_LIGHT/MEDIUM/HEAVY/OVERWEIGHT/SUPERLIGHT`, l.184-217) selecting different clips (roll i-frame/recovery lengths come from the clip, not script).
- **Dodge on release, not press**: Elden Ring's roll fires on button release because the same button is sprint (nexusmods.com/eldenring/mods/101 description; player inference, not developer-documented).

Source B: eldenring.wiki.gg "Invincibility Frames" (https://eldenring.wiki.gg/wiki/Invincibility_Frames), baseline **30 fps frames** (page states rule of thumb "x2 for 60 fps"):
- Roll i-frames: **14** light/medium, **13** heavy (17 / 16 with Crucible Feather / Windy Crystal Tear, SOTE). Backstep: 7 (most 1H/paired light) or 8 with the talisman. Quickstep 15, Bloodhound's Step 16, Blind Spot 10. Jump 25 (torso/legs).
- Conflict: another secondary source (models.yclas.com / guides) gives 13 i-frames + 8 recovery (light/medium) and 12 i-frames + 16 recovery (heavy) at 30 fps, i.e. 400-433 ms of invulnerability and 267 / 533 ms recovery; the wiki also says light and medium rolls have the same effectiveness and recovery, heavy has almost twice the recovery (eldenring.wiki.gg/wiki/Equipment_load). Treat 12-14 frames @30 = ~0.40-0.47 s as the invulnerability window; recovery frame counts are weakly sourced.
- Equip-load classes: light <30%, medium <70%, heavy up to 100%, no roll above 100%.

### 2.2 Inferences [I]
- Roll root motion is authored in the animation (c0000 TAE + anim motion track), with direction decided by *selecting which of 4 authored clips to play* (lock-on) or by turning the body first (free). The engine applies it the same way as ordinary character motion; no velocity override script is visible in HKS. (Absence of any `SetVelocity`-style call in the evasion functions: consistent, not exhaustively proven.)
- Recovery cancel works because the *recovery tail* of the clip has a TAE jump-table region that sets the `IsMoveCancelPossible`/attack-cancel flags earlier than clip end; the earliest frame where attack/roll/move become legal differs per action (attack-from-roll becomes available sooner than a new roll).
- The input latch keeps an unconsumed press alive for a short fixed time; only the "ActionRequest" flag, not a timestamp, is visible in script, so queue length cannot be read from HKS. Community feel suggests ~0.1-0.2 s [I, weak].
- Snap vs analog: the engine gives an analog roll angle; only the lock-on case quantizes it (to 4 clips), the free case keeps the analog angle by turning.

### 2.3 Recommended principles for our Skyrim system [R]
Numbers below assume 60 Hz-equivalent and wall time; convert to seconds so they work at variable fps.

1. **Direction chosen late and analog.** Sample intent (TDM `GetActualMovementInput` when present, else `moveInputVec`/`prevMoveVec`, converted to world heading via camera yaw) at the moment the dodge *starts executing* (after buffer), not at key down. Keep a continuous heading (float), never an 8-way event. Lock-on: express as target-relative angle; free: world heading.
2. **We own the displacement, not the clip.** Drive the mover ourselves (velocity into the character controller or a per-frame positional step at the ApplyMovement choke point, 1.5/1.6) following a distance/speed curve (ease-out, ~0.55-0.65 s total for a roll-sized dodge, ~5-7 m at default; tune), and let the clip play with root motion suppressed/ignored (AMR-style `ProcessMotionData` override returning zero for our tagged clips, or a clip authored in-place). Obstacle/ledge/slope checks become possible.
3. **Rotation policy like ER**: turn body to dodge heading instantly/at max rate in the first 1-3 frames (also compatible with TDM `RequestYawControl(handle, 0)`), then lock yaw for the move; allow residual turn only under lock-on (<= ~360 deg/s) so a locked target stays in front on a back-step.
4. **Window structure** (timeline in seconds from start; ER light roll analog ~0.43 s i-frame at 30fps frames=13-14):
   - Startup/anticipation (stamina committed here): 0.05-0.08 s, vulnerable (can be 0 for "reactive" dodges).
   - I-frame window: 0.30-0.45 s (default 0.40), starting ~0.03-0.05 s after start, ending before the recovery; ghost/hit-discard on via Precision-compatible `Actor::IsGhost()` or hit pre-callback, always cleared on interrupt/death/load.
   - Free movement/steering window: from end of startup to 80% of travel for speed shaping only.
   - Recovery: 0.20-0.27 s (light), 0.40-0.55 s (heavy gear), with the cancel flags opening inside it.
5. **Cancel structure = per-action "cancel-from-dodge" flags polled each tick** (ER `EvasionCommonFunction` order): defined as time points in the dodge timeline: `attackCancelAt` (earliest, ~50-60% of the recovery elapsed), `dodgeChainAt` (a bit later than i-frame end, ~70-75% of total), `moveCancelAt` (resume sprint/run, earliest after travel finishes, ~85-90% of total). Evaluate in fixed priority: damage-interrupt > jump > block/guard > dodge-chain > item/spell > attack > move > idle.
6. **Input buffer**: latch a press for **0.15-0.20 s** (default 0.18) and consume it at the first tick where a cancel flag for that action is true; clear the latch on failure paths that consume the intent (insufficient stamina) and on state exit; a newer press replaces an older one of a different action (last-wins). Buffer applies to dodge, attack, and movement resume; ER's buffer is engine-side but the vendor describes it as short.
7. **Chaining**: allow dodge-from-dodge as its own request type (`Selftrans` pattern) with separate direction state, stamina charged once at accept time (rejected requests cost nothing, like TK's anim-event charge or ER's `AddStamina` after gating), diminishing i-frames on rapid chains (e.g. -25% i-frame length for a chain started within 0.1 s of the previous end) to prevent invulnerability lock.
8. **Stamina**: charge on accept, refuse below minimum (ER `STAMINA_MINIMUM`), no regen delay mechanics here but pause regen during dodge + recovery.
9. **Recovery cancel into attack/run**: attack variants "from dodge" (lunge) become legal at `attackCancelAt`; running/sprint resumes at `moveCancelAt` by re-enabling normal locomotion input (no hard stop), preferably with a short velocity blend (velocity ramps from the dodge exit speed to run speed over ~0.1 s) instead of the vanilla stop-start.
10. **Compatibility names**: set graph bools `DF_IsDodging`, `DF_UnlockRotation`, `DF_UnlockRotationFull` and emit `DF_DodgeStart` (0.1.0 TDM expectations) so TDM behaves correctly; expose own state for Precision callbacks; keep `bIsDodging`/`TKDR_*` untouched so TK/DMCO packs can coexist or be detected and yielded to.
11. **Robustness**: every driven-displacement step goes through `bhkCharacterController` collision (no teleport), abort on `kNoSim`/ragdoll/mount/sit/sync-anim/AI-driven; on abort clear ghost, restore yaw ownership, release TDM/Precision resources.

### 2.4 Open items / uncertainties
- No public documentation of TAE JumpTable IDs retrieved (soulsmodding.com/wikidot pages not reachable via tooling); needs the local BND route in 2.0 or the Souls modding wiki in a browser.
- Exact ER input-buffer length is not extractable from HKS; confirm by in-game frame stepping if needed.
- SE ids above are taken from the mods' source; before shipping our own hooks verify each against the 1.5.97 binary (REL ID -> RVA; use the IDA MCP, then check the call-site opcode at the stated offset).
- `bhkCharacterController::velocityMod` as the actual sink of `Character::ProcessMotionData` output is [I]; confirm with a write breakpoint / xrefs on `velocityMod` in IDA.
- ER i-frame numbers conflict between sources (13 vs 14, light vs heavy); use as a tuning range, not as exact.

## Sources
- https://github.com/ersh1/TrueDirectionalMovement (src/Hooks.h, Hooks.cpp, DirectionalMovementHandler.cpp, TrueDirectionalMovementAPI.h)
- https://github.com/ersh1/Precision (src/Hooks.h, Hooks.cpp, PrecisionHandler.cpp, Havok/ContactListener.cpp, PrecisionAPI.h)
- https://github.com/ersh1/DodgeFramework (src/Events.cpp, Hooks.cpp)
- https://github.com/max-su-2019/TK_Dodge_RE and https://github.com/tavvfiq/TKDodgeAddon
- https://github.com/alexsylex/AnimationMotionRevolution (include/Hooks.h, Motion.h, source/Hooks.cpp)
- https://github.com/max-su-2019/BehaviorDataInjector (src/Hook.h)
- https://github.com/ividyon/EldenRingHKS (c0000.hks decompile)
- https://eldenring.wiki.gg/wiki/Invincibility_Frames ; https://eldenring.wiki.gg/wiki/Equipment_load
- https://www.bandainamcoent.com/news/elden-ring-introduction-part-4-combat-guide ; https://www.nexusmods.com/eldenring/mods/101
- Local: `S:\Programming\CommonLibSSE-NG\include\RE\B\bhkCharacterController.h`, `src\RE\A\Actor.cpp` (IsGhost 36286, SetHeading 36248)
