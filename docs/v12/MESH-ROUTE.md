# MESH ROUTE — furthest direct / via mesh, the route on the map, km ↔ miles (beta96, branch b96-meshfar)

Stuart, 2026-11-26, from the Pi 500's ISM · mesh nodes map: "furthest t::1 334 km" looked like garbage (it is a real
MeshCore repeater on the north Cornwall coast, key 8d76bb29…, 13 hops), the furthest should say what the aerial reached
itself, and clicking a node should show the route its advert took.

## RESUME HERE

Done and committed on `b96-meshfar` (not merged, not pushed):

- **Server / native** (c8bf3381c, the previous agent): `vibe_meshcore.h` keeps a FLOOD packet's path (one hash per relay,
  first relay first, 1–3 bytes each; DIRECT packets' paths are not a route travelled); `vibe_lora_track.h` keeps the FEWEST
  hops (`direct:1` = heard first-hand at least once) and the newest flood route (`relays:"5a 9f 1c"`); the gallery's
  "stations received" card writes `fewest hops`, says `furthest direct … · via mesh …` (implausible fixes never count) and
  keeps a `Routes heard (…)` section after the table. test-lora-nodes 51/0.
- **Shared rules** — `src/utils/meshRoute.ts` (web and app import the same file): `meshName`, `meshPlausible`
  (500 km a hop, 3000 km hops unknown), `meshFurthest` / `meshFarText`, `meshRelays`, `meshRoute` (matching + the ordered
  drawing data: segments and badges, each with the tick it lights on), `MESH_ROUTE_JS` (the page-text drawer, ES5).
- **THE INTERACTION (Stuart 11-26, final):** a tap on a node (map marker or NODES row) OPENS ITS CARD and ZOOMS IN to it
  (the auto fit paused). The card has a **ROUTE** key only when the node's packet carried at least one relay (no dead
  control). ROUTE draws the route hop by hop and **fits the view to the whole path** (`meshRouteFit`: node + placed hops +
  receiver; ambiguous candidates only when they keep the box within 2× each way); the key then reads HIDE ROUTE. ROUTE
  again, another node, or the card closing clears it; the card closing resumes the auto fit.
- **Web**: ISM's MAP and the gallery's LoRa nodes MAP — `TrackView` `zoomOnSelect`, `setRouteSource` + `routeCard` as the
  view's `onCard` (the card's `.vmrBtn[data-rtk]`), `showRoute`. The AMATEUR window's MESH layer: a marker tap
  (`amMeshPick`: zoom, hold) — `vibemapgl-compat.js` circleMarker gained `.on('click')` — and the popup's
  `.vmrBtn[data-amk]` (a delegated listener, `amRouteBind`). Units: `ismMilesPref()` + `ISM_UNITS_EVENT`.
