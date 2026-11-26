# Basic map water (beta96)

The GPU map without High Detail Maps: lakes from z6 up (part 1), and the rivers >= 200 m in the basic pack (part 3,
approved by Stuart 11-26 from the measured options in part 2).

## RESUME HERE

### Part 5 (branch `b96-packs2`): the packs are never copied — read in place from the APK / the iOS bundle

- **Done.** Stuart approved 11-26 ("these go into b96 too"); closes both "Open" lines of part 4.
- **Android, the server** (`vibe_mapgl.h`): new `setBundleSlice(rel, path, offset, length)` — a bundle file served from
  a run inside another file (the APK entry). Checked before it is trusted: a bundle name (never the detail pack), inside
  the file, and a `.pmtiles` run must start with `PMTiles` (a compressed entry would not). `serve()` is now
  `serveTo(out, …)` — socket-free, `pread` from a base offset, so a plain file and an APK slice take one path; same
  URL, Range/206/416, `Content-Range …/<entry length>`, `?r=` stripping, pacing. JNI `VibeMapGL.nativeSetBundleSlice`
  (returns "" or why not).
- **Android, Kotlin** (`VibeApkPack.kt`, android-free, shared with Lite; `VibeMapGL.kt`): `apkSlices` once per process —
  `AssetManager.openFd("mapgl/<pack>")` gives the APK descriptor, `startOffset`, `length` (it THROWS for a compressed
  entry → that pack is unpacked as before, logged `VibeMapGL`), the file via `/proc/self/fd/<fd>` (split APKs; `sourceDir`
  fallback), length checked against the manifest, magic checked. `start()` registers them with the server before
  `setBundleDir` (a refusal there — should be impossible — copies that one pack out, logged). The unpack skips them.
  The app's map: `mapglPack.ts readPackRange` → sync `VibePowerModule.mapglPackRange(name, off, len)` →
  `VibeMapGL.readPackRange` (one RandomAccessFile on the APK, base64; the unpacked copy if a pack is not a slice).
  `MapOverlay.tsx` unchanged (same `mgpack` bridge message).
- **Upgrade clean-up, Android:** the marker is now `<manifest version> apk:<packs>`. A beta95/96 bundle's marker is the
  version alone → mismatch → `mapgl-bundled/` is deleted and redone once with fonts/scripts/icons/style only. The
  30.5 MB of old pack copies go with it.
- **iOS** (`mapglPack.ts`, cannot be built here): the packs are read from the app bundle — `resolveBundlePacks()`
  (expo-asset `localUri`: the bundle file itself in a release build, `downloadAsync` returns a file URL unchanged) on
  every `ensureMapglPack`, then `File(src).open(FileMode.ReadOnly)` (★ the default open is read-WRITE —
  `FileHandle(forUpdating:)` — which a read-only bundle file refuses). Only fonts/scripts/icons/style are copied to
  `documents/mapgl/`. **Upgrade clean-up, iOS:** the marker is `MAPGL_VERSION + '+bundlepacks'`, so a beta95/96 copy
  no longer matches; the re-copy first deletes `documents/mapgl/vibemap-{basic,relief,runways}.pmtiles` (the detail
  pack in the same folder is not touched).
- About: the rivers note's "about 14 MB on Android, which keeps one copy for the app and one for its built-in server"
  was stale since part 4 — now says the map is read from the app itself, ~30 MB less on the phone.

| Measured (11-26) | Before (part 4) | After | Change |
|---|---|---|---|
| APK `assembleCurrentRelease`, built CLEAN (fresh `android/app/build`) | 137,640,528 B | 137,850,136 B | +0.2 MB — not this branch (b96-integrate merges since part 4); packs still Stored once, `res/.*pmtiles` = 0 |
| `filesDir/mapgl-bundled/` after first run (manifest bytes, 80 files) | 33,658,705 B | 3,202,239 B | **−30,456,466 B (−30.5 MB)** |
| Android storage once installed (APK + bundle) | ≈ 171.3 MB | ≈ 141.1 MB | **−30.2 MB** net (−30.5 MB from this work) |
| iOS storage (bundle + `documents/mapgl/`) | packs twice | packs once | −30.5 MB expected; NOT measured on a device |
| App RAM with a map open | — | — | unchanged from part 4 (ranged reads either way) |

