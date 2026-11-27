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
- **WEB PAGE DONE (iterating live with Stuart 11-27 to 13:10), APP NOT STARTED** — the app port below. The layout of
  record is "What is where" — the tweak list is history.
- Live preview (while it runs): `http://<Mac LAN IP>:47610/` — scratchpad `mapcards/run-preview.sh` (a local door: the
  Lenovo PiAware Beast feed read-only + a fake RTL replaying a synthetic VDL2 clip; NAVTEX fixtures + a ship store
  seeded by `mapcards/seed.mjs`) and `mapcards/preview.mjs` (serves web/dist, proxies the rest, rebuilds through the
  build lock on every save).

## The live waterfall in the drawer + short landscape (beta97 work, 11-27 afternoon)

- **Where:** a live map over a radio (not the history viewer) offers a folder tab at the LIST drawer's top-LEFT (`.lSwap`,
  the mirror of the maximise tab `.lMax`) carrying the OTHER view's glyph, and the same tab on the collapsed chip
  (`.lSwapC`). W toggles. `TrackView.setWaterfall(on)` offers it; `onWaterfall(pane | null)` tells the host when it shows.
- **What it draws** (`web/client/src/mapWf.ts`, model `src/utils/mapWfModel.ts`, unit `scripts/test_map_wf.ts`):
  passive (no pointer handlers); a MINIATURE of the page's waterfall — the page's own processed frames
  (`Waterfall.onProcessed`: row palette indices, smoothed trace, peak hold) in the page's look (`Waterfall.mapLook()`:
  LUT, `specGradientFor`, outline, peak colour), the slice re-levelled the way auto levels do (floor → index 70).
  Span by mode: dial slice 5× the filter (12 kHz–1.2 MHz); the datalink's channels from hwinfo `avdataBands[].chans`
  (server change) labelled `Ch1 136.700`; the VOR window, fresh per dwell; NDB band; decoded idents stamped into the
  history (never a listed ident). Cropped client-side (a per-listener view change can move a shared IF filter).
- **History:** a ring of palette indices, 1024 columns × the screen's longer side (≤ 1600 rows), re-rendered on any
  resize (small ↔ maximised, rotation, the side panel), kept across list ↔ waterfall (a grey gap line), freed when the
  map closes; only a retune clears it. Light: DPR 1, ≤ 10 rows/s.
- **Spectrum on only while it shows** (`spec` on:1 / on:0). **Server** (`recomputeEngineRate`, `vsMapMode`): in a map
  mode the display FFT runs at the 1 fps housekeeping floor with nobody's waterfall open, ≤ 10 fps with one open; the FFT
  size is untouched (a change restarts the engine and dips everyone's audio).
- **Short landscape** (`mapCards.shortMap`: map height < 480 and wider than tall) — STUART'S SAFE RECTANGLE (11-27
  evening, approved on ls13p): "the map takes the full width, but we build the chip system around the safe zones, so the ✕
  joins the top row". The MAP runs edge to edge under the insets (`fitViewport`); ALL chrome sits inside the safe rectangle
  (x from the left inset to the width less the right inset, above the bottom inset). ONE top row hanging from the top edge
  (no inset there), native boxes, one height (`--trk-tab-h`) and one width (`--trk-tab-w`, both measured by `shortGeom`),
  8 px gaps: ✕ (the first tab, red) · SERVERS · TUNE · LAYERS · STATS · MAP … CHAT · HEALTH · TIME · RATE (the last; wider
  for its words). LAYERS keeps its own width so every family's count shows; a row too long for the window drops ALL the
  family's counts together (`famCompact`). Every card drops from under the row. The LIST / waterfall is a side panel from
  the safe left, under the row, above the bottom inset (300–360 px, its two tabs on its right edge). The pill sits at the
  bottom centre above the inset ("Ask · 3/4" when short of room, never an ellipsis through the count, measured by its
  words). `TrackView.shortGeom` places the bottom-right group — credits (i), ?, zoom [− +], a HORIZONTAL scale — from the
  box and the raw insets, and the pill's span. Insets 0 (Android without a cutout, a short desktop window): the same layout
  full width. Portrait is untouched. ★ ONE block of `[data-short]` rules in tracks.ts (after the portrait list rules) —
  the earlier landscape layouts (bottom-edge family row, right-column edge tabs, inset strips, corner ✕ / RATE) were
  DELETED, not overridden: nothing relies on cascade order.
