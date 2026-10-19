# Verification report (2026-10-19)

Three levels, kept separate. **Nothing below level 3 has been run inside Skyrim.**

## 1. Automated (passing)

`build\windows\x64\releasedbg\rdf_tests.exe` — 46 cases, 0 failures. Deterministic traces at
30/60/64/144/240 fps and with 0.25 s frame spikes:

* input buffer: press before a window opens executes when it opens; presses at window start/end;
  expiry; paused clock (dt = 0) never expires; mashing consumes once per action; debounce;
* arbiter: priority (interrupt > dodge/chain > attack > block > locomotion exit), one decision per tick,
  reject reasons; attack→dodge gated by attack state or anim-event window;
* motion: integrated distance within 2 % of the configured 300 u at every frame rate and with spikes;
  velocity continuity at chain and at exit; playback speed fits the clip to the timeline;
* steering: monotonic, rate-capped heading change for a mid-dodge input change; releasing input keeps
  the heading; reversals are rate-limited;
* forced interruption ends ownership immediately; seqlock never tears under a writer/reader stress test.

`python tools\test_hkxcheck.py` — 10 validator self-tests pass.

## 2. Static / offline (passing)

* Hook sites checked in `F:\STB\Skyrim\SkyrimSE.exe` 1.5.97 and in IDA: `41160+0x111` = `E8 6A 41 DE FF`
  → 31949 `ProcessMotionData` (only call site); `37673+0x3C0` → 37633 (hit processing, same site
  Precision/MaxsuOnHitAnimFW use); `TESActionData` vfunc 5 → 40551. The plugin re-verifies the motion
  site bytes and call target at runtime and refuses to install on mismatch.
* Real Pandora run on a staged copy of every enabled STB mod's patch inputs (baseline reproduces the live
  output byte-for-byte for all 57 hkx), with the output redirected to `_work`: `validate_rdf.py` 80/80 —
  events/variables resolved (no −1), states 9701/9702 and every transition/effect as designed, vanilla
  wildcards intact, 16 clips registered in both characters, 122 unrelated state machines unchanged, no new
  Pandora log failures. A mistyped-name control patch fails 5 checks.
* Build: no warnings from project code (only CommonLib's bundled minhook C4701).

## 3. In game — NOT YET RUN (needs the user)

Enable the mod, tick `rdf` in Pandora, set `bDevLog=1`, then check `ReactiveDodgeFramework.log`
(`Documents\My Games\Skyrim Special Edition\SKSE`). First confirm the four open assumptions:

| # | Check | How to read it |
|---|---|---|
| A | motion hook installed and called | log `motion hook installed`; trace `hook calls/overrides` grow during a dodge |
| B | local axes (x right, y forward) and yaw sign | trace `requested` ≈ `actual`, `err` near 0°; if the character turns the wrong way set `iYawSign=-1` |
| C | `bAnimationDriven` stays on through the dodge | `overrides` keep growing until the dodge ends |
| D | hook thread | `thread=first/last` (informational; the snapshot is thread-safe either way) |

Then the scenario list:

* Directional: dodge in 8 directions; change input mid-dodge (right → forward-right should curve);
  rapid changes; release input mid-dodge (keeps heading); analog half-tilt (weaker steering); rotate
  camera during dodge; with TDM lock-on (sidestep clips, keeps facing target).
* Transitions: idle→dodge→idle; run→dodge→run (no stop at the end while input held);
  dodge→attack at ≥0.30 s; attack follow-through→dodge; dodge→dodge (3 chained, then refused);
  dodge→block; hit/stagger during dodge.
* Buffering: press dodge during an attack swing → dodge fires at follow-through; press attack early in a
  dodge → attack fires at 0.30 s; mash dodge → exactly one dodge (+ chain if pressed in the chain window);
  a press 0.3 s before eligibility is dropped.
* Physics: dodge into a wall (stops, no jitter), up/down a slope and stairs, off a ledge (falls), near NPCs.
* Lifecycle: save/load mid-dodge, cell change through a door, death mid-dodge, open a menu mid-dodge,
  sheathe/draw during a dodge, first person (refused).
* Visual: blend quality of every transition — this needs real dodge animations; the TUDM fixture clips
  have their own baked direction poses, so FaceMotion (forward roll + turning) is the representative mode.
