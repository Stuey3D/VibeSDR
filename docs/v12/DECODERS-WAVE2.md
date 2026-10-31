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

## 4. CTCSS / DCS tone ID — ENGINE DONE, not wired to clients yet (2026-10-31)

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

### Owed (the wiring — deliberately not done tonight)
- **Server**: in `local_sdr_shim.cpp` `feedOneClient`, beside the raw-IQ tap (`iqTap(c.get(), c->slice.data(), got)`),
  feed a per-client `ToneId` while the listener is in NFM (create on NFM, `reset()` on retune), at `c->chanRate`;
  push `{"type":"tone","label":"CTCSS 88.5 Hz"}` to that listener on `onChange`. Not RxPipeline. Cost per listener is
  below the noise of that loop; if the thread-priority rule wants it off the DSP worker, hand the slice to the
  decoder host's queue instead.
- **Clients**: the VTS line's quiet suffix (DECODER-RESEARCH.md §CTCSS), app + web; the AGENTS.md copy rule applies
  (tour/About mention it).
- **Real signals**: a 2 m / 70 cm repeater with a known tone (UK repeaters publish theirs) and a PMR446 DCS user;
  confirm the N/I convention on one known DCS radio.

---

## RESUME HERE — the queue from MSK144 on (not started)

3. ~~MSK144~~ DONE (section 3). ~~FST4 gap~~ halved (section 2b). ~~CTCSS/DCS engine~~ (section 4; wiring owed).
   NEXT: time stations (JJY/BPC/TDF/CHU check).
4. CTCSS/DCS — NOTE: the decoder host's audio is post-voice-filter in NFM VOICE (the 300 Hz high-pass cuts CTCSS by
   ≥ 40 dB: vibedsp.h setNfmVoice); the tone decoder needs the listener's RAW discriminator or its own FM
   discriminator on the per-client IQ tap (`iqTap` in local_sdr_shim.cpp) — outside RxPipeline as asked.
5. JJY/BPC/TDF/CHU · 6. ACARS → VDL2 · 7. HFDL · 8. RS41 · 9. PAGING (spec at the top) · 10. MFSK/Hell/SELCAL/EAS.

Black-box kit (not in git): `~/wsjtx-blackbox/` — `dl/x/usr/bin/*` (WSJT-X 2.7.0 arm64 binaries), docker image
`wsjtx-bb` (`img/Dockerfile`), corpora and sims. Colima was started with 2 CPUs / 2 GB for it.
No server session was used this run (all tests synthetic or black-box): the server log above is empty.
