# MAP CONTROL CARDS (beta96, Stuart 11-27)

Stuart, looking at the Lenovo AIR & SEA viewer (Mac) and the datalink map (iPhone): *"the maps themselves are great the
controls need work"*. His design (memory `map_control_cards_design.md`): the map takes centre stage; only the way out,
the listeners / ask-to-tune line and CHAT are always on screen; top right only the live message rate; everything else
in CARDS that pop out of small buttons and shrink back (the app's SERVERS button and the health chips are the model);
how many cards can be open follows the window; a phone shows one at a time.

## RESUME HERE

- Branch `b96-mapcards` (off v12-experimental 567de0bd4) + b96-rangefeed's 62d82dcc7 and f7838c6cf cherry-picked (the
  viewer's receiver position, RINGS / FIT / RANGE enable rules, the range outline shape). Rebase onto b96-rangefeed /
  b96-asklock / b96-feedtest when they merge: the only shared lines are main.ts `adsbChromeMount` / `openAirSeaViewer`,
  tracks.ts `makeAcLayer` and adsblayer.js `mk()` + the `ctlHost` append (two lines).
- **WEB PAGE DONE, APP NOT STARTED** — Stuart sees the web page first (live preview), then the app port below.
- Live preview (while it runs): `http://<Mac LAN IP>:47610/` — scratchpad `mapcards/run-preview.sh` (a local door: the
  Lenovo PiAware Beast feed read-only + a fake RTL replaying a synthetic VDL2 clip; NAVTEX fixtures + a ship store
  seeded by `mapcards/seed.mjs`) and `mapcards/preview.mjs` (serves web/dist, proxies the rest, rebuilds through the
  build lock on every save).

## Tweaks from Stuart (live preview, 11-27)

1. NAVTEX stations: no line from the receiver (navtexMap.ts navItems — web and app); the distance stays on the label.
   Other receiver lines left for him to decide: the AMATEUR map's beacons (always), NDB (tapped beacon), VOR radials
   (the fix), mesh routes (on tap).
2. The bar is EXACTLY `EXIT | Shared VFO: Ask before changing mode or exiting 2/5 | CHAT | VDL2 18 msg/min` — no title;
   the card buttons moved to a small vertical RAIL at the map's top-right edge (`.trkMapHost .cardBtns`), the cards
   open beside it (right to left), the fits keep clear of that corner. The line is `dialChat.mapBarLine` (six new
   chat-words keys, 8 languages); the app's MapTopStrip keeps `mapSharedLine` until its port.

## What is where (web)

| Always on (the bar) | |
|---|---|
| EXIT x / CLOSE MAP | first, as before |
| title | not shown (tweak 2); kept hidden for the dialog's name and the drivers |
| "Shared VFO: … n/max" + CHAT | the `share` slot (main.ts `mapSharedSync` → `dialChat.mapBarLine`) — ADS-B's own CHAT in the `top` slot when the line is not shown |
| card buttons | NOT in the bar: a rail on the map's top-right edge, `.trkMapHost .cardBtns` — each `[data-cbtn]`, `aria-expanded` |
| live rate, right after CHAT | `.rate` — `ADS-B 1,425/s` (adsbReport.msgRate is per second), `VDL2 98 msg/min` (avReport.msgRate), `AIS 12 msg/min`; nothing on the viewer |

The old status paragraph (`.st`) is kept for screen readers only (1 px, clipped) — drivers still read it. Its facts went:

| Fact | Now in |
|---|---|
| counts per layer | VIEW chips (as before) and LIST tabs |
| "N heard, not on the map" | LIST card's first line |
| warnings in force | VIEW card's first line (`navWarnStatus`) |
| furthest aircraft | MAP card, under the layer keys |
| frequency / gain / capture of the live mode | TUNE card's first line (`adsbStatus` / `avStatus` / `aisStatus` / …) |
| what the scope hides (OTHER RADIOS / THIS SESSION) | VIEW card, under the SOURCES SHOWN chip |

| Card | Holds | Offered |
|---|---|---|
| LIST | `.trkList` (tabs, sort, filter, safety strip, rows; NDB's and ADS-B's chips in the `head` slot) | always |
| TUNE | the TUNE rows (`.tunePop [data-tune]`, `data-dial` — the ask lock greys them), ADS-B's GAIN and capture warning (`tune` slot) | only with rows or a control: never on the viewer |
| VIEW | SHOW chips (`.tgl`), OTHER RADIOS (`.scope`) | while either has a chip |
| MAP | AUTO (`.fit`, the shared auto fit's key), FIT (`.fitOnce`), FIT TO (`.fitSel`), the ADS-B layer's RINGS / RANGE / LABELS / ⌖ and NDB's RINGS / UNHEARD (`ctl` slot), the time filter | always |
| KEY | the legend (`.trkKey`) | while there is one |
| SOURCES | per radio (from aircraft.json / the ship store's `radios`, labels from /vibeserver/radios) and per feed (aircraft.json `feeds`) | only with 2+ sources |

**How many open** (`src/utils/mapCards.ts`, shared with the app): window < 600 px wide = ONE (opening another shuts the
first); wider = columns of 340 px, as many as fit in 72 % of the width (max 3), two stacked per column when the window is
≥ 700 px tall; LIST alone in its own column. The oldest gives way. Remembered per viewer in `localStorage`
(`vibesdr.mapCards.narrow` / `.wide`, try/catch): defaults nothing on a phone-width window, LIST on a wide one.
**Escape** shuts the top (last opened) card, then the selection, then the map, as before. Every control is a real
`<button>`, so the pad and the keyboard reach them (gamepad.ts walks focusables); opening a card by a press moves focus
into it, Escape / BACK comes back to its button.

**FIT TO ONE LAYER** (Stuart 11-27: "with this much data the fit button doesnt work"). On the AIR & SEA map the shared
auto fit (mapAutoFit.ts) always owns the camera now (the ADS-B layer's own AUTO and FIT keys are hidden in the card —
`data-k` names them); its points are ONE layer: the viewer's FIT TO pick (remembered, `vibesdr.mapFitTo`), else ADS-B
where there is ADS-B, else the live mode's layer, else the fullest; the furthest 5 % (≥ 8 points) are left out
(`trimOutliers`), measured from the receiver. FIT = frame it now (and AUTO carries on); never greyed — with nothing to
frame its title says so. Why FIT was greyed on the Lenovo: the standalone viewer never read the receiver's position, so
the ADS-B layer disabled FIT / RINGS ("the receiver's position is not known") — fixed on b96-rangefeed (62d82dcc7),
cherry-picked here.

## Maps this covers / leaves

Covered (all are `TrackView`): the AIR & SEA map in every state (viewer, ADS-B, DATALINK, AIS, MARITIME, SONDES, VOR MAP,
NDB, opened from NAVTEX / DSC / HFDL boxes and the gallery), the ISM mesh map, the ISM gallery map.
Left as they are, on purpose: the AMATEUR / spots map (its own window, `digiMap.ts`, a different shape — a candidate for
the same cards later), the satellite sky plot / ORBCOMM (satellites.ts, not a TrackView), the handheld page's map
(handheldMap.ts, pad-only screen), the community map (directory, not the server page).

## Skins and transparency

The map view draws its own dark chrome (unchanged palette from the old bar), not the faceplate's — default, silver and
black chassis and transparency on / off do not change it (checked in the preview screenshots). The display font is not
used here: per `feedback_display_font_everywhere` it is for the main controls and the VTS; the map's bar, cards and
menus are standard type.

## Tests

- `scripts/test_map_cards.ts` (pure rules) — 29.
- `scripts/test-web-map-cards.mjs` (headless Edge, `--mute-audio`, 390×844 / 820×1180 / 1440×900): a door with a
  synthetic Beast feed + a fake-RTL VDL2 radio (two sources) and a single radio (one source). Needs `VIBESERVER_BIN`
  (built with this page) and `VIBE_VDL2_CLIP` (`test-vdl2 --write-clip v.cu8 2400000`).
- Selectors moved in: test-web-avmap, test-web-map-scope, test-web-map-shared, test-web-ndb, test-web-adsb-list-width,
  test-web-ais-aprs, test-web-navtex-map (opens MAP before keying the time filter), test-web-shipping-map (FIT TO NAVTEX
  STATIONS before its sea-area sweep — FIT frames the ships by default now, close in).
- Results 11-27 (this branch's server, headless Edge): test-web-map-cards 203/0 · test_map_cards 29/0 · test-web-avmap
  88/0 · test-web-adsb-list-width 122/0 · test-web-navtex-map 48/0 · test-web-shipping-map 44/0 · test-web-map-shared
  36/0 · test-web-airsea-standalone 19/0 · test-web-gamepad, test_nav_coverage, test_gamepad_nav pass ·
  test-web-map-scope 22/1 — "OFF: the ships follow" fails identically on the base 567de0bd4 (pre-existing, not this work).
  Not run here (clips not made): test-web-ndb, test-web-ais-aprs, test-web-ism-map, test-web-lora — their selector edits
  are in; the coordinator's full run covers them.
- Screenshots (before / after, 390 / 820 / 1440, the viewer and the DATALINK map): the session scratchpad `mapcards/`
  (`before-*`, `after-*`, `after-mapcard-*` with the MAP card open).

## The app port (after Stuart has seen the web page)

Reuse `src/utils/mapCards.ts` as is (limits, columns, defaults, FIT trim and default, rate words) and ServersChip's
pattern for each card button: a small chip that expands in place into a panel, collapses on its own chevron / ✕, an
outside tap, or BACK; `useListNav` for the pad; `AccessibilityInfo` reduce-motion for the pop-out.

1. `src/components/MapTopStrip.tsx` — becomes the bar: EXIT, the shared line + CHAT (already there), a row of card chips
   (new `MapCardChips`), the rate at the right. Drop the SHOW row and the TUNE chip from it.
2. New `src/components/MapCard.tsx` — the ServersChip-style shell (chip → panel, PopupShell glass, NavCard keys), one
   per card; `MapCards.tsx` lays them out with `cardColumns(open, width, height)` from `useWindowDimensions` (window,
   never device), persists `open` per narrow / wide in the app's settings store.
3. `src/components/TrackLiveList.tsx` — its body goes into the LIST card (it is a sheet today); the SHOW chips into
   VIEW; `NdbLiveList` / `AdsbLiveList` stay what LIST shows in those modes.
4. `src/components/MapOverlay.tsx` (the WebView page) — host the ADS-B layer's keys outside the map: the WebView keeps
   drawing them (it cannot render into RN), so instead hide `.vsadsb-ctl` there and expose `__trkPrefs(p)` /
   `__trkFitTo(layer)`; the MAP card's RN keys call them. `__trkListW` → an obstacle for the open cards' corner.
   Add the `_fg` grouping and `trimOutliers` to its `trkFitMake` points (the same rule as tracks.ts).
5. `src/screens/SDRScreen.tsx` `airSeaTrk` — compute the facts (not placed, warnings, furthest, TUNE line), the rate and
   the sources (its aircraft.json poll already has `feeds` and `radios`), as main.ts `asCardsSync` does.
6. Copy: the AboutOverlay note's "the app follows" line, the sdrTour if it mentions the map's strip, the watch tutorial
   untouched (no map there).
