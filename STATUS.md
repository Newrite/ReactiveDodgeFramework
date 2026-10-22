# Status (2026-10-22)

## Working in game (verified with dev-log traces and the user's play-tests)
* 2026-10-22 (VanillaLike, BFCO): power attacks out of a dodge with and without move input (BFCO diagonals), perfect
  dodges, i-frames against arrows and spells (projectiles pass, not removed).
* Dodge from idle and from running, all 8 directions, keyboard (key V) with TDM in both 360 and Vanilla Style.
* Plugin-owned displacement: requested and measured displacement match frame by frame; heading error 0.3-7 deg.
* Continuous steering mid-dodge; diagonals from the plugin's own key/stick tracking (TDM rewrites the game's input).
* Orientation: 360 mode turns to the path; backward dodges (> 100 deg) and Vanilla Style / lock-on keep facing and
  play the directional clip.
* Dodge -> dodge chaining (A/B states), mashing consumes presses once, buffered presses fire when the window opens.
* Clip annotations drive the cancel windows (`RDF_Windows` received on 72/72 dodges in the last run).
* Hand-over into running with the out-blend driven by the plugin (no planted frames) and the post-dodge speed floor
  (no one-frame speed drop / stance jerk; 51/58 continuous, the rest had no movement held or were sprinting).
* Attack -> dodge before the attack's end (user-verified).
* Dodge -> attack (cancels at 0.21-0.46 s, momentum carried) and dodge -> held block.
* Arm stance layer: during steps the arms keep the current weapon's stance while it is drawn; sheathed dodges use the
  clip's own arms (user: both look right).
* First external clip set as an OAR mod (`AI - RDF Dodge Set - Greatsword`, `tools/build_clip_set.py`): 134/134
  dodges played it with its own windows (user: looks and feels good).
* First person: vanilla 1P dodge clips on torso/camera with the arms masked to the stance (fArmStanceFirstPerson 1)
  (user-verified 2026-10-22).
* The "press -> twitch, no dodge" bug: both causes fixed (start into the state being left; zero-length streaming clip
  firing a clip-end exit). 0/72 failures after the fix.

## Built and deployed, waiting for an in-game check
* Projectile i-frames (`bProjectileIFrames`): arrows and spell missiles fly through during the window.
* Precision hits ignored during i-frames (pre-hit callback, `[Compat] bUsePrecision`).
* Gamepad and analog magnitudes; dodge button on RS click collides with TDM's target lock (rebind in the menu).
* Lifecycle: cell change, death, menus mid-dodge.
* Camera switch mid-dodge sends the exit; the orphaned-state watchdog.
* First person: forward without the sneak roll, mouse turn during the dodge (fFirstPersonLookScale).
* Dodge out of a charging spell interrupts the cast (InterruptCast on enter).
* Jump blocked until the exit window (`[Jump]`); dodge out of stagger (`[Stagger]`).
* Steering limits (fMaxDeviation, fInputCone, bInputFromStartCamera, bHoldFacing), direction commit window,
  orientation warp, facing handed back to the camera at the end (no TDM snap on the next attack), TDM mode latched
  for a chain (diagonal chains keep the arm blend).
* FLICK menu (F7): seven pages, live edits, save to ini, dodge button rebinding.
* Nemesis patch (`Nemesis_Engine/mod/rdfm`): offline check 33/33, never run through Nemesis itself.
* Motion hook chains behind another plugin's hook on the same call (user fix); not yet seen with one installed.
* `RDF_ChainIndex` graph variable (0, 1, 2... per dodge of a chain, 0 when over); needs a Pandora / Nemesis re-run.
* Dodge on any actor through the API (Papyrus `ReactiveDodge.Dodge`, C `RDF_RequestDodge`); never run on an NPC yet.

* Dodge cancels attacks of MCO / BFCO from their recovery (`MCO_IsInRecovery`, `MCO_Recovery` .. `MCO_InputBuffer`,
  `BFCO_DIY_recovery` .. next `Bfco_AttackStartFX`), attack clip annotations RDF_DodgeCancelOpen / Close (registered in
  1hm_behavior), and the anytime keywords ([AttackCancel]). Needs a Pandora re-run; not yet tried with MCO or BFCO.
* Dodge -> attack verified by the attack state and repeated after the graph left the dodge (MCO / BFCO dropped it);
  not yet tried with either.
* Attack attempts of any source (input, BFCO power idles, Input Support actions, AI) held back during the dodge and
  replayed as they were when it cancels into an attack (ActorMediator::PerformAction callers); not yet tried in game.
* Dodge inside 1hm_behavior (weapon drawn, third person; BEHAVIOR_INTEGRATION.md 7): attacks / block / bash leave it at
  once through 1HM_Ready_State's transitions, other mods' direct blockStart is held until the block window. Staged
  Pandora validated on STB and VanillaLike; needs a Pandora re-run; not yet run in game.

## Known limitations
* A one-frame upper-body pop at dodge start, visible only in slow motion (parked; findings in JOURNAL.md).
* Fixture animations are TUDM's clips (local only, not redistributable); visual quality is bounded by them, e.g. they
  settle into a full stance at the end. A proper clip set is needed for final visuals.
* First person: no 1P forward dodge clip exists; forward keeps the neutral idle on the body.
* Beams and flame streams are not covered by projectile i-frames; left-hand and dual attacks from a dodge are not
  implemented.
