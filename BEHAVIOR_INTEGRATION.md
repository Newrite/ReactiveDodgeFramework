# Behavior integration (Pandora)

RDF adds a root-level dodge to the humanoid graph through ONE native Pandora mod, code `rdf`. Nothing is patched in the
weapon / sheathed / magic sub-graphs, so it works with every weapon type and from attacks.

## 1. What it changes (third person only)

`0_master` (Nemesis ids in brackets):
- events (`#0106/eventNames`, `#0108/eventInfos`): `RDF_DodgeStart/StartB/Chain/Exit/Entered/Left` and the clip annotation events
  `RDF_Windows/IFrameStart/IFrameEnd/ChainOpen/AttackOpen/BlockOpen/ExitOpen`
- variables (`#0106`, `#0107`, `#0108`): `RDF_DodgeClip` int 0, `RDF_DodgeSpeed` float 1.0, `RDF_IsDodging` bool, `RDF_ArmBlend` float 0.0, `RDF_ChainIndex` int 0
- `RootBehaviorGraph` (`#0340`): new states `RDF_Dodge_A` (stateId 9701) and `RDF_Dodge_B` (9702). Each is
  `hkbModifierGenerator{ BSIsActiveModifier(bAnimationDriven, RDF_IsDodging), arm-stance blender }` (section 5), enter notify
  `RDF_DodgeEntered`, exit notify `RDF_DodgeLeft`. The dodge clips are a `hkbManualSelectorGenerator(RDF_DodgeClip)` over
  16 `hkbClipGenerator` (SINGLE_PLAY, speed <- RDF_DodgeSpeed, no triggers).
- root wildcard array (`#0341`): `RDF_DodgeStart` -> 9701 and `RDF_DodgeStartB` -> 9702 (IS_LOCAL_WILDCARD,
  `RDF_DodgeInBlend` 0.12 s). The plugin alternates them so a new dodge never targets the state still blending out.
- A/B transitions: `RDF_DodgeChain` A<->B (`RDF_DodgeChainBlend` 0.10 s); `RDF_DodgeExit` and
  `IdleForceDefaultState` -> `Default_State` (14) with `RDF_DodgeOutBlend` 0.25 s. The return is written like vanilla
  `Stagger_State` (4 conditional transitions per event that restore the equip sub-state, reusing the vanilla conditions
  `#0355 #0354 #0353 #0350`); `RETURN_MODE = "simple"` in the generator gives one plain transition instead.
  `StaggerPlayer` -> `PlayerStagger_State` (48): the player's stagger event is per-state in vanilla (not a wildcard), so our
  states need their own copy.
- 291 new nodes (`#rdf$0 ..`); all other nodes are untouched.

`defaultmale` / `defaultfemale`: 16 names `Animations\RDF\rdf_{roll,step}_{f,fr,r,br,b,bl,l,fl}.hkx` appended to `animationNames` (`#0029`).

First person is patched separately (section 6). Not touched: creatures, every other behavior file, animation data (root motion is
ignored, see ANIMATION_CONTRACT.md).

## 2. Install (the manual steps are yours)

