# Decoder SIMD / CPU audit — 2026-10-31

Stuart: *"have a scan back through the code and check the decoders we already have and make sure they are as
optimised as can be, I think they are already but worth a check to be safe."* Standing rule: everything that can
be SIMD (NEON, SSE2) is, so the decoders run on potato hardware.

Branch `v12-decoder-simd-audit` (off `v12-experimental`). Not merged, not pushed. No live server was touched and no
recordings were made: everything below ran on recordings already in `~/VibeSDR-test-audio`, or on the
benches' and tests' synthetic signals.

## Summary

- **The biggest finding is not in any one decoder.** GCC's 32-bit `arm_neon.h` writes the float compares
  (`vcgtq/vcgeq/vcltq/vcleq/vceqq_f32`) and `vnegq_f32` as plain vector operators. Without `-ffast-math`, GCC
  expands each one lane by lane: four `vcmpe.f32`, a `vmrs` (which stalls a Cortex-A7), and some moves. That is
  about 30 instructions where one was written. So every "NEON" compare in the tree ran scalar on the Pi 2, the Pi 3
  and every armhf host:
  - the ADS-B preamble test;
  - the noise blanker;
  - the DAB demapper guards;
  - the CW and NDB skimmer gates;
  - the atan2 kernel that FM demod uses.

  FT8 had already hit this (`ft8_lib/ft8/vibe_simd4.h` compares bit patterns), but nothing else had.
  `vibedsp/neon_compat.h` now supplies each of these as one instruction (inline asm) and redirects the names. This
  applies to GCC on ARMv7 only. Clang (Android, the armv6 build), AArch64 and x86 are untouched.
- **The second is divides and library maths.** 32-bit ARMv7-A has no integer divide instruction, and no float
  rounding instruction before ARMv8. So the following each cost a library call per sample:
  - a `%` or `/` with a variable divisor;
  - a 64-bit `%`;
  - `std::floor`, `std::lround`;
  - a `uint64 → double` conversion.

  These were 38 % of the NDB skimmer, 23 % of RTTY auto, 20 % of SITOR-A and 13 % of ISM mode. The Mac and the
  compiler's own reports never show this; only the armhf instruction count does.
- **No GCC float loop is auto-vectorised for 32-bit ARM.** NEON flushes denormals, so GCC needs
  `-funsafe-math-optimizations` before it will vectorise float code, and the server does not use it. On the Pi 2,
  anything not hand-written in NEON is scalar VFP. All the new kernels below are hand NEON + SSE2 + scalar.
