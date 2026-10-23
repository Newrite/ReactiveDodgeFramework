# CastOn rules: design (phase 1 built 2026-10-24, not run in game)

Spells cast because of a dodge (CastOnDodge, CastOnPerfectDodge, more triggers later), described by rules a combat
designer or animator edits in game, in an OAR-like window: conditions, who casts on whom, what it costs, when it fires.

## 1. What exists to build on

**Reflyem** (`S:\Programming\Reflyem`, `Core.cpp` `cast_on_handle`, `cast`): pairs of form lists, a keyword (or an
8-slot "meta" form list) per spell. The meta list's slots are keywords for:
- conditions on the caster, the source and the target;
- "only while power attacking";
- blocking enchantment or spell projectiles;
- ammo and potion keywords.

Spell keywords switch two behaviours:
- `cast_on_caster_swap`: the reverse cast, caster and target swapped;
- `cast_on_is_cost`: pay the cost. The cost is `CalculateMagickaCost`, refused when it is above the current magicka,
  then damaged from the AV.

The cast itself is `CastSpellImmediate` on the instant caster: a self spell targets the caster, anything else the target,
and a keyword can forbid target casts. A richer flag set (`CastOnMetaFlags`) was started but never finished. Kept from it:
- the reverse cast;
- conditions per participant;
- optional cost.

Not kept:
- index-aligned form lists, which are unreadable and fragile;
- the fixed 8-slot meta list.

**Engine, IDA (SkyrimSE 1.5.97):**
- `MagicCaster::CastSpellImmediate` (vfunc 1, 0x140559D40 -> 0x14054C5F0 -> 0x14054CD10):
  - no `CheckCast`, no `SpellCast`, no cost at all;
  - an aimed fire-and-forget spell launches its projectile;
  - everything else applies its effects (perk entry points included);
  - aimed concentration spells get the caster's concentration bookkeeping.
  - It is what Papyrus `Spell.Cast` and every CastOn mod use.
- `ActorMagicCaster::CheckCast` (vfunc 0xA, 0x1405427B0): the engine's "can cast" check.
  - Cost = `MagicItem::CalculateCost` (0x140101A30; skill and perk entry points, so cost perks apply), times the dual-cast
    multiplier (0x1403C1720) when dual.
  - It is checked against the AV returned by `GetActorValueForCost` (0x140556780), with god mode honoured.
  - Per-effect checks give the `CannotCastReason`.
- `ActorMagicCaster::SpellCast` (vfunc 9, 0x140542470): the end of a real cast.
  - Deducts the same cost through `ActorValueOwner` vfunc 6 (`ModActorValue(kDamage)`), but only for fire-and-forget
    spells with zero charge time (gate 0x140556990). Charged spells are drained during the charge, in `Update`
   .
  - Removes the effects a cast breaks (invisibility).
  - Applies the player's imagespace.
  - Sends `TESSpellCastEvent` (Papyrus `OnSpellCast`, other mods).
- **Cost-changing mods** use perks (inside `CalculateCost`), hooks on `CalculateCost` / `CheckCast` / the caster vfuncs, or
  `ModActorValue` hooks. A path that calls `CheckCast` and `CalculateCost` and deducts through `ModActorValue` meets all
  of them.

