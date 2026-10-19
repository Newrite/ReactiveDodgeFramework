# Modlist and TUDM audit (research, read-only)

Legend: [O] observed (path/command), [I] inferred.
MO2 root `F:\STB\Skyrim\[STB] Mod Organizer`, profile STB. Line numbers are raw modlist.txt lines (first = highest priority; line 1 is a comment header).
Game SkyrimSE.exe 1.5.97; SKSE `skse64_1_5_97.dll` file version 0.2.0.20 [O].

## 0. Key surprises (read first)
1. TUDM has NO SKSE DLL. Its ESP description: "A dynamic dodge mod that does not rely on SKSE" [O: strings of `mods\The Ultimate Dodge Mod\Ultimate Dodge Mod.esp`]. It is Papyrus + behavior + 16 animations.
2. `S:\Programming\Dodge` is NOT a TUDM fork. It builds `DodgeAll.dll` (PRODUCT_NAME "DodgeAll", AUTHOR "Viny", `CMakeLists.txt`), a separate OAR / `BF_*` event based dodge system. `DodgeAll.dll` is NOT installed in any MO2 mod [O: only `DodgeNPC.dll` matches "dodge"]. No `BF_*`, `BFDodgeDirection`, `isDodgingCMF` strings exist in the Pandora output 0_Master / 1hm_behavior [O].
3. The player's dodge KEY is already implemented in `STB.dll` (`UltimateDodge.cpp`, namespace `TKDodge`), reading globals defined by TUDM's ESP (see 4.1).
4. TK Dodge (`TKDodge*` events) exists as files (Ultimate Combat SE) but is NOT in the active Pandora behavior output, so those events are dead in the player graph [O, section 5].

## 1. TUDM as installed

### 1.1 Mods
- `The Ultimate Dodge Mod` (line 1418, version f4.04b, Nexus 9486, 356 files) [O: its meta.ini].
- `(TUDM) The Ultimate Dodge Mod Pandora Fix` (line 114, Nexus 168142 v1.0.0): only `Nemesis_Engine\mod\tudm\*` (2531 txt patch files) + `info.ini` (`name=The Ultimate Dodge Mod`, `auto=Ultimate Dodge Mod.esp`) [O].
- Pandora applied it as "Pandora Mod 17 : The Ultimate Dodge Mod". Only warning: `defaultfemale~1hm_behavior > Replace Text at #0426/modifiers FAILED` (non-fatal; the Turn patch has the same) [O: `mods\Pandora\Engine.log`].

