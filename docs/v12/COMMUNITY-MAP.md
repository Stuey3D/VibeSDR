# The VibeSDR community map (beta94)

Stuart, 2026-11-25, in the order he asked:

- *"I'd like a combined world map that everyone can contribute to with a simple toggle. A link to the map can show on the
  directory page and the main website too."*
- *"we should have a button to show the contributing servers as a list then a user can select it to listen to"*
- *"the community map needs all the maritime details from the navtex map we have"*
- *"anything that we map now should be able to be contributed to the community map so sondes etc too"* — *"amateur spots
  excluded"* — *"ism is only showing meshcore stations anyway"*
- *"all layers can be on at once aviation/maritime/misc"*
- *"we need to optimise everything so it doesn't get too overloaded and takes an eternity to load, navtex limited to last
  24h maybe?"*
- *"we can use existing data that is on the servers too so my navtex is already there use it, meshcore if i run the ism
  decoder as these are now remembered"*

## RESUME HERE

| Piece | Where | Branch | State |
|---|---|---|---|
| Owner's switch `communityMap` (off) + per-mode `communityOff` | `vibeserver_config.{h,cpp}`, `main.cpp` (3 places), Setup card, Android (`VibeServerBoot`, JNI, `vibe_android_setup.h`, `ServerModeScreen` COMMUNITY MAP + mode chips, `serverSettingsMirror`) | b94-commap (v12) | built, tested |
| `GET /vibeserver/community.json[?s=]` + `/vibeserver/community/navtex?f=` | `vibe_community.h`; `local_sdr_shim.cpp` (`communityBody`, `communitySection`, `communityNavtex`, `communityLiveTick`, the route, the door's allow-list) | b94-commap | built, tested |
| Remembered sondes / APRS / VOR rows | `vibe_rs41_service.h communityRows`, `vibe_aprs_run.h CommunityTable`, `vibe_vormap_service.h communityRows`, `<gallery>/.community/live-*.json` | b94-commap | tested (stored-file half) |
| `communityMap` + `communityLayers` in the listing | shim `vibeserver.json` → `directory.cpp` / `VibeTunnel.kt` → Worker | both | tested |
| `/api/community` | `directory/src/index.js` | dir-commap (main) | tested, **not deployed** |
| The page | `directory/public/community-map.html` + `community-map.js` + `community-kit.js` (generated: `node scripts/build-community-kit.mjs <dir>/directory/public/community-kit.js` on v12) | dir-commap | tested headless, **not deployed** |
| Links | directory header, `website/index.html`, AboutOverlay beta94 note | both | **not deployed** |

## The design: PULL, DIRECT, LEAN, LAYERED

```
 browser ──(1) /community-map, /api/community──▶ directory Worker (D1, edge cache 120 s)
    │
    ├─(2) https://<each contributor's tunnel>/vibeserver/community.json?s=<layers on> ──▶ VibeServer (per-section cache, gzip, ETag)
    └─(3) …/vibeserver/community/navtex?f=<item>   only when a viewer opens a NAVTEX message
```

1. The Worker **only establishes the connection**: the page and one small list of contributors (id, name, country, the
   **four-character** grid and its centre, the slug address for LISTEN, the https tunnel for data, the layers, listener
   counts) — edge-cached 120 s.
2. Every item comes **straight from the receiver** over its own free tunnel — never a slug, never the Worker
   (`dataUrl()` refuses anything else; a test checks it). CORS: `vibe_cors.h` echoes `https://vibeserver.vibesdr.net`.
3. **Layered:** the receivers are listed and the map drawn at once; each receiver is fetched in parallel (≤ 6 in flight,
   8 s timeout, back-off 5 → 30 min) and drawn as it arrives — a dead one never holds up the rest.
4. **Only what is switched on** is asked for (`?s=adsb,ais,…`); a chip switched on asks for itself then.
5. **Cadence:** live layers (aircraft, ships, DSC, APRS, MeshCore) every 60 s, slow ones (NAVTEX, NDB, VOR, sondes)
   every 5 min, nothing while the tab is hidden.
6. **GPU:** MapLibre GeoJSON layers; aircraft / ships / APRS / MeshCore clustered below zoom 7; the servers' own SVG icons
   rasterised once each; warning areas and routes and sonde tracks as line / fill layers; only the forecast pills (a few
   dozen, from zoom 4) are DOM.

### What a server shares (`vibe_community.h`) — from what it STORES

| Layer(s) | Key | Source (stored) | Window · cap | Row |
|---|---|---|---|---|
| ADS-B ACARS VDL2 HFDL | `a` | the shared aircraft table | positioned ≤ 5 min · 600 | `[key, lat, lon, age, alt, trk, gs, srcMask, flight, reg, type]` |
| AIS | `s` | the ship store | 6 h · 300 | `[mmsi, lat, lon, seen, sog, cog, family, kind, name, call]` |
| DSC | `d` | gallery DSC items with a position | 24 h · 150 | `[t, lat, lon, level, title, file]` |
| NAVTEX | `ns` `nw` `nm` `nx` | the gallery's NAVTEX log | 24 h | stations (navtex-stations.json, rx null) · warnings (navtex-warnings.json, ≤ 300) · forecasts (navtex-met.json, newest per area, **no text**) · messages `[id, kHz, first, last, n, lost, station, conf, iso, file, title]` (≤ 300 / 220 KB, one per id, the cleanest copy) |
| NDB | `b` | the gallery's day cards (today, yesterday) | 24 h · 300 | `[ident, hz, lat, lon, last, name, cc]` |
| VOR | `o` | remembered rows (the VOR map) | 24 h · 200 | `[ident, hz, lat, lon, last, name, cc, kind]` — no radial, bearing or distance |
| Sondes | `z` | remembered rows (the sondes mode) | 12 h · 32 | `[serial, type, lat, lon, alt, climb, last, maxAlt, temp, track]` (≤ 60 points) |
| APRS | `p` | remembered rows (every APRS run with a position) | 6 h · 300 | `[key, from, lat, lon, t, sym, kind, line]` |
| MeshCore | `m` | the gallery's ISM nodes cards (remembered) | 24 h · 300 | `[id, name, type, lat, lon, last, count, approx]` |

* **Stored, not only live:** a mode that is not running still shows its window — the NAVTEX log, the ISM nodes cards, the
  NDB day cards, the ship store and the aircraft table were already stored; sondes, APRS and VOR keep what each radio last
  decoded beside the gallery (`<gallery>/.community/live-<section>-<radio>.json`: written only when changed and at most
  every 2 min, never per message; a file untouched for 2 days is removed).
* **Text on demand:** NAVTEX message text is never in the poll; `/vibeserver/community/navtex?f=` gives one live NAVTEX item
  of the gallery (looked up, never a path), cached an hour.
* **Excluded:** amateur digital spots (PSK Reporter has them); every ISM device but MeshCore nodes (sensors, meters, tyres
  live at people's homes); the receiver's position (NAVTEX `rx` null, no distances, the ISM card's `Receiver:` line never
  read out).
* **Bounds:** each section its own window and cap; `assemble()` drops any section that would take the answer past 640 KB
  and names it in `"dropped"`; each section built at most once per TTL (30 s live, 120 s slow) per process.

### Measured

* **A real NAVTEX day** (Stuart's Lenovo, 2026-10-30, 466 receptions, `scripts/measure-community-navtex.mjs`): 30
  stations, 121 warnings, 51 sea areas, 280 messages = **118 KB raw, 26 KB gzipped**; a rebuild 77 ms (M5 Pro), from the
  cache 2 ms. (A Pi 2 will be tens of times slower — at most once per 2 min, off the DSP threads.)
* The test server's stored answer (61 real NAVTEX messages, DSC, NDB, MeshCore, APRS, a sonde): 38.8 KB raw, **9.6 KB
  gzipped** (25 %).
* 600 aircraft: 35.8 KB raw, **7.0 KB gzipped**.
* So a typical busy receiver (ADS-B + a NAVTEX day): ~7 KB a minute + ~26 KB every 5 min, gzipped — inside the 50–80 KB
  target, and a 304 when nothing changed.
* The page, headless Edge on software GL, 10 fixture receivers: receivers listed 50–600 ms, every receiver's data in
  0.2–1.1 s, JS heap 19–29 MB; the GPU basemap's first draw 1–30 s **in software GL** (a real GPU: about a second).

### Request maths

Per open, **visible** tab:

| What | Who pays | Rate |
|---|---|---|
| the page + scripts (incl. community-kit.js, 50 KB) + map packs | the Worker (as the directory's own page) | once a visit, ~40–80 requests with panning |
| `/api/community` | the Worker, from the edge cache | once a load + once per 10 min |
| `community.json` per contributor | that server's own tunnel (free) | ≤ 1 a minute (live layers) + 1 per 5 min (slow), only for layers on and shared |
| a NAVTEX message's text | that server's tunnel | only when opened; cached an hour |

* **Worker:** independent of contributors and of polling — 1 000 visits a day ≈ 50–80 k requests ≈ 2 M a month, inside
  Workers Paid's 10 M. (Optional, not done: `run_worker_first` as a pattern list would take the static map packs off it.)
* **Each contributing server:** V viewers → ≤ 1.2 V requests a minute, but at most 2 builds a minute for live sections
  and 1 per 2 min for slow ones whatever V is; ~9 KB a minute per viewer gzipped for a busy receiver (less when 304).
  20 viewers ≈ 3 KB/s up.
* **N servers × V viewers:** ≤ 1.2 N·V requests a minute, all on the servers' own tunnels; zero on the Worker.

## Privacy

* **Off by default**, the owner's choice; under it a tick box per mode (default: all the server runs) — Setup page, the
  app's Server mode, the phone's remote settings. A config POST without the admin proof is refused (tested).
* Off → `404 {"on":false,"why":"off"}` and `vibeserver.json` says nothing (never listed as a contributor).
* A **PIN** → nothing. A **private gallery** → aircraft only. A **blocked** mode or one **left out** → absent.
* A receiver is drawn at the centre of its four-character grid square; a MeshCore companion (a person) is rounded to ~1 km.

## Tests

| Test | Result |
|---|---|
| `vibeserver/test-community.cpp` (run-tests.sh, NDB group) — assemble cap, aircraft rows, NDB card, MeshCore card (MeshCore only, person rounded, no receiver line), NAVTEX fold (cleanest copy) and titles, remembered rows (two radios, torn file, windows), per-mode choices, `?s=` | 55/0 |
| `vibeserver/test-ndb.cpp` (DayLog restore through `parseCard`) | 104/0 |
| `vibeserver/test-android-setup.cpp` (switch + modes round-trip) | all passed |
| `scripts/test-community-server.mjs` (VIBESERVER_BIN) — off by default, owner only, **stored NAVTEX + remembered MeshCore / APRS / sondes with no decoder running**, cleanest copy, text on demand, `?s=`, gzip, ETag/304, CORS, communityOff, private gallery, PIN | 40/0 |
| `directory/scripts/test-community.mjs` | 53/0 |
| `directory/scripts/test-community-render.mjs` (muted headless Edge, screenshots) | 42/0 |

## Not proven yet

* Against Stuart's real servers and real tunnels (everything ran on this Mac).
* AIS ship rows from a real ship store (the format is the store's; no store was seeded in a test), and live ADS-B from a
  running decoder (the shared table's rows were unit-tested).
* The sondes / APRS / VOR **writers** on air (the remembered-file reader and the rows' builders are tested; a running
  sondes or APRS decoder writing them was not).
* A multi-radio front door answering it; Android's own server (JNI + Kotlin not compiled here).
* A Pi 2's NAVTEX rebuild time.
