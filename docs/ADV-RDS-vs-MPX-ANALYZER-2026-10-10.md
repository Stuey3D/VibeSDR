# Advanced RDS vs mrwish7's sdrpp-mpx-analyzer — measured (2026-10-10)

Stuart: *"Can you run it through their tool?"* · *"add the sign and do an investigation between ours and theirs … run it
against all the stations you find"*. Context and the code-reading comparison: `~/Desktop/ADV-RDS-vs-sdrpp-mpx-analyzer.md`.
Decision already taken: Advanced RDS stays the instant, on-the-move tool — no MPX-analyser heft (see memory).

## Method
- **Recordings:** 65 s of raw IQ, 8-bit, 2.4 MS/s, full-rate raw IQ out from the Pi 500's RTL-SDR Blog V4L (YouLoop),
  one station at a time, only while that radio had no listeners, the radio put back on 648 kHz AM after each. 21
  stations — every FM station the Pi 500 has learned. ("96.0" was dropped: it is 96.1 Flex FM seen off-channel.)
- **Theirs:** `src/mpx_dsp.h` at commit 2412063, **unmodified**, compiled headless against SDR++'s own DSP headers
  (commit 8c9f5ee) with plain stand-ins for the six VOLK kernels it calls; our 2.4 MS/s resampled to the 384 kS/s its VFO
  delivers (windowed sinc, flat to ±160 kHz — a wide channel, not an IF filter).
- **Ours:** vibedsp `RxPipeline` + `MpxMeasure` — the server's own code — as shipped in RC34 and as fixed tonight.
- **Known truth:** synthetic broadcasts with an exactly known RDS-to-pilot phase (two independent generators).

## Findings
1. **MPX power (BS.412) and pilot agree.** On clean stations within ~0.2 dB and ~0.02 kHz. Ours is up to 1.2 dB lower on
   noisy stations — by design: ours subtracts the noise power, theirs counts it.
2. **The RDS-to-pilot phase:**
   - **Theirs is exact** on both synthetic generators (±30°, ±70° read back to 0.1°).
   - **Ours had the opposite sign** (our 57 kHz reference is sin(3θ)) — it only ever showed an unsigned 0–90°, so this
     was invisible. **Fixed:** a new signed figure, + = RDS leads the pilot's third harmonic (the standard sense).
   - **Ours was biased by programme audio:** +1.2° mono, +1.5° stereo, +2.7° at 1.4× level, ≈0 with pilot + RDS alone —
     proportional to programme POWER. The pilot PLL's wide (~190 Hz) loop is pulled by the composite it sees. It also read
     the pilot low (6.716 / 6.690 kHz against a true 6.75). **Fixed** in the instrument's own PLL (MpxMeasure — the second
     wide demodulator; the listener's stereo PLL is untouched): acquire wide, narrow to ~38 Hz after 1.5 s tracked, wide
     again after a hole. Synthetic bias now ~0.3°; pilot 6.739.
   - On clean real stations the signed figure now agrees with theirs within about ±2.5° (median +0.5°), sign included.
3. **RDS level:** ours reads ~8–10 % higher, almost entirely the crest-factor constant (ours 1.507, calibrated against
   Pira data; theirs the theoretical 1.444).
4. **Peak deviation:** theirs reads 6–24 kHz higher on clean stations and 100–200 kHz higher on noisy ones — it takes the
   raw sample peak (their README says it over-reads). Ours matched the true 125 µs peak on synthetic signals. Treat
   theirs as an upper bound.
5. **Weak signals:** both struggle. Theirs reports impossible deviations (240–300 kHz); ours loses pilot lock (coherence
   0.02–0.05) and its pilot figure reads far too low. Neither phase means anything there.
6. **Robustness gap (ours, not real-world so far):** on a deliberately crude synthetic (unshaped biphase) ours failed to
   lock at +30° only. Real RDS is shaped; noted, not chased.

