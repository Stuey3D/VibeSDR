# Aviation datalink (V12): ACARS, VDL Mode 2, HFDL — our own decoders

Branch `v12-aviation` (from `v12-experimental` 460bb2f12). Written 2026-10-31 / 11-01.

| Mode | Code | Test | Status |
|---|---|---|---|
| **VHF ACARS** (AM-MSK 2400 bit/s) | `vibe_acars.h` on `vibe_avi_front.h` | `vibeserver/test-acars.cpp` | **Decoder works on real signals.** Wired: the Aircraft datalink mode's `acars` band. |
| **VDL Mode 2** (D8PSK 10.5 ksym/s) | `vibe_vdl2.h`, `vibe_vdl2_fec.h` on `vibe_avi_front.h` | `vibeserver/test-vdl2.cpp` | **Decoder works on real signals.** Wired: the mode's `vdl2` band. |
| **HFDL** (PSK 1800 Bd, HF) | `vibe_hfdl.h`, `vibe_hfdl_run.h` (a listener's USB dial) | `vibeserver/test-hfdl.cpp` | **Decoder works on real signals** (BPSK modes verified; QPSK/8PSK only self-consistent). Wired: decoder ext `hfdl`. |
| Service, table, JSON, bands | `vibe_avi_service.h`, `vibe_avi_mode.h` | `vibeserver/test-avi-service.cpp` | Wired into `local_sdr_shim.cpp` (2026-11-16) — see RESUME HERE. |
| ACARS applications (positions, OOOI, ARINC 622) | `vibe_acars_apps.h` | `vibeserver/test-acars-apps.cpp` | 2026-11-16, branch `v12-acars-apps` — §Applications. **Not yet seen on air.** |
| Transmitters (tests + benchmark) | `vibe_avi_tx.h` | — | ACARS, VDL2, HFDL bursts; `test-vdl2 --write-clip` makes the server e2e's clip. |
| **Shared aircraft table** (every radio process) | `vibe_aircraft_share.h` | `vibeserver/test-aircraft-share.cpp`, `scripts/test-server-avmap.mjs` | 2026-11-16, branch `v12-avmap` — §The shared table. |
| **AVIATION MAP** (web + app) | `src/services/aviationMap.ts` | `scripts/test_aviation_map.ts`, `scripts/test-web-avmap.mjs` | 2026-11-16 — §The aviation map. |

All headers are in `android/app/src/main/cpp/`. The tests are header-only plus vibedsp's `fft.cpp`, `channelizer.cpp`,
`ddc.cpp` (and PFFFT/KissFFT); they are `vibeserver/CMakeLists.txt` targets (ctest acars / vdl2 / hfdl / avi-service) and
in `scripts/run-tests.sh`.

## The short version (results)

| | Recording | What we decoded | Reference |
|---|---|---|---|
| ACARS | Pi 500 V4L, 2.4 MS/s, 483 s, 131.525/.725/.825 | **32 blocks** (6 repaired), 11 synced bursts not decoded | none exists (no other ACARS decoder on the LAN) |
| ACARS | Pi 2 V4, 2.048 MS/s, 302 s | **17 blocks** (1 repaired), 3 synced bursts not decoded | none |
| VDL2 | Pi 500 V4L, 2.4 MS/s, 482 s, 12 channels 136.700–136.975 | **387 AVLC frames** (94 ACARS), **34 aircraft** | none |
| VDL2 | Pi 2 V4, 2.048 MS/s, 302 s | **165 frames** (23 ACARS), **23 aircraft** | none |
| HFDL | UberSDR RX888 iq192, 8931 kHz window, 600 s | **12 of 16** reference messages (+ ones the reference did not list) | UberSDR's HFDL add-on |
| HFDL | UberSDR RX888 iq192, 6598 kHz window, 600 s | **23 of 42** reference messages | UberSDR's HFDL add-on |
| **2026-11-16 (daytime, §On air 2026-11-16)** | | | |
| ACARS | Pi 500 V4L (:48002, serial 4), 2.4 MS/s, 476 s, 131.675 centre | **91 blocks** (8 repaired), 8 synced bursts not decoded | none |
| VDL2 | Pi 500 V4L, 2.4 MS/s, 480 s, 136.8625 centre | **1028 AVLC frames** (233 ACARS), **78 aircraft** | none |
| HFDL | UberSDR RX888 iq192, 11245 kHz window (11184 + 11306), 600 s | **23 of 96** reference messages (+ 39 the feed does not list) | UberSDR's HFDL add-on |
| HFDL | UberSDR RX888 iq192, 15000 kHz window (15025), 600 s | **23 of 127** reference messages (+ 57 the feed does not list) | UberSDR's HFDL add-on |

- **Zero false decodes on noise:** ACARS 10 min of noise on 3 channels: 0 blocks; VDL2 10 min on 12 channels: 0 frames;
  HFDL 20 min: 0 messages. Every ACARS repair passes a BCS check *and* a plausibility check, and is unique
  (test-acars 2b: 0 of 10 000 parity-blind corruptions turned into a wrong block).
- The VDL2 aircraft addresses match their registrations' countries (4CA97B EI-EST, 4070E8 G-JZHU, 3C6548 D-AIJH,
  781361 B-1243), and VDL2 found aircraft on 136.900 (A9C-FA, Gulf Air) as well as the channels the README listed.
- HFDL: the reference recordings hold only 300 and 600 bit/s bursts. QPSK (1200) and 8PSK (1800) round-trip through
  our own transmitter, but the bit labelling of those two is **unverified on air**.
- **CPU, Pi 2** — see §Cost.

## Licences and sources (the house rule)

- **ACARS:** the free Wavecom decoder documentation (modulation, block layout) and libacars (MIT; the BCS flavour, the
  ACARS-over-AVLC framing). The bit convention and BCS were then **measured on our own capture**: a 1200 Hz bit toggles
  the data bit, a 2400 Hz bit keeps it; BCS = CRC-16 reflected 0x1021, preset 0 — a real block leaves zero. acarsdec
  (either fork) was not read.
- **VDL2:** ETSI EN 301 841-1/-2 (free) and Jamal & Matolak (arXiv:2005.11456: the unique word). The facts only ICAO Doc
  9776 holds (paid) were **read in dumpvdl2 (GPL-3.0-or-later, read for inspiration, no code copied)**: the scrambler
  (x¹⁵+x+1, 0x6959), the header code's parity rows, RS field 0x187 / first root α¹²⁰, the shortened parity rule, the
  interleave, the Gray table and the AVLC address packing. vdlm2dec was not read.
- **HFDL:** ITU-R M.1458 (free) for the waveform. The sequences (A, M1 and its eight shifts, M2, T), the frame of
  30 data + 15 probe symbols, the 120-symbol scrambler, the 40-row interleaver and its column steps, the K=7 code and the
  PDU layouts were **read in dumphfdl (GPL-3.0-or-later, read only)**. Ground-station names are the public HFDL list.
- Clones used for reading live outside git: `~/VibeSDR-test-audio/aviation/refs-readonly/` (dumpvdl2, dumphfdl,
  libacars). Nothing from them is in the tree. No ARINC/ICAO document was opened.
- ⚖ **Message bodies** (ACARS free text) are behind the owner's opt-in (DECODER-RESEARCH.md §4.3); flight IDs,
  registrations, positions and labels are shown by default. `vibe_avi_service.h` enforces it.

## Architecture

### The shared front end (`vibe_avi_front.h`, ACARS and VDL2)

One chain for all channels, flat in the channel count:

