# The map follows the mode (beta96)

## The report
Mac Ha, Discord 11-26, the app in portrait on a server in Vietnam (beta94/95): switching between AIS and ADS-B left the
map on the old mode. The title said "EXIT AIS · SHIPPING MAP" with the ADS-B AIRCRAFT list under it, and the reverse.
Exiting the map and choosing the mode again fixed it.

## Root cause (app only)
`MapOverlay` writes every map's page to the same file (`mapPack.ts writeMapPage` → `<documents>/vibemap.html`) and gave
the WebView only that file's uri. The uri is the same string for every kind of map, so:

1. **Exit, then another mode** (the path Mac Ha describes). The overlay stays mounted while hidden and kept the old
   `pageUri`. On the next opening the WebView mounted straight away on the file, which still held the **previous**
   mode's page. The new page was written a moment later (ADS-B's later still, because it reads its saved toggles first).
   `setPageUri(same uri)` then changed nothing, so the WebView never reloaded. The lists are drawn in React Native from
   `kind`, so they followed the new mode. The page did not: its title, EXIT key and layers stayed on the old one. Doing
   it a second time worked because the file now held the right page.
2. **A mode change while the map is open.** Another listener moves the shared dial, or the server answers a
   `mode_resume`. The server sends `ais_off`, then the first ADS-B report. `kind` changes under the open map and the
   same no-reload happens.

Other paths I checked: the Mode sheet, keys, gamepad and band, bookmark or scanner tunes cannot be reached while the
map's modal is up. The modal covers the screen, and `useOverlay` makes the screen yield its keys. Satellite passes and
schedules change the mode on the server, so they arrive as path 2. The web client has a separate view for each mode
(`TrackView` / `adsbView` / `NdbView`), so it never showed this mismatch.

## The fix
- `src/utils/mapPage.ts`: a written page is stored with the kind it was built for and the opening it was built in
  (`mapPageWritten`). It is shown only while both still match (`mapPageShown`). A new kind or a new opening gets a new
  id, which becomes the WebView's `key`, so a fresh WebView loads the fresh file. A rewrite for the same kind in the same
  opening (for example the receiver's position arriving) keeps its id, so the map is not torn down.
- `MapOverlay.tsx`: the page is dropped when the overlay closes. The ADS-B selection is cleared when its map goes.
- `SDRScreen.tsx`: the map precedence now lives in `shownMapOf` (behaviour unchanged, but now testable).
  `wholeMapYield`: when a report arrives for a whole-radio map mode we were not in (ADS-B, AIS, NDB, SONDES, VOR MAP), the
  other modes' screens are put away here instead of waiting for their `_off`.
- `web/client/src/main.ts`: the same `wholeMapYield` at the top of `adsbUiOn` / `aisUiOn` / `ndbUiOn` / `sdUiOn` /
  `vmUiOn`. This is defensive: today the server always sends the `_off` first (`local_sdr_shim.cpp` leaves every other
  whole-radio mode before entering one).

## Tests
`node --no-warnings scripts/test_map_page_kind.ts` (also in run-tests.sh) passed 55 of 55. It models the page lifecycle:
one file on disk, and a WebView that reads the file only when it mounts. It first runs the OLD rule to reproduce both of
his screenshots (exit→ADS-B shows the AIS page under AIRCRAFT; exit→AIS shows the ADS-B page under RECEIVED; a change
under the open map makes the page and list disagree). Then it runs the new rule:
- exit→ADS-B and exit→AIS;
- AIS↔ADS-B under the open map;
- a same-kind rewrite gives no remount;
- reopening the same kind gives a new WebView.

It also tests `shownMapOf` across the transitions and the wiring in MapOverlay, SDRScreen and the web.

Other suites run against the change: test-only-mode-clients 96/96 and test_sondes_app (all passed) after their regexes
were widened to allow the new first line. test_ais_aprs_app 181/181, test_ndb_app 204/204, test_vormap_app 52/52,
test_adsb_list, test_adsb_furthest, test_digi_map, test_digi_map_stats, test_map_autofit and check-map-overlay.mjs all
pass. `tsc` (4 GB) shows the same 45 errors as v12-experimental, all of them missing node types under scripts/ and
handheld-ui/test; there are none in src/ or web/.

Not done: an on-device run. This needs the coordinator or Mac Ha: open AIS, EXIT, then ADS-B on a phone.

## RESUME HERE
Merged? If not, merge b96-mapmode. Then ask Mac Ha to repeat AIS → EXIT → ADS-B on the next beta, and to repeat it with
a second listener switching the mode while his map is open.
