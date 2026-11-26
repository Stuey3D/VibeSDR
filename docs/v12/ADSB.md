# ADS-B 1090 (V12): the server contract and the client behaviour

Our own decoder (`android/app/src/main/cpp/vibe_adsb.h`, service `vibe_adsb_service.h`), wired into VibeServer as a
whole-radio mode like DAB. Branch `v12-adsb`. Memory: `adsb_v12`.

## Server contract

**Capability** — `/vibeserver.json` (ask EACH radio, `/r/<id>/vibeserver.json`, never the front door — as for DAB):

| field | meaning |
|---|---|
| `"adsb": true` | this receiver can run ADS-B now |
| `"adsb": false` + `"adsbWhy": "…"` | it cannot; the reason in words. Show the control **disabled with this reason** (never hidden) |
| no `adsb` field | an older server: no control |
| `"modes"` contains `"adsb"` | same as `adsb: true` (directory badge) |

Reasons the server gives: the owner switched it off; a transceiver; centre locked by the owner; the radio cannot
tune to 1090 MHz; **the capture is below 2.4 MS/s** ("ADS-B needs a capture of 2.4 MS/s or more — this receiver runs
at 2.048 MS/s"). ADS-B **never changes the sample rate** (Stuart, 2026-10-24: the 32-bit re-rate teardown).

**Client → server** on the spectrum socket: `{"type":"adsb","on":1}` / `{"type":"adsb","on":0}` (missing `on` = off).
While ADS-B is on, `tune`, `mode`, `bandwidth`, `zoom`, `reset`, `tunerbw`, `sampleRate` are **ignored** — the
client must disable those controls (as in DAB). Entering ADS-B from DAB is allowed: the server leaves DAB itself
(clients get `dab_off`). Entering DAB from ADS-B likewise sends `adsb_off` first. Gated by ask-before-tuning.

**Server → client** (JSON text frames, to every spectrum socket):

- `{"type":"adsb", "now":s, "messages":n, "aircraft":[…], "rate":Hz, "noise":dBFS, "msgRate":per s, "dropped":n}`
  once a second while on, and immediately to a socket that joins. **Its arrival means ADS-B is on: open the mode's
  screen** (the app: the full-screen map; the web client: the box).
- `{"type":"adsb_off"}` then a `config` — close the map / box, the receiver is back where it was.
- `{"type":"adsb_error","why":"…"}` — refused; show the reason.

**An aircraft** (dump1090 `aircraft.json` field names; absent = unknown):

| field | | field | |
|---|---|---|---|
| `hex` | ICAO address, 6 hex (`~` prefix = non-ICAO) | `gs` | ground speed, kt |
| `flight` | callsign | `track` | track over ground, ° |
| `category` | `A0`–`D7` emitter category | `mag_heading` | heading, ° (airspeed messages) |
| `lat`,`lon` | position (≤ 60 s old) | `tas` / `ias` | airspeed, kt |
| `seen_pos` | seconds since the position | `baro_rate` | vertical rate, ft/min |
| `alt_baro` | ft, or `"ground"` | `squawk` | 4 digits, string |
| `alt_geom` | GNSS height, ft | `emergency` | non-zero = emergency state |
| `seen` | seconds since any message | `messages`, `rssi` | count, dBFS |

Aircraft not heard for 60 s are dropped by the server.

**Comm-B / Enhanced Surveillance** (V12, `vibe_adsb_commb.h`): an aircraft that has answered a ground radar (DF20/21)
may carry `"commb":{…}` — **confirmed registers only** (data parity, two agreeing ADS-B quantities, or a second reply
within 30 s with close values; an ambiguous field is thrown away, never guessed). Values are 30 s fresh (4,4 120 s,
2,0 5 min, an ACAS advisory 1 min). Fields, in aircraft.json's names: 4,0 `nav_altitude_mcp`, `nav_altitude_fms` (ft),
`nav_qnh` (hPa), `nav_modes` (`vnav`/`althold`/`approach`); 5,0 `roll` (° right +), `true_track`, `gs`, `track_rate`
(°/s), `tas`; 6,0 `mag_heading`, `ias`, `mach`, `baro_rate`, `geom_rate`; 4,4 `ws`, `wd`, `oat` (°C), `static_hpa`,
`humidity`; 2,0 `ident`; 3,0 `acas_ra{age, active, terminated, advisory, threat}`; 1,7 `bds` (which of 4,0/4,4/5,0/6,0
the aircraft says it has). The report gains `"commb":{"fields","confirmed","ambiguous"}` counters. The card shows them
under "From its replies to radar (Mode S)", kept apart from the ADS-B rows above them.

**What each aircraft IS** (V12 aircraft data, 2026-10-24 — design and licences: `ADSB-DATA-RESEARCH.md`) arrives in its
own message, ONCE per aircraft and again only when it changes (a callsign arrives, the route is re-judged), just before
the report it belongs to; a socket that joins gets the whole set (`full:1`) before its first report:

`{"type":"adsb_info","aircraft":{"4ca2d6":{"r":"EI-DLJ","t":"B738","desc":"BOEING 737-800","wtc":"M","eng":"2J",
"species":"L","country":"IE","countryName":"Ireland","op":"RYR","opName":"Ryanair","logo":"RYR-B738","sil":"B738",
"route":{"from":"GCRR","to":"EGBB","fromIata":"ACE","toIata":"BHX","fromName":"…","toName":"…","fromPlace":"…",
"toPlace":"…","stops":[…],"plausible":true}}}}` — `{}` for a hex = it no longer has any. All fields optional.

| field | | source |
|---|---|---|
| `r`, `t`, `desc` | registration, ICAO type, its description (readsb's names) | Mictronics (ODC-By), VRS overlay (CC0) |
| `wtc`, `eng`, `species` | wake L/M/H/J, engines `2J` (count + J/T/P/E/R), L/H/S/A/G/T | type tables |
| `country`, `countryName` | ISO alpha-2 of the state of REGISTRY | our Annex 10 table + VRS territories |
| `mil` | `1` = military (omitted otherwise) | VRS military blocks, the Mictronics flag |
| `op`, `opName` | ICAO operator from the callsign; never a person's name — `"Private"` | Mictronics + VRS airlines |
| `logo`, `sil` | a key the server has a file for: `GET /adsb/logo/<key>`, `/adsb/silhouette/<key>` (the OWNER's own) | admin page upload |
| `route` | the leg the aircraft is on; `plausible:false` = far from it (shown struck through) | VRS routes (CC0) |

- The clients merge these into each aircraft by hex (`src/services/adsbInfo.ts`, both clients), so the list and the
  shared layer's card read `a.r`, `a.op` … directly. Military: **the server says (`mil`), else the old rule**.
- The data: `<data dir>/adsb/vrs.vad` + `mic.vad` (mmap'd, binary-searched), a CC0 starter compiled in
  (`vibe_adsb_starter.h`), a weekly jittered conditional download (`vibe_adsb_data_update.h`); the admin page shows it,
  switches it off, and installs the owner's logo/silhouette zips. Photos (Planespotters, client-side) are built but
  OFF until their terms are read at first hand — the TODO in `adsblayer.js`.

**Category → icon** (DO-260B emitter category):
A1 light (< 7 t: light prop) · A2 small (7–34 t: regional/bizjet) · A3 large (34–136 t: narrow-body jet) ·
A4 high-vortex large (B757) · A5 heavy (> 136 t: wide-body/jumbo) · A6 high performance (> 5 g, > 400 kt: fighter) ·
A7 rotorcraft (helicopter) · B1 glider · B2 lighter-than-air (balloon) · B3 parachutist · B4 ultralight · B6 UAV ·
B7 space vehicle · C1 emergency surface vehicle · C2 service surface vehicle · C3 obstacle · A0/none = generic jet.
★ beta96: A0 / none no longer means the jet outright — `iconClassOf` (adsblayer.js) then asks the aircraft data (type,
wake, engines, description letter), so datalink and remembered aircraft get their class too. docs/v12/AIRCRAFT-ICONS.md.

## Client behaviour (Stuart, 2026-10-23/24)

- **The app (Stuart, 2026-10-24 — supersedes "box + MAP button"):** "when ADS-B is selected the waterfall drops and
  the full-screen map is on, an Exit ADS-B button is at the top of the screen, same as our HFDL map exit", with "a live
  list in the bottom corner".
  - A report arriving (whoever started the mode, a second listener, connecting to a radio already on 1090) opens
    `MapOverlay` kind `'adsb'` straight away — an opaque modal, so the waterfall stops drawing behind it
    (`useCoversScreen`); the server slows it to 1 fps meanwhile (gain/AGC/link keep working).
  - The top bar's key, in the HFDL key's place and style, reads **EXIT ADS-B** and **leaves the mode** (`exitAdsb`),
    unlike HFDL's "← SDR", which only closes its map. Android back, a pad's Back and the keyboard's Backspace/Esc do the
    same. `adsb_off` takes the map down and the receiver screen is back as before. On a locked dial (ask before
    tuning) the exit is refused: the map is put away so the ask nudge can be seen, and the ADS-B key brings it back.
  - **The live list** (`src/components/AdsbLiveList.tsx`, logic `src/services/adsbList.ts`, test
    `scripts/test_adsb_list.ts`): bottom-left, nearest first — callsign/hex, FL (≥ 18 000 ft) or ft, kt, NM + compass —
    3–8 rows (never over half the map), scrolling; a tap or Enter calls the layer's `focus(hex)`; the header folds it to
    a ✈-count pill (kept per device, `lsv_adsb_list_open`). ★ Drawn by React Native over the WebView, not in the page,
    so the D-pad/keyboard reach it through PanelNav (a pad is never seen by the WebView on iOS/Mac). The page moves its
    ⓘ legend to the top-left in ADS-B and narrows the attribution to the room right of the list (`__adsbListW`).
  - A refusal (`adsb_error`) closes the map and shows the reason as an alert.
  - **Coming back to it (2026-10-27 — Stuart: the map "dropped back to a waterfall view on the previous frequency" after
    a switch to another app).** With nothing to hear the app releases the audio session, so iOS suspends it soon after
    it goes to the background and its sockets go with it. The server holds a whole-radio mode (ADS-B, NDB, ISM, AIS,
    DAB) after its last listener for the owner's idle grace, capped at 60 s (never under the old 5 s — `modeLeaveGraceS`
    in the shim). Past that it gives the radio back and records the leave; the returning app — whose report silence is
    now counted only while a spectrum socket is open (`src/services/wholeModeResume.ts`) — sends
    `{"type":"mode_resume","mode":"adsb"}`, and the server re-enters **only** if nobody else is on the radio and nobody
    has tuned it since, answering `{"type":"mode_resume","mode":…,"ok":1|0,"why":…}`. A refusal (or no answer from an
    older server) closes the map with the reason. The web client does the same for a backgrounded tab. Tests:
    `scripts/test_wholeModeResume.ts`, `scripts/test-server-mode-resume.mjs`.
  - **Since v12-session-hold (2026-10-27) the server HOLDS the suspended listener's session for 3 minutes** (capped by
    the owner's idle grace, cut short by a hard limit). The mode, dial, decoders and VFO B carry on, the place stays
    counted, and on iOS a "VibeSDR paused" note says until when. The 60 s grace and `mode_resume` above are what applies
    after the hold. See BACKGROUND-DECODING.md, "The held session".
  - **Apple TV** has no WebView: there the old box (`AdsbPanel`, AircraftPanel list, EXIT ADS-B, no MAP key) is the
    interface. Android TV has one and gets the map (`IS_APPLE_TV`, src/utils/tv.ts).
- **The web client (2026-10-24): the map IS the view too** — full screen over the page, EXIT ADS-B in a top bar,
  a live list in the bottom corner (below).
- **The map** (our own MapLibre maps, GPU — the same stack as the HFDL map; in the web client too):
  - **auto-zoom** (toggle): the view always just contains every aircraft with a position; one appearing 200 NM out
    pulls the view out; when it drops off the view comes back in to the next furthest. Smooth, not jumpy.
  - **fit to aircraft** (toggle): the receiver need not be centred — if traffic is mostly to the south-east the
    receiver moves towards the top-left so more of it fits at a closer zoom. Off = receiver centred.
  - **range rings** (toggle) in NM around the receiver.
  - The three toggles live **inside the map**.
  - **New aircraft breathe** softly (or a ring as on the HFDL map) — **only after the initial load**, never the
    first batch, or everything flashes.
  - **Type icons** from `category` (helicopter, heavy, medium/large jet, light prop, fighter, …), rotated to track.
  - **Smooth glide** between position updates.
  - **Airports and runways** as on the HFDL map.
- The receiver position is the server's own location (`/location`), as the HFDL/spots maps already use.

## The shared map layer — `web/mapkit/adsblayer.js` (web client + app)

One file draws the aircraft on every ADS-B map. Edit only `web/mapkit/adsblayer.js`, then
`node scripts/gen-adsblayer-source.mjs` (writes `web/client/src/generated/adsbLayerSource.ts`,
`src/generated/adsbLayerSource.ts` — the string the app injects like `VIBEMAP_JS` — and
`directory/public/mapgl/vendor/adsblayer.js`, which the server installs at `/mapgl/vendor/adsblayer.js`).
`node scripts/test_adsb_layer.mjs` tests the maths and fails on a stale copy. No backticks in the file.

```js
const layer = VibeAdsbLayer.create(map, { receiver: {lat, lon} | null, prefs: {autoZoom, fit, rings} | null,
                                          onPrefs: (prefs) => void, theme: 'dark' | 'light',
                                          drawReceiver: true });   // ADDED: false = the host draws its own house
layer.update(aircraftArray)   // the report's whole `aircraft` list, every report
layer.setReceiver(lat, lon)   // 0,0 or null = unknown
layer.setPrefs({autoZoom, fit, rings})
layer.focus(hex)              // ADDED: select + open the card (a list row tapped); pans only if off screen
layer.prefs()                 // ADDED: the current prefs
layer.destroy()
```

- `map`: a MapLibre map (`vm.map`), the compat adapter — **the app's GPU path passes the Leaflet-shaped
  wrapper from `VibeMapGLCompat.install`, as the HFDL map does; the layer detects it and draws on its `._gl`
  (the underlying MapLibre map) directly**, GPU layers rather than compat markers —
  or a real Leaflet map (fallback: DOM markers at a lower frame rate, same behaviour). **The host creates
  the map with the `aero` profile** — that is where airports and runways come from (as on the HFDL map);
  the layer draws only the aircraft, the receiver house, rings and the toggles. `drawReceiver: false`
  (the app: its page already draws the HFDL receiver marker) skips the house only — the position still
  drives rings, distances and the camera. `maplibregl` must be global
  for the aircraft card.
- **The range outline** (beta94) — see "The range outline" below. `prefs.range` (default true) and `prefs.rangeAll`
  (default false) join the prefs; `layer.rangeShown()` is a test's view. The layer fetches `<imageBase>range` itself.
- **The card** (beta94, Stuart's greyed ×): its text scrolls in the layer's own `.vsadsb-sc`, never in the popup's
  content box — MapLibre puts its × at the top-right of that box, where an overlay scroll bar (macOS) lay over it and
  took the press. The × and the fold key sit above the scroller (z-index) and never move. On the GPU map a refresh
  refills `.vsadsb-sc` only (`renderCard`), so MapLibre's × is made once per card; `setHTML` is used only to open it.
  `node scripts/test-web-adsb-card.mjs` (headless Edge, silent) holds it.
- Defaults `{autoZoom: true, fit: true, rings: true}`; `onPrefs` fires only when a person flips a toggle.
  The host persists them. A camera move the layer did not start, within 1 s of a touch/click/wheel/key on
  the map (or carrying MapLibre's `originalEvent`), **pauses** auto-zoom (AUTO shows hatched) until AUTO or
  ⌖ is tapped. A host's own programmatic move does not pause it.
- Feel = the HFDL map's constants: 2.2 s eased corrections over dead-reckoning (gs/track, ≤ 20 s), corrections
  > 5 NM fade across instead; new aircraft get the HFDL ring ×3 + breath, never in the first batch nor the first
  10 s; HFDL age tones by position age (green < 10 s, yellow-green < 30 s, amber), grey on the ground, red in
  an emergency; labels decluttered twice a second (sticky, fade), FL from 18 000 ft (AircraftPanel's rule).

## The web client (web/client/src/adsb.ts + main.ts, beside DAB)

- `ADS-B` sits beside `DAB` in the demodulator menu: live when `adsb: true`, greyed when `false` (a tap shows
  `adsbWhy`), absent on an older server. An `adsb` report opens the map view whoever started the mode (a second
  listener joining, or a page loaded into a radio already in ADS-B, lands straight on it); `adsb_off` closes it;
  `adsb_error` is shown as a pill.
- **THE MAP IS THE MODE** (Stuart, 2026-10-24 — supersedes the decoder box + MAP button + popup window):
  `#adsbView` covers the page, the waterfall and the deck are hidden behind it (made inert, their drawing paused
  as behind the START screen; nothing underneath is rearranged, so leaving restores them exactly).
  - Top bar, the HFDL map's (MapOverlay.tsx `#topbar`): **EXIT ADS-B** at the left (sends `{type:'adsb',on:0}`),
    the title, the count on the map + heard + msg/s, and **CHAT** (the deck's key is under the map; same panel,
    same unread count, greyed with it).
  - The GPU map in the page (MapLibre from this server's `/mapgl/`, `aero` profile, greyline), the Leaflet map
    where there is no WebGL 2; made on entry, destroyed on exit. The layer comes from the bundle as a string.
  - The server health pill stays, moved below the map's AUTO / FIT / RINGS. Panels, pills, the owner's notice,
    the tour and the START / idle / busy screens are all above the view (it sits at z-index 47).
  - **The live list**, bottom left: nearest first (last heard without a receiver position), callsign / hex ·
    squawk, FL / ft + climb, kt (not on a phone), NM + compass bearing; ~7 rows then it scrolls; a row → the
    aircraft's card on the map (`layer.focus`). Its head collapses it to a pill (remembered, `vibesdr.adsbListMin`).
    The credits close to their ⓘ on a narrow map; opened, the list lifts above them.
  - Keyboard / controller: the view is a `[data-navscope]` — the highlight stays in it; the list is a
    `[data-navbox]` like the DAB list (enter takes the D-pad, rows are the stops, back gives it back). Back with
    nothing else open **leaves ADS-B**, as the HFDL map's back closes it.
  - Prefs live in the page's localStorage (`vibesdr.adsbMapPrefs`).
- Held while in ADS-B: tune, arrows, step, zoom, FIT, frequency entry, IF filter, passband, sample rate
  (greyed with the reason, and stopped in SpectrumClient). A demodulator picked in ADS-B leaves the mode and is
  applied to the dial the server gives back (no 1090 MHz tune is sent).
- ★ **The server sends next to no audio in ADS-B** (bench: 131 B in 6 s), so the web client holds its audio
  self-heal for as long as reports keep coming, or it reopened the audio socket every 6 s. The app may need
  the same.

## ADS-B as a radio's landing, and keep it running (2026-10-24)

Stuart: *"can we set ADSB as a landing page for an SDR and when it is set that way have a tick box to keep it running when
nobody is connected that way a new connection instantly sees a list of planes and a full map"*.

- **Config, per radio:** `landingAdsb` (bool) and `adsbKeepRunning` (bool, meaningful only with `landingAdsb`). Both off by
  default. `landingFreq`/`demodMode` stay the fallback: EXIT ADS-B goes there. A hand-written file that also has a DAB
  landing lands in ADS-B (one landing per radio; both pages offer one or the other).
- **Only where ADS-B can run** (`vsAdsbWhyNot` empty — not an HF+ or an Icom, not a locked centre with own-VFO listeners,
  2.4 MS/s or the owner's rate switch, not blocked). Elsewhere the toggle is not drawn; where only a setting stands in the
  way the setup page says which. `/vibeserver/hardware` carries `adsbHw` / `adsbHwWhy` for that.
- **Landing:** the DAB landing's rules (local_sdr_shim.cpp, the spectrum accept path) — every new session on a radio that
  is not a shared dial; on a shared dial only the first listener after a start. The plain landing is applied first, then
  ADS-B is entered through the same internal `{"type":"adsb","on":1}` a listener's button sends.
- **Keep running** (`adsbKeepTick`, every 2 s on the housekeeping thread): with nobody here and no whole-radio mode on,
  the radio is taken back if parked/released, put on the plain landing and entered into ADS-B — at the server's start and
  after the last listener leaves. The rate change, if any, happens then, once; a connection to a radio already in ADS-B
  changes nothing and gets the full aircraft table in its first messages (measured: 122 aircraft within 3 ms of connect).
- **Precedence — kept-running ADS-B owns the idle radio.** No idle park, no release to another program, no RSP keepalive
  release, and Background Decoding profiles for that radio do not run (`bgTick`'s `can` is false; the admin line says
  why; the setup page greys the profiles and says so). Chosen this way round because the owner who ticked it has said what
  the idle radio is for more specifically than a profile does, and the two cannot share it: ADS-B takes the whole capture
  at 1090 MHz where no profile VFO can be. A radio where ADS-B cannot run falls back to the ordinary idle behaviour.
- **Cost:** continuous decoder CPU — about 13 % of a Pi 3 core at 2.4 MS/s — stated on the tick box.
- Tests: `scripts/test-server-adsb-landing.mjs` (real server, fake rtl_tcp replaying the FlightAware clip),
  `scripts/test-setup-adsb-landing.mjs` (the page's real functions), `vibeserver/test-config-radios.cpp` (every hop).

## Is the radio delivering every sample? (2026-10-24)

- **Live `capture`** in every `adsb` report: the % of the samples the rate promises that the ADS-B service actually
  RECEIVED over the last ~10 s (`vibe_adsb_capture.h` CaptureMeter; -1 for the first 3 s). It sees the dongle/USB
  shortfall and the server's own IQ queue drops; the service's own queue drops stay in `dropped` (a slow CPU). Under
  97 % both map top bars say, in red, "capture N % — the receiver is dropping samples" (`adsbCaptureWarning`, one rule
  in `src/services/adsbTypes.ts` and `web/client/src/adsb.ts`).
- **Benchmark rows** `adsb_2400` / `adsb_3000` (`vibe_benchmark_adsb.h`): CPU only, the radio untouched — one second
  of Stuart's 1090 MHz sky at 2.4 and 3.0 MS/s fed in real time through `vibeadsb::Service`, scored like the DAB rows.
  The clip (`adsb-bench.vbu8.gz`) is fetched on the first run from the `bench-clip-v2` GitHub release —
  `vibeserver/bench-clip/README-adsb.txt`.
- **A radio that falls over at a boosted rate** (`vibe_rate_guard.h`, ADS-B or DAB under the rate switch): a stall
  (no IQ for 3 s) leaves the mode back at the base rate; a whole-process crash is found at the next start from the
  marker written just before the boost. Either way the rate switch is OFF (persisted) and the setup page and the
  app's server screen say so in red beside it until the owner clears it (`/vibeserver/rate-guard?clear=1`).
  `scripts/test-rate-guard.mjs` drives both through the real server.

## The gain rule (2026-10-26) — `vibeadsb::GainControl`, `vibe_adsb_service.h`

**Why it changed.** From 10-24 the rule was messages only: ramp from the middle of the gain list, a higher rung kept
unless clearly worse, stopped by a cliff. On the Pi 500's V4L (YouLoop → wideband Nooelec LaNA, no 1090 filter) the
cliff is real — out-of-band energy overloads the tuner at 43.4 → 44.5 dB — and it settles at 40.2. On Stuart's
FlightAware Pi (an LNA **and** a 1090 MHz filter) nothing out of band reaches the tuner, so there is no cliff and the
message count creeps up to the top: it pinned 49.6 dB. The 12.5-hour A/B against dump1090-fa (10-25) on that Pi: +5.8 %
messages, −2.3 % positioned aircraft; our best runs were where the climb happened to stop at 37–43 dB. Messages are
dominated by near aircraft; past the point where the noise fills the ADC, gain buys their replies with clipping (57 %
of replies within 3 dB of full scale at 49.6 on the 10-24 clip) and costs the distant ones. **The filter, not the LNA,
is what told the two apart** — both have an LNA; only the unfiltered one has a cliff to stop the climb.

**The rule.**
- **A noise ceiling**: our floor (`Demod::noiseDbfs`, the quietest 256-sample block of the last 16, ×1.1) must stay at
  or under **−30 dBFS**. Mapped on the FlightAware clip with the gain lowered in steps: ours sits 0.3–0.7 dB above the
  40th-percentile power (what dump1090-fa's adaptive range judges, 30 dB target on an RTL) and **11.3 dB under
  graphs1090's "noise"** (the mean power of everything not decoded, fruit included). So dump1090-fa's 30 dB of range ≈
  our −30, and its graphs1090 "−20 dBFS" ≈ our −31.
- **Entry ramp**: middle up, every other rung, ~6.5 s each, measuring messages *and* the floor. Stops at the first rung
  over the ceiling (choice made among the rungs below), at a message cliff, or two rungs past the peak; the rung above
  the choice is then tried A/B. **One step over the ceiling is allowed only on a clear message win** (3 % and 2σ, A/B):
  −30 is calibrated on one filtered feed, and an unfiltered one whose floor sits a little higher (a V4L whose limit is
  its cliff) must not be cut below its best for it; the FlightAware feed's next rung adds ~1.5 % and never earns it.
  A middle already over the ceiling steps down a rung every 4 s. An empty sky: the highest rung under the ceiling.
- **Steady**: floor (smoothed) over the ceiling by more than 1 dB for 10 s → one rung down. Every ten minutes one rung
  either side, kept only if it clearly decodes more — and up only when the rung above is predicted under the ceiling.
- **Strong-reply pressure**: > 30 % of replies within 3 dB of full scale for 30 s proposes a rung down (A/B, taken
  unless it clearly loses); then no climbing for 30 min. (FlightAware Pi healthy: ~11 %; V4L ~8 %.)
- **SDRplay (IF AGC on)**: no ceiling (its floor is the AGC's); starts three-quarters up, only looks down, never above
  the start; strong pressure steps the LNA down.
- **Shown**: `gainWhy` in the report (the GAIN card, web and app); `feedWarn` when at the top rung for 10 min with the
  floor 6 dB or more under the ceiling and a thin sky (nothing past 100 NM, or without a position < 40 aircraft and
  < 400 msg/s) — a red CHECK THE ANTENNA banner over the map. Replayed on 2.5 years of the FlightAware Pi's graphs1090:
  121 of the 126 fault-month rows at full gain (May–June 2025), 0 of 2 249 healthy rows.

**Tests**: `vibeserver/test-adsb-gain.cpp` (model feeds built from the FA clip, the V4L's beta13 curve, the Pi 2's bare
antenna, the 2025 fault, hysteresis, strong pressure, SDRplay), `scripts/test-adsb-gain-e2e.sh` (the real server on
the FA clip with `fake-rtl-tcp --adsb-gain`: 25.4=1043/−42 … 40.2=1388/−31, 43.4=1367/−28 → **40.2 dB**, plus a
reconnect inside the leave grace).

**A/B on real hardware**: `scripts/ab-adsb-gain.py` — arms (dump1090-fa, a VibeServer build) alternated in windows on
the one dongle, one CSV row per window (the 10-25 `fa.csv` columns + `noise_med`, `why`, `warn`). Stdlib only.


## The range outline (beta94) — `vibe_adsb_range.h`, `adsblayer.js` rangeRing

Stuart, 2026-11-25: *"can our adsb maps for the server put a jagged ring around the receiver showing the maximum range
detected. my virtual radar is setup like that"* — Virtual Radar Server's range plot. Then: *"an expiry so one super strong
signal one day doesn't permanently skew the ring"*, *"make the line thicker and a bolder colour"*, and the reset *"in the
admin section with the aircraft settings"*.

**Server.** Per radio (each radio is its own process): for each 1° bearing (bin *i* = [*i*, *i*+1)°) the furthest believed
position per UTC DAY, for the last 90 days (a ring of day slots, slot = day mod 90, wiped when reused), plus the all-time
maximum per bearing (with when) and the furthest catch ever. The outline served is the maximum per bearing over the owner's
WINDOW: 7, **30 (default)**, 90 days, or 0 = all time — a one-day tropo opening leaves it when its day leaves the window.
- Counted: a position from an aircraft with **two consistent fixes in a row** (`Aircraft::posOk ≥ 1`, set by `setPos`:
  the second within 1 200 kt of the first; a jump resets it), at most **650 km** from the receiver, offered once a second
  (`Service::loop`). After the Tracker's own checks (CPR pairs, 360 NM cap, single-frame wait).
- Centre: the server's location at ADS-B start. Moved > 5 km → a new outline (a different site's).
- Storage: `<data>/adsb-range-<serial>-adsb.bin` (the phone: `adsb-range-adsb.bin` beside its bookmarks) — binary,
  versioned, 69 KB whatever the sky, written atomically (temp + rename) at most once a minute and when ADS-B stops.
- `RangeStore` is per SOURCE; only ADS-B exists. ACARS / VDL2 outlines were asked for and then held (they rarely carry a
  position) — a source is one more `RangeSrc` with its name and distance limit. HFDL (worldwide) would never have one.

| Endpoint | |
|---|---|
| `GET /adsb/range[?src=adsb]` | `{ok, window, lat, lon, since, now, km[360], day[360], allKm[360], days, far{km,brg,at}, record{km,brg,at}}` — km 0 = nothing heard that way; `far` is the window's furthest (`at` = its UTC day), `record` the furthest ever. PIN-gated like aircraft.json, CORS open. ~7 KB. |
| `GET/POST /vibeserver/admin/adsbrange` | the admin page, per radio (`/r/<serial>/…`), admin proof: the same JSON + `available`; POST `?reset=1` (clears the outline, its all-time line and record; window kept), `?window=7\|30\|90\|0` (400 otherwise). |
| `POST /vibeserver/adsb-range?reset=1 \| ?window=N` | the same for the app's server screen on the phone that IS the server (loopback = the owner, `vsAdminHttpOk`). |

**Map.** `RANGE` beside RINGS (default on; drawn only once the server has answered — an older server: no button). A 3 px
magenta edge (`THEME.range`: 255,56,210 dark / 205,0,150 light — clear of the aircraft tones and the amber rings) over a 9 % /
7 % fill, under the rings and every aircraft; the all-time outline as a dotted 1.5 px line behind it (off; the key's tick
box). A bearing with nothing heard is skipped (the line joins its neighbours). The key: the window, *Furthest: 402 km at
141°, 8 Nov*, *Furthest ever: 512 km at 160°, 3 Nov*. Fetched on open, then every 5 minutes while shown and the page is
visible — never per report.

**Owner.** Web admin page → WHAT THIS RECEIVER HAS LEARNED → ADS-B RANGE OUTLINE, one row per radio that can run ADS-B
("Built over 23 days · furthest … · furthest ever …", DRAWN OVER, RESET RANGE OUTLINE with *"Clear the range outline for
<radio>? It starts again from the next aircraft heard."*). The app's server screen: the same card while the server runs.

**Tests.** `vibeserver/test-adsb-range.cpp` (bins, limit, the window and expiry, slot reuse, bounded file, damage, reset,
moved site, store persistence, posOk) · `scripts/test_adsb_layer.mjs` (rangeRing) · `scripts/test_adsb_report.ts` (prefs,
the owner's line) · `scripts/test-web-adsb-range.mjs` (headless Edge: the polygon from fixture bins, order, bold edge,
toggle remembered, key, all-time line, no button on an older server, both themes; `VIBE_SHOTS=dir` saves screenshots) ·
`scripts/test-server-adsb-range.mjs` (a real vibeserver + synthetic 1090 clip: fills, admin 401/200, window, restart,
reset → served empty, the loopback door).

**RESUME HERE.** Not yet seen on air: the outline from a real sky (Pi 500 / Lenovo ADS-B) over a few days — check the
shape against the owner's own expectation and that nothing spikes. Not built: ACARS/VDL2 outlines (held by Stuart),
the outline on the setup page (it is on the admin page and the app's server screen).
