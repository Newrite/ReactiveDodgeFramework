# Reactive Dodge Framework 0.3.1 (SE 1.5.97)

- **A time slowdown from another mod no longer breaks a dodge.** The plugin scaled its own dodge
  clock by `player / world` on top of what the engine already applies to the player's frame delta,
  so any world-only slowdown ran the dodge `1 / world` times too fast. The vanilla Quick Reflexes
  perk (world x0.05, player x1.0) finished a 0.7 s dodge in three frames and left the character in a
  sprint state. Verified in game: with `fPerfectSlowMotion = 0.30` and
  `fPerfectSlowMotionPlayer = 0.85` a dodge now takes its normal 0.68-0.72 s.
- **The perfect-dodge slow-motion split now works as documented.** `fPerfectSlowMotion` (the world)
  and `fPerfectSlowMotionPlayer` (the player) are two independent engine values; the dodge timeline
  and its clip follow the player's speed, so they stay in step whatever the split. Previously the
  split only behaved when both values were equal.
- **A spell in the left hand is no longer a block.** The game's left button casts with that hand
  (the engine decides this before any attack or block handling), but RDF took the press for a block:
  a dodge held it back and raised `blockStart` on its way out, and an attack out of the dodge came
  out as a bash. The test is now the engine's own, `wantBlocking` included, so a block-button mod
  (Input Support, CGO, dual-wield block patches) keeps its block exactly as before. The press is not
  shaped into an attack either: RDF's attack is the right hand, which swung the weapon in the other
  hand instead of casting.
- **Dialogue menus no longer leak dodge presses**: a menu that does not pause the game (dialogue) is
  treated like a pause for input, and a release after closing a menu cannot turn an earlier press
  into a tap dodge.
- NPC dodges tick on the world's clock instead of the player's (they were ticked from the player's
  update with a player-scaled delta).
- The dev trace carries `time world= player= p/w=`, so a slowdown in effect is named in the log
  instead of having to be inferred.

## Limits, stated plainly

- A left-hand cast asked for *during* a dodge does not happen: the engine consumes that press in the
  frame it arrives and the plugin has no left-hand action to replay. Press the button after the dodge.
  "Dodge -> cast" needs the magic caster's own charge / release path, which is not in this release.
- With a spell in the left hand and nothing blockable, RDF's block handling is unchanged for an
  empty-handed or spell-only setup; that case has not been reworked.
- The open 0.3.0 issue stands: a separate mod can emit projectile impact VFX/sound before RDF filters
  the contact. This release does not claim to suppress that external feedback.

## Validation

129 tests pass; the SE 1.5.97 DLL builds without project warnings. The time-slowdown fix was
confirmed in game from the user's dev log; the left-hand cast / block change is built and deployed
and still waiting for its in-game check.

No behavior-graph changes: no new events or variables, so an existing Pandora or Nemesis output keeps
working and only the DLL needs replacing.

Install the main no-clips FOMOD and supply compatible dodge animations separately. Third-party
animation fixtures are intentionally excluded. The release contains RDF only; Enhanced Combat
Behavior is maintained separately and is not required by RDF.