**RDF:**
- `forms` (text -> form, null-safe, cached, a picker catalog per form type).
- `Settings::Observe`.
- The keyword walk: actor, race, worn items, active effects.
- Graph events with payloads (annotation `Name.payload` arrives as tag `Name` + payload, as PIE's `Pie` / `@SGVI|...`).
- FUCK windows (`IWindow`, `RegisterWindow`).
- The perfect-dodge detection, and the `DodgeAgent` timeline.

## 2. The rule

One rule: when it may fire, the conditions, what it casts, how it pays, when it fires.

| Field | Meaning |
|---|---|
| name, enabled, priority | order of evaluation; a rule may stop the ones after it (`exclusive`) |
| trigger | `Dodge` (a dodge started), `PerfectDodge` (a hit dodged inside the perfect window). Later: `DodgeEnd`, `Chain`, `StaggerEscape`, `AttackFromDodge` |
| conditions | an AND / OR tree with NOT, as OAR (section 3) |
| casts | one or more spells, each with caster, target and effectiveness (below) |
| cost | `Free`, `Engine`, `Custom` (section 4) |
| timing | seconds after the trigger; past the dodge's end = at its end. A clip annotation overrides it (section 5) |
| limits | cooldown (s), at most once per dodge / per chain, chance % |

A cast entry:
- the spell (any `MagicItem`: spell, scroll, enchantment, potion; named as in `forms`);
- the **caster**: dodger, attacker or combat target;
- the **target**: self, dodger, attacker, combat target, or the aim. Reflyem's reverse cast is just caster = attacker,
  target = dodger;
- effectiveness (multiplier), magnitude override (0 = none), and "no hit effect art".

## 3. Conditions, and keeping them cheap

**Subjects.** Each condition names the participant it reads: dodger, attacker (on PerfectDodge), or combat target.

**Two kinds of condition:**
- **Native, typed, fast:**
  - keyword (on the actor, race, worn items, effects) and magic-effect keyword with a magnitude comparison;
  - perk, race, faction (rank), equipped type per hand, spell in hand / casting;
  - actor value (current, max, percent) compared to a number or to another AV;
  - in combat, sneaking, sprinting, first person, player;
  - dodge facts: layer (root / 1hm / bow / magic), direction (clip 0..8), chain index, out of a stagger, perfect, i-frame
    time;
  - random chance.
- **Any vanilla condition function** (the CK's ~400): built as a real `TESConditionItem` and evaluated by the engine
  (`TESCondition::IsTrue`), with its function list and parameter types read from the game's own command table at runtime
  (`SCRIPT_FUNCTION`), so nothing is hardcoded. Designers already know them from the CK.

**Speed.** Rules are compiled when they load or the menu commits an edit:
- forms are resolved;
- each condition becomes a small struct with a function pointer;
- groups become index ranges.

Evaluation does no string work and no lookups. It only runs on a trigger (a few times a second at most), short-circuits,
and puts the cheap checks before the costly ones within an AND. The keyword walk over worn items and effects, the
costly part, runs once per subject and trigger for all the rules' keywords together (one bitset).

With `bDevLog` on, the dev log records the evaluation time per trigger. Target: well under 50 µs for a few dozen rules.

## 4. Cost: three modes

- **Free:** `CastSpellImmediate` only (`Spell.Cast` behaviour).
- **Engine** (the vanilla path, for cost mods): the caster's own `CheckCast` (refused with its `CannotCastReason`, e.g.
  not enough magicka), then `CalculateCost` (× dual multiplier if asked) deducted through `ModActorValue(kDamage, AV for
  cost)`, then `CastSpellImmediate`. The deduction is done by us for every spell type: the engine's `SpellCast` skips
  charged spells, which would make them free. Optional "like a real cast":
  - `TESSpellCastEvent`;
  - breaking invisibility (the `SpellCast` side effects).
- **Custom** (independent of other mods):
  - the cost: AV (magicka / stamina / health), then a flat amount, a share of the spell's base cost and a share of the
    max AV, times a multiplier;
  - when it can't be paid: skip, cast anyway, or cast with the effectiveness scaled down to what was paid.

On refusal, the player optionally gets the HUD meter flash and the fail sound.

Constant effects (abilities) are refused.

**Concentration spells** run for the cast's `duration` (seconds, default 1, up to 60). Why that is needed (IDA,
2026-10-24): `CastSpellImmediate` leaves the instant caster in state 6 with the spell, and nothing releases it.
- `Character::Update` (0x140632E50) updates all four casters every frame.
- `ActorMagicCaster::Update` (0x140541FC0) keeps a concentration spell going (`sub_14054C1A0`) and drains its cost per
  second, unless the caster's `kSkipCheckCast` flag (bit 1 of +0xF8) is set.