* In this modlist V also triggers a bash when a weapon is drawn (another mod's binding); choose a free key if that
  matters (FLICK menu, Controls page).
* fRunSpeed (exit speed) is a constant; it matches STB's running speed (~300) by chance, not by reading it.

## Next actions
1. In game: run into running after dodges (speed floor), shoot arrows/spells at the player (i-frames), dodge -> attack
   and attack -> dodge, block from a dodge; send the log.
2. Remove the dev tracing of graph events once the above is confirmed (it is behind `bDevLog` already).
3. A proper animation set annotated with `tools/annotate_clips.py`.
4. Neutral dodge clip (built 2026-10-22, waiting for an in-game check; since 2026-10-23 one set of 9 clips, no roll /
   step kinds): RDF_DodgeClip 8 = RDF_Dodge_Neutral, picked for a fresh dodge pressed without move input that keeps facing. Default content: copies of
   the backward clips (annotations included) in the RDF mod; 1P plays DodgeBack. Both patches, install_fixtures,
   build_clip_set (optional "n" in a spec, else "b"), validate_rdf. The greatsword OAR set in MO2 was built
   before this and has no n clips: there the base n (the TUDM backward copy) plays until the set is rebuilt.
5. Projectiles, every type (built 2026-10-22, waiting for an in-game check): AddImpact (vfunc 189) hooked on all seven
   projectile vtables, HandleHits (190) on the single ones; arrows, missiles and grenades that passed through during
   i-frames are remembered and never hit the player late, and so are cones (shouts, breath: a cone outlives any
   i-frames), per target only; beams, flames and barriers skip the player only while invulnerable. Test: arrows / firebolts through a dodge, a dodge late in a bolt's flight, Flames and Sparks streams,
   a Wall of Flames, a thrown grenade if any mod has one.
6. Explosions (built 2026-10-22, waiting for an in-game check): Explosion::ApplyToTargets' line-of-sight check says
   "blocked" for an invulnerable player (no damage, spell or knockdown). Test: fireball blast during i-frames vs after,
   a rune / trap. Open: a dodged fireball that explodes after the i-frames still hits (the explosion does not know its
   projectile); decide whether that needs more (e.g. matching the blast to a remembered projectile by time and place).
7. Data layers for designers and modpacks (design agreed in chat 2026-10-21): (a) runtime modifiers from active magic
   effects by keyword, Reflyem-style (sum of magnitudes; i-frame add/mult, cost mult, distance mult, chain add, gates
   such as RDF_Allow_DodgeFromStagger / RDF_Block_Dodge), with a log line explaining each resolved value; (b) profile
   files with OAR-like conditions and priorities (cost, cooldown, chain, gates, window scale, distance); (c) a clip
   descriptor annotation at 0.0 (versioned, e.g. RDF_Clip.v1|dur=..|dist=..|motion=root) and a root-motion mode that
   follows the clip's own animmotion. Per actor / NPCs later.
8. Exit speed from the real locomotion speed (movement type run/walk speed x SpeedMult, sprint) instead of fRunSpeed.
9. Perfect dodge, base (built 2026-10-22, waiting for an in-game check): a hit the i-frames dodged (melee, Precision,
   projectile, explosion) inside [fPerfectWindowStart, fPerfectWindowEnd] counts once per dodge: graph events
   RDF_PerfectDodge (player) / RDF_PerfectDodged (attacker, both registered in the patch), the SKSE mod event
   RDF_PerfectDodge (sender = attacker, numArg = dodge time), optional slow motion ([Perfect], FLICK Rules page).
   Since 2026-10-22: clip annotations RDF_PerfectStart / RDF_PerfectEnd override the ini window (registered in both
   patches); slow motion through VATS::SetMagicTimeSlowdown (43103, the Slow Time shout's call, checked in IDA):
   world and player speeds apart (fPerfectSlowMotionPlayer 1 = the player keeps full speed), ease in / hold / ease out,
   re-hold on another perfect dodge, cooldown, skipped while the shout or a kill cam owns the slowdown, left alone if
   one takes it over; the player's dodge clock runs at the engine's player/world ratio meanwhile.
   Next: a window measured from the attacker's swing start (reaction-based), perk / keyword gating through item 7.
10. Lock-on style option: Souls-like FaceMotion rolls under lock-on (turn to the path, roll forward, turn back to the
    target in the recovery) next to today's directional clips.
11. NPC dodges. Mechanics done (2026-10-22, waiting for an in-game check): the dodge is a per-actor `DodgeAgent`
    (core, graph handshake, clips and arm stance, chains, exits, cost, stagger escape, i-frames, perfect dodge, motion
    slot) used by the player and by any actor asked through the API: `ReactiveDodge.Dodge(Actor, float)` (Papyrus,
    console `cgf`), `RDF_RequestDodge` (C export, `api/ReactiveDodgeAPI.h`). An NPC faces its combat target through
    the dodge (side dodges around it) and turns to the path without one; its agent is dropped a second after the
    dodge. Player-only by design: input, camera / first person, TDM, block holding, jump gate, slow motion.
    Next: the AI side that decides when an NPC dodges (attack start / projectile launch events), and checking that the
    combat AI does not fight the animation-driven motion.
