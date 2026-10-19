# Behavior integration (Pandora)

RDF adds a root-level dodge to the humanoid graph through ONE native Pandora mod, code `rdf`. Nothing is patched in the
weapon / sheathed / magic sub-graphs, so it works with every weapon type and from attacks.

## 1. What it changes (third person only)

`0_master` (Nemesis ids in brackets):
- events (`#0106/eventNames`, `#0108/eventInfos`): `RDF_DodgeStart/StartB/Chain/Exit/Entered/Left` and the clip annotation events
  `RDF_Windows/IFrameStart/IFrameEnd/ChainOpen/AttackOpen/BlockOpen/ExitOpen`
- variables (`#0106`, `#0107`, `#0108`): `RDF_DodgeClip` int 0, `RDF_DodgeSpeed` float 1.0, `RDF_IsDodging` bool, `RDF_ArmBlend` float 0.0
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
- 227 new nodes (`#rdf$0 ..`); all other nodes are untouched.

`defaultmale` / `defaultfemale`: 16 names `Animations\RDF\rdf_{roll,step}_{f,fr,r,br,b,bl,l,fl}.hkx` appended to `animationNames` (`#0029`).

Not touched: first person (C++ blocks dodge there), creatures, every other behavior file, animation data (root motion is
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
python tools/validate_rdf.py                                                 # 127 checks, exit 0 = pass
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
    child0  weight 1  worldFromModel 1  mask NOT_ARMS    generator = dodge ClipSelector (RDF_DodgeClip, 16 clips)
    child1  weight 1  worldFromModel 0  mask ARMS        generator = ArmLayer
  ArmLayer        hkbBlenderGenerator flags 16 (PARAMETRIC), blendParameter <- RDF_ArmBlend
    p0  weight 0  generator = a SECOND dodge ClipSelector (own clip nodes, same variables)
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
