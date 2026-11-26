# beta96 follow-ups to beta95 (branch b96-fixes, 2026-11-26)

Stuart's decisions of 11-26 on the beta95 work, plus the coordinator's ADS-B RANGE label item. Every step is its own
commit on `b96-fixes`.

## RESUME HERE

| Item | Commit | State |
|---|---|---|
| 1. Rate latch: 2.4 MS/s floor | dae4fb4d0 | built; unit + e2e tested (Linux binary; Android uses the same shim) |
| 2. Battery back on the health pill, with time left | dc1cf3c7f | built; unit tested; not seen on a real battery server |
| 3. SEA AREAS on every server | e8921ea46 | built; unit tested (web + app share the rule) |
| 4. Datalink aircraft 30 min on the community feed | 07347e148 | built; unit tested. **Directory page not changed** (main) |
| 5. ADS-B RANGE label on the outline | 6afed5f2b | built; unit/source tested; not looked at in a browser |
| AboutOverlay notes | 527970de3 | |
| vibe_web_page.h | 7a8480fe4 | rebuilt via the lock |

Not done / open:
- The directory's community map (directory/public/community-map.html, on main) still drops aircraft by its own rules
  and does not fade by age. The change it needs is in "4" below.
- The directory deploy carries its own copy of adsblayer.js (directory/public/mapgl/vendor/adsblayer.js). It is
  regenerated on this branch; the directory on main needs the same file for the label to show there.
- Gradle / iOS DSP library (build_ios.sh) not run: the shim changed (vibe_rate_guard.h, vibe_health.h,
  vibe_community.h, local_sdr_shim.cpp), so iOS's prebuilt lib needs a rebuild before an iOS build ships this.

## 1. The rate latch never caps below 2.4 MS/s

Stuart: "2.4msps is the safe rate dont want to limit further as you will loose modes".
`vibe_rate_guard.h` rule (c'): `rateGuardCanStep(hz)` is false at or below 2.4 MS/s; `rateGuardRungBelow` returns 0
there and never lands below 2.4 from above. A stop at the floor (ADS-B 2.4, DAB 2.048) goes to `floorDeathLocked`:
the first is noted like any other stop, and a second at that rate within 24 h raises `RateFault.floor`
(`floorRate`, `floorAt`, `floorMode`, persisted, forgotten 24 h after the last). Nothing is capped. `say()` reads:
"The SDR stopped twice in 24 hours at 2.4 MS/s during ADS-B — kept at that rate, because a lower one would lose
modes. A radio that stops at 2.4 MS/s usually needs a better USB power supply, cable or hub." While a latch at 2.4
(from 2.56) stands, a stop at 2.4 keeps the ceiling at 2.4 and adds the floor note to the latch's own line.
The Setup page and the app's server screen draw the floor line in red (`rateGuardIsBad`). Linux and Android are the
same shim; `main.cpp` needed no change.
Tests: `vibeserver/test-adsb-capture.cpp` 75/75, `scripts/test_capture_and_rate_guard.ts` 22/22,
`scripts/test-rate-guard.mjs` (e2e, see the report for the count).

## 2. The battery on the health pill

Stuart: "why is the battery level off the pill surely that is useful info for battery powered servers and the
listeners should know that info so they can work out how long they have left". beta95's blanking line in
`healthTick` is gone. `vibehealth::BatteryEta` (vibe_health.h) measures the drain between 1 % step edges (it says
nothing until 2 % / 10 min of drain; an overdue step reads as a slower drain; 3 h window; charging or a rising level
forgets), and gives minutes to the owner's suspend floor (`batteryPauseAt`) or to empty, rounded 5/10 min, capped
48 h. Sent as `bat.mins` (additive) and as a quiet NOTE line, "Battery: about 2 h 30 min of listening left" —
every web and app client already draws notes, so no client change was needed.
Tests: `vibeserver/test-health-snail.cpp` all passed (+11).

## 3. SEA AREAS on every server

Stuart: "Sea areas on all servers since community map is on them". `shippingMap.ts shipLayersPresent` always offers
SEA AREAS. The areas near the receiver now only set the default (`shipLayersFor`): on near the coast, off inland,
until the listener flips it (`ShipLayers.areasSet`, `shipToggle`). A beta95 save with `areas:false` counts as the
listener's choice. Web `main.ts shipLayersNow()`, app `SDRScreen shipCompose` (`eff`, `sc.layers` for the toggles).
Tests: `test_sea_areas` 77/77, `test_shipping_map` 57/57, `test_navtex_map` 73/73.

## 4. Datalink aircraft 30 min on the community feed

Stuart: "hold them for 30 is fine faded to show older". `vibe_community.h kAircraftDatalinkAgeSec` 1800: a row whose
POSITION came from ACARS/VDL2/HFDL/ADS-C (`posSrc`) is kept 30 min; ADS-B (and unknown) positions keep 5 min. The
shim reads the share table over the longer window. Row field 3 is the position age in seconds.

**The directory page change (main — not made here). Read on main 11-26: the page parses field 3 already
(`t = now - age`), fades by `CM.fade` and the card already says "Position VDL2, 12 min ago". Two edits in
`directory/public/community-map.js`:**
1. `MODES`: `acars`, `vdl2`, `hfdl` `win: 300` → `win: 1800` (adsb stays 300). Today `filter()` drops anything older
   than `min(maxAge, MODE[m].win)`, so the 30-min rows would be thrown away at 5 min. The chips' tooltips read `win`.
2. `fade(it, now)`: for `it.kind === 'aircraft'` use the age rule directly — `a <= 300 ? 1 : max(0.35, 1 - 0.65 * (a - 300) / 1500)`
   (full under 5 min, 35 % at 30 min) — instead of the per-mode window (which with win 1800 would hold full to 6 min,
   and for a mixed ADS-B + VDL2 aircraft would take the ADS-B rule from `modes[0]`).
The card's "… ago" needs no change. `scripts/test-community-map*.mjs` (if present on main) will want the windows.
Tests: `vibeserver/test-community.cpp` 65/65 (+2).

## 5. ADS-B RANGE label

`web/mapkit/adsblayer.js` (web, app WebView, directory): GPU map — symbol layer `vsadsb-range-lbl` on a LineString
copy of the outline, `symbol-placement: line`, every 280 px, kept upright, JetBrains Mono in the outline colour with
the theme halo. Leaflet fallback — one label at the furthest vertex (`rangeLabelPoint`). KEY heading "ADS-B RANGE
(the solid outline)"; RANGE button tooltips "ADS-B RANGE outline … (not the distance rings)". Generated copies
refreshed (`gen-adsblayer-source.mjs`). Tests: `test_adsb_layer` 191/191.
