# METEOR LRPT sensitivity (V12, 2026-11-24): pictures from weaker passes

Branch `lrpt-sens` (off `v12-experimental` ca0a713a5). Stuart: *"can we improve the lrpt decoder sensitivity?"* — his
Pi 500 RTL-SDR V4 on a loop aerial was at the edge: 24 Nov 10:55 METEOR-M2-3 gave "peak SNR 3.8 dB · 1 frame (63
failed), 8 lines" live; 09:20 gave 0 frames. Everything below was measured on the same IQ before and after.

**Headline.** Synthetic OQPSK 72k + differential (what N2-3 / N2-4 send): the 50 % frame point moved from **Es/N0
3.0 dB to 1.6 dB (−1.4 dB)**. Real passes replayed: the 10:55 target **0 → 97 frames (≈ 90 lines of picture in each
of channels 1, 2, 4)**, 09:20 **16 → 63**, 23 Nov 13:59 **98 → 148** frames (46 → 68–73 strips a channel). Noise still
gives no picture. CPU: unchanged on a decoding signal, +11 % for QPSK, **+22 % on pure noise** (more ASM candidates
tried — see §Cost).

## What changed (and what each was worth)

| Change | File | Synthetic o72d 50 % point | Real: frames over 4 captures × 3 start offsets |
|---|---|---|---|
| baseline (beta92) | | 3.01 dB | 168 |
| **carrier loop 0.005 → 0.002** (Bn·T; ≈ 360 → 144 Hz at 72k) | `vibe_lrpt_demod.h` | ≈ 1.9 dB | 342 |
| **timing loop 0.002 → 0.004** | `vibe_lrpt_demod.h` | level | 361 |
| **soft-decision Costas detector** | `vibe_lrpt_demod.h` | **1.60 dB** | 400 |
| **ASM find 46 → 44 of 52** (+ "sync" reported only once confirmed) | `vibe_lrpt.h` | level | **415** |

(The "4 captures" are 23 Nov 13:59, 22 Nov 14:19, 24 Nov 10:55 and 09:20; each replayed from 0, 0.37 and 0.81 s and the
frames averaged, because a single replay is chaotic — one change of lock timing early on moves the target pass by ±40
frames.)

1. **The carrier loop was too wide.** At Es/N0 2–3 dB a 0.005 loop's phase jitter alone cost OQPSK ≈ 1 dB (QPSK, whose
   points sit still between symbols, lost less). Synthetic says narrower still is better (0.001: 1.65 dB with the old
   detector), but **the real passes disagree**: 0.0015 348, 0.002 361, 0.003 319, 0.0005 far worse. The real carrier
   has more life in it than a clean oscillator (and a narrow loop is slow back after a fade), so the real captures set
   it. Not the RTL's oscillator: the x⁴ line on the 23 Nov pass is ≈ 1 Hz wide over 3.5 s (Wiener phase noise
   ≪ 0.1 Hz) — 1 Hz of Wiener noise in the synthetic would already cost 0.3 dB, 3 Hz 1.5 dB.
2. **Timing loop 0.004** beat 0.002 on the real passes (361 vs 342), level on synthetic.
3. **Soft decisions in the Costas detector.** sign(I)·Q − sign(Q)·I assumes each symbol decision is right; at 2 dB one
   rail in ten is wrong and kicks the loop at full weight. The ML form tanh(a·I)·Q − tanh(a·Q)·I, a = 2A/σ², weighs each
   by how sure it is. A and σ² come from the rail power for a design Es/N0 of 0 dB (a = 2√2/√P) — the adaptive version
   from the M2M4 reading was no better (1.66 vs 1.63 dB) and depends on a noisy number. tanh is a Padé form (no libm).
   Synthetic 1.89 → 1.60 dB; QPSK 1.89 → 1.42 dB; real 361 → 400.
