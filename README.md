# Reactive Dodge Framework (RDF)

An SKSE plugin + Pandora behavior patch for **Skyrim SE 1.5.97** that turns the dodge into a
player-controlled movement action instead of an animation the player waits out:

Current release: **0.3.0**, Skyrim SE **1.5.97**. See
[release notes](docs/RELEASE_0.3.0.md) for area i-frame fixes, NPC API v2 and the
optional Enhanced Combat Behavior integration. Projectile impact feedback from
other mods remains an open compatibility issue.

* the plugin owns the character's displacement during the dodge (one authoritative source per frame,
  Havok character controller still handles collision) — see `ARCHITECTURE.md`;
* the direction can be steered continuously while dodging (analog or keyboard, camera-relative,
  rate-limited turning per phase);
* presses are buffered on a game-time clock (pauses/menus freeze it), consumed exactly once, and
  cancel windows decide when a dodge may chain, turn into an attack/block, or hand over to running;
* the behavior graph gets real blended transitions: into the dodge from any root state (including
  attacks), dodge→dodge (A/B ping-pong), dodge→locomotion, and the vanilla stagger/ragdoll/death exits.

## Install (MO2, STB profile)

1. Disable nothing yet; the mod folder `mods\AI - Reactive Dodge Framework` is already in the instance.
2. Enable it in MO2 (it loads last; it has no ESP).
3. Run Pandora, tick **rdf** (Reactive Dodge Framework), and generate. Pandora must output to the usual
   `Pandora` mod.
4. Optionally set `bDevLog=1` in `SKSE\Plugins\ReactiveDodgeFramework.ini` for the first sessions.

Dodge key: V / right-stick click (ini `[Input]`). With `bSuppressLegacyDodge=1` (default) the
plugin sets STB's legacy dodge globals `UDDodgeKey`/`UDSprintDodge` to 0 on load so only RDF reacts.
This value is stored in saves; to go back, disable RDF and run `set UDDodgeKey to 56` in the console.

## Animations

The bundled `meshes\actors\character\animations\RDF\rdf_*.hkx` in the local mod folder are copies of
The Ultimate Dodge Mod clips installed by `tools/install_fixtures.py` **for local testing only**; they
are not part of the source and must not be redistributed. Any set that follows `ANIMATION_CONTRACT.md`
replaces them. RDF ignores clip root motion — direction and distance come from the plugin.

## Build

```
xmake build -y                                   # DLL + tests (CommonLib: S:/Programming/CommonLibSSE-NG)
build\windows\x64\releasedbg\rdf_tests.exe       # 53 deterministic core tests
python tools\gen_rdf_patch.py                    # regenerate the Pandora patch
python tools\stage_pandora.py --label rdf --rdf behavior/Pandora_Engine/mod/rdf
python tools\validate_rdf.py                     # 127 checks on the generated graphs
python tools\annotate_clips.py                   # write RDF_* window annotations into the clips
```

## Documents

| File | Contents |
|---|---|
| `ARCHITECTURE.md` | motion authority decision, components, frame order, threading |
| `REVERSE_ENGINEERING.md` | engine addresses/IDs with evidence and confidence |
| `BEHAVIOR_INTEGRATION.md` | what the Pandora patch adds, validation, conflicts |
| `ANIMATION_CONTRACT.md` | clip files, index order, events/variables, timing |
| `TESTING.md` | verification report: automated / static / in-game (what was verified, what is waiting) |
| `STATUS.md`, `JOURNAL.md` | current state and decision log |
| `docs/research/` | modlist audit, public implementations + Elden Ring notes, behavior pipeline |

## Behavior engines and the FOMOD
The behavior patch ships for both engines, generated from one model:
- Pandora: `behavior/Pandora_Engine/mod/rdf` (`tools/gen_rdf_patch.py`; checked by staging Pandora and `tools/validate_rdf.py`).
- Nemesis: `behavior/Nemesis_Engine/mod/rdfm` (`tools/gen_rdf_nemesis.py`, converted from the Pandora patch). Edited nodes
  carry Nemesis' own original text (the vanilla graph in Nemesis' line layout, confirmed against the Nemesis mods installed
  in the modlist); `tools/check_rdf_nemesis.py` applies the patch to the vanilla graphs offline, packs them with serde-hkx
  and checks them with hkxcheck.
`tools/build_fomod.py` packs `ReactiveDodgeFramework-<version>.zip`: core (DLL, ini, translations, clips), then a choice of
Pandora or Nemesis, a steering preset (default / Committed Dodge) and the optional greatsword OAR set. After installing,
run the chosen engine with "Reactive Dodge Framework" ticked.

## In-game settings menu (FLICK)
With [FLICK](https://www.nexusmods.com/skyrimspecialedition/mods/181603) installed, its menu (F7 by default) gets a
"Reactive Dodge" group with seven pages: controls (the dodge button for keyboard/mouse and for gamepad, input buffer), timing and windows, motion, steering and facing, animation, cost and rules, debug.
Every change applies live (a running dodge keeps the tuning it began with; the next one uses the new values).
"Save to ini" writes the values into `SKSE/Plugins/ReactiveDodgeFramework.ini` in place (comments and order kept;
under MO2 that is the winning copy, e.g. a preset mod's), "Reload ini" discards unsaved edits, "Page defaults" resets
one page. The i-frame hooks and compatibility switches stay ini-only (they apply at game start).
Labels and descriptions: `Interface/Translations/ReactiveDodgeFramework_<LANGUAGE>.txt` (English and Russian, generated
by `tools/gen_menu_translations.py`). Without FLICK the plugin works as before.

## API: dodge any actor
The player's dodge (states, clips, annotation windows, chains, exits, cost, i-frames, perfect dodge) runs on any
humanoid actor; only the decision to dodge is the caller's. The angle is relative to where the actor faces (0 forward,
90 right, 180 back, -90 left). The request is queued (any thread) and starts on the next frame if the actor can dodge.
- Papyrus: `ReactiveDodge.Dodge(Actor akActor, float afAngle = 180.0)` (`Scripts/ReactiveDodge.pex`, source in
  `dist/Scripts/Source`). Console: click the actor, then `cgf "ReactiveDodge.Dodge" <refid> 90`.
- SKSE plugins: `api/ReactiveDodgeAPI.h` (`ReactiveDodgeAPI::Get()` returns `RDF_RequestDodge`, or null without the DLL).
An NPC faces its combat target through the dodge; without one it turns to the path.

## Third-party API headers
- `src/third_party/fuck/FUCK_API.h`, `IconsFontAwesome6.h`: FLICK's API from
  [Fuzzlesz/FUCK_API](https://github.com/Fuzzlesz/FUCK_API) (bc52809, API version 7, FLICK 1.7.2), unchanged. GPL-3.0,
  like CommonLibSSE-NG; the plugin is GPL-3.0 either way.
- `src/third_party/imgui/imgui.h`, `imconfig.h`: from [powerof3/imgui](https://github.com/powerof3/imgui) at
  fbbe3efd (the revision FLICK builds with; MIT). Types only: every ImGui call goes through FLICK's interface.
- `src/game/PrecisionAPI.h`: Precision's modder API header, copied unchanged from
  [ersh1/Precision](https://github.com/ersh1/Precision) `src/PrecisionAPI.h` (v2.0.6, GPL-3.0 with a modding exception; the
  header asks modders to copy it). Used only to register a pre-hit callback for i-frames (`[Compat] bUsePrecision`).
- `src/game/TrueDirectionalMovementAPI.h`: True Directional Movement's API header (lock-on target and directional mode).
