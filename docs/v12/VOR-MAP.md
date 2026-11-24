# The VOR / ILS map (V12, beta93)

Branch `vor-map` (off `v12-experimental` ca0a713a5), 2026-11-24. Stuart: *VOR/ILS shares no spectrum with other modes, so it
is a map-based, whole-radio mode treated the same as NDB.* The receiver sweeps the whole navaid band, 108–117.95 MHz, a
window at a time, decodes **every** 50 kHz channel of the window at once, and draws what it hears on a map: each station at
its published position with its ident, a VOR's radial as a line towards the receiver, and the receiver's FIX where radials
from different stations cross. The single-channel VOR / ILS decoder (docs/v12/VOR-ILS.md) is unchanged.

**Nothing has been heard on air with this mode yet.** Every figure below is from our own transmitter (`nav_tx.h`
`WideSynth`, `fake-rtl-tcp.mjs --vor`). The on-air check is **DTY Daventry 116.40 MHz** from IO92 — Stuart's Pi 500 read
radials ~054° FROM it with the single-channel decoder (VOR-ILS.md "RESUME HERE").

## Files

| | |
|---|---|
| `android/app/src/main/cpp/vibe_vormap_service.h` | the plan, the sweep clock, the decoder thread, the workers, the beacons, the report |
| `decoders/nav_rx.h` | the receiver, one per channel (unchanged) |
| `decoders/nav_tx.h` `WideSynth` | many navaids streamed into one wide capture (tests, the bench row) |
| `local_sdr_shim.cpp` | the mode (`vorEnter` / `vorLeave` / `vorTick` / `vorControl`), `vsVormapReach` / `vsVormapWhyNot`, every whole-radio rule |
| `vibe_radio_caps.h`, `vibe_setup_page.h`, `src/screens/ServerModeScreen.tsx` | the owner's switch `vormap` |
| `vibe_benchmark_more.h` (`dec_vormap`), `vibe_benchmark_hw.h` (`Need::Vormap`) | the benchmark row that sets the switch's default |
| `src/services/vorMap.ts` | the clients' half: report, list, map items, the FIX and its words (one rule, two readers) |
| `web/client/src/vormap.ts` + `main.ts`, `mobile.ts`, `spectrum.ts` | the web page (a TrackView) |
| `src/components/MapOverlay.tsx` (`'vormap'`), `ModeSelector.tsx`, `SDRScreen.tsx`, `VibeServerClient.ts` | the app |

## The channel plan

ICAO Annex 10 Vol. I, `nav_rx.h channelKind`: **108.00–111.95 MHz** — even tenths (108.00, .05, .20, .25 …) VOR, odd tenths
(108.10, .15, .30, .35 …) ILS localisers; **112.00–117.95 MHz** VOR only. A 50 kHz grid: 200 channels (8.33 kHz is voice only
and does not apply).

## The sweep

- **Windows** (`vibevormap::plan`): as many channels as fit in 90 % of the RUNNING rate (never raised — ADS-B's rule), each
  channel ±12.5 kHz inside it, the windows as even as the count allows, each centred **between** two channels (an odd count is
  moved half a channel) so the radio's DC remnant lands in no channel.

  | rate | windows | channels each | a sweep (30 s dwell) |
  |---|---|---|---|
  | RTL 2.4 MS/s | 5 | 40 | 2.5 min |
  | RTL 2.048 MS/s | 6 | 33–34 | 3 min |
  | 3.2 MS/s | 4 | 50 | 2 min |
  | RSP 8 / 10 MS/s | 2 | 100 | 1 min |
  | 20 MS/s | 1 | 200 | (continuous) |
  | under 1 MS/s | — not offered (more than 12 windows) | | |

- **Dwell** `kDwellSec` = **30 s** (a named constant): the bearing (30 Hz + 9960 Hz) is held after 3 agreeing seconds; the ident
  (1020 Hz) comes every 10–15 s, so 30 s reads it twice. `kSettleSec` = 0.3 s of samples are thrown away after each retune.
  The dwell is counted in **samples fed**, on the DSP thread, so the window changes on time even when the decoder lags.
- **The hop** is on the DSP thread (`vorTick`, under `modeMtx`, as `flushPendingDongle`): the hardware tuned, the listeners'
  views retuned, a config sent so every waterfall's scale follows.
- **Every channel at once**: the aviation datalink's front end (`vibe_avi_front.h` `MultiChannel` — half-bands, ONE forward FFT
  a block, a slice per channel; NEON / SSE2 / scalar) and its workers (`vibe_avi_workers.h` — up to 4 threads at decoder
  priority, channels dealt round-robin). Each channel runs `nav::NavRx` (its hot loops NEON / SSE2 with bit-exact scalar
  twins). The channel rate is ≥ 30 kHz (37.5 kHz at 2.4 MS/s, 31.25 kHz at 8 MS/s): the slice is flat to a quarter of it and
  the subcarrier's 10.44 kHz edge sits on the gentle top of the roll-off — test-vormap reads every radial within 0.1°.