- **Safe areas:** `viewport-fit=cover` only while a map is up (`TrackView.coverOn`); `--sai-l/-r/-t/-b` = env() as the
  browser reports it (iOS gives both sides the same inset in landscape). Portrait's edge pieces sit on the physical edge
  and pad their content in by the inset; landscape's chrome stands inside the safe rectangle. The island side
  (`islandSide.ts`, `<html data-island>`) is diagnostic only now — no layout value depends on it. Stuart's own device
  numbers (956×376, L62 R62 T0 B20) are the e2e's device profile, with 667×320 (SE) and three Android profiles. `?perf=1`
  shows the decision (`window.__mapGeom`). ★ The radio page itself has NOT had a safe-area pass — it keeps Safari's letterbox.
- **The PURGE (11-27, Stuart: "old and new fighting each other")** — deleted, not hidden: the old ADS-B view / live sheet /
  table / NEWEST-FURTHEST-LONGEST chips and their CSS and code (`adsb.ts adsbListHtml` & co, `#adsbView` `#adsbLive`
  `#adsbList` `#adsbChips`), the old top bar's SHOW / TIME / RANGE / scope / tune rows, the card rail, the list's minimise
  and the docks' tuck leftovers, the old CHAT keys (`#mapChat`, `#adsbChat`); ADS-B live is ONE list like every mode (its
  rows keep their reply SNR chip), the aircraft data's licence words are HELP's. Kept on purpose (still read): the hidden
  `.trkTop .ttl / .rate / .st` data holders (the title, the rate probe, the status line for screen readers).
  ★ APP PORT: the app's MapOverlay still has the old top strip, its list header chips (src/components/AdsbLiveList.tsx,
  TrackLiveList.tsx) and the AUTO/FIT keys in the layer — purge them the same way when the cards land in the app.
- **e2e:** `scripts/test-web-map-waterfall.mjs` (swap, spec on/off and server frame rates, markers per channel, passive,
  palette follows the page, lit + bursts, history kept across maximise / rotation, drawer never grows, tabs clear),
  `scripts/test-web-map-landscape.mjs` (932×370, 932×430, 852×340, 667×320; the 956×376 device profile both island sides and 667×320: every chrome rect inside the safe rectangle, the map under every corner and edge, a real touch per top-row tab opens its own card, one height / width, arrows centred, LAYERS counts; Android; `PREVIEW_URL=` drives the preview).