Real-APK check: in the built APK the three packs are Stored at offsets 101,549,792 / 119,727,796 / 128,092,756
(4-byte aligned), `PMTiles` at each; `setBundleSlice` + `serveTo` on that APK answer `bytes=0-6` with 206, `PMTiles`,
`Content-Range: bytes 0-6/18177938`.

**Tests (part 5):**
- `vibeserver/test-mapgl-range` (ASan/UBSan build via the lock): **80 ok, 0 failed** — new: a real zip (stored +
  deflate-flagged entries, odd extra fields) → slice accepted/refused (no magic, out of bounds, not a bundle name, the
  detail pack, missing APK), 206 + magic + `Content-Range …/<entry length>`, middle range, `bytes=-n` = end of the entry
  (never the central directory), full 200 in small chunks, HEAD, 416, unsliced files still from the bundle dir, 404
  with neither, re-register, APK gone → 404.
- `scripts/test-apk-pack.sh` (VibeApkPack.kt on a plain JVM with gradle's cached Kotlin 2.1.20 compiler; a
  `java.util.zip` APK with STORED and DEFLATED entries; a fake `openFd` that throws for compressed ones): **18/0**.
- `scripts/test-server-map-water-nl.mjs` with this branch's vibeserver + mapgl-detail-v4: **476 passed, 0 failed**.
- `check-map-overlay.mjs`: all passed. `tsc`: no new errors (45 before = 45 after, all in handheld-ui/test and scripts).
- Not run: Lite build (VibeApkPack.kt added to its SHARED list — compile it on the next Lite build); any device.

**On a device (owed):**
- Android, upgrade over beta95/96: `adb shell run-as net.vibesdr.app du -sk files/mapgl-bundled` ≈ 3,100 KB (was
  ≈ 32,900); logcat `VibeMapGL`: "GPU map packs read from the APK: vibemap-basic.pmtiles, …" and "unpacked: 77 files";
  logcat (native): "mapgl: vibemap-basic.pmtiles served from /data/app/…/base.apk @…". Open the app's map
  (basic, relief shading, runways at an airport) and a listener's map on the phone-as-server (`/mapgl/`).
- Lite: the same server log line; its admin map draws.
- iOS, upgrade over beta95/96: Settings → General → iPhone Storage → VibeSDR "Documents & Data" drops ~30 MB; the
  map draws basic + relief + runways (if the bundle read fails the whole GPU map fails → Leaflet fallback, so a
  Leaflet map after this build = look here first); High Detail Maps still works (same folder, untouched).

### Part 4 (branch `b96-mappack`): Android carries the GPU map ONCE, and the app reads the packs BY RANGE

- **Done, both jobs.** Commits c94e639b7 (renderer + test), 9caeb9a62 (app + Android manifest + checks), then the web page
  and this section.
- **Job 1, one copy on Android.** Every `require()` in `src/generated/mapglAssets.ts` made Metro embed that file in the APK
  (`res/`, names shortened by R8) — but Android's map never read those copies: since 10-13 it reads the APK's own
  `assets/mapgl/` bundle (mapgl-assets.gradle), unpacked once to `filesDir/mapgl-bundled/` by `VibeMapGL.bundleDir`, the
  same folder the in-app VibeServer serves at `/mapgl/`. So all 75 map files went in twice: 30.5 MB of Stored packs plus
  fonts, scripts and icons. Now `sync-mapgl-assets.mjs` also writes **`src/generated/mapglAssets.android.ts`** (same
  `MAPGL_VERSION`, `MAPGL_FILES = []`); Metro picks it over `mapglAssets.ts` on Android, so nothing is embedded twice.
  The app and the built-in server share the one unpacked `mapgl-bundled/` (as they already did); the server's URL and
  `?r=<basicRev>` are unchanged (no C++ change). `check-map-overlay.mjs` fails if the Android twin is stale or ever
  `require()`s a file. Lite never had the RN copy (no React Native) and keeps its single `assets/mapgl/` copy.
