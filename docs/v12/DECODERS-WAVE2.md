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
| SIMD | NEON = scalar decodes bit for bit (sweep + noise). Hot loops vectorised: WHT64 (register-resident), BP products/normalise, sync search row sums, complex dot/mix. Not vectorised (does not pay): the GF(64) permutation gathers, the per-symbol exp/log metric (4 k per attempt) |
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
