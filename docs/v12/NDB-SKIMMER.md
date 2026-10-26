# NDB (V12): every aeronautical beacon on the LF band, decoded at once, idents by consensus

Stuart, 2026-10-26: an NDB skimmer — every carrier in a wide LF slice, its tone AM-detected, the ident CW-decoded and
VOTED across repeats ("never show noise letters as an ident"), matched against the published beacons for name, place,
flag, distance and bearing; harmonics flagged; shared frequencies ranked by ident and distance. Web first, with a map,
the gallery and background-ready decoding.

Branch `v12-ndb`. Engine: `android/app/src/main/cpp/decoders/ndb_skimmer.{h,cpp}`. Published list:
`scripts/gen-ndb-navaids.mjs` → `vibe_ndb_navaids.h` (generated), matching `vibe_ndb_db.h`. Service:
`vibe_ndb_service.h`. Gallery: `vibe_ndb_gallery.h`. Server: `local_sdr_shim.cpp` (NDB MODE beside ISM). Web:
`web/client/src/ndb.ts`, wiring in `main.ts`, `mobile.ts`, `spectrum.ts`. Tests: `vibeserver/test-ndb.cpp`,
`scripts/test-server-ndb.mjs`, `scripts/test-web-ndb.mjs`; bench `tools/ndb-bench`, `scripts/test-ndb.sh`.

Built from ICAO Annex 10 (the NDB, its ident: ≈ 7 WPM, 400 ± 25 or 1020 ± 50 Hz, at least three times in 30 s) and ITU-R
M.1677 (the Morse code — the CW skimmer's own table, `cw::morseText`). No other decoder's source was read.

## Architecture

**One filterbank, every carrier** (the CW skimmer's design, sized for NDBs): a Hann-windowed PFFFT per hop over the
whole slice, ≈ 23 Hz bins, hop = half a window (21 ms; a 7 WPM dot is eight hops). Power and its 1.5 s average in one
SIMD pass (NEON ARMv7-safe / SSE2 / scalar via `VIBE_FORCE_SCALAR`). Nothing is queued: sixty beacons cost the same
transform as one.

- **Carriers**: a local peak of the long average ≥ 3 dB over the floor (30th percentile per 64-bin block, calibrated to
  the mean: 1.00 on noise), seen two seconds running; not a window sidelobe of a stronger carrier, not a data signal's
  hump (DGPS MSK at 283.5–325 kHz — a carrier must be a LINE), not the radio's DC or its IQ image. A peak shaped like a
  stronger carrier's mirrored sidebands must stay above the threshold 15 s without swinging 6 dB (a keyed sideband
  falls in every pause; a real carrier on the 0.5 kHz raster does not).
- **The tone**: the sideband pair in the ICAO windows whose weaker side stands highest over ≈ 10 s (never chosen in the
  first eight seconds: noise reaches 1.15 somewhere on one look). **AM detection in the filterbank, coherently**:
  U·C̃* + L*·C̃ (both sidebands turned by the carrier's own phase from the same frame) — incoherently when another beacon's
  keyed sideband lands on this carrier's bin. A second keyed tone in the other window is a second beacon on the same
  frequency ("four on freq"): a child with its own vote. A1A (keyed carrier) is read from the carrier.
- **The decode** (once a second, on 48 s of envelope): a dot-long moving average (the matched filter); keyed only if
  the marks stand five noise spreads clear of the KNOWN noise level (the floor — a two-means split of noise always finds
  two clusters); threshold half-way in power; runs debounced at half a dot; the dot from the marks' two clusters (4–16
  WPM, learnt only from clear signals); one word per repeat; a word is a ballot only if every element is within 50 % of
  machine timing.
- **The consensus** (`IdentVote`): three identical clean readings (more for readings that say little — "EE" needs six;
  one letter, all-one-letter or all-single-element readings never), or a 70 % letter-by-letter majority of four that
  was itself read whole twice — **or the FOLD**: the repeat period found by autocorrelation and pinned by epoch folding,
  96 s of envelope averaged in step and read in **disjoint groups** (three groups of ≥ 3 repeats, or two) that must
  agree, or two disjoint whole-history windows that agree. A clean fold VETOES a vote of single repeats that disagrees
  (one repeat at a time a weak beacon is misread the SAME way — "OX" as "OTA"); three agreeing groups correct an ident.
- **The list** (`vibe_ndb_db.h`): ✓ match (ident published within ±600 Hz — the nearest if published twice), ≈ one
  letter off, italic candidate (not read yet: the published beacons on the frequency, nearest first — a guess, labelled),
  unlisted (read, not published: shown alone). **Harmonic**: a carrier at twice another's frequency (± 30 Hz).
- **RAM ↔ sensitivity, stated**: 96 s × 2 floats a hop per carrier ≈ 36 KB; a hundred carriers ≈ 3.6 MB. 48 s would halve
  it and lose the fold's deep path (−13 dB 400 Hz drops from 5-6/8 to 2/8). The published list is ≈ 250 KB of .rodata,
  paged in only when NDB runs.

## The mode (server contract)

`{"type":"ndb","on":1|0}` (gated by ask-before-tuning). The server saves the listener's place, puts the hardware centre
on the band (`vibendb::plan`: as much of 190–535 kHz as the radio reaches and its rate holds, centred on Europe's
255–526.5 when it cannot hold it all; x.25/x.75 kHz, between raster beacons) and gives the radio back exactly as found.
**A listening mode**: unlike ISM and ADS-B the VFO, mode and audio stay the listener's inside the capture (a `tune` there
moves only the VFO — tap a beacon, hear it; outside it is refused with the reason). Zoom, reset, the IF filter and the
rate are the mode's. One whole-radio mode at a time. No rate change, ever.