- **Threads** (Network > Audio > Spectrum > Decoders): `feed()` copies into a quarter-second queue that drops the OLDEST block
  and counts it; the decoder thread and the workers run at decoder priority. ★ The workers' rings **wait** (lossless) and only
  the queue drops: on the Mac server with the fake radio a ring that overwrote lost ~450 blocks a second to worker wake-up lag
  and no bearing held anywhere; with waiting rings, 0 dropped.
- **Memory** (the 256–512 MB boxes): the queue (¼ s: 2.4 MB at 2.4 MS/s, 16 MB at 8 MS/s), the rings (≤ 2 MB of bins a
  worker), one NavRx per channel of the WINDOW only, 200 small beacon records.

## A name is earned on air

A beacon is named only by an ident read from it (`vibe_nav_run.h`'s rule): the NDB skimmer's consensus (three clean identical
readings …) **or** a reading that IS a station published on that channel near the receiver, read twice. A channel whose ident
has not been read is "unidentified" in the list and has no place on the map — never a guess from the list. Votes and readings
persist across sweeps (the vote keeps 5 minutes), so an unlisted station whose ident needs three readings is named on its
second visit if not its first. Localisers: the open data lists 21 (by airport ICAO code, not their real I-idents), so most
read "not in the open data".

**Fresh / faded**: heard within the last sweep (one full pass, plus one dwell — a station heard early in one visit and late in
the next stays bright); otherwise faded, still listed.

## The server contract

- `{"type":"vormap","on":1|0}` — gated by ask before tuning (`kGated`), the scan lock, the only mode; one whole-radio mode at a
  time (the others leave through their own doors); tune / mode / passband / zoom / rate / IF filter ignored with a log line;
  no audio (spectrum-only), the waterfall at 1 fps; the gain left as the listener had it (steady carriers, NDB's rule); an RTL in
  manual direct sampling has it switched off and put back on leaving. Idle leave + arrival resume as every whole-radio mode.
- `vibeserver.json`: `vormap` true / false + `vormapWhy` / **absent** where the radio cannot tune 108–118 MHz (an HF-only radio;
  the Airspy HF+ for AIS's reason — and ≤ 912 kS/s could not sweep it; an Icom); `vormapWins`, `vormapDwell`; `"vormap"` in
  `modes` (LAST — an app before beta93 kept only the first 40 names of that list; it now keeps 96).
- `{"type":"vormap", now, rate, window, windows, wlo, whi, centre, sweep, inWin, dwell, progress, pos, wins:[[lo,hi]…],
  beacons:[{hz, kind:"vor"|"loc", win, ident, reading, votes, held, level, first, last, fresh, wpm?, radial?, sigma?, trueBrg?,
  ddm?, sdm?, station?:{ident, name, cc, lat, lon, var, dme, km10?, brg?}}], heard, identified, dropped, credit}` once a second
  and at once to a joiner; `vormap_off`; `vormap_error {why}`. Distances to 10 km (`vibe_nav_run.h`'s precision for listeners).

## The map (both clients)

`src/services/vorMap.ts`: the list lowest frequency first (ident or "unidentified", MHz, VOR / LOC, the station, the radial
"054° FROM" or the localiser's DDM and side, level, last heard); every NAMED station at its published position; a VOR's radial
from the station along its **true** bearing (magnetic + the station's declination), 40 km past the receiver; bright this
sweep, faded (dashed) older.

**The FIX** (`vorFix`): one radial per station, heard this sweep; every pair crossed with `radialFix` (great circles, ahead of
both stations, within 600 km); each crossing's uncertainty from the two radials' spreads (the reading's σ and ±1.5° for the
station's own alignment) at their distances, divided by the sine of the crossing angle; several crossings → their weighted
centre and the larger of the formal error and their spread. Said in words: *"Your position from the DTY and HON radials:
within about 2.1 km (they cross at 58°)."*; a crossing under 20° is called poor along the radials' line; far stations
(> 25 km of uncertainty) are said to be far. Drawn as a crosshair with its circle.

- **Web**: a TrackView (EXIT VOR MAP, NAVAIDS list, KEY), the key under TUNES THE RADIO ITSELF in the decoder panel and in the
  mode menu, beside NDB; NDB's holds; the only mode; ask before tuning; a pad / TV Back leaves it, as NDB.
- **App**: VOR MAP beside NDB under TUNES THE RADIO ITSELF; MapOverlay `'vormap'` on the shared track layer (aero basemap,
  NM scale, the list, the key); EXIT VOR MAP; the audio session let go; none on the Apple TV (no maps there), as NDB;
  Android TV through the track layer's own focus handling, as SONDES.

## Measured (Apple M5 Pro)

| | |
|---|---|
| bench `dec_vormap` (one 40-channel window @ 2.4 MS/s, 5 VORs + a localiser, real time, workers) | **4.07 % of one core**, 6 heard |
| test-vormap, transmitter's share taken out | 40 channels @ 2.4 MS/s **4.8–6.2 %**; 100 channels @ 8 MS/s **12.3 %** |
| single-channel `dec_vor` (37.5 kHz) | 0.04 % |

**Pi 2 (Cortex-A7) is an ESTIMATE, not measured**: 20–30× an M5 core → roughly a whole Cortex-A7 core for an RTL window,
spread over its four. The benchmark's row on that box sets the switch's default (off when red), never hides it.

## Tests

| Suite | What | Result |
|---|---|---|
| `test-vormap` | the plan (RTL 5, 2.048 → 6, 3.2 → 4, RSP 8 / 10 → 2, 20 → 1, under 1 MS/s none; one window per channel; centred between channels); one window with 4 VORs (two on ADJACENT channels) and a localiser at 2.4 and 8 MS/s: radials ±2° (worst 0.1°), idents read, the listed one named, the DDM, nothing on the 35 / 95 empty channels; SIMD = scalar; the sweep 0→1→2→3→4→0, a beacon only in window 3 found with its radial and station; fresh → faded after a silent sweep, nothing false on 200 channels | **46 / 0** |
| `test-server-vormap` | the real server, fake rtl_tcp `--vor` (HON 113.65 in window 3, DTY 116.40 in window 5) at 2.4 MS/s: a radio held off VHF offers none; vormap / 5 windows / modes; entering, window 1, the step to window 2 at the dwell, HON and DTY found in their windows (radials ±2°, Honiley / Daventry, DTY's true bearing and ~20 km), every window visited, nothing else heard, tune ignored, 1 fps, leaving gives the receiver back exactly; ask before tuning refuses a second listener (logged); the only mode lands a listener and refuses their "off" | **32 / 0** |
| `test_vormap_app` | sanitise, the list, the status, the map items (named only; the radial through the receiver; faded), the FIX (two radials at the receiver within 0.5 km, three a cluster, 1° error inside its claim, one per station, this sweep only, no fix from an unnamed radial, a shallow crossing said), both clients' wiring, the setup screens | **45 / 0** |
| `test-radio-caps` | `vormap` on RTL / R2 / HackRF / USRP / RSP, never the HF+, an Icom, an RTL behind a 65 MHz up-converter | **262 / 0** |
| `test-bench-hw`, `test-bench-decoders` | `dec_vormap` runs only where a radio reaches the band; it decodes, has a defaults rule | 64 / 0; `dec_vormap` pass (`dec_rade` fails here: RADE is not built into this Mac's test tree — not this change) |
| also | test-only-mode-clients 96, test_ask_before_tuning 34, test_decoder_bands, test_navaids_app 42, test_ndb_app 204, test_sessionHoldNotice 34, test-setup-radio-caps 71, test-local-bench 37, test-bench-defaults, check-setup-page, test-vor 35 | pass |

## Owed

- **On air**: DTY 116.40 from IO92 (expect ~052° magnetic FROM, 48° true; the Pi 500 read 054–057°); a second station for a
  real FIX (HON 113.65, BNN 113.75, CPT 114.35 …); a localiser if one is heard. Watch the drop count on the Pi 2 / Pi 500.
- **Real Pi timing** of `dec_vormap`; the RSP's 8 MS/s window on a Pi-class box.
- A device run of the app's map (placement on an SE and a foldable) and the Android TV focus path.
- Gallery cards / a heard log (NDB has them; not asked for here).
- `build_ios.sh` for the iOS DSP library (the shim changed) — the release step, not done on this branch.
