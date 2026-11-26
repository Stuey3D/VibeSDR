# TWO LIVE MAPS: AIR & SEA, and AMATEUR (V12, beta96)

Branch `b96-onemap` (from `v12-experimental` = beta95 c7fbde793), 2026-11-26.

Stuart (2026-11-26): *"b95 is out so do this as the main b96 change"*, which approved ONE LIVE MAP: ships, aircraft and
the other mapped modes together, as the community map already shows them.

Stuart, later on 11-26 (design change, via the coordinator): **TWO maps, not one.**

1. **AIR & SEA map** (shown as "AVIATION & MARITIME"). Holds ADS-B, ACARS, VDL2, HFDL (+ ADS-C), NDB, VOR, AIS, DSC,
   NAVTEX, sea areas and **radiosondes**. Sondes are aloft and are weather, so they sit with aviation. Lists in tabs:
   AIRCRAFT / SHIPS / MESSAGES (+ NAVTEX / SONDES where there is something to list).
2. **AMATEUR map** (the DIGI MODES MAP grown into the amateur and hobby map). Holds digital spots (FT8/FT4/WSPR/JS8 …), CW
   and RTTY skimmer spots, NCDXF beacons, APRS and **MeshCore / Meshtastic**. MeshCore is licence-free LoRa, but it is
   hobby and community traffic, so it goes here and not on the air/sea map. Lists in tabs: SPOTS / APRS / MESH (+ BEACONS).

**Why two.** The two maps serve different audiences and work at different scales:
- The air/sea map is about **traffic and safety**. It runs at a local-to-regional scale (NM rings, the ADS-B range,
  airports, ports, sea areas), and every layer comes from a decoder that hears vehicles and the services around them.
- The amateur map is about **propagation and people**. It runs at continental-to-world scale (great circles to
  spotters, grids, bands), and its layers are callsigns. One chip row holding 20 layers would be unreadable on a 4"
  phone (see feedback_control_deck_stays_minimal).
- The community map is UNCHANGED. It still excludes amateur spots, so it matches the air/sea map plus MeshCore as it
  stands today.
- The brief's MISC group is gone: sondes moved to air/sea, APRS and MeshCore to amateur.

Both maps are built on **one map kit and one shared code path**:
- `src/services/liveMaps.ts` (pure, no DOM, no React Native) holds the map catalogue, the live-layer rule, the title,
  exit and tab rules, the aircraft list, the message feed and the fade.
- The web's `TrackView` (`web/client/src/tracks.ts`) and the app's `MapOverlay` track page draw both maps.
- `web/mapkit` draws the shapes: `tracklayer.js` and `adsblayer.js`. Both are bundled into the app as strings by the
  `gen-*-source` scripts.

## THE RULES (both maps)

### 1. The current mode is the LIVE layer
`liveOf(mode)` maps the receiver's current mode to `{ map, layers[], tab }`:

| Current mode | Map | Live layer(s) | Opens on tab |
|---|---|---|---|
| ADS-B | air/sea | ADS-B | AIRCRAFT · NEAREST |
| DATALINK (ACARS / VDL2) | air/sea | ACARS + VDL2 (+ ADS-C) | MESSAGES · (aircraft sort LATEST) |
| HFDL (USB on an HFDL channel, decoder on) | air/sea | HFDL + HFDL STATIONS | MESSAGES |
| NDB | air/sea | NDB | NAVAIDS |
| VOR MAP | air/sea | VOR | NAVAIDS |
| AIS | air/sea | SHIPS | SHIPS |
| DSC box on | air/sea | DSC | SHIPS (DSC section) |
| NAVTEX box on | air/sea | NAVTEX (stations / warnings / weather) | NAVTEX |
| SONDES | air/sea | SONDES | SONDES |
| anything else | the map the menu key named | none live: every layer is stored | that key's tab |
| FT8/FT4/WSPR/JS8/…, CW / RTTY skimmer | amateur | SPOTS | SPOTS |
| APRS box on | amateur | APRS | APRS |
| ISM (LoRa) | amateur | MESH | MESH |
| NCDXF tracker on | amateur | BEACONS | BEACONS |

- **The live layer updates in real time and is emphasised.** It always has a chip, and that chip comes first and is
  marked LIVE. Its markers are drawn at full strength and its list section sits first.
- **Every other layer shows what this server has STORED.** Sources: the shared aircraft table (`aircraft.json`), the
  ship store (`ais-ships.json`), the gallery (DSC, NAVTEX, NDB/VOR cards, sondes), the station store (`stations.json`)
  and the remembered mesh nodes. These markers are **faded by age** (`fadeOf`) and are never drawn as live.
