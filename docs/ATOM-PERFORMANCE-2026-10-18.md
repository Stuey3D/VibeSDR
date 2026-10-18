# Atom (Bonnell) performance — 2026-10-18

Stuart: *"squeeze more out of old Intel Atom netbooks … a realistic baseline … those netbooks were prolific."*
Targets: Kiko's **Atom N270** (32-bit only, i386 package) and Danny's **Atom N450** (64-bit, amd64 package,
public server "Acer Netbook RTL-SDR"). Both are the same in-order **Bonnell** core: SSE2 + SSE3 + SSSE3 + MOVBE,
no SSE4, no AVX, one core with Hyper-Threading.

★ Everything below was measured on the Lenovo (i5-11300H, 192.168.86.13) in bookworm containers. **The i5 is not an
Atom**: it is out-of-order and hides latency, unaligned-load penalties and short dependency chains that Bonnell pays in
full. So every result carries **instructions retired** (perf stat / perf record -e instructions) as the Atom-relevant
proxy — Bonnell retires roughly 0.5–0.8 instructions per cycle on this kind of code, so instructions are close to
cycles there — with i5 cycles/CPU % as secondary evidence only.

## Summary

* **Top stages** (11.0.5, 2.4 MS/s WFM, 20 fps, one listener; share of the server's instructions): the channel
  decimation FIR **28 %**, the audio/MPX FIRs **13 %**, the MPX noise meter's five biquads **~8 %**, the spectrum FFT
  **~17 %**, per-frame spectrum analysis **~7 %**, Opus **5.5 %** per listener. u8→float conversion only ~1 %.
* **What paid — code, not flags.** An SSE2 FIR kernel built for in-order x86 (aligned loads via phase-shifted tap banks,
  no shuffles, no scalar tail), a cheaper form of the noise meter, an end to per-block vector zero-fills and per-bin
  push_back, decimators that step straight to the next kept output, and a runtime-dispatched **SSSE3** DAB Viterbi
  (−45 %): the whole server needs **−20 % instructions at 2.4 MS/s and −22 % at 1.2 MS/s on amd64, −18 / −19 % on
  i386**, before PGO.
* **Flags:** `-march=bonnell` (the ceiling of a raised floor) **+1.5…2.3 % instructions** — nothing; LTO, no-PIE,
  `-fno-math-errno -fno-trapping-math`: ±1 %; **PGO −3.4…−5.3 %** (kept, for the test packages); `-mtune=bonnell`
  +1.5…2.7 % instructions by design, value unknowable on the i5 → A/B package for Kiko.
* **Floor: keep SSE2.** SSSE3 is used where it pays (Viterbi) through runtime dispatch at no measurable cost; a raised
  floor would buy nothing and drop every pre-2011 AMD.
* **Test packages** (`vibeserver/build/prebuilt/`, never published): amd64 `11.0.5-1+atom2` (Danny), i386
  `11.0.5-1+atom2` and `+atom2g` (Kiko A/B). Against the released 11.0.5-1: **−20.8 / −23.0 % instructions (amd64,
  2.4 / 1.2 MS/s), −22.7 / −23.9 % (i386)**.
* **N450 sandbox** (an i5 core pair throttled to Danny's measured behaviour): 2.4 MS/s **15.2 → 16.8 fps, audio loss
  21 % → 10 %**; 1.2 MS/s from "at the limit" to headroom (CPU 8.9 → 7.9 % against an 8.5 % cap). On i386: 2.4 MS/s
  13.8 → 15.9 fps; 1.2 MS/s 18.2 → 19.9 fps with no audio lost. 2.4 MS/s is still too much for an N450 — Stuart's
  1.2 MS/s low-end recipe stands, now with headroom.
* **Codecs:** DAB+ AAC is decoded by the OS's ffmpeg (not ours, already SIMD); MP2 already SSE2; a bundled libopus
  would save ≤ 3 % of Opus on i386 and nothing on amd64 — keep Debian's.


## 1. Method

* **Workload = Danny's.** The full `vibeserver` (not just the DSP library), fed RTL-style **unsigned 8-bit IQ** over
  rtl_tcp by a paced replay (`rtltcp_replay.py`, real time, 10 ms chunks) of a generated, *decodable* station: WFM
  stereo + pilot + real RDS group 0A (the benchmark's own `fmStation`), two AM carriers and Gaussian noise
  (σ ≈ 4 LSB, so FTZ/denormals do not flatter anything). One headless listener (Python websockets, silent): binary8
  spectrum, 1024 bins, **20 fps**, Opus audio, tuned to the station. Rates **2.4 MS/s** and **1.2 MS/s**.
* **Counting.** `perf stat -e instructions,cycles -p <server>` over a 23 s window after 12 s warm-up (instructions per
  second of real signal, so lower is better and the number is directly a CPU budget). Run-to-run spread of the
  instruction counts: **< 0.2 %** — so single-digit-percent differences are real. Per-stage attribution:
  `perf record -e instructions:u -c 200000` on the same run.
* **The other modes** (NFM/AM/SSB, Advanced RDS, locked-range listeners, decoders, DAB): `vibeserver-bench` (3 s per
  scenario, the DAB clip `dab-bench-clean.vbu8`) under `perf stat`.
* **Containers**: `--privileged` only for the perf runs; server pinned to one physical core's two SMT siblings (3,7),
  replay + listener on 0,1. nice 19, -j4 builds, `--rm` everything. OWRX untouched.

### 1.1 The "N450-equivalent" sandbox (Stuart's idea)

The server container is pinned to CPUs 3+7 (one core + its hyperthread, like the N450) and throttled with a CFS quota
(`--cpu-period 20000 --cpu-quota …`; a 20 ms period, because the default 100 ms made the throttling bursty — audio gaps
an Atom would not have). Calibrated against Danny's real figures (memory `n450_netbook_host.md`, 11.0.5 amd64, RTL
2.4 MS/s WFM, 20 fps, ~1 listener → **13.8 of 20 fps**, CPU saturated; 1.2 MS/s → **19.7 fps**, amber):

| quota (fraction of one i5 thread) | 2.4 MS/s fps | 1.2 MS/s fps |
|---|---|---|
| 0.080 | **13.8** (audio 879/1250 frames) | 17.3 |
| 0.090 | 15.4 | **19.4** |
| 0.095 | 17.0 | 20.0 |
| 0.100 | 17.2 | 20.0 |
| 0.110 | 18.9 | 20.0 |
| 0.120 | 20.0 | 20.0 |

No single quota fits both points exactly: 0.080 reproduces 2.4 MS/s exactly but is too harsh at 1.2; 0.090 fits 1.2
but is too kind at 2.4. **0.085 is used as the N450 model** (between the two; both points within ~1.5 fps). The
mismatch is in the direction you would expect: the real box also pays for USB bulk transfers and libusb at 2.4 MS/s,
which the TCP replay does not. ★ **The sandbox cannot capture in-order effects** — a throttled i5 is a fast core
running less often, not a slow core — so it shows *how much less work* a build does, and instructions remain the
second, independent proxy. As a cross-check the instruction rate agrees with Danny's experience: the 11.0.5 server
needs **~907 M instructions/s at 2.4 MS/s** and **~750 M/s at 1.2 MS/s**; a 1.66 GHz Bonnell at ~0.5–0.7 IPC retires
roughly 0.8–1.2 G/s — saturated at 2.4, amber at 1.2, exactly what Danny saw.

## 2. Where the time goes (11.0.5 + FTZ, before any change)

Instruction share of the whole server, 2.4 MS/s WFM + RDS, 20 fps, one Opus listener (perf record -e instructions):

| stage (thread) | amd64 | i386 | what it is |
|---|---|---|---|
| `FirDecimator::process` (vibe-dsp) | **28.5 %** | **26.6 %** | the channel decimation cascade, complex, at the full IQ rate |
| `RealFir::process` (vibe-demod) | 12.6 % | 13.1 % | MPX / audio low-pass filters |
| `RxPipeline::demodTail_` (vibe-demod) | 10.6 % | 10.2 % | mostly the inlined **MpxNoiseMeter** — five biquads per MPX sample (vibedsp.h:1008) |
| pffft `passf4/passf2/transform/finalize` + `powerDbShifted` (vibe-spec) | 16.3 % | 17.4 % | the spectrum FFT (engine runs at 4× the 20 fps view and averages) |
| `onSpectrum` + `nth_element` + `emplace_back` (vibe-dsp) | 8.0 % | 7.0 % | per-frame band analysis (floor, shoulders, contrast) |
| `StereoPLL::processBlock_` | 4.8 % | 4.2 % | pilot PLL |
| `RdsDemod::process` | 3.5 % | 2.6 % | |
| `IqCleaner::process`, `NCO::mix` | 5.1 % | 5.0 % | DC/IQ correction, the tuning mixer |
| `enqueueIq` (vibe-tcp) | 1.1 % | 1.0 % | **u8 → float conversion** — already SSE2, too small to matter |
| libopus (per listener) | 5.5 % | 5.6 % | ≈ 45–55 M instructions/s per listener on amd64, ≈ 68 M/s on i386 |
| libm `floor`/`ceil` (i386 only) | — | 1.6 % | real calls on 32-bit (no SSE4.1 `round`, x87 return) — from the spectrum rebinning |

Threads: vibe-demod 42 %, vibe-dsp 37 %, vibe-spec 20 %, vibe-tcp 1 %.
**i386 vs amd64**, same source: **+7.3 % instructions, +13.5 % cycles** at 2.4 MS/s (8 GPRs, PIC, x87 float returns).
The DAB chain (bench, 2.048 MS/s) is light by comparison: `DabReceiver::push` (OFDM) and the MSC Viterbi
(`decodeLogicalFrame`) about 1.5 % each of the whole bench.


## 3. What each lever gave

All instruction figures are M instructions per second of signal, medians, perf stat (spread < 0.2 %).

### 3.1 Code (kept — committed)

| change | stage effect (instruction samples) | commit |
|---|---|---|
| **x86 FIR over aligned loads**: tap banks pre-shifted for each 16-byte misalignment (2 complex / 4 real), pair-duplicated complex taps (no shuffle), zero-padded (no scalar tail), 4 accumulators. SSE2. | FirDecimator **−34 %** amd64 / **−28 %** i386; RealFir **−23 % / −24 %** | 21fe0cf7 |
| **FIR delay line never shrunk** (all platforms): `resize` up and down each call zero-filled every block | `_M_default_append` 1308 → 508 | 21fe0cf7 |
| **MpxNoiseMeter constant-skirt biquad** (b1 = 0, b2 = −b0): 3 mul + 3 add instead of 5 + 4, shorter chain | demodTail_ **−21 %** amd64 / **−16 %** i386 | d97282ed |
| **Spectrum analysis**: indexed writes instead of push_back per bin; floor/ceil by truncation; nth_element prefix | emplace_back −1.5…2 % of server; i386 libm floor/ceil gone | ba9ff5c3 |
| **DAB Viterbi pshufb lookup by runtime dispatch** (SSSE3 kernel in `target("ssse3")`, chosen once by `__builtin_cpu_supports`) | Viterbi **−45 %** amd64 (45.8 → 25.4 ns/bit), **−48 %** i386 (49.5 → 25.8); bit-identical; forced-SSE2 path unchanged (dispatch cost nil) | 8b83323a |
| **Decimators step from kept output to kept output** (all platforms): the per-input countdown was a read-modify-write of `phase_` in memory per input sample — 100 empty iterations per output at decimate-by-101 | whole server WFM a further **−3.5 %** (both arches); narrow SSB in test-passband-cost's harness **−24…−36 %** (i5 time); fixed the narrow-USB slowdown the banks had exposed | b0287e14 |
| **x86 banks capped at 8 KB per filter** (long narrow-SSB filters keep the 1× taps so they fit Bonnell's 24 KB L1d with their delay line) | i5 prefers uncapped by ~5 % on narrow SSB only; the cap is for the Atom | b0287e14 |

Microbench of the FIR kernels (ns per input sample on the i5, amd64 / i386):

| shape | complex old → new | real old → new |
|---|---|---|
| K=31 D=4 | 3.17 → 1.89 / 3.78 → 2.07 | 1.55 → 1.55 / 2.60 → 1.68 |
| K=63 D=8 | 2.52 → 1.73 / 3.74 → 1.84 | 1.40 → 1.24 / 2.59 → 1.31 |
| K=137 D=2 | 13.7 → 11.3 / 18.3 → 11.2 | 7.63 → 6.29 / 11.6 → 6.56 |
| K=275 D=1 | 53.0 → 41.7 / 71.0 → 42.3 | 31.5 → 22.3 / 42.9 → 22.4 |
| K=1117 D=1 (loaded box) | 346 → 148 | 159 → 77 |

Accuracy vs a double-precision reference (60 shapes × random block sizes): worst relative error complex
1.01e-7 → **8.7e-8**, real 9.7e-8 → **7.5e-8** — slightly *better* (four partial sums).

**Whole server, all code changes together** (k2 = everything above; amd64/i386):

M instructions per second of signal (lower is better), whole server, perf stat, 2 interleaved rounds; i5 Mcycles/s in
brackets (secondary — out-of-order):

| build | amd64 2.4 MS/s | amd64 1.2 MS/s | i386 2.4 MS/s | i386 1.2 MS/s |
|---|---|---|---|---|
| **11.0.5-1 as released** | 908 (504) | 750 (371) | 973 (584) | 792 (417) |
| main e4c2e36a (+FTZ) | 908 (499) | 751 (373) | 973 (555) | 792 (413) |
| + FIR banks, noise meter, spectrum analysis, Viterbi (k2) | 751 (450) −17.2 % | 604 (332) −19.5 % | 825 (499) −15.2 % | 661 (366) −16.6 % |
| + decimator stepping (k3 = all code changes) | 728 (437) **−19.9 %** | 587 (321) **−21.7 %** | 795 (493) **−18.3 %** | 638 (348) **−19.4 %** |
| **test package +atom2** (k3 + PGO + `-mtune=bonnell`) | **719 (456) −20.8 %** | **578 (331) −23.0 %** | **752 (489) −22.7 %** | **602 (353) −23.9 %** |
| test package i386 +atom2g (k3 + PGO, generic tuning) | — | — | 751 (505) −22.9 % | 603 (361) −23.9 % |

(percentages vs the released 11.0.5-1; the two rows above k3 are from separate runs of the same harness.) The i386
package now needs **fewer instructions than the released amd64 one** at both rates. `-march=bonnell` on top of k2:
amd64 +2.3 % / +1.5 %, i386 +1.6 % / +1.7 %.

### 3.4 The other modes (vibeserver-bench, k2 vs main — measured before the decimator-stepping commit)

`vibeserver-bench` 3 s per scenario, 2 interleaved rounds, medians; % of one i5 core summed over the row's threads
(the bench's own timers — 0.1 % resolution, so the small rows are coarse). Whole bench under perf stat:
**42.9 → 39.3 G instructions amd64 (−8.4 %), 47.7 → 44.6 G i386 (−6.6 %)** — the bench includes signal generation,
the decoders and DAB setup, which these changes do not touch.

| row | amd64 base → k2 | i386 base → k2 |
|---|---|---|
| WFM stereo 2.4 / 2.048 / 1.024 MS/s | 4.60 → 4.20 / 4.20 → 3.50 / 3.65 → 3.20 | 4.80 → 4.40 / 4.00 → 3.75 / 3.90 → 3.25 |
| WFM + Advanced RDS 2.048 | 10.65 → 10.10 | 11.55 → 10.65 |
| NFM / AM / SSB 2.048 | 1.0 → 0.8 / 0.8 → 0.7 / 0.7 → 0.6 | 1.0 → 0.8 / 0.8 → 0.7 / 0.7 → 0.6 |
| locked-range WFM listener | 3.20 → 3.00 | 3.90 → 3.40 |
| decoders (RTTY, NAVTEX, WEFAX, SSTV, time, FT8) | unchanged (not touched) | unchanged |
| DAB+ / DAB+ with label scan | 3.30 → 3.15 / 4.30 → 3.90 | 3.60 → 3.50 / 4.70 → 3.90 |

### 3.2 Compiler flags (measured on top of the code changes — none kept for the official packages)

| lever | amd64 instr 2.4 / 1.2 | i386 instr 2.4 / 1.2 | verdict |
|---|---|---|---|
| `-mtune=bonnell` (tuning only, floor unchanged) | +2.7 % / +2.3 % | +1.5 % / +1.4 % | more instructions *by design* (in-order scheduling: loads split from arithmetic, `lea` choices); the i5 cannot show whether Bonnell gains — **in the test packages, for Kiko to A/B** |
| `-march=bonnell` (whole binary may use SSSE3/MOVBE — the ceiling a raised floor buys) | **+2.3 % / +1.5 %** | **+1.6 % / +1.7 %** | GCC finds almost nothing to do with SSSE3 in this code: 174–206 SSSE3 instructions in a 784k-instruction binary, none hot. **A raised floor buys nothing measurable.** |
| `-fno-math-errno -fno-trapping-math` | −0.7 % / −0.8 % | −0.2 % / −0.4 % | noise-level; not worth a global semantics flag |
| LTO (`-flto`) | +0.4 % / +0.3 % | 0.0 % / +0.3 % | binary 9 % smaller, no speed |
| i386 `-fno-pie -no-pie` (frees EBX) | — | +0.3 % / +0.2 % | the hot loops are inlined: no PIC thunks in them |
| `-O3` vs `-O2` | — | — | already `-O3` (CMake Release; vibedsp explicitly `-O3 -ffp-contract=fast`) |
| **PGO** (`-fprofile-use`, trained on this workload + the bench) | **−3.9 % / −3.4 %** | **−5.3 % / −5.3 %** (cycles −6.8 / −6.2 %) | the only flag lever that pays; in the test packages via `VIBE_PGO` (68ea586e) |
| FTZ/DAZ (the other agent's e4c2e36a) | 0 on this noisy RTL input | 0 | big on clean/quiet input, already on main; included in every build here |

### 3.3 Codecs

* **DAB+ AAC is not ours to optimise.** `vibe_dab_aacdec.h` hands ADTS to the **operating system's decoder** (ffmpeg
  on Linux, AMediaCodec on Android, AudioToolbox on Apple) — "WE STILL SHIP NO AAC DECODER, AND THAT IS DELIBERATE".
  Debian's ffmpeg already carries runtime-dispatched SSE/SSSE3 assembly. Nothing to do on our side.
* **MP2** (`vibe_dab_mp2.h`, in-house) already has SSE2 kernels; not visible in the DAB profile.
* **The DAB Viterbi** was the natural SSSE3 fit and got it (above).
* **Opus (per listener)**: system libopus0 1.3.1 vs the same source built static (encode of 60 s at the server's
  settings — 48 kHz stereo, AUDIO, MUSIC, complexity 6):

  | build | amd64 instructions | i386 instructions |
  |---|---|---|
  | Debian libopus0 | 3.271 G | 4.092 G |
  | static, `-O2` (i386 with `-msse2 -mfpmath=sse`) | 3.270 G (0.0 %) | 3.967 G (**−3.1 %**) |
  | static, `-O2 -march=bonnell` | 3.342 G (+2.2 %) | 3.999 G (−2.3 %) |

  At ~5.5 % of the server per listener, a bundled libopus would save ≤ 0.2 % of an i386 server and nothing on amd64,
  while taking security updates away from Debian. **Recommendation: keep the system libopus.**
* **u8 → float with pshufb**: `enqueueIq` is 1.0–1.3 % of the server; pshufb would save ~2 of 22 instructions per
  16 bytes. Not worth a dispatch. **pmaddubsw on raw u8 in the first FIR** is not possible without restructuring the
  chain: the FIR's input is not the raw u8 (the NCO mix and IQ cleaning happen in float first), and 8-bit taps would
  cost stopband depth.


## 4. The floor: keep SSE2, dispatch SSSE3

**Recommendation: SSE2 stays the package floor on both i386 and amd64.** Reasons, in order:

1. **A raised floor buys nothing measurable.** `-march=bonnell` on the whole binary *adds* 1.5–2.3 % instructions; GCC
   emits a couple of hundred SSSE3 instructions in a 784k-instruction binary and none in a hot loop. The hot loops are
   hand-written intrinsics, and flags do not rewrite them (the earlier −0.2 / −0.3 % SSE3/SSSE3 result, confirmed).
2. **Where SSSE3 genuinely pays — the DAB Viterbi's table lookup (−45 %) — runtime dispatch gets all of it** with the
   floor untouched, at no measurable cost (forced-SSE2 path = old timing). The pattern is now in the tree
   (`vibe_dab_fec.h`): `__attribute__((target("ssse3")))` kernel, `__builtin_cpu_supports("ssse3")` once into a static,
   `VIBE_NO_SSSE3=1` to force the SSE2 path.
3. **An SSSE3 floor would exclude** every pre-2011 AMD (Athlon 64, Sempron, Phenom/Phenom II, Athlon II — K8/K10 have
   SSE3 and SSE4a but no SSSE3) and Intel Core Solo/Duo (Yonah) — exactly the cheap developing-world hosts this project
   targets, many of which outrun an Atom.
4. The biggest Atom win (the aligned-load FIR, −28…−34 % on the hottest stage) is **plain SSE2**.

`-mtune=bonnell` does not change the floor (it runs on any x86); it is a scheduling choice the i5 cannot judge, so it is
in the test packages for Kiko to measure on the real N270.


## 5. The N450 sandbox, before → after

Quota 0.085 of one i5 thread on one core's two SMT siblings (§1.1), 2.4 and 1.2 MS/s WFM + RDS, one listener, 20 fps;
3 interleaved rounds, medians; audio = Opus frames delivered in the 30 s window (**1500 = none lost**).
"before" is the **released 11.0.5-1 binary**, extracted from the package.

| build | 2.4 MS/s spectrum fps | 2.4 MS/s audio frames | 1.2 MS/s fps | 1.2 MS/s audio | 1.2 MS/s server CPU (cap 8.5 %) |
|---|---|---|---|---|---|
| amd64 11.0.5-1 (Danny today) | 15.2 (15.2–15.5) | 1190 (79 %) | 19.83 | 1500 | 8.9 % (at the cap) |
| **amd64 +atom2** (Danny's test package) | **16.8** (16.7–17.2) | **1347 (90 %)** | **19.93** | **1500** | **7.9 %** |
| i386 11.0.5-1 (Kiko today) | 13.8 (13.6–14.1) | 1081 (72 %) | 18.2 (18.2–18.6) | 1491 | 8.9 % (at the cap) |
| **i386 +atom2** (Kiko primary) | **15.9** (15.9–16.0) | **1247 (83 %)** | **19.87** | **1499** | 8.6 % |
| i386 +atom2g (generic tuning) | 15.7 (15.0–15.7) | 1232 (82 %) | 19.93 | 1500 | 8.6 % |

(An earlier round on the packages built before the decimator-stepping commit gave amd64 16.6 fps / 1336 frames and
i386 15.8 / 1239 at 2.4 MS/s — the same picture.)

Read it as: on the modelled N450, **2.4 MS/s goes from 15.2 to 16.8 fps and from 21 % of the audio lost to 10 %** —
better, still not enough for 2.4 MS/s on an N450 (Stuart's low-end recipe of 1.2 MS/s stands). **1.2 MS/s goes from
"at the limit" to having headroom** (server CPU under the cap; on i386 from 18.5 fps and dropped audio to a clean
19.9 fps with every frame). The i386 package on the same model now beats the *released amd64* one at both rates.
(The +atom2 vs +atom2g difference on the i5 is within what a throttled out-of-order core can tell apart — see §8.)


## 6. Accuracy and tests

* **FIR kernels vs double precision** (60 shapes, random block sizes, all output counts identical): worst relative error
  complex 1.01e-7 → 8.7e-8, real 9.7e-8 → 7.5e-8. The decimator-stepping change gives the identical outputs and phase.
* **DAB Viterbi**: decoded bits hashed over 300 × 3072-bit noisy blocks — identical for the old kernel, the SSSE3 kernel
  and the forced-SSE2 path (`cd36a25d814e9a3f`, the same 3 bit errors in 921 600 in all three).
* **vibedsp suites** (adjacent, cfft, cfft_kernel, channelizer, demod, iqclean, passband, pipeline, rds, rds_dsp,
  zoomspec, the FFT suite; spectrum_processor where node could make its vectors): **all pass on amd64 and i386 (SSE2,
  the package flags) in the bookworm containers, and on arm64 (the Mac, NEON) — on the final code.**
* **The DSP/DAB suites from run-tests.sh, final code, amd64 and i386**: test-dab-fec, test-dab-receiver,
  test-wfm-stereo, test-stereo-highblend, test-resampler-rate, test-multipath-meter, test-rds-ps-frames,
  **test-passband-cost** — all pass (passband-cost run on a quiet pinned core: it is a timing test).
* **scripts/run-tests.sh, whole, in the containers**: amd64 82 suites pass; the failures are the suites that need node,
  a browser or a full Python (absent from the build image) plus **test-rtty-shift** (3 cases), which fails identically
  on the untouched baseline (the FTZ agent's log shows the same three) — RTTY decoder logic this work does not touch.
  i386 the same, plus: `test-mpx-measure` **cannot be compiled by the 32-bit cc1plus** ("virtual memory exhausted")
  — identical on the baseline source, a test-harness limit, not a code fault (the server's own mpxmeasure.cpp builds);
  test-ft8-passes and test-log-latch failed once while I was running timing experiments beside them and **pass on a
  quiet rerun, old and new source alike**.
* **ctest** (the CMake-registered suites: DAB, health snail, power dips, mDNS, crash report…): **25 / 25 on amd64 and
  25 / 25 on i386**.
* **The Mac (arm64)**: run-tests.sh — **165 C++ suites pass**; the 17 failures are the web/JS checks that need
  `web/dist` and node_modules, which the scratch worktree did not have. The SSSE3 code is behind `__SSE2__`/GCC-x86
  guards; ARM compiles none of it. **The iOS lib was not rebuilt** (vibedsp changed: ddc.cpp, simd_internal.h,
  vibedsp.h — for the batch build).


## 7. Test packages (never published)

Built through `scripts/build-x86-debs.sh` itself (bookworm containers on the Lenovo, same gates: Architecture,
settings wizard in the binary, glibc baseline — `libc6 (>= 2.35)`, as 11.0.5-1), PGO-trained with
`VIBE_PGO=generate|use` on the sandbox workload (2.4 and 1.2 MS/s WFM with a listener) plus `vibeserver-bench` (NFM, AM,
SSB, Advanced RDS, decoders, DAB). Source = e4c2e36a (FTZ) + the seven commits of this work (not the installer commit 2904eb73, which landed meanwhile and does not touch the server). In `vibeserver/build/prebuilt/`:

| file | for | contents | sha256 |
|---|---|---|---|
| `vibeserver_11.0.5-1+atom2_amd64.deb` | **Danny** (N450) | code + PGO + `-mtune=bonnell`, SSE2 floor | cea12476…f9ba |
| `vibeserver_11.0.5-1+atom2_i386.deb` | **Kiko** (N270) — primary | code + PGO + `-mtune=bonnell`, SSE2 floor | 48b10c1c…fc41 |
| `vibeserver_11.0.5-1+atom2g_i386.deb` | Kiko — the A/B | code + PGO, generic tuning | ca1220e4…31af |

★ `1+atom2` sorts above `11.0.5-1`, so it installs as an upgrade; the published package (or 11.0.6) replaces it later.
★ The earlier `+atom1` i386 package was **not** Bonnell-tuned: `-mtune=bonnell` in `VIBE_EXTRA_CFLAGS` was overridden by
the i386 ISA line's `-mtune=generic` (fixed: 81486029 / 68ea586e).

### Test recipe for Kiko (technical; ~20 minutes)

> Kiko — three builds of the same 11.0.5, please measure each the same way. Install one at a time with
> `sudo apt install ./vibeserver_11.0.5-1+atom2_i386.deb` (then `+atom2g`, then back to the current one with
> `sudo apt install --allow-downgrades ./vibeserver_11.0.5-1_i386.deb`); the service restarts itself. For each: set the
> RTL-SDR to **2.4 MS/s**, tune a strong **WFM station with RDS**, open **one** browser listener with the spectrum at
> **20 fps**, wait a minute, then run `top -b -d 2 -n 30 -p $(pidof vibeserver) | awk '/vibeserver/{s+=$9;n++} END{print s/n}'`
> (average CPU %) and note the spectrum fps the client shows and `dspCpu` from `curl -s localhost:48000/vibeserver.json`.
> Repeat at **1.2 MS/s**. Finally run the **server benchmark** from the setup page once per build and send the rows
> (WFM 2.4 / 2.048 / 1.024, NFM, AM, SSB). That gives us real-Bonnell numbers for: the code changes (current vs either
> atom2), and in-order tuning on its own (`+atom2` vs `+atom2g`). If anything misbehaves, `sudo systemctl edit vibeserver` with
> `[Service]` / `Environment=VIBE_NO_SSSE3=1` forces the DAB Viterbi back to its SSE2 kernel (the only runtime-dispatched code).

For **Danny** (not technical): just the one package, `+atom2_amd64`, installed by Stuart's instructions; the useful
measurement is the delivered fps at 2.4 MS/s — Stuart can read it remotely as a listener on "Acer Netbook RTL-SDR"
(with Danny's agreement; not done tonight).


## 8. What needs Kiko's real N270 (or Danny's N450) to confirm

1. **The absolute gain on an in-order core.** The instruction cut (−21…−24 %) is the honest proxy; the i5's cycles moved
   −10…−16 %. On Bonnell the FIR change should do *better* than its instruction count says (it removed unaligned loads,
   which Bonnell pays several times over and the i5 hardly at all) — but that is a prediction, not a measurement.
2. **`-mtune=bonnell` on its own.** +1.5…2.7 % instructions by design; whether the in-order schedule wins that back
   (and more) can only be seen on Bonnell: `+atom2` vs `+atom2g`.
3. **PGO trained on synthetic input.** The profile came from the generated station and the bench; real RF (multipath,
   weak RDS, AGC activity) takes some different branches. `-fprofile-partial-training` keeps untrained code optimised
   normally, so the risk is only a smaller gain.
4. **Hyper-Threading.** The N450/N270 have one core and two threads; the sandbox models that with two SMT siblings of
   one i5 core under a quota, but Bonnell's HT shares a narrower pipeline.
5. **USB.** The sandbox feeds rtl_tcp over loopback; the netbooks pay libusb + the kernel's bulk transfers at 2.4 MS/s
   (the likely reason the calibration fits 1.2 MS/s better than 2.4).


## 9. Notes

* **The Lenovo's own VibeServer was never stopped.** Stuart allowed using its SDRplay for real-radio runs, but Danny's
  and Kiko's radios are RTL-SDRs (u8 IQ) and the SDRplay path is compiled out of i386, so the paced RTL replay was the
  faithful input; nothing on the box was changed (no sudo was used or needed). OWRX and watchtower untouched.
* **i386 still costs +7 % instructions / +13 % cycles over amd64** for the same source (8 GPRs, PIC, float returns
  through x87 per the i386 ABI — ~860 x87 instructions remain in the binary, none in the hot loops now that the
  spectrum rebinning's floor/ceil are gone). Not worth more: the i386 package is now cheaper than the released amd64.
* **Not pursued, with reasons:** halving the spectrum engine's FFT rate (it averages 4 FFTs per 20 fps frame — a visible
  design choice, not a free win); the per-frame contrast percentile (an AGC input — exact only, so only the prefix
  trick); hand-written SSSE3 for u8→float (1 % stage); `palignr` sliding windows (the tap banks made every load aligned
  without it); a bundled libopus (§3.3).
* **Reproduce:** `tools/atom-bench/` (images.sh, build.sh, sandbox.sh, ab.sh/abstat.py, sbab.sh/sbstat.py,
  pgo-train.sh, debs.sh, the microbenches). Raw results stay on the Lenovo in `~/atomwork/results` and `~/atomwork/perf`.
