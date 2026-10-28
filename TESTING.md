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
  wildcards intact, 9 clips registered in both characters, 122 unrelated state machines unchanged, no new
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
* Power attack from a dodge, attack -> dodge, MCO-style windows; arm stance with fArmStance below 1.
* Gamepad, slopes/stairs/walls, save/load and cell change mid-dodge, death, menus.
* Visual blend quality with a proper animation set (fixtures are TUDM clips).
* Bow / magic layers ([Casting], set a keyword first, Pandora re-run): dodge while drawing a bow (draw kept, an arrow
  released mid-dodge flies after it: `replayed the release held by the dodge`), while charging a fire-and-forget spell and
  during a concentration spell; a chain inside the layer; dodge -> attack / block from a layer dodge; `dodge start ... (bow)`
  / `(magic)` in the log, and no `did not enter` warning.

## Protection window contract update — 2026-10-24

120 C++ cases pass: start + duration, independence from cancel annotations, signed modifiers, zero duration,
hard End before/after Start, independent windows, chain reset, interruption/lifetime, malformed/late metadata,
and frame-zero motion shortening startup before protection metadata.
The replaced test expected negative modifiers to delay start and positive modifiers to outlive End;
those expectations were deliberately removed because the new contract forbids both.

`python tools/test_protection_clips.py`: 3 tests pass (frame-zero conversion, preservation, idempotence,
invalid/ambiguous metadata rejection). All 54 migrated clips passed full XML round-trip comparison, including
End, root motion and other annotation preservation. The updated generator also passed a real HKX round trip.
Deployed DLLs and clips match build/staging outputs; FOMOD CRC, DLL, INI and base clips were verified.
This revision still needs an in-game dodge/perfect test.

### Area delivery and early projectile feedback, 2026-10-28

Added two native tests using the production Xbyak adapters in synthetic engine
frames: area allow/reject/re-allow, displaced Effect* restoration, ray collidable
and projectile argument forwarding, and null/non-null result forwarding. These
exercise generated code without loading a game DLL into another process.
Gameplay cases and hook contracts: docs/IFRAMES_DELIVERY_FIXES_2026-10-28.md.
Result: 129 cases, 0 failed checks; SE 1.5.97 DLL build succeeded. Game launches
are left to the user. Test output: _work/iframes-20261028-tests.log.

v0.3.0 final cleanup: removed experimental extra projectile delivery/feedback
wrappers after the user's negative runtime result. Retained area/ray adapters
and stream-contact protection. Final run: _work/release-0.3.0-tests.log,
129 cases, 0 failed checks; DLL build passed. Remaining impact feedback is open.

## ECB / API v2 — 2026-10-27

127 C++ cases pass. New checks cover exclusive concurrent actor reservation, queued vs confirmed result,
terminal consumption, busy/exiting rejection, cancellation racing processing, stale queue rejection, reset without
reusing ticket IDs, invalid vectors/budgets, capacity/abandoned result expiry, cancellation without undrained queue
growth, rejected one-shot removal from the input buffer, and clip-motion distance capped to the probed path.
The following normal player dodge has no inherited distance cap.

Both modified DLLs compile for Skyrim SE 1.5.97 (x64). Static PE inspection confirms RDF_GetApiV2, the v1 exports
and the SKSE export triad. The test override ZIP contains both matched DLLs; installed copies, SHA256 manifest,
source ZIP and archive CRC were verified. No game startup or in-game behavior verification is claimed.
A direct Python LoadLibrary probe was abandoned because CommonLib expects the game executable; it is not a
valid way to validate a loaded SKSE plugin.

ECB upstream and reproducible integration patch: integrations/enhanced-combat-behavior/.
