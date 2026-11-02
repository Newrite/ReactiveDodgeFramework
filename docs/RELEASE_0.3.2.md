# Reactive Dodge Framework 0.3.2 (SE 1.5.97)

Everything in 0.3.1 is included; this release adds the menu gate below.

- **A dodge no longer fires in a menu.** The plugin only knew about the game's pausing menus plus the
  console and dialogue, so every menu a mod puts over a running world let the dodge key through: the
  FLICK menu (F7), where this plugin's own settings are edited, QuickLoot's loot window (QuickLoot IE /
  EE, live in the author's order) and Wheeler's wheel. The input gate now asks `UI::GameIsPaused()`
  first and then names the menus explicitly -- the same list TrueFlasksNG uses, so the answer does not
  depend on which menu raises the engine's paused-by-a-menu counter: console, dialogue, tween, magic,
  inventory, lockpicking, race menu, stats, loading, fader, favorites, sleep/wait, journal, barter,
  main menu, book, container, gift, message box, training, map, tutorial, level up, credits, crafting,
  cursor, plus `LootMenuIE` / `LootMenu`, FLICK's own menu and Wheeler's `IsWheelerOpen` (only while
  `wheeler.dll` is loaded; that DLL is not loaded in the author's order, so the branch is inert there).
- **Nothing stale survives a menu.** Held keys and the thumbstick are still tracked while a menu is
  open and only command queueing stops, so a movement or block key released inside a menu no longer
  stays "held" for the next dodge, and a press made in a menu cannot become the first tap of a double
  tap dodge.
- The tick is unchanged: a pausing menu still freezes it, and a dodge already running continues
  normally when a non-pausing menu (dialogue, loot window, F7) is opened over it.

## Limits, stated plainly

- The open 0.3.0 issue stands: a separate mod can emit projectile impact VFX/sound before RDF filters
  the contact. This release does not claim to suppress that external feedback.
- The menu list is a list: a menu added by some other mod is caught only if it raises the engine's
  pause counter or is named here. `LootMenuIE` / `LootMenu`, FLICK and Wheeler are the ones known from
  the author's modlist.
- The left-hand cast / block change from 0.3.1 is still waiting for its in-game check.

## Validation

129 tests pass; the SE 1.5.97 DLL builds without project warnings. The menu gate is mechanical (the
engine's own menu queries) and is not verified in game yet: open F7 settings / a loot window / a
dialogue / the console and press the dodge key, then start a dodge and open a loot window over it.

No behavior-graph changes: no new events or variables, so an existing Pandora or Nemesis output keeps
working and only the DLL needs replacing.

Install the main no-clips FOMOD and supply compatible dodge animations separately. Third-party
animation fixtures are intentionally excluded. The release contains RDF only; Enhanced Combat
Behavior is maintained separately and is not required by RDF.
