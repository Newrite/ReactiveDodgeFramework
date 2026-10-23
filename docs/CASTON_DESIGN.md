# CastOn rules: design (draft for review, 2026-10-24)

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

Concentration spells cast through the instant caster fire once (the engine's behaviour), noted in the menu. Constant
effects (abilities) are refused.

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
