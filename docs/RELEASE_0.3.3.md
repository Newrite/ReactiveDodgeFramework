# Reactive Dodge Framework 0.3.3 (SE 1.5.97)

Everything in 0.3.2 is included; this release adds one setting.

- **`[Input] bBlockDodgeInLootMenu`** (default 1) decides whether the QuickLoot loot window holds the
  dodge back. QuickLoot IE / EE do not pause the game, so the dodge key used to work while the loot
  window was open; 0.3.2 made that window a blocking menu like the rest. Set it to 0 -- or turn off
  "Block dodge in the loot menu" on the Controls page of the FLICK menu (F7) -- and dodging works with
  the loot window open again. Every other menu in the 0.3.2 gate is unchanged: an open container, a
  dialogue, F7 itself, the console, the wheel, a paused game.

## Limits, stated plainly

- The setting only covers QuickLoot's `LootMenuIE` / `LootMenu`. Other menu mods are still held back
  by the 0.3.2 gate and have no toggle.
- The open 0.3.0 issue stands: a separate mod can emit projectile impact VFX/sound before RDF filters
  the contact. This release does not claim to suppress that external feedback.
- The left-hand cast / block change from 0.3.1 and the 0.3.2 menu gate still wait for their in-game
  check.

## Validation

129 tests pass; the SE 1.5.97 DLL builds without project warnings. The setting is read at load and on
"Reload ini" / a live change in the FLICK menu; the menu translations (English, Russian) and
`docs/ru/Settings.md` are regenerated from one table.

No behavior-graph changes: no new events or variables, so an existing Pandora or Nemesis output keeps
working and only the DLL needs replacing.

Install the main no-clips FOMOD and supply compatible dodge animations separately. Third-party
animation fixtures are intentionally excluded. The release contains RDF only; Enhanced Combat
Behavior is maintained separately and is not required by RDF.
