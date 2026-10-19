# Behavior integration (Pandora)

RDF adds a root-level dodge to the humanoid graph through ONE native Pandora mod, code `rdf`. Nothing is patched in the
weapon / sheathed / magic sub-graphs, so it works with every weapon type and from attacks.

## 1. What it changes (third person only)

`0_master` (Nemesis ids in brackets):
- events (`#0106/eventNames`, `#0108/eventInfos`): `RDF_DodgeStart/Chain/Exit/Entered/Left/ClipEnd`
- variables (`#0106`, `#0107`, `#0108`): `RDF_DodgeClip` int 0, `RDF_DodgeSpeed` float 1.0, `RDF_IsDodging` bool
- `RootBehaviorGraph` (`#0340`): new states `RDF_Dodge_A` (stateId 9701) and `RDF_Dodge_B` (9702). Each is
  `hkbModifierGenerator{ BSIsActiveModifier(bAnimationDriven, RDF_IsDodging), hkbManualSelectorGenerator(RDF_DodgeClip) ->
  16 hkbClipGenerator (SINGLE_PLAY, speed <- RDF_DodgeSpeed, RDF_DodgeClipEnd trigger at end-0.05 s) }`, enter notify
  `RDF_DodgeEntered`, exit notify `RDF_DodgeLeft`.
- root wildcard array (`#0341`): `RDF_DodgeStart` -> 9701 (IS_LOCAL_WILDCARD, `RDF_DodgeInBlend` 0.12 s).
- A/B transitions: `RDF_DodgeChain` A<->B (`RDF_DodgeChainBlend` 0.10 s); `RDF_DodgeExit`, `RDF_DodgeClipEnd` and
  `IdleForceDefaultState` -> `Default_State` (14) with `RDF_DodgeOutBlend` 0.25 s. The return is written like vanilla
  `Stagger_State` (4 conditional transitions per event that restore the equip sub-state, reusing the vanilla conditions
  `#0355 #0354 #0353 #0350`); `RETURN_MODE = "simple"` in the generator gives one plain transition instead.
  `StaggerPlayer` -> `PlayerStagger_State` (48): the player's stagger event is per-state in vanilla (not a wildcard), so our
  states need their own copy.
- 117 new nodes (`#rdf$0 ..`); all other nodes are untouched.

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
python tools/validate_rdf.py                                                 # 80 checks, exit 0 = pass
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
