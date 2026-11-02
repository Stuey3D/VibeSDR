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