- **A mode change while the map is open only changes which layer is live.** The title, the exit key and the list
  follow the current mode on the very next render, because they are all computed from `(map, liveOf(mode), openedAs)`.
  Nothing is cached when the map is made. This makes Mac Ha's bug (11-26: the AIS map kept "SHIPPING MAP" after an
  exit to ADS-B) impossible here: **one view per map, and its chrome is recomputed on every render**. The old
  arrangement had one view per mode, each titled when it was made.
- **Exit:** while the map is the mode's own screen (ADS-B, AIS, DATALINK, NDB, VOR MAP, SONDES), the key reads
  `EXIT <MODE>` and leaves the mode, giving the receiver back as before. Opened from a menu or a box, it reads
  `CLOSE MAP` and the mode carries on. THE ONLY MODE: CLOSE MAP, with the note.

### 2. Layer chips, grouped
- AIR & SEA: **AVIATION** (ADS-B · ACARS · VDL2 · HFDL · ADS-C · HFDL STATIONS · NDB · VOR · AIRPORTS) ·
  **WEATHER ALOFT** (SONDES) · **MARITIME** (SHIPS · DSC · NAVTEX STATIONS · WARNINGS · WEATHER · SEA AREAS).
- AMATEUR: **SPOTS** (by family: DIGITAL · CW · RTTY) · **APRS** · **MESH** · **BEACONS**.
- **A layer with nothing on this server is ABSENT** (AGENTS.md: no dead controls). One layer alone draws no chip row.
  The live layer is always present. Presence is computed from what the server returned, never from a hard-coded list
  (feedback_app_no_hardcoded_settings). A group with one present layer shows the chip with no group label.
- Toggles are remembered per map (`vibesdr.airSeaLayers`, `vibesdr.amateurLayers`; the app uses AsyncStorage with the
  same keys). The old per-map keys (`vibesdr.avMapLayers`, `vibesdr.shipMapLayers`, `vibesdr.navMapLayers`) are read
  once as the starting values.

### 3. Lists as TABS under the map
- **AIRCRAFT**: every source in one row per aircraft (the shared table's merge: ICAO address, then registration, then
  flight), with source badges (ADS-B · VDL2 · ACARS · HFDL · ADS-C). **Emergency (7500/7600/7700, declared) and military
  are pinned to the top.** A sort chip offers **NEAREST | LATEST**. In ADS-B the live report's rows win.
- **SHIPS**: the current ship list unchanged, including the beta95 DX tag and "position implausible", plus the DSC
  section.
- **MESSAGES**: the datalink feed, newest first. Sources: this radio's live DATALINK messages, plus the shared table's
  last label per aircraft from the other radios. Tap a message and its aircraft is highlighted (`focus`). Tap an
  aircraft (row or map) and the messages are filtered to it, with a chip "× REG / FLIGHT" to clear the filter. ⚖ The
  message text appears only where it does today: on the owner's own session, with the opt-in. Everyone else sees the
  label's plain name (`labelName`), as the shared table holds.
- **NAVTEX** (when NAVTEX layers are present): stations, warnings and weather, as the current RECEIVED list's sections.
- **NAVAIDS** (NDB / VOR): the NDB and VOR lists as their maps have them.
- **SONDES**: the sonde list.
- AMATEUR: **SPOTS** · **APRS** · **MESH** · **BEACONS**.
- **A tab with nothing is ABSENT**, the same rule as the chips. The tab you came from is chosen when the map opens. A
  mode change moves to the new live layer's tab, unless the viewer has picked a tab since the map opened, in which case
  their choice stays.