A second finding came from the first in-game test (2026-10-24): one burst, then nothing. `MagicCaster::Fire` (0x14054C950)
runs on every interval of a concentration cast and asks the caster's `CheckCast` first; when that refuses, it calls
`InterruptCast`. For the instant caster it refuses. `kSkipCheckCast` makes `CheckCast` pass at once.

So CastOn:
- sets `kSkipCheckCast` on every concentration cast it starts (`SetSkipCheckCast`, vfunc 0x13, 0x140543620);
- keeps a module-wide list of the concentration casts it started, advanced from the per-frame tick on game time;
- ends each with `MagicCaster::InterruptCast` (ID 33630: `InterruptCastImpl`, which also clears `kSkipCheckCast`, then
  state 9 and `SpellCast(false)`, so nothing is charged);
- forgets a cast whose caster died, unloaded, or went on to another spell (a load or a new game forgets them all).

The flag also turns off `ActorMagicCaster::Update`'s per-second drain. The cost by mode:
- **Engine:** nothing up front. CastOn drains it per frame: `CalculateCost` (the cost per second, perks and cost mods
  included) × dt, through `ModActorValue`, clamped to what is left, free in god mode. Refused at 0, stopped at 0.
- **Free:** no drain.
- **Custom:** the spell share is the cost per second × the duration, paid up front, then no drain.

## 5. Timing and annotations

- A rule's timing is seconds after its trigger: Dodge counts from the dodge's start, PerfectDodge from the dodged hit.
  A time past the dodge's end fires at the end (chain or exit), as asked.
- **Clip override:** the annotation `RDF_CastOnDodge` / `RDF_CastOnPerfectDodge` in the dodge clip is the moment for the
  rules of that trigger. `RDF_CastOnDodge.<tag>` sets it only for the rules carrying that tag. A PerfectDodge rule whose
  annotation came before the dodged hit fires at the hit.
- Both events are registered in every patched file (root, 1hm, bow, magic, 3P and 1P), like the other annotations.
  `ANIMATION_CONTRACT.md` and `annotate_clips.py` get them.

## 6. Files

- Rules in JSON, one file per author:
  - `SKSE/Plugins/ReactiveDodgeFramework/CastOn/*.json` (a mod ships its own file);
  - `*.user.json` beside it shadows it, OAR's `user.json` way. The menu writes only `.user.json`, so a mod updates without
    losing the user's edits.
- Forms are named as everywhere in RDF (`Plugin.esp|0xID` or EditorID).
- The JSON library is parsed without exceptions (gameplay code throws nothing): rapidjson, or nlohmann with
  `allow_exceptions = false` and checked access. Malformed files are logged and skipped.

## 7. The window

A FUCK `IWindow`, "CastOn rules", opened from the Rules page (button) and from the sidebar:
- left: the rules, with the file they come from, the enabled tick and a "user" mark, as in OAR;
- right: the selected rule:
  - trigger, timing, limits, cost;
  - casts (spell picker from `forms::CatalogOf(Spell)`, caster / target);
  - the condition tree: add condition / add group, AND / OR, NOT, parameters with pickers, and **a live result per
    condition for the player** (green / red as in OAR). It is re-evaluated a few times a second on the main thread, only
    while the window is open.
- buttons: Save (user file), Reload, Delete user file, "Fire now" (runs the rule on the player, for testing).

## 8. Phases

1. Model, JSON load / save, compiler, evaluator with the native conditions, executor (three cost modes, caster / target),
   triggers Dodge and PerfectDodge, timing and annotations (patch events), dev log. Rules can already be written by hand.
2. The window.
3. Vanilla condition functions (`TESConditionItem` from the command table).
4. More triggers.

## 9. Open questions

- Is JSON with `.user.json` shadows right, or should rules live in the ini?
- Which triggers in phase 1 besides Dodge and PerfectDodge (DodgeEnd? StaggerEscape?)
- Skill XP on Engine-cost casts (the real cast awards it elsewhere; not yet traced in IDA).

## 10. Phase 1 as built (2026-10-24)

