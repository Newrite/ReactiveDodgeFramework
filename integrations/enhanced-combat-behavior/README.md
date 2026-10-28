# Enhanced Combat Behavior integration patch

Base: https://github.com/tavvfiq/enhanced-combat-behavior commit
`6c33674a830fc70eb535b70bf28124406dea43d3` (1.0.0).

Apply `rdf-api-v2.patch` with `git apply`, set COMMONLIBSSE_NG_DIR to the installed checkout,
and build SE only with xmake as described in ../../docs/ECB_INTEGRATION.md.
The local working fork is _ext/enhanced-combat-behavior, branch codex/rdf-backend.
The patch includes the matching RDF v2 API headers and GPL-3.0-or-later ECB modifications;
upstream LICENSE, CREDITS and THIRD_PARTY notices must accompany a distributed ECB build.

It prefers RDF v2 and retains TK fallback when RDF v2 is absent. To test the RDF backend,
use this ECB DLL together with this repository's API v2 RDF DLL. Both are built for 1.5.97.
The local ECB commit is `9a7c21a`; the published patch reproduces it from the base
above. Game launches are left to the user; compilation and 129 RDF tests pass.

The patch also fixes SCAR annotation hkStringPtr pointer truncation exposed by the
2026-10-28 crash dump. See ../../docs/ECB_CRASH_2026-10-28.md. Its standalone test
is `xmake build -P . havok_string_test`, then `xmake run -P . havok_string_test`.
