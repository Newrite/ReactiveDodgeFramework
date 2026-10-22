# Verification report (2026-10-19)

Three levels, kept separate. Level 3 lists what was actually run in Skyrim (user play-tests + dev-log traces) and
what is still waiting.

## 1. Automated (passing)

`build\windows\x64\releasedbg\rdf_tests.exe` — 53 cases, 0 failures. Deterministic traces at
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
  Precision/MaxsuOnHitAnimFW use); `TESActionData` vfunc 5 → 40551. The plugin requires CALL rel32
  (E8) at the motion site and preserves its current target, including another plugin's trampoline,
  for calls outside RDF motion ownership. Chaining with the reported third-party hook awaits an in-game test.
* Real Pandora run on a staged copy of every enabled STB mod's patch inputs (baseline reproduces the live
  output byte-for-byte for all 57 hkx), with the output redirected to `_work`: `validate_rdf.py` 80/80 —
  events/variables resolved (no −1), states 9701/9702 and every transition/effect as designed, vanilla
  wildcards intact, 16 clips registered in both characters, 122 unrelated state machines unchanged, no new
  Pandora log failures. A mistyped-name control patch fails 5 checks.
* Build: no warnings from project code (only CommonLib's bundled minhook C4701).

## 3. In game (user play-tests with `bDevLog=1`, traces in `ReactiveDodgeFramework.log`)

### Verified
| What | Evidence |
|---|---|
| Hooks install; root-motion hook owns displacement | `motion hook installed`; overrides grow every dodge frame |
| Local axes and yaw sign | requested vs measured displacement equal per frame; heading error 0.3-7 deg |
| Hook thread | several job threads (snapshot design holds) |
| 8 directions, steering, diagonals, TDM 360 / Vanilla Style | user + start-angle/clip statistics |
| Backward dodges in 360 mode keep facing | user |
| Chaining, mashing, buffer | 45 chains in one run; 40/47 unused presses were mashing, 7 were > 0.2 s before the chain window |
| Clip annotations reach the plugin | `RDF_Windows` on 72/72 dodges |
| Hand-over into running without planted frames | trace: motion driven through the out-blend |
| No dropped dodges | 0/72 entered-and-left after the clip-end fix (was: Entered -> ClipEnd -> Left in one ms) |

| Dodge -> attack / block | 24 attack cancels at 0.21-0.46 s, 0 failures; held block kept until release |
| Arm stance layer on steps | 72 steps with stance 1.0; user: looks good |

### Waiting
* Post-dodge speed floor (`carry` / `floorHits` in the trace).
* Projectile i-frames (`i-frames: projectile ... passed through` lines).
* Power attack from a dodge, attack -> dodge, MCO-style windows; arm stance on rolls (default off).
* Gamepad, slopes/stairs/walls, save/load and cell change mid-dodge, death, menus.
* Visual blend quality with a proper animation set (fixtures are TUDM clips).