- `vibeserver.json`: `ndb` true / false + `ndbWhy` / **absent** where the radio reaches no LF — an RTL only with direct
  sampling on or automatic (then 0–14.4 MHz); HF+ and RSPs by their ranges; HackRF, Airspy R2, Icom: no control at all.
  `ndbLo`/`ndbHi`; `"ndb"` in `modes`; the owner's block list (Setup → Modes and decoders: "NDB beacons").
- `{"type":"ndb",…}` once a second and at once to a joiner: centre, lo/hi, carriers, identified, published/heardIn
  (coverage), far, beacons (hz, snr, tone, a1a, wpm, ident, votes, read, db {k,ident,name,cc,lat,lon,km,brg}, alt, also,
  harm, copy), log (the heard log), copy (live copy, sequence-numbered). `ndb_off`, `ndb_error {why}`.
- `{"type":"ndb_find","q","limit"}` → `ndb_found` (published beacons nearest first; in or out of the mode).
  `{"type":"ndb_stats"}` → per hour (heard, furthest), each beacon's SNR every 5 min (in memory, 7 days).
- Health pill: "NDB skimmer" drops (WideTap's drop-oldest queue). Clean shutdown writes the gallery's day.

**Gallery**: per UTC day, the heard log (a card rewritten at most once a minute, read back after a restart) and a MAP
(PNG, azimuthal equidistant about the receiver, range rings, the map pack's country outlines when installed, the idents).
Gallery chip NDB. The `DayLog` takes `origin` "background"; **a background-decoding NDB profile is not wired yet**.

**Web** (`ndb.ts`): NDB in the Mode sheet beside ISM; the box: EXIT NDB · FIND NDB · STATS · IDENTIFIED ONLY; tiles
CARRIERS / IDENTIFIED / FURTHEST / COVERAGE; a spectrum strip with idents over carriers; tabs BEACONS (kHz · ident ✓/≈/
italic? · flag + station · distance + bearing · SNR · tone · WPM · copy; a row tunes to it), MAP (MapLibre: rings, links,
heard in the last 10 min…7 days/all, unheard published beacons, autofit; without WebGL 2 a note), LIVE COPY, HEARD LOG;
Find NDB (nearest first, ident/name/kHz); stats (per hour, furthest per hour, by hour of day, per-beacon SNR over time,
CSV each). 390 px: fits with a 16 px gutter, nothing sideways.

## Measured (Apple M5 Pro)

**One beacon, 4 min, 8 draws a cell**, SNR = carrier / noise in 2.5 kHz (`ndb_bench accuracy`) — confirmed right /
wrong, mean time to confirm:

| SNR | 1020 Hz A2A | 400 Hz A2A | A1A |
|---|---|---|---|
| +10 dB | 8/8, 0, 41 s | 8/8, 0, 40 s | 8/8, 0, 40 s |
| 0 dB | 8/8, 0, 40 s | 8/8, 0, 40 s | 8/8, 0, 41 s |
| −5 dB | 8/8, 0, 43 s | 8/8, 0, 42 s | 8/8, 0, 46 s |
| −10 dB | 6/8, 0, 100 s | 8/8, 0, 83 s | 8/8, 0, 49 s |
| −13 dB | 2/8, 0, 211 s | 6/8, 0, 144 s | 8/8, 0, 65 s |
| −16 dB | 0/8 | 0/8 | 1/8, 0 |

5–13 WPM at −5 dB: 7–8/8, no wrong ident. **Noise: 30 min → 0 carriers, 0 idents.** **Forty beacons in 192 kHz**
(−12…+28 dB, 0.5 kHz neighbours, a shared frequency, both tones, A1A, double idents): 37–38 identified, 2–3 missed, 1
wrong — LBA read correctly but ALSO on its own sideband, where the test places another beacon's keyed sideband on
LBA's carrier (1 kHz raster + 1035 Hz tone); a known limit.

**Real**: Stuart's UberSDR, 256–448 kHz I/Q at 192 kHz, 10 min, 2026-10-26 07:29 UTC (daylight),
`~/VibeSDR-test-audio/ndb/`. **11 idents, the first six in 17–36 s**: HEN 433.5 (17 s), EME 353.5 (24 s), EMW 393.0
(26 s), WOD 352.0 (28 s), BHX 406.0 (33 s), OX 367.5 (36 s), CHT 277.0 (45 s), EPM 316.0 (130 s), CAM 332.5 (150 s),
SND 362.5 (281 s), MRV 404.0 (303 s). The UberSDR NDB addon's list for the same window held four (WOD, EME, EMW, BHX),
re-identifying WOD at 29 s, EMW at 519 s, EME at 601 s and BHX not at all. Our eleven: nine are published UK beacons at
their frequencies (EME, EMW and SND are real UK beacons OurAirports lacks — shown "unlisted"); **MRV 404.0 is
unverified** (in no list we have; the only candidate for a false ident). DGPS humps at 284–291 kHz: no ident since the
narrowness test. 43 carriers.

**CPU**, one M5 core: real 192 kHz recording 0.43 %; synthetic busy slices (`ndb_bench cpu`) 192 / 384 / 600 kHz:
0.12 / 0.26 / 0.42 % NEON, 0.19 / 0.41 / 0.71 % scalar (≈ 1.6× from the SIMD loops + PFFFT SIMD). Plus the WideTap
channeliser on the raw capture (16384-point FFT per ¾ block — at 2.4 MS/s the CW skimmer tap's documented price).
**Pi 2 (Cortex-A7) is an ESTIMATE, not measured**: 20–30× an M5 core → ≈ 8–13 % of one core for a 384 kHz HF+ slice,
plus the channeliser at 2.4 MS/s on an RTL. ARMv7 NEON was not cross-compiled here (no armhf sysroot on this Mac);
x86-64 SSE2 syntax-checks clean.

## Tests

- `vibeserver/test-ndb.cpp` (run-tests): 38 checks — the vote's rules, one beacon on both tones and A1A at +10/−5 dB,
  −10 dB by the fold, 5/12 WPM, 5 min of noise, the floor's calibration, the published list (Woodley 93 km 181°, ≈,
  candidates, EME never mismatched, Find NDB), the service (report, both named, stats, Find marks heard, a harmonic), the
  gallery (card, map PNG, restart). `--write-clip` makes the e2e clip.
- `scripts/test-server-ndb.mjs`: 20 checks on the real server — an RTL without direct sampling offers no NDB; with it,
  the capability, the band, WOD and CAM identified and named (38 s), a tap moving only the VFO, a tune outside refused,
  Find, stats, the receiver given back exactly.
- `scripts/test-web-ndb.mjs`: 21 checks in headless Edge (`--mute-audio`), desktop and 390 px.

## Not done / owed

- **The app** (React Native): no NDB yet — web only. When it lands: the Mode sheet entry, the box, and the About V12 note
  + tour copy (not changed now: the app has no NDB, and nothing in either tour misdescribes it).
- **Background decoding profile** for NDB (the gallery's `DayLog` takes origin "background"; the setup page and
  `vibe_bgdecode` have no NDB profile).
- The waterfall in NDB shows the whole capture (on an RTL at 2.4 MS/s the band is a sixth of it); a view narrowed to
  the band would read better.
- Weak beacons below −13 dB (2.5 kHz); a strong neighbour's keyed sideband sitting on a weak carrier.
- Stats are in memory only (reset on restart; the gallery keeps the days).
- Real Pi 2 timing; real RTL direct-sampling and RSP captures (only an UberSDR RX888 recording was available).
