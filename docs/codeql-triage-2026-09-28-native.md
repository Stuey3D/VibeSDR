# CodeQL + Dependabot triage — native, Go, Python, Java, config (2026-09-28)

Scope: every open code-scanning alert that is NOT JavaScript/TypeScript/HTML (those are in
`codeql-triage-2026-09-28-web.md`), plus the four open Dependabot alerts. Analysed at `1bd1d95b`.
Nothing here has been dismissed on GitHub — the dismissals below are recommendations for Stuart.

## Verdicts

| # | Rule | Where | Verdict | Action |
|---|------|-------|---------|--------|
| 52 | cpp/command-line-injection | `android/app/src/main/cpp/vibe_benchmark_dab.h:138` | **REAL** (local) | Fixed — no shell |
| 18 | cpp/integer-multiplication-cast-to-long | `android/app/src/main/cpp/local_sdr_shim.cpp:19037` | False positive in practice | Widened anyway (free) |
| 17 | cpp/integer-multiplication-cast-to-long | `android/app/src/main/cpp/ft8_lib/common/monitor.c:39` | False positive in practice | Widened anyway; vendored, noted in-file |
| 15, 16 | cpp/system-data-exposure | `android/app/src/main/cpp/net_shim.cpp:110` | **False positive** | Dismiss as **false positive** (owner-only admin replies) |
| 2 | go/request-forgery | `tools/vibeiq/bridge.go:221` | Target is user-chosen by design, but was **unvalidated** — REAL hardening + a real bug | Fixed |
| 1 | py/bind-socket-all-network-interfaces | `tools/spyserver/ss_proxy.py:89` | **REAL** (dev tool) | Fixed — loopback default, `--bind` |
| 46 | java/android/webview-debugging-enabled | `node_modules/expo/.../ExpoLogBoxWebViewWrapper.kt:27` | Third-party | Dismiss **won't fix — third-party code**; now filtered out |
| 47 | java/android/webview-debugging-enabled | `node_modules/react-native-webview/.../RNCWebViewManagerImpl.kt:92` | Third-party | Dismiss **won't fix — third-party code**; now filtered out |
| 48 | java/implicit-cast-in-compound-assignment | `node_modules/react-native-svg/.../PathParser.java:477` | Third-party | Dismiss **won't fix — third-party code**; now filtered out |

## Detail

### #52 — command injection in the DAB benchmark clip download (REAL, fixed)
`ensureDabClip()` built `curl -sSL '<url>' | gzip -dc > '<cacheDir>/dab-bench.vbu8.part'` and ran it
through `std::system`. `cacheDir` is not a constant: `vibeserver` passes the directory of its config
file (`--config` / environment), and `vibeserver-bench` passes `$TMPDIR`. One `'` in that path closed
the quoting and the rest ran as shell — CodeQL's flow is exactly getenv → operator+ → system.
Local, not remote (whoever sets the environment can already run commands), but the same class of
hole that `vibeserver/proc.h` was written to remove everywhere else.
**Fix:** the desktop branch now uses `vibeproc::run` (curl `-o <tmp>.gz`) then
`vibeproc::runToFile` (gzip `-dc -- <tmp>.gz` → `<tmp>`): an argument vector, no shell, the pipeline
turned into two programs through a temp file as `proc.h` prescribes. Android was already `#ifdef`'d
out and does not include `proc.h`.
**Verified:** a throwaway program linked against `libvibeserver_core.a` downloaded the real 23.6 MB
clip into a directory named `it's $(touch PWNED) dir` — clip whole, no `PWNED` file, no `.part`
left behind; an unreachable URL returned empty and left nothing. `vibeserver`, `vibeserver-bench`
and `dab-switch` build on the Mac; Lite `assembleRelease` rebuilds `vibe_localsdr_jni.cpp`.

