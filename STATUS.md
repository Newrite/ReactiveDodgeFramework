# Status (2026-10-19, evening)

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
* Arm stance layer: during steps the arms keep the current weapon's stance (user: looks good).
* First external clip set as an OAR mod (`AI - RDF Dodge Set - Greatsword`, `tools/build_clip_set.py`): 134/134
  dodges played it with its own windows (user: looks and feels good).
* The "press -> twitch, no dodge" bug: both causes fixed (start into the state being left; zero-length streaming clip
  firing a clip-end exit). 0/72 failures after the fix.

## Built and deployed, waiting for an in-game check
* Projectile i-frames (`bProjectileIFrames`): arrows and spell missiles fly through during the window.
* Gamepad (right-stick click) and analog magnitudes.
* Lifecycle: cell change, death, menus mid-dodge.

## Known limitations
* Fixture animations are TUDM's clips (local only, not redistributable); visual quality is bounded by them, e.g. they
  settle into a full stance at the end. A proper clip set is needed for final visuals.
* First person: dodge is refused (no 1P graph patch).
* Beams and flame streams are not covered by projectile i-frames; left-hand and dual attacks from a dodge are not
  implemented.
* In this modlist V also triggers a bash when a weapon is drawn (another mod's binding); choose a free key if that
  matters.

## Next actions
1. In game: run into running after dodges (speed floor), shoot arrows/spells at the player (i-frames), dodge -> attack
   and attack -> dodge, block from a dodge; send the log.
2. Remove the dev tracing of graph events once the above is confirmed (it is behind `bDevLog` already).
3. A proper animation set annotated with `tools/annotate_clips.py`.