1. `python tools/gen_rdf_patch.py` (regenerates `behavior/Pandora_Engine/mod/rdf`; the committed output is current).
2. `python tools/install_fixtures.py` copies the patch to `mods\AI - Reactive Dodge Framework\Pandora_Engine\mod\rdf\` and 16
   placeholder clips (from TUDM, local only, never commit or ship) to `...\meshes\actors\character\animations\RDF\`.
   It does not touch modlist.txt or Pandora's ActiveMods.json.
3. In MO2 enable `AI - Reactive Dodge Framework`.
4. Start Pandora from MO2, tick `Reactive Dodge Framework` (a new mod code is inactive by default), Run.
5. `python tools/pandora_log.py --mod "Reactive"` must print no problem line. Re-run Pandora whenever the modlist or the patch changes.

The patch only appends/inserts and uses unique state ids. In Pandora's type-major apply order its appends land after every
other mod's, so its wildcard is last in the root wildcard list.

## 3. Validation without touching the real output

`tools/stage_pandora.py` rebuilds Pandora's inputs on disk (copy of the exe, a `Data` overlay with every enabled STB mod's
`Pandora_Engine\mod`, `Nemesis_Engine\mod` and FNIS lists in MO2 priority order plus the overwrite folder; settings from a
Settings.json beside the copied exe) and runs the real Pandora exe (`--auto_run --auto_close`) with the output in
`_work/pandora_test_out/<label>`. It prints `real output mod untouched: True` (SHA of `mods\Pandora\Engine.log`,
`ActiveMods.json`, `0_Master.hkx` before and after).

Fidelity: the no-rdf baseline reproduces the real `mods\Pandora` output byte for byte for every hkx (57 files compared; only
Engine.log, PreviousOutput paths and some id counters inside animationdatasinglefile.txt differ).

```
python tools/dump_pandora_out.py --vanilla                                   # vanilla Nemesis-id dumps (once)
python tools/gen_rdf_patch.py --verify-ids                                   # vanilla ids used by the generator
python tools/stage_pandora.py --label baseline                               # same modlist, no rdf
python tools/stage_pandora.py --label rdf --rdf behavior/Pandora_Engine/mod/rdf
python tools/validate_rdf.py                                                 # 219 checks (3P + first person), exit 0 = pass
python tools/test_hkxcheck.py
```

`validate_rdf.py` checks: no new failure line in Engine.log; only 0_Master / DefaultMale / DefaultFemale changed; events and
variables exist with the right types and initial values and no macro resolved to -1; both states, the generator chain, 16 clips,
bindings, triggers; every transition (events, targets, effect durations and flags); the vanilla stagger / ragdoll / jump / swim /
bleed-out wildcards still present; animation names in both characters; per state machine the number of states and transitions
equals the baseline (root machine +2 states / +29 transitions); no object lost except the five nodes the patch edits.
Negative control: a copy of the patch with mistyped event/variable names (Pandora logs nothing for those) fails 5 checks.

Not covered (needs the game): dodge from idle, sprint, mid-attack, mid-block, bow/magic drawn, sheathed; chain; stagger and death
during a dodge; weapon equip mid-dodge; save/load during a dodge; whether clips with root motion move the actor.

## 4. Conflicts and caveats

- TUDM uses `RollStart` etc. inside the 1hm / mt / magic behaviors; RDF uses other events and states, so they coexist (RDF does
  not disable TUDM's dodge).
- stateIds 9701 / 9702 are free in the root machine (checked against vanilla and the current output). Re-run `validate_rdf.py`
  after adding mods.
- `DF_IsDodging` (True Directional Movement) does not exist in this modlist's 0_master, so it is not created or bound
  (`gen_rdf_patch.py --with-df` binds it as bIsActive2 if it appears).
- Pandora silently turns unknown `$eventID[..]$` / `$variableID[..]$` macros into -1 and drops unresolved pointers; always
  run `validate_rdf.py` after changing the generator.
- Pre-existing, unrelated: 6 clips from "Turning Animation Won't Affect UpperBody" reference `Animations\male\1HM_*.hkx` that are not
  in the character animationNames (docs/research/behavior-pipeline.md section 5).
- No animation data is supplied: clips play, root motion is not extracted (by design).

## 5. Arm stance layer (`RDF_ArmBlend`)

While dodging, the arms (clavicle, upper arm, forearm, hand, fingers, weapon / shield / magic-node bones) can keep the pose of
the player's current weapon stance, layered over the dodge clip. The plugin controls it per dodge with ONE float.

### 5.1 Node structure (per state; A and B are identical)
```
ModifierGenerator { BSIsActiveModifier(bAnimationDriven, RDF_IsDodging),
  ArmStanceBlend  hkbBlenderGenerator flags 0                       (vanilla pattern: complementary masks, weights 1)
    child0  weight 1  worldFromModel 1  mask NOT_ARMS    generator = dodge clip state machine (startStateId <- RDF_DodgeClip, 16 states = 16 clips)
    child1  weight 1  worldFromModel 0  mask ARMS        generator = ArmLayer
  ArmLayer        hkbBlenderGenerator flags 16 (PARAMETRIC), blendParameter <- RDF_ArmBlend
    p0  weight 0  generator = a SECOND dodge clip state machine (own clip nodes, same variables)
    p1  weight 1  generator = StanceBlender
  StanceBlender   hkbBlenderGenerator flags 0
    right  boneWeights <- character property RightArm   generator = ManualSelector(iRightHandType) over 13 stance clips
    left   boneWeights <- character property LeftArm    generator = ManualSelector(iLeftHandType)  over 13 stance clips
}
```
ARMS = union of the vanilla `RightArm` and `LeftArm` character-property masks (126 bones in the current XPMSE skeleton:
R = 30-32 clavicle/upper/fore arm, 39 hand, 41 AnimObjectR, 43 Weapon, 45 pauldron, 48 R magic node, 56-59 twists, 82-96 fingers;
L = the mirror bones 27-29, 38, 40, 42 Shield, 44, 47, 52-55, 67-81, plus the XPMSE extra bones 99-125 which the real LeftArm
mask contains). NOT_ARMS = 1 - ARMS. Both are static `hkbBoneWeightArray` nodes; the two stance children reuse the vanilla masks
through the same `boneWeights <- RightArm/LeftArm` character-property binding the vanilla graphs use (1hm_behavior
`1HM_Readied_*`, `1stP_MLh_1HM_Idle_Blend`). `validate_rdf.py` re-reads the real masks from the output character file and
checks `NOT_ARMS + RightArm + LeftArm == 1` for every bone.

### 5.2 Evidence for the weight semantics (and why the structure is built this way)
- I found no Havok documentation in the workspace (serde-hkx only has field names), so the design does not rely on how
  `hkbBlenderGenerator` normalises weights; it uses only two patterns that vanilla uses extensively and that are exact under
  either reading:
  1. **Complementary masks, all weights 1** (vanilla: 151 flags-0 blenders, e.g. `H2H_1HM_Readied_Blend`, whose two bone masks
     #4520 and #4518 are 0/1 complements; `MLh_1HM_Idle_Blend` masks are 0.5/0.5 complements). Every bone gets total weight 1 from
     exactly one child, so average, normalised sum and plain sum give the same pose. Here: dodge on NOT_ARMS, ArmLayer on ARMS.
  2. **Parametric blend** (`flags 16`, children `weight` = positions on the parameter axis, `blendParameter` bound to a
     variable). Vanilla `Tor_1stP_Locomotion_Blend` (flags 16, `blendParameter <- SpeedDamped`) has children at 0 (pose),
     82.454 (walk speed) and 350.987 (run speed): at a child's position the output is that child, in between it is the lerp of
     the two neighbours. Here the positions are 0 (dodge copy) and 1 (stance), so `RDF_ArmBlend` = f gives
     `arms = (1-f) * dodge + f * stance` exactly.
- Why the dodge appears twice: a single dodge generator under a weighted-average child would give `W/(1+W)` and never reach
  100 % stance. With the second copy the lerp reaches exactly 0 and 1. Cost: one extra selector with 16 clips per state; the two
  copies start together, run at the same speed and are never shared between parents.
- `worldFromModelWeight` 1 only on the main dodge child, 0 on the ArmLayer and its children, so root motion comes from the dodge
  clip alone (the plugin owns displacement anyway).
- `BSBoneSwitchGenerator` (vanilla override node, e.g. `1HM_Readied_1stP_BoneSwitch`) was considered; its switching is by
  selector index, not by a bindable weight, so it cannot produce a continuous per-dodge weight.

### 5.3 What the plugin writes
| `RDF_ArmBlend` | meaning |
|---|---|
| `0.0` (default) | off: arms follow the dodge clip exactly (rolls; unarmed) |
| `1.0` | 100 % stance on the arms |
| `0 < f < 1` | linear mix `(1-f) * dodge + f * stance` (written directly, no `f/(1-f)` conversion) |
Values outside [0,1] are clamped by the parametric blend (position 0 / 1 are the end children). Set the variable before the
dodge starts (together with `RDF_DodgeClip`); changing it during the dodge works (the stance children activate when their
weight becomes non-zero, so a first non-zero value mid-dodge starts the stance clip from its first frame).
The stance side is selected live from `iRightHandType` / `iLeftHandType`, which the game keeps current for the player; the
plugin only has to write `RDF_ArmBlend`. For unarmed players (both hands 0) write 0: the table maps right 0 to `H2H_Idle` but
left 0 to the one-hander pose (the vanilla choice when the right hand holds a weapon), so a fist-fighter would get mismatched arms.

### 5.4 Stance clip table (all already registered in DefaultMale AND DefaultFemale animationNames; nothing is added)
Hand-type values, from how vanilla compares them (`1hm_locomotion` `Melee_Direction_Behavior` states = iRightHandType,
`1hm_behavior` `Weap_Idle_Behavior`, `magicbehavior` `MagicReady_*_MSG`): 0 none/fists, 1 sword, 2 dagger, 3 axe, 4 mace,
5 two-handed sword, 6 two-handed axe/hammer, 7 bow, 8 staff, 9 spell, 10 shield (left only), 11 torch (left only), 12 crossbow.
For two-handers, bow and crossbow the game sets both hand variables to the same type.

| value | right arm clip (iRightHandType) | left arm clip (iLeftHandType) |
|---|---|---|
| 0 | `H2H_Idle` | `1HM_Idle` |
| 1-4 | `1HM_Idle`; nested selector on iLeftHandType: left 1-4 -> `DW1HM1HMIdle` (dual wield), else `1HM_Idle` | `DW1HM1HMIdle` |
| 5 | `2HM_Idle` | `2HM_Idle` |
| 6 | `2HW_Idle` | `2HW_Idle` |
| 7 | `Bow_IdleHeld` | `Bow_IdleHeld` |
| 8 | `StaffRight_Idle` | `Staff_Idle` |
| 9 | `MRh_Idle` | `MLh_Idle` |
| 10 | `1HM_Idle` | `1HM_Idle` (the shield is held by the 1HM idle) |
| 11 | `1HM_Idle` | `Tor_1HMPose` (vanilla torch left-arm pose) |
| 12 | `CrossBow_IdleHeld` | `CrossBow_IdleHeld` |
All paths are `Animations\<name>.hkx` (crossbow `Animations\DLC01\CrossBow_IdleHeld.hkx`), mode LOOPING. They are the weapon-ready
idle poses vanilla uses in `Weap_Idle_Behavior` (third-person standing, weapon drawn), `1HM_Readied_Torch_Blend` and the magic
ready idles (`MRh_Ready_MSG`, `MLh_Ready_MSG`).

### 5.5 Not tested in game
Whether Skyrim's runtime renders the parametric ArmLayer exactly as the vanilla locomotion blends do with these masks (same
node classes and flag values as vanilla, validated structurally), and the visual quality of idle poses over a roll.

### 5.6 Why the dodge clip chooser is a state machine, not a selector
An in-game pose trace showed a one-frame whole-body pose step on the 2nd/3rd frame of every dodge whose state played a different clip
last time (head 8-27 units in one frame, size tracking how different the previous clip was). Cause: the SE `hkbManualSelectorGenerator`
has no change effect; it reactivates on its previous child and switches to the bound index a frame later, unblended. Both dodge
choosers are now `hkbStateMachine`s (the TUDM `*DodgeStyle` pattern and vanilla's `startStateId <- iRightHandType` machines):
`startStateId` bound to `RDF_DodgeClip`, DEFAULT start mode, NO_TRANSITION self mode, no transitions/wildcards, one state per
clip (stateId = clip index). The stance selectors are unchanged (they are not switched mid-dodge, but they are a selector on
`iRightHandType`/`iLeftHandType` and could show the same step if the hand type changes between two dodges of one state;
validate in game before converting them too).

## 6. First person (`firstperson~0_master`)

### 6.1 Findings (current Pandora output, dumped with serde-hkx; vanilla ids from the Nemesis `_1stperson` cache via hkxcmd)
- Pandora project `firstperson` (project file `actors\character\_1stperson\FirstPerson.hkx`, character `Characters\FirstPerson.hkx`,
  skeleton `CharacterAssets\skeletonFirst.hkx`, behavior `Behaviors\0_Master.hkx`). Pandora file name for patches:
  `firstperson~0_master.xml` (the log uses the same `firstperson~0_master` form). A bare `0_master.xml` hits the 3P graph.
- The 1P `0_master` is a twin of the 3P one: same 1217 events and 230 variables in vanilla (so `bAnimationDriven` [81],
  `iRightHandType` [35], `iLeftHandType` [60], `currentDefaultState`, `bAttached`, and the events `IdleForceDefaultState` [496],
  `StaggerPlayer` [1072], `staggerStart`, `Ragdoll`, `JumpStandingStart`, `SwimStart`, `bleedOutStart` all exist; the graph lacks
  nothing RDF needs). Current output: 1240 events / 241 variables before RDF.
- Nemesis ids (vanilla 1P `0_master`): StringData `#0095`, VariableValueSet `#0096`, GraphData `#0097`, root `Master_Behavior #1995`
  (8 states, no ragdoll/death wildcards: only the 1P cinematics), `RootBehaviorGraph #0167` (20 vanilla states, startStateId 14,
  wildcard array `#0168`). TUDM's 1P patch touches `#0095`/`#0097` and `#1877 #1880 #1886 #1894`, which agrees with these ids.