- **App**: MapOverlay 'ism' page (items carry `_rt`; a tap → `trkLayer.focus` zoom; the card's ROUTE → `trkRoute`), the
  'digi' page's MESH layer (`amMeshPick`, `amRouteShow`). **The pad**: the card is in the WebView, out of its reach, so
  TrackLiveList shows a ROUTE / HIDE ROUTE key (a `ToggleKey` in its own NavRow — test_nav_coverage) while the chosen node
  carries a route — on the ISM map (`__trkRoute`) and the AMATEUR map (`__amRoute`); the page reports back
  (`trkRoute` / `amRoute` messages) so the key lights. Units via `ismMilesSet/On` (IsmPanel, `lsv_ism_prefs`).
- **A selection holds the view** (Stuart's 16:15 recording: a node picked from NODES, then the map glided back out to
  all of England): any selection — a row, a link, a tap, a route — pauses the auto fit until FIT or the selection is
  cleared (then it resumes). Web `TrackView.selPause/selResume` (every track map; not while the ADS-B layer owns the
  camera), app page `trkSelFit`, AMATEUR `amRouteShow` (digiFit).

Not done / open:
- The Pi 500 runs a build without the routes section, so **no real path has been checked yet** — after the next server
  update, open its ISM map, tap t::1 and compare the hops with the card's `Routes heard` line.
- The app's paths were checked by source tests and a syntax check of the page text, not on a device.
- R36S / watch: no mesh map there; nothing to do.

## The rules

1. **Names** — printable text as advertised (any script, emoji, punctuation; quotes and `<>` escaped where shown). Dropped:
   control, private-use, lone surrogates, U+FFFD, format characters except the emoji joiner. 40 characters. Nothing left →
   the node's short key id.
2. **Furthest** — DIRECT = heard first-hand at least once (`hops 0` or `direct:1`); VIA MESH = only relayed. A fix beyond
   500 km × (hops + 1) (3000 km hops unknown) is not plotted and never "the furthest" — its card says "position
   implausible (not plotted)". Header: `furthest direct: N-Ise repeater NN29 16 km · via mesh: t::1 334 km`; the via-mesh
   half only when it is further.
3. **Route** — each relay hash matched by prefix to the MeshCore repeaters and room servers this map holds (live and
   remembered, any time filter; never a companion, never the node itself): one → placed (solid line, numbered badge);
   several → up to 6 nearest the previous stop, dimmed lines to each and on from each ("+N more"); none → "unknown repeater
   0x5e", a dashed line bridges from the last known stop to the next. A small arrowhead mid-segment points towards the
   receiver. One tick per hop, 300 ms (reduced motion: all at once), then it holds. Shown only by the card's ROUTE key;
   the view glides (60 ms after the press, after the layer's own fly) to the whole path, padded 48 px, at most zoom 11.
   Card: `route: 13 hops, 9 placed, 2 ambiguous, 2 unknown` then one line a hop.
4. **Units** — the ISM list's KM/H ↔ MPH key is the only setting: km or miles on the mesh map's status, NODES list,
   cards, route card and the AMATEUR map's MESH tab. Never NM.

## Tests (how)

| test | what | result |
| --- | --- | --- |
| `npx tsx scripts/test_mesh_route.ts` (in run-tests.sh) | names; plausibility; furthest direct vs via mesh (km + miles); MeshCore packet-format fixtures → relays; meshRelays bounds; matching unique / ambiguous / unknown on a 13-hop path; segment order, dim / dash, badges, ticks; MESH_ROUTE_JS played on a fake Leaflet (timers, clear, reduced motion, the path fit); which points the fit takes (a far candidate left out); the ROUTE key only with a path, key escaped; Routes section round trip; km/mi on every surface; app pages (tap zooms, never draws; card key; the pad's list key) | 130/0 |
| `vibeserver/test-lora-nodes.cpp` (single-file g++) | packet path kept, fewest hops, card furthest direct / via mesh, routes section | 51/0 |
| `scripts/test-web-ism-map.mjs` (real server, headless Edge, muted) | furthest wording; KM/H→MPH turns the map to miles; a NODES pick → card + zoom in, no route; ROUTE → 4 badges / 6 arrowheads and node + placed hops + receiver all in view; HIDE ROUTE / ROUTE again; a node with no path has no key and clears the other's route; a marker tap → card + zoom; a selection holds the view through a refresh; FIT re-frames; closing the card clears the route and resumes; back to km | 69/0 |
| `scripts/test_nav_coverage.ts` | the list's ROUTE key is a pad key | ok |
| `scripts/test-web-ais-aprs.mjs` | the AMATEUR window still draws (it caught a paren error in cleanMesh) | 40/0 |
| test_ism_app / test_amateur_map / test_map_autofit / test_map_page_kind / test_ais_aprs_app | updated for the new wording / rule | 256 / 187 / 64 / 72 / 181, 0 failed |

Synthetic data only, plus the Pi 500's public card read (no tuning): its nodes and distances agree (t::1 334 km at 13
hops plausible); it carries no routes yet.
