# Reactive Dodge Framework 0.3.0 (SE 1.5.97)

- Fixed hostile/detrimental area magic bypassing explosion i-frames. Per-target
  delivery and its follow-up are skipped; beneficial effects remain available.
  The user confirmed that explosion damage is now blocked in the reported case.
- Added NPC API v2: world-space direction, one-shot requests, graph-confirmed
  results, cancellation before processing, and a probed travel-distance cap.
  Existing API v1 remains compatible.
- Optional Enhanced Combat Behavior build uses RDF for NPC dodges. RDF owns
  stamina, timing and protection. Includes a SCAR annotation string pointer fix.
- Kept early ray-candidate and per-type contact protection. Removed seven
  experimental extra delivery/feedback hooks and verbose impact diagnostics
  after they failed to remove the reported visual/audio impact.

The remaining projectile impact VFX/sound issue is unresolved. A separate mod
can emit feedback before RDF filters the contact; no CIF compatibility fix is
included. This release does not claim to suppress that external feedback.

Validation: 129 RDF tests pass; SE DLL builds. Offline adapter tests cover the
generated hook code. Game launches remain manual; not every NPC/API scenario
has been verified in game.

Install the main no-clips FOMOD and supply compatible dodge animations separately.
Third-party animation fixtures are intentionally excluded. Update the DLL normally;
these changes add no new behavior events. The optional ECB addon requires the
main RDF installation and must win the ECB/RDF DLL conflicts. Disable Combat
Pathing Revolution and Enhanced Combat AI when using ECB, per upstream requirements.
The source archive contains both modified projects and license notices.