- `RootBehaviorGraph` has the same stateIds as 3P: 14 `Default_State` (`Default_Behavior #0363`, START_STATE_MODE_SYNC on
  `currentDefaultState`), 7 `Stagger_State`, 48 `PlayerStagger_State`, 0-5 jump states, 8 bleed-out, 38 swim, 9 `FullBodyPlayerIdles`
  (1P-specific id), 21 horse. Wildcards (`#0168`, 22 in the current output): `staggerStart` -> 7 (conditioned), `WardBreak`, `bleedOutStart`
  -> 8, jump starts/lands, `JumpLandEnd` -> 14 (4 conditioned), `SwimStart`, `Ragdoll` -> 14, `HorseEnterInstant`, idle events -> 9,
  `pa_KillMoveDLC02RipHeartOut`. `StaggerPlayer` is per-state exactly as in 3P (jump states, `Default_State`, ritual spell), so RDF
  states carry their own copy. 9701/9702 are free.
- 1P `Stagger_State` return uses the same 4 conditional transitions as 3P with 1P ids: conditions `#0181 #0180 #0179 #0176`, effect
  `DefaultBlend_FromAnimDriven #0351`; `StaggerPlayer` effect `FastBlendResetTransition #0245`. The patch reuses the conditions.
- TUDM in first person (reference only): events and 1P copies of its `DodgeState` 25 / `Post_DodgeState` 26 machines in the 1P
  `1hm_behavior`, `mt_behavior`, `magicbehavior`, playing `Animations\Roll_*.hkx` / `Sidestep_*.hkx` (1P files); nothing in 1P `0_master`.