- **Model and files.** `src/core/CastOnRules` (engine-free, unit-tested): the rule structs, read and written with
  glaze. Options:
  - reading: JSONC comments allowed, an unknown key is an error with `glz::format_error` context;
  - writing: prettified. Every field is written: glaze's `skip_default_members` compares with the type's zero, so a
    `false` / 0 would come back as its non-zero default.

  Enums are written by name. `Validate` drops a rule with a kind-specific problem (a condition without its form, an
  unknown layer...) and names the reason; the file's other rules load.
- **In game.** `src/game/CastOn`:
  - `Load` at kDataLoaded: the files, `.user.json` winning; forms resolved; conditions compiled into flat nodes with the
    cheap ones first in an AND. A missing form or spell, or an ability, drops the rule with a log line.
  - `Scheduler`, one per `DodgeAgent`: trigger, limits, pending casts, annotations.
  - The keyword walk runs once per (actor, keyword) and trigger. With `bDevLog` the evaluation time is logged per trigger.
- **Cost.**
  - Engine: `CalculateCost` (ID 11213, 0x140101A30, the function `CheckCast` and `SpellCast` use), times
    `fMagicDualCastingCostMult` and plus `fMagicDualCastingCostBase` when dual. Taken from the AV that
    `GetActorValueForCost` (ID 33817) names, through `ModActorValue(kDamage)`. God mode is free.
  - `realCast` adds what `SpellCast` does besides paying: `RemoveMagicEffectsDueToAction` (ID 37864), keeping an
    invisibility / ethereal effect the spell itself carries, then `TESSpellCastEvent`.
  - A refusal flashes the HUD meter (ID 51907) for the player. No fail sound yet.
- **Triggers.**
  - Dodge: after the start was sent.
  - PerfectDodge: in `HandleDodgedHits`.
  - A chain ends the previous dodge's waits (they fire); a fresh dodge starts a new chain for `oncePerChain`; a
    rolled-back start drops its Dodge casts.
- **Missing participant.** A condition on a participant that is absent fails, negated or not. A cast without its caster
  or target is skipped and logged.
- **Example file.** `dist/CastOn/RDF_Examples.json` (all rules disabled), deployed to
  `SKSE/Plugins/ReactiveDodgeFramework/CastOn/`.
- **Switch.** `[CastOn] bCastOn` (Rules page).
- **Not yet:**
  - the window (phase 2);
  - vanilla condition functions (phase 3);
  - a reload without restarting the game;
  - the fail sound;
  - skill XP for Engine casts.

## 11. Presets, templates, schema (2026-10-24)

The format stays JSON (user decision). The rest is why, and what was added instead of a richer config language.

Why not Pkl, CUE, Nickel, Dhall, Jsonnet, KDL or Lua:
- **The menu writes the rules back.** The evaluate-to-data languages can't be written back from data.
- **Embedding in an SKSE DLL.** Each needs a JVM, Go, Rust, Haskell, a library that throws, or a scripting runtime.
- **The evaluator compiles the rules anyway**, so the source language buys no speed.
- **JSON is what those languages output**, so a designer can still author in one of them and ship JSON.

What was added:
- **Presets** (OAR's `conditionPresets`). A file's `"presets"` holds named condition lists. `{ "kind": "Preset", "text":
  "<name>" }` stands for an AND of the list; presets may nest.
  - Every file's presets are visible to every rule (a modpack's shared presets file). When two files name the same
    preset, the first file by name wins and a warning is logged.
  - The model keeps the Preset condition, and `Load` expands it before compiling (`ExpandPresets`), so the menu can show
    and write the reference.
  - An unknown preset, or presets naming each other in a circle, drops the rule with the reason.
- **Templates.** A file's `"templates"` holds named partial rules; a rule (or a template) has `"extends": "<name>"`. The
  merge happens on the JSON before the typed read, so a value equal to its default still overrides:
  - the rule's keys replace the template's;
  - the `cost` / `timing` / `limits` objects merge key by key;
  - `conditions` add up, the template's first.

  Known limit: a rule read from a template holds the merged values, so `WriteRules` writes it whole and without
  `extends`. The menu will need a diff against the template to keep the link.
