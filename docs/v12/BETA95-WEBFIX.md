# beta95 web/app quick fixes (branch b95-webfix, 2026-11-26)

The queue is `memory/beta95_small_queue.md` (Stuart's words, 11-25). Eight items, web client + app + the server parts
they needed. Every step is its own commit on `b95-webfix`.

## RESUME HERE

- Everything below is built, committed and unit-tested. Not done: a look at the pages on real hardware (Sony / Pi 500
  AIS, a real METEOR pass), the server-backed browser tests if they were not run (see "Tests").
- The app's half of item 8 (WaterfallView band labels) is **not** on this branch — it is `793ca4f2f` on b95-duo /
  b95-integrate. Merge that too or the app keeps overprinting.
- The community map (directory/, main only) was not changed: it draws what the servers' `community.json` sends, which
  now carries only believed AIS positions (and `dx` / `unconf` fields an older page ignores). If the community map
  should show the DX tag, that is a directory change on main.

## 1. ADS-B list rows keep their signal bars
`web/client/src/adsb.ts adsbListHtml`: the callsign alone is the ellipsised `<b>`; bars + MIL/emergency tag are a
`flex: 0 0 auto` sibling (`.l1` / `.ex`). `index.html`: one column template for every row
(`minmax(0,1fr) 5.6em 3.6em 6.6em`; phone `5.6em 6.6em`). The app list (`AdsbLiveList.tsx`) already had the bars outside
the shrinking text and fixed widths — unchanged.

## 2. ADS-B range outline live, no star
`web/mapkit/adsblayer.js` (one file: web, app WebView, directory): `rangeConsistent` (≤ 1 200 kt between two fixes, dt
floored at 1 s — the server's posOk), `rangeOffer` (≤ 650 km, max per 1° bin), `rangeMerge` (max per bearing),
`rangeSmooth` (a bearing lends ±1° 90 %, ±2° 75 %; never lowers a heard bearing). Every non-datalink fix feeds the live
bins; drawn = smooth(merge(server, live)); redraw at most every 1.5 s; server re-sync now 15 min. RANGE shows as soon as a
live outline exists (a local radio, an older server). Furthest texts in NM. Server `vibe_adsb_range.h` unchanged.

## 3. NM on the aviation and shipping maps
Datalink list/card (`src/services/aviation.ts`, `aviationMap.ts` — both clients), HFDL map card + ground station popup
(`MapOverlay.tsx`), the app's OWRX ADS-B table (`AircraftPanel.tsx`), ADS-B range catch lines (`adsbTypes.ts`, web
`admin.ts`), VOR fix (`vorMap.ts`), NAVTEX map (`navtexMap.ts`, the gallery's SHIPPING MAP row), AIS area-notice sizes
(`vibe_ais_asm.h`). Already NM: AIS rows/cards, NDB. Left in km: APRS, ISM, DAB, spots, sondes.
Not changed (server-side text, b95-srvfix's ground): VOR SAR's "the radial passes … km from this server".

## 4. AIS implausible positions + APRS implausible altitude
Server: `vibe_ais.h` `Msg::wholeMin` (raw 1/10 000′ lat AND lon % 10 000 == 0; 28-bit positions only).
`vibe_ais_service.h` (constants `kDxNm` 75, `kShipMaxKn` 60, `kSarMaxKn` 600, `kAgreeNm` 0.3):
- moving types (1/2/3/18/19/9) with a whole-minute position: dropped, `fakes` counted, "position implausible".
- no distance rejection. First position further than 75 NM: held (`unconf`, `uLat/uLon`, no dot) until a second report
  agrees (same place or believable speed) → believed with `dx:1`.
- a jump from a known track faster than 60 kn: `held`; the track's next report drops it; two reports agreeing on the new
  place relocate the ship and restart its trail.
- AtoN / base stations: agree only in place; round positions kept.
- nothing unbelieved enters the trail → never the ship store's track.
`vibe_ais_store.h`: `scrub()` removes whole-minute positions and track points of moving kinds on read and merge (old
stores keep HOOIHANDEL's fake otherwise); `Rec.spare` bit 0 DX, bit 1 coarse (type 27). `local_sdr_shim.cpp`:
`g_ais.setReceiver(g_bgLat, g_bgLon)` before start.
Clients: `src/services/aisDx.ts` (one rule, two readers), both `tracks.ts`: gold halo + "NAME · DX" label (ranked above
other ships), DX tag in the range column + gold row bar; "DX? 205 NM SW" for unconfirmed; "implausible"; card words.
APRS: `vibe_aprs.h altCeilingM / altPlausible` (ground 9 000 m, aircraft 25 000 m, other 60 000 m, satellite none,
< −500 m any) → readable "(implausible for a car)", JSON `altBad:1`; `aprsSymbols.ts aprsAltCeiling` for older servers;
the card strikes the figure through.

## 5. Sea areas worldwide
`seaCompose` composes all 909 areas, each with `_vb` (its box). `tracklayer.js` makes only `_vb` items whose box meets the
padded view (±25 %, across ±180°) and re-runs the last list after each move; `viewBox()` exposed. Web: in-process; KEY
attribution = `seaAreasInView(view)`. App: the WebView posts `trkView` (rounded out to 2°, once per settled move) and
only areas in view cross the bridge (all 909 would be ~1.5 MB JSON per update); the legend credits every source. The
SEA AREAS toggle's presence rule is unchanged (an inland server has none). Heard counts exclude chart items.

## 6. METEOR
`lrpt.ts`: "decodes from about +3, best above +4" (from `needDb`), "320 lines decoded · 336 of no signal".
`vibe_lrpt_run.h gapart`: no-picture pixels are a dim diagonal hatch; a gap ≥ 9 preview rows carries "NO SIGNAL
hh:mm-hh:mm" (own 5×7 glyphs, times from the strips either side); status `decoded` / `gapLines`. Web: the live LRPT
picture is shown only as tall as its last row (always a decoded one), so the SATELLITE box's follow stays on picture
(the 0x07 pre-size of 1 600 rows had it following the foot of an empty canvas). App: already cropped to its last line.

## 7. Spots map TOTALS
`digiMapStats.ts digiAprsToday`: "APRS today (UTC): N stations · M packets", its own line, "not digital spots". The APRS
points carry `count`; `digiAprsPoints` age is now since LAST heard (`seen`).

## 8. Web band bar overprint
`web/client/src/main.ts drawBands` + `src/utils/bandLabelPiece.ts`: a label's room is its band's widest piece not covered
by a later (narrower) band; placed and clipped there.

## Tests (all run on this Mac, silent)
- `node scripts/test_adsb_layer.mjs` 180/180 (+14 live outline / smoothing)
- `node scripts/test-web-adsb-range.mjs` 37/37 headless Edge `--mute-audio` (+ live growth, merge; page brought to front
  before each map — headless Edge marked it hidden and MapLibre never loaded the 5th map, on the base commit too)
- `test-ais` 104/104 (+16), `test-ais-store` 66/66 (+8), `test-ais-binary` 64/64, `test-aprs` 73/73 (+4),
  `test-lrpt --quick` 71/71 (+4) — single-file `g++ -O2`, nice 10
- tsx/node: test_ais_aprs_app 181, test_sea_areas 69, test_lrpt_app, test_digi_map_stats 65, test_digi_map 116,
  test_shipping_map 57, test_navtex_map 73, test_aviation_map 92, test_aviation_app, test_adsb_report 75,
  test_adsb_list 34, test_vormap_app 52, test_navtex 207, test_band_label_piece 4
- `npx tsc --noEmit`: clean apart from the pre-existing `handheld-ui/test` node-types errors (TS2591/TS2580).
Synthetic only — nothing on real signals.