4. **ASM search threshold 44.** On the differential path the marker is searched on the differentially decoded stream,
   whose hard error rate is ≈ 2× the channel's: at 2 dB ≈ 9–10 of the 52 fixed bits are wrong, and ≥ 46 matched one CADU
   in eight. 44: real 400 → 415 (the weak 09:20 pass 44 → 56). 42: 385 (false locks hold the search off the real
   marker). Noise passes 44 now and then; each costs one Viterbi that Reed–Solomon refuses, and **`FrameSync::locked()`
   now needs the marker again one CADU on (same ambiguity, ≥ 42) or an RS pass** — so the status / pass summary never
   says "sync" on noise (test-lrpt's noise case: "nothing heard").
5. **"A decode needs" +5 → +4 dB** (`Run::kNeedDb`): of the frames the new decoder got, 89 of 97 (10:55), 43 of 63
   (09:20), 60 of 148 (23 Nov) came while the reading said 3.5–4.5 dB.

## Tried and not kept (measured)

- **Partial frames (RS words independently).** No use: the four RS words are byte-interleaved, so one failed word spoils
  every fourth byte of every packet in the frame. And on the real passes failures were all four words or none (302 of
  302 failed CADUs on 23 Nov: every word −1).
- **Errors-and-erasures RS from the Viterbi's re-encoded path.** Built and tested (`Rs223::decode(cw, era, ne)`, 480/480
  within 2·err + era ≤ 32): erase the 8, then 16 bytes of a failing word that the re-encoded path disagreed with most
  strongly. Real +1 frame in 400, synthetic nothing. Why: a failing word at Es/N0 1.5 dB (AWGN, ideal coherent) has
  17–24 bad bytes and the doubt ranking puts only a third of them in its top 16. The decoder is kept (unused) for a
  SOVA's reliabilities. More than 16 erasures was not tried for the receiver: with ≤ 8 symbols of redundancy left a
  random word "decodes" ≈ 3 % of the time.
- **Third-order carrier loop** (to follow an unfed Doppler ramp with a narrow loop): unstable from a noise start — the
  rate integrator random-walks before the signal arrives. 0 frames.
- **FLL assist** (4th-power cross product): the x⁴ noise at 2 dB drives the frequency off; 0 frames either sign.
- **Gear shift** (wide loop until frames decode, then narrow): at the threshold frames rarely decode, so it stays wide —
  2.23 dB. 
- **Long sync flywheel** (24–60 missed markers after a proven lock, ±4–8 bit window): level at best (397–414 vs 415),
  worse when long. The real fades are seconds long and the signal under them is undecodable anyway.

## Where the remaining loss is

Ideal coherent BPSK soft bits into our Viterbi + RS fail 1.25 % of words at Es/N0 1.5 dB and almost none at 2.0 — the
code's own threshold is ≈ 1.2–1.3 dB. We are at 1.42 (QPSK) / 1.60 (OQPSK + differential): **≈ 0.2–0.4 dB of
implementation loss left**, most of it carrier jitter at the real-pass loop width. The real passes are limited by the
signal: on 24 Nov 10:55 the reading is above +1 dB Es/N0 for only ≈ 20 s (321–340 s), and 97 frames is most of that.

## Results

**Synthetic** (`vibeserver/tool-lrpt-sens.cpp`: 300 kS/s, the 22 Nov pass's −3.34 kHz radio error, −25 Hz/s Doppler
fed forward, 20 ppm clock, 0.1 Hz phase noise, 24 scan periods × 8 seeds; 80k: 16 × 4):

| Profile | 50 % frame point before | after | gain |
|---|---|---|---|
| OQPSK 72k + differential (N2-3 / N2-4) | 3.01 dB | **1.60 dB** | **1.4 dB** |
| same, Doppler NOT fed forward, +65 Hz/s (hand-tuned dial) | 3.14 dB | 1.73 dB | 1.4 dB |
| QPSK 72k (N2) | 1.89 dB | 1.42 dB | 0.5 dB |
| OQPSK 80k + differential, interleaved | 3.75 dB | 2.33 dB | 1.4 dB |