- **App port:** the same model (`mapWfModel.ts`) and rules; the app's waterfall view takes the processed frames the same
  way; short landscape the same safe rectangle (the app's own insets from react-native-safe-area-context).

## Tweaks from Stuart (live preview, 11-27)

1. NAVTEX stations: no line from the receiver (navtexMap.ts navItems — web and app); the distance stays on the label.
   Other receiver lines left for him to decide: the AMATEUR map's beacons (always), NDB (tapped beacon), VOR radials
   (the fix), mesh routes (on tap).
2. The bar is EXACTLY `EXIT | Shared VFO: Ask before changing mode or exiting 2/5 | CHAT | VDL2 18 msg/min` — no title;
   the card buttons moved to a small vertical RAIL at the map's top-right edge (`.trkMapHost .cardBtns`), the cards
   open beside it (right to left), the fits keep clear of that corner. The line is `dialChat.mapBarLine` (six new
   chat-words keys, 8 languages); the app's MapTopStrip keeps `mapSharedLine` until its port.

## What is where (web) — as of 11-27 13:10 (supersedes tweak 2's bar)

**No top bar** (Stuart 12:50–13:06): the map runs the full height. `.trkTop` stays in the DOM (drivers, slots) but is an
overlay with nothing of its own on show. Four CORNER PIECES, one system:

| Corner | What | Where in code |
|---|---|---|
| top-left | the EXIT — a red corner piece, 52 px, square outer corner; aria/title = what it does ("Close map", "Exit datalink", "Leave server"); greyed by the ask lock when it leaves a whole-radio mode | `.trkTop .exit` (CSS in tracks.ts, label from `setChrome`) |
| top-right | RATE — this radio's live rate (ADS-B n/s · VDL2/AIS n/min) and each feed's own messages a second (`vibe_feeds.h` "perSec", parsed into `AvFeed.perSec`); never a count; hidden with no rate. One source = a static reading; several = "+N" ⌄ and a card dropping from it, a row per source (live / stalled), written in place | `setRateRows` (tracks.ts), rows from `asCardsSync` (main.ts) |
| bottom-left | LIST — the drawer (below) and its chip, the toggle: glyph, count, ⌃ / ⌄ | `.mChipsBL`, `.trkCard2[data-card="list"]` |
| bottom-right | the ? (HELP grows out of it) and the vertical scale — one stack that lifts over the open drawer, 8 px clear of its maximise tab; the scale shortens before it reaches the right column; both step away while LIST is maximised; the map credits' (i) stays hidden under the drawer | `liftOverList`, `scaleDraw` (`scaleMaxPx`) |

Top-centre: the shared-tuner PILL (`dialChat.mapBarLine` words, `mapSharedSync`) — tap = the CHAT card; it wraps rather
than lose its count and keeps clear of the X and the right column (`--trk-pill-pad`).
Left edge, under the X: SERVERS, TUNE, LAYERS (and MAP when the map is tall — `oneSide`, the X counted in the stack).
Right edge, under RATE: CHAT (glyph + unread badge, one gentle pulse per new message; its card holds the chat's own
`#chatLog` + `#chatPhrases`, moved in while open — `mapChatCard` — no typing; folds 2 s after a reply; a message restarts
its idle clock), then MAP in the flank layout, then the zoom ± (`--trk-zoom-top`, always under the column).

**LIST drawer**: flush left/bottom; a phone's full width, else `clamp(420px, 30vw, 640px)`; NORMAL height = 22 % of the
visual viewport in whole rows, 3–7; MAXIMISE (the folder tab on its top edge, M, a long press of the chip) = up to just
under the left chips, AUTO pauses, remembered (`vibesdr.mapListMax`); Escape restores first, then folds. No title row, no
footer strip: the chip floats over the last row with a fade. Header = one scrolling tab row (edge fades + ‹ › only where
there is more) + ONE sort toggle (rings = nearest, clock = latest). Rows: `[flag] NAME [MIL][EMERG] [FL241 | 97nm]` over
the subtitle; right side the SNR chip and the age (compact on a phone); no bearing (`trkRowHtml`, `trkKvPill`,
`acTags.ts`). LIST steps aside (measured) while a transient card would cover it.

**STATS** (left edge, under LAYERS; the house 'stats' glyph): per MODE present (ADS-B, HFDL, VDL2, ACARS, ADS-C) — NEWEST,
FURTHEST (from the receiver), FURTHEST TRACKED (distance flown while that mode heard it) and LONGEST TRACKED (time heard);
modes never compete; a row selects that aircraft and eases the map to it (`setStats`, main.ts `asStatsSync`). AIS owed.

**Under the map** (MAP-TEMPLATE.md rule 4): a same-page map parks the radio page's render loop and sends
`{"type":"spec","on":0}` (the shim's `specPaused`), `on:1` on close; a new-tab viewer leaves the radio tab alone. The
APP: the non-live viewer keeps the waterfall flowing and its history ingesting (as today); LIVE modes follow the web.

**Transient cards** (TUNE, LAYERS, SERVERS, MAP, HELP, RATE, CHAT, STATS): one at a time; fold on a map tap, another card,
`CARD_IDLE_MS` idle, Escape / BACK; AUTO pauses while one (or an item's card, or the maximised LIST) is open and resumes
without a forced glide. Every card is opaque, no backdrop blur; the map never resizes under a card.

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
★ b97 (branch b97-maps-port): the AMATEUR / spots map (its own window) and the directory's COMMUNITY MAP are on the cards
too — through `mountMapChrome` with `hostMap` (each keeps its own MapLibre / Leaflet map and layers; the chrome keeps the
scale chip, the credits' fold and the shared AUTO / FIT on it). See "The standalone pages" below.
Left as they are, on purpose: the satellite sky plot / ORBCOMM (satellites.ts, not a TrackView), the handheld page's map
(handheldMap.ts, pad-only screen).

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

## The module (11-27, after Stuart's sign-off: "yes these are the maps")

- `web/client/src/mapChrome.ts` — THE card system: `TrackView` (chips, cards, the LIST drawer + live waterfall, the
  short-landscape top row and the safe rectangle, RATE, HELP, STATS, the pill, the bottom-right group), its CSS, `esc`,
  `ago`, `LatLon`. Was the bottom of `tracks.ts` (git mv keeps its history); `tracks.ts` keeps the AIS / APRS looks
  (icons, rows, cards) and re-exports the chrome, so its importers are unchanged.
- `web/client/src/mapChromeApi.ts` — ★ THE STABLE SURFACE for new maps: `mountMapChrome(opts)` → `MapChrome` with
  `chips()` (which chips / cards exist), `card()`, `layers()` (LAYERS + its counts), `list()` / `listTabs()` / `listSort()`
  (the LIST source), `rate()` (RATE rows), `stats()`, `servers()`, `chat()`, `credit()`, `onRow` / `onSelect` / `onCard`,
  `show` / `hide` / `destroy`, and `view` (the TrackView, an escape hatch). Add, never rename or remove. The amateur /
  spots map and the directory's community map are ported onto it; the air & sea and ISM maps drive TrackView directly.

## The app port (after Stuart has seen the web page)

Reuse `src/utils/mapCards.ts` as is (limits, columns, defaults, FIT trim and default, rate words) and ServersChip's
pattern for each card button: a small chip that expands in place into a panel, collapses on its own chevron / ✕, an
outside tap, or BACK; `useListNav` for the pad; `AccessibilityInfo` reduce-motion for the pop-out.

1. `src/components/MapTopStrip.tsx` — NO BAR any more (13:06 layout above): it becomes the four corner pieces (EXIT
   top-left, RATE top-right, LIST bottom-left, ? + scale bottom-right), the shared-tuner pill top-centre and the edge
   chips (SERVERS / TUNE / LAYERS left, CHAT / MAP right). The pill and CHAT exist today; RATE needs the feeds'
   `perSec` (already in the app's aircraft.json poll). Safe areas from `useSafeAreaInsets`, never a device check.
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

## The standalone pages (b97, branch b97-maps-port)

- `web/client/src/mapChromeStandalone.ts` → `scripts/gen-mapchrome-source.mjs` → `window.VibeMapChrome` (mountMapChrome,
  mapRowHtml, esc, ago) in two GENERATED copies: `web/client/src/generated/mapChromeSource.ts` (MAP_CHROME_JS — the
  amateur map's window inlines it; lazy, with the map) and `directory/public/map-chrome.js` (the community map page).
  index.html's `.edgeDock` / `.gpFocus` / `.trkView` rules are lifted in; the radio page's gamepad, chunk cache, Leaflet
  asset, GPU probe and layer sources are stubbed. `build-web.mjs` writes both first; `run-tests.sh` runs `--check`.
- API additions (mapChromeApi.ts, additive): `hostMap` / `MapHost` (the host's own map in the chrome's holder; `points` =
  the shared AUTO / FIT on it), `controls()` (the host's section of MAP CONTROLS), `ranges()`, `legend()`, `retitle()`,
  `cardBody()`, `fitNow / fitPoke / fitPause`, `root`, `mapRowHtml()` (rows carry data-nofocus: the host focuses its map).
  ★ hostMap reaches TrackView's private members by name (listed in the file) — rename them there and here together.
- Layout check for any map on the cards: `scripts/lib/map-chrome-layout.mjs` (visible, inside the window / safe rectangle,
  no overlaps) at 1440×900, 390×844, 412×915, 915×412, 956×376 (L62 R62 B20), 667×320 — used by
  `scripts/test-web-digimap-cards.mjs` (the built page, no server) and `directory/scripts/test-community-render.mjs`.