### #18 — float product converted to double (`local_sdr_shim.cpp`, the once-a-minute "capture clean" log line)
`(double)B * B / (A * A + B * B)`: the denominator was computed in `float` and then promoted. `A`
and `B` are the IQ-imbalance correction coefficients (A ≈ 1, B ≈ 0) — overflowing `float` needs
|A| > 1.8e19, which the corrector cannot produce. Not a real bug, but widening costs nothing on a
log line, so both occurrences (this one and the `asin` argument on the next line) are now
`(double)A * A + (double)B * B`. If the alert does not close on its own: **dismiss as false positive**
— values are bounded correction coefficients, see `iqCapClean_.coefA()/coefB()`.

### #17 — `waterfall_init` size in vendored ft8_lib
`max_blocks * time_osr * freq_osr * num_bins * sizeof(...)` multiplied four `int`s before the
`size_t`. Real values: ~100 blocks × 2 × 2 × ~1000 bins ≈ 4e5 — nowhere near `INT_MAX`, and the
parameters come from `ftx_monitor_config_t`, set by our own decoder, not from the network. False
positive in practice. **Minimal local patch** (first operand cast to `size_t`), marked in the file as
a VibeSDR change not in upstream ft8_lib. ft8_lib is deliberately NOT excluded from analysis: it is
committed here and compiled into the server and both apps.