`test-lrpt`'s live-pass case (OQPSK + diff, 8 periods × 2 seeds): Es/N0 2.0 dB **82 %** of picture blocks (≈ 2 %
before; a new check needs ≥ 40 %), 3.0 dB 83 %, 3.5 dB 98 %, 5.0 dB 100 %.

**Real** (`tool-lrpt-replay <cf32> --engine --png …`, one replay from 0 s; PNGs of the composite in
`~/VibeSDR-test-audio/lrpt/sens-2026-11-24/before/` and `after/`, summaries `before.txt` / `after.txt`):

| Capture | Before: frames (failed) · strips ch 1/2/4 | After: frames (failed) · strips ch 1/2/4 |
|---|---|---|
| **24 Nov 10:55 M2-3** (1795517718, the target) | 0 (24) · — | **97 (605) · 11 / 12 / 11** |
| 24 Nov 09:20 M2-3 (1795512000) | 16 (72) · 2 / 3 / 2 | **63 (562) · 9 / 7 / 8** |
| 23 Nov 13:59 M2-4 (1795442388, the reference) | 98 (283) · 46 / 46 / 45 | **148 (1109) · 68 / 73 / 73** |
| 22 Nov 14:19 M2-4 (1795357190, LRPT-LIVE-PASS) | 60 (255) · 30 / 25 / 30 | **95 (964) · 50 / 52 / 47** |
| 13 others (22–23 Nov, incl. 1795448952, 1795462500) | 0 | 0 — peak reading ≤ 1.9 dB: no satellite above the noise |

A strip is 8 lines. "Failed" rose because the search now tries more candidates (§4) — each is refused by RS.

## Cost

M5, single thread, `-O3`: synthetic at Es/N0 3 dB — QPSK 0.54 → 0.60 % of a core, OQPSK + diff 0.77 → 0.74 %. Replays:
the 23 Nov pass (640 s) 7.05 → 7.27 s (+3 %); 99 s of pure noise 1.29 → 1.57 s (**+22 %**: about one extra Viterbi
a second on false marker candidates, and the QPSK/OQPSK contest's two demodulators). No new hot loop: the detector is
two Padé tanh and a square root a symbol (scalar, at symbol rate); the SIMD kernels are unchanged and test-lrpt's
SIMD-vs-scalar picture is still identical. Pi 2 (estimate, not measured): LRPT.md's 22–30 % of a core becomes
≈ 24–33 % on a decoding signal; a little more on noise.

Builds: arm64 NEON 70/0, `VIBE_FORCE_SCALAR` 67/0 (`--quick`), x86-64 SSE2 compiles (not run: no Rosetta here). armhf
not rebuilt.

## Tools

- `vibeserver/tool-lrpt-sens.cpp` — the curve: `--profile o72d|o72|q72|q72d|o80i --es 1:3:0.25 --seeds 8 --periods 24
  [--pn Hz] [--slope Hz/s] [--no-feed] [--seed0 n]`; `LRPT_JOBS=1` per seed, `LRPT_TRACE=1` the carrier each block.
- `vibe_lrpt_tx.h` gained `phaseNoiseHz` (Wiener).

## Open

1. **A live pass on the merged server** — the gains are from replays of the server's own captures, so they should
   carry, but the pass summary line on a real pass is the proof.
2. **80k: one seed in four never locks** its 0x27 marker / deinterleaver for the whole run, before and after this
   branch (base: seed 4 at 4.0 dB; now seed 3 at 3–4 dB, seed 4 at 4.5 dB) — the 80k mode is still synthetic-only; worth
   a look before anyone relies on it.
3. **More CPU on noise** (+22 %): an unattended station listening through long silences pays it. If it matters on a
   Pi 2, the search can drop back to 46 while the reading is under ~0 dB.
4. The last 0.2–0.4 dB: a SOVA (then the erasure decoder above has real reliabilities) or a decision-directed
   timing detector; neither tried.