- **Schema.** `JsonSchema()` (`glz::write_json_schema`) generates `dist/CastOn/rdf_caston.schema.json` via
  `rdf_tests --write-schema <file>`, with field descriptions. The schema ships beside the rules (the loader skips
  `*.schema.json`), and a file names it with `"$schema": "./rdf_caston.schema.json"` (VS Code then checks it and
  completes the names). Its `$defs` keys are glaze's C++ type names: valid, if ugly.
- **Strictness.** Unknown top-level keys are errors, like unknown keys inside rules.
- **The examples** use both: the presets `HoldsASpell` / `AttackerAlive`, and the template `PerfectRetort` that two
  rules extend.

## 12. Phase 2: the window (built 2026-10-24, not run in game)

**Where it is.**
- `src/game/CastOnEditor`: a FLICK `IWindow` ("CastOnRules"), opened by the button at the top of the Rules page.
- `src/game/MenuWidgets`: the form picker, moved out of Menu.cpp and shared with the settings pages. Its `compact` mode is
  for table cells: a status icon, the pick and clear buttons, then the box, with the resolution in its tooltip.

**What it shows.**
- **Left:** each rule file as the game picks it (`<name>.user.json` over `<name>.json`), with its load errors. Under each
  file, its rules (an enable tick, then name, trigger and priority) and its presets. Buttons add a rule or a preset.
- **Right, for the selected rule:**
  - name, enabled, trigger, priority, exclusive (a note when the rule came from a template);
  - the condition tree as one table in depth-first order: live result, NOT, kind (indented by depth), whose, what (a form
    picker, a list for Layer / Direction / Equipped / Preset, a text box for an actor value), how (hand, mode, compare and
    value, chance), and move up / down / delete. A group row adds a condition or a group;
  - the casts table (spell picker, caster, target, effectiveness, magnitude, duration, no hit art);
  - cost by mode, timing, limits;
  - `Validate`'s problems in red;
  - Fire now, Duplicate, Delete.
- **Right, for a selected preset:** rename, its conditions with live results, delete.

**Toolbar.**
- **Save:** `SaveUserDocument` writes `<name>.user.json`, then `ReloadInGame` (`Load` as a main-thread task).
- **Reload files:** drops unsaved edits.
- **Apply in game.**
- **Delete user file:** the mod's own file comes back.
- **New file:** a user-only file.

**Live results.**
- `WatchRule` hands the selected rule (or a preset as a rule) and every file's presets to the module. While the window
  draws, `PollLive` posts `EvaluateLive` as an SKSE task at most every 0.25 s. Tasks run while the menu pauses the game;
  the frame tick does not.
- Each row of the tree is evaluated on its own, for the player (the dodger), with its combat target as the attacker and
  the combat target. A Preset row stands for its whole expansion. Groups are folded from their children (an Or with a true
  child, an And with a false one, decides even with unknown children).
- -1 (grey ?) = no such participant, or a missing form or preset.

**Fire now.** `FireNow` compiles the rule like `Load` (`CompileRule`, now shared) and runs `Execute` for the player
context; whether the conditions hold now is only logged.

**Known limits.**
- A rule built from a template is saved whole, without `extends` (section 11).
- No vanilla condition functions yet (phase 3).
- Enum names are shown as the JSON writes them (untranslated).
- How the window behaves in game (cursor, pause, Esc) is not tried yet.

## 13. Switched-off conditions, comments, drag and drop (2026-10-24)

- **`disabled`** (a condition key, default false): the condition is as if not written, OAR's way. The game drops it
  (`DropDisabled`) before and after the presets are expanded, so a switched-off reference to a missing preset is no
  error, and a preset's switched-off conditions go too. A group left with nothing inside is dropped as well: it holds.
  `Validate` skips a switched-off condition, so a half-made one does not stop the rule. Live rows: -2, nothing drawn; a
  group decides on the rest (all off: it holds).