- The 1P character `FirstPerson` lists 901 animationNames and ALREADY contains every stance clip below (`H2H_Idle`, `1HM_Idle`,
  `DW1HM1HMIdle`, `2HM_Idle`, `2HW_Idle`, `Bow_IdleHeld`, `StaffRight_Idle`, `MRh_Idle`, `DLC01\CrossBow_IdleHeld`, ...). No
  animationName is added and no 1P character patch is shipped. The 1P skeleton is arms/camera only, so there are no bone masks.
- Vanilla 1P weapon idle (1P `1hm_behavior`: `1stP_WeapIdle_MSG` on iRightHandType, `1HM_1stP_BodyBehavior` -> `WeapIdleMSG` on
  iLeftHandType): right 0 `H2H_Idle` (left shield: 1HM_Idle blend), right 1-4 -> by left hand: 0 `1HM_Idle`, 1-4 `DW1HM1HMIdle`,
  5 `2HM_Idle`, 6 `2HW_Idle`, 7 `Bow_IdleHeld`, 8-11 `1HM_Idle`; right 5 `2HM_Idle`, 6 `2HW_Idle`, 7 `Bow_IdleHeld`, 8-11 1HM pose,
  12 `CrossBow_IdleHeld`. Spells/staffs are drawn by `magicbehavior` (`MRh_Idle`, `MLh_Idle`, `StaffRight_Idle`, `Staff_Idle`).

