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

## Whole-radio air/sea modes found (report to Stuart)
ADS-B · datalink bands `acars`, `vdl2`, `acars+vdl2` (wide, ≥ 6.1 MS/s) · VOR MAP · NDB · AIS, `ais+dsc` (MARITIME) · sondes
`lo` / `mid` / `hi` / `all` (≥ 6.7 MS/s). No 1680 MHz sonde band exists in `vibers41::bands()`. DSC alone and HFDL are decoders.

## RESUME HERE
1. **Not done (coordinator's later asks):** "OTHER RADIOS ON THIS SERVER" toggle and "THIS SESSION" time window (maps on a
   radio only); SHOW counts on the app's TrackLiveList chips; the AMATEUR map's TUNE/SHOW (the web AMATEUR map is a separate
   window — its page keeps the bar — and its spots do not retune); a real two-radio server test (no fake HackRF/HF+ exists:
   the per-radio shapes are unit-tested from the server's gates instead).
2. b96-sbsfeed's standalone landing map: set `window.__vibeMapStandalone = true` (or open through `openAirSeaViewer`) and the
   map shows no shared line and no TUNE chip. The app's viewer is `asViewer` (SDRScreen).
3. Device run of the app strip (phone width, pad order with the list, chat over the map in the Modal) — not run on a device.

## Tested how
This Mac; vibeserver built from this worktree through the build lock; headless Edge `--mute-audio`; fake rtl_tcp (no clip).
- `scripts/test-web-map-shared.mjs` 36/0 (real server, `--users 4 --ask-before-tuning` / `--users 1`). FINDING: after a raw listener leaves, the server keeps counting it (`users` and the dial's `listeners` stay 2/4; the controls bar says the same) while the ask lock lifts — the line agrees with the bar; FREE TO EXIT after a leave is therefore not proven.
- `scripts/test_air_sea_tune.ts` 37/0 (RTL 2.4 / RSP 8 / HF+ shapes, the line's states, 8 languages).
- `scripts/test_map_shared_app.ts` 28/0, `test_nav_coverage` ok, `test_decoder_key_grid` 79/0, `test_vormap_app` 52/0,
  `test_ndb_app` 204/0, `test_ais_aprs_app` 181/0, `test_map_page_kind` 72/0, `test_live_maps` 63/0, `test_chat_i18n` 102/0.