### 1.2 Files
- `Ultimate Dodge Mod.esp`; 21 `UD*.psc/.pex` (scripts\Source), 9 vanilla script overrides (Companions, DGIntimidate, MS11Calixto, ...); ~190 roll wavs under `sound\fx\roll\<surface>\{light,heavy}`; Russian translation; `controlmap.txt.mohidden` (hidden by user) [O].
- Animations `meshes\actors\character\animations\`: 8 `roll_*.hkx` + 8 `sidestep_*.hkx` (forward, forwardleft, forwardright, left, right, backward, backwardleft, backwardright). 1st person: 16 stub anims of 9056 B [O].
- Shipped full behaviors (0_master, 1hm, mt, magic, magicmounted, bow_direction, + _1stperson, defaultmale/female) exist but are outranked by the Pandora output (Pandora line 54 beats TUDM line 1418) [O].
- No INI. Settings are MCM globals (`UDMCMMenuScript`). ESP defaults [O: housecarl_records GLOB]: `UDDodgeKey=56` (DIK Left Alt), `UDSprintDodge=1`, `UDNPCDodgeAIGlobal=0`. Script defaults: IFrameR 0.4 s, IFrameS 0.25 s, StaminaCostR 20, StaminaCostS 10, MaxSpeedPenalty 0.4, DodgeSpeed 1.0 (roll) / 1.647 (sidestep) [O: `scripts\Source\UDActivationScript.psc:30-41`].

### 1.3 Animation data (hkxc convert to XML) [O]
- All 16 clips are ~0.667 s (20 frames at 30 fps); only `sidestep_forward` is 0.679 s (re-saved 2026-09-05).
- Clips carry NO event annotations; only `animmotion` root-motion keys. Total travel ~248-250 units (forward ends `0 248.463 0`, backward `0 -250.521 0`, left `-250.036 0 0`).
- `roll_X` and `sidestep_X` have identical byte sizes per direction (e.g. 103552 backward, 103680 left), so the sets are probably duplicates [O sizes, I content]. TK Dodge `Stepdodge*` have the same sizes too.

### 1.4 Behavior wiring (Pandora output `mods\Pandora\meshes\actors\character\Behaviors\mt_behavior.hkx`, converted) [O]
- New graph variables: `DodgeSpeed` (float, bound to blender `playbackSpeed`) and `DodgeID` (int, bound to `startStateId` of each direction style SM: 0 = roll clip, 1 = sidestep clip). There is no angle/direction variable.
- New events: 8 directional starts `ForwardRollStart, Right_ForwardRollStart, Right_RollStart, Right_BackwardRollStart, BackwardRollStart, Left_BackwardRollStart, Left_RollStart, Left_ForwardRollStart`; 8 `Post_*RollStart`; `RollStart, RollStop, RollTrigger, SidestepTrigger, RollContinue, RollLeap, RollLand, RollWinStart/End`. 0_Master also has `DodgeMasterSpell, DodgeCharge, DodgeLightningStorm` (NPC telegraph events).
- mt_behavior root SM `MT_RootBehavior`: states `DodgeState` (id 25) and `Post_DodgeState` (26); `RollStart -> 25` (transition array #0019). Inside: SM `DodgeDirection` StateA = CBTG blend `DodgeDirectionA_CBTG` (fTransitionDuration 0.1, smooth curve), StateB = `DodgeDirection_MG`. Directional start events switch StateA -> StateB with no blend; `RollStart -> StateA` 0.1 s, condition `DodgeID == 0`. Three banks (A, B, C/Post), 8 style SMs each, two states each (roll / sidestep clip).
- `DodgeRollTransition` blending effect 0.1 s, `FLAG_IGNORE_FROM_WORLD_FROM_MODEL`; `DodgeRollAllowRotationModifier` is a BSIsActiveModifier bound to variable index 50 (rotation allowed during dodge).
- `Post_DodgeState`: `RollStart -> 25` 0.1 s (`DodgeID == 0`); `RollStop -> state 0` 0.1 s.
- 1hm_behavior: `RollStart -> 25` 0.1 s gated by `iRightHandType` (bows/crossbows 5/6 go to states 2/3/50-53; `IsNPC` variants); `Post_*RollStart -> 26`; `RollStop -> 0/1` 0.1 s by `iWantBlock`/`iLeftHandType`. magicbehavior mirrors with 0.1 s. `DodgeID/DodgeSpeed` exist in 0_Master, 1hm, magic, mt.
- 0_Master also has `TDM_Dodge`, `TDM_Roll`, `TDM_LockRotation` (from TDM Nemesis patches).
- Who fires `RollStart`: STB.dll (string `RollStart` in STB.dll, sender not decoded) [I], and TUDM NPC AI via `enemy.playIdle(DodgeRollNPC)` [O: `UDBattleReady.psc` BeginDodging]. TUDM scripts contain no player input code.

### 1.5 Papyrus logic
- Player i-frames: `registerForAnimationEvent(player,"RollTrigger"/"SidestepTrigger")`; then `player.setGhost(true); Utility.Wait(0.4 | 0.25); setGhost(false)` [O: `UDActivationScript.psc` OnAnimationEvent ~L49-66 and InvincibleFrame ~L214-224]. `StaminaDamage()` is an empty stub in the shipped source; cost is likely applied through spell `UDRollZeroStaminaDamage` [I].
- DodgeID/DodgeSpeed picked by armor weight, carry weight and enemy-size counts (`ReEnemyCheck`, `WeightCheck`, `ArmorCheck`) [O].
- NPC reactive dodge (relevant to "reactive"): `UDBattleReady` registers on the PLAYER's anim events `BowDrawing, BowFullDrawn, PowerAttack_Start_end, NextAttackInitiate, NextPowerAttackInitiate, DodgeSpell, DodgeMasterSpell, DodgeLightningStorm, DodgeCharge`, gates on `getHeadingAngle` (<=5 deg ranged, <=35 melee), distance (<=1100 / 2100), LOS and a pseudo-random chance, `Utility.wait(0.3-0.5)`, then `playIdle(DodgeRollNPC)`; NPC i-frames also by `setGhost` [O: `UDBattleReady.psc` ~L61-95, 320-470].
- Weaknesses: Papyrus latency (event round trip + Wait); `setGhost` is binary, wall-clock timed and stalls with the VM; one ActiveMagicEffect spell + animation-event registration per enemy; clip lengths are not tied to i-frame length; 8 separate events instead of an angle variable; clips have no event annotations so `RollTrigger` must come from the graph [I].

### 1.6 `S:\Programming\Dodge` = DodgeAll (NOT installed)
Builds: `build\release\DodgeAll.dll` (2026-08-17), `build\release-se\DodgeAll.dll` (2026-09-11) [O]. Sources: Events.cpp 1825 lines, Settings.cpp 2809, GhostTrail.cpp 570, Hooks.cpp 284.
- Direction: graph var `DirecionalCycleMoveset` (owned by the "Direction-Movement"/DMK mod, not installed), optional raw-input re-aim `DodgeReaim::Resolve` (`Events.cpp` ~1268-1283), republished as `BFDodgeDirection` (`Events.cpp` ~1293). 8 directions.
- Events sent: `BF_RollDodgeStart`, `BF_StepDodgeStart`, `BF_DashStart`, `BF_DodgeStop` (`Events.cpp` ~1299-1336); `StopTimeStartCMF/EndCMF`; `PerfDodgedCMF/DodgedCMF/GotDodgedCMF/GotPerfDodgeCMF` (`Hooks.cpp:73-79, 142-148, 213-219`); bools `isDodgingCMF`, `isPerfDodgingCMF`.
- Movement: own impulse (`ProcessAndExecuteDodgeImpulse`), not root motion.
- I-frames: `g_dodgeWindows` map keyed by FormID under a `shared_mutex`, steady_clock timestamps, delayed callbacks from a `std::jthread` worker (`DelayedDispatcher.h:18,60`; `Events.cpp` ~185-370).
- Stamina: `ConsumeResource` with cost source / actor value / perk gates (`Events.cpp` ~700-710, ~1304). Cancellation: movement cancel (`TryCancelPlayerDodgeFromMovement` ~L197-250), attack cancel, spam gating `canSpamDodge`.
- Weaknesses (line refs are approximate, from grep): (a) direction depends on an external mod's graph var, which the author documents as rewritten every frame, workaround is a 2nd var (`Events.cpp` ~1275-1293 comment); (b) timers run on a worker thread against real time, not game or clip time, hence the StopTime hacks (`Events.cpp` ~535-550); (c) clip selection delegated to OAR reading the var one frame after the event (`Events.cpp` ~1345-1352, "measures what the graph sees one frame later"); (d) impulse movement diverges from clip root motion and TDM rotation lock (special case for TDM free-movement); (e) one global mutex + map; (f) settings duplicated across Roll/Step/Dash/NPC structs [I].
- Missing in modlist: any behavior patch defining `BF_*` events [O].

## 2. `S:\Programming\CustomDodge` (xmake, CommonLib NG, SE only) [O]
Earlier own attempt. `SimpleDodge.cpp` (1788 lines): "one extremely simple impulse type dodge": state {elapsed, duration, baseSpeed, maxDistance, startPos, localDir, WorldDir, landingLock}, 8-direction enum, ini keys `DodgeKey`, `TapDodgeWindow`, `TapDodgeOnRelease`, `SteerDodge`, `PostDodgeMomentum`, `DodgeCooldownSeconds`, `StaminaCost`/`StaminaAsPercent` (or global `Dodge_StaminaCost`, FormID 0x802), `FarDodge_*` hold variant, calls Papyrus `Actor.SetGhost` natively (`SimpleDodge.cpp:747`), TDM compat by looking up global `TDM_DirectionalMovement` (`:549-552, 1005-1009`). No animation graph integration visible by grep [I].

## 3. Pandora
- Output lives in MO2 mod `Pandora` (line 54), not overwrite. Last run 2026-10-18 19:37. Log `mods\Pandora\Engine.log`; also `Pandora_Engine\ActiveMods.json`, `PreviousOutput.txt`. Engine mod: `Pandora Behaviour Engine Plus` (line 1109, v1.0.1.0) [O].
- Output behaviors [O], all 2026-10-18: `Behaviors\` `0_Master, 1hm_behavior, 1hm_locomotion, bashbehavior, blockbehavior, bow_direction_behavior, crossbow_direction_behavior, horsebehavior, idlebehavior, magicbehavior, magicmountedbehavior, magic_readied_direction_behavior, mt_behavior, shoutmounted_behavior, shout_behavior, sprintbehavior, staggerbehavior, weapequip`; `Characters\DefaultMale/DefaultFemale.hkx`; `_1stperson\Behaviors\{0_Master,1hm_behavior,bow_direction,magicbehavior,magicmounted,mt_behavior,weapequip}`; ~25 creature behaviors; `animationdatasinglefile.txt`, `animationsetdatasinglefile.txt`; `SKSE\Plugins\fnis_aa\config.json`.
- Engine.log: `FNIS Parser > Scan FNIS Animations > TKDodge in defaultfemale > FAILED` (so FNIS_TKDodge_Behavior is not injected) [O].
- Active patch mods (27, Pandora order): 1pvaf, abeef, ebeef, draws, flinch, gmill, haasf, hpmhr, jpbo, evfmgo (Payload Interpreter), colis (Precision), pscd, rthf, rtsf, SkyParkour 1st/3rd, tudm, tdm horse/head/lean, turn, ultcr, itmuse, wbsp, bowspd, xpmse, pandora base [O].
- Patch folders found in modlist (code: mod @line: behaviors touched; counts = patch files):
  - `tudm`: TUDM Pandora Fix @114: 0_master 9, 1hm 681, mt 212, magic 266, magicmounted 44, bow_direction 26, defaultmale/female, animdata/setdata, + `_1stperson` (0_master 8, 1hm 534, magic 257, magicmounted 40, mt 212, bow 26).
  - `colis`: Precision @1396: 0_master 93, 1hm 75, 1hm_locomotion, bash, block, bow, crossbow, horse, idle, magic... (combat graphs, not dodge states).
  - `evfmgo`: Payload Interpreter @1475: 0_master, 1hm, 1hm_locomotion, bash, block, bow, horse, idle, magic(+mounted), magic_readied, mt, shout(+mounted)... (2 files each).
  - `tdmv` @1385: 0_master 8, 1hm 6, magic 6; `tdmlen`: 0_master 19; `tdmh`: horse 151 (TDM vars).
  - `jpbo` (Jump Behavior Overhaul @1304): 0_master 92. `sppftp` (SkyParkour @1288): 0_master 12. `itmuse` (potions): 0_master 15. `flinch`: 0_master 107 + creatures. `bowspd`: 0_master 9, 1hm 19. `haasf`: mt 24. `gmill`: mt 1. `rtsf`: 1hm 3. `turn`: 1hm 53. `rthf`: 1hm 91. `pscd`: 1hm 29, magic 25, shout 35 (these three in "Some Nemesis defaults for Pandora" @112).
  - `wbsp` (WeaponBlockSprint @64, Pandora xml format): `patches\0_master.xml`, `blockbehavior.xml` (no dodge states).
  - `pandora` (engine bundled): `0_master.xml, magicbehavior.xml, magicmountedbehavior.xml`.
  - `tkds` (in engine mod): animationdatasinglefile only (entries TKDodgeBack/Forward/Left, `TKDodgeStop:0.8`), no behavior [O].
  - Disabled: `NEMESIS - Unlimited Behavior Engine` (zcbe, tkuc, nemesis...).
- Consequence: any new mod patching 0_master / mt / 1hm / magic stacks with 12+ patches; TUDM's patches already rewrite the dodge region of mt_behavior and 1hm. Reusing `RollStart` / `DodgeID` / `DodgeSpeed` would collide [I].

## 4. Conflicts and neighbours
### 4.1 STB.dll (mods\STB\SKSE\Plugins\STB.dll, 3.1 MB, 2026-10-18; PDB `F:\STB\STB_RE\v3.1-2026-10-15\STB.pdb`) [O]
- Source `...\STB\src\UltimateDodge.cpp`. Classes: `TKDodge::InputEventHandler` (BSTEventSink<InputEvent*>; `GetKey, GetJumpKey, GetSprintKey, GetPowerAttackKey, GetFavoriteKey, MapDeviceKey, GetGamepadIndex`), `TKDodge::PlayerAnimationHandler`, `TKDodge::BashKey::{Press,Tick,kTimeout}`, `TKDodge::ShieldCharge::{Tick,block_key_held}`, `TKDodge::SellByCategory`, `init_forms`.
- Strings: `UDDodgeKey`, `UDSprintDodge`, `KeyDodge`, `SprintDodge`, `DodgeID`, `DodgeSpeed`, `TKDodgeStop`, `RollStart`, "No Dodge Event Get!". Both `Ultimate Dodge Mod.esp` and `STB.esp` contain `UDDodgeKey` / `UDSprintDodge` EditorIDs. Interpretation: STB.dll reads the TUDM globals (LAlt = 56; sprint-tap dodge on) and fires `RollStart` after setting DodgeID/DodgeSpeed [I: strings only, not decompiled].
- Also `Effects::ForDodge/CheckDodge` with spells `aaMZs_DodgeResist, aaMZkw_DodgeVisual, aaMZs_NpsDodgeKD, aaMZs_LA_DodgeEffectKD, aaMZp_LA_Dodge`, and `CrossbowReload::DodgeOutOfReload` (graph patch cancelling crossbow reload) [O].
- So STB.dll implements the player dodge input and a dodge-resist layer. No separate ini; config is TUDM MCM globals. A new framework must replace or coexist with it.

### 4.2 Others
- `STB\SKSE\Plugins\DodgeNPC.dll` (844 KB, 2026-10-08; `DodgeNPC.pdb` next to it; source path `C:\dev\DodgeNPC\src\main.cpp`): NPC dodging; strings `TKDodgeRight/Forward/Left/Back`, `MT_Dodge`, `DodgeRoll` (combat tree node), "Dodge Threat Selector", `schedule_shot_dodge_followup`, `TriggerSinkDodges`. Sends `TKDodge*` events, which no active behavior handles in the human graph [I].
- True Directional Movement `d2024.10.7` (line 1386), DLL 2024-12-05 with PDB. User settings (`MCM\Config\TrueDirectionalMovement\settings.ini`): `uDirectionalMovementSheathed=2`, `uDirectionalMovementDrawn=2`, `fDodgeUnlockedRotationSpeedMult=0.5`, `fRunningRotationSpeedMult=1.5`, `fSprintingRotationSpeedMult=2`, `bFaceCrosshairWhileBlocking=1`, `fTargetLockDistance=2000`. DLL embeds `DodgeFramework.dll`, `TDM_Dodge`, `TDM_Roll` [O]; no `DodgeFramework.dll` is installed.
- Precision `2.0.4.0` (line 1397). Behavior Data Injector `d2023.11.3.0` (line 1474). Payload Interpreter `1.1.0.0`, two mods ship the DLL, "(import 2)" wins. OAR `d2026.8.2` + IED conditions + RaySense (lines 1315, 1475) [O].
- Not found: DMCO, BFCO/MCO DLLs, SCAR, TK Dodge RE DLL, DodgeFramework.dll, Direction-Movement, DodgeAll. Only "Nier Replicant X ... MCO-BFCO Ultra greatsword moveset" (line 16, OAR moveset). `Draugr Shout Dodge` (line 1431) is just 3 OAR draugr shout-inhale variants, not a dodge framework [O].
- `InputSupport.dll` (mod `Inputtest`, line 35): ini keys `KeyBlock`, `KeyPowerAttack=47`, magic/ranged support flags; no dodge. `STB_HotkeySystem` (line 1173): favorites hotkeys only. So the only dodge key binding is STB.dll's UDDodgeKey (LAlt). Sprint-related neighbours: ClassicSprinting, SprintStutteringFix, SSMT_Fix, WeaponBlockSprint.
- Movement / input / animation plugins present: TrueDirectionalMovement, Precision, BehaviorDataInjector, PayloadInterpreter, OpenAnimationReplacer (+IED, RaySense), AnimationMotionRevolution (root-motion tweak, relevant to annotation-driven clips), AnimationQueueFix, BetterJumpingSE, StepUpOntoSKSE, InputSupport, STB_HotkeySystem, ClassicSprinting, WeaponBlockSprint, BetterAttackControl, SmarterCombatTarget, MaxsuPowerAttackControl, MaxsuCombatEscape, MaxsuOnHitAnimFW, loki_DynamicAnimationCasting, BehaviorPatcher.dll, AUltimateCombat.dll, InsertAttackData.dll, KeyboardShortcutsFix [O: housecarl_skse inventory + mods scan].

## 5. Dodge animation sets usable as dev fixtures
1. TUDM: `mods\The Ultimate Dodge Mod\meshes\actors\character\animations\{roll,sidestep}_{forward,forwardleft,forwardright,left,right,backward,backwardleft,backwardright}.hkx` (16 files, 0.667 s, ~250 u animmotion). Already wired in the live mt_behavior as `Animations\Roll_*.hkx` / `Sidestep_*.hkx`; the 8 directional events work today [O]. Best fixture: fire `ForwardRollStart` etc. with `DodgeID` 0/1 [I].
2. TK Dodge: `mods\Ultimate Combat SE\meshes\actors\character\animations\TKDodge\{Stepdodgeback,Stepdodgeforward,Stepdodgeleft,Stepdodgeright}.hkx` + `behaviors\FNIS_TKDodge_Behavior.hkx` (events `TKDodgeForward/Back/Left/Right`, `TKDodgeStop`; vars `aTKDodge*`). Not in the active graph (Pandora FNIS parse failed) [O]. Old shipped `1hm_behavior`/`magicbehavior` in TUDM and Ultimate Combat SE contain TKDodge states but are overridden [O].
3. No DMCO/BFCO step/roll sets found (name-based search only).
4. Vanilla NPC `DodgeBack/Left/Right` (sprintbehavior, `Animations\DodgeBack.hkx` ...) in output [O].

## 6. SKSE and Address Library
- SKSE64 mod (line 1487) ships `skse64_1_5_97.dll` (1,315,328 B, 2021-10-03, 0.2.0.20) in game root [O].
- Address Library for SKSE Plugins (line 1107); `version-1-5-97-0.bin` located at `mods\Skyrim Script Extender 64 (SKSE64)\SKSE\Plugins\` [O]. 157 plugins declare Address Library, 3 version-locked (FasterCellLookup, KeyboardInputOffsetCrashFix, TheosRenderPipeline) [O].

## 7. Recommendations (inferred)
- Treat STB.dll's `TKDodge` handler plus `RollStart/DodgeID/DodgeSpeed` as legacy; do not reuse those names (TUDM, STB, DodgeNPC, 4 behaviors use them). Use namespaced event/var names injected via a Pandora patch (and/or BDI).
- Use TUDM's 16 clips first as fixtures (already wired to the 8 directional events).
- Do i-frames natively on game time (avoid Papyrus Wait and wall-clock worker threads, the weaknesses of TUDM and DodgeAll).
- Snapshot direction at key press from raw input, independent of per-frame-rewritten vars from other mods.
- Check AnimationMotionRevolution interplay with annotation-based root motion.
