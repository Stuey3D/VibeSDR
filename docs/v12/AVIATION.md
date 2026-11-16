# Aviation datalink (V12): ACARS, VDL Mode 2, HFDL — our own decoders

Branch `v12-aviation` (from `v12-experimental` 460bb2f12). Written 2026-10-31 / 11-01.

| Mode | Code | Test | Status |
|---|---|---|---|
| **VHF ACARS** (AM-MSK 2400 bit/s) | `vibe_acars.h` on `vibe_avi_front.h` | `vibeserver/test-acars.cpp` | **Decoder works on real signals.** Wired: the Aircraft datalink mode's `acars` band. |
| **VDL Mode 2** (D8PSK 10.5 ksym/s) | `vibe_vdl2.h`, `vibe_vdl2_fec.h` on `vibe_avi_front.h` | `vibeserver/test-vdl2.cpp` | **Decoder works on real signals.** Wired: the mode's `vdl2` band. |
| **HFDL** (PSK 1800 Bd, HF) | `vibe_hfdl.h`, `vibe_hfdl_run.h` (a listener's USB dial) | `vibeserver/test-hfdl.cpp` | **Decoder works on real signals** (BPSK modes verified; QPSK/8PSK only self-consistent). Wired: decoder ext `hfdl`. |
| Service, table, JSON, bands | `vibe_avi_service.h`, `vibe_avi_mode.h` | `vibeserver/test-avi-service.cpp` | Wired into `local_sdr_shim.cpp` (2026-11-16) — see RESUME HERE. |
| Transmitters (tests + benchmark) | `vibe_avi_tx.h` | — | ACARS, VDL2, HFDL bursts; `test-vdl2 --write-clip` makes the server e2e's clip. |

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
- `{"type":"avdata", on, band?|step?}` → `{"type":"avdata", band, bandName, hz, msgs, aircraft[], dropped, gain, gainAuto}`
  once a second, `avdata_off`, `avdata_error{why}`. Bands (`vibe_avi_mode.h`): **ACARS** radio at 131.675 MHz (131.525,
  131.550, 131.725, 131.825) and **VDL2** at **136.8625** MHz (every channel 136.700–136.975) — both centres between
  channels so the DC remnant hits none (the plan said 136.850, which is itself a channel). Step clamps at the ends.
- Gain: AIS's 85 % up the radio's own list, `vibeism::GainRule` on the service's clip share (measured on the service
  thread, every 4th sample). One whole-radio mode at a time; tune / zoom / mode held; given back exactly as found; leaves
  when the radio empties and comes back for the same listener (`mode_resume` / arrivalResume, with its band).
- Reach: a radio that tunes the whole band, not the HF+, not an Icom (`vsAvdataReach`); hwinfo `avdata`, `avdataWhy`,
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

## RESUME HERE

Done: steps 1–5 below (see "Wired"). Left, in order:

1. **On air.** Re-record (the captures are gone): ACARS 131.675 / VDL2 136.85 at 2.4 MS/s and two HFDL windows, re-run
   `test-acars --cu8`, `test-vdl2 --cu8`, `test-hfdl --wav … --ref` — the HFDL hold change must be confirmed on the real
   6598 / 8931 kHz recordings (23/42 and 12/16 before). Then the mode on a real RTL (bench radio, 0 listeners).
2. **The ADS-B card's datalink line** (§Decision): on a two-radio server, the ADS-B aircraft with the same ICAO address
   as a Tracker row gets its last label / position. Not built: the Tracker is per radio PROCESS, so this needs the two
   processes to share it (the gallery-lock pattern AIS's ship store uses) — a design call.
3. **ACARS label parsing** (labels 16 / H1 positions → `lat`/`lon` in the rows; the app's box and the web map then show
   them), and **US ACARS** (129.125–131.550 MHz needs its own centre and a heavier front end — a third band).
4. The app: `mode_resume` for `avdata` (the server's arrivalResume already puts a returning listener back), tune keys /
   drum stepping the bands (ISM's stepper — today only the box's keys do), a gamepad/keyboard nav for `AvdataPanel`.
   The web: the tune arrows stepping bands. The R36S: a canned feed + golden for the mode.
5. SIMD left (unchanged): HFDL's 126-term correlator, the equaliser, the Viterbi ACS; HFDL's mixer in `vibe_hfdl_run.h`
   is a scalar complex rotator (48 k multiplies a second).
6. Decoder work left (unchanged): HFDL's misses (decision feedback from the Viterbi), QPSK/8PSK labelling on air, a VDL2
   soft header Chase, X.25/CLNP summaries.
7. Optional: a real Pi 2 bench run (bench binaries only, 0 listeners, never :48000) — the benchmark rows now exist.

### Decisions for Stuart
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
   `g_ais.feed(...)` site. Reach: any radio that tunes 131–137 MHz (not the Airspy HF+; not an Icom).
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