- **Rows are capped** at 300 per tab (the shipping map's existing cap) and the list renders only the tab shown. This is
  the 32-bit 1 GB Android rule (android_potato_phone_users).

### 4. Data and cost
- Polls are floored and run only while the map is open (the Workers incident):
  - `aircraft.json` every 3 s, as today.
  - The ship store, NAVTEX and DSC every 30 s, as today (2 s while NAVTEX is still building its index).
  - `stations.json` on the digi map's existing schedule.
  - Nothing while the page or app is hidden.
- The live layer needs no new request: ADS-B, AIS, DATALINK, NDB, VOR and SONDES already push reports over the socket.
- Memory stays bounded. Each source is already capped (aircraft table 1024 rows, ship store `limit=1000`, warnings
  300). The maps add no new caches, and one view per map replaces up to four TrackViews (navView, aisView, avView,
  avMapView). That saves a MapLibre context each time the user moves between them.
- **No request goes through the Worker.** Everything is this server's own HTTP (feedback_worker_only_establishes).

### 5. The basemap
- AIR & SEA uses a new profile pair, **`airsea`** (airports and ports, major places) and **`airsea-plain`** (AIRPORTS
  off), added to `vibemap-style.json`. A server whose map pack predates the pair falls back to `aero` / `aero-plain`
  plus the `ports` layer switched on by name, the way an older pack's airports are already switched by name.
- AMATEUR keeps the `spots` profile.
- NM scale bar on air/sea, km on amateur. The greyline on both. High Detail Maps packs are used by both, unchanged
  (`mapgl.ts probeMapGL` / app `mapglDetail`).

### 6. Menus: nobody loses their way
Each existing entry opens the right map, layer and tab:

| Entry (web and app) | Opens |
|---|---|
| ADS-B mode | AIR & SEA, ADS-B live, AIRCRAFT · NEAREST, `EXIT ADS-B` |
| ADS-B's AVIATION MAP key | (gone: the ADS-B map IS the air/sea map; other sources show as stored layers) |
| DATALINK mode / box MAP | AIR & SEA, ACARS+VDL2 live, MESSAGES |
| HFDL box MAP | AIR & SEA, HFDL, MESSAGES (CLOSE MAP) |
| VIEWS · AVIATION | AIR & SEA, AIRCRAFT (CLOSE MAP) |
| AIS mode | AIR & SEA, SHIPS live, SHIPS, `EXIT AIS` |
| VIEWS · SHIPPING, NAVTEX box MAP, DSC call MAP, gallery AIS / NAVTEX / DSC rows | AIR & SEA, that layer, SHIPS / NAVTEX (CLOSE MAP), on the item when given |
| NDB / VOR MAP modes | AIR & SEA, that layer live, NAVAIDS, `EXIT NDB` / `EXIT VOR MAP` |
| SONDES mode / box MAP | AIR & SEA, SONDES live, SONDES |
| VIEWS · DIGI MODES, spots MAP, gallery spot rows | AMATEUR, SPOTS |
| APRS box MAP | AMATEUR, APRS |
| ISM box MAP (mesh) | AMATEUR, MESH |
| NCDXF box | AMATEUR, BEACONS |

The VIEWS keys are renamed AIR & SEA and AMATEUR. Each key is drawn only where its map has something to show, the
existing `viewsProbe` rule.

## FEATURE CHECKLIST: nothing lost

✓ = on the new map and tested · ◐ = on the new map, not yet tested · ☐ = not yet carried over · ✗ = dropped (with reason)

### ADS-B aircraft map (web `adsb.ts`/#adsbView, app MapOverlay 'adsb' + AdsbLiveList)
- ☐ Map is the mode, EXIT ADS-B leaves it; Back leaves it
- ☐ Range outline (`adsblayer.js` rangeRing, `prefs.range` / `rangeAll`), labelled **ADS-B RANGE** (label: b96-fixes)
- ☐ Rings in NM, AUTO / FIT / RINGS toggles (`vibesdr.adsbMapPrefs`), glide, dead reckoning, new-aircraft rings
- ☐ Airports + runways (`aero` profile), greyline, NM scale
- ☐ Silhouettes, operator logos (imageBase), flags, MIL, emergency red, the card (ODC-By credit)
- ☐ Live list: nearest first, squawk, FL/ft + climb, kt, NM + bearing; collapses (remembered); row → card
- ☐ Top-bar chips FURTHEST / NEWEST / LONGEST TRACKED (tap → focus)
- ☐ Status: N heard · msg/s · MS/s · capture %; capture warning (red); feed warning
- ☐ GAIN key (adsbGainView: only where the radio has a gain list the owner has not fixed)
- ☐ Colour KEY (ⓘ legend toggle)
- ☐ CHAT key in the top bar; health pill under AUTO/FIT/RINGS; obstacle (layer keeps aircraft clear of the list)
- ☐ Keyboard / pad: navscope, list navbox
- ☐ ADS-B only while heard (`avNowRule`, the in-use words)

### Aviation map (web avView/avMapView, app 'avdata')
- ☐ Sources ADS-B · ACARS · VDL2 · HFDL · ADS-C as layers; absent when empty; opened source first
- ☐ HFDL STATIONS (mast glyph), AIRPORTS toggle → profile live
- ☐ Source-coloured glyph, multi-source number, faded past 10 min
- ☐ Card: flight, reg, ICAO, type, route/OOOI, position source + age, alt, speed, track, Heard on …, last label
- ☐ DATALINK band control (ACARS 131 / VDL2 136) in the top bar; the server owns it
- ☐ THIS RADIO — HEARD NOW / OTHER SOURCES sections; the owner's message text
- ☐ ODC-By credit line at the list's foot
- ☐ AUTO fit via the ADS-B layer

### Shipping map (web navView/aisView, app 'ais' / 'navtex' / 'dsc')
- ☐ SHIPS live (AIS report) + store (faded > 1 h), short stored track; live ship trail from the server
- ☐ beta95 DX tag, implausible position (no dot, listed)
- ☐ AIS safety strip (types 12/14)
- ☐ DSC pins (DISTRESS red ringed SOS, URGENCY orange), never decluttered
- ☐ NAVTEX STATIONS (great-circle arcs, distance NM), WARNINGS (icons, shapes, clusters, not-working marks), WEATHER
  (wind arrows, VRB, CYC, change arrows), cards with gallery text + TRANSLATE
- ☐ SEA AREAS (worldwide, in-view attribution) + forecast tints (amber 7, red gale 8+)
- ☐ Time filter 1H / 24H / 7D / 30D / ALL on every maritime layer
- ☐ "Building the map from N saved messages… %" note
- ☐ KEY: ships, DSC, hazards, marks, wind arrows, sea areas + sources
- ☐ `marine` look: ports
- ☐ Focus on an item when opened from a DSC call / gallery row

### NDB / VOR maps
- ☐ NDB: beacons at published positions, 3-bar meter, Find, stats, owner confirm (NdbLiveList); EXIT NDB
- ☐ VOR: radials, FIX crosshair + words, NAVAIDS list, KEY; EXIT VOR MAP
  (Both are the mode's own screen with heavy mode-specific chrome: see "Staging".)

### Radiosondes
- ☐ Sondes at last position, a chosen sonde's track, band control, EXIT SONDES

### AMATEUR (the DIGI MODES MAP grown; web popup window `spotsMapHtml`, app MapOverlay 'digi')
- ☐ SPOTS from the station store (1H/24H/7D/30D/ALL) + live, coloured by mode, MODE filter + legend (this server's modes)
- ☐ Stats panel (range / countries / bands / modes), totals, radio filter (2+ radios), FIT/AUTO
- ☐ APRS layer (the box's stations), focus a station
- ☐ Satellite pass overlay (digiOverlay)
- ☐ MESH layer (ISM nodes + the remembered ones, gallery node cards)
- ☐ CW / RTTY skimmer spots (already in the station store, by mode family)
- ☐ NCDXF BEACONS layer
- ☐ Tabs SPOTS / APRS / MESH / BEACONS

## STAGING (the order of the build, each stage committed and mergeable)

1. `src/services/liveMaps.ts` + `scripts/test_live_maps.ts`: the catalogue, `liveOf`, chrome, tabs, the aircraft list
   (pinning, NEAREST/LATEST), the message feed and filter, the fade.
2. Web `TrackView`: dynamic chrome (`setChrome({title, exitLabel, listTitle})`), **tabs**, grouped chips with a LIVE
   mark, a sort chip.
3. Web AIR & SEA: one view (`airSeaView`) replacing navView + aisView + avView + avMapView. Shipping + aviation +
   sondes compose into it, AIS and DATALINK modes are its live states, and every opener is routed to it.
4. Web ADS-B mode as the AIR & SEA map's ADS-B-live state. The ADS-B chrome (GAIN, chips, capture, CHAT, legend) moves
   into the view's top-bar slot. `#adsbView` is retired.
5. App: MapOverlay kind `'airsea'` (the 'avdata' page: ADS-B layer + track layer) with title, exit and tab pushed live;
   TrackLiveList tabs; SDRScreen's `shownMap` collapses ais / navtex / dsc / avdata (and adsb at stage 6) onto it.
6. App ADS-B on the air/sea page (AdsbLiveList's features inside the AIRCRAFT tab, the GAIN key).
7. AMATEUR: the digi map gains MESH, BEACONS and tabs. On the web it moves into the page (one TrackView-hosted map)
   or stays a window; see the decisions.
8. NDB / VOR / SONDES as air/sea live states.
9. Copy: sdrTour, pickerTour, AboutOverlay beta96 note + feature lists, web tutorial, watch tutorials (if they mention
   the maps), website. vibe_web_page.h rebuilt via the lock.

## RESUME HERE

- 2026-11-26: design written (this file), two-map split folded in. Nothing built yet.

## Decisions for Stuart
- (to be filled as they arise)
