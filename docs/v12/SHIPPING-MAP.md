# The SHIPPING MAP (V12)

Branch `v12-shipping-map` (from `v12-experimental`), 2026-11-02.

Stuart (2026-11-02): *"rather than separate Navtex maps why dont we integrate Navtex with DSC and AIS and have a complete
shipping overview. In theory a user with a multi SDR server could have one set for Navtex and another for AIS and or DSC
and then you will see all ships and forecasts etc on one map. Also rather than just the labels is it possible to have
arrows in the direction of the weather/sea state so E 4-6 have the arrow pointing and being bigger or smaller and red for
severe that way it looks even more like a full shipping weather and hazard breakdown."*

## What it is

ONE map, titled **SHIPPING MAP**, in the web client and the app, with layers switched on and off:

| Layer | From | Notes |
|---|---|---|
| **SHIPS** | the live AIS report (a listener in AIS mode) and the gallery's **ship store** (`/vibeserver/gallery/ais-ships.json`) | live wins per MMSI; a ship last heard over an hour ago is faded; a stored ship's card and short track |
| **DSC** | the DSC box's calls with a position, and the gallery's pinned DSC alerts (`gallery.json?kind=dsc`, `plat`/`plon`) | a map-pin glyph; DISTRESS red, bigger, ringed, "SOS", never decluttered away; URGENCY orange |
| **STATIONS** | `navtex-stations.json` | unchanged |
| **WARNINGS** | `navtex-warnings.json` | unchanged (icons, shapes, clusters, latest wins) |
| **WEATHER** | `navtex-met.json` | now **wind arrows** (below); an older server's forecast keeps its pill |

- **No dead controls:** a layer this server has no data for is ABSENT (no toggle). The layer the map was opened for is
  always there and first; one layer alone draws no toggle row. (`shippingMap.ts shipLayersPresent`.)
- **Opened from:** the NAVTEX box's MAP (NAVTEX first) · a DSC call's MAP (DSC first, on that call — the web used to open
  a pop-up window) · AIS mode (its own map, SHIPS first) · the gallery: the standing "SHIPPING MAP · NAVTEX stations
  received" row, MAP on a DSC call with a position (on that call), MAP on an AIS day (ships first).
- **AIS mode's map is the same map:** its live ships exactly as before (EXIT AIS gives the receiver back, a live ship's
  trail from the server), plus the stored ships it is not hearing now, DSC and the NAVTEX layers, and the time filter.
- The **time filter** (1 h / 24 h / 7 d / 30 d / all) applies to every layer; the **RECEIVED** list has a section per
  layer shown; tapping any marker or row opens its card; the **KEY** lists the ship, DSC, hazard and arrow glyphs.

## The weather arrows (`vibe_navtex_met.h windOf`, `src/services/navtexWind.ts`)

- Read on the SERVER from the same wind sentence the label is read from (`windSentence`, lifted out of `read()`
  unchanged). In `navtex-met.json` each forecast gains `"wind":{k,bf,sev,from,to,chg,k2,bf2,sev2,from2,to2}`.
- The first compass word is where the wind comes FROM (16 points; NORTHERLY … NORTHWESTERLY); the arrow points
  **DOWNWIND** (`to` = from + 180: "SW 6-8" points north-east). "N OR NW" is the point between.
- **Force = the upper bound** of the first wind ("4 TO 6, OCCASIONALLY 7" → 7; NEAR GALE 7, GALE 8, SEVERE GALE 9,
  STORM 10, VIOLENT STORM 11, HURRICANE 12). Length 16 px (calm) … 52 px (hurricane), weight likewise.
- **Colour:** the map's ink below 7, **amber** at 7, **red** at gale 8+; a gale the arrows do not carry (another sentence,
  a native-language gale word) makes the first arrow red.
- **VARIABLE** = a dashed ring "VRB"; **CYCLONIC** = a curling arrow. Never an arrow guessed.
- **A change** (VEERING / BACKING / BECOMING / BECMG / THEN, after the first direction) = a fainter dashed arrow for the
  later wind, behind the first, coloured by its own force. No direction after the change word = no second arrow.
- Beside it the compact label "6–8 · ROUGH" (· FOG), the area's name above; the marker is centred on the arrow.
- Two parser fixes found while checking arrows against labels on the Lenovo set: "4-5" read only its first figure, and
  "NEAR GALE 7" was flagged GALES. `vibe_navtex_warn.h kVersion 7` rebuilds the met index once per server (the existing
  backfill — the map says "Building the map from N saved messages…").