### 6.2 What the patch adds (365 new nodes)
Same events (13), variables (5: `RDF_DodgeClip/DodgeSpeed/IsDodging/ArmBlend/ChainIndex`), blends, wildcards, exit/stagger transitions and conditional
return as 3P (with the 1P ids above). Per state:
`ModifierGenerator{ BSIsActiveModifier(bAnimationDriven, RDF_IsDodging), Blend{ body mask: DodgeSM, arm mask: Blend(blendParameter <- RDF_ArmBlend){ 0: DodgeSM copy, 1: DrawnSM } } }`, the 3P layout. The 1P skeleton (vanilla `skeletonfirst.hkx`) has the same 99 bones as the vanilla 3P one; the arm mask is `FP_ARM_BONES` (clavicles, arms, twists, hands, fingers, pauldrons, magic and weapon/shield nodes), so the clip moves torso, head and camera while the arms hold the stance.
`DodgeSM = hkbStateMachine, startStateId <- RDF_DodgeClip`, 16 states like 3P, playing Bethesda's own 1P dodge clips (vanilla, already in
`FirstPerson`, playback speed <- `RDF_DodgeSpeed`): right/forward-right/back-right -> `Animations\DodgeRight.hkx`, left/forward-left/back-left
-> `DodgeLeft`, back -> `DodgeBack`, forward -> `MT_Idle` (no 1P forward dodge exists; the vanilla 1P sneak roll dropped the camera far
below the stance arms, so they left the view). Roll and step slots get the same clip. TUDM's 1P `Roll_*`/`Sidestep_*` are 16 byte-identical copies of the vanilla
sneak roll, so they are not used. The clips carry no RDF annotations, so 1P dodges use the ini window timers.
`DrawnSM = hkbStateMachine, startStateId <- currentDefaultState` (the vanilla sync variable holding Default_Behavior's stateId; states 0, 1, 2,
10-14, 20): sheathed/unequipping/killmove ids play the looping neutral 1P idle `MT_Idle` (vanilla 1P sheathed idle), weapon/spell ids (1, 2, 10, 11)
use `StanceSM = hkbStateMachine, startStateId <- iRightHandType, 13 states (stateId = hand type)`; entries are looping `hkbClipGenerator`s
or nested `hkbStateMachine`s on `iLeftHandType`. The plugin sends
`RDF_ArmBlend = fArmStanceFirstPerson` (1.0) while a weapon/spell is drawn, 0 otherwise, and fits the playback to `fClipLengthFirstPerson`.
The chooser is a state machine rather than a ManualSelector for the reason in 5.6 (a selector shows a one-frame step when it reactivates
on a different child; a state machine applies the bound start state at activation).