- **Every change is bit-identical or decode-identical** on the same inputs (evidence in each table). One attempt
  made things worse and was dropped (JTTY's fused dot products: GCC spilled, and the Pi 2 count went *up*).

| Decoder | Pi 2 before → after (est. % of one core) | Change |
|---|---|---|
| **ISM + LoRa** (433.92, Stuart's capture) | ~307 % → ~157 % (two threads) | **−49 %**; then split across six threads, no thread over 49 % on the real Pi 2 ([below](#ism-mode-on-the-pi-2-split-across-threads-2026-10-31)) |
| **NDB skimmer** (192 kHz) | ~15 % → ~8.8 % | **−42 %** |
| **ADS-B** (FlightAware 2.4 MS/s) | ~27 % → ~17 % | **−37 %** |
| **JTTY** | ~28 % → ~19 % | **−33 %** |
| **RTTY auto** | ~21 % → ~15 % | **−26 %** |
| **SITOR-A** | ~2.4 % → ~1.9 % | −22 % |
| **SSTV** (its FFTs) | Kiss 819 M → PFFFT ~270 M per 20 s | **−67 % of the FFT share** |
| **RDS** (cost over plain WFM) | ~18 % → ~15 % (RdsDemod itself −55 %) | −17 % |
| **Auto notch** (per listener) | ~7.1 % → ~6.1 % | −14 % |
| **PSK skimmer** (48 kHz IQ) | ~9.5 % → ~8.3 % | −13 % |
| **CW skimmer** (192 kHz) | ~12.6 % → ~11.4 % | −10 % |
| **NAVTEX** | ~3.1 % → ~2.9 % | −6 % |
| **DAB** (12B, MP2 service, incl. 2.4→2.048) | ~80 % → ~78 % | −3 % |
| **WFM itself** (side effect: atan2 + blanker compares) | 414 → 401 M instr/s | −3 % |
| FT8/FT4, JS8, WSPR, AIS, DSC, WEFAX, audio NR, DRM | unchanged; see their rows | — |

**How "% of a Pi 2 core" is estimated.** There is no Pi 2 on the bench. These figures come from counting every
guest instruction of the armhf build under qemu-arm (method below), then assuming ~600 M instructions/s for a
900 MHz Cortex-A7 core (IPC ~0.67). That assumption is rough. The instruction counts themselves are exact and
noise-free, so trust the **before → after ratio**, not the absolute percentage.

## Method

- **Pi 2 instruction counts.** The binaries are built to match the Pi 2:
  - armhf static;
  - the server's `-O3 -ffp-contract=fast` plus `-marm -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard`;
  - GCC 12, in `vibeserver-cross:bookworm-armhf`.

  They run under qemu-arm with the TCG instruction-count plugin from the FT8 work
  (`tools/ft8-bench/qemu/insncount.c`), in the `ft8perf-x86` image. Counts are mapped to functions (`fnsum.py`) and
  to source lines through inlining (`addr2line -i`). Work dir: `~/Library/Caches/decaudit` (`xb.sh`, `qr.sh`,
  `ab.sh`, `callers.py`, `fndiff.py`). "Base" is `f3bf2a0df`; "after" is the branch.
- **Mac:** the native benches at the server's flags, plus `sample` for a first look. No xctrace was used.
- **x86 SSE2:** i686 static builds (`-march=i686 -msse2 -mfpmath=sse`, the Atom floor) run under qemu-i386, base
  against after. Rosetta is not installed on this Mac, so x86 results are a parity check only, not timings.
- **Load on this Mac:** at most two processes, all `nice -n 15`, one build at a time. Every test was silent.

## Per decoder

### ISM front end + LoRa (`vibe_ism.h`, `vibe_lora.h`) — FIXED

Input: `ism-offline` over `pi500-rtl4-433920-2400k-30s.u8` (VIBE_DAB_PFFFT, as the server builds it). Totals include
the tool's own u8→float conversion (1.15 G in both).

| Hot spot (Pi 2, base) | Share | Status |
|---|---|---|
| LoRa `Channeliser::block` NCO: double `sin`/`cos` per output sample | 16.6 % | **Fixed.** One `sincos` per channel per block; a float rotation inside the block; the NCO phase still carried in double |
| `%` with a variable divisor in the bin gather (4 library divides a bin) | 12.7 % | **Fixed.** Masks (L and Lc are powers of two) |
| ISM `runChan`: three NaN-checked `std::complex` products and a serial rotator per sample | ~15 % | **Fixed.** The mixer is folded into the triangle filter's weights (two tables per channel); `macRun` does two 4-lane MACs and a vector max |
| LoRa bin gather / NCO / 8-tap interpolator / dechirp | ~15 % | **Fixed.** `cmulRun`, `rotateRun`, `sinc8` (NEON / SSE2 / scalar), `cmulf` |
| PFFFT itself | ~25 % after | already SIMD |

- **CPU, Pi 2:** 56.45 G → 29.44 G instructions over 30 s (**−48 %**; −49 % excluding the tool).
- **CPU, Mac:** ISM 433 went 6.3 → 3.9 % of a core. In `test-lora`, all channels went 4.5 → 2.1 % and the default
  set 2.2 → 1.2 %.
- **Parity:**
  - `test-lora` prints the same line for line on Mac NEON, `VIBE_FORCE_SCALAR` and i686 SSE2 (each against its own
    platform's base). That covers every SF × BW × CR round trip, the sensitivity table, the whole band and 20 s of
    noise.
  - `test-ism` and `test-wmbus` give the same results. Only the privacy test's JSON device order changes, and it
    changes from run to run in the base build too.
  - `ism-offline` over the three Pi 500 captures gives the same bursts, decodes and devices. Five noise-level bursts
    (at the 6.5 dB threshold) change their OOK run counts by ±2.
- **Note:** `test-lora`'s SF7 sensitivity check fails on i686 in the **base** build too (75 % at Semtech's SNR
  against ≥ 80 %). libstdc++'s noise differs from libc++'s, so the threshold is marginal there. This is not caused by
  this change.

### ADS-B (`vibe_adsb.h`) — FIXED (via neon_compat.h)

10 s of `flightaware-2026-10-24-2.4msps.u8`, through a driver in the service's block shape.

| Hot spot | Status |
|---|---|
| `pulseMask16` (stage 1, `vcgeq_f32` ×16 per 16 samples) — 35 % | **Fixed.** It was GCC-scalarised; now one `vcge` each |
| the rest (power, pair max, quiet zone, fine grid) | already NEON / SSE2 from the Pi 3 work: already optimal |

- **CPU, Pi 2:** 1604 → 1010 M decoder instructions per 10 s (**−37 %**).
- **CPU, Mac:** 0.86 % of a core (unchanged; clang already emitted the instruction).
- **Parity:** 13 320 frames and 13 182 accepted, the same frame-bytes hash. The full 20 s gives 26 940 frames against
  dump1090-fa's 25 441.

### RDS (`vibedsp/rds.cpp`) — FIXED (part); FIR PROPOSED

pi-bench WFM + RDS at 2.4 MS/s, 2 s, against WFM alone.

| Hot spot | Share of RDS's cost | Status |
|---|---|---|
| The two decimating LPFs (`RealFir`, ~412 taps, I and Q) | 56 % → 68 % | already NEON. **Proposed, see below** |
| 16 timing hypotheses per decimated sample | ~18 % | **Fixed.** Four at a time, NEON / SSE2, bit-identical (same ops, same order) |
| the 57 kHz I/Q mixer at MPX rate | ~6 % | **Fixed.** NEON |
| `vcltq_f32` in the new loop | — | needed the neon_compat fix to be one instruction |

- **CPU, Pi 2:** RdsDemod went 66.9 → 30.0 M per 2 s. RDS's cost over plain WFM went 216.7 → 179.8 M (−17 %). WFM
  itself went 828.5 → 801.3 M, from FmDemod's atan2 compares and the blanker.
- **Parity:** `test_rds`, `test_rds_dsp` and `rds_snr_probe` (the BER sweep and the subcarrier phase sweep) print the
  same output, SIMD and `VIBE_FORCE_SCALAR`. i686 SSE2: `test_rds_dsp` and `rds_snr_probe` are identical to the i686 base.

### CW skimmer (`decoders/cw_skimmer.cpp`) — FIXED (small); PFFFT filterbank verified

The PFFFT filterbank is 45 % of the skimmer and is already SIMD. The leftovers were these:
- `l.smPos = (l.smPos + 1) % l.L`: a library divide per lane per hop. Now a wrap compare.
- The floor factor's `sqrt` every hop. Now kept per lane.
- `track()` and `scan()` compares (neon_compat).

- **CPU, Pi 2:** 22.63 → 20.46 G per 300 s of 40 m 192 kHz (−9.6 %).
- **Parity:** the `wav` decode, `cw_bench accuracy` and `cw_bench multi` print the same.

### PSK skimmer (`decoders/psk_skimmer.cpp`) — FIXED (small)

- `updateFloor` called `std::floor` (a library call on ARMv7) on a double for every searched bin every hop. It now
  uses an integer shift, with the same value.
- A `uint64 → double` library conversion per filter output is now a double counter kept alongside.
- The rotator uses plain products (no NaN check).

- **CPU, Pi 2:** 17.14 → 14.95 G per 300 s (−12.8 %).
- **Parity:** the `wav` decode, `accuracy`, `offsets` and `drift` print the same.
- Left as is: per-symbol `atan`, `log10` and `sincos` (~5 %, per symbol rather than per sample).

### NDB skimmer (`decoders/ndb_skimmer.cpp`) — FIXED

`(start + i) % (uint64_t)R` indexed the envelope ring per sample: a 64-bit library divide (`__udivmoddi4` +
`uldivmod`, 38 % of the skimmer). The period search's epoch folding also re-read the same samples ~60 times.
`unroll()` now lays the ring out straight with one remainder per pass.

- **CPU, Pi 2:** 29.93 → 17.37 G per 330 s (**−42 %**).
- **Parity:** the `wav` decode, `accuracy`, `multi` and `bias` print the same.
- Remaining: `findPeriod`'s autocorrelation (23 %, double) and PFFFT. Could be vectorised; not done.

### JTTY (`decoders/jtty_decoder.cpp`) — FIXED

The tail-biting list Viterbi (512 states × 46 symbols × 2 passes) was 52 % of the decoder and entirely scalar. The
butterfly now runs four predecessors at a time: pm[j..j+3] and pm[j+256..j+259] produce eight interleaved
successors. The four-tone branch metric is a `vtbl` on NEON and a 16-scalar gather on SSE2.

- **CPU, Pi 2:** 100.1 → 66.8 G per 600 s (**−33 %**).
- **CPU, Mac:** 8.85 → 7.77 s for the three recordings.
- **Parity:** the decodes of `jtty_14090000`, `jtty_7090000` and `wsjtx_sample` are identical on NEON,
  `VIBE_FORCE_SCALAR` and SSE2 (i686, recording + `cpu 60`).
- **Tried and dropped:** fusing `decodeAt`'s FIR and correlator dot products to share loads. GCC armhf spilled the
  accumulator arrays and the count rose 1.5 G.
- Remaining: `decodeAt` (47 % after). Its dot products are NEON already; the cost is the number of candidates.

### RTTY / NAVTEX (`decoders/fsk_decoder.cpp`), RTTY auto (`rtty_auto.cpp`) — FIXED

- `FskDecoder`: `sampleCount % bitSampleCount` per sample (a library divide) is now a counter. RTTY auto went
  1961 → 1461 M per 15.9 s (**−26 %**; it runs several FskDecoders at once).
- `NavtexRx`: the decimator ring and the raised-cosine ring used `% N` per sample. NAVTEX went 11.30 → 10.59 G per
  600 s (−6 %).
- **Parity:** NAVTEX on `nav48_4209500` (48 kHz) and `nav8` (8 kHz), RTTY auto and fixed-shift on both JARTS
  recordings: the text is identical.
- Left as is: three double divides per sample in the ATC tracker (`(in − avg) / w`). Replacing them with
  reciprocals would not be bit-identical, and they are about 0.5 % of a Pi 2 core.

### SITOR-A (`decoders/sitor_a.cpp`) — FIXED

`acqSample` computed seven `std::lround((6 − i)·K)` per bin per sample, plus `% Ki`. The offsets are now hoisted to
one set per sample and the ring uses a wrap. It went 1205 → 945 M per 84 s (−22 %), with identical output on all
four SITOR recordings.

Also fixed: a bin's ring index is now reset if a re-measured rate shortens the ring. Before, it could index past the
end.

### SSTV (`decoders/sstv_decoder.cpp`) — FIXED

`SstvFFT` (2048 / 1024 / 512 real points) used scalar Kiss FFT: 45 % of SSTV on the Pi 2. It now uses PFFFT
(ordered real output unpacked per bin, same convention), with Kiss kept for any size PFFFT cannot do.

- **CPU, Pi 2:** the FFT share went 819 → ~270 M per 20 s of the benchmark's Robot 36.
- **CPU, Mac:** `test-sstv-quality` went 20.3 → 9.7 s.
- **Parity:** `test-sstv-quality` prints the same table: 23 transmissions, slant, offset and colour per mode, every
  figure.
- There is no real SSTV recording in the library. `tools/sstv_harness.cpp`'s build line was updated (pffft.c, and
  the Kiss paths, which had moved).

### Auto notch (`decoders/auto_notch.cpp`) — FIXED (small)

The prediction and the tap energy were two `dotReal` passes over the same 160-tap history. `dotPair()` does both in
one pass, keeping dotReal's accumulators and reduction.

- **Parity:** the output hash over 15.9 s of 48 kHz audio is bit-identical (Mac, armhf and i686 SSE2).
- **CPU, Pi 2:** 677.6 → 584.6 M (−14 %; ~6 % of a Pi 2 core per listener with the notch on).

### Already optimal / measured, not changed

| Decoder | Pi 2 (est.) | Finding |
|---|---|---|
| **FT8/FT4** (`ft8_decoder.cpp`, ft8_lib) | 2.46 G per 390 s of 40 m, ~1 % | **The v12-ft8-perf work IS merged into v12-experimental** (merge `f00ef133b`, 2026-10-25) and on this branch. `bp_decode_ex` (NEON BP) is 65 %; its compares were already bit-pattern. Count identical before/after |
| **DAB** chain | 7.0 G per 15 s, ~78 % | Resampler (16-tap polyphase, NEON, pair-interleaved for in-order cores) 28 %, OFDM + demapper 16 %, Viterbi 14 % — all hand-tuned for the Pi 2 already (commits `caaec2570`, `82dfeb09f`, `ffe502eff`). This audit gave −2.9 % via the demapper's compares. Output identical |
| **JS8** | 26.8 G per 900 s, ~5 % | `js8::bpDecode` (LDPC 174,87) 37 % is **scalar** — PROPOSED below. Turbo's 1200-point STFT cannot use PFFFT (not a multiple of 32), so it stays on Kiss (9 %) |
| **WSPR** | ~4.7 G per busy 2-min slot, ~6.5 % averaged | Fano 30 % (sequential by nature); the rest is in SIMD kernels already. The bench's own synthesiser dominates its count |
| **AIS** | ~24 % at 2.4 MS/s (synthetic clip, 40 frames) | 46 % is vibedsp `FirDecimator` (NEON, shared); the GMSK/HDLC stages are spread thin. Count identical before/after |
| **APRS** | Mac 0.05 % (1200 Bd), 0.09 % (300 Bd) | not Pi-profiled — tiny |
| **DSC** | ~2.3 M instr/s, < 1 % | front end 46 %; nothing worth changing |
| **WEFAX** | ~16 M/s, ~3 % | `demodulateData`; no recording in the library — not changed |
| **Audio NR** | ~23 M/s, ~4 % per listener | Kiss FFT is 45 % of it — PROPOSED below |
| **DRM** | Mac 0.30–0.38 % (DRM.md) | **not re-profiled in this audit** (the SIMD demapper was done in its own work) |
| **Time signals** | Mac 0.009 % | busy file (`time_decoder.*`) — not touched |

## Proposed — not done

These were left either because the file belongs to a parallel branch, or because the change cannot be made
bit-identical and needs its own validation.

1. **RDS's decimating LPF (`RealFir`, ~412 taps × I/Q at the MPX rate) is 68 % of RDS's remaining cost.** Two
   options:
   - **Two-stage decimation** (a halfband or a short FIR first, then the 2.4 kHz filter at ~40 kHz). This is the big
     one, roughly 3–5× less. It changes the filter's response, so the PIRA-validated deviation figures (rdsRms_,
     kRdsChainGain) and the noise correction must be re-checked against Hans's table before shipping.
   - **One dual-output FIR** (I and Q share the tap loads). This is bit-identical and worth ~15 % of the FIR, but it
     belongs in `vibedsp/ddc.cpp` (`RealFir`), which the AM-sync branch's RxPipeline work is near.
