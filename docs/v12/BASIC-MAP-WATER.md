# Basic map water (beta96)

The GPU map without High Detail Maps: lakes from z6 up (fixed), and what coarse waterways in the basic pack would cost
(measured, not added).

## RESUME HERE

- **Part 1 is done** on branch `b96-basicwater`: the lakes stay water from z6 up on every map without the detail pack.
  Commits cae1cdfd2 (fix + synced copies), 6ebff9300 (test), 55ced8eec (vibe_web_page.h rebuilt via the lock).
- **Part 2 is measured. Nothing was added** to the basic pack, the APK or the assets. Stuart picks from the table below.
- Once this reaches main, the directory's stopgap `keepZ5Layers()` in `directory/public/mapgl/vibemap-water.js`
  (main dc449eb7b) stands down by itself: the shared renderer has already added `vs-basic-z5`, the source id that
  function checks for. It can be deleted later. It does no harm if it stays.

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

## Part 2: coarse waterways in the basic pack (measured only)

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
