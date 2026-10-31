# Decoders, wave 2 (V12) — status log

Branch `v12-decoders-wave2` (worktree `~/VibeSDR-decoders2`), off `v12-experimental`. One section per decoder, in queue
order, appended as each lands, so a follow-up agent can resume exactly where the last one stopped.

Queue (Stuart's order): Q65 → FST4W → FST4 → MSK144 → CTCSS/DCS → JJY/BPC/TDF/CHU → ACARS → VDL2 → HFDL → RS41 →
PAGING (POCSAG + FLEX) → MFSK family + Hell → SELCAL → EAS/SAME. Never: HD Radio, AMBE voice, anything encrypted, TV on
servers.

## Standing specs recorded before the work reached them

### PAGING (queue item 7) — spec as refined by Stuart, 2026-10-31

- **ONE paging demodulator with an AUTO mode**, built like our other auto decoders (`decoders/rtty_auto.*` is the
  pattern: candidates run side by side, scored, the winner's text shown, what it chose said in the text, keeps watching).
- It detects and decodes on its own, picking encoding and rate from the signal:
  - **POCSAG** 512 / 1200 / 2400 baud (2-FSK). Tell: the preamble (alternating 1010…, ≥ 576 bits) and the frame sync
    codeword **0x7CD215D8**.
  - **FLEX** 1600 / 3200 / 6400 bps, 2-level and 4-level FSK. Tell: bit sync, the A-word and the frame sync.
  - **A channel that carries both** must work (a shared paging channel switching between POCSAG and FLEX).
- **Show the detected mode/rate on the readout** (e.g. "POCSAG 1200", "FLEX 3200/4").
- **Manual override** to a specific mode/rate when auto cannot lock.
- **0 false decodes on noise** (long noise runs, measured, as every other decoder).
- Unchanged from the earlier decision: **ONE "PAGE" button**; **Android local hardware only** (the phone's own
  plugged-in radio) — never on any server, not even to LAN clients; **nothing to the gallery**.

### SIMD — hard requirement for every decoder in this wave (Stuart, 2026-10-31)

> "make sure EVERYTHING that can be optimised by SIMD NEON SSE2 etc do it. I want these decoders to be so lightweight
> they run on potato spec hardware but powerful enough to hang with the big players whilst doing so."

- Profile each decoder; vectorise every hot loop: FFT/filterbanks (PFFFT, as the CW skimmer), mixers/NCOs,
  FIR/decimation, correlators/sync search, soft demod/LLRs, LDPC/BP and Viterbi inner loops, power spectra.
- NEON (ARMv7 armhf `-marm`, Pi 2; and ARM64), SSE2 baseline on x86 (Atom N450), and a SCALAR fallback giving
  identical decodes.
- Targets: Pi 2 (900 MHz ARMv7), 2009 Atom N450 hosts, 32-bit 1 GB Android phones.
- Each section reports scalar vs SIMD CPU (Mac + Pi 2 instruction estimate) and sensitivity / decode counts vs the
  reference (jt9 as a black box, UberSDR / PSKReporter). Never trade decodes for speed. Where SIMD does not pay on a
  loop, say so rather than forcing it.

### Real-signal servers — standing permission for this wave (Stuart, overnight 2026-10-31)

"if you need UberSDR or any server to test on real signals use mine": UberSDR (RX888, 192.168.86.11:8080,
`tools/ubersdr-rec`); VibeServers Pi 500 192.168.86.88, Lenovo i5 192.168.86.13, Pi 2 192.168.86.129 (ports
48000/48001). Rules: check the listener count first and take a dial only at 0 listeners; restore frequency, mode, gain
and settings afterwards (Pi 2: `agcLock` back to 1 if borrowed); recording and tuning ONLY — no install, update,
restart or reconfiguration; no builds on the Lenovo; never adb keys into the Sony/XCover UIs; recordings stay local and
out of git (paging captures: decode-test only, never commit message content). Every session is logged below.

#### Server session log

| UTC | Server | Frequency / mode | Listeners at start | Restored |
|---|---|---|---|---|


---

## 1. Q65 — DONE (2026-10-31)

Files: `decoders/digimodes/q65_code.{h,cpp}` (frame, code, BP), `q65_decoder.{h,cpp}` (receiver), `q65_simd.h`,
`q65.h` (the Digital-spots mode), `digi_bands.h` ("Q65" rows), `tools/q65-bench/` (bench, synth, `qemu.sh`),
`vibeserver/test-q65.cpp` (in `scripts/run-tests.sh`).

### How the protocol was learnt — clean room
No WSJT-X source was read. WSJT-X 2.7.0's arm64 .deb was downloaded into its own directory (`~/wsjtx-blackbox/`, not in
git) and its shipped utilities run as black boxes in a Debian container:
- `q65code "<message>"` prints the 90 bits (78 message + 12 CRC), the 65-symbol codeword and the 85 tones. 265 messages
  (160 random 71-bit telemetry words — near-arbitrary bit patterns —, 60 standard QSO messages, 30 free texts, 15
  others) were run through it.
- From those, by linear algebra over GF(2) (scratch Python, not shipped): bit 78 is always 0 (77-bit payload + 0); the
  CRC-12 is affine with constant 0, has linear complexity 12 only when each six-bit symbol is fed LSB first, and
  brute force then gives polynomial 0x80F (reflected register 0xF01; low six bits → info symbol 13, high → 14); the
  65-symbol codeword is systematic (15 info) + 50 parity, and consecutive parity differences each depend on ONE info
  symbol through a 6×6 GF(2) matrix that is multiplication by a field element in GF(64) with x⁶+x+1 — i.e. a
  repeat-accumulate code; the 50 (info index, weight) steps are the table in `q65_code.cpp`. Channel: tone 0 at the 22
  positions where every vector has 0; data tone = codeword symbol + 1 with the two CRC symbols punctured.
- All 265 black-box codewords and tone sequences reproduce exactly (test-q65 pins 17).
- `q65sim` audio (noise-free) measured: samples per symbol 1800/3600/7200/16000/41472 for 15/30/60/120/300 s; start
  0.5 s (15/30 s) or 1.0 s (60 s+) into the period; tone spacing = baud × 2^submode (A–E).
- jt9 was run only as a black box to COMPARE decodes on the same files.
- ft8_lib (MIT) packs/unpacks the 77-bit payload. It agrees with q65code on standard messages; it does NOT pack hex
  telemetry from text and packs a few contest types and "RR73" differently (q65code sends RR73 as the GRID RR73) —
  irrelevant to receiving, since we only unpack.

### The receiver
Search spectrogram (half-baud bins, quarter-symbol hop, per-bin median noise) → sync contrast (sync minus data
positions at tone 0, so carriers score 0) → candidates → per candidate one slice of the window's single big FFT back
to a baseband at exactly 72·2^m samples a symbol → two-stage start/frequency refinement on the 22 sync symbols (once,
shared by all submodes) → per-symbol DFT energies, per-bin carrier normalisation → AWGN metric ln I0(2√(ρE)) ×1.5 →
belief propagation over GF(64) in the Walsh-Hadamard domain, forward-backward along the accumulator chain, 50
iterations with an early give-up → CRC-12 → guards. Submodes ranked by tone concentration; BP on the best two; "CQ ? ?"
a-priori on the best one. Engine: 30 s and 60 s periods, A–D, audio 200–4800 Hz; one SlotDecoder per period shared by
every engine in the process.

### Numbers
| | |
|---|---|
| Sensitivity, blind, 50 % (synthetic AWGN, 60 trials/point) | 15A ≈ −21.6 · 30A ≈ −24.6 · 60A ≈ −27.5 dB (2500 Hz); 60B/C/D within ≈ 0.5 dB of 60A; all four submodes searched at once costs ≈ 0.3 dB |
| vs jt9 (black box, same q65sim files, 60A, 20 files/point, jt9 given unrelated MyCall/DxCall so no AP) | −25: jt9 20/20, ours 20/20 · −26: 20/19 · −27: 12/10 · −28: 0/0. (jt9 with its default calls decodes all to −29 by AP "q3" — it is being told the calls) |
| False decodes on noise (all guards) | 15 s × 3000, 30 s × 1500, 60 s × 1000 windows (≈ 42 h of periods): **0** |
| Guard calibration | guard off at threshold: 21 blind CRC-passing wrong decodes (max score 2.59, next 1.76) vs true ≥ 2.89; a-priori wrong ≤ 1.65 (one 2.32 later on noise) vs true ≥ 2.37 → blind 2.75; a-priori 2.6 + CQ with a real grid; non-standard messages +0.5; a codeword that is > 12/63 one tone (a carrier) rejected |
| CPU Mac (M-series, noise, worst case) | 60 s window 42 ms, 30 s 34 ms → ≈ 110 ms/min (FT8 ≈ 42 ms/min) → costVsFt8 2.6 |
| CPU Pi 2 (qemu-arm count, `tools/q65-bench/qemu.sh`) | 60 s window 0.84 G instr NEON / 1.89 G scalar; 30 s 0.68 G / 1.78 G → ≈ 2.2 G/min → ≈ 3–4 s of a 900 MHz core per minute (≈ 5–7 %), ESTIMATE at 0.6–0.8 IPC |
| SIMD | NEON = scalar decodes bit for bit (Mac: sweep + noise); SSE2 = scalar bit for bit (x86-64 build under qemu, 33 decodes + noise). Hot loops vectorised: WHT64 (register-resident), BP products/normalise, sync search row sums, complex dot/mix. Not vectorised (does not pay): the GF(64) permutation gathers, the per-symbol exp/log metric (4 k per attempt) |
| 70 cm dials | 432.055 was dropped (FST4 commit): with it the Q65 cluster was too wide for a narrow capture to hold beside FT8 (test-band-slice); 432.065 + 432.087 remain |
| Memory | per engine: 3.4 MB ring + transient window copy; shared: ≈ 7 MB (30 s) + 13 MB (60 s) process-wide |

Optimisation history (60 s noise window, Mac): 222 ms → 140 (submode ranking) → 83 (a-priori on the best submode,
50 iterations) → 70 (refine shared) → 42 ms (BP early give-up). Pi 2 count 3.4 G → 0.84 G instructions.

### Clients
Server (Single + Band, governor priority 100, cost 2.6/1.2), web and app: spots arrive as any mode does; the map
colour/letter (blue Q) and the family filter ("Q65-60A" → Q65) were already in `src/utils/digiModes.ts`. The mode is
offered only on bands the radio reaches (`reachableSpans`). About: V12 release note updated. Android local hardware:
same engine (the native lib globs `digimodes/*.cpp`) — not compile-checked with gradle this session.

### Owed
- **Real signals.** Not validated on air: there is no Q65 on HF, and none of Stuart's servers has a 2 m / 70 cm EME
  antenna. PSKReporter shows Q65 almost only on 1296 MHz EME (this weekend) plus 432/144. A real-air capture is owed.
- Q65's averaging over successive periods (EME practice), Doppler-spread energy summing (K > 1 bins) and drift
  tracking; a heard-calls AP book like FT8's (`FtxApBook`).
- 15 s and 120/300 s periods are built and tested but not run by the engine (cost; outside our bands).
- Android gradle and iOS `build_ios.sh` compile not run.

### Questions for Stuart
1. 23 cm: most live Q65 is 1296.05–1296.12 MHz EME. Add a 23 cm band to Band mode's list (it would carry only Q65)?
2. 6 m: the IARU R1 EME centre 50.310–50.320 overlaps FT8's 50.313 and no Q65 was heard there today. Add 50.310 anyway?
3. Run Q65-15 too (≈ +40 % cost)?

---

## 2. FST4 and FST4W — DONE, with a stated sensitivity gap (2026-10-31)

Files: `decoders/digimodes/fst4_code.{h,cpp}`, `fst4_tables.h` (generated), `fst4_decoder.{h,cpp}`, `fst4.h`,
`fst4w.h`, `digi_bands.h` ("FST4" rows; FST4W uses its existing WSPR-dial rows), `tools/fst4-bench/`,
`vibeserver/test-fst4.cpp`.

### Clean room
`fst4sim` (WSJT-X 2.7.0, black box) prints a message's bits, its CRC-24 and its 160 tones. 177 FST4 + 236 FST4W
messages (telemetry words for arbitrary bit patterns; random calls/grids/powers; compound calls) were solved:
- FST4's printed 77 bits = the FT8 payload XOR FT4's published scrambling sequence (constant XOR across all 130
  telemetry words); CRC-24 = 0x100065B, register 0, MSB first over those 77 bits, no padding (all 134 printed CRCs);
- FST4W: 50 bits = FT8 c28 (call) + g15 (grid) + 5-bit power index (0,3,7…60 dBm → 0…18) + 2 type bits; not scrambled;
  same CRC. Compound calls (type ≠ 00) carry the base call's c28 and an unsolved field → decoded, **not unpacked**;
- systematic, Gray (00→0, 01→1, 11→2, 10→3), sync 5 × 8 at 0/38/76/114/152 (A B A B A);
- the LDPC parity checks: sparse vectors of the dual of the observed codewords by an information-set search
  (scratch Python): (240,101) 139 checks weight 5–6, (240,74) 166 checks weight 4–5, every column weight 3. All 413
  black-box vectors reproduce; BP on BPSK/AWGN gives 50 % FER at Eb/N0 ≈ 1.1 dB (a healthy code).
- Timing from the audio: nsps 720/1680/3888/8200/21504/66560/134400; start 0.5 s (15 s) or 1.0 s.

### Receiver
Per (mode, period) stream: vibedsp NCO + FIR (shared by all periods of a mode) then a polyphase resampler to a
baseband with an exact FFT-friendly samples-per-symbol (FST4 ≈ 2.65 kHz, FST4W ≈ 260–280 Hz) → STFT sync search (SIMD
over bins) → refine → coherent fine frequency on the sync groups → five detection views tried in turn: one global
carrier phase (all 40 sync symbols), piecewise phase between sync groups, coherent 4-, 2- and 1-symbol blocks →
sum-product BP → CRC-24 → guards (one-tone codeword, mean tone energy ≥ 1.6, plausible message; FST4W: a real call
and grid). FST4W tries its (240,74) code then (240,101) (hashed-call messages use the 77-bit form).

### Numbers
| | |
|---|---|
| Sensitivity, 50 % (synthetic GFSK BT=1, AWGN) | FST4-15 ≈ −18.7 · FST4-60 ≈ −26.1 · FST4W-120 ≈ −29.6 dB (2500 Hz) |
| vs jt9 (black box, fst4sim FST4-60 files, 16 distinct per point, no AP calls) | −24: 16/16 both · −25: jt9 16, ours 6 · −26: 16 / 2 · −27: 15 / 0 · −28: 6 / 0 → **jt9 ≈ 2–2.5 dB better** |
| Ideal detection (our BP, perfect phase, simulated symbols) | coherent 50 % at −27.2 dB, single-symbol non-coherent −24.7 — so our receiver loses ≈ 1 dB to ideal coherent, and jt9 has ≈ 1 dB more (OSD or a better detector) |
| False decodes on noise | FST4-15 × 1000, FST4-60 × 300, FST4W-120 × 200 windows: **0** |
| CPU Mac | FST4 engine (60/120/300) ≈ 36 ms a minute (FT8 ≈ 42) → costVsFt8 0.9; FST4W (120/300/900/1800) ≈ 10 ms a minute → 0.25 |
| CPU Pi 2 | NOT MEASURED under qemu yet; at Q65's measured Mac→Pi 2 ratio (≈ 30–35×) ≈ 1.1–1.3 s/min FST4, ≈ 0.3 s/min FST4W (ESTIMATE) |
| SIMD | sync search, complex dot products (q65_simd.h), vibedsp's NEON/SSE2 NCO/FIR/resampler. Not vectorised yet: the LDPC BP (scalar sum-product with tanh/log) — it is small here (few candidates per long period) but should get the FT8 BP kernels' treatment; scalar-vs-SIMD identity not yet checked for FST4 |

### Clients
Server Single (FST4 window 250–1550 Hz on 474.2/136.0; FST4W ±150 Hz of WSPR's dials) and Band (+ the background Band
profile, so FST4W runs beside WSPR there). Spots `FST4-60` … / `FST4W-120` …, colour/letter already in digiModes.ts.
About updated. FST4 window was narrowed to 1.55 kHz so 2200 m's 136.0 dial stays inside the band (137.8).

### Owed
- The ≈ 2 dB gap to jt9: OSD after BP (as ft8_decoder's), a GFSK-matched multi-symbol detector, and a joint
  phase/frequency track (drift on LF paths). Validate on REAL LF/MF/HF FST4W (PSKReporter shows FST4W on 136/475 kHz
  and 3.570/7.040 MHz; Stuart's UberSDR reaches all of them — record a night of 630 m and 40 m WSPR/FST4W windows).
- FST4W compound-call field layout (more black-box runs with systematic prefixes/suffixes).
- qemu Pi 2 count and SIMD-vs-scalar identity check for FST4 (as Q65's `tools/q65-bench/qemu.sh`).

---

## 3. MSK144 — DONE (2026-10-31)

Files: `decoders/digimodes/msk144_code.{h,cpp}` (frame, CRC, code), `msk144_tables.h` (generated checks),
`msk144_decoder.{h,cpp}` (receiver), `msk144.h` (the Digital-spots mode), `ldpc_small.{h,cpp}` + `vmath4.h` (a shared
small-LDPC BP + OSD, SIMD — FST4 moves onto it next), `digi_bands.h` ("MSK144" rows), `tools/msk144-bench/` (bench,
synth, `qemu.sh`), `vibeserver/test-msk144.cpp` (in `scripts/run-tests.sh`).

### Clean room
No WSJT-X source read. `msk144code` (WSJT-X 2.7.0, black box, Colima container) prints a message's 144 "channel
symbols". 400 messages (272 telemetry words: 200 random, the 71 single-bit words, zero; 128 standard / compound / free
text) → solved in scratch Python (not shipped):
- the printed symbols are TONES (1 = upper). A tone sequence synthesised as CPFSK ±500 Hz around 1500 Hz decodes in jt9
  only with 1 = +500 Hz — that settled the polarity (jt9 used only to compare decodes);
- tone i = C[i] ⊕ C[i+1 mod 144], C = frame ⊕ 0011 0011 … (period 4). Equivalently offset-QPSK with half-sine pulses:
  each frame bit is the sign of one matched-filter sample, on I (even bits) or Q (odd), against ONE carrier phase for
  the whole transmission (verified: 144/144 bits at the ideal instant on a clean synthetic frame);
- frame: sync 0x72 at bits 0–7 and 56–63; codeword bits 8–55 and 64–143; systematic: the 77-bit FT8-family payload
  (unscrambled), CRC-13, 38 parity;
- CRC-13 = 0x15D7 (x¹³+x¹²+x¹⁰+x⁸+x⁷+x⁶+x⁴+x²+x+1), register 0, MSB first over 77 bits + 6 zeros: all 400;
- the (128,90) LDPC: the dual of the observed codewords is 51-dimensional (13 CRC + 38 LDPC); an information-set
  search for sparse dual vectors found EXACTLY 38, weight 10–11, rank 38, every column weight 3. All 400 re-encode.
- ft8_lib packs a few messages differently from WSJT-X (free text that looks like a call, "RR73", two compound CQs):
  22 of the 398 tone sequences differ for that reason only — irrelevant to receiving (we unpack).
- MSK40 (short hashed-call messages) not decoded: no callsign for a spot.

### The receiver (free-running)
12 kHz audio → NCO at 1500 Hz + FIR ÷2 → 6 kHz complex (3 samples a bit) → 5-tap half-sine matched filter. Every 1 s
an analysis of the last 1.5 s: for 9 coarse offsets (±200 Hz, 50 Hz steps; rotation by a table, SIMD) the 8-bit sync
correlation at every sample (adds/subtracts only, SIMD four starts at a time); frame score = both words' power over
the noise (the matched filter's 20th-percentile power, median of 8 windows). Folded at the frame period: runs of 1, 2,
4, 8, 16 frames at each phase against Gamma tails (false alarm 1e-5 a trial) — SIMD four phases at a time with a
vmax screen, so the exact tail is computed only for groups that cross. Top 6 candidates → timing to a fraction of a
sample → frequency from the FFT of the run's SQUARED bits (data-free; peak at 2Δf; ±75 Hz in one FFT, parabola) →
each bit weighted by the ping's ENVELOPE at its instant (maximal-ratio combining in time; a neighbour frame each side,
so a ping's ragged ends count) and summed coherently over the frames → LLRs → BP (40 iterations, early give-up) →
CRC-13 → plausible message → guards (≤ 45 hard errors of 144, agreement ≥ 0.3). Second detector: each frame its own
phase (a drifting ping). The two sync words are identical: a ping holding one word of each of two frames is tried at
both readings (start ±168 samples). One report per message per 30 s.

**OSD is OFF.** With only a CRC-13 the trials of OSD cannot be vetted: in a 240-ping sweep 19 of 31 OSD decodes were
wrong (plausible-looking calls). BP alone: 0 false in 48 h of noise (429 k BP attempts) and 0 wrong in every sweep.

### Numbers
| | |
|---|---|
| Sensitivity, 50 % (synthetic CPFSK, AWGN, 40 trials/point, random frame offset and ±50 Hz) | one 72 ms frame ≈ +1.3 dB · 4 frames ≈ −5.3 · a 1 s ping (14 frames) ≈ −8.2 dB (2500 Hz) |
| vs jt9 (black box, `jt9 -k -p 15 -F 100`, the SAME 180 files, 10/point) | 1 frame +3: jt9 8, ours 10 · 0: 1 / 1 · 4 frames −3: 8 / 9 · −6: 0 / 1 · 1 s −6: **0 / 8** · −9: **0 / 5** → equal on single frames, ≈ 1.5 dB better on 4 frames, ≈ 3–4 dB better on long pings (coherent combining over the ping) |
| False decodes | white noise 48 h (BP only): **0**; 8 h after the SIMD fold: 0; every sweep: 0 wrong |
| SNR | calibrated +2.3 dB; within ±1 dB on short pings; a ping filling a whole window reads low (its own power in the noise floor) |
| Frequency | ≤ 2 Hz on one frame, ≤ 0.35 Hz on a 1 s ping |
| CPU Mac (M-series, whole receiver on noise) | 15.7 ms a minute (search + decodes 10 ms; mix + FIR 5.6 ms) → costVsFt8 ≈ 0.4 (ModeDef says 0.8 — margin for busy pings; refine after real-air use) |
| CPU Pi 2 (qemu-arm count, `tools/msk144-bench/qemu.sh cpu 60`) | NEON 0.58 G instructions a minute of audio (the noise generator excluded), scalar 1.2 G → ≈ 0.8–1.1 s of a 900 MHz core a minute (≈ 1.5 %), ESTIMATE at 0.6–0.8 IPC |
| SIMD | NEON = SSE2 = scalar by construction (q65_simd lanes, our own exp/log); Mac SIMD vs `SCALAR=1` builds: the 180 jt9 files and a sweep **bit-identical**. Vectorised: rotation, sync correlation, fold screen, BP check update (vmath4.h). GCC armhf compares: `vmath4.h`'s log uses one vcgtq — route through `neon_compat.h`'s fix when v12-decoder-simd-audit merges |
| Memory | ≈ 1 MB per engine (window, 9 score arrays, FFT buffers) |

### Clients
Server Single + Band (free-running, gate 2 s, priority 100), web and app: spots `MSK144`, grey M (digiModes.ts had it).
About: V12 note. DIGITAL-SPOTS.md row. Dials: digi_bands.h note (IARU R1 VHF Handbook 10.03 — downloaded and read
2026-10-31: 6 m 50.320–380 MS centre + the plan's accepted legacy MGM below 50.300 where activity is (50.280); 4 m
70.250 MS centre; 2 m MS MGM range 144.360–144.397; 70 cm 432.370 MS centre). test-band-slice / test-digi-spots pass.

### Owed
- **Real meteor pings.** Not validated on air: no server session was used (it was ≈ 04 UTC, few MSK144 operators
  on, and no server was checked for a 6 m / 2 m antenna). A real capture on 50.280 at a morning peak, or during a
  shower (Geminids 13–14 Dec), is owed.
- The gate: a short weak lone ping does not light Band's 2 s activity gate. Measure with real pings; MSK144 may
  want gate-off by default during showers (Stuart to decide).
- MSK40 short messages; AP decoding with the operator's own call (we have none); averaging repeated pings of the
  same message across periods (WSJT-X-style message averaging).
- Android gradle / iOS `build_ios.sh` not compiled this session.

### Questions for Stuart
1. MSK144 dials: 50.280 (where it is) and 50.360 (where R1 put it) both run — keep both? 70.250 vs 70.230 on 4 m?
2. Gate default for MSK144: Auto (now) or Always?

---

## 2b. FST4 / FST4W — the gap to jt9 closed by about half (2026-10-31)

What changed (`fst4_decoder.cpp`, `fst4_code.{h,cpp}`, `ldpc_small.*`):
- **BP on the shared SIMD decoder** (`ldpc_small`: four checks a lane, our own exp/log, identical scalar); the old
  scalar sum-product removed.
- **Decision-directed frequency.** The oracle (the bench's sent tones, `FST4_VDBG`) showed that at −27 dB HALF the
  trials had every coherent view wrong (≈ 120 of 240 bits) while the 4-symbol non-coherent view was ≈ 80 % right: the
  40 sync symbols alone pin a wrong frequency (grating lobes 1/38 baud apart, or noise). Now the 4-symbol view's tone
  decisions + the sync make a 160-symbol reference, |Σ X_i[t̂_i]e^{−j2πδi}|² over ±1/16 baud (1/4096 steps,
  parabola), and the coherent views are rebuilt there — then once more from the best coherent view's decisions. The
  sync-only search now covers ±1/32 baud and also tries its second lobe.
- **A GFSK-matched view**: a coherent MAP (forward-backward) detector over (previous, this) tone states with the 64
  exact BT = 1 waveforms of each tone triple, integrated over 32 sub-blocks a symbol — global and piecewise phase.
  Measured: a few bits better than the one-symbol coherent view, not a large gain (the coherent views were already
  at the ideal coherent 4-FSK error rate once the frequency is right: ≈ 35 of 240 bits at −26 dB, theory 37).
- **OSD**: order 1 + every pair of the 101 (or 74) information positions, up to 3000 trials, on the three views BP came
  closest with, ORDERED BY BP'S ACCUMULATED POSTERIORS (the iterations' evidence summed — this, not the channel LLRs,
  is what made OSD pay: −27 dB 4 → 9 of 20). The CRC-24 vets the trials; the message must unpack as a message people
  send / a real FST4W call + grid; then the energy and one-tone guards.
- Views are made lazily (each tried before the next is made).

| | before | now |
|---|---|---|
| FST4-60, our synth, 50 % | −26.1 | ≈ −26.7 dB |
| FST4-15, 50 % | −18.7 | ≈ −19.5 |
| FST4W-120, 50 % | −29.6 | ≈ −30.1 |
| fst4sim files (20 per point, same files for both) | −25: ours 15, jt9 20 · −26: 10 / 20 · −27: 4 / 19 | **−25: 19 / 20 (jt9 20) · −26: 15 / 20 · −27: 9 / 19 · −28: 1–2 / 9** |
| gap to jt9 at 50 % | ≈ 2–2.5 dB | **≈ 1 dB** |
| False decodes | — | FST4-15 × 6000, FST4-60 × 400, FST4W-120 × 200 noise windows: **0** |
| SIMD vs scalar | not checked | `SCALAR=1` build: 40 fst4sim files and a FST4W sweep **bit-identical** |
| Pi 2 (qemu-arm count, `tools/fst4-bench/qemu.sh`) | not measured | one FST4-60 window of noise incl. its whole stream ≈ 0.6 G instructions NEON (scalar ≈ 0.95 G) → FST4 (60/120/300 sharing the mix) ≈ 0.7–0.9 G a minute ≈ 1–1.5 s of a 900 MHz core (ESTIMATE). Hot: the FIR/resampler (vibedsp, NEON), OSD on noise candidates (≈ 12 %), the double-precision twiddles (`sincos`, 8 %: a recurrence would remove most — owed) |
| Mac decode (noise, worst case) | 10.4 ms / FST4-60 window | 11.7 ms |

Still owed: the last ≈ 1 dB (candidates: order-3 OSD restricted to the least reliable, iterating BP ↔ detector,
non-coherent GFSK-matched views for fading paths); real LF/MF/HF FST4W (a night of 630 m / 40 m on the UberSDR);
twiddle recurrence.

---

## 4. CTCSS / DCS tone ID — DONE, wired to server, app and web (2026-10-31)

Files: `decoders/tone_id.{h,cpp}` (outside RxPipeline), `digimodes/vmath4.h` (+ branch-free `vatan2`, `signBit01`),
`vibeserver/test-tone-id.cpp` (in `scripts/run-tests.sh`).

- **Own discriminator** on the listener's CHANNEL IQ (`feedIq`; or `feedDisc` for a raw discriminator): arg(x·x*₋₁)
  with our vector atan2 (4.8e-6 rad, no float compares — sign-bit selects, so GCC armhf's compare problem cannot
  bite), then CIC³ to ≈ 2 kHz, a 63-tap 300 Hz low-pass ÷2 → ≈ 1 kHz.
- **CTCSS**: 2 s Hann FFT every 0.5 s; peak in 60–260 Hz ≥ 15 dB over the band median AND ≥ 10 dB over the band's
  second peak (a DCS word's line spectrum fails that), log-parabola frequency within ±0.6 % (and < 0.45 of the gap to
  the neighbour) of one of the 51 tones; three agreeing looks lock (≈ 3.5 s from cold), five misses unlock.
- **DCS**: 134.4 bit/s; best of 16 bit phases; ≈ 2 s folded at the 23-bit period (fold consistency ≥ 0.7);
  every cyclic shift × both bit orders × both polarities against Golay (23,12). The generator and frame layout were
  PINNED by the published inverted-code equivalences: 12/12 pairs (023N = 047I, 025 = 244, 026 = 464, 031 = 627,
  032 = 051, 043 = 445, 054 = 413, 065 = 271, 071 = 306, 072 = 245, 073 = 506, 074 = 174) reproduced only by
  0xC75 with the 9-bit code under "100" (of 16 polynomial/layout/order combinations). The N/I polarity convention is
  not pinned by that (an inverted code IS another code's normal): the label says both, "DCS 023N (= 047I)".
- Measured (synthetic NFM at 24 kHz, ±600 Hz sub-audible under ±3 kHz voice-band audio): **51/51 CTCSS tones right**
  (±0.4 % transmitter error, 15 dB CNR, lock ≤ 3.5 s), 151.4 beside 150.0 at 10 dB CNR; **32/32 DCS** (16 codes N + I);
  weak: CTCSS and DCS 10/10 down to 3 dB CNR (16 kHz), 5/10 and 4/10 at 0 dB. **0 locks** in 10 min of noise, 5 min of
  voice with no tone; DCS never read as CTCSS. Cost 0.13 ms a second of 24 kHz IQ on the Mac (≈ 0.013 %).
  SIMD = scalar: the whole test's output identical with `-DVIBE_FORCE_SCALAR`.

### The wiring (done, second run of 2026-10-31)
- **Where it is fed — one change from the plan above.** Not the raw slice beside `iqTap`: that slice is ≥ 2.5 × the
  passband (≈ 31–62 kHz for a 12.5 kHz NFM channel), so a neighbouring channel inside it would capture the
  discriminator, and it sits up to half a bin off the dial. Instead the PIPELINE's channel-IQ callback, which already
  exists for DRM (`clientIqCb`, `cb_.iq` in `demodTail_`): post-NCO, the dial at DC, cut to the passband, at `chFs_`.
  Per listener (`ClientDsp::tone`, `toneFeedClient`): built on the first block in NFM, rebuilt when the channel rate
  changes, `reset()` when the dial moved (`toneAtHz` set in `clientRetune`), destroyed off NFM. Shared dial
  (`toneFeedShared` from `iqCb`): one ToneId, the dial read from `audioFreq`, `toneSharedWant` set in `buildAudio`; not
  on a per-listener radio. All on the DSP thread under the locks those callbacks already hold (c->mtx, then clientMtx
  — the order `onClientAudio` uses).
- **Message**: `{"type":"tone","label":"CTCSS 88.5 Hz"}` / `"DCS 023N (= 047I)"` / `""` (cleared), on change only, to
  the listener's spectrum socket (shared dial: every spectrum socket, and a listener arriving gets the current one in
  `sendFullState`).
- **App** (`VibeServerClient` `tone` → `onToneId`, SDRScreen `toneLabel`): the VTS line's quiet suffix in the MESSAGE
  slot — "GB3NB: CTCSS 82.5 Hz" (text colour, not the name's) after a bookmark on the dial, or the label alone; held
  while locked; NFM only (dropped at once on any other mode). **Web** (`spectrum.ts` `tone`, `main.ts` `toneLabel` in
  `updateVts`): the same sentence through `vtsLine.ts` line mode. ⚠ Web edits need build-web (`vibe_web_page.h`).
- **Copy**: About V12 note ("CTCSS AND DCS ON NFM"). Not a control, so no tour card; the website has no NFM text.
- **Tests**: `test-tone-id` + all 51 tones / 32 DCS at seven channel rates (12–93.75 kHz: 14/14) and a `--write-clip`
  for the new end-to-end `scripts/test-server-tone.mjs` (the REAL server, fake rtl_tcp, three NFM carriers at 2 m):
  shared dial — the right label on each carrier, '' first on a retune, nothing on a toneless carrier for 10 s, cleared
  leaving NFM, a joiner told at once; range-locked — two listeners each their own, never the other's. **13/13**.
  Cost on the Mac (optimised): ≈ 0.13 ms per second of 24 kHz IQ (`-O0` in run-tests prints ≈ 1.5).
- Not compiled: Android gradle, iOS `build_ios.sh` (the shim is not in the iOS lib). Full app `tsc` exceeds the 1 GB
  memory cap — syntax-checked the edited files and type-checked `VibeServerClient.ts` alone instead.

### Still owed
- **Real signals**: a 2 m / 70 cm repeater with a known tone (UK repeaters publish theirs) and a PMR446 DCS user;
  confirm the N/I convention on one known DCS radio.

---

## 5. Time stations — JJY DONE; BPC, TDF, CHU checked and NOT built (2026-10-31)

**JJY (40 / 60 kHz, NICT)** in `decoders/time_decoder.*` as `Station::JJY`: the format from NICT's published page
(signal levels, markers, field order, parity, JST meaning) and the second map as Wikipedia's "JJY" tabulates it.
It is WWVB's layout (minute, hour, day-of-year at the same seconds, markers 0/9/19…59, the same flywheel and
framing) with the OPPOSITE envelope (full carrier 0.8 / 0.5 / 0.2 s from the second's start, then 10 %), so it is
read inverted as WWV is. Checks WWVB never gets: PA1 (hour) and PA2 (minute) even parity, and the weekday must be the
date's. :15 / :45 (callsign minutes): seconds 40–48 are Morse, not symbols — not asked for; the year then comes from
the host clock. Time shown as JST. Wired: decoder host (`jjy`), shim key, app (`TimeStation`, station override row,
auto-pick: 40 kHz always, 60 kHz east of 100°E), web (button + hint), known-signal labels (40 kHz "JJY", 60 kHz
"MSF/WWVB/JJY"), the "tune to a time signal" message, About. test-time-decoder: decodes, +1 minute, date, weekday,
parity catches a flipped hour bit, the :15 minute. **Not validated on air** (JJY is not receivable in the UK).
⚠ The web page is embedded in the server as `vibe_web_page.h`: run build-web before any release (the gates).

Checked, not built:
- **BPC (68.5 kHz, Shangqiu)**: no official public format from NTSC — what exists is hobbyist reverse engineering.
  "Published standard first" rules it out unless Stuart accepts a documented-by-listeners format.
- **TDF / ALS162 (162 kHz)**: the time code is carrier PHASE modulation (±1 rad), needing a carrier PLL — the
  synchronous-AM work on branch v12-am-sync. Build it there, after that merges.
- **CHU (3.330 / 7.850 / 14.670 MHz, NRC)**: published (NRC's CHU broadcast format); Bell-103 300 bit/s FSK
  (2225/2025 Hz) in seconds 31–39 of each minute, two 10-character frames (A: day/hour/min/sec, B: year, DUT1, DST,
  leap, with a redundancy check). Needs a burst FSK demodulator beside the envelope reader — a day's job; next in
  this queue when it is picked up again. Its known-signal labels already exist (no decoder).
  **→ NOT BUILT: CHU IS OFF THE AIR (checked 2026-10-31, second run).** NRC ended the shortwave broadcast on
  **22 June 2026** (NRC's own page: "As of June 22, 2026, the shortwave radio broadcast of the NRC's official time
  signal will no longer be available"; recordings of the final minutes exist). A decoder for a station that no longer
  transmits cannot be tested on air and would show nothing to anyone, so it was dropped from the queue. ★ Question
  for Stuart: the CHU known-signal labels (3.330 / 7.850 / 14.670 MHz) now name a silent channel — remove them?

---

## 6. ACARS → VDL2 → HFDL → RS41 — BLOCKED this run, nothing built (2026-10-31, second run)

- **No primary standard to hand.** ACARS (ARINC 618/620), HFDL (ARINC 635) and VDL2 (ICAO VDL Mode 2 Technical
  Manual, Doc 9776) are published but sold, not free; RS41 is Vaisala's and unpublished (only listener
  reverse-engineering). Free public descriptions of ACARS (MSK 2400 bit/s on AM, 1200/2400 Hz, NRZI, "+*", SYN SYN,
  SOH, 7-bit + odd parity LSB first, a 16-bit BCS) disagree on the pre-key and do not name the BCS polynomial with
  authority.
- **So a real capture was to settle it**: 5 minutes of AM audio on 131.725 MHz from the Lenovo (192.168.86.13, an open
  range to 1.766 GHz, 0 listeners, at 6.791 MHz CWU ±200 Hz at the time; a recorder written to restore that). **The
  tuning step was refused by this session's permission layer** ("modify shared resources"), so **no server session
  took place** and the dial was never moved. Not worked around.
- Without either a standard or a real signal, an ACARS decoder would only be checked against its own encoder — the same
  misunderstanding on both sides passes. Not built.
- ★ Needs from Stuart: (a) OK for the agent to tune a 0-listener VibeServer to 131.725 MHz AM for 5 min (the recorder is
  `tools`-free, scratch only: listener check, restore of frequency/mode/passband) — or a capture he records himself
  (AM audio ≥ 24 kHz, or IQ); (b) whether buying ARINC 618 / ICAO Doc 9776 is wanted, or listener documentation is
  acceptable as the "standard" for ACARS / VDL2 / HFDL / RS41.

## 7. PAGING — POCSAG engine DONE (AUTO 512/1200/2400 + manual); FLEX blocked; NOT wired (2026-10-31, second run)

Files: `decoders/paging_decoder.{h,cpp}` (Android CMake only — deliberately NOT in the server build), `vibeserver/test-paging.cpp`
(run-tests), `tools/paging-bench/` (bench + `qemu.sh`).

- **Standard**: ITU-R M.584-2 Annex 1 ("Radio-paging code No. 1 … sometimes referred to as POCSAG"), downloaded from
  the ITU and read (the scanned table 3 page rendered and read by eye): preamble ≥ 576 reversals; batch = sync + 8
  frames × 2 codewords; codewords MSB first; address = flag 0, 18 address bits (the 3 LSBs of the 21-bit identity are
  the frame), 2 function bits; message = flag 1 + 20 bits; BCH(31,21) generator x¹⁰+x⁹+x⁸+x⁶+x⁵+x³+1 + even parity;
  numeric 4-bit BCD LSB first, 1010 spare (shown *), 1011 U, 1100 space, 1101 hyphen, 1110 "]", 1111 "["; function
  00 numeric, 11 alphanumeric (IA5, 7 bits LSB first, packed across words). Deviation and which shift is "1" are NOT in
  M.584 → the sync is searched in both polarities.
- **Pinned against the ITU's own tables**: the Table 1 sync (0x7CD215D8) and Table 2 idle (0x7A89C197) words are clean
  codewords under our generator and parity rule — two independent 32-bit checks.
- **Receiver**: per rate a DC tracker (fast hunting, slow locked), a one-pole low-pass for crossings (sub-sample
  crossing time), a SECOND-ORDER DPLL (the first-order loop left a 0.25-bit standing error at 0.5 % clock error and
  lost lock — measured, fixed), integrate-and-dump over each bit's exact time (each sample's interval split at the bit
  edge — 5 samples a bit at 2400 on 12 kHz), sync within 2 bits either polarity, 2-bit BCH correction, next sync
  expected 544 bits on (≤ 4 bits). Noise guard: a batch counts only if ≥ 12 of 16 words are clean or one bit out; an
  address must be clean or one bit out; an address whose message never came (lock lost) is not a page.
- **AUTO**: 512, 1200, 2400 side by side; `detected()` = "POCSAG 1200" while a batch lock holds; each page carries its
  rate. Manual: one rate. A rate needs the input at ≥ 4 × it.

| | |
|---|---|
| Decodes (synthetic discriminator, numeric + alpha across batches + tone-only, ±0.5 % clock, DC offset, 18 dB Eb/N0) | every rate × both polarities × 12 and 48 kHz: all pages right; AUTO names the rate; manual at the wrong rate: nothing |
| Sensitivity, 50 % of transmissions with all four pages right (Eb/N0 on the discriminator) | 512 ≈ 9 dB, 1200 ≈ 8 dB at 12 kHz; **2400 ≈ 12 dB at 12 kHz but ≈ 8 dB at 24 kHz** → feed paging at ≥ 24 kHz when wired |
| False pages | 4 h white noise (AUTO): **0**, never "detected"; 10 min of random NRZ at each rate: **0** |
| CPU | Mac ≈ 0.3 ms per second of 12 kHz audio incl. the noise generator; **Pi 2 (qemu-arm count)**: 129 M instructions a minute at 12 kHz, 230 M at 24 kHz → ≈ 0.2–0.4 s of a 900 MHz core a minute (≈ 0.4–0.7 %), ESTIMATE at 0.6–0.8 IPC |
| SIMD | none, by decision: the work is a per-sample feedback loop (DPLL, DC tracker) per rate, nothing to put in lanes, and it is ≈ 0.5 % of a Pi 2. No per-sample divide or round (one float divide per zero crossing) |

**FLEX — blocked.** M.584-2 Annex 2 (FLEX-TD) gives only the outline (1.875 s frames, 1600 bit/s Sync 1 + FI, 11
interleaved 160 ms blocks, 8/16/32 words a block at 1600/3200/6400, the same BCH words, 4-bit checksums); the Sync 1
patterns, frame-information and vector layouts are in ARIB RCR STD-43 / Motorola FLEX-93001 — neither free. The AUTO
structure takes a FLEX receiver unchanged once the source is decided (question for Stuart). The "channel carrying both"
case is therefore POCSAG-only today (each POCSAG transmission re-acquires on its own preamble).

### 7b. PAGING WIRED — Android local hardware only (2026-10-31, third run)

Files: `vibe_paging_run.h` (new), `vibe_decoder_host.h`, `local_sdr_shim.{h,cpp}`, `CMakeLists.txt` (Android),
`decoders/paging_decoder.{h,cpp}` (+ the modes list), `src/services/paging.ts` (new), `DecoderClient.ts`,
`sdrTypes.ts`, `ModeSelector.tsx`, `DecoderPanel.tsx`, `MenuSheet.tsx`, `modeBox.ts`, `decoderBands.ts`,
`modeIcons.ts`, `SDRScreen.tsx` (gate, state, tour), `AboutOverlay.tsx`, `scripts/test_decoder_bands.ts`,
`vibeserver/test-paging.cpp`, `tools/paging-bench/`.

- **Where it can run — three locks, each enough on its own.** (1) Compiled only with `VIBE_PAGING`, which the Android
  CMake defines for the CLIENT app's library and NOT for VibeServer Lite (`VIBE_LITE_APP`) — the desktop/Linux
  VibeServer does not compile the engine at all. (2) `LocalSdrShim::pagingCapable()`: not serving (`g_serveOnLan`
  false — the engine bound to 127.0.0.1, so no LAN, tunnel or proxy client can exist) and a USB radio with our own IQ
  (not rtl_tcp, SpyServer or an Icom). Only then does vibeserver.json carry `"paging":true` + `pagingModes`. (3) The
  attach handler refuses `paging` unless (2) holds AND the decoder socket is loopback; never on VFO B. The app gates
  the key again: Android, `isLocal`, no `localHost`, not `isTcp`, not a SpyServer (`pagingHere`).
- **Fed the CHANNEL IQ, not the audio** (the pipeline's `cb_.iq`: post-NCO, dial at DC, cut to the passband, at the
  channel rate — ≈ 31 kHz for a 12.5 kHz NFM filter, never under 2.5 × the band). Its own discriminator, so the NFM
  Voice filter (300 Hz high-pass), de-emphasis, squelch and the 48 kHz resampler are all out of the path whatever the
  listener chose. The DSP thread only copies (`DecoderRouter::feedChannelIq{Session,Shared}` → `DecoderHost::
  feedChannelIq` → its own bounded queue `qc_`, 2 s, oldest dropped); the decode thread runs `PagingRun`. One relaxed
  atomic (`pagingRunning()`) per channel block when nothing pages. Counts as a consumer for the idle gate
  (`sharedWantsAudio`).
- **Never to the gallery**: no tap is made for `paging` (`start()`); not in background decoding; status JSON not in
  the gap log (the page lines are, in memory, for the phone's own held session).
- **Wire**: 0x01 one line per page `[POCSAG 1200 · 1234567 · F3] text` ("(tone only)", "(cut short)"); 0x03 lock
  0/1; `{"type":"paging_status","data":{mode,detected,pages,rate,short}}` on change (`short` = channel rate < 24 kHz).
- **AUTO, FLEX-ready**: the manual choices are DATA (`PagingDecoder::modes()` → vibeserver.json `pagingModes`, AUTO
  first); the app draws one RATE key per entry. A FLEX receiver joins as more entries + an `Rx` in the engine; the
  client does not change.
- **App**: PAGE key in a new Paging group on the VHF and UHF tabs (pager glyph), RATE row + hint under it, the status
  line ("POCSAG 1200 · 3 pages", the narrow-filter warning). Copy: sdrTour mode card (Android-only clause, built with
  `Platform.OS`), About V12 note ("PAGING ON ANDROID …" — every sentence names Android, so platformCopy drops it on
  iOS). The website lists no decoders: nothing to change. No web client key (the web is never local hardware).

| | |
|---|---|
| Wired path decodes (FM IQ ±4.5 kHz, 24 dB Eb/N0, ±0.4 % clock, channel filter in the synth) | 20 / 25 / 31.25 / 48 kHz × 512 / 1200 / 2400 (1200 inverted): all 4 pages right everywhere, status names the rate, `short` flagged below 24 kHz |
| Sensitivity, IQ path, 12.5 kHz passband, 50 % of transmissions all four pages right | 512 ≈ 15 dB Eb/N0 (≈ 1 dB CNR), 1200 ≈ 14 dB (≈ 4 dB CNR), 2400 ≈ 12 dB (≈ 5 dB CNR) at 31.25 kHz; 2400 ≈ 12 dB at 20 kHz. The limit is the discriminator's FM threshold, not the engine (on clean discriminator audio the engine reaches 8–9 dB) |
| False pages | 15 min of channel noise at 31.25 kHz through the whole run: 0, never detected (plus the engine's 4 h noise / 30 min random NRZ: 0) |
| CPU | Mac ≈ 0.9 ms per second of 31.25 kHz IQ incl. the noise generator. **Pi 2 (qemu-arm count, `tools/paging-bench/qemu.sh 60 31250 iq`)**: NEON 325 M instructions a minute, scalar 421 M → ≈ 0.4–0.6 s of a 900 MHz core a minute (≈ 0.7–1 %), ESTIMATE at 0.6–0.8 IPC |
| SIMD identity | The discriminator is q65v lanes (vatan2: sign-bit selects, no float compares). Mac NEON = scalar bit for bit (whole test output); x86-64 SSE2 = scalar bit for bit (qemu-x86_64); armhf `-marm` NEON = scalar bit for bit with `-ffp-contract=off`; with GCC's default `fast` (and the Android CMake's explicit `-ffp-contract=fast`) the discriminator's low bits differ, but **the decoded lines are identical on every build** (one lines fingerprint across Mac, armhf ×4, x86-64 ×2) |
| Per-sample divide | vatan2's one float divide a sample (VFP VDIV, ≈ 15 cycles on Cortex-A7): 1.9 M a minute ≈ 28 M cycles ≈ 0.05 % of a core; a reciprocal estimate + Newton would break identity with the scalar path. No integer divide, no round. The engine's per-rate loops: per-sample feedback (DPLL, DC tracker), not vectorised (section 7) |

Not done / owed: **real-air test on a phone** (Android gradle build not run — the shim was syntax-checked with and
without `VIBE_PAGING` on the Mac; the app's TS: services type-checked, the TSX files syntax-checked only — full `tsc`
of the components runs out of the 512 MB cap); a tone-filter (non-coherent FSK) detector would beat the
discriminator's threshold at 512/1200 (≈ several dB) — a later improvement; switching the mode to NFM on PAGE is
left to the user (the hint says so).

## 8. EAS / SAME — DONE, wired to server and app (2026-10-31, third run)

Files: `decoders/same_decoder.{h,cpp}`, `vibeserver/test-same.cpp` (run-tests), host ext `same` (`vibe_decoder_host.h`
`buildSameLocked_`), vibeserver.json `"same"`, the owner's switch (`vibe_setup_page.h`), CMake (server, Android) and
`build_ios.sh`'s list; app: `DecoderClient` ('same'), `sdrTypes`, `ModeSelector` (SAME key + hint), `DecoderPanel`,
`decoderBands` (VHF → Broadcast, beside ADV RDS; near 162.400–162.550 MHz), `modeIcons` (broadcast glyph),
`SDRScreen` (capability, tour card), `AboutOverlay` (V12 note). **No web key yet** (owed: decoders.ts / main.ts /
index.html, then build-web).

- **Standard**: 47 CFR § 11.31 (LII's copy of the eCFR text; eCFR itself refused the fetch): AFSK 520.83 bit/s, mark
  2083.3 Hz, space 1562.5 Hz, 7-bit ASCII + a null eighth bit, preamble 16 × 0xAB, `ZCZC-ORG-EEE-PSSCCC…+TTTT-JJJHHMM-
  LLLLLLLL-` three times ≥ 1 s apart, attention signal 853 + 960 Hz, `NNNN` three times; the ORG list, the national and
  state/local event codes (§ 11.31(e)), the FIPS state numbers (§ 11.31(f)), marine areas 57–98. LSB first (the null
  bit last) — pinned: the same header sent MSB first decodes nothing.
- **Receiver**: box-average 48 → 12 kHz; the two tones mixed from exact tables (25 bits = 576 samples, whole cycles of
  both tones), q65v lanes; one-bit boxcar correlators (running sums, re-made from the ring every bit); second-order DPLL
  (leaking while hunting so noise cannot walk its rate to the clamp); acquire on 32 exact preamble bits; bytes read with
  their soft values; three bad bytes in a row end a burst.
- **No checksum in SAME — so two readings must agree.** The bursts are soft-summed bit by bit (a burst that slipped and
  agrees with no other on ≥ 80 % of its characters is left out), and the alert is shown only if two bursts' hard
  characters equal the result, or (three bursts) their character majority does. A lone burst (tuned in mid-alert) is
  NOT shown: measured, its clean-looking bits (mean margin 0.8) still misread digits. One alert per header per 10 min;
  `[SAME] end of message (NNNN)` once per run.
- Line: `[SAME] Tornado Warning (TOR) · National Weather Service · areas 024031 (MD), 024033 (MD) · issued day 304
  13:45 UTC · valid 0 h 30 min · from KLWX/NWS` (synthetic).

| | |
|---|---|
| Format | the § 11.31 header parses field by field; 10 malformed headers (bad ORG, a letter in a location, day 367, minute 60, a 9-character sender, no trailing '-', '-' for '+', hour 24 …) all refused |
| Decodes | three bursts → one alert (heard 3) + one end; a 50 ms drop-out in one burst → still right; 48, 24 and 12 kHz; 20 s of attention signal → nothing |
| Sensitivity (white noise, SNR in 3 kHz, ±1 % clock, ±20 Hz tones, 20 alerts a point) | +2 dB 5/20 · **+4 dB 19/20** · +6 dB and up 20/20; **0 wrong in the whole sweep** |
| False alerts | 2 h white noise: 0 alerts, 0 ends; 20 min of random AFSK bytes at 520.83 bit/s: 0 |
| CPU | Mac ≈ 0.31 ms per second of 48 kHz audio incl. the noise generator. **Pi 2 (qemu-arm count, `tools/alert-bench/qemu.sh`)**: first build NEON 150 M / scalar 141 M instructions a minute — the gathered SIMD mixer did not pay, so it was rebuilt planar with contiguous padded tables: **NEON 129 M / scalar 148 M a minute → ≈ 0.2 s of a 900 MHz core a minute (≈ 0.3–0.4 %)**, ESTIMATE at 0.6–0.8 IPC; output unchanged (same fingerprint, NEON = scalar) |
| SIMD | the mixer products in q65v lanes (planar, contiguous loads); boxcar and DPLL per-sample feedback (≈ 20 flops a sample at 12 kHz), not vectorised. Mac NEON = scalar: whole test output identical (fingerprint 69b64f10b37cc543). No per-sample divide (one per BIT for the soft value), no modulo (the table index walks) |

Owed: **a real NOAA Weather Radio capture** (not receivable in the UK; no server session this run — the weekly test,
RWT, Wednesdays ≈ 11:00–12:00 local, on a US receiver would do), the web key + build-web, Canada's Weatheradio
location codes (shown raw today).

## 9. SELCAL — DONE, wired to server and app (2026-10-31, third run)

Files: `decoders/selcal_decoder.{h,cpp}`, `vibeserver/test-selcal.cpp` (run-tests), host ext `selcal`, vibeserver.json
`"selcal"`, the owner's switch, CMake ×2 + `build_ios.sh`; app: DecoderClient, sdrTypes, ModeSelector (key + hint),
DecoderPanel, decoderBands (Aviation on HF beside HFDL, and a new Aviation group on VHF), modeIcons (aircraft),
SDRScreen (capability, tour), About. `tools/alert-bench/` (bench + `qemu.sh`, shared with SAME). **No web key** (owed).

- **Standard — found free and authoritative**: ICAO Annex 10 Vol III Part II Ch 3 as amended by **Amendment 91**
  (adopted 6 Nov 2020, applicable 3 Nov 2022), its text reproduced in ICAO's own working paper "Amendment 91 to Annex
  10 Volume III on Selective Calling Codes" (APAC CNS SG/28 WP/21, 2024, icao.int — downloaded and read with
  pdftotext): two pulses of two simultaneous tones, 1.0 ± 0.25 s each, 0.2 ± 0.1 s apart, tones ± 0.15 %, within
  3 dB; Table 3-1: 32 tones, Red A–S (312.6–1479.1 Hz) + SELCAL32's Red T–9 (329.2–1557.8 Hz), log⁻¹ 0.0225 apart.
  (The ordering of the two letters inside a pulse is not in that text: ours is Table 3-1's order.)
- **Receiver**: 48 → 8 kHz box average; every 50 ms a 200 ms Hann window; Goertzel at the 32 tones × three dial
  offsets (−4/0/+4 Hz), four tones a lane; a pulse window = at the best offset, two tones ≥ 9 dB over the median of
  the 32, within 6 dB, the third ≥ 8 dB under; a pulse = the same pair 0.6–1.6 s; a code = two disjoint pulses with a
  change-over gap (a window straddling a short gap pairs one tone of each pulse — that ≤ 0.4 s "pair" is ignored, it
  does not discard the first pulse). One report per code per 20 s.

| | |
|---|---|
| Table | A–S at log⁻¹ 0.045, T–9 each log⁻¹ 0.0225 above its partner (checked); no I, N, O |
| Codes | **300/300 random codes right** over all 32 tones (pulses 0.75–1.25 s, gaps 0.1–0.3 s, ±3 dB, ±0.15 %), 0 wrong; the same code twice → once |
| Dial | 40/40 at every mistuning 0–10 Hz; beyond (12 Hz, measured in an earlier build) a neighbouring SELCAL32 tone can be read — the hint asks for the dial within ≈ 10 Hz |
| Sensitivity (white noise, SNR in 3 kHz, 40 codes a point) | −8 dB 5 · **−6 dB 37** · −4 dB and up 40; 0 wrong in the sweep |
| False codes | 2 h white noise: 0; 3 × the 16 DTMF digits at SELCAL timing: 0; ≈ 5 min of speech-like harmonic syllables: 0 |
| CPU | Mac ≈ 1.6 ms per second of 48 kHz audio incl. the noise generator. **Pi 2 (qemu-arm count, `tools/alert-bench/qemu.sh`)**: NEON 552 M instructions a minute, scalar 1150 M → ≈ 0.8–1.0 s of a 900 MHz core a minute (≈ 1.3–1.7 %), ESTIMATE |
| SIMD | Goertzel four tones a lane (q65v); Mac NEON = scalar (fingerprint 06a45fcba7f214e0 both). No per-sample divide or round |

Owed: real SELCAL on air (Shanwick/Gander HF, e.g. 8.891 / 8.864 / 5.598 MHz USB — there was no server session this
run), the web key + build-web.

## 10. Web keys for SAME and SELCAL — DONE (2026-10-31, fourth run); BUILD-WEB STILL NEEDED

Files: `web/client/index.html` (two `data-dec` keys, hidden unless vibeserver.json says `"same"` / `"selcal"`),
`web/client/src/decoders.ts` (the modes, their extensions, the text + state frames as SITOR-A's), `web/client/src/main.ts`
(caps, the hide rule, the state line, the box title and hint, `anyDecoderLeft`), `src/services/alertDecoders.ts` (NEW —
the hints and the state words, ONE copy: the app's `ModeSelector` and `DecoderClient` now import them too),
`scripts/test-web-alerts.mjs` (NEW), `tools/alert-bench/alert_clip.cpp` (NEW — its clip).

- Placement comes from the shared table (`src/utils/decoderBands.ts`, unchanged): SELCAL in HF **Aviation** (alone on the
  web — HFDL is the app's), SAME on the **VHF** tab's Broadcast group beside ADV RDS. Icons from the shared map.
- No settings: the status line is the state ("listening for alert headers" / "reading an alert header…", "listening for
  SELCAL" / "tones…"), the hint is its title, as SITOR-A's.
- `test-web-alerts.mjs` — a real vibeserver fed by `fake-rtl-tcp` replaying `alert_clip` (SAME header ×3, SELCAL BH-DM,
  NNNN ×3 as USB audio on 8891 kHz), headless Edge `--mute-audio`: **18/18** — both keys offered and placed, the alert
  line once across two loops, the end of message, the SELCAL code, the hints, nothing sideways at 390 px, no exception.
  Run: `alert_clip alerts.cu8; VIBESERVER_BIN=… VIBE_ALERT_CLIP=alerts.cu8 node scripts/test-web-alerts.mjs`.
- ★ **build-web is needed before any release**: the test ran against a locally built page (`scripts/build-web.mjs`, then
  vibeserver rebuilt), but `vibe_web_page.h` was put back and NOT committed — the generated page is a release step.
- The web type check (`tsc --noEmit` in web/client) runs out of memory under the 512 MB cap (and at 900 MB); esbuild
  bundled the page cleanly and the headless run exercised every new line.
- Copy: the web lists no decoders on the website and the web client has no tour card for decoder keys; the app's tour
  and About already name SAME and SELCAL (sections 8, 9).

## RESUME HERE (updated 2026-10-31 ≈ 06:40 UTC, after the third run: paging wired, EAS/SAME, SELCAL)

Done this run (commits on `v12-decoders-wave2`): PAGING wired, Android local hardware only (section 7b); EAS / SAME
(section 8); SELCAL — unblocked by ICAO's own free text of Amendment 91 (section 9). No server or device was touched.

Next, in order:
1. **MFSK16/32, Olivia, Contestia, Thor, Feld Hell — NOT STARTED.** Plan: published amateur descriptions only
   (IZ8BLY's MFSK16 notes, ZL1BPU's MFSK/Hell articles, Pawel Jalocha SP9VRC's Olivia description, the Contestia and
   Thor authors' notes); fldigi (GPL) may be READ for ideas, never copied — and is better used as a BLACK BOX (its
   binary generating reference WAVs in a container, as jt9 was) to settle the varicode / interleaver / Walsh-Hadamard
   details the prose leaves open. Feld Hell is an image-like text (columns), so it wants the image path, not text.
2. **ACARS → VDL2 → HFDL → RS41, FLEX, BPC, TDF** — still waiting on Stuart (sections 5–7).
3. Web keys for SAME and SELCAL (web/client decoders.ts / main.ts / index.html) + **build-web**.
4. Owed across the wave (unchanged + new): real-air tests (paging on a phone, NOAA Weather Radio, HF SELCAL, MSK144,
   FST4W, Q65), Android gradle and iOS `build_ios.sh` compiles, FST4's last ≈ 1 dB, `-ffp-contract` decision (measured
   this run: armhf NEON = scalar bit for bit with `off`, decodes identical either way), build-web before any release.

Build used (second run; this run only syntax-checked the shim with its flags): `~/VibeSDR-builds/decoders2-server` (cmake of this worktree's vibeserver, deps from
`~/VibeSDR-v12/vibeserver/mac/deps/prefix` — the main tree's prefix lacks FAAD2). The tone e2e test:
`VIBESERVER_BIN=…/vibeserver VIBE_TONE_CLIP=clip.cu8 node scripts/test-server-tone.mjs` (clip: `test-tone-id
--write-clip clip.cu8`). Colima was started for the paging qemu count and stopped again. No server session took place
(the one attempted — ACARS on the Lenovo — was refused before tuning; the log table stays empty).

Black-box kit (not in git, unchanged): `~/wsjtx-blackbox/` — WSJT-X 2.7.0 arm64 binaries, docker image `wsjtx-bb`,
corpora; the comparison files are regenerated with the run.sh scripts in `~/wsjtx-blackbox/cmpf4b` and `~/wsjtx-blackbox/msk`.
