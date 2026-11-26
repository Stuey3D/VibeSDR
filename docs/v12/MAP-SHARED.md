# The shared tuner on the full-screen maps · TUNE / SHOW · AIR & SEA LIVE (beta96, branch b96-mapchat)

Stuart 11-26, "big issue": Listen-Amateur-VLD's SHIPPING MAP on a shared dial covered the controls bar, so a listener could
not see the shared-tuner state or reach CHAT. Grew (coordinator, same day) into TUNE / SHOW, the TUNE chip + list, the cover
rule, and the menu consolidation.

## What is built

| Piece | Web | App |
|---|---|---|
| SHARED TUNER line + CHAT | TrackView `slot('share')` (first in the top bar's sideways row) — `main.ts mapSharedSync`, run from `updateSharedBanner` and every map render | `MapTopStrip` under the page's top bar (one scrolling row), fed by `SDRScreen mapLine` |
| The words | `dialChat.mapSharedLine` — the bar's inputs; EXIT verbs while EXIT leaves a whole-radio mode, the bar's own words otherwise; 3 new chat keys × 8 languages (`bannerFreeExit`, `bannerAskExit`, `bannerLockedExit`) | same function |
| Dial lock | `TrackView.setDialLock(why, exitToo)` greys EXIT + `[data-dial]` (the TUNE chip and rows, ADS-B GAIN), captures the press → `nudgeAsk` + pill; Escape too. Never the map, chips, tabs, FIT, time filter | `MapOverlay closeGuarded` (page EXIT, Android back), the page's `__backLock`, the strip's TUNE chip refused |
| TUNE | ONE chip "TUNE · <live> <freq> ▾" + popup list (`TrackView.setTune`, rows are buttons: arrows, Escape) | strip chip + RN list (pad rows) |
| TUNE rows | `src/services/airSeaTune.ts airSeaTuneRows` from THIS radio's `vibeserver.json`: caps, `avdataBands` (+ new `chLo`/`chHi`), `sondesBands`, `ndbLo/Hi`, new `vormapLo/Hi`; the cover rule; greyed with the server's reason; the only mode | same |
| SHOW | the layer chips with counts + colour dots, labelled SHOW (`setToggles` n / colour, counts updated in place) | unchanged (the chips are in TrackLiveList) — **not done**: counts on the app's chips |
| Menu | ONE "AIR & SEA LIVE" (`mobile.ts #mModeAirSea`, decoder panel `setWholeRadioKeys`) — last used (`vibesdr.airSeaLast`) else ADS-B else first; VIEWS' AVIATION + SHIPPING → ONE "AIR & SEA" (viewer) beside GALLERY (`openAirSeaViewer`, `window.__vibeOpenAirSeaViewer`) | `ModeSelector airSeaKey` (WHOLE_RADIO_IDS 'airsea'), `'airseaview'` beside `'gallery'`; the seven separate keys not passed (`asMenuOne`; Apple TV keeps them — no maps there) |
| Maps covered | AIR & SEA (every live state), ISM mesh map (+ ISM band TUNE rows), ISM gallery map | every track page (MapOverlay `isTrkKind`) |

Server: `local_sdr_shim.cpp` — `avdataBands[].chLo/chHi`, `vormapLo/vormapHi` in `vibeserver.json` (no behaviour change).
Web: the shared banner's count now also follows the `dial` message's `listeners` (the server's beta83 leave fix — `users` lagged).

## The maps' scope: OTHER RADIOS ON THIS SERVER · THIS SESSION (coordinator's asks, built 11-26)

| Piece | Where |
|---|---|
| The rule (one, pure) | `src/services/mapScope.ts` — `otherRadiosOffered` (a map on a radio, an id, and 2+ sources: `radioCount >= 2` or an entry tagged by another source, e.g. a feed), `scopeRadioOk` (OFF = this radio's own; an UNTAGGED entry is unknown and stays — the logs' rule), `scopeSessionOk` / `FirstSeen` / `sessionStart` (reset on (re)opening and on a live-mode change) / `sessionWindow` (the polls' covering window) / `withSession` (THIS SESSION beside the window, ALL \| THIS SESSION where a map has none) |
| AIR & SEA sources | `src/services/airSeaScope.ts` — the DATA goes through the scope before anything is composed, so SHOW counts, tabs, items and rows follow. Server times are moved onto the viewer's clock by each answer's skew. Every entry is OBSERVED on every pass (FirstSeen), so THIS SESSION picked later knows what came before |
| Server | `vibe_aircraft_share.h`: `Rec::radios` (padding → a mask, bit = `<runtime dir>/radios.idx` line, vibe_log_radio.h's registry), set by `Table::write` from this process's id (`asId` for a feed — **b96-sbsfeed should write with `asId = "feed"`** so OFF hides it); `aircraft.json` per row `"radios"` + `"first"`; `vibeserver.json` `radioId` + `radioCount`. Ships, NAVTEX stations, stations and gallery items already carried radios (beta92); the clients now keep them (sanitisers) |
| Web | `TrackView.setScope` (the chip after SHOW, never dial-locked) + `setScopeNote`; `main.ts asScoped / asRender` (AIR & SEA), `ismMapRender` (mesh map: remembered nodes carry their card's `ri`), the AMATEUR window (`#scope`, THIS SESSION in `#wins`, SHOW label + colour dots + counts on `#layers`; the radio keys hide when OFF). `localStorage vibesdr.mapOtherRadios` ("0" = off) |
| App | `SDRScreen` (`asSrcS` → the map region reads `*S` names; `ismTrk`), `TrackLiveList` chips with `n` + `colour` (SHOW label), `DigiMapKeys` (THIS SESSION, SHOW with counts, OTHER RADIOS) + `MapOverlay amData` / page `__amScope` on the AMATEUR map. `AsyncStorage lsv_map_other_radios` |
| AIR & SEA LIVE while lit | REOPENS the map (web `toggleAirSeaLive` → `asOpen({by:'mode'})`, app likewise); EXIT in the map leaves |
| Viewer alias | `window.openAirSeaViewer` (b96-sbsfeed's name) and `window.__vibeOpenAirSeaViewer` are ONE function |
| AMATEUR TUNE | none: no amateur map entry retunes the radio (a spot is a station heard) — said on SHOW's title / the tour |

Known limits (honest): NAVTEX warnings / weather carry no radio yet (shown under every radio); HFDL ground stations carry none (the
GsRec has no spare bytes); an entry with no server first-heard time that was already in a report when the map opened counts as new
(ADS-B aircraft unknown to the shared table, sondes, VOR without a first, NDB, APRS, beacons); on the app's SONDES live state the
band keys hold the window row (no THIS SESSION there while SONDES runs); a stored station / node is "this radio's" if its day card or
row lists this radio (a card two radios wrote lists both).

## Whole-radio air/sea modes found (report to Stuart)
ADS-B · datalink bands `acars`, `vdl2`, `acars+vdl2` (wide, ≥ 6.1 MS/s) · VOR MAP · NDB · AIS, `ais+dsc` (MARITIME) · sondes
`lo` / `mid` / `hi` / `all` (≥ 6.7 MS/s). No 1680 MHz sonde band exists in `vibers41::bands()`. DSC alone and HFDL are decoders.

## RESUME HERE
1. **Done 11-26 (scope branch work):** OTHER RADIOS ON THIS SERVER, THIS SESSION, SHOW counts on the app's chips, the AMATEUR map's
   SHOW chips (web window + app), AIR & SEA LIVE reopening the map, the viewer alias — see the section above.
2. **Open:** b96-sbsfeed's feed writes must pass `asId = "feed"` to `vibeavshare::Table::write` (else a feed's aircraft count as
   this radio's); NAVTEX warnings / weather and HFDL ground stations are untagged; a device run of the app (chips with counts at
   phone width, DigiMapKeys wrapping with SHOW + THIS SESSION + OTHER RADIOS, the pad walking them) — not run on a device.
3. b96-sbsfeed's standalone landing map: `window.__vibeMapStandalone = true` (or `openAirSeaViewer`) → no shared line, no TUNE,
   no scope keys. The app's viewer is `asViewer` (SDRScreen).
4. `scripts/test-web-avmap.mjs` and `scripts/test-web-ais-aprs.mjs` were stale (the DATALINK / AIS mode keys, the DATALINK band
   keys now TUNE rows, chip labels with SHOW counts) — fixed; both pass again.

## Tested how
- **Scope (11-26):** `vibeserver/test-aircraft-share` 67/0 (radios mask: one radio, both, a feed's own id, untagged; JSON
  `radios` + `first`; an older record = unknown); `scripts/test_map_scope.ts` 52/0; `scripts/test_map_scope_app.ts` 34/0;
  `scripts/test-web-map-scope.mjs` 23/0 (real server, stubbed aircraft/ship answers tagged by this radio / another / a feed /
  untagged); `scripts/test-web-avmap.mjs` 88/0 (TWO REAL radio processes: radio A's ADS-B-only 400F11 hidden on radio B's map
  with OTHER RADIOS off, 4CA97B — heard by both — kept); `scripts/test-web-ais-aprs.mjs` 40/0 (the AMATEUR window's SHOW label,
  dots, counts); regressions test-web-map-shared 36/0, test_amateur_map 187/0, test_ism_app 256/0, test_live_maps 63/0,
  test_air_sea_tune 37/0, test_ais_aprs_app 181/0, test_vormap_app 52/0, test_ndb_app 204/0, test_map_page_kind 72/0,
  test_stored_layers 37/0, test_log_radio 17/0, test_map_shared_app 28/0, test_map_autofit 64/0, test_nav_coverage ok; tsc clean
  (outside scripts/ and handheld-ui/test). Not run: a device; the web ISM mesh map and the AMATEUR window's THIS SESSION / OTHER
  RADIOS keys in a browser (source + unit only).
This Mac; vibeserver built from this worktree through the build lock; headless Edge `--mute-audio`; fake rtl_tcp (no clip).
- `scripts/test-web-map-shared.mjs` 36/0 (real server, `--users 4 --ask-before-tuning` / `--users 1`). FINDING: after a raw listener leaves, the server keeps counting it (`users` and the dial's `listeners` stay 2/4; the controls bar says the same) while the ask lock lifts — the line agrees with the bar; FREE TO EXIT after a leave is therefore not proven.
- `scripts/test_air_sea_tune.ts` 37/0 (RTL 2.4 / RSP 8 / HF+ shapes, the line's states, 8 languages).
- `scripts/test_map_shared_app.ts` 28/0, `test_nav_coverage` ok, `test_decoder_key_grid` 79/0, `test_vormap_app` 52/0,
  `test_ndb_app` 204/0, `test_ais_aprs_app` 181/0, `test_map_page_kind` 72/0, `test_live_maps` 63/0, `test_chat_i18n` 102/0.
