# Enhanced Combat Behavior / RDF API v2 (SE 1.5.97 test build)

The local ECB fork lives in `_ext/enhanced-combat-behavior`, branch `codex/rdf-backend`, based on upstream
`6c33674a830fc70eb535b70bf28124406dea43d3` (1.0.0). This folder is intentionally outside the RDF Git history.

## Ownership

ECB decides when and where an NPC dodges, probes obstacles/ledges and submits a world-space vector.
RDF owns execution: graph/layer/clip, motion, resource choice and price, attack cancellation, i-frames and perfect.
ECB's TK cost, TK iframe duration and iStep writes are skipped whenever RDF API v2 is available. The optional ECB
MinDodgeStamina and forward-dodge stamina percentage remain tactical thresholds, not a second charge.
The old EnableTKDodgeIntegration setting enables/disables the selected backend, including RDF.

RDF is preferred when its v2 export exists; otherwise ECB keeps its original TK event backend. The chosen backend
is logged once in EnhancedCombatBehavior.log. A v2 request rejected at runtime never falls through to TK.

## Requests and results

`api/ReactiveDodgeAPI.h` and `api/ReactiveDodgeTypes.h` define the ABI. V1 exports remain compatible.

- `GetV2()->SubmitWorld(actor, x, y, maxDistance)` returns a ticket; zero means the request was not admitted.
  V2 accepts NPCs only. V1 still accepts player and NPC requests.
- Calls are thread-safe. Only the main update tick touches dodge agents/graphs. Actor handles survive the queue,
  not raw actor pointers. The direction is world-space, so the actor turning before processing cannot reverse it.
- The request is attempted once, without chains or the player's input buffer. A failed press is explicitly
  discarded. A queued request older than 250 real milliseconds is rejected, rather than triggering after a menu.
- `Poll(ticket, consumeTerminal)` returns Pending, Starting, Started, Rejected or Unknown. Starting reserves the
  actor but is not a successful dodge. Started requires the graph's entered handshake. Missing graph entry uses
  RDF's existing timeout and resource refund. Results remain up to 60 real seconds, bounded to 512 entries;
  consumers should consume terminal results. Unknown also means reset/expired/consumed, not success.
- `State(actor)` returns an update-thread snapshot (Idle/Pending/Starting/Dodging/Exiting). This is an advisory
  busy check, not a guarantee about resources or graph support. Final validation belongs to the attempt.
- `Cancel(ticket)` can stop a queued request before processing claims it. It cannot interrupt an already starting
  dodge. Save/load clears requests, results and snapshots; ticket numbers are not reused within the process.

ECB waits for Started before recording the action, notifying its observers/API or spending the action cooldown.
While pending or dodging it does not choose another AI action. A rejected ForwardDodge falls back to ordinary
Advancing against a freshly gathered target state; Unknown after save/load does not replay old decisions.

## Travel budget

ECB passes the smaller of its obstacle-ray distance (220 units) and enabled ledge-probe distance (default 180).
RDF caps that request's INI distance and subsequent RDF_Motion overrides to this budget. The next normal dodge
has no inherited cap. This bounds the planned trajectory; existing collision/ledge probes are still sampling
checks, not a proof that every point of the swept path is clear.

## Build and test

RDF: `xmake build rdf_tests`, then `xmake build ReactiveDodgeFramework`.
ECB: set COMMONLIBSSE_NG_DIR to the existing CommonLibSSE-NG checkout, then from its repository:

```
xmake f -P . -m releasedbg --skyrim_se=y --skyrim_ae=n --skyrim_vr=n -y
xmake build -P . -y EnhancedCombatBehavior
```

The integration package needs the matching RDF DLL and modified ECB DLL together. Keep the normal RDF mod,
behavior patch, configuration and clips installed. No new behavior events or animation patch generation are
required by this API change. Do not run Enhanced Combat AI or Combat Pathing Revolution alongside ECB
(upstream incompatibilities). A separate test override mod makes both DLLs reversible together.

In-game verification remains necessary: select an NPC in the console with ECB EnableDebugLog enabled and
RDF bDevLog=1. Check successful entry, refusal at insufficient resources/non-cancellable attack, no double charge,
side/forward dodges, missing graph rollback, save/load and fallback advancing. Offline compilation/tests do not
establish runtime correctness for NPC dodges or the two chained update hooks.

## Local verification notes

A direct LoadLibrary test under Python was abandoned: CommonLib initializes its game-module metadata and
rejects python.exe as a game executable. No API runtime smoke-test result is claimed from that attempt.
Use PE export inspection for offline ABI presence, and Skyrim/SKSE for runtime validation.
