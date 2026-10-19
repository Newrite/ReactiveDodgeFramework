# Reactive Dodge Framework (RDF)

An SKSE plugin + Pandora behavior patch for **Skyrim SE 1.5.97** that turns the dodge into a
player-controlled movement action instead of an animation the player waits out:

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
build\windows\x64\releasedbg\rdf_tests.exe       # 51 deterministic core tests
python tools\gen_rdf_patch.py                    # regenerate the Pandora patch
python tools\stage_pandora.py --label rdf --rdf behavior/Pandora_Engine/mod/rdf
python tools\validate_rdf.py                     # 87 checks on the generated graphs
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