- **`comment`**: a note for people. The node's line shows it after `//`; hovering the line shows it whole.
- **Editor.** A tick before each node switches it on and off (the line greys). A node dragged by its line onto another
  node of the same list moves there: before it going up, after it going down. Moves into or out of a group are not
  supported (copy and paste do that). The section notes moved from a "(?)" on a line of its own to beside the add
  buttons.

## 14. Phase 3: the game's condition functions, and effects that count (2026-10-24, not run in game)

**How OAR does it.** OAR (S:\Programming\BethESPLibs\OpenAnimationReplacer) uses no engine condition item: its ~120
conditions are its own code. Its HasMagicEffect / HasMagicEffectWithKeyword have an "Active effects only" box that skips
effects flagged kInactive.

**What the engine does (IDA, SE 1.5.97).**
- `TESConditionItem::IsTrue` (29090 = 0x1404454C0) picks the subject by `object` (+0x31: 0 self, 1 target with the two
  swapped, 2 a reference...). It swaps again on flag 0x10, and fills an empty reference first parameter with the target.
  It calls the command table's condition function for the item's function index (+0x18):
  `fn(subject, params[0] +0x20, params[1] +0x28, double& result)`. A reference function (+0x20 of the command) is skipped
  without a subject. The result is compared (`0x140444830`, opcode = flags >> 5) with the float at +0x08, or with the
  global's value (+0x34 of it) when flag 0x04 is set.
- The command table: `SCRIPT_FUNCTION` array, 501789 = 0x141DB8910, 0x50 bytes per command, the index = opcode - 0x1000
  (0 GetWantBlocking, 14 GetActorValue, 214 HasMagicEffect, 699 HasMagicEffectKeyword). Names at +0x00, parameters at
  +0x28 (0x10 each: name, SCRIPT_PARAM_TYPE +0x08, optional +0x0C), the condition function at +0x40. CommonLib's layout
  matches.
- **The effect trap.** HasMagicEffect (0x1402DA780) → `MagicTarget::HasMagicEffect` (0x1405530D0) walks the effect list
  and compares the base effect only: no flag is read. An effect switched off by its spell's conditions (`ActiveEffect`
  flags +0x7C, kInactive 0x8000, set by vfunc 5 at 0x14053E636) or dispelled (0x40000) still counts.

**What CastOn does.**
- **`Vanilla`.** `text` holds the function's name, `args` its parameters as text, and `compare` / `value` the
  comparison (None: the result is not 0). It is run on `subject`. The list is read from the game's table at runtime
  (`game/VanillaConditions`); a function whose parameter cannot be written (an alias, package data, a script variable)
  is not offered.
  - Parameters by type:
    - a form, of the parameter's type when the CK takes one type;
    - a reference: a placed one, or Dodger / Attacker / CombatTarget, filled in per check. An empty first reference is
      the other participant, as the engine fills in the target: for the dodger, the attacker, else the combat target;
      for anyone else, the dodger;
    - an actor value by name;
    - a whole number or a float;
    - X / Y / Z; Male / Female; Left / Right / Voice / Instant.
  - We call the function directly, as IsTrue does, and compare it ourselves.
  - HasMagicEffect and HasMagicEffectKeyword are answered by CastOn itself, with the active-only rule below.
- **`HasMagicEffect`** (form: a magic effect): the actor is under it. With a comparison, the summed magnitudes are
  compared.
- **`inactiveEffects`** (default false) covers HasKeyword's effect part, HasEffectKeyword, HasMagicEffect and Vanilla
  HasMagicEffect[Keyword]. It decides whether effects that are kInactive or kDispelled count too. By default they do
  not, which is OAR's "active effects only".
- **Editor.** The function comes from a filtered list, and the game's help line, if it has one, is its tooltip. Each
  parameter gets a box by its kind: a form picker, a list for X / Y / Z or sex, or text with a hint. Effect conditions
  get an "Inactive effects too" tick.
- Example: the preset `Seasoned` in RDF_Examples.json (Vanilla GetLevel >= 10).