| hand type | 1P pose (iRightHandType) |
|---|---|
| 0 (fists) | nested on iLeftHandType: 10 (shield) `1HM_Idle`, else `H2H_Idle` |
| 1-4 (one-hander) | nested on iLeftHandType: 0 `1HM_Idle`, 1-4 `DW1HM1HMIdle`, 5 `2HM_Idle`, 6 `2HW_Idle`, 7 `Bow_IdleHeld`, 8-12 `1HM_Idle` |
| 5 | `2HM_Idle` |
| 6 | `2HW_Idle` |
| 7 | `Bow_IdleHeld` |
| 8 staff | `StaffRight_Idle` |
| 9 spell | `MRh_Idle` |
| 10, 11 | `1HM_Idle` |
| 12 | `DLC01\CrossBow_IdleHeld` |
Sheathed: `iRightHandType` keeps the equipped weapon type while sheathed, so the drawn state decides: `currentDefaultState` 0 (MT) -> `MT_Idle`
(neutral), never a weapon pose with the weapon on the hip. Unarmed and drawn -> `H2H_Idle`. The same caveat exists for the 3P arm stance:
the 3P stance selectors only look at the hand types, so the plugin should write `RDF_ArmBlend = 0` when the weapon is sheathed. Left-hand
spell/torch with a right weapon uses the right weapon's pose (the 1P graph has no separate left-arm layer; vanilla 1P also uses one
whole-arms clip).

### 6.3 Validation
`python tools/validate_rdf.py` now also dumps the 1P files (`fp_*.xml`) and runs the P group (77 checks): events/variables present once
and typed, vanilla variables present, root machine +2 states, both states (enter/exit events, generator chain, `bAnimationDriven` /
`RDF_IsDodging` bindings, stance chooser structure and full table, every stance clip in the FirstPerson character, no -1 binding),
wildcards and transitions as 3P with the 1P conditions/effects, vanilla 1P wildcards intact, every other state machine unchanged,
objects lost = only the five modified nodes, hkxcheck equal to baseline, FirstPerson character unchanged, and all other 1P hkx
byte-identical to the baseline run.
Not tested in game: how the 1P arms look when entering/leaving the dodge pose, and whether the game reads the hand-type variables in
the 1P graph as it does in 3P (they are vanilla variables used by the 1P graph itself).