- ★★ **TRAP: an INCREMENTAL gradle build keeps the old copies.** `android/app/build/generated/res/react/<variant>/` is not
  cleaned by the bundle task, so a build on top of an older one still packs the stale `res/raw/assets_mapgl_*` files
  (seen here: the first after-build was still 169.2 MB). A release build must start clean, or delete that folder first.
  Check: `unzip -l <apk> | grep -c 'res/.*pmtiles'` must be 0.
- **Job 2, range reads.** `vibemapgl.js` takes a new option **`packRange(name, offset, length)`**: when a host gives it
  (and no `rangeBase`), the basic, relief and runways packs are pmtiles sources over ranged reads, each first probed for
  its `PMTiles` magic (a missing relief/runways pack is left out as before; a missing basic pack fails as before). The
  map page (MapOverlay) answers through the bridge exactly as for the detail pack: message `mgpack` →
  `mapglPack.ts readPackRange(glBase, name, off, len)` (FileHandle, base64, same `__mgDeliver`). Only the three bundled
  names, only under the page's own base (`mapgl/` iOS, `mapgl-bundled/` Android), reads capped at 4 MB, handles closed
  before an iOS re-copy. `detailRange` now works on either app path. Without `packRange` the old whole-read path remains
  (any other host; an iOS install whose copied `vibemapgl.js` is still the old one simply reads whole, as before).
  ★ Trade-off: a bridge round trip per tile (base64) instead of a memcpy, on first draw only (MapLibre keeps decoded
  tiles), for ~29 MB less RAM the whole time a map is open.
- **Upgrades from beta95:** Android — the APK manifest version changes (`#version` in `assets/mapgl/manifest.tsv`), so
  `VibeMapGL` re-unpacks `mapgl-bundled/` (marker last) and the app and server read the new pack; Android never used
  `MAPGL_VERSION`. iOS — `MAPGL_VERSION` 33407805-75-13d314b3ce3e → 33410715-75-3e2fee586428, so `ensureMapglPack`
  re-copies (and with it the new `vibemapgl.js`).