1. a cascade of **half-band decimators** (vibedsp `HalfbandDecimator`: NEON on both ARMs, SSE2 on x86, scalar
   elsewhere) down to the narrowest rate that holds the channel list (2.4 MS/s → 600 kS/s for ACARS's ±162 kHz, → 300
   kS/s for VDL2's ±137 kHz);
2. **one forward FFT a block** (vibedsp `Channelizer`, overlap-save, PFFFT) — bins of ≤ 600 Hz;
3. each channel = a slice of those bins inverse-transformed at its own rate (32 bins = 18.75 kS/s for ACARS, 64 =
   37.5 kS/s for VDL2) plus a tiny NCO for the sub-bin offset.

The slice is the channel filter: flat over half its rate, raised-cosine edges, **nothing outside** — a strong
neighbour 25 kHz away cannot leak in (the integrate-and-dump front ends' first sinc side lobe is only ~13 dB down: the
"same message on two channels" fault the survey describes). A fourth channel costs one 32- or 64-point FFT a block.

### ACARS (`vibe_acars.h`)

- **AM envelope** (|z|), the carrier level taken out and used as AGC, mixed down by 1800 Hz — one SIMD kernel
  (`envMix`: NEON / SSE2 / scalar, checked against double precision in test-acars §3).
- **Coherent MSK as BPSK:** the MSK matched pulse (a half cosine two bits long — its first null falls on the AM
  carrier), sampled at each bit instant and turned back k quarter turns; what is left is ±e^{jθ} whose sign IS the data
  bit (data = the XOR of the tones, exactly the measured convention). θ comes data-free from the squared samples.
  No error propagation (a tone detector would integrate its decisions: one wrong tone inverts every later bit).
- **Four bit clocks** a quarter bit apart, each with its own phase and deframer; duplicates within a second dropped.
- **Sync**: the 40 bits '+' '*' SYN SYN SOH with ≤ 3 errors, either polarity.
- **Two-sided θ** at a candidate end (±16 bits around each bit) — +1 block on the Pi 500.
- **Repair: a syndrome Chase.** The BCS is linear, so each candidate flip's residue change is computed once; up to 4
  parity-failing characters × their 2 weakest bits × {none, one of the 2 weakest BCS bits}. Accepted only when
  **exactly one** combination clears the residue **and** the block is plausible (registration characters, printable
  text, a block id). 9798/10000 weak-bit errors repaired, 0/10000 false repairs.

### VDL2 (`vibe_vdl2.h`, `vibe_vdl2_fec.h`)

- The **receive low-pass is applied in the channel slicer** (frequency domain, bins of 293 Hz): the magnitude of a
  Blackman low-pass at 4.8 kHz + 4.5 kHz transition, sampled on the slice's bins with a linear phase that keeps its
  tails inside overlap-save's discarded head. Then each channel goes to **4 samples a symbol** (42 kS/s) by an exact
  **rational polyphase resampler** (37.5 → 42 kS/s = 28/25, 32 → 42 = 21/16; a 12-tap Kaiser sinc per phase, NEON/SSE2).
  (Until 2026-11-01: Catmull–Rom + a 58-tap Blackman FIR at 42 kS/s — the FIR was 22 % of VDL2's Pi 2 instructions.)
  ★ Measured: the transmit pulse is a FULL raised cosine — an RRC receive filter left ±0.5-step residuals on a 33 dB
  burst; the RC itself is worse; no filter lets in 3.6× the symbol rate of noise. The sweep is in §Results.
- A **power gate**: the sync search runs only while the channel's power is up (VDL2 is CSMA, mostly idle).
- **Sync**: the unique word's 16 phase steps correlated against y[n]·y*[n−4]; |D|/Σ|p| with **all sixteen products of
  a similar size** (one dominant product at a burst's leading edge gave Q≈1 — 40 000 false syncs in the first run);
  the best peak within 1.5 symbols wins; its angle IS the carrier's frequency error.
- **Coherent D8PSK**: the carrier phase from the unique word's 16 known points, a decision-directed phase/frequency
  loop, data step = point − previous point. +30 % frames over differential detection.
- Header: (25,20) code with our own minimum-weight syndrome table; RS(255,249) errors-and-erasures (our own BM/Forney;
  the shortened block's missing parity as erasures; then the weakest 2 or 4 bytes erased if errors alone fail, always
  leaving ≥ 2 checks); our own interleave walk; HDLC; the FCS judges every frame.
- AVLC: addresses (type, 24-bit address, air/ground), I/S/U frames, U names, **ACARS over AVLC** (FF FF 01, then the
  VHF block after SOH) through the ACARS parser with its BCS.

### HFDL (`vibe_hfdl.h`)

- Per channel at **4 samples a symbol** (7200 S/s), a low-pass to ±1.3 kHz.
- **Detection: the A1+A2 pair** by differential correlation — Q at A2's end + Q 127 symbols before, held 64 symbols
  for the true peak. Blind to the carrier's frequency; the pair rejects level steps and impulses (both 6535 and 6619
  kHz used to "detect" the same instant) and the strong burst's partial overlap with the pre-key (the synthetic test
  failed above +4 dB until this).
- **The burst is buffered and worked as a block** (its length is known from M1): timing in ¼-sample steps by
  16-symbol coherence over A1+A2, the frequency from the differential correlator plus the segments' phase slope, M1
  (the mode) from eight candidates.
- A **T/2-spaced NLMS equaliser** (11 taps, μ 0.06, swept) with a decision-directed carrier loop, **trained twice** over
  the 531 known preamble symbols, decision-directed through the data and re-trained on every 15-symbol probe.
- **Soft values with each data segment's own noise** (the probes either side; a fading segment says so): +3 messages.
- Descramble (π turn), deinterleave, the rate-¼ pairs **added**, soft K=7 Viterbi (our own), PDUs: MPDU (header CRC),
  LPDU (own CRC), HFNPDU performance/frequency data (flight + position), logons (ICAO address), enveloped ACARS (SOH
  first — measured: `FF FF 01 32 C8 …`).

### Service (`vibe_avi_service.h`)

Its own thread fed by copy, a half-second queue that drops the OLDEST block and counts it (Network > Audio > Spectrum
> Decoders). One `Tracker` row per aircraft from all three decoders: VDL2 by ICAO address, ACARS by registration (a row
found by registration adopts the ICAO address when VDL2 or HFDL brings it), HFDL by ICAO (logons) or flight. JSON:
`{"msgs":N,"aircraft":[{icao, reg, flight, lat, lon, posAge, label, labelName, text?, level, n, age, via}]}` — `text`
only with the owner's opt-in.

## Applications (`vibe_acars_apps.h`, 2026-11-16, branch v12-acars-apps)

What a BCS-good message's text SAYS, for the few layouts that are published and checkable. Every ACARS message from all
three decoders (VHF ACARS, ACARS over VDL2, ACARS over HFDL) goes through it in the `Tracker` (`acars_`), after a bounded
multi-block (ETB) reassembler (32 messages × 8 blocks, 60 s).

| Layout | Labels | Check | Gives |
|---|---|---|---|
| **ARINC 622** envelope `/<gs>.<IMI><reg7><hex>` | A6 AA B6 BA, H1 after the sublabel (`#M1B`, `- #M1`) and MFI (`/B6 `) | its CRC-16 (0x1021, preset FFFF, residue 1D0F) | the IMI: **ADS-C** (`ADS`, `DIS`) or **CPDLC** (`AT1` `CR1` `CC1` `DR1`) |
| ADS-C downlink groups | inside ARINC 622 `ADS` | the envelope CRC; group lengths walked, an unknown group ends the walk | the first basic-shaped group (7 basic, 9 emergency, 10/18/19/20 events): lat/lon (21-bit), altitude (16-bit × 4 ft), seconds past the hour (15-bit × ⅛ s); accuracy 0 (navigation lost) is NOT shown; group 12 flight ID |
| FMS downlinks (ARINC 702-style) | H1 `#M1B…`, `/.POS…` | the trailing 4-hex-digit CRC (reflected 0x1021, preset/xor FFFF, nibbles reversed) — refused otherwise | IMI `POS` or IEI `PS`: position (deg, min, tenths), the reported HHMMSS and flight level after it; IEI `AF`: the airports |
| Label 16 | 16 | whole fixed part matched | `N 44.203,W 86.546,31965,…` / `N 28.177/W 96.055` decimal positions, altitude |
| Label 15 | 15 | `(2…(Z` frame | `(2N38448W 77216…` position; `OFF` + date + HHMM the take-off |
| Label 44 | 44 | whole fixed part | `POS0n` position + FL + airports; `OFF0n` / `ON0n` / `IN0n` position + airports + time |
| QP QQ QR QS | QP–QS | 4+4 capitals, a real HHMM | OUT / OFF / ON / IN, the airports (QQ's long form: + position) |
| H2 `02E` | H2 | every 32-character observation (position, HHMM, FL, temp, wind, 'G') | the airports and the LAST observation's position, time, FL. The weather itself is not decoded. |

- **CPDLC is named, never decoded** (`app: "CPDLC"`): its content is controller–pilot conversation. ADS-C uplinks
  (contract requests) and disconnects are named, no position.
- **Never a guessed field**: a near miss (a minute of 60, a lower-case airport, a wrong CRC, a bogus text) is counted as
  `unparsed`; so is any label with text and no layout here. test-acars-apps §5: 200 000 random texts across the position
  labels → 0 positions. **Label 20 is left out**: two published readings of its `N38160` disagree (decimal degrees vs
  degrees and minutes) — a real capture must settle it. QA–QF (OOOI on some airlines) have no published sample layout
  we could check: named only (as before).
- ⚖ **Privacy**: position, altitude, the fix time, flight, airports, the OOOI event are **flight data** — shown by default
  like the registration. Nothing else leaves the server: the message text (ADS-C hex, FMS text, free text) stays behind
  the owner's `avdata+text` opt-in on the owner's own socket; weather values, fuel, CPDLC content are not decoded at all.
- **JSON** (each row, when known): `alt` (ft), `posSrc` (`ADS-C` | `FMS` | `ACARS` | `HFDL`), `posTime` (AS REPORTED:
  `21:57:54`, or ADS-C's `:06:36` past the hour — never converted to a date we would have to guess), `from`, `to`,
  `oooi` (`out|off|on|in`), `oooiTime` (`20:30`), `app` (`ADS-C` | `CPDLC` | `OOOI` | `FMS` | `position`); and the report
  `apps: {pos, oooi, adsc, cpdlc, unparsed}`. A later message without a position keeps the last one (`posAge` ages it).
- **Clients**: `src/services/aviation.ts` (one copy) — `avFix`, `avRoute`, `avDetail`, the map's items/rows/card. The
  app's DATALINK box has **MAP** (MapOverlay `avdata`, the shared track layer, CLOSE MAP back to the box); the web's
  TrackView map already drew positions and now its rows carry them; the R36S list/card show the route, altitude, fix.
- **Sources** (read for facts, no code taken): libacars (MIT) and its PROG_GUIDE for the ARINC 622 envelope, its CRC
  and the ADS-C groups, with its published sample messages and their decoded positions; acars-decoder-typescript
  (airframesio, MIT) for the airline layouts, the FMS CRC and their sample messages. Both CRCs were verified on those
  samples before use. libacars is **not linked** (Stuart's call).
- **Test vectors**: the published samples (with their published answers), our own transmitter (`vibe_avi_tx.h`
  `adsCBasic`, `fmsPos`: 2000 random ADS-C reports round trip; an ADS-C report over our ACARS modem and receiver), and
  ONE frame from our own Pi 500 VDL2 capture (G-EZGJ's H2 `02E31GMMXEGGW…`, kept in test-vdl2 §4). ★ The capture
  `.u8.log` files in `~/VibeSDR-test-audio/aviation` hold the session's server messages only — **not** the decoded
  frames — so the real captures' messages were not available as parser input (the IQ is gone too).

## The shared table (`vibe_aircraft_share.h`, 2026-11-16, branch v12-avmap)

Stuart (2026-11-16): *share the aircraft table across radios — a server with 2 SDRs, one on ADS-B, one on ACARS/VDL2, must
not show duplicate aircraft.* Each radio is its own PROCESS (`vibeserver/main.cpp superviseRadios`, or systemd's
`vibeserver@<serial>`), each with its own tables (`vibe_adsb.h` Tracker, `vibe_avi_service.h` Tracker) — so the rows meet in
ONE small file every radio process of the machine reads and writes under `flock`, the ship store's pattern.

- **Where:** `<runtime dir>/aircraft-live.dat` — `handoffDir()` (`/run/vibeserver`, a tmpfs, on a Linux server; `/tmp/
  vibeserver-<uid>` without systemd; `VIBESERVER_RUNTIME_DIR` in the tests). ★ Never the data dir: ADS-B rewrites its
  aircraft every 2 s, which an SD card must not see. Live data, not a log — it does not outlive a reboot. The Android app's one
  radio (no directory set): the same table in memory, one code path.
- **Bounded by construction:** header 64 B · 128 HFDL ground stations × 16 B · **1024 rows × 256 B** = 264 256 B, made at
  full size; a wrong magic / version / size is re-made empty. Full: the row heard longest ago gives its slot up; a row
  unheard for an hour is free.
- **Off every DSP and decoder thread:** a `Sharer` thread per process (decoder priority) asks each provider every 2 s for
  what changed — ADS-B's `Service::shareSince` (the tracker under its lock: address, callsign, the aircraft database's
  registration / type / the leg it is on, the position while it is under a minute old) and the datalink table's
  `Tracker::shareSince` (address, registration, flight, airports, the last label, the position and where it came from,
  never a body) — and merges them into the file: one `pread` of the table, a `pwrite` per changed row.
- **The merge (`mergeInto`, one rule for every writer):** by ICAO address; else by REGISTRATION (dots, hyphens and spaces
  ignored), never across two different addresses; else by FLIGHT ID exactly, never across different addresses or
  registrations. An update that names two rows (its address one, its registration the other) joins them. Field by field the
  newest wins; the position is the newest fix by arrival, with its source.
- **Sources:** a bit each — ADS-B, ACARS, VDL2, HFDL, and ADS-C beside its bearer when an ADS-C report gave the position —
  with when each last heard the aircraft.
- **Read:** `GET /vibeserver/aircraft.json[?since=<s>]` on ANY radio (CORS open, as `/addon/hfdl/*`):
  `{now, count, aircraft:[{key, icao?, reg?, flight?, type?, lat?, lon?, alt?, trk?, gs?, ground?, posSrc, posAge, posTime?,
  from?, to?, label?, labelName?, app?, via?, src:[…], srcAge:{…}, age, n}], stations:[{id, name, lat, lon, khz, age}]}`;
  `"off":1` where the owner switched ADS-B, the datalink and HFDL all off. `key` is the address, else `r:<REG>`, else
  `f:<FLIGHT>`.
- ⚖ Flight data only, as the datalink list shows by default; the table never holds a message body.
- **Tests:** `test-aircraft-share` 42/0 (the merge rules, joins, no merge across addresses, newest fix, the cap and the hour,
  the exact file size and a corrupt file, **four processes writing 200 aircraft at once** — every row with all four sources
  and every count kept —, the datalink provider never handing over a body, the JSON); `test-server-avmap.mjs` 15/0 (two
  REAL radio processes on one runtime dir: ADS-B from `test-adsb --write-clip` on one, the VDL2 clip on the other; 4CA97B and
  406B90 once each with ADS-B + VDL2 from either radio's endpoint, 400F11 ADS-B only, 3C6548 VDL2 + ADS-C with its
  position, no key twice, no body, the file's size, the rows outlive the modes).

## The aviation map (`src/services/aviationMap.ts`, 2026-11-16)

Modelled on the SHIPPING MAP (`shippingMap.ts`, docs/v12/SHIPPING-MAP.md): one rule, two readers (web `main.ts`, app
`SDRScreen` → `MapOverlay`), the shared track layer (`tracklayer.js`).

- **Data:** `/vibeserver/aircraft.json` every 3 s while open, and — in the DATALINK mode — its live report (`avMerge`: the live
  row's fields win, the table adds the other radios' sources and a newer position; joined by address, registration, flight).
- **Layers = the sources:** ADS-B · VDL2 · ACARS · HFDL · ADS-C; an aircraft is drawn while ANY source that heard it is on.
  Then **HFDL STATIONS** (the ground stations heard, a mast glyph) and **AIRPORTS** — always present, **on by default**: the
  basemap's `aero` profile; off = the new `aero-plain` profile (aero without airfields and runways; `vibemap.js` and
  `vibemap-style.json` — a server whose map pack predates it gets the airport layers switched off by name).
- **No dead controls:** a source with no aircraft has no toggle; the source(s) the map was opened for are always there and
  first (DATALINK → VDL2 + ACARS, HFDL → HFDL + its stations, ADS-B → ADS-B); one layer alone, no toggle row.
- **The glyph:** an aircraft pointing along its track (ADS-B), coloured by the source of its position (ADS-B amber, VDL2
  green, ACARS blue, HFDL orange, ADS-C violet), a small number when more than one source heard it; faded past 10 min.
  The card: flight, registration, ICAO, type, route / OOOI, position (and from which source, how old), altitude, speed and
  track, the fix time as reported, the range from here, **Heard on: ADS-B now · VDL2 40 s**, the last message's plain name.
- **Where it opens — web:** the DATALINK mode's own view IS the map (`AVIATION MAP · DATALINK`, its bands in the top bar, the
  list in two sections: THIS RADIO — HEARD NOW (with the owner's message text where the server sent it) and OTHER SOURCES
  ON THIS SERVER, EXIT DATALINK); **MAP** in the HFDL decoder box (standalone, CLOSE MAP — the web had no HFDL map); **AVIATION
  MAP** in ADS-B's top bar, drawn only when another source has aircraft (asked every 20 s). **App:** the DATALINK box's MAP
  (MapOverlay `avdata`, now `AVIATION MAP`, the `aero` basemap, toggles in its list, AIRPORTS switched live through
  `__trkProfile`).
- **Copy:** sdrTour mode card, About V12 note, the web tutorial.
- **Tests:** `test_aviation_map.ts` 47/0; `test-web-avmap.mjs` 28/0 (headless Edge `--mute-audio`, two real radio processes);
  `test-web-avdata.mjs` 22/0 (adapted: the list is the map's); `test-web-shipping-map.mjs` 36/0 (TrackView unchanged for it).

## ADS-B only while heard (beta88, 2026-11-20, branch `b88-maps`)

Stuart, the Pi 500 in DATALINK (ACARS) on the V4L that normally does ADS-B: *"how is there 326 adsb aircraft on the map when i
am in ACARS mode? plus my adsb only picks up about 30-40 on this box … as i am using the sdr that is normally used for adsb
there should be 0 then"*. Cause: the table keeps every row `kKeepSec` (an hour — right for the datalink, whose bursts are
minutes apart) and the map drew and counted every row as live; only 4 had been heard in the last 60 s.

- **Server** (`vibe_aircraft_share.h`): `kAdsbNowSec` 60. A process whose ADS-B decoder is running stamps the file header's
  `adsbLiveMs` (old padding — same layout, same version) every flush (`Sharer::adsbLive`, registered beside the "adsb"
  provider in `local_sdr_shim.cpp`, asks `g_adsb.running()`); a stamp older than `kAdsbLiveSec` (10 s) = no radio on this
  server is decoding ADS-B. `aircraft.json` adds `adsbLive` 1/0, `adsbLast` (s since the stamp; -1 never), `adsbNow` (rows
  ADS-B heard within the minute while live), `adsbWin` 60, `adsbOff` (the owner's switch). The rows still stay the hour.
- **Clients** (`aviationMap.ts avNowRule`, inside `avCompose` — web and app alike): ADS-B counts only within
  `AV_ADSB_NOW_SEC` (60) AND while `adsbLive` is not 0 (an older server that cannot say: the age alone). An ADS-B-only
  aircraft not heard now is neither drawn nor counted; one also heard on the datalink keeps its datalink history, 'ADS-B'
  leaves its sources (the card's *Heard on* shows "ADS-B 20 min ago (not since)") and an ADS-B position becomes
  `lastSeen` "HH:MM UTC" — faded, no track or speed, the row says "ADS-B last seen 17:36 UTC". `avAdsbNote` puts the reason
  on the status line: "ADS-B: this server's ADS-B radio is in use for something else" (stamp seen, now stale) / "no radio on
  this server is decoding it" (never stamped, but ADS-B rows exist) / "none heard in the last minute" / "switched off on
  this server". Nothing is said on a server ADS-B never touched.
- **Tests:** test-aircraft-share 54/0 (§11 new), test_aviation_map 72/0 (25 new), test-server-avmap 18/0 (3 new),
  test-web-avmap 34/0 (6 new: "ADS-B 3" while radio A decodes; radio A leaves → the in-use words, BAW117 gone, 4CA97B kept
  with "ADS-B last seen"), test-web-avdata 22/0, test_aviation_app all passed. Not verified on the Pi 500 itself.

## Aircraft data for every source (beta91, 2026-11-22, branch `b91-avinfo`)

Stuart: *"our adsb logos and silhouettes could apply to vdl2 acars hfdl cant it, they are the same aircraft after all"*.
Until now only the ADS-B decoder looked aircraft up (`vibe_adsb_service.h enrich`), so a row ADS-B never heard showed just
its address; one also heard on ADS-B had a registration and type only because ADS-B put them in the shared table.

- **Server** (`vibe_aircraft_info.h`, new): `GET /vibeserver/aircraft.json` passes `vibeavinfo::extraFn()` to
  `vibeavshare::toJson` (new `Extra` hook). For each row it READS a cache (per row key — the address, else `r:REG`, else
  `f:FLIGHT`) and queues a miss; ONE detached worker at decoder priority (`vibe-avinfo`) does the lookup — the ADS-B path's
  own calls on the ADS-B tables (no copy): `Db::lookupHex` (address), `Db::lookupCallsign` (operator from the flight's
  airline prefix, IATA ACARS IDs too — "U21" → EZY; the VRS overlay's operator by address), `Db::judge` (the leg, where the
  row has a position), `Db::resolveImages` (the owner's logo / silhouette keys). Redone when the row's flight or
  registration changes, on a first position (for the leg), or after an hour (the weekly data update). Bounded: kCap (1024)
  answers, least-recently-asked evicted; kCap waiting. A registration-only row is not looked up (no index by registration).
  ★ A row's FIRST poll goes without — the next (3 s later) has it.
- **JSON** (each row, when known): the ADS-B aircraft data's own names — `desc wtc eng species country countryName mil op
  opName logo sil` and `route{…}` (only where the row has no `from`/`to` of its own). `reg` / `type` are filled from the data
  only where the row has none (the datalink's own registration wins).
- **Clients** (`aviationMap.ts`): `sharedSanitise` keeps them as `AvPlane.info` (adsbInfo.ts `cleanAdsbInfo` — the ADS-B
  sanitiser, its patterns); `avMerge` carries them onto the live DATALINK row; `avPlaneAdsb` hands them to the ADS-B layer
  (t → the type silhouette, op → the badge, logo / sil, the flag, MIL, the card's operator and route); `avPlaneRows` puts the
  flag, operator and MIL in the AIRCRAFT list. The aviation layer now gets `imageBase` (app MapOverlay `acMake`, web
  TrackView `imageBase` from `adsbImageBase()`), so the owner's logos show as on the ADS-B map. ODC-By: the layer's card
  carries the Mictronics notice whenever these fields show (unchanged).
- **Tests:** `test-adsb-data` 318/318 (§ the aviation map's datalink rows, new: a VDL2-only 4CA2D6 → EI-DLJ / B738 / IE; a
  callsign arriving → RYR; an IATA ACARS flight "U21" → EZY + route, the row's own airports win; the row's registration wins;
  a military HFDL logon; an unknown address → nothing; registration-only not queued; the queue and the answers stop at kCap;
  the worker thread answers within 2 s); `test-aircraft-share` 54/0 (unchanged, the hook is optional); `test_aviation_map`
  85/0 (13 new: parsing, hostile keys dropped, the layer shape, the database route vs the datalink's, list flag / operator /
  MIL, the live row joined); `test-server-avmap` 22/0 (4 new — two real radio processes: VDL2-only 3C6548 → DE on both
  endpoints, BAW117 → BAW); `test-web-avmap` 40/0 (2 new — headless Edge `--mute-audio`: 3C6548's row has 🇩🇪, 4CA97B's
  names Ryanair; the web rows now draw the flag). Fixtures and synthetic clips only — **not yet seen on air**.
- ★★ **Shared IATA codes (Stuart approved, 2026-11-22):** BA, LH and U2 are each held by more than one airline on the VRS
  data, so the unambiguous IATA map names none. `Db::lookupCallsign` now lets the aircraft decide (`iataAllLocked`,
  `sharedIataLocked`): the address's own VRS-overlay operator wins where it is one of the code's airlines; else R = the
  airlines with a filed route for that flight number, C = the airlines (Mictronics operators' country) of the aircraft's own
  country (its address block). One in R not contradicted by C, or none in R and exactly one in C, or the single R∩C — else
  none (never a guess between two). ADS-B benefits too. Fixtures: BAX / EZS / DLH / GEC share BA / U2 / LH. test-adsb-data
  327/327: BA0117 on G-XLEA → BAW; U22468 on G-EZWK → EZY + route; LH4567 on a German address → none (both German, no route);
  BA0117 with no address → none; the route and the country disagreeing → none. ★ The country clue needs the Mictronics
  download and the route clue the VRS routes (the weekly update) — on the starter alone a shared code still names none.
- ★★ **The ODC-By notice on the AIRCRAFT list:** the card's words and links (`AV_DATA_CREDIT` / `avCreditHtml`) as one small
  line at the list's foot while any row shows the aircraft data (`avShowsData`: a type, or data the server attached) — app
  `TrackLiveList credit`, web `.avCredit`. The ADS-B map's own live list (AdsbLiveList) has no such line (unchanged).

## On air 2026-11-16 (daytime, 16:38–16:56 UTC)

Recordings (not in git): `~/VibeSDR-test-audio/aviation/2026-11-16/`, each with `.log` (the session) and `.decoded.txt`
(every block / frame / message our decoder printed — the text kept this time, as RESUME step 3 asked).

- **ACARS** — Pi 500 V4L via the front door's `/r/00000004/` (the radio ports listen on 127.0.0.1 only now), listeners 0
  checked by `iqrec.mjs` before and every 5 s during; 131.675 MHz, 2.4 MS/s, 476 s, 0 drops, the dial put back.
  **91 blocks** (131.725: 51, 131.825: 28, 131.525: 12, 131.550: 0), 8 repaired, 8 synced bursts not decoded; 0.5 % of an
  M5 core. Labels seen: `_` (link test), H1, H2, B0/BA/B1/B2, 14, 15, 16, 18.
- **VDL2** — same radio, 136.8625 MHz, 480 s, 0 drops. **1028 frames, 233 of them ACARS, 78 aircraft** (136.825: 626,
  136.975: 170, 136.850: 127, 136.875: 44, 136.900: 43, 136.775: 18). Through the service and the applications
  (`test-avi-service --cu8 … 136862500`, `.service.json.txt`): **1 ADS-C position** (G-ZBLB 52.475 N 1.255 W, 21 980 ft,
  ":53:10" — plausible over the Midlands), **5 OOOI**, 7 CPDLC (named), **70 unparsed**, 0 FMS / label 16 positions.
- **HFDL** — UberSDR RX888 (read-only, its own channel), two 600 s windows at once, frequencies from 75 s of its feed (11184,
  15025, 17928, 8942 were busy). **11245 kHz window: 23 of 96** reference messages; **15000 kHz: 23 of 127**; plus 39 / 57
  messages the reference feed does not list (ground uplinks — logon confirms, logoffs, squitter-carried ACARS). The misses
  are mostly 300 bit/s aircraft downlinks. **The detection-hold change (11e8bff4f) is confirmed harmless on real signals:**
  the decoder from before it (689452b00's `vibe_hfdl.h`) gives exactly the same 23 / 96 and 23 / 127. HFDL's real-signal
  catch rate is the biggest gap left (23/42 and 12/16 on the 2026-10-31 night windows; a quarter or less today).
- **Real samples for the layouts left out** (no layout added — the rule: published + checkable first):
  - **label 20** `POSN51524W001012,,165521,199,VELAG` (3C5821, DAFAA) — the open question (decimal 51.524 / −1.012 or
    51° 52.4′ / 1° 01.2′): an ADS-B recording at the same time would settle it.
  - **label 16** `POSGI4235/161641,ALT 3448,ETA ,FOB 259,N 52.714 W  1.053,WS …` — a different label-16 layout from the one we
    read; **label 15** `FST01EGLLKATLN535028W0030313340 …` (an FST flight-status report).

## Decision: its own mode AND the ADS-B aircraft

- **ACARS and VDL2 need the radio at 131.x / 136.x MHz; ADS-B needs 1090.** One RTL cannot do both, and ACARS
  (131.525–131.825) and VDL2 (136.725–136.975) are 5 MHz apart — more than any capture here. So: **one whole-radio mode,
  "Aircraft datalink", with two BANDS** (ISM's pattern: the tune steps move a band): `acars` (the radio at 131.675 MHz
  in Europe) and `vdl2` (136.850). Regions add bands (US ACARS 129.125–131.550 needs its own centre).
- **And attached to the ADS-B aircraft:** VDL2 names the aircraft by its ICAO 24-bit address — the same key ADS-B
  uses — and ACARS by its registration (the ADS-B aircraft database maps ICAO ↔ registration). On a server with two
  radios (one on 1090, one on 136.850), the ADS-B card gets a "datalink" line (the last label / position report) from
  the `Tracker` row with that ICAO address. The Tracker is the join point; ADS-B's list keeps its own shape.
- **HFDL is a listener's decoder on the dial** (HF USB, `vibe_decoder_host.h` style), one channel. Its natural client
  view **already exists**: the clients' HFDL map (`MapOverlay` `KIND==='hfdl'`) polls UberSDR's `/addon/hfdl/aircraft`
  and `/groundstations`. Serving the same two endpoints from VibeServer (from the `Tracker`'s HFDL rows) lights it up
  with no new map code; `SDRScreen.tsx` gates it `!isVibe` today.

## Cost

**Method** (as `PI2-REAL-CPU.md` / `DECODER-SIMD-AUDIT.md`): armhf static, `-O3 -ffp-contract=fast -marm
-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard` (GCC 12, `vibeserver-cross:bookworm-armhf`), counted under qemu-arm
with the TCG instruction-count plugin (`ft8perf-x86`), on 10 s slices of the REAL recordings (60 s for HFDL). Build and
run scripts: `~/Library/Caches/decaudit/xb_avi.sh`, `qa_avi.sh`; results in `q/avi/`. The tool's own u8 → float
conversion (`cu8`, 36 M instr/s at 2.4 MS/s) is subtracted — the server converts once for every mode. "% of a Pi 2
core" uses the measured IPC of NEON-heavy kernels on that Pi (0.35–0.48, PI2-REAL-CPU.md): 900 MHz × 0.4 = 360 M
instructions a second, range in brackets. **Not run on the real Pi 2** (optional; not done).

| Decoder | Workload | NEON M instr / s | % of a Pi 2 core |
|---|---|---|---|
| ACARS | 3 channels, 2.4 MS/s, one thread | **107** (front end ≈ 96, the three demods ≈ 10) | **≈ 30 %** (25–34) |
| HFDL | one listener's dial (demod only) | **≈ 11** | **≈ 3 %** |
| **VDL2** | **all 12 channels, 2.4 MS/s** | see the table below | |

### VDL2: SIMD + worker threads (2026-11-01, all 12 channels — "worldwide")

Counts: 10 s of the Pi 500 capture, armhf NEON, qemu; the tool's u8 conversion (36 M/s) taken out. % of a Pi 2 core
at 900 MHz × IPC 0.4 = 360 M instructions a second (range 0.35–0.48).

| | total M instr / s | **hottest thread** | % of a Pi 2 core (hottest) |
|---|---|---|---|
| before (one thread, 58-tap FIR, gate fixed — 33adaa05e) | 341 | the one thread | **95 %** (79–108) |
| SIMD, still one thread (9eff85188 with 0 workers) | 205 | the one thread | **57 %** (47–65) |
| SIMD + 1 worker | 77 front + 132 worker | the worker (12 channels) | **37 %** (31–42) |
| SIMD + 2 workers | 77 + 2 × 68 | the front end | **21 %** (18–24); workers 19 % each |
| SIMD + 3 workers | 77 + 3 × 46 | the front end | **21 %**; workers 13 % each |
| **SIMD + 4 workers (quad core default)** | 77 + 4 × 36 = 221 | **the front end** | **21 %** (18–24); workers 10 % each |
| one channel per thread (12 workers), for comparison | 77 + 12 × 14 = 245 | the front end | 21 %; +28 M/s more ring copies and wakes than 4 workers, no better hottest thread |

- **Per thread, measured:** front end (half-bands + one 2048-point PFFFT) 77 M/s; each channel (its slice, response,
  inverse FFT, resampler, gate, sync, demod) 10.5–11.0 M/s (busy 136.975 the same as an idle one: the gate keeps idle
  channels cheap); the ring copies and wakes 3.5 M/s per worker (the 4-worker run minus the inline run).
- **Parity after the SIMD + threads work:** 60 s of each capture (Pi 500 from 60 s, Pi 2 from 30 s), 0 workers: the
  whole output is identical line for line on armhf NEON, armhf `VIBE_FORCE_SCALAR` and i686 SSE2 (qemu); natively 0, 1, 2
  and 4 workers decode the same frames on the full recordings.
- **A handful of channels per worker beats one per thread**: the hottest thread is the front end either way (21 %),
  and 12 threads cost 3× the copying of 4 on a 1 GB Pi 2.
- **SIMD, step by step** (whole process incl. the tool's 36 M/s): 377 → 278 M/s (the receive filter into the slicer)
  → 299 (a rational resampler whose 64-bit divide per output was 20 % on ARMv7 — `__udivmoddi4`) → 232 (stepped
  without a divide; the lag products and the 16-term unique-word correlator four samples at a time). The frames never
  dropped (Pi 500 387, Pi 2 165 against 387 / 162 before).
- **ACARS is not split**: its front end is ≈ 90 % of it (96 of 107 M/s), the three demods ≈ 10 — a worker would
  take 3 % off a 30 % thread.

★ Trade-offs stated: the workers cost 14 M instr/s more in total (+7 %) to cut the hottest thread from 57 % to 21 %;
the slicer filter needs a 2048-point FFT (bins of 293 Hz) where 1024 did before (+~10 % on the FFT, far less than the
FIR it replaced).

## Dead ends (all committed)

- **ACARS coherent AM** (carrier frequency from the lag-4 product, a ~90 Hz carrier phase, the in-phase audio):
  no gain over the envelope on either recording or the synthetic table (180142e44, removed in 052a6d6f0).
- **ACARS 8 bit clocks** instead of 4: +1 block on one recording, nothing on the other; not worth 2× the demod.
- **ACARS repair beyond 4 bad characters / 3 candidate bits**: no further block on either recording.
- **VDL2 root-raised-cosine and raised-cosine receive filters**: ISI (the transmitter's pulse is a full RC).
- **VDL2 differential detection**: 230 frames vs coherent 300 on the same recording.
- **HFDL single-A detection**: phantom detections and early commits; replaced by the pair.
- **HFDL retry on CRC failure** (a second step size, timing ±½ sample): found nothing (4373649ef), removed.
- **Bugs that cost real decodes, found by the recordings** (kept in the history): VDL2 erased parity refused as an
  error location (every shortened block failed: 35 → 230 frames); VDL2 noise floor rising 3.7× a second; VDL2 Q≈1 on
  one dominant product; HFDL Σ|d| over every sample instead of every 4th.

## Wired (2026-11-16, branch v12-aviation-wire)

**Server** (`local_sdr_shim.cpp`, AIS's shape with ISM's bands):
- `{"type":"avdata", on, band?|step?}` → `{"type":"avdata", band, bandName, hz, msgs, aircraft[], dropped, gain, gainAuto}` (2026-11-24: `gainNote` in place of gain on the HF+, `rateNote` while ACARS + VDL2 has raised the capture)
  once a second, `avdata_off`, `avdata_error{why}`. Bands (`vibe_avi_mode.h`): **ACARS** radio at 131.675 MHz (131.525,
  131.550, 131.725, 131.825) and **VDL2** at **136.8625** MHz (every channel 136.700–136.975) — both centres between
  channels so the DC remnant hits none (the plan said 136.850, which is itself a channel). Step clamps at the ends.
- Gain: AIS's 85 % up the radio's own list, `vibeism::GainRule` on the service's clip share (measured on the service
  thread, every 4th sample). One whole-radio mode at a time; tune / zoom / mode held; given back exactly as found; leaves
  when the radio empties and comes back for the same listener (`mode_resume` / arrivalResume, with its band).
- Reach: a radio that tunes the whole band, not an Icom (`vsAvdataReach`; the HF+ too since 2026-11-24 — §The HF+, and ACARS + VDL2 together); hwinfo `avdata`, `avdataWhy`,
  `avdataBands`, `avdataText`; in the directory's `modes`; `vibe_radio_caps.h` `avdata`, `hfdl` (HFDL reads audio, so an
  Icom has it).
- ⚖ **Message bodies**: only with the owner's `avdata+text` (a "+" token, IN blockedModes when on, off by default, with
  §4.3's notice on the setup page and ServerModeScreen), and then only on sockets that proved the admin password
  (`adminNow`). Everyone else: flight, registration, ICAO, label name. The HFDL box line never carries a body.
- **HFDL**: decoder ext `hfdl` (`vibe_hfdl_run.h`: 48 kS/s USB audio, mixed down by 1440 Hz, FIR to 12 kS/s, a sharp FIR
  flat to 1.25 kHz / shut by 1.65 kHz against the image, `Demod(12000)`), reach = the HF aeronautical bands
  (`vibe_decoder_reach.h`). Every message to the radio's table (`vibeavi::sharedTracker()`); `GET /addon/hfdl/aircraft`
  and `/addon/hfdl/groundstations` in the clients' HFDL-map shape (the public station list with positions; frequencies
  only as heard here; no `sig_level` — the map reads dBFS and we measure SNR).
- Benchmark rows: `dec_vdl2` (primary for `avdata`), `dec_acars`, `dec_hfdl` (a slot) — real time, hottest thread,
  on `vibe_avi_tx.h`'s bursts; a benchmark HFDL run writes to its own table (Env::aircraft), never a server's map.
- ★ **HFDL fix found while wiring**: a strong burst (≥ 16 dB, synthetic) decoded nothing — the pre-key armed the pair
  rule at A1's end and the 256-sample hold expired before A2's end (508 samples later). The hold now covers the pair's
  spacing: 25 dB 0/10 → 8–9/10 per mode, −4…10 dB unchanged. **Not re-checked on the real recordings: the IQ captures
  are no longer in `~/VibeSDR-test-audio/aviation` (only their logs / reference transcripts are).** Known limit: 40 dB
  over digital silence still commits early (not an HF condition).

**Clients**: the app — DATALINK key (TUNES THE RADIO ITSELF), `AvdataPanel` (ISM's box: band keys, EXIT, the list),
HFDL MAP on a VibeServer starts the `hfdl` decoder on the dial then opens the map (a VibeServer without `hfdl` draws no
HFDL MAP key), the known-signal HFDL label no longer excludes VibeServers. The web — DATALINK in the mode menu and the
whole-radio keys (a TrackView: bands in the top bar, the list, EXIT), the HFDL decoder key (lines in the box; the web has
no HFDL map). The R36S — `avdata` map mode (list first, ACARS / VDL2 keys). Copy: About V12 note, sdrTour mode card,
web tutorial (website: once it ships).

**Tests** (all registered in `scripts/run-tests.sh`): test-acars 10034/0, test-vdl2 42/0, test-hfdl 44/0 (+ §5 the audio
path, the map JSON, 2 min of audio noise = 0), test-avi-service 7/0, `test-server-avdata.mjs` 29/0 (the real server on a
fake rtl_tcp with `test-vdl2 --write-clip`: bands, the table, bodies to the owner only, the step, the hand-back, the
owner's switch, `/addon/hfdl/*`), `test-web-avdata.mjs` 15/0 (headless Edge `--mute-audio`, desktop + 390 px),
`test_aviation_app.ts` 11/0, test-bench-decoders, test-radio-caps 227/0, test-setup-radio-caps 71, test-decoder-hosts,
test-server-ais-aprs 36/0 and test-server-decoder-reach 35/0 unchanged. **Nothing was run on air.**

## The HF+, and ACARS + VDL2 together (2026-11-24, branch `hfplus-avdata`)

**The Airspy HF+ gets the datalink and AIS** (Stuart's XCover: an HF+ at 912 kS/s, VHF 60–260 MHz, no DATALINK key while
the Pi 500's V4L heard 39 aircraft on VDL2 in 5 min). It was left out "for AIS's reason — its VHF is one fixed-gain front
end". That was not a reason on its own; what would have made the mode wrong there was its **gain**: 85 % up the radio's
gain list, stepped down by `vibeism::GainRule`, reported as "gain 43.4 dB auto". The HF+ has no variable gain (preamp,
8-step attenuator, its own AGC — the owner's, drawn by LocalHardwarePanel). So (`vibe_mode_gain.h`):
- `burstGains()` is EMPTY on the HF+ (`vibemodegain::steps`), so the enter, the tick and the leave write no gain at all
  (the leave's `setGain(saved)` is skipped there too); `getTunerGains()` was already empty on it — now it is a rule;
- the report carries `gainNote` ("HF+ AGC · attenuator 12 dB · preamp", from the source's own state) instead of
  `gain`/`gainAuto`; the web status line and the app's AvdataPanel print it (`avGainText`); AIS's status shows no gain;
- the rates: the HF+ runs its one rate (912 kS/s; 768 on older units), pinned — the mode never asks for another. The
  reach checks now use the RECEIVABLE span (`usableSpanHz`: 768 of 912 kS/s, 648 of 768), and ACARS (0.361 MS/s) and VDL2
  (0.389 MS/s) both fit with room. The HF+ is low-IF with no DC spike; the band centres between channels cost nothing.
- `vibe_radio_caps.h`: `ais`, `aisstore`, `avdata` no longer exclude `airspyhf` (sondes still does: it stops at 260 MHz).

**ACARS + VDL2 together** (Stuart: "on a radio wide enough we should allow ACARS and VDL2 together"): a third band,
`acars+vdl2` ("ACARS + VDL2"), all 16 channels, the radio at **134.25 MHz** (the middle of 131.5125…136.9875 — the DC 2.4 MHz
from the nearest channel). `minRateHz` 6.083 MS/s. `Band::wide`: **offered only where it can run** (`vibeavi::offered`) —
the running usable capture already holds it, or the owner's 8 MS/s switch will raise it there. Never an RTL (2.4 MS/s),
never the HF+. The service runs it as two LANES (`Service::start("acars+vdl2")`): VDL2's 12 channels on their own thread
and its workers (the heavy half), ACARS's 4 on another, one copy of each block shared by both (8 MS/s × 0.5 s is 32 MB).
One aircraft heard on both is one row (the tracker's ICAO / registration merge — tested).

**The owner's 8 MS/s switch** (Stuart: "a settings toggle to allow 8 MS/s (for SDRplay, to compensate for the roll-off)"):
`avdata+8m`, an option under the datalink, IN the modes CSV when ticked (`lora+all`'s convention), on the Setup page and the
Android server screen. Drawn only where the radio lists 8 MS/s or more (`vibe_radio_caps.h`: an RSP, an Airspy R2, a
HackRF, a USRP — the row is `cap: true`, answered by the radio, not its parent). With it, picking ACARS + VDL2 on a narrower
capture raises the rate to 8 MS/s (or the next listed rate above: an R2's 10) — ADS-B's door: the rate guard's marker down
first (`g_rateGuard.mark(…, "AVDATA")`), the centre stored, then the change; an RSP's IF filter is the rate's own (8 MHz).
Any single band, or leaving, gives the owner's rate back (`g_avBoostFrom`, before the owner's lock is restored). Never
while the owner has PINNED the rate, never once the guard recorded a fall-over at a boosted rate (the stall watchdog now
covers the datalink too and leaves the mode). A crash at 8 MS/s: the mode's rate is never persisted, so a restart opens at
the owner's own, and the guard holds the switch off until the owner clears it. The report says
`rateNote: "capture raised to 8 MS/s for ACARS + VDL2 — back to 2 MS/s when you leave"`. Refused politely where it cannot
run: "ACARS + VDL2 needs a capture of 6.1 MS/s or more — this receiver runs at 2.000 MS/s (its owner can let the aircraft
datalink raise it to 8 MS/s)". 6.0 MS/s is just short — an RSP at 6 needs the switch.
- Default: the benchmark's `dec_avboth` row (both decoders at 8 MS/s, real time, hottest thread) with `lora+all`'s
  INVERTED rule — GREEN ticks it on (on a fresh setup), anything else leaves it off with the figure; never hidden. Measured
  here (M5 Pro): 3.2 % of a core at the hottest thread (vibe-vdl2), 8.3 % process. The radio's own cost at 8 MS/s (USB, the
  spectrum) is NOT in the row.
- ✗ Not built: the R36S handheld's DATALINK keys are fixed to ACARS / VDL2 (its RTL can never be offered the third band).

**Tests**: test-avi-service 41/0 (+ §3 the HF+ at 912 / 768 kS/s — every VDL2 and ACARS channel decodes through the
service, no gain stepped, the report true; §4 ACARS + VDL2 at 6.144 and 8 MS/s on two threads, the merge, who is offered
it), test-ais 88/0 (+ the HF+ rates, clean and at 14 dB), test-radio-caps 265/0, test-bench-decoders (`dec_avboth`; its
`dec_rade` rows are NOT RUN-shaped failures here — no RADE library in this worktree), `test-server-avdata.mjs` 53/0 (+ an RTL
never sees the band or the switch; a radio listing 8 MS/s — the fake rtl_tcp given an RSP's list with the test-only
`VIBE_TEST_TCP_RATES` — at 2 MS/s: not offered with the switch off, refused with the numbers; with it on: 8 MS/s on entering,
2 MS/s back on VDL2, 8 again, 2 on leaving with the dial given back; the rate pinned: not offered), `test_aviation_app.ts`.
**Nothing on air yet** — owed: the XCover's HF+ on VDL2 / AIS, and an RSP at 8 MS/s near an airport.

## RESUME HERE

**2026-11-22, branch `b91-avinfo` (beta91):** the aircraft data for every source (§Aircraft data for every source) — server,
web and app wired, fixtures only. Owed: an on-air look on a server with the Mictronics data downloaded (a VDL2-only row
gaining type / operator a few seconds after it appears; the owner's logos on the aviation map's card); the app on a device.

**2026-11-16, branch `v12-avmap` (from `v12-experimental` 8e28d5250), not merged, not pushed:** the shared aircraft table
(§The shared table), the AVIATION MAP (§The aviation map), and the on-air run (§On air 2026-11-16) — step 1's recordings
are done; step 2 (the ADS-B card's datalink line) is superseded by the shared table + the map. Owed: an app device run of
the map (tsc clean); an app ADS-B entry to the map (the web has AVIATION MAP in ADS-B's bar; the app's ADS-B map has none);
an AVIATION MAP for a VibeServer's HFDL in the app (its HFDL map still reads `/addon/hfdl/*` of its own radio); the
directory's copy of `vibemap-style.json` (`sync-mapgl-assets.mjs` full run) before the next directory deploy; HFDL's
catch rate on real signals (above); the label 20 / 16 / 15 layouts on the samples above (a decision).

Done before: steps 1–5 below (see "Wired") and the ACARS applications (§Applications, 2026-11-16: commits 439b7a91f, d246cc9e8,
05bcb9f0f on `v12-acars-apps`, not merged). Tests: test-acars-apps 61/0, test-avi-service 7/0, test-acars 10034/0,
test-vdl2 42/0, test-hfdl 44/0, test_aviation_app 21/0, test-server-avdata 35/0, test-web-avdata 21/0 (headless Edge
`--mute-audio`, 3 aircraft on the map). **Nothing run on air.** Left, in order:

1. **On air.** Re-record (the captures are gone): ACARS 131.675 / VDL2 136.85 at 2.4 MS/s and two HFDL windows, re-run
   `test-acars --cu8`, `test-vdl2 --cu8`, `test-hfdl --wav … --ref` — the HFDL hold change must be confirmed on the real
   6598 / 8931 kHz recordings (23/42 and 12/16 before). Then the mode on a real RTL (bench radio, 0 listeners).
2. **The ADS-B card's datalink line** (§Decision): on a two-radio server, the ADS-B aircraft with the same ICAO address
   as a Tracker row gets its last label / position. Not built: the Tracker is per radio PROCESS, so this needs the two
   processes to share it (the gallery-lock pattern AIS's ship store uses) — a design call.
3. ~~ACARS label parsing~~ — **done 2026-11-16** (§Applications, branch `v12-acars-apps`). Left: **check it on air** —
   record ACARS + VDL2 with the decoders' frame print (`test-acars --cu8`, `test-vdl2 --cu8` print every block's text)
   and KEEP those transcripts this time; count how many real messages each layout takes and look at every `unparsed`
   label that carries a position-looking text (label 20, 5U, 80, H1 sublabels other than M1, the European airlines' own
   layouts) before adding any. HFDL carries much ADS-C: its positions now reach `/addon/hfdl/aircraft` too (untested on
   air). Then **US ACARS** (129.125–131.550 MHz needs its own centre and a heavier front end — a third band).
4. The app: `mode_resume` for `avdata` (the server's arrivalResume already puts a returning listener back), tune keys /
   drum stepping the bands (ISM's stepper — today only the box's keys do), a gamepad/keyboard nav for `AvdataPanel`.
   The web: the tune arrows stepping bands. The R36S: a canned feed + golden for the mode.
5. SIMD left (unchanged): HFDL's 126-term correlator, the equaliser, the Viterbi ACS; HFDL's mixer in `vibe_hfdl_run.h`
   is a scalar complex rotator (48 k multiplies a second).
6. Decoder work left (unchanged): HFDL's misses (decision feedback from the Viterbi), QPSK/8PSK labelling on air, a VDL2
   soft header Chase, X.25/CLNP summaries.
7. Optional: a real Pi 2 bench run (bench binaries only, 0 listeners, never :48000) — the benchmark rows now exist.

### Decisions for Stuart
- **The shared table (2026-11-16)**: it lives in the runtime dir (tmpfs) and is gone at a reboot — live, not kept. Keep it
  that way, or also keep the aircraft in the gallery like the ship store? It holds an hour; ADS-B-only aircraft too.
- **Flight-ID joins**: an aircraft is joined by flight ID only when neither side has an address or registration that
  disagrees; ADS-B callsigns (ICAO "EZY45KL") and ACARS flight IDs (IATA "U22468") rarely match, so the address and the
  registration do nearly all the joining. An IATA↔ICAO airline table would join more (ours to build from the airline
  data the ADS-B database already holds).
- **AIRPORTS** switches the basemap's airfields and runways; a separate airport LAYER (names, ICAO codes, cards from the
  ADS-B database's airports) was not built.
- **ACARS applications**: (a) linking libacars (MIT) instead of / beside our parser — it decodes far more (CPDLC, MIAM,
  media advisory); we did not link it. (b) Origin / destination airports from OOOI and FMS reports are shown by default
  as flight data — or behind the opt-in? (c) Label 20 and other airline layouts wait for a real capture.
- The `avdata+text` notice wording (setup page / ServerModeScreen `AVDATA_TEXT_NOTICE`), and whether the owner should
  see bodies in the app at all or only on the setup page.
- VDL2 centre 136.8625 (between channels) instead of the plan's 136.850.
- The key's name: DATALINK (fits one key size); the box title says AIRCRAFT DATALINK · ACARS + VDL2 on the web.

## The original plan (kept for the record)


1. **Build targets**: add `test-acars`, `test-vdl2`, `test-hfdl`, `test-avi-service` to `vibeserver/CMakeLists.txt`
   (header-only + `vibedsp`, `-O3 -ffp-contract=fast`, as `test-ais`). The cross builds used
   `~/Library/Caches/decaudit/xb_avi.sh` (armhf NEON / armhf `-DVIBE_FORCE_SCALAR` / i686 SSE2).
2. **Server, "Aircraft datalink" mode** — copy AIS's shape in `local_sdr_shim.cpp` (`g_ais`, `aisEnter/Leave/Tick/
   Control`, `vsAisReach/WhyNot`, the modes lists at the `"ais"` string sites, the health watch, `mode_resume`,
   the gallery DayLog) with ISM's BANDS (`acars` 131.675, `vdl2` 136.850). The capture feed is AIS's
   `g_ais.feed(...)` site. Reach: any radio that tunes 131–137 MHz (not the Airspy HF+; not an Icom). *(Superseded 2026-11-24: the HF+ is in.)*
   Gain: AIS's 85 %-up rule.
3. **Owner switches** (the benchmark sets the default, never hides): `vibe_setup_page.h` BLOCKABLE and
   `src/screens/ServerModeScreen.tsx` (ONE list) under "VHF / UHF" → `avdata` (ACARS + VDL2), and under the HF
   decoders → `hfdl`; the **message-body opt-in** on the setup page with DECODER-RESEARCH.md §4.3's notice.
   Benchmark rows in `vibe_benchmark_more.h` (`ruleFor`): VDL2 12 channels, ACARS 3, HFDL one dial.
4. **HFDL as a listener decoder**: a `vibe_decoder_host.h` host on the USB dial (the carrier 1440 Hz above it) feeding
   one `vibehfdl::Demod` at the listener channel's rate; serve `/addon/hfdl/aircraft` and `/addon/hfdl/groundstations`
   from the Tracker; lift `SDRScreen.tsx`'s `!isVibe` gate for HFDL.
5. **Clients**: the web (`web/client/src/` — copy `ais.ts`'s list + map shape) and the app (`adsbList.ts` /
   `MapOverlay` pattern). AGENTS.md: About V12 note, tours, website copy — only once the mode is visible.
6. **SIMD left**: VDL2 is done (slicer multiply, resampler, lag products, unique-word correlator; the coherent demod
   and RS run per symbol / per burst and are not in the profile). HFDL's 126-term differential correlator, its
   equaliser and the Viterbi ACS are still scalar. **Wiring note:** the Service must call `vibevdl2::Receiver` with
   the default worker count (it does) — its frames come out on the service thread at each feed().
7. **Decoder work left**: HFDL's misses (23/42 on 6598 kHz) — next try decision feedback from the Viterbi (re-encode
   the first pass, re-train the equaliser on it); HFDL QPSK/8PSK labelling needs a real 1200/1800 bit/s burst; VDL2
   a soft header Chase and X.25/CLNP summaries; ACARS label/position parsing (labels 16/H1 positions), US channels.
8. Optional: a real Pi 2 bench run (bench binaries only, 0 listeners, never :48000).