### #15 / #16 — "system data exposure" at `net_shim.cpp:110` (FALSE POSITIVE — dismiss)
`net_shim.cpp:110` is `Socket::send`, the sink for every byte the server writes. The eight code
flows in the SARIF all start at `getenv("HOME")` / `getenv("VIBESERVER_DATA_DIR")`
(`vibeserver/main.cpp:630`, `vibeserver/vibeserver_api.cpp:122`) — the DATA DIRECTORY — and end in
exactly three replies, all in the `/vibeserver/admin/` handler in `local_sdr_shim.cpp`:
- `GET /vibeserver/admin/bans` — the ban list, read from `<datadir>/bans.jsonl` (taint = "file read
  from a path built from getenv");
- `GET /vibeserver/admin/connections` — the connection log from `<datadir>/connections.jsonl`;
- `POST /vibeserver/admin/notice` error — `"could not write <datadir>/notice.json"`.
All three are behind the admin gate at the top of that handler (`vsAdminProof(secret, reqLine)`,
refused with 401 when no password is set, brute-force backoff per IP). The reader is the OWNER of
the server, who set those paths, and the data is the owner's own ban list/log. Nothing reaches an
unauthenticated listener. **Dismiss: false positive — admin-authenticated owner-only endpoints.**

### #2 — request forgery in the VibeIQ bridge (hardening + a real bug, fixed)
`wsURL()` fetched `"https://" + p.host + p.path + "vibeserver.json"`. The host is MEANT to be chosen
by the user (the receiver's address typed in the VibeIQ window / `--host`, or the directory's answer
to a pairing code) — this is not an open proxy, and the local UI is already guarded against
cross-site and DNS-rebinding requests (`ui.go` `guard`). But the string went into the URL raw, so
`evil@host`, `host?x`, `host#x` or a `/../` path changed which server or page was fetched, and the
receiver's `directUrl` answer was used without checking its scheme.
**Fix:** `checkTarget()` runs on both pairing routes: host must be a bare `host[:port]` (no
userinfo/query/fragment/path, port 1-65535) and the path must be `/` or `/r/<radio>/` — the same shape
the directory enforces (`IQ_PATH_RE`). The URL is built with `url.URL{}`; `directUrl` must be
`http(s)` with a host and no userinfo.
**Real bug found on the way:** a pairing with no radio path (every single-radio receiver) fetched
`https://<host>vibeserver.json` — the slash was missing — so it could never connect. The path now
normalises to `/`. `tools/vibeiq/bridge_test.go` covers good and hostile inputs; `go vet`, `go build`,
`go test` pass.

### #1 — SpyServer capture proxy bound to 0.0.0.0 (REAL, fixed)
`tools/spyserver/ss_proxy.py` is a developer capture tool, but it bound every interface while its own
banner said "listening on 127.0.0.1" — an open relay to someone's SpyServer that also records
everything, reachable from the LAN by accident. Now `--bind` defaults to `127.0.0.1`; the docstring
shows `--bind 0.0.0.0` for a client on another machine (SDR# on Windows). `py_compile` passes.

### #46 / #47 / #48 — node_modules (dismiss: won't fix, third-party code)
All three are in React Native modules compiled by the Android build (react-native-svg,
react-native-webview, expo's log-box). We do not own or ship patches for that code; upgrades are
Dependabot's job. Note both WebView findings are `setWebContentsDebuggingEnabled` in library code
that the library gates on the host app's debug flag.

## The CodeQL configuration change (`.github/workflows/codeql.yml`)
`node_modules` was ALREADY in `paths-ignore` — and these alerts still appeared, because
**`paths-ignore` does not apply to compiled languages** (java-kotlin, c-cpp, swift with manual/auto
build): the database contains whatever the build compiled, and the Android build compiles every
autolinked RN module out of `node_modules`. So the analyse step now writes SARIF to disk
(`upload: never`, `output: sarif-results`), a `jq` step drops every result located under a
`node_modules/` directory (logging how many), and `upload-sarif` uploads the rest under the same
category. Tested on the live SARIF: java-kotlin 3 → 0, c-cpp 5 → 5.

Deliberately still analysed: all of `android/app/src/main/cpp`, INCLUDING the vendored C trees
(`ft8_lib`, `libairspy`, `libairspyhf`, `vibedsp/third_party/pffft`) — they are committed, compiled
into what we ship, and patched in-tree when needed. The existing `paths-ignore` list (node_modules,
build output, two unreferenced static HTML skins, `scripts/`) is unchanged.

Once the next analysis runs, #46-48 should close as "fixed" (no longer reported) on their own; if
they do not, dismiss them as "won't fix — third-party code".

## Dependabot — `image-size` (#50, #51, #52, #53)
GHSA-w3rx-r6r6-pgpr (ICNS) and GHSA-5p2g-fcmc-qvqq (JXL/HEIF): vulnerable ≤ 2.0.2, first patched
**2.0.3**. There is **no patched 1.x**. Pulled in only by Metro (build-time; it reads our own image
assets to record their width/height — never at runtime in any app):
- root: `@expo/cli 56.1.13 → @expo/metro 56.0.0 → metro 0.84.4 → image-size 1.2.1`
- lite/app: `@react-native/metro-config 0.73.5 → metro-config/metro 0.80.12 → image-size 1.2.1`

Every Metro release up to 0.87 still declares `image-size ^1.0.2`, so no parent upgrade exists.
**An override alone BREAKS Metro** (measured): 2.x takes bytes only — root Metro passes a file PATH
(`The "list" argument must be an instance of ... ArrayBufferView`) — and its CommonJS export is an
object, so Lite's `const getImageSize = require("image-size")` is not a function.
**Fix:** `overrides: { "image-size": "^2.0.4" }` in both `package.json`s, plus a small patch-package
patch to Metro's `src/Assets.js` (read the file into a Buffer; in Lite also take the named
`imageSize` export): `patches/metro+0.84.4.patch`, `lite/app/patches/metro+0.80.12.patch`. The root
already runs patch-package on postinstall; **Lite gains `patch-package` as a devDependency and a
`postinstall`** — which is why `lite/app/package-lock.json` grew by ~430 lines (30 packages, all
patch-package's own deps). Root lockfile: 1.2.1 → 2.0.4, `queue` dropped, 20 lines.
**Verified:** `npx expo export --platform ios` bundles with all image assets; root Metro's
`getAssetData` returns 192×192 for `assets/rtltcp.png`; a Lite `react-native bundle` of an entry
requiring that PNG emits `width:192,height:192` and the drawables; `tsc --noEmit` 9 errors before
and after (pre-existing, unrelated).
**Maintenance note:** the Metro patches are keyed to the Metro version. When Expo or Lite's RN
moves Metro, patch-package will say the patch does not apply — re-make it (two lines) or drop the
override if Metro has moved to image-size 2.