- `vibe_web_page.h` rebuilt via the lock (the pages' `/mapgl/` `?r=` covers `vibemapgl.js`).

| Measured (11-26) | Before | After | Change |
|---|---|---|---|
| APK, `assembleCurrentRelease` (debug-key signed, arm64 + armv7) | 169,229,797 B | 137,640,528 B | **−31,589,269 B (−31.6 MB)** |
| APK `res/` entries | 1,489 (37.9 MB) | 1,414 (4.5 MB) | the 75 map files gone; `assets/mapgl/` 81 entries, unchanged |
| Android storage once installed (APK + `mapgl-bundled/` 33.6 MB) | ≈ 202.9 MB | ≈ 171.3 MB | **−31.6 MB** (the `res/` copy was never unpacked, so the saving is the APK's) |
| Lite APK | one copy | one copy | only `vibemapgl.js` changed |
| Map page ArrayBuffers with a map open (V8 backing stores, `MEASURE_MEM=1`, 3 rounds each, 6 views incl. the world) | 35.1 MB | 6.0 MB | **−29.1 MB** (basic+relief+runways = 30.5 MB) |
| Bytes over the bridge for those 6 views | — | 2.26 MB in 134 reads | |

Correction to part 3's table: Android storage was ≈ +28 MB *per copy* there; the app's `documents/mapgl` copy does not exist
on Android (that path is iOS only), so the doubled storage was the APK's `res/` copy, now gone.

**Memory, how measured and what is still owed.** Headless Edge (Chromium, the Android System WebView's engine), the app's
page path (`OPEN('whole')` vs `OPEN('range')`, the bridge played by HTTP ranges + the base64 round trip), forced GC, then
`Runtime.getHeapUsage().backingStorageSize`. Renderer RSS is printed too but swings by hundreds of MB under SwiftShader,
so it is not judged. Not measured on a phone: no emulator image here (only an armv7 Android-TV 5.0 and an API 28 image;
the two adb devices are Stuart's live servers, not touched). **On a device:** open a map, wait for idle, then
`adb shell dumpsys meminfo net.vibesdr.app` AND the WebView's renderer (`adb shell dumpsys meminfo | grep -i sandboxed`
→ `dumpsys meminfo <pid>`), before (beta95) and after; expect ~30 MB less in the renderer. iOS: not built here (vendored
device-only libs block a sim link); `tsc` clean for app code, and the iOS path is the same JS (`readPackRange` with base
`mapgl/`, the files `ensureMapglPack` already copies).

**Tests (part 4):** `scripts/test-server-map-water-nl.mjs` with the b96-keeprun vibeserver and mapgl-detail-v4: **476
passed, 0 failed** — a new fetch test of the server's `/mapgl/vibemap-basic.pmtiles?r=319a8e477c` (206 + magic +
`Content-Range …/18177938`, HEAD length), the three old phases, and two new ones: **app range path** (261 reads, 1.46 MB
for every sample) and **app range path + detail** (387 reads, 2.08 MB) — every water/land point the same as the server's
map. `MEASURE_MEM=1 … test-server-map-water-nl.mjs`: 5/0. `check-map-overlay.mjs`: all checks passed. Lite
`assembleDebug -x bundleJs` built (release needs the rotation keys; Lite's Metro cannot resolve through a worktree's
symlinked node_modules, and the JS bundle does not touch the map): 73.5 MB, `assets/mapgl/` once, no `res/` map copies,
the new `vibemapgl.js` inside; app `assembleCurrentRelease` built with a throwaway
keystore.properties pointing at the debug key (deleted after).

**Open (one line each):**
- ✓ DONE in part 5: Android no longer unpacks the three packs (read from the APK by the server and the app).
- ✓ DONE in part 5 (device check owed): iOS reads the packs from the app bundle; only fonts/scripts/icons are copied.

- **Part 3 is done** on branch `b96-basicrivers` (off b96-integrate): Overture river/canal/tidal/dock areas >= 200 m wide,
  z5–8, are IN the basic pack. Ships at Rotterdam, London, Antwerp and Saigon sit on water on every server and in the app
  without High Detail Maps. Commits 13bdc62bc (pipeline), abed9ab5e (pack + style + renderer + copies), fe94e714c (pack
  revision on the URL + checks), c549fb349 (test), 218be1c30 (vibe_web_page.h via the lock), 3a2c6427d (About + credits).
- Rebuild: `node scripts/gen-river-areas.mjs --basic` (7 s from the cached parquet, duckdb ~2.7 GB RSS — run it alone),
  which writes `build/maptiles/vibemap-basicwater.pmtiles` and joins it into `build/maptiles/vibemap-basic.pmtiles` if that
  exists without the rivers. `gen-map-tiles.mjs` now joins it after every basic build and REFUSES without it
  (`--no-basicwater` on purpose). Then copy the pack to `assets/mapgl/` and `directory/public/mapgl/` and run
  `node scripts/sync-mapgl-assets.mjs --scripts` (stamps the style's `vibesdr:basicRev`, copies the style, rewrites the
  manifest). `check-map-overlay.mjs` fails if the two copies differ or the revision is stale.
- ✗ The directory on main was NOT touched (its own `vibemap-water.pmtiles` overlay and its own basic pack stay).
  ★ If main's directory ever takes this v12 basic pack, it carries the rivers itself, so its overlay can then go
  (both are the lakes colour, so drawing both is harmless, just redundant bytes).

## Part 3: the rivers in the basic pack

**One pack, not a sidecar.** `scripts/lib/basic-water.mjs` merges the water pack into `vibemap-basic.pmtiles` with
`tile-join`: z5–7 tiles gain one layer (`riverareas`), z8 tiles hold only the rivers. Every host already fetches, bundles,
whitelists and caches that one file (`vibe_mapgl.h resolve()`, CMake install, `mac/build-app.sh`, `mapgl-assets.gradle`,
`src/generated/mapglAssets.ts`), so no C++ or packaging change was needed and no host can carry half of it. A sidecar
would have needed a new name in all of those, plus a fourth whole-file load in the app.
- The join keeps the old layers exactly: 337 tiles (every z0–3 tile, 240 random z4–7 tiles, Rotterdam/London/Antwerp/
  Saigon at z5–7) decode identically to the old pack for every old layer (tippecanoe-decode, layer by layer).
- **Style:** layer `riverareas-b` (fill, the lakes colour `#1b4a6b`, minzoom 5), placed above `urban`/`lakes` and under
  `coast-b`, like the detail pack's `riverareas-d`. Metadata `vibesdr:basicRivers` lists it.
- **Renderer (`vibemapgl.js prepareStyle`):** those layers read the same pack through source `vs-basic-rivers`
  (minzoom 5, maxzoom 8), so the z8 tiles over-zoom above; the `basic` source stays maxzoom 6, so the land never asks for
  the river-only z7/z8 tiles. With the detail pack the layer's maxzoom becomes `riverareas-d`'s minzoom (8): z5–7 drawn by
  the basic pack, z8+ by the detail pack only, never both (checked in the test: z7 top fill `riverareas-b`, z8+
  `riverareas-d` with no `riverareas-b`). Detail's z8–9 rivers are >= 150 m, a superset of ours, so nothing is lost at
  the hand-over.
- **Cache safety:** `/mapgl/` goes out with a day's max-age and no ETag. The pack URL now carries `?r=<sha1 prefix>`
  (`vibesdr:basicRev`, stamped by sync-mapgl-assets); the style is always fetched no-cache, so a browser cannot pair an
  old cached header with the new pack's bytes. The server strips the query (as for the vendor scripts).
- **Attribution:** the `basic` source now credits "© OpenStreetMap contributors (rivers via Overture Maps) · ODbL";
  About's OpenStreetMap credit and the website's map-data line name the rivers.

**Copies updated:** `assets/mapgl/vibemap-basic.pmtiles` (the app), `directory/public/mapgl/vibemap-basic.pmtiles`
(servers via CMake, the Mac app, the APK's in-app server), the style in `web/mapkit`, `assets/mapgl` and
`directory/public/mapgl/vibemap-style.js`, `vibemapgl.js` in all three places, the manifest
(`MAPGL_VERSION 33407805-75-13d314b3ce3e`, so installed apps re-copy), and `vibe_web_page.h` (the pages' `?r=`).
Servers get the new pack with the package upgrade (it is bundled, not downloaded).

### Measured

| What | Before | After | Change |
|---|---|---|---|
| Basic pack | 11,054,318 B | 18,177,938 B | +7,123,620 B (+64 %) |
| Basic pack, gzip -9 (≈ what deflate would get) | 8,987,004 | 16,209,583 | the tiles are already gzip |
| Android APK (current, 164.0 MB) | — | ≈ 178.3 MB | **+14.2 MB**: the pack is Stored twice (res/ for the app, assets/mapgl for its in-app server) |
| Lite APK | — | — | +7.1 MB (the server copy only) |
| iOS app | — | — | +7.1 MB |
| App RAM with a map open | — | — | +7.1 MB: the app path reads the basic pack whole into the WebView (vibemapgl.js `o.load(... 'arraybuffer')`) |
| Android storage after first run | — | — | ≈ +28 MB: APK ×2 + the app's copy in documents/mapgl + the server's unpacked mapgl-bundled |
| z7 tile over Rotterdam (65/42) | 1,148 B | 2,354 B | |
| z8 tile over Rotterdam (131/84) | none | 4,530 B | |

Render sanity check (headless Edge, SwiftShader, M5 Pro; `scripts` bench copied from the test, Rotterdam, median of
30 forced repaints timed around `map._render`; first view = jump from a far view to Rotterdam until idle, first round is
the cold one):

| | z7 cold view | z8 cold view | z9 cold view | repaint z7/z8/z9 |
|---|---|---|---|---|
| Server page, before | 51 ms | 50 ms | 50 ms | 0.9 / 0.8 / 0.6 ms |
| Server page, after | 62 ms | 67 ms | 49 ms | 0.9 / 0.9 / 0.6 ms |
| App path, before | 49 ms | 50 ms | 50 ms | 0.9 / 0.8 / 0.5 ms |
| App path, after | 41 ms | 49 ms | 50 ms | 0.9 / 0.8 / 0.5 ms |

One cold sample per cell: the cold numbers are noise-level (±15 ms either way); repaint cost is unchanged. Not measured
on a phone, least of all a 32-bit 1 GB one: the RAM line above is the figure to judge there.

### Test

`scripts/test-server-map-water-nl.mjs` (headless Edge `--mute-audio`, a real vibeserver serving this checkout's
`/mapgl/`, prebuilt b96-keeprun binary), three phases (no detail pack; mapgl-detail-v4 250,319,197 B linked in; the app's
in-memory path):
- water at z8–11: Rotterdam Erasmusbrug (266 m), Nieuwe Waterweg (468 m, also z7), Thames at Tower Bridge and London
  Bridge (208 m), Antwerp Het Steen and the reach south (273/272 m), Saigon Bach Dang (372 m), Saigon Thanh Da (230 m,
  z9–11: at z8 it is 0.77 px wide at 10°N — the polygon is there but no pixel is blue, with the detail pack too);
- land at z8–11: Rotterdam Coolsingel, Rotterdam Zuid, Rozenburg, the City of London, Southwark, Antwerp centre, Saigon D1;
- part 1's NL points at z5–10 (IJsselmeer, Markermeer, Wadden Sea water; Lelystad, Emmeloord land — Flevoland);
- the wiring: `riverareas-b` on `vs-basic-rivers` (maxzoom 8), over-zoomed without the detail pack, maxzoom 8 with it.

Each water point was first checked inside an Overture polygon (DuckDB `ST_Contains`; widths above); each land point
inside none >= 100 m.

| Run | Passed | Failed |
|---|---|---|
| After (this branch) | 282 | 0 |
| Before (the e4f2bb9aa pack, style and renderer; detail phase not run) | 122 | 66 — all 32 river water samples per phase drew as land, plus the riverareas-b wiring check, in both phases |

### Not done / open
- The Natural Earth river LINES (`rivers-b`) still draw over-zoomed; close in they sit beside the true river area
  (visible at Rotterdam z9). Hiding them where areas exist would need per-feature data; left as it is.
- ✓ DONE in part 4: Android stored the pack twice in the APK; it now carries it once.
- ODbL: the website says the OSM-derived data is "published unrestricted with every release". Check the release step
  publishes the basic pack's river data (the pack itself in the release assets would do).

## Part 1: the lakes gap

**Cause.** The basic pack holds `lakes`, `rivers`, `islands`, `reefs` and `playas` as tier1 data for MapLibre z4–5 only
(gen-map-tiles `TIER_STEP`: from z6 the detail pack takes over). Its `land`, `cover` and `urban` layers run to z6, and the
style's `basic` source has maxzoom 6. With no detail pack, MapLibre over-zooms the z6 tiles. Those tiles contain no
lakes, so the IJsselmeer, the Markermeer and every other lake drew as land from z6 up. Decoded tile 6/32/20 over the
IJsselmeer: airports, countrylabels, grid, gridlabels, land, places, ports, roads and urban, but no lakes, rivers or
islands.

**Fix: shared renderer, style only, no pack rebuild.**
- `web/mapkit/vibemap-style.json` gets new metadata, `vibesdr:basicOnlyZ5`, listing `lakes-b`, `rivers-b`, `islands`,
  `reefs` and `playas`.
- `web/mapkit/vibemapgl.js` `prepareStyle()`: only when there is no detail pack, it adds source `vs-basic-z5`, which is
  the basic source with maxzoom 5, and points those layers at it. The z5 tiles then over-zoom. They include the lake
  islands, so Flevoland stays land. Data, order and paint are unchanged. The one pmtiles instance serves both sources,
  so no bytes are added.
- With the detail pack nothing changes: its own lakes and rivers start at z6.
- The copies were synced: `assets/mapgl/vendor/vibemapgl.js.txt` and `assets/mapgl/vibemap-style.json` for the app,
  `directory/public/mapgl/vendor/vibemapgl.js` and `vibemap-style.js` for the directory and the server bundle, and the
  `src/generated/mapglAssets.ts` manifest, so installed apps take the new copy. `check-map-overlay.mjs` passes.
  `vibe_web_page.h` was rebuilt so the pages' `/mapgl/` `?r=` revision changes.

**Test:** `scripts/test-server-map-water-nl.mjs`. It uses headless Edge with `--mute-audio` and a real vibeserver whose
`/mapgl/` comes from this checkout, and builds the map the way `mapgl.ts` does. It samples the rendered pixel at three
water points (IJsselmeer, Markermeer, Wadden Sea) and two land points (Lelystad, Emmeloord), at z5–10, in three phases:
- no detail pack;
- the detail pack (mapgl-detail-v4, 250,319,197 B) linked into the server's data directory;
- the app's path, where the packs are read whole and served from memory.

Command:
`VIBESERVER_BIN=… VIBE_DETAIL_PACK=…/vibemap-detail.pmtiles node scripts/test-server-map-water-nl.mjs [shots]`

| Run | Passed | Failed | Notes |
|---|---|---|---|
| After the fix | 96 | 0 | All three phases. |
| Before the fix (old renderer served, no detail pack) | 21 | 11 | IJsselmeer and Markermeer drew as land at z6–10. |

The vibeserver binary was a prebuilt copy of the b96-keeprun build. That is enough because the map files are read off
disk, not compiled in. The app's WebView was not run on a device; its path was exercised in the same browser
(`rangeBase` unset, packs held in memory).

## Part 2: coarse waterways in the basic pack (the measurements Stuart chose from)

Built with `node scripts/gen-river-areas.mjs --overlay=<m> --maxzoom=<z>`. Data: Overture 2026-11-18.0 river, canal,
tidal-channel and dock areas. The minimum zoom depends on width: 1500 m and wider from z5, 750 m from z6, 400 m from
z7, the rest from z8. Each run took 3–7 s from the cached parquet, with duckdb at about 2.2 GB RSS.

**How coverage was checked.**
- The test points were first checked against the Overture polygons themselves, using DuckDB `ST_Contains`. The width in
  brackets is that polygon's mean width (2·area/perimeter), which is what the cut-off filters on.
- Each built pack was then decoded with `tippecanoe-decode` and the point tested for being inside a polygon, at z5 to
  the maxzoom.
- The cut-off is per polygon, so a whole reach goes when its mean is under the cut, even where the river is wider.
- Humber note: at both Humber points the basic pack already shows sea (Natural Earth coast), so the Humber is water
  without any overlay.

| Option | Size | Rotterdam / Nieuwe Waterweg | Thames | Humber | Scheldt | Saigon |
|---|---|---|---|---|---|---|
| ≥500 m, z5–8 | 3.06 MB | Europoort only. ✗ Nieuwe Waterweg (468), ✗ Nieuwe Maas (361, 266) | Erith, Gravesend ✓ (z7). ✗ London (208) | ✓ | Westerschelde (Bath) ✓, Kallo ✓. ✗ Antwerp city (273) | Nha Be / Long Tau ✓ (z6). ✗ Saigon river at the city (372, 230) |
| ≥300 m, z5–8 | 5.05 MB | Nieuwe Waterweg ✓ (z7), Waalhaven reach ✓ (z8). ✗ Erasmusbrug reach (266) | Erith, Gravesend ✓. ✗ London (208) | ✓ | Westerschelde, Kallo ✓. ✗ Antwerp city (273) | Thu Thiem bend ✓ (z8), Nha Be ✓. ✗ north reach (230) |
| ≥300 m, z5–9 | 8.84 MB | Same as ≥300 m z5–8 | Same | ✓ | Same | Same |
| ≥200 m, z5–8 | 7.12 MB | All ✓, including the city reach (z8) | All ✓, including Tower Bridge (z8) | ✓ | All ✓, including Antwerp city | All ✓, including the north reach |
| Directory's overlay (≥150 m, z5–9, for reference) | 17.3 MB | All ✓ | All ✓ | ✓ | All ✓ | All ✓ |

Notes:
- The z9 option adds 3.8 MB and covers nothing new. Its only gain is finer edges when over-zoomed. Without the detail
  pack the coastline itself is z6 data over-zoomed, so z8 edges already exceed what the land around them shows.
- For scale: `assets/mapgl` is 25.1 MB today, and the basic pack in it is 11.05 MB.
- Not measured: render cost per tile on a 32-bit 1 GB phone, and the pack size after merging into the basic pack
  rather than shipping as a separate file. Expect roughly additive.
- Not wired: a style layer for any of these. It would need to be hidden or capped where the detail pack's
  `riverareas-d` takes over (z8+).