## Per station (end of each 65 s run)
| MHz | station | MPX power (dBr) theirs / ours | pilot (kHz) theirs / RC34 / fixed | RDS (kHz) theirs / ours avg | RDS–pilot phase theirs / RC34 (unsigned) / fixed (signed) | peak dev (kHz) theirs max / ours hold | our coherence |
|---|---|---|---|---|---|---|---|
| 88.6 | BBC R2 | +3.73 / +3.73 | 6.06 / 6.07 / 6.08 | 2.03 / 2.20 | -37.8° / 35.7° / -36.6° | 83 / 66 | 0.89 |
| 88.9 | BBC R2 | +0.74 / +0.50 | 6.03 / 6.04 / 6.06 | 2.16 / 2.34 | -43.4° / 42.4° / -43.2° | 85 / 61 | 0.80 |
| 89.5 | BBC R2 | -0.54 / -1.16 | 5.81 / 5.81 / 5.82 | 1.93 / 2.20 | -25.6° / 27.1° / -27.2° | 82 / 55 | 0.55 |
| 90.1 | BBC R2 | -2.01 / -2.10 | 5.94 / 5.95 / 5.96 | 1.96 / 2.18 | -38.2° / 37.8° / -38.3° | 75 / 64 | 0.84 |
| 92.3 | BBC R3 | -0.88 / -0.85 | 5.90 / 5.92 / 5.92 | 1.90 / 2.09 | -37.8° / 37.3° / -37.4° | 71 / 34 | 0.99 |
| 93.0 | BBC R4 | -0.39 / -0.54 | 5.90 / 5.91 / 5.92 | 1.78 / 2.05 | -32.3° / 28.8° / -30.3° | 80 / 66 | 0.84 |
| 93.9 | BBC R4 | -1.29 / -2.07 | 5.91 / 5.93 / 5.93 | 1.85 / 2.50 | -65.0° / 64.8° / -64.7° | 115 / 59 | 0.39 |
| 94.5 | BBC R4 | +0.10 / -0.83 | 5.97 / 5.97 / 5.98 | 1.59 / 2.75 | -28.4° / 24.3° / -24.3° | 171 / 60 | 0.41 |
| 96.1 | Flex FM | +8.55 / +8.41 | 6.64 / 6.53 / 6.61 | 3.44 / 4.84 | -13.9° / 9.1° / -11.8° | 123 / 85 | 0.45 |
| 96.6 | Heart | +7.19 / +7.21 | 6.74 / 6.69 / 6.75 | 2.25 / 2.41 | -32.0° / 28.6° / -31.4° | 80 / 74 | 0.97 |
| 98.2 | BBC R1 | +8.87 / +7.93 | 5.37 / 4.87 / 5.26 | 4.91 / — | -14.7° / 53.4° / -60.8° | 242 / 96 | 0.04 |
| 98.5 | BBC R1 | +12.39 / +11.19 | 4.04 / 0.64 / 0.64 | 7.56 / — | -23.6° / 82.9° / -82.9° | 253 / 96 | 0.03 |
| 99.1 | BBC R1 | +11.47 / +10.28 | 4.05 / 0.87 / 1.23 | 0.00 / — | -66.3° / 34.0° / -5.3° | 245 / 96 | 0.03 |
| 99.7 | BBC R1 | +6.77 / +6.24 | 5.95 / 5.92 / 5.96 | 0.00 / 3.58 | -48.1° / 49.4° / -49.5° | 171 / 68 | 0.14 |
| 100.1 | Classic FM | +1.68 / +1.47 | 6.64 / 6.65 / 6.66 | 2.08 / 2.46 | -51.1° / 16.1° / 14.0° | 79 / 60 | 0.06 |
| 100.4 | Classic FM | -0.66 / -0.68 | 6.55 / 6.56 / 6.57 | 2.38 / 2.56 | +65.6° / 36.0° / 35.2° | 74 / 57 | 0.52 |
| 101.3 | Classic FM | +0.45 / +0.25 | 6.80 / 6.81 / 6.82 | 2.19 / 2.56 | +78.4° / 4.0° / 3.7° | 159 / 57 | 0.08 |
| 101.9 | Classic FM | +14.82 / +14.25 | 6.15 / 2.00 / 2.17 | 0.00 / — | +26.1° / 44.4° / 21.8° | 299 / 96 | 0.03 |
| 102.3 | HFM | +15.76 / +15.42 | 16.60 / 11.04 / 11.07 | 0.00 / 21.73 | -42.4° / 70.1° / -9.8° | 294 / 96 | 0.05 |
| 104.2 | BBC Northampton | +3.71 / +3.72 | 6.07 / 6.06 / 6.08 | 1.91 / 2.06 | -69.3° / 65.5° / -66.8° | 77 / 70 | 1.00 |
| 104.7 | Horizon | +8.63 / +8.63 | 5.90 / 6.03 / 5.96 | 3.58 / 3.58 | -90.0° / 6.6° / -1.4° | 117 / 79 | 0.62 |

Peak deviation: theirs = max since reset; ours = the published hold (decays). RDS "—" = ours had no decoded RDS to
gate on. "Coherence" is ours — read the phase only where it is high (≥ 0.8).

## Tests added
`test-mpx-measure` — signed phase at ±30°/±70° within 1° (75/75). Harnesses (not in the repo): run_theirs, run_ours,
gen_known, recfm.mjs — see memory `iq_close_deadlock_and_rds_phase`.