## 7. Prototype: the dodge inside 1hm_behavior (`--onehand`, not shipped)

Why: the engine accepts an action only when its event triggers a transition among the graph's active nodes
(`BShkbAnimationGraph` 0x140AEFEB0 calls the graph's `handleEvent` synchronously and reports the `hkbContext` flag "an event
triggered a transition"). Our root-level dodge states switch 1hm_behavior off, so an attack pressed during them has no
transition and is refused; that is why the plugin waits for the graph to leave the dodge and replays the attack. DMCO puts
its dodge state inside `1HM_Behavior`, next to `AttackState`, with transitions on the attack events (straight into the nested
attack state), so its attacks start at the press.

What `python tools/gen_rdf_patch.py <out> --onehand` writes into `1hm_behavior` (3P; 313 nodes; the 0_master part is the
regular one):
- our events (all of `EVENTS`, `RDF_DodgeStart1H`, `RDF_DodgeStartB1H`, the cancel events) and variables (`VARIABLES`): the
  clips play inside 1hm now, so their annotations must be 1hm events, and the bindings 1hm variables (shared by name);
- `1HM_Behavior` (#4876) states `RDF_1H_Dodge_A` 9701 / `RDF_1H_Dodge_B` 9702: the same generator as the root states (arm
  masks bound to 1hm's own character properties 7 RightArm / 5 LeftArm), and `transitions` = **1HM_Ready_State's own array
  #4872**. Every exit an attack framework adds to Ready therefore also leaves the dodge (VanillaLike: BFCO's
  `BFCOAttackstart_0` / `BfcoAttackSwim` and its nested attack states, MCO's sprint attacks, vanilla attacks of every hand and
  direction, block, bash / power bash, bow, crossbow, shout). Vanilla references whole state machines from several parents
  the same way (1hm `CrossBow_ReloadStandingBehavior` has 4), so a shared array is a known shape for Havok;
- `1HM_Behavior` wildcards (#0088): `RDF_DodgeStart1H` -> 9701, `RDF_DodgeStartB1H` -> 9702 (a chain is the start of the
  other state), `RDF_DodgeExit` -> Ready (0) under `RDF_IsDodging == 1`.

Not a separate behavior file through a BRG (the first idea): Pandora only builds animation data and checks for the files it
patches, a prebuilt hkx would be invisible to it; embedding reuses the generator and the staged validation as they are.
Not a switch inside Ready's own generator either: SCAR replaces `#4874.generator` and Nemesis' core patch `zcbe` replaces
`#3974.generator`; a new state only appends to lists, which merge.

Validation: `tools/stage_pandora.py [--instance stb|vanillalike] --label <ref> --rdf behavior/Pandora_Engine/mod/rdf`, the same
with `--label <proto> --rdf _work/proto_1hm/rdf`, then `tools/validate_onehand.py --ref <ref> --label <proto>`: 63/63 on STB
(Ready has 40 transitions there) and on VanillaLike (60, BFCO / MCO / SCAR / Precision installed).

What the plugin still has to do before this can run in game (none of it written yet):
- send `RDF_DodgeStart1H` / `RDF_DodgeStartB1H` first and the root `RDF_DodgeStart` / `B` only when the 1H event is not
  accepted (1hm not active: sheathed, magic, first person);
- gate what reaches the graph during the dodge's locked part: actions are already held back (capture), but some mods send
  graph events directly (InputSupport's and STB's `blockStart`), and `1HM_Behavior`'s wildcards (recoil, Precision's
  `Collision_Recoil`, combat idles, BFCO / MCO sprint-attack wildcards) now apply to the dodge as well;
- a held-back attack replayed at its window is accepted at once (no waiting for the graph to leave the dodge), and an attack
  pressed inside the window simply passes;
- first person and the Nemesis variant are not covered.
