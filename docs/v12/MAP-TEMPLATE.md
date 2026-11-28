# THE MAP TEMPLATE (beta96, Stuart 11-27)

Stuart: *"once done these maps become the template for all mapping going forward"*. Every map — ADS-B, datalink,
AIS/ships, sondes, APRS, the ISM mesh, the amateur/spots map, satellites, the community map — is built from the same
parts and passes the same tests. The web page first; the app's MapOverlay WebView uses the same `web/mapkit` modules,
and its React Native chrome follows the same rules (docs/v12/MAP-CONTROL-CARDS.md, "The app port").

## RESUME HERE

- Branch `b96-mapcards`. The web page is the reference; the AIR & SEA map (`web/client/src/tracks.ts` TrackView,
  `web/mapkit/adsblayer.js`) is the first map built to it.
- Owed: a second map moved onto `VibeGpuMarkers` (ships next) — then split the engine out of adsblayer.js into its own
  `web/mapkit/gpumarkers.js` (one more entry in `scripts/gen-adsblayer-source.mjs`'s and `scripts/sync-mapgl-assets.mjs`'s
  lists); labels to a real glyph atlas (today one bitmap slot per aircraft); the selected aircraft's trail tail on the GPU.

## The rules

1. **GPU first. Anything that moves is drawn by the GPU.** A moving marker, its label and its ring go up as vertex
   attributes ONLY when the data changes; the vertex shader dead-reckons, rotates and fades between. A moving thing is
   never re-fed to MapLibre with `setData` per frame. Static things (stations, airports, sea areas, range rings, the
   outline) stay ordinary MapLibre layers updated at the data rate.
2. **Frames only when something visibly moves.** The ambient rate ADAPTS: fast enough that nothing on screen steps more
   than ~1.5 px a frame (5 fps far out, the display's own rate up close); a glide / turn at the rate its own pixel speed
   needs (turns under 3° snap); a fade / ring / label easing in at 12 fps; up at once, down after 1 s. Nothing moving on
   screen → **no frames at all**. A finger on the map is MapLibre's own loop, at the display's full rate.
3. **Idle budget:** an untouched map with ~150 aircraft costs **≤ 2 %** of a core in Safari (the whole radio page is
   3–4 %). Headless Edge on Metal is the bench: main thread ≤ 6 %, GL frames ≤ 30/s with ambient motion, ≈ 0 still.
4. **Nothing under a map runs.** A full-screen map in the same page parks the radio page's render loop and tells the
   server to stop sending this listener spectrum (`{"type":"spec","on":0}` → `specPaused` in the shim; `on:1` when it
   closes — the waterfall carries on from there, a short gap in its history is fine). CSS animations outside the map
   pause (`body.trkMapUp`). A map opened in a NEW TAB leaves the radio tab alone (its own hidden-tab rules apply: the
   page tells the server `presence: background`; the spectrum keeps flowing and the browser throttles the drawing).
   **The app:** the NON-live viewer over the radio screen keeps the spectrum flowing and its history ingesting, as it
   does today (so closing the map shows an unbroken waterfall); a LIVE map mode follows the web rule (spectrum off,
   nothing drawn).
5. **Memory:** MapLibre `maxTileCacheSize: 60` a source, `pixelRatio ≤ 2`; every per-aircraft store bounded (the GPU
   buffers by the aircraft present, STATS ≤ 3 000 a mode and forgotten after 10 min, trails bounded). Flat over time.
6. **The chrome** (MAP-CONTROL-CARDS.md "What is where"): four CORNER PIECES — the red ✕ top-left, RATE top-right, LIST
   bottom-left, the ? and the scale bottom-right; the shared-tuner pill top-centre; edge chips (SERVERS, TUNE, LAYERS,
   STATS left; CHAT, MAP right) that pop out TRANSIENT cards (one at a time; fold on a map tap, another card, 9 s idle,
   Escape / BACK); LIST the one sticky card — a drawer, a phone's full width else `clamp(420px, 30vw, 640px)`, 22 % of
   the viewport in whole rows (3–7), one bar at its foot (chip · tabs · ‖ +N · sort), nothing ever over a row, MAXIMISE
   on its folder tab. No top bar. Safe areas inside every corner piece; the pad reaches everything in reading order
   (✕, SERVERS…, CHAT); Escape un-maximises, then folds, then exits.
7. **No dead controls** (AGENTS.md): a card or chip with nothing in it is not offered; one source makes RATE a reading,
   not a button.

## How to add a map

1. Host it in a `TrackView` (tracks.ts): `slot('tune')` / `slot('ctl')` for its controls, `setFacts`, `setRateRows`,
   `setSources`, `setStats`, `setLayerSummary`, `setChat` — each card appears when it has something.
2. Moving markers: `VibeGpuMarkers.create(map, { iconBox, dpr, beforeId, iconFor })` and `set(markers)` on each data
   change (`{ id, lat, lon, t, gs, track, maxDrMs, key, hdgFrom, hdgTo, hdgT0, corr, shownAt, goneAt, bornAt, rgb,
   label: { l1, l2, c2, col, halo, at }, z }`); `pick(point, r)` on a tap. If `ok` is false (or it fails over), draw
   with symbol layers at the throttled rate — never leave markers invisible.
3. Static layers: ordinary MapLibre sources, `setData` at the data rate.
4. Add it to `trackViewUp()` if it covers the radio page.

## Tests every map must pass

- **Perf budget** — `scripts/test-web-map-perf.mjs <url>`: 30 s idle → GL frames ≈ 0 with nothing moving, ≤ 30/s with
  ambient motion, main thread under budget; no `styleimagemissing` (window.__vsMissingImages empty).
- **Layout** — `scripts/test-web-map-cards.mjs`: no header; the corner pieces where they belong; pill / ✕ / RATE / CHAT /
  zoom never overlap; LIST widths (390 → full, 1440 → ≥ 420, 2560 → ≥ 560) and whole rows; nothing over a row; the age
  and the pill whole at 375 / 390 / 1440; the ? / scale / (i) / maximise tab apart; step-aside exactly when covered.
- **Under the map** — the radio page's spectrum frames → 0 within 2 s of a same-page map opening, back within 2 s of
  it closing (`scratchpad spec.mjs` is the measurement; to be folded into the e2e).
- **Pad reach** — scripts/test-web-nav-coverage / gamepad, as for every page.
