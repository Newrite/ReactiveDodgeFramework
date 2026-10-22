# Status (2026-10-22)

## Working in game (verified with dev-log traces and the user's play-tests)
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
* The "press -> twitch, no dodge" bug: both causes fixed (start into the state being left; zero-length streaming clip
  firing a clip-end exit). 0/72 failures after the fix.

## Built and deployed, waiting for an in-game check
* Projectile i-frames (`bProjectileIFrames`): arrows and spell missiles fly through during the window.
* Precision hits ignored during i-frames (pre-hit callback, `[Compat] bUsePrecision`).
* Gamepad and analog magnitudes; dodge button on RS click collides with TDM's target lock (rebind in the menu).
* Lifecycle: cell change, death, menus mid-dodge.
* Camera switch mid-dodge sends the exit; the orphaned-state watchdog.
* First person: vanilla 1P dodge clips on torso/camera with the arms masked to the stance (fArmStanceFirstPerson 1),
  forward without the sneak roll, mouse turn during the dodge (fFirstPersonLookScale).
* Dodge out of a charging spell interrupts the cast (InterruptCast on enter).
* Jump blocked until the exit window (`[Jump]`); dodge out of stagger (`[Stagger]`).
* Steering limits (fMaxDeviation, fInputCone, bInputFromStartCamera, bHoldFacing), direction commit window,
  orientation warp, facing handed back to the camera at the end (no TDM snap on the next attack), TDM mode latched
  for a chain (diagonal chains keep the arm blend).
* FLICK menu (F7): seven pages, live edits, save to ini, dodge button rebinding.
* Nemesis patch (`Nemesis_Engine/mod/rdfm`): offline check 33/33, never run through Nemesis itself.
* Motion hook chains behind another plugin's hook on the same call (user fix); not yet seen with one installed.

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
4. Neutral dodge clip "N": its own slot for a dodge pressed without move input (today that backstep plays the backward
   clip, slot 4 / 12). Default content: a copy of the backward clip, so nothing changes until a set provides its own.
   Touches: RDF_DodgeClip index (new slot after the 16 directional ones) and ClipIndex in the plugin, CLIPS in
   gen_rdf_patch.py (Pandora + Nemesis patches, character animationNames), the 1P chooser (DodgeBack), the clip-set
   spec / build_clip_set.py ("n" direction, falling back to "b"), annotate_clips.py and ANIMATION_CONTRACT.md.
5. Projectiles that outlive the i-frames. Arrows and missiles already pass through (their contacts with the player are
   dropped while invulnerable, the projectile is not destroyed, as in Elden Ring; not yet checked in game). Open case: the
   i-frames end while the projectile still overlaps the player, so the next physics step lands the hit late. Plan: remember
   every projectile that passed through during i-frames (handle + expiry) and keep ignoring its contacts with that actor
   for the rest of its life, or until it has fully left the actor's collision; decide which. Also look at what is not
   covered at all: beams, flames (cone), barrier and grenade projectiles.
6. Explosions. They hit through their own path: an explosion (RE::Explosion, from a BGSExplosion record) finds the actors
   in its radius and builds its own HitData, so neither the melee hook (37673+0x3C0) nor the projectile contact hook sees
   it; a fireball that a dodge lets pass still explodes where it lands and its blast still hits. To study in IDA: where an
   explosion applies its hit / damage to an actor, so i-frames can drop it there; how a dodged projectile's own explosion
   should behave (no blast, blast that ignores the dodger, or a normal blast that the i-frames cover); whether blast
   knockdown / stagger (explosion force) is applied separately from the damage and needs its own guard.
7. Data layers for designers and modpacks (design agreed in chat 2026-10-21): (a) runtime modifiers from active magic
   effects by keyword, Reflyem-style (sum of magnitudes; i-frame add/mult, cost mult, distance mult, chain add, gates
   such as RDF_Allow_DodgeFromStagger / RDF_Block_Dodge), with a log line explaining each resolved value; (b) profile
   files with OAR-like conditions and priorities (cost, cooldown, chain, gates, window scale, distance); (c) a clip
   descriptor annotation at 0.0 (versioned, e.g. RDF_Clip.v1|dur=..|dist=..|motion=root) and a root-motion mode that
   follows the clip's own animmotion. Per actor / NPCs later.
8. Exit speed from the real locomotion speed (movement type run/walk speed x SpeedMult, sprint) instead of fRunSpeed.
9. Perfect dodge: a hit inside the first part of the i-frames (or measured from the attacker's swing start) sends
   graph events to both actors and a hook for slow motion; perk / keyword gated through item 7.
10. Lock-on style option: Souls-like FaceMotion rolls under lock-on (turn to the path, roll forward, turn back to the
    target in the recovery) next to today's directional clips.
