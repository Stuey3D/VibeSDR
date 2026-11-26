# Basic map water (beta96)

The GPU map without High Detail Maps: lakes from z6 up (part 1), and the rivers >= 200 m in the basic pack (part 3,
approved by Stuart 11-26 from the measured options in part 2).

## RESUME HERE

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
- Android stores the pack twice in the APK (and twice more on disk once unpacked). Sharing one copy between the app and
  its in-app server would save ~18 MB APK and ~18 MB storage, but is a packaging change, not part of this job.
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
