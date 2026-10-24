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

**Category → icon** (DO-260B emitter category):
A1 light (< 7 t: light prop) · A2 small (7–34 t: regional/bizjet) · A3 large (34–136 t: narrow-body jet) ·
A4 high-vortex large (B757) · A5 heavy (> 136 t: wide-body/jumbo) · A6 high performance (> 5 g, > 400 kt: fighter) ·
A7 rotorcraft (helicopter) · B1 glider · B2 lighter-than-air (balloon) · B3 parachutist · B4 ultralight · B6 UAV ·
B7 space vehicle · C1 emergency surface vehicle · C2 service surface vehicle · C3 obstacle · A0/none = generic jet.

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
