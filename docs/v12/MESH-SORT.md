# The mesh NODES list: NEAREST | LATEST (beta96, b96-meshsort)

Stuart, 2026-11-26 (screenshot of the ISM · mesh nodes map on the Pi 500, NODES 35): *"meshcore list needs to filter by
nearest first and latest first"*. Before this the list was nearest first only.

## What it does
- The NODES list under the ISM mesh map, and the gallery's LoRa nodes map, has **NEAREST | LATEST** in its header — the
  AIR & SEA list's own sort chip (web `tracks.ts` `setSort`, `.hd .srt`; app `TrackLiveList` `sort` RangeKeys), same look
  and words. A screen reader hears "Sort the nodes nearest first / most recently heard first".
- **NEAREST** (default — the order before the key): by distance from the receiver; a tie → the most recently heard first.
- **LATEST**: by `seen` (seconds since last heard); a tie → the nearest first.
- **No receiver position**: there is no distance, so the list is newest first and the key is **not offered** (no dead
  NEAREST) — `meshSortOffered(sort, rx)` returns null.
- Per viewer, remembered: web `localStorage['vibesdr.meshListSort']` (lsGet / lsSet — try/catch), app AsyncStorage
  `lsv_mesh_list_sort`. One choice for the live map and the gallery's map.
- The order touches only the list's rows: the markers, the SHOW / status counts, OTHER RADIOS / THIS SESSION and routes
  are computed as before (tested).

## Where
- Rule: `src/utils/meshRoute.ts` — `MeshSort`, `meshSortRows`, `meshSortOffered`, `meshSortParse`, the two storage keys.
- Web: `web/client/src/ism.ts` `ismMapRows` / `ismGalRows` take `sort`; `main.ts` `ismMapRender` / `ismGalMapOpen` call
  `setSort(..., 'nodes')`; `tracks.ts` `setSort` gained the rows' noun (`what`, default 'aircraft' — AIR & SEA unchanged).
- App: `src/services/ism.ts` twins; `SDRScreen` `ismTrk` / `ismGalTrk` give `sort`, `onSort`, `sortWhat: 'nodes'`;
  `MapOverlay` hands `sortWhat` to `TrackLiveList`. Pad: the keys are RangeKeys in the tabs' NavRow (test_nav_coverage).
- **Not changed: the AMATEUR map's MESH tab.** It does not list nodes the same way — its own page (web `amList`, app
  `amListRows`) lists every layer newest first, with no distance sort to switch from. Adding NEAREST there is a separate
  piece of work (its own tabs header in two map pages).

## Tests
- `scripts/test_mesh_route.ts` §9 — both orders, both tie rules, unknown position (LATEST, no key), the stored value,
  items/status unchanged, the gallery's rows, web = app, the web wiring. 153/0.
- `scripts/test_nav_coverage.ts` +3 (the sort keys are pad keys; MapOverlay hands them; both ISM maps give them).
- `scripts/test-web-ism-map.mjs` (headless Edge, --mute-audio, real server): the key in the header, NEAREST distances
  never go down, LATEST orders the fake nodes by their seen, remembered, the map unchanged, NEAREST again by keyboard;
  the gallery map's key with the kept pick and LATEST by last heard. 77/0 on a vibeserver built from this branch (its one
  stale line — the live map's ranges now include beta96's THIS SESSION — corrected).

## RESUME HERE
Done and committed on b96-meshsort. Open: the AMATEUR map's MESH tab (above) if Stuart wants NEAREST there too.
