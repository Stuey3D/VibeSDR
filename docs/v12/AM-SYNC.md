# V12 — Synchronous AM, ECSS, AM Stereo (C-QUAM compatible) and AM AUTO BW

Branch `v12-am-sync` (off `v12-experimental`). VibeServer's own DSP (`vibedsp/amsync.cpp`, class `SyncAm`), so it
reaches every VibeServer platform, the Android app's local-hardware server and (after `build_ios.sh`) iOS.
Original code throughout, written from the published descriptions of each technique; no GPL source was copied.
"C-QUAM" is Motorola's trademark: everywhere in the product this is **"AM Stereo (C-QUAM compatible)"**.

Stuart's brief, in his words: *"actually make SAM do something — whenever I've enabled it on other servers it either
makes no difference or makes the signal worse."* Everything below was built and then judged against that sentence —
on synthetic signals where the truth is known, and on real night-time skywave recorded tonight.

---

## 1. What a listener gets

| key | wire id | what it is |
|---|---|---|
| **SAM** | `sam` | carrier-locked AM, both sidebands coherently; weighted toward the cleaner sideband where a neighbour dirties one |
| **SAM-U / SAM-L** | `sau` / `sal` | the same lock, ONE sideband (the other one's splatter rejected 48 dB at 400 Hz, 88+ dB above 1.5 kHz) |
| **ECSS-U / ECSS-L** | `ecssu` / `ecssl` | the same lock and detector with an **SSB-shaped, one-sided passband** (default 50–4500 Hz) |
| **AM-ST** | `amst` | AM stereo, C-QUAM compatible — stereo only while the 25 Hz pilot is received |
| AM AUTO BW | (the `autobw` switch) | narrows the AM family's passband against a neighbour; widens again by itself |

- **Offered only where the server lists them.** The six ids are in `/vibeserver.json` `modes` (never on an Icom, whose
  radio demodulates). An older VibeServer accepted `sam` and played it as plain AM — a key that changes nothing — so
  clients draw the keys only from that list (`src/services/amSync.ts amSyncOffered`). Kiwi, OpenWebRX and UberSDR keep
  offering only their own modes. Owners can switch each one off on the setup page.
- **ECSS vs SAM-U/L — why both exist (Stuart chose both).** The DSP is the same detector. The difference is the
  filter the listener drives: SAM-U/L keep AM's symmetric ±width passband and pick a side; ECSS-U/L give the SSB
  slider a DXer expects when zero-beating a carrier and taking one sideband (lo/hi edges, one-sided width, SSB's 12 kHz
  ceiling and its chain ladder). The PLL does the zero-beating, so there is no manual BFO to hold.
- **The AM sync line** (web: beside the frequency, in the airband channel line's style; app: under the AM AUTO BW row
  in the audio sheet, standard font): `SYNC +3.2 Hz` (the carrier's offset from the dial — a DX tool in its own right),
  `SYNC …` while acquiring, then `AUTO BW L 5.0k · U 3.9k` when auto bandwidth has cut, `NOTCH 1004 Hz` for a
  heterodyne. Carried by a new additive `{"type":"amsync"}` message (once a second, on change, only in the AM family).
- **AM-ST's light** is the existing stereo rings (web `#mStereo`, app mode box), lit by the 25 Hz pilot via the
  existing `rds` message's `stereo` flag.
- **AM AUTO BW** is the same server switch as FM's A-BW (one switch — `g_autoBwOn`), now also applied to every
  pipeline's AM auto bandwidth. It has its own row where an AM listener looks (web audio menu "AM AUTO BW", app audio
  sheet), each row kept in step with the FM one. **Manual + auto:** the listener's width is always the ceiling; auto only
  ever narrows inside it (per sideband in SAM/ECSS), and a retune clears every verdict.

## 2. The design (vibedsp `SyncAm`)

One carrier tracker, six detectors. The channel is rotated by a PLL until the carrier sits at 0 Hz / 0°: y = I + jQ.

**Tracker.** A coarse FFT search (±1 kHz, a line ≥15 dB over the median) puts the oscillator on the carrier; a 20 Hz
natural-frequency loop pulls in for 0.5 s, then narrows to 5 Hz (2.5 Hz for AM stereo, so it cannot follow the 25 Hz
pilot or the bass of L−R). The loop runs once per 32 samples on a two-pole low-pass of y (120 Hz pulling, 30 Hz
tracking, 10 Hz stereo) — **it hears the carrier, never the sidebands**, which is the first reason SAM elsewhere
"sounds worse": a loop that hears modulation turns it into phase noise.

The things that decide whether SAM is better or worse, and what each one does here:

1. **Flywheel through carrier fades.** The phase error is weighted by (carrier ÷ its 0.5 s average)²: when selective
   fading takes the carrier down, the loop coasts on its frequency integrator instead of chasing noise.
2. **Lock quality** = the amplitude-weighted mean of cos 2φ over 0.3 s (faded moments barely count, so a fade does not
   read as lost lock). Under 0.3 for 2 s = lost → re-acquire.
3. **Safety net.** Lock quality drives a blend (0.15 s up, 0.3 s down) between the coherent output and the shipping
   envelope detector, both levelled the same way — poor lock eases to plain AM, never to garbage, never a squeal (the
   oscillator only rotates; it is never heard).
4. **Carrier-referenced level.** Audio is divided by the received level (rises in 0.3 s, falls over 2 s), and the
   pipeline's AGC behind it is held to 12 dB of lift (`Agc::setMaxGain(4)`; envelope AM/SSB/CW keep 256, unchanged),
   so the noise is not lifted into every pause.
5. **Overmodulation** (1+m < 0) is demodulated correctly by the coherent path; the envelope detector cannot.

**Detectors** (y after DC removal = the 30 Hz carrier estimate, interpolated across each 32-sample block):

- **SAM**: I. U = I − H{Q}, L = I + H{Q} (H = Hilbert). With no neighbour SAM is exactly I and the Hilbert is not even
  computed. With one, the band is split where the neighbour starts and each region combines the sidebands with
  maximum-ratio weights from their own measured noise: `out = aHi·U + (1−aHi)·L − 2(aLo−aHi)·H_low{Q}`, where H_low
  is the Hilbert band-limited to the split, (1 − cos ω_s m)/(π m) — **linear phase, the full Hilbert's own delay**.
- **SAM-U/L, ECSS-U/L**: U or L, then an 8th-order low-pass at auto bandwidth's edge for that side (above the
  listener's edge when auto has not cut).
- **AM stereo (C-QUAM compatible)**, from the published description (envelope = 1+L+R, tan φ = (L−R+pilot)/(1+L+R)):
  L+R = |y| − carrier; L−R = |y|·Q/I (the 1/cos φ correction; I floored at |y|/4, i.e. ±76°); the 25 Hz pilot is
  detected synchronously at the loop rate (two-pole 0.25 s average, its phase must stand still against our 25 Hz:
  a real pilot is crystal), declared after 0.3 s over 0.015 of the carrier (off under 0.008), and **subtracted
  exactly** (its reconstructed phasor), not notched. L = S + b·D, R = S − b·D, b ramping 0→1 over 0.3 s only while the
  pilot is present AND the lock is good; one linked AGC for both channels (per-channel AGC would pan the image).
- **ENV** (plain AM): the shipping `AmDemod`, sample for sample, when auto bandwidth is off; with it on, the same
  detector on the rotated signal after the RF cut.

**AM auto bandwidth.** A 512/1024-point FFT of y about 10 times a second, **on the uncut signal, always** — so a cut can
never confirm itself ([[never_limit_permanently]]). DSB AM's two sidebands are mirror images, so Re(Y(f)·Y(−f))
averages to the programme power S(f) while noise and a neighbour average to zero; each side's own noise+interference
is N±(f) = |Y(±f)|² − S(f). Per 250 Hz band, over a 4 s average:

- **interference arm** (ON): a side is dirty from the lowest band ≥ 1.5 kHz from which most (≥ 60 %) bands up to the
  edge have N_side ≥ 16 × N_other AND ≥ 4 × S. SAM weighs that region down (above); SAM-U/L/ECSS cut that side;
  envelope AM and AM stereo cut the RF symmetrically (`AdaptiveIf`, centred on the tracked carrier).
- **heterodyne notch** (ON): a steady line on ONE side only (≥ 16 dB over its neighbourhood and 10 dB over the other
  side), seen on two decisions running, gets a notch (Q = f/40).
- **noise arm** (OFF — measured harmful, §4): "narrow to where the programme stops being above the noise".
- **time rules**: 2.5 s of evidence before any cut; narrowing needs 4 decisions (2 s) and goes to the widest of them;
  widening needs 2 (1 s); a lost lock widens at once; a retune clears everything.

**CPU / SIMD.** NEON (both ARMs, `-marm` armv7 included) / SSE2 (x86-64 and the 32-bit i386 build) / scalar:
the rotator (`rotateBlockIlv`), magnitudes, the block sums and ramps, the sideband split and the blend, the C-QUAM
decode, both Hilbert kernels (four outputs per tap, contiguous loads), the measurement FFT (pffft). Not vectorised:
the loop (once per 32 samples — nothing to gain), the 8th-order sideband low-passes and notches (recursive biquads;
only SAM-U/L/ECSS and a heterodyne use them), the pilot loop (scalar, per sample). The AM family's channel runs at
2× its band top instead of 3× (envelope AM keeps 3×), because the Hilbert's length is set by its lowest audio
frequency and its cost goes as the channel rate squared.

## 3. Synthetic tests — `vibeserver/test-am-sync.cpp` (against truth)

Programme = 18 tones on exact FFT bins, so SINAD (tone power ÷ everything else in 150–4500 Hz) is a pure
distortion+noise figure, blind to filter phase and level. All pass (39/39) — numbers from the final run:

| test | result |
|---|---|
| **Acquire** a carrier 0 / 7 / 42 / −310 / +880 Hz off | locked at 0.72 s in every case; offset read to ±0.01 Hz |
| **Selective fading**, two-path ρ 0.90, τ 0.3 ms, CNR 40 (carrier notch every 4 s) | worst second: AM −0.5 dB → **SAM +7.9** (raw), −0.6 → **+9.9** (through the AGC); median 23.5 → **28.1** |
| … ρ 0.97 (−30 dB carrier notches) | worst AM −6.5 → **SAM +2.1** (raw) / **+4.9** (AGC); median 23.5 → **28.3** |
| … ρ 0.90, τ 1.0 ms, CNR 30 | worst AM −2.4 → **SAM +6.0** / **+7.6**; median 15.4 → **16.7** |
| **Clean**, no fading, CNR 45 / 25 / 15 | AM 32.8 / 12.8 / 2.7 — SAM 32.8 / 12.8 / 2.9 (never worse) |
| **Sideband rejection** SAM-U/L, ECSS-U/L (wanted vs other side) | 48–53 dB at 300–400 Hz, 88–89 dB at 1.2–1.5 kHz, 106 dB at 2.5–3.2 kHz |
| **AM stereo**: pilot, separation | pilot found at 1.33 s; separation **L 63.7 / R 62.6 dB** (target ≥ 25) |
| … the pilot in the audio | removed: 44.8 dB under the programme |
| … mono compatibility | an envelope (ordinary AM) receiver hears L and R equally (41.6 / 41.6 dB) |
| … no pilot | never stereo; L = R exactly |
| … 25 dB CNR + 60 % flat fading | stereo held 100 % of the time, separation 40.8 dB |
| **Auto BW**, neighbour +5 kHz at −6 dB (SW spacing) | AM −1.4 → **+10.9** dB; SAM −0.2 → **+15.2** (U side weighted to 0.08 above 2.15 kHz, top end kept) |
| … neighbour −9 kHz at +6 dB, width 16 kHz | AM −7.1 → **+10.8**; SAM 7.2 → **15.3** |
| … no neighbour, CNR 20 / CNR 45 | unchanged: 4.0 → 4.0, 28.8 → 28.8 (no cut taken) |
| **Retune** after a cut | the upper edge 1900 Hz → back to the listener's 5000 at once |
| **Station vanishes** (carrier gone, noise left) | no burst: peak 0.50 vs programme 0.43; the blend eases to envelope (0.00) |

## 4. Real signals — tonight's UberSDR captures

Stuart's UberSDR (RX888), iq192 (192 kHz I/Q), recorded 2026-10-31: 49 m (6.050 MHz) and 41 m (7.350 MHz) together
at 01:49 UTC, 150 s; MW (1.000 MHz, 904–1096 kHz, European skywave at night) and 31 m together at 02:15 UTC, 300 s.
Kept in `~/VibeSDR-test-audio/amsync/` (broadcast audio — **not in git**).

**The score.** No clean original exists on air, so each station is judged against **the same programme off another
transmitter recorded at the same moment** (found by audio cross-correlation, `tools/am-sync/pairs.cpp`): an
independent path with its own fading and noise. Magnitude-squared coherence between the two audios over 300–3500 Hz,
γ²/(1−γ²) in dB, per 2 s window, is a S/N figure that needs no original and is blind to level, AGC setpoint and filter
phase; the SAME reference scores every mode of a station, and the modes are compared **paired, window by window**
against envelope AM (mean ± standard error, and the share of windows won). `tools/am-sync/am_replay.cpp`.

| station (kHz) | reference | SAM | AM + AUTO BW | SAM + AUTO BW | SAM-U | SAM-L |
|---|---|---|---|---|---|---|
| 963 | 972 (143 × 2 s, 0 fade) | +0.17 ± 0.01 | +0.00 ± 0.00 | +0.17 ± 0.01 | −0.78 ± 0.04 | −1.12 ± 0.04 |
| 972 | 963 (143 × 2 s, 0 fade) | +0.83 ± 0.02 | +0.08 ± 0.01 | +0.70 ± 0.02 | −0.53 ± 0.03 | −0.65 ± 0.03 |
| 990 | 1080 (143 × 2 s, 0 fade) | +0.10 ± 0.02 | −0.00 ± 0.00 | +0.10 ± 0.02 | −0.60 ± 0.05 | −0.55 ± 0.05 |
| 1080 | 990 (143 × 2 s, 0 fade) | +0.78 ± 0.10 | +0.69 ± 0.12 | +0.81 ± 0.11 | −0.28 ± 0.11 | −0.33 ± 0.12 |
| 1044 | 1080 (143 × 2 s, 0 fade) | +0.21 ± 0.03 | +0.03 ± 0.05 | +0.11 ± 0.03 | −0.53 ± 0.05 | −0.98 ± 0.06 |
| 1071 | 1053 (143 × 2 s, 20 fade) | +0.24 ± 0.04 (**fades +1.12**) | 0.00 | +0.24 ± 0.04 (fades +1.12) | −1.07 ± 0.13 | −0.95 ± 0.13 |
| 1053 | 1071 (143 × 2 s, 0 fade) | +0.04 ± 0.01 | +0.00 ± 0.00 | +0.03 ± 0.02 | −0.19 ± 0.02 | −0.23 ± 0.02 |
| 7350 | 7335 (68 × 2 s, 0 fade) | +0.30 ± 0.05 | −0.07 ± 0.03 | +0.21 ± 0.06 | +0.02 ± 0.11 | +0.38 ± 0.10 |
| 6060 | 7355 (68 × 2 s, 3 fade) | +0.24 ± 0.08 | 0.00 | +0.24 ± 0.08 | +0.07 ± 0.13 | +0.21 ± 0.11 |
| 7355 (5 kHz neighbour) | 6060 (68 × 2 s, 6 fade) | **+1.98 ± 0.37** (fades +7.70) | +1.40 ± 0.37 | **+3.05 ± 0.49** (fades +9.50) | **+3.68 ± 0.56** | +1.13 ± 0.32 |
| 7335 | 7350 (68 × 2 s, 6 fade) | **+1.39 ± 0.11** (fades +2.50) | 0.00 | +1.39 ± 0.11 | +0.62 ± 0.15 | +1.26 ± 0.19 |
| 6000 | 7280 (68 × 2 s, 6 fade) | −0.11 ± 0.07 (fades −0.49) | −0.01 ± 0.01 | −0.11 ± 0.07 | −0.18 ± 0.10 | −0.09 ± 0.08 |

dB of coherence-S/N against envelope AM on the same station, paired per window: mean ± standard error ("fades" = the
same over the windows where the station's carrier was ≥ 6 dB under its median). Final code (commit 5e8671cdb).

**Read plainly:**
- **SAM vs AM: better on 11 of 12 stations, never significantly worse.** +0.04 … +1.98 dB, most by many standard
  errors (972: +0.83 ± 0.02; 7335: +1.39 ± 0.11, won 67 of 68 windows). **In the carrier fades the gain grows**:
  7355 +7.7 dB, 7335 +2.5, 1071 kHz MW skywave +1.1 — the case SAM exists for. Tonight's captures had few deep fades
  (0–20 windows per station), so the synthetic two-path test (§3) is where the fade benefit is shown in full.
- **Where SAM does NOT win: 6000 kHz**, −0.11 ± 0.07 (1.6 SE — not significant) and −0.49 in its 6 fade windows. It
  locked solidly (offset +5.5 Hz) with no neighbour; I could not find a cause in the time available and say so
  plainly. **1044 kHz shows a real limitation:** its carrier readout jumped between ~0 and ~+3 Hz and lock quality sat
  at 0.3–0.9 — two co-channel transmitters a few hertz apart (a synchronous network). A PLL cannot lock to two
  carriers; the safety net eased it toward envelope AM for long stretches, and SAM still came out +0.21.
- **SAM-U / SAM-L** are the manual tools: +3.7 dB on 7355 (the side away from the neighbour; +10.8 in its fades), but
  −0.2 to −1.1 dB on clean stations, because one sideband carries half the signal of two. Offer, never default.
- **AM AUTO BW** (envelope AM): never worse than −0.07 dB, +1.40 on 7355, +0.69 on 1080. **SAM + AUTO BW**: +3.05 on
  7355 (+1.1 over plain SAM), +0.81 on 1080, and within 0.13 dB of plain SAM everywhere else.
- **Default ON**, like FM's: measured not to hurt anywhere tonight once the dead ends below were fixed.

**Dead ends, kept on the record** (each one is a commit):
1. *SAM 0.4–0.7 dB worse than AM on clean stations* — the tool reused one cross-correlation lag (the first mode's) for
   every mode, and SAM carries the Hilbert's ~10 ms. Per-mode lag fixed it.
2. *The noise arm* — "narrow to where the programme is no longer above the noise" — cut a healthy 6060 kHz to 2 kHz
   (−1.8 dB) and 7350 (−1.3 dB): on skywave the two sidebands lose their mirror coherence (selective fading makes H(f)
   and H(−f) differ), so the estimator under-reads the programme. **Off by default.**
3. *The interference arm's first version* took the first dirty band and chattered 1.5 ↔ 4.4 kHz on 7350 under 7355
   (−0.9 dB). Now 16× / 4× S / contiguous-to-the-edge / 2 s to narrow.
4. *SAM's hard cut* of the dirty side threw away the second copy of the programme (−1.9 dB on 972 kHz) → two-region
   maximum-ratio weighting.
5. *…whose region split was an 8th-order IIR* — its phase mixed the sidebands with complex weights (−0.9 dB on 1044,
   −1.2 on 1080). The band-limited Hilbert (linear phase, same delay) fixed it.

## 5. CPU per listener

The whole listener chain (channel filter, demodulator, AGC, resampler) through the real `RxPipeline`, on a 48 kHz
listener channel (`tools/am-sync/bench.cpp`; synthetic AM with a 5 kHz neighbour). **Pi 2** = the server's armhf build
(`-O3 -marm -march=armv7-a -mfpu=neon-vfpv4`) cross-compiled in `vibeserver-cross:bookworm-armhf`, run under
`qemu-arm` with the instruction-counting plugin (`tools/ft8-bench/qemu/insncount.c`), signal generation subtracted;
instructions → % of one 900 MHz Cortex-A7 at 0.6–0.8 instructions per cycle — **an estimate, not a measurement on a
Pi** (the same convention as DIGITAL-SPOTS.md).

| mode | Mac M5 (SIMD) | Pi 2 SIMD, M instr/s | Pi 2 SIMD, % core | Pi 2 SCALAR, M instr/s | scalar ÷ SIMD |
|---|---|---|---|---|---|
| AM (shipping) | 0.124 % | 56.2 | 7.8–10.4 | 146.6 | 2.6× |
| AM + AUTO BW | 0.140 % | 62.8 | 8.7–11.6 | 157.8 | 2.5× |
| **SAM** | 0.068 % | 39.1 | 5.4–7.2 | 89.7 | 2.3× |
| SAM + AUTO BW | 0.070 % | 39.7 | 5.5–7.4 | 90.4 | 2.3× |
| SAM-U | 0.131 % | 55.6 | 7.7–10.3 | 150.0 | 2.7× |
| SAM-L + AUTO BW | 0.134 % | 56.2 | 7.8–10.4 | 150.7 | 2.7× |
| ECSS-U | 0.057 % | 31.6 | 4.4–5.9 | 61.0 | 1.9× |
| ECSS-L + AUTO BW | 0.061 % | 32.4 | 4.5–6.0 | 61.7 | 1.9× |
| AM stereo | 0.085 % | 55.6 | 7.7–10.3 | 108.8 | 2.0× |
| AM stereo + AUTO BW | 0.086 % | 56.2 | 7.8–10.4 | 109.5 | 1.9× |
| USB (reference) | 0.116 % | 69.1 | 9.6–12.8 | 153.0 | 2.2× |

- **Every new mode costs a Pi 2 listener no more than plain AM does** (SAM and ECSS less, because their channel runs at
  2× the band top). AM AUTO BW adds ~6 M instr/s (≈ 1 % of a Pi 2 core) to envelope AM — the carrier tracker and the
  measurement FFT it needs — and ~1 M/s to the synchronous modes, which already track the carrier.
- **SIMD paid**: 1.9–2.7× fewer instructions than the scalar build on the Pi 2's own flags. **Where it did not pay**
  (so it was not done): the loop update (once per 32 samples), the measurement's bin arithmetic (10 frames a second
  × 512 bins), the recursive sideband low-passes and notches, and the AM-stereo pilot loop — together a small part of
  each mode, and none of it is vectorisable along time without changing the answer.
- **The first version was 10 % of a Pi 2 core for SAM-U** at a 48 kHz channel (+68 M instr/s over AM): the Hilbert's
  length is set by its lowest audio frequency (~150 Hz), so its cost goes as the channel rate squared. The family's
  channel now runs at 2× its band top; the audio is resampled to 48 kHz afterwards as always.
- **Scalar vs SIMD output**: the scalar fallback is the reference. The vector kernels give the same answers to float
  rounding (not bit-identical: the four-lane rotator recurrence and the C-QUAM decode's Newton-refined reciprocals
  round differently), and the `-DVIBE_FORCE_SCALAR` build passes the same 39/39 test-am-sync checks at the same
  thresholds. Mac, SIMD vs scalar, SyncAm alone at 36 kHz: SAM-U 0.138 % vs 0.274 % of a core.
- **One-pipeline radio** (2.4 MS/s RTL, Mac): AM 0.347 %, SAM 0.298 %, SAM-U 0.347 %, ECSS 0.280 %, AM stereo
  0.305 %, AM + AUTO BW 0.369 % of one core — the capture-rate channel filter dominates and the AM family adds nothing
  measurable on top of it.

## 6. Clients

- **VibeServer web client**: done (keys from `modes`, labels, a11y names, the AM sync line, AM AUTO BW row, tutorial
  card; page header rebuilt). Checked: the TypeScript of every changed file (a `tsc` subset), the bundle builds.
- **React Native app**: done in code (mode keys with VoiceOver names, AM AUTO BW row + live line, AM-ST rings, tour
  card, V12 release note). Type-checked as a subset; the full app `tsc` needs > 1 GB of RAM, over tonight's limit.
  **Not looked at on a device or simulator** — the faceplate/width/transparency visual check is owed (§8).
- **Android local hardware**: the same native DSP; the keys appear from its own server's `modes` list.
- **iOS**: the prebuilt DSP library is rebuilt and committed (`build_ios.sh`: link check clean, FAAD2-free).
- **Watches**: **deliberately not offered.** Both watch apps' Demod lists are fixed per backend and nothing on the
  phone↔watch link (or in the standalone watch's VibeServer client) says which demodulators THIS server has — adding
  them would put keys on watches facing older servers. They now NAME the mode properly (SAM-U, AM-ST …) when the dial is
  put there by the phone or another listener. To offer them: forward `modes` from vibeserver.json over the link.

## 7. AM stereo on air — where to test (research, not guesswork)

- **Europe: no confirmed C-QUAM station tonight.** Undated references name a handful in France, Italy and Greece, and a
  2011 forum post names Broadcast Italia on 1485 kHz (Rome); none is confirmed current. Europe's AM is closing down.
- **North America:** a Radio World engineer's column counts ~100 US stations still in C-QUAM; the FCC keeps no list.
  A July 2025 report has KBMR showing a 25 Hz pilot on the Valley City, ND KiwiSDR. Japan adopted C-QUAM in 1992 and
  most have reverted to mono; JOUF was recorded in full C-QUAM in 2021.
- **How:** a North American KiwiSDR at night (Kiwi has its own C-QUAM decoder, "QAM", useful as a cross-check), its IQ
  recorded and replayed through `am_replay` in `amst`. Not done tonight; the stereo decoder is proven only on our own
  synthetic C-QUAM modulator (§3).

## 8. Owed / untested

- The visual check of the new chrome on default / silver / black faceplates, transparency on/off, phone and iPad widths
  (app mode grid with six more keys; the audio sheet row; the web pill line). Code follows the existing rows' patterns.
- A real C-QUAM station (§7).
- VFO B (VibeServer's second VFO) does not follow the AUTO BW switch (its pipeline keeps the default, ON).
- The app's full type-check and run-tests suite (not run tonight: CPU and memory limits). Run tonight instead: the
  vibedsp pipeline / demod / passband / NaN-recovery tests, test-am-sync (SIMD and scalar), test_am_sync.ts, and the
  four faceplate suites that compose mode legends (segfield, dotfield, meters, landscape) — all pass.
- An Android (gradle/NDK) compile was not run; the same sources compiled for macOS arm64, iOS arm64 and armhf (gcc).
- The shim compiles (worktree server build); the new `amsync` message and the server-side mode list were not exercised
  against a running server — no live server was touched tonight.

## 9. Questions for Stuart

1. ECSS-U/L and SAM-U/L share a detector; ECSS differs by its SSB-style one-sided passband. Is that the ECSS you want,
   or should ECSS also keep the carrier audible (a true "exalted carrier" with a faint beat at zero offset)?
2. AM AUTO BW rides the FM A-BW switch (one server switch, two rows). Separate switches instead?
3. The noise arm (narrow to the station's own programme width when that buys S/N) is built but OFF — it misreads
   skywave. Keep it as an experiment, or remove it?
4. Default the six keys on for owners, or let owners opt in on the setup page?
5. The sync line shows `SYNC +3.2 Hz` — the carrier offset to 0.1 Hz. Wanted in the main pill on the app too (as on
   the web), or is the audio sheet enough?

## 10. Server sessions tonight

| time (UTC) | server | what | restored |
|---|---|---|---|
| 01:48–01:51 | Stuart's UberSDR 192.168.86.11:8080 | iq192 recordings, 6.050 + 7.350 MHz, 150 s (own channels) | n/a — read-only, own session |
| 02:15–02:20 | Stuart's UberSDR | iq192 recordings, 1.000 + 9.600 MHz, 300 s | n/a |

No VibeServer (Pi 500, Lenovo, Pi 2) was connected to, tuned or touched.

Sources (§7): [AM stereo — Wikipedia](https://en.wikipedia.org/wiki/AM_stereo) ·
[C-QUAM — Wikipedia](https://en.wikipedia.org/wiki/C-QUAM) · [AM Stereo – Current Status](https://www.liquisearch.com/am_stereo/current_status) ·
[radiodiscussions.com (KBMR, 2025)](https://radiodiscussions.com/goto/post?id=6891810) ·
[HFUnderground wiki: C-QUAM](https://hfunderground.com/wiki/index.php/C-QUAM) ·
[Radio World](https://www.radioworld.com/?p=105432) · [JOUF-AM 2021](https://shortwavearchive.com/archive/tag/JOUF)
