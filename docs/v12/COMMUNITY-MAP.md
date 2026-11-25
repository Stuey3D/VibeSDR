# The VibeSDR community map (beta94)

Stuart, 2026-11-25: *"right now the lenovo is getting all the navtex and ndb, the rsp1b on the pi500 is handling dsc, and
the v4l does adsb. I'd like a combined world map that everyone can contribute to with a simple toggle. A link to the map
can show on the directory page and the main website too. its one map that a user can have both maritime and aviation
together or filter by mode etc"* — and later: *"we should have a button to show the contributing servers as a list then a
user can select it to listen to"*.

## RESUME HERE

| Piece | Where | Branch | State |
|---|---|---|---|
| Owner's switch `communityMap` (off) | `vibeserver_config.{h,cpp}`, `main.cpp` (3 places), setup page card, Android (`VibeServerBoot`, JNI, `vibe_android_setup.h`, `ServerModeScreen` COMMUNITY MAP, `serverSettingsMirror`) | b94-commap (v12) | built, tested |
| `GET /vibeserver/community.json` | `vibe_community.h`, `local_sdr_shim.cpp` (`communityBuild`, the route, the front door's allow-list) | b94-commap | built, tested |
| `communityMap` + `communityLayers` in the listing | shim `vibeserver.json` → `directory.cpp` / `VibeTunnel.kt` → Worker `list()` | both | tested |
| `/api/community` | `directory/src/index.js` (`communityOf`, `contributorOf`, `community`) | dir-commap (main) | tested, **not deployed** |
| The page | `directory/public/community-map.html` + `community-map.js` | dir-commap | tested headless, **not deployed** |
| Links | directory header, `website/index.html` (Listen section + footer), AboutOverlay beta94 note | both | **not deployed** |

Not proven against a real server yet (see the end).

## The design: PULL, DIRECT, CACHED

```
 browser ──(1) /community-map, /api/community──▶ directory Worker (D1, edge cache 120 s)
    │
    └──(2) https://<each contributor's own tunnel>/vibeserver/community.json ──▶ the VibeServer (cache 30 s, ETag)
```

1. The Worker **only establishes the connection** (Stuart's rule, 2026-11-24): the page HTML and one small list of who
   contributes (`/api/community`: id, name, country, the **four-character** grid and its centre, the slug address for
   LISTEN, the https tunnel URL for data, the layers, the listing's listener counts). Edge-cached 120 s, so D1 is read at
   most once per two minutes per colo whatever the traffic.
2. Every aircraft, ship and beacon is read **straight from the receiver that heard it**, over its own free tunnel —
   never through a slug, never through the Worker (`community-map.js dataUrl()` refuses anything else; the test checks it).
   CORS: `vibe_cors.h` already echoes `https://vibeserver.vibesdr.net` (the apex is one of the allowed origins).
3. The page merges in the browser: one marker per ICAO address (else the row's key), per MMSI, per NAVTEX station, per
   NDB ident + frequency, and a DSC call heard by two receivers (same position, within 2 min) once. The newest sighting's
   position wins; the card says **"Heard by N receivers"** with each one's name (a LISTEN link) and how long ago.

### Request maths

Per open, **visible** tab (a hidden tab asks nothing; it catches up when looked at again):

| What | Who pays | Rate |
|---|---|---|
| the page + its furniture (scripts, fonts, map packs by Range) | the Worker (as the directory's own front page does today) | once per visit, ~40–80 requests incl. tiles while panning |
| `/api/community` | the Worker, from the edge cache | once per load + once per 10 min |
| `community.json` from each contributor | that server's **own tunnel** (free) | once per 60 s per contributor; a failing one 5 → 10 → … 30 min |

* **Worker**: ≈ 2 list requests per 10-minute visit plus the page's furniture. 1 000 visits a day ≈ 50–80 k requests ≈
  2 M a month — inside Workers Paid's included 10 M, and **independent of how many servers contribute**.
  ★ Optional saving, NOT done here (it changes how every directory asset is served): `run_worker_first` as a pattern list
  (`["/api/*", "!/mapgl/*", "!/vendor/*", …]`) would let the static map packs and scripts skip the Worker entirely.
* **Each contributing server**: V viewers → V requests a minute over its tunnel, but **at most two builds a minute** (the
  30 s cache), and an unchanged body is a 304 (`cache: 'no-cache'` + ETag). Body sizes: the Lenovo's NAVTEX + NDB + DSC
  is tens of KB; 300 ADS-B aircraft ≈ 60 KB (600 = 116 KB, measured in test-community). 20 viewers on an ADS-B server ≈
  20 KB/s up — worth knowing on a 4G holiday server or a Pi 2's uplink.
* **N servers × V viewers**: N·V requests a minute in total, all on the servers' own tunnels; zero on the Worker.

## What a server shares (`vibe_community.h`)

One endpoint, answered by any process of the server (a radio or the front door — every source is the machine's):

| Key | Format (reused, not invented) | Caps | Layers |
|---|---|---|---|
| `aircraft` | `/vibeserver/aircraft.json` (the shared aircraft table) | positioned, ≤ 15 min old, newest 600 | adsb acars vdl2 hfdl (ADS-C rides its bearer) |
| `ships` | `/vibeserver/gallery/ais-ships.json` | heard in 6 h, 400, no tracks | ais |
| `dsc` | `/vibeserver/gallery.json?kind=dsc` | 7 days, 150 (the page keeps those with `plat`/`plon`) | dsc |
| `navtex` | `/vibeserver/gallery/navtex-stations.json` with `rx: null` | 7 days | navtex |
| `ndb` | the NDB report's `log` entry (`NdbLog`) **without `km`/`brg`** | today's + yesterday's day card, 24 h, 300 | ndb |

* `assemble()` adds sections in order and **drops** any that would take the answer past **640 KB**, naming it in
  `"dropped"`. One body per process in RAM. A section with nothing in it is left out and its layer not named — the page
  then draws no filter for it (no dead controls).
* NDB is read from the gallery's day cards with **the same reader the day log restores with**
  (`vibe_ndb_gallery.h parseCard`), placed from the published list; a listed match the server itself marks "unlikely"
  is left off the public map; an unlisted ident (no position) is left out.
* VOR MAP is not shared: it is a live, whole-radio listener mode with no store, and its beacons are fixed published
  stations — nothing a world map gains from.
* NAVTEX warnings / weather are not shared yet (the warnings index has a backfill; the stations layer is cheap and cached).

### Privacy

* **Off by default**, the owner's choice: the Setup page's *VibeSDR community map* card, the app's Server mode →
  COMMUNITY MAP, and the phone's remote settings. A config POST without the admin proof is refused (tested).
* Off → `404 {"on":false,"why":"off"}` and `vibeserver.json` says nothing, so the directory never lists it.
* A **PIN** (master or this radio's) → nothing (`"why":"pin"`): the endpoint must not walk round it.
* A **private gallery** (`galleryPublic` off) → aircraft only (exactly what `/vibeserver/aircraft.json` gives anyone).
* A mode the owner switched off is left out, as each source endpoint already does.
* Never the receiver's position: NAVTEX `rx` is null, NDB has no distance or bearing, and the page draws the receiver at
  the centre of its **four-character** grid square from the directory (`contributorOf` cuts a six-character one).

## The page

* AVIATION / MARITIME / BOTH, then a chip per mode **somebody shares**, then 15 MIN · 1 H · 6 H · 24 H · 7 DAYS.
* GPU map (the directory's VibeMapGL + the Leaflet-shaped adapter; items are GPU circles, so thousands cost one layer),
  Leaflet if no WebGL 2. Fits to what there is on first load. A stale ADS-B aircraft (> 2 min) is drawn faded.
* **RECEIVERS (n)**: a drawer (a bottom sheet at phone width) listing every contributor — name, country · grid, the layers
  it shares, how many items it has on the map now, the directory's own words for how busy it is ("free slot", "no free
  slot · 4/4 listening", "not responding") and **LISTEN** (the directory's `listenUrl`: the slug page). A row flies to and
  highlights the receiver; **ONLY THIS** narrows the map to what it heard (a chip clears it).
* Prefs (scope, modes off, age) in localStorage, try/catch'd.

## Tests

| Test | Where | Result |
|---|---|---|
| `vibeserver/test-community.cpp` (run-tests.sh, NDB group) | assemble cap/drop, aircraft filter + cap + format, NDB card → no km/brg, cache | 33/0 |
| `vibeserver/test-ndb.cpp` (DayLog restore after the `parseCard` refactor) | | 104/0 |
| `vibeserver/test-android-setup.cpp` | the switch round-trips Android ↔ page | all passed |
| `scripts/test-community-server.mjs` (needs `VIBESERVER_BIN`) | off by default, owner-only, on → bounded JSON + 304 + CORS echo, private gallery, PIN | 27/0 |
| `directory/scripts/test-community.mjs` | Worker list (real SQLite), merge, filters, link/data rules, floor | 39/0 |
| `directory/scripts/test-community-render.mjs` | headless muted Edge, 3 fake receivers, screenshots 1280 px + 390 px | 27/0 |

## Not proven yet

* Against Stuart's real servers (Lenovo NAVTEX/NDB, Pi 500 RSP1B DSC + V4L ADS-B): the formats are the servers' own and
  the server test ran a real build with a seeded gallery, but no real aircraft table or ship store has been read through
  it, and no real tunnel has carried it.
* A multi-radio front door answering it (the door serves the gallery and the runtime aircraft file — expected to work,
  not run).
* Android's own server (built: the JNI + Kotlin compile was not run here).
* The Worker and page are not deployed; `/community-map` relies on Workers Assets' `.html` handling (as `/store` does).