## The SEA AREAS (branch `v12-sea-areas`, 2026-11-02)

Stuart (2026-11-02): *"can we add the shipping lanes and shipping zones to the map too?"* … *"worldwide if possible"* …
*"the things like Dogger, Wight Plymouth etc"*.

- **One dataset, three readers:** `scripts/gen-sea-areas.mjs` (sources + licences in its header) →
  `src/utils/seaAreas.generated.ts` (web client and app) and `android/app/src/main/cpp/vibe_sea_areas.generated.h`
  (the met parser). Its inputs: `scripts/mapdata/sea-areas/uk-shipping.json` (the UK corner points, typed in),
  `fetch.sh` (the downloads, not committed) and `sea-areas.cache.json` (the processed polygons, committed — `--check` and
  a rebuild of the readers need no network). Not GPU tiles: `gen-map-tiles.mjs` builds the 243 MB basemap pack that
  ships separately; a forecast overlay that changes with the forecast does not belong in it.
- **Sources and licences:**

| Region | Source | Licence | What |
|---|---|---|---|
| UK (31 areas, Viking … SE Iceland, Trafalgar) | Met Office boundary corner points (2002 revision), typed from M. Stubbs' tabulation of them; Wikipedia only to corroborate single entries | facts (coordinates); coast between two corners on one coast from Natural Earth 1:10m land (public domain) | polygons, our label points |
| US (564 zones) | NOAA NWS marine zones, coastal `mz16ap26` + offshore `oz16ap26` | public domain | Great Lakes and the 5 high-seas basins left out |
| Canada (158 areas) | ECCC MSC Geography Package 6.15.0, `water_MarStdZone_coarse` | Open Government Licence – Canada | Atlantic, Pacific, Arctic; Great Lakes and inland waters left out |
| Germany (7 areas) | DWD `dwd:Warngebiete_Kueste` (GeoServer WFS) | GeoNutzV (Datenlizenz Deutschland – Namensnennung 2.0), © GeoBasis-DE / BKG 2021 | the Elbe river stretch left out |

- **Not sourced (and why):** UK inshore waters and the Irish coastal areas (Met Éireann) — headland-to-headland strips out
  to 12 / 10 NM; the headlands are known but a coastal buffer we trust is not built yet, so they keep their label points
  only · Australia (BoM IDM00001/3) — no Creative Commons statement on the spatial files, and the Bureau's default terms
  forbid supplying them on or commercial use · Norway (MET Norway), the Netherlands (KNMI), France (Météo-France) and
  DWD's offshore Seegebiete (Deutsche Bucht, Westliche Ostsee …) — no open boundary set or published corner list found.
- **Size:** `seaAreas.generated.ts` 213 KB (UK 8.6 KB of outline, US 80 KB, CA 58 KB, DE 1.5 KB; 0.001° polylines,
  Douglas–Peucker 0.012–0.02°); `vibe_sea_areas.generated.h` 107 KB (names and match forms only).
- **Drawing (`src/services/seaAreas.ts`, through `tracklayer.js`):** an item per area — its name the icon (small capitals,
  `_y` yields to every other icon and name, `_mz` from zoom 4 UK / 5 offshore / 8 US coastal), its outline the shape (thin,
  several rings, a US coastal zone's from zoom 6), `_nofit` (never widens the first view, never pulses, counted apart in
  `data-vtl-chart`). An area with a current forecast has no marker of its own: the forecast's arrow (moved to the area's
  label point, inside its outline — `seaPlaceMet`) names it. Tint: **amber** at force 7, **red** at a gale 8+ (forecast
  words, force, or either arrow) — only while WEATHER is drawn, and never from a stale forecast.
- **Its own toggle, SEA AREAS, after WEATHER** — not inside it: the outlines are geography (which area is that ship in?),
  useful with no forecast; the tint is the weather's. **Present only where an area is within reach** (`seaAreasNear`: the
  receiver within 2600 km of a UK area, 2000 of an offshore one, 700 of a US coastal zone, or a forecast names one) — a
  server in Japan gets no toggle.