2. **RxPipeline** (amsync branch's area) does not need a change for the compare fix. FmDemod's atan2 and the
   ImpulseBlanker already gained from `neon_compat.h` without a code change (WFM −3 %).
3. **JS8 belief propagation (`digimodes/js8_ldpc.cpp`)** is scalar, with a rational tanh/atanh per edge (divides).
   A check-major padded layout as FT8's `bp_decode_ex` uses, with 4-wide tanh/atanh, would roughly halve 37 % of
   JS8. It would not be bit-identical (NEON reciprocal vs IEEE divide), so it needs the JS8 decode-count parity run
   (`tools/js8-bench`, the three JS8 recordings, plus PSKReporter).
4. **Audio NR (`decoders/audio_nr.cpp`)**: move its 2048-point Kiss real FFT pair to PFFFT. That is ~45 % of NR, but
   the output is no longer bit-identical, so it needs a listening check plus an SNR/PSNR comparison.
5. **NDB `findPeriod` autocorrelation** (23 %, double, `nd ≈ 1000` lags): float NEON would roughly halve it. It is
   not bit-identical (double → float), so it needs `ndb_bench accuracy/multi`.
6. **The digi_* wiring (Band spots: `digi_spotter`, `digi_tuner`)** is the decoders-wave-2 branch's. Not profiled
   here. Mac `dec_ft8band` is 0.7 % of a core, the heaviest benchmark decoder row. It is worth a Pi 2 count once
   that branch has merged.
7. ~~**ISM mode on a Pi 2 is still ~1.5 cores** at 2.4 MS/s with the 433 plan, even after −49 %.~~ **Done
   differently**: the work is split across threads (and steady emitters are held), measured on the real Pi 2. See
   [ISM mode on the Pi 2: split across threads](#ism-mode-on-the-pi-2-split-across-threads-2026-10-31).

## ISM mode on the Pi 2: split across threads (2026-10-31)

Stuart's Pi 2 (192.168.86.129, the minimum spec) on ISM 433 at a 2.0 MHz span, measured live from `/proc` before this
work (v12-experimental, without the audit's savings): the whole server at 216 % of a core, one decoder thread at 106 %
(saturated, so bursts were lost), another at 29 %, `vibe-dsp` 38 %, `vibe-spec` 31 %, `vibe-rtl` 10 %. The health
chip said "too heavy". The ISM panel showed 248 bursts, 247 of them unidentified, and the waterfall had steady vertical
lines across 433–434.8 MHz. Stuart: those lines are because 433 is well out of that antenna's range ("OUTSIDE ANTENNA
RANGE"), so they are spurs and pickup, not signals. Hosts with poor antennas will look like this, so it is a real load
case.

**Goal:** no single ISM thread over about 60–70 % of a Pi 2 core at the 433 plan's full span, with decodes unchanged.

### What the live figures could not say

- **Neither decoder thread had a name or a priority.** Both were started from the connection thread that switched
  ISM mode on, so `top` showed them as `vibe-conn` at nice 0. That put them level with the connection threads, above
  spectrum, against the priority rule (Network > Audio > Spectrum > Decoders).
- **So which thread was 106 % is not known.** The offline measurements below show that LoRa is the heavier of the two
  on the default set. On the build measured live (before the audit) it was heavier still. The 106 % thread may well
  have been LoRa, not the sensor decoder.

### Profile (Pi 2, `perf stat` and stage timers, `nice -n 10`)

Built with `-DVIBE_ISM_PROF` (`ism-offline` prints each stage), on Stuart's Pi 500 recording
`pi500-rtl4-433920-2400k-30s.u8` (2.4 MS/s), on the Pi 2 itself. % of one core in real time.

| Stage (one thread before) | Pi 500 capture | hog-heavy capture |
|---|---|---|
| copy into the ring + overload meter | 7.8 | 7.1 |
| detector (FFT, floor, groups) | 8.7 | 8.3 |
| channel mixing (each burst's channel) | 23.7 | 39.0 |
| slicing (OOK/FSK run lists) | 24.9 | 61.9 |
| protocol decoders | 0.9 | 1.4 |
| **sensor front end, one thread** | **~66** | **~118** |

- **Steady emitters.** On the Pi 500 capture, 15 channels ran the full 1.6 s limit. They were two wandering emitters
  near 433.27 and 434.82 MHz, hit in about half the detector frames over a ±30 kHz spread. The floor cannot absorb
  them because they move between bins. Each was cut at 1.6 s and re-opened on the next frame, for ever. They held 43 %
  of the samples mixed, and their 1.6 s slices were most of the slicing.
- **The hog-heavy case** is that recording with six such emitters added across the band (`tools/ism-bench/add-hogs.cpp`,
  twice the recording's RMS). Before the changes, 3.2 of the 4 channels were busy on average. One thread needed ~118 %:
  the live symptom.
- **The decoders themselves are under 1.5 %.** The cost is the front end.

**LoRa, on its own thread before** (Pi 2, inline, `VIBE_ISM_PROF`, the default set the server picks on armhf), % of
one core:

| Part | 433 default (2 channels, 2 detectors) | 868 default (5 channels, 20 detectors) |
|---|---|---|
| capture FFT (2048 points every 1536 samples) | 35.9 | 41.3 |
| each channel's filter + inverse FFT | 10.9 | 22.8 |
| rotation + residual NCO | 2.3 | 5.0 |
| resampler to the chip rate | 3.5 | 7.8 |
| detectors | 3.0 | 29.6 |
| **LoRa, one thread** | **~59** (70 in real time) | **~110** (101 in real time, dropping) |

LoRa was always on its own thread, and after the sensor split it was the heaviest one in ISM mode. On 868 it could not
keep up at all.

### What changed

1. **The front end in three stages** (`vibe_ism.h`), each its own class:
   - `Detector` decides where each burst is, and when its channel opens, is promoted, is cancelled or closes. It says so
     as `ChanEvent`s at sample positions and mixes nothing. It no longer copies the capture: each FFT window is read
     from the block, and the overload meter is a vector max (`absPeakRun`, NEON / SSE2 / scalar, exact in any order).
   - `ChanMixer` follows those events over the same samples and hands each closed channel on as a `RawBurst`.
   - `FrontEnd::finish` is the slicer, unchanged.

   `FrontEnd` keeps its API and runs the three inline. A channel's output depends only on the samples it covers and its
   frequency, never on when it was mixed, so the bursts are the same bit for bit.
2. **The service runs them on three threads** (`vibe_ism_service.h`), each fed by a bounded queue that drops its OLDEST
   work and counts it:
   - `vibe-ism`, the detector: the one stage that must keep real time. Decoder rung, nice 10.
   - `vibe-ism2`, the mixer: half a second of work samples may wait.
   - `vibe-ism3`, the slicer, decoders, device list and gallery: four of the longest channels and a second more may wait.

   The later two sit one notch lower (new `vibeDecoderLagThread`, nice 12; on the Mac, UTILITY −4). On one core, or a
   box the DSP already fills, the detector gets its time first, and what gives way is the stage built to give way.
3. **LoRa in three stages too** (`vibe_lora.h`, `vibe_lora_service.h`). The channeliser's block is now three parts,
   run in block order:
   - `vibe-lora` (decoder rung): `feedSpectra`, the capture FFT and each channel's bins × its filter;
   - `vibe-lora2`: `deliver`, each channel's inverse FFT, the block's phase, the residual NCO and the resampler;
   - `vibe-lora3`: `pushOut` and the detectors.

   The last two sit one notch lower. Half a second of work may wait before each; past that the oldest goes, counted in
   LoRa's `"dropped"`. A block dropped before `deliver` still moves each channel's NCO phase on, so the channel stays on
   frequency. Inline, `feed()` runs all three parts in turn: the same arithmetic in the same order.
4. **A hog is held, not re-opened** (`Detector`, `FeConst::kHoldQuietS`):
   - When a channel runs the full 1.6 s, its frequency is held. Detections within 120 kHz refresh the hold and open
     nothing, until none has been seen there for 0.2 s.
   - A detection 6 dB over the hog's own peak still opens a channel of its own.
   - **No burst is lost by it.** While the hog's channel was open, any burst within 120 kHz was already folded into it.
     The splatter rule also treated anything 25 dB under an open hog channel as its splatter, anywhere in the band, so
     hogs used to hide weak bursts band-wide. Those now get channels.
5. **Every thread is named and ranked:** `vibe-ism`, `vibe-ism2`, `vibe-ism3`, `vibe-lora`, `vibe-lora2`.
6. **The report says more.** `"drops":[capture, work, channels]` gives what each stage lost; `"dropped"` (their total)
   still feeds the health pill's "too heavy". `"carriers"` is the number of frequencies held right now.
7. **Memory stays bounded**, and the worst case applies only while a stage is behind. In order:
   - half a second of capture before the detector, and half a second of work samples before the mixer;
   - before the slicer, four 1.6 s channels and a second more. That baseband was already in memory while those
     channels were open, before the split as well; the bound only limits how long it may wait.

   In all, ~34 MB at 2.4 MS/s on 433 and ~48 MB on 868's wider channel, plus LoRa's half second (~10 MB). Normally the
   queues are near empty.

### Before and after on the Pi 2 (real time, per thread)

`ism-offline --realtime` runs the server's own `vibeism::Service` (its threads, queues and priorities), fed at the
capture's own pace in the radio's 32 ms blocks. Thread CPU comes from `/proc/self/task`. "Old" is the same harness built
against the headers from before the split (`-DVIBE_ISM_OLD_SERVICE`), so the comparison is like for like. LoRa runs the
set the server picks on armhf with no benchmark yet (the default tier). 2.4 MS/s; the live Pi ran 2.0 MS/s, which is
about 17 % lighter. The feeding thread (u8 to float, the server's `vibe-rtl` / `vibe-dsp` work) is listed apart. % of
one core.

**433, Stuart's Pi 500 capture** (30 s; a repeat run agreed within 0.8 points on every thread):

| Thread | Old | Split, no hold, LoRa on one thread | **Final** |
|---|---|---|---|
| sensor worker (one thread) | **63.0** | — | — |
| `vibe-ism` detector | — | 11.3 | 11.3 |
| `vibe-ism2` channels | — | 27.6 | 15.8 |
| `vibe-ism3` slicer + decoders | — | 23.8 | 9.3 |
| LoRa (one thread) | **70.1** | 70.0 | — |
| `vibe-lora` / `lora2` / `lora3` | — | — | **47.1** / 16.7 / 4.6 |
| feeder (u8 → float) | 24.3 | 22.7 | 22.7 |
| **total** | **157.5** | 155.4 | **127.5** |
| drops | 0 | 0 | 0 |

**The hog-heavy capture** (six wandering emitters added, `add-hogs … 2`). The old service drops 171 capture blocks in
30 s: the live "too heavy".

| Thread | Old | **Final** |
|---|---|---|
| sensor worker | **96.8** (171 blocks dropped) | — |
| `vibe-ism` / `ism2` / `ism3` | — | 11.5 / 25.3 / **35.3** |
| LoRa | 71.9 | 49.0 / 16.7 / 4.6 |
| **total** | 197.1 (and 5 s of capture never processed) | **160.6**, nothing dropped |

**Stuart's car-key capture** (60 s, 344 short bursts):

| Thread | Old | **Final** |
|---|---|---|
| sensor worker | 32.3 (still **4 blocks dropped**: one long slice held it past its half-second queue) | — |
| `vibe-ism` / `ism2` / `ism3` | — | 11.1 / 10.4 / 10.5, nothing dropped |
| LoRa | 68.4 | 47.7 / 16.5 / 4.5 |
| **total** | 125.5 | 125.5 |

**868, the Pi 500 capture**, LoRa default set (five channels, twenty detectors):

| Thread | Old | **Final** |
|---|---|---|
| LoRa (one thread) | **101.5**, 109 blocks dropped | `vibe-lora` **58.2** / `lora2` 36.3 / `lora3` 38.2, nothing dropped |
| sensor worker | 17.8 | 11.0 / 6.8 / 0.5 |
| **total** | 144.6 (incomplete: LoRa dropped) | 177.1 (all of it done) |

**Every LoRa plan channel** (the owner's "lora+all"; never the Pi 2 default):

| Case | Old (one LoRa thread) | **Final** (`vibe-lora` / `lora2` / `lora3`) |
|---|---|---|
| 433 (7 channels, 22 detectors) | 101.8, 265 blocks dropped | 66.9 / 57.2 / 42.5, nothing dropped |
| 868 (13 channels, 42 detectors) | (not run: would be ~2 cores on one thread) | 67.2 / 81.3 / **83.7**, nothing dropped |

- **The overhead of splitting is nil, measured.** The split without the hold totals 155.4 % against the old 157.5 %.
  LoRa on three threads costs 68.4 % against 70.1 % on one. The total falls because the hold removes the hog churn.
  Where the total rises (868), it is work the old service never did: it dropped it.
- **Every ISM and LoRa thread is under one core with headroom** on the defaults: the heaviest is `vibe-lora` at 47–49 %
  on 433 and 58 % on 868. The sensor threads are at most 35 % (the hog case).
- **Instruction counts barely move.** The threaded runs need 2–4 % more instructions than inline (copies and queues).
  They run at a lower IPC (0.46 against 0.52 when every stage runs flat out at once), because the stages share the
  Cortex-A7's 512 KB L2.

### Decodes: identical

- **`ism-offline --bursts` over the three Pi 500 captures, inline:**
  - against the base build (`cb0fabfd4`): identical until the hold (commit 3 of the series), on the Mac (NEON and
    `VIBE_FORCE_SCALAR`) and on the Pi 2 itself (NEON and scalar, the 640 µs ARM hop);
  - with the hold: decodes identical on all three (0, 1, 0). Only the 433 capture's burst list changes: the emitters'
    churn goes, and a few weak bursts the splatter rule had hidden appear (none decodes).
- **The service's threads equal inline, burst for burst** (`ism-offline --threads`, lossless, against inline
  `--band-only`). This holds on all three captures on the Mac (NEON and scalar) and on the Pi 2.
- **test-ism** (160 checks), all passing. It now also checks:
  - the three threads against inline;
  - a block dropped before the mixer (the channel across the hole is abandoned, is never stitched, and is counted;
    every later burst is unchanged);
  - the hog hold with two wandering emitters and eight sensors: 1.6 s channels 12 → 4, readings 7 → 7, one sensor 20 dB
    over a hog inside its hold still decoding.
- **Unit tests identical to base otherwise:**
  - test-wmbus (311), test-ism-gallery (56), on the Mac and on the Pi 2. On the Pi, test-wmbus has one failure, "S1:
    frequency 868281666", which the base build has too.
  - test-lora, which now also checks that its three threads give the inline engine's 11 frames, field for field. That
    check passes on the Pi 2 too. There, test-lora is otherwise identical to base, including two failures the base
    build has on armhf: the SF7 and SF8 sensitivity rows, 72 % and 77 % against "≥ 80 %". It is the same marginal
    threshold under libstdc++'s noise noted for i686 above.
  - NEON = scalar on every output. i686 SSE2 (qemu): test-ism 160/160, the keyfob capture identical to base, and the 433
    capture's bursts identical to the Mac NEON build.
- **End to end against the new server binary (Mac):** `test-server-ism` 34/34, `test-web-ism` 25/25,
  `test-server-ism-gallery` 33/33, `test-server-lora` 18/18, `test-web-lora` 21/21, all silent (`--mute-audio`).
  `test-web-ism-gallery` fails one check: it expects the gallery chips "ALL NAVTEX RTTY WEFAX SSTV FT8 CW ISM", and the web
  client now shows more (DIGITAL SPOTS, PSK, NDB, AIS, APRS, DSC, SITOR-A). That expectation is stale; this work touches
  no web code.
- **Builds:** the server on the Mac (`make -j2`) and cross-built for armhf (`-DVIBESERVER_ALLOW_NO_TUNNEL=ON`: a compile
  check, not a package).

### Single core, weak boxes, Android

- **The same code runs in the Android app's server** (`local_sdr_shim.cpp` → `vibe_ism_service.h`); nothing in it is
  Linux-only. The Android affinity sweep (`vibe_affinity.cpp`) already moves any non-frame thread off the frame cores,
  and these threads take its default.
- **On one core** the stages share it. The detector's rung (nice 10 against the later stages' 12) gets it its time
  first, so detection keeps real time and a later stage lags or drops, counted. Nothing waits for anything: every queue
  drops its oldest work and has a size limit.
- **Not measured:** an actual single-core box or a 32-bit phone (see below).

### What Stuart should check live (after a build reaches the Pi 2)

1. **`top -H` (or `~/bench/v12/threadcpu.sh 30`) in ISM 433.** Expect `vibe-ism`, `vibe-ism2`, `vibe-ism3`, `vibe-lora`,
   `vibe-lora2` and `vibe-lora3` at nice 10 / 12, and no `vibe-conn` doing decoder work. Expect each under ~60 %;
   `vibe-lora` should be the heaviest, at ~40–50 % at the live 2.0 MHz span.
2. **The health chip should stop saying "too heavy"** in ISM 433 at full span. If it does say it, `/vibeserver.json`
   or the ISM report's `"drops":[capture, work, channels]` (and LoRa's `"dropped"`) says which stage fell behind.
3. **The report's `"carriers"`** counts the spurs being held. On the Pi 2's out-of-range antenna, expect a few.
   "Unidentified" should fall well below the 247 of 248 seen, because the hog churn no longer makes bursts.
4. **A real sensor near one of those steady lines should still appear.** For example, a key fob pressed beside a spur:
   if it is clearly stronger than the spur, it gets its own channel.

### Questions for Stuart

1. **The ARM detector hop.** 32-bit ARM looks every 640 µs instead of 320 µs (an earlier CPU saving). On Stuart's
   car-key capture that misses the one Oregon decode the 320 µs hop finds. The detector is now its own thread at ~11 %
   of a Pi 2 core, so 320 µs would cost about another 8–10 points there. Restore it on ARM? Not changed here: decodes
   had to stay as they were.
2. **"lora+all" on 868 on a Pi 2** keeps up now, but its detector and channel threads are at 81–84 %. Keep it as
   owner-only, or show a warning when it is chosen on a Pi 2?
3. **`test-web-ism-gallery`'s chip list** is stale (see above). Update the expected chips?

### Not tested

- **Live:** no server was deployed or reconfigured. The live figures above are from before; the after figures are the
  offline real-time harness on the Pi 2, which runs the server's own `vibeism::Service` but not its DSP, spectrum or
  network threads. The Pi's live server was idle (0 listeners) during every run, but it was running.
- **Android:** the app's server was not built (no gradle). The code is the same headers the Mac and armhf server builds
  compiled, NEON / SSE2 / scalar throughout.
- **A single-core box and a 32-bit 1 GB phone:** reasoned about, not run.
- **x86 SSE2** was checked for identical output under qemu-i386, not timed.
- **868 meters (wM-Bus) through the threads:** the 868 capture holds no meter. test-wmbus covers the meter decoder
  (unchanged, identical), and test-server-ism's meter clip passed through the threaded server.

## Not tested

- **Nothing was run on a real Pi 2.** All Pi 2 figures are qemu-arm instruction counts, and the "% of a core"
  conversion assumes an IPC.
- **x86 timings:** SSE2 paths were checked for parity only, under qemu-i386 (not timed). These were run on i686:
  - `test-ism`, `test-lora`, JTTY;
  - RDS (`test_rds_dsp`, `rds_snr_probe`) and auto notch (output hash).

  Every one was identical to its own i686 base.
- **The NEON-compare inline asm** was built and run under qemu-arm (DAB, RDS, ADS-B, CW, NDB, ISM). It was not run on
  real Cortex-A7/A53 hardware. It is the same instruction clang emits for Android armeabi-v7a.
- **iOS:** the vibedsp changes (`rds.cpp`, `neon_compat.h`) reach iOS only after `build_ios.sh` rebuilds the prebuilt
  DSP library. They are AArch64 there, so only the RDS loop changes.
- **Not run:** `scripts/run-tests.sh`, as the brief asked. Only the targeted tests and benches listed above were run.

## Recordings and servers

- No recording sessions were made, and no server (UberSDR, Pi 500, Lenovo, Pi 2) was touched.
- **The ISM thread split** ran its benches on the Pi 2 itself (192.168.86.129), in `~/bench/v12/` only, through the
  gated `run.py` (`nice -n 10`, 0 listeners and no live throttle bit before and during each run). Nothing was installed,
  restarted or reconfigured. Its only new input is synthetic: `hog433.u8`, the Pi 500 433 capture with six emitters
  added by `tools/ism-bench/add-hogs.cpp` (kept in the Mac's cache, not in `~/VibeSDR-test-audio`).
- Every input was already in `~/VibeSDR-test-audio`, or was a bench's or test's own synthetic signal:
  - ADS-B `flightaware-…-2.4msps.u8`;
  - ISM `pi500-rtl4-*` ×3;
  - DAB `12B-bbc-2026-10-26.vbu8` (15 s converted to VIBEIQ16);
  - CW `uber_40m_7025000_iq192`;
  - PSK `uber_40m_7040500_iq48_afternoon`, `uber_20m_14071500_iq48_afternoon`;
  - NDB `uber_ndb_352500_iq192_…_330s`;
  - JTTY ×3;
  - DSC `dsc_8414500_…`;
  - NAVTEX / SITOR (`sitor/`);
  - RTTY `jarts_*`;
  - FT8 `ft8_40m_7074000_20261025T2044`;
  - JS8 `js8_40m_7078000_…`.