- **KEY:** the outline, amber and red rows and the sources line (web `shipKeyHtml`, app MapOverlay legend).
- **Matching (`vibe_navtex_met.h`, extended, not a second table):** `areas()` appends the generated areas after the
  hand-made UK rows (a name both carry stays ours); a first-word index serves the heading match. An NWS bulletin's UGC
  line names its zones (`ANZ800-031000-`, ranges with `>`); winds in **knots** are converted to Beaufort (`knotsBf` —
  "10 TO 15 KT" is force 3–4, never 10); seas in feet; `.TODAY...` is read, `.TONIGHT...` ends the block. Generic
  names (CENTRAL, EAST COAST …) are never headings. `vibe_navtex_warn.h kVersion 8` re-reads every server's forecasts once.

## The AIS background profile and the ship store (server)

- **Setup → the radio → Background decoding → "Decodes: AIS ships (161.975 + 162.025 MHz)"** (`"ais":true`), beside ISM's
  whole band. Offered only where AIS can run (not an HF+, not an Icom, not a locked radio, AIS not switched off); refused
  at the save in words otherwise (`vibe_bgdecode_config.h validate`, `aisOk`).
- With nobody here (after the owner's wait) the shim enters the listener's own AIS mode (`bgAisStartLocked → aisEnter`,
  origin "background"); a listener arriving ends it through `aisLeave` (listeners win — the governor's rule); never over
  another whole-radio mode; the admin line gains `"ais":{ships,storeBytes}`.
- **The ship store** (`vibe_ais_store.h`): `<gallery>/ais-ships.dat` — 64 B header, a directory (cap × 8 B), fixed 512 B
  records (latest state per MMSI + the last 32 track points, one per 2 min or 300 m). **Bounded**: cap = a 32nd of the
  owner's gallery space, at most 2048 ships (1 MB at the default 100 MB); full = the ship heard longest ago gives its
  slot up; grows only as slots are used; a smaller gallery restarts it smaller; gallery size 0 removes it. **Counted inside
  the gallery's size** (`Store::sideBytes` — usedBytes, gallery.json and eviction; `Store::fit()` after a write).
  **Written incrementally**: every 30 s and when AIS stops the service copies out the ships that changed (state + trail
  points since) and writes them with its lock released (one pread + pwrite each + the directory) — the DSP thread never
  waits on disk. **Every radio on the machine** writes and reads it under `flock(<gallery>/.lock)`, reading the directory
  fresh each time, so two AIS radios merge (newest position wins, static fields kept, both tracks in time order).
- Kept on the day log's terms: only where the gallery keeps anything; a listener's AIS only while listeners' decodes are
  kept. **Blockable:** "AIS: keep the ships for the SHIPPING MAP (in the gallery)" (`aisstore`) nested under AIS in Modes
  and decoders (setup page + ServerModeScreen.tsx, one list); the server neither writes nor serves it when off.
- `GET /vibeserver/gallery/ais-ships.json?since=<s>&range=<km>&tracks=1&limit=` — the live report's vessel shape plus
  `stored:1` and `pts:[[lat,lon,ageSec]…]`; `"off":1` when AIS or the store is switched off.

## Data flow

```
radio A (NAVTEX bg)  ──► gallery items + navtex-warn.idx / navtex-met.idx ─┐
radio B (AIS bg/live) ─► AIS service ─30 s─► ais-ships.dat (flock) ───────┤  shared <gallery>/
radio C (DSC)        ──► gallery DSC alerts (plat/plon) ──────────────────┘
                                         any radio's HTTP: navtex-*.json, ais-ships.json, gallery.json?kind=dsc
                                                          │
                     shippingMap.ts (one rule) ◄──────────┘──► web main.ts shipCompose / app SDRScreen shipCompose
```

## Files

`vibe_ais_store.h` (new), `vibe_ais_service.h`, `vibe_gallery.h`, `vibe_bgdecode_config.h`, `local_sdr_shim.cpp`,
`vibe_setup_page.h`, `vibe_navtex_met.h`, `vibe_navtex_warn.h`, `vibeserver/main.cpp`; `src/services/shippingMap.ts`
(new), `src/services/navtexWind.ts` (new), `navtexWarn.ts`, `src/components/MapOverlay.tsx`, `GalleryOverlay.tsx`,
`src/screens/SDRScreen.tsx`, `ServerModeScreen.tsx`, `AboutOverlay.tsx`; `web/client/src/main.ts`, `tracks.ts`.

## Tests (this Mac, 2026-11-02)

| Suite | Checks |
|---|---|
| `vibeserver/test-ais-store.cpp` (new) | 52 — the profile config; round trip; merge; bounded (200→64 slots, a 500-ship batch, exact file size, shrink, size 0, corrupt file); since/range/limit; **four processes writing at once** under the lock; inside the gallery size (fit evicts); the service hands over only what changed |
| `vibeserver/test-navtex-met.cpp` | 58 (+25 wind arrows); Lenovo set: 938 arrows + 68 VRB/CYC for 1,008 winds, 423 with a later wind, arrow force = label upper force everywhere |
| `scripts/test_shipping_map.ts` (new) | 47 — layer presence, ships live + store, DSC pins, time filter, KEY, arrows (SVG, label, words, marker) |
| `scripts/test-server-ais-aprs.mjs` | 36 (+13) — the background AIS profile end to end; the store after a listener's AIS |
| `scripts/test-web-shipping-map.mjs` (new) | 21 — headless Edge `--mute-audio` |
| `scripts/test-web-navtex-map.mjs` | 48 (fixture times moved so it no longer rots with the calendar) |
| `scripts/test-web-ais-aprs.mjs`, `test_navtex_map.ts`, `test_ais_aprs_app.ts`, `test-navtex-warn`, `test-navtex-map`, `test-gallery-store`, `test-ism-gallery`, `test-bgdecode`, `test-ais` | unchanged, pass |

## RESUME HERE

- **SEA AREAS (branch `v12-sea-areas`, from `v12-experimental` c967cb58e, 2026-11-02) — built and tested on this Mac:**
  dataset + generator, the met parser's table extended (UGC, knots, feet), web + app drawing, KEY, About, tour copy.
  Tests: `test-navtex-met` 80 (+21 matching; the fixture set reads exactly as before), `test_sea_areas.ts` 38 (new),
  `test-web-shipping-map.mjs` 28 (+6, screenshots `sea-areas-uk.png`, `sea-areas-close.png`), `test-web-navtex-map.mjs`
  48, `test_shipping_map.ts` 48, `test_navtex_map.ts` 55, `test-navtex-warn` 113, `test-navtex-map` 58. A server built
  from the worktree (`vibeserver/build`, deps from `~/VibeSDR-v12/vibeserver/mac/deps/prefix`).
  **Owed:** an app device run (tsc clean; the WebView gets the same items); a run over the Lenovo's real gallery (the
  index rebuild at kVersion 8, and whether any European forecast's figures now read as knots — only a figure followed by
  KT/KNOTS is); the iOS prebuilt DSP lib (`build_ios.sh`) for the iPhone's own server; the mapdata/apt side needs nothing.
  **Not built:** UK inshore and Irish coastal polygons (a 12 / 10 NM strip between headlands), Norway/KNMI/Météo-France/
  DWD offshore (no open data found), BoM (licence), shipping LANES (Stuart's first ask — traffic separation schemes are on
  charts, not open data we found; not attempted). At zoom 5 a gale area's arrow can give way to a station's name (the
  existing rule: weather yields first) — the red tint still shows; consider letting a GALE arrow outrank station names.
  The sources sit at the foot of the KEY, not on the basemap's attribution control.
- **Owed, not done:** an app device run (tsc is clean; the app was not run on a phone — the MapOverlay legend is one
  static SHIPPING legend for kinds navtex/ais/dsc, not trimmed to the layers present as the web's KEY is); a visual
  review of the arrows at close zoom over the UK (the headless test checks they are drawn and rotated; the declutter
  lets weather give way first, so zoomed out few arrows show); the app's store ships carry no stored track (the app
  asks `ais-ships.json` without `tracks=1` — navGet appends `?since=`; add the flag there).
- The app's `dscMapOpen` / MapOverlay kind 'dsc' path is now unused (DSC's MAP opens the shipping map) — remove once a
  device run confirms.
- The NAVTEX indexes (`navtex-warn.idx`, `navtex-met.idx`) are still NOT counted in the gallery size — only the ship
  store is (`Store::sideBytes` lists side files; adding them there is one line, but changes NAVTEX's proven accounting).
- Not built: an APRS layer; per-radio attribution of a stored ship (which radio heard it); a DSC layer from the DSC day
  logs (only pinned alerts and the box's own calls carry positions today).
- Nothing pushed, merged, published or triggered. The worktree's server was built with its `vibeserver/mac/deps`
  pointed at `~/VibeSDR-v12`'s prefix (FAAD2 is not in the main checkout's deps; the link has been removed); an ignored
  `node_modules` symlink to the main checkout's is left in the worktree for tsc / tsx.
