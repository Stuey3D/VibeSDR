# V12 — Synchronous AM, ECSS, AM Stereo (C-QUAM compatible), AM AUTO BW and AUTO bandwidth in every mode (§11)

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
- **The AM sync line** (web: beside the frequency, in the airband channel line's style; app: the AM SYNC row in the
  audio sheet, standard font): `SYNC +3.2 Hz` (the carrier's offset from the dial — a DX tool in its own right),
  `SYNC …` while acquiring, then `AUTO BW L 5.0k · U 3.9k` when auto bandwidth has cut, `NOTCH 1004 Hz` for a
  heterodyne. Carried by a new additive `{"type":"amsync"}` message (once a second, on change, only in the AM family).
- **AM-ST's light** is the existing stereo rings (web `#mStereo`, app mode box), lit by the 25 Hz pilot via the
  existing `rds` message's `stereo` flag.
- **AM AUTO BW** is the same switch as FM's A-BW. ★ **Superseded by §11**: its rows in the web audio menu and the app
  audio sheet are gone; the switch is now the AUTO key above SYNC by the passband sliders, per listener, in every mode
  that has an automatic width. **Manual + auto:** the listener's width is always the ceiling; auto only
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
- ~~VFO B does not follow the AUTO BW switch~~ — it does now (§11).
- The app's full type-check and run-tests suite (not run tonight: CPU and memory limits). Run tonight instead: the
  vibedsp pipeline / demod / passband / NaN-recovery tests, test-am-sync (SIMD and scalar), test_am_sync.ts, and the
  four faceplate suites that compose mode legends (segfield, dotfield, meters, landscape) — all pass.
- An Android (gradle/NDK) compile was not run; the same sources compiled for macOS arm64, iOS arm64 and armhf (gcc).
- The shim compiles (worktree server build); the new `amsync` message and the server-side mode list were not exercised
  against a running server — no live server was touched tonight.

## 9. Questions for Stuart

1. ECSS-U/L and SAM-U/L share a detector; ECSS differs by its SSB-style one-sided passband. Is that the ECSS you want,
   or should ECSS also keep the carrier audible (a true "exalted carrier" with a faint beat at zero offset)?
2. ~~AM AUTO BW rides the FM A-BW switch~~ — answered by Stuart's AUTO key (§11): one switch, by the sliders.
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

---

## 11. AUTO above SYNC — automatic bandwidth in every mode that can use it (2026-10-31, afternoon)

Stuart: *"as for bandwidth simply add an auto button above the sync button between the upper and lower sideband sliders
in the app and make auto work for any mode we can, and when in a mode that we cant use auto bandwidth on simply revert
to the layout we have now with just the sync button."*

### 11.1 What a listener gets

| mode | what AUTO does | where it lives |
|---|---|---|
| WFM | narrows a weak station toward its own occupied width (the shipping TEF-style A-BW) | shim, `g_autoBwOn` → rxBwHz |
| AM, SAM, SAM-U/L, ECSS-U/L, AM-ST | narrows the side a neighbour splatters into (§2–§4) | vibedsp `SyncAm` |
| **USB, LSB** | **fits the passband to the voice actually being sent** — new | vibedsp `AutoPassband` (`autobw.cpp`) |
| **CWU, CWL** | **±150 Hz round the keyed tone nearest the beat note** — new | vibedsp `AutoPassband` |
| NFM | **not offered** — §11.4 | — |
| DRM, DAB, ADS-B, ISM, NDB, AIS, decoders | no passband to fit (the row is hidden or the width is the decoder's) | — |

- **The key** (app `ModeSelector`, web bandwidth row): AUTO stacked ABOVE SYNC in the middle column, in both of the
  app's styles (PopupKey on silver/black, the bordered key on the default). It is drawn only when the server lists the
  current mode in hwinfo **`autoBwModes`**; otherwise the column is SYNC alone — the row exactly as it was. Nothing in
  either client knows which modes have AUTO ([[feedback_app_no_hardcoded_settings]]); `src/services/bwAuto.ts` holds
  the shared rules, pinned by `scripts/test_bw_auto.ts`.
- **While AUTO is on and cutting, the sliders show the width AUTO chose** — from `{"type":"bwauto","on","cut","lo","hi"}`,
  in the sliders' own coordinates (Hz from the dial; `null` = that edge is the listener's own). Display only: a client
  never sends those edges back as a bandwidth.
- **★ Behaviour chosen for a slider move: AUTO switches OFF for the session — a manual override, and your width wins.**
  The client sends `autobw off` first (once per drag), then the edge, starting from what the sliders showed. Reasons:
  it is what a TEF668x radio does when a bandwidth is picked by hand, and the other choice (AUTO stays on with the drag
  as its new ceiling) leaves a listener dragging a slider whose effect they cannot see. Turning AUTO back on is one tap.
  The server never infers it from a bandwidth frame, because a mode change carries edges too and is not a drag.
- **The AM AUTO BW rows are gone** (web audio menu, app audio sheet — both unreleased); the app keeps the AM SYNC
  readout line. FM's A-BW button stays where FM-DX listeners know it, in step with AUTO (the same switch).

### 11.2 One switch, per listener; the shared dial

- `{"type":"autobw","on":bool}` — the existing message, so FM's A-BW, AUTO and an older client all drive the same thing.
- **Per-listener DSP** (a locked range with several VFOs): it is THIS listener's (`ClientDsp::autoBwOn`, ON for a new
  listener — a joiner inherits nobody's choice), applied to their pipeline and re-applied when it is rebuilt. Nobody
  else's audio moves.
- **One pipeline** (a single listener, or a shared dial): it is the dial's, exactly as the passband is — in the
  ask-before-tuning gated list, broadcast to the room in hwinfo, adopted by a joiner, and never sent by a client except
  on a person's tap or drag ([[shared_dial_contract]]: the server states its full state, the client has no vote).
- **The owner's A-BW switch does not exist** — the brief assumed one. `autobw` has always been a listener control
  (server-wide on a single pipeline), default ON at start. So there is no owner "offer AUTO" setting; the server offers
  AUTO by mode. If Stuart wants an owner switch (offer it at all / its default for new listeners), it is a small
  addition: a config key read where `autoBwModes` is built and where `ClientDsp::autoBwOn` is initialised (§11.8 Q1).
- **VFO B follows its listener's AUTO** (`vibe_watch.h WatchChannel::setAutoBw`, set when B is created and whenever the
  listener switches AUTO) — the AM family, SSB and CW autos in B's own pipeline. B has no WFM.
- Fixed on the way: on a shared dial the AM sync line reached only the first listener (one shared "last sent" stamp,
  the bug the `sendFmMetaAll` note already describes); it is now built once and sent to everybody.

### 11.3 The design (vibedsp `AutoPassband`)

USB/LSB/CW are demodulated to audio that is a linear image of the RF passband (0 Hz = the dial's zero beat; CW's tone
at the middle of its 1200 Hz filter). So the cut is made on that audio, **before the AGC** (so the AGC no longer levels
on what was cut), with a linear-phase FIR (`RealFir`: the NEON / SSE2 / scalar kernel the Weaver pair uses, retuned in
place). **Measured on the uncut audio, always** — a cut can never confirm itself. While AUTO is on and not cutting the
FIR is a pure delay (11 ms at a 12 kHz channel), so the audio does not jump in time as cuts come and go.

- **SSB — envelope correlation.** Per 100 Hz band, the correlation over ~5 s of that band's log power with the log
  power of the voice's core (500–1500 Hz). A band carrying THIS talker rises and falls with the core syllable by
  syllable; a band of noise, another talker splattering in, or a steady heterodyne does not. Trimmed from each edge to
  the outermost band that correlates (≥ 0.15, it and its inner neighbour both — a three-band average leaked the voice
  outward), with a 75 Hz margin; the top never under 2 kHz, the bottom never over 300 Hz; nothing cut when there is no
  voice (the core's log power barely moves).
- **CW — the tone nearest the beat note.** An 11.7 Hz-resolution spectrum averaged over 1.5 s; the significant peak
  (≥ 6 dB over the median bin) NEAREST the beat note, confirmed on two decisions, gets ±150 Hz — narrower when another
  signal is closer (never under ±60). A cut tone is HELD while it is there (±40 Hz); gone for 3 s → full width, and only
  then is a new tone taken.
- **Time rules** (as the AM auto): 3 s of evidence first (CW 2 s); narrowing needs four decisions running (2 s) and
  takes the widest; widening is at once; a retune clears every verdict (`RxPipeline` `tuneReq_`, as `SyncAm::retune`).
- **Default ON**, like FM's and AM's — measured never worse on the test set below.

### 11.4 Measured — the score, synthetic, and off air

**The score** (`vibeserver/autobw_score.h`). AM-SYNC §4 used per-frequency coherence, but for USB/LSB/CW a filter after
the detector is linear: it scales programme and noise at each frequency alike, so a per-frequency S/N cannot move — that
score is blind by construction to what a passband does. What a passband changes is the broadband picture: per 2 s
window, C(f) = the output coherent with the reference (the wanted signal), N(f) = the rest; each run normalised on the
core band; **score = Σ C ÷ Σ (N + any wanted signal the fixed passband kept and AUTO did not)** — cutting a band that
held only noise raises it, cutting the voice lowers it. Paired per window against the fixed passband on the same signal.

**Synthetic, through the real pipeline** (`vibeserver/test-auto-bw.cpp`, 20/20 SIMD and `VIBE_FORCE_SCALAR`):
a pseudo-voice (wandering 95–190 Hz pitch, harmonics under moving formants, ~4 Hz syllables with pauses), a Morse keyed
tone (20 WPM), in white noise; the known programme is the truth. dB over the fixed passband, mean ± SE (windows won):

| case | AUTO gain | voice kept (worst window) | AUTO's passband |
|---|---|---|---|
| USB, TX 300–2700, RX 2.7 k, S/N 6 / 15 / 25 dB | +0.13 ± 0.02 / +0.07 ± 0.01 / +0.01 ± 0.01 | ≥ 99.8 % | 125–2675/2700 |
| USB, TX 300–2400 (narrow transmitter), RX 2.7 k, S/N 10 | +0.18 ± 0.03 | 99.9 % | 125–2475 |
| USB, TX 300–2700, **RX opened to 4.0 k**, S/N 10 / S/N 0 | **+0.89 ± 0.12** / **+1.03 ± 0.20** | 99.4 % / 90.7 % | 125–2775 / 300–2775 |
| USB, WIDE TX 100–3500, RX 4.0 k (must keep it all) | +0.10 ± 0.01 | 100 % | 0–3675 |
| LSB, RX opened to 4.0 k, S/N 10 | +0.79 ± 0.13 | 99.5 % | 125–2775 |
| USB + a 2nd talker 2.2 kHz up, equal / **+6 dB** | +0.50 ± 0.21 / **+3.81 ± 1.01** | 99.6 % / 96.4 % | cut some of the time |
| USB TX 300–2400 + heterodyne at 2.6 kHz (−10 dB) | **+2.24 ± 0.37** | 99.5 % | 0–2575 |
| noise only | — | — | not cut |
| CW on the beat note, S/N −6 / +6 / +20 dB (in 1.2 kHz) | **+5.63 ± 0.07** / +5.63 ± 0.07 / +2.94 ± 0.05 | ≥ 97.1 % | 450–750 |
| CW 100 Hz off the beat note, S/N 0 | +5.67 ± 0.07 | 99.2 % | 549–849 (centred on 700) |
| CW + a neighbour 250 Hz up, +6 dB louder | **+16.07 ± 0.21** | 94.8 % | 451–751 |
| CW + a neighbour 150 Hz down, equal | **+14.12 ± 0.16** | 98.2 % | 525–675 |
| CW noise only | — | — | not cut |

**Off air** (`tools/auto-bw/ab_replay.cpp`; recordings in `~/VibeSDR-test-audio/autobw/` and `cw/`, not in git):
- **SSB: Shannon VOLMET on 3413 and 5505 kHz, recorded together** (UberSDR iq48, 240 s, 12:17 UTC): the same programme
  on two paths, one the reference for the other. ★ Their SSB audio drifts in phase (a fraction of a hertz apart), so a
  Welch cross-spectrum averages to nothing (waveform correlation 0.09; envelopes 0.54) — the reference is used by
  POWER: per 100 Hz band, the output's power regressed on the reference's, segment by segment (`abscore::cnPower`).

  | judged (reference) | RX 2.7 k | RX opened to 4.0 k |
  |---|---|---|
  | 3413 kHz, weak (5505) | +0.06 ± 0.05 (won 27/57) | **+1.03 ± 0.11** (won 46/57) |
  | 5505 kHz, strong (3413) | +0.00 ± 0.00 (no cut) | +0.05 ± 0.01 (won 56/57) |

  Never significantly worse. On the weak station AUTO sometimes pulled the top to 2.0–2.2 kHz; the estimated voice kept
  was median 92–96 % (worst windows 74–79 % — the per-band power regression is itself noisy on a −4 dB signal), and the
  score, which charges every lost band, still came out level or ahead.
- **CW: four night/evening recordings** (40 m 7.020 MHz iq192, 20 m 14.035 iq192, 80 m 3.550 iq48), eight signals. No
  second path exists for CW, so the score is the tone (±40 Hz round the tone AUTO centred on) against the rest of
  0–1400 Hz, fixed vs AUTO: **+5.1 to +13.9 dB** (40 m: +13.9 ± 1.0, +9.9 ± 0.6, +11.5 ± 0.5; 20 m: +5.1 ± 0.5,
  +11.4 ± 0.1; 80 m: +7.2 ± 0.4, +8.6 ± 0.5, +6.7 ± 0.8), every signal winning 98–100 % of its windows (final code). ★ This proxy measures round the tone AUTO picked, so it cannot see a WRONG pick; the
  pick itself is what the synthetic neighbour cases pin (nearest the beat note, held through pauses).

**Read plainly:**
- **CW is the big win** — the 1200 Hz CW passband narrowed round the signal: ~+5.6 dB against noise, +14–16 dB
  against a neighbour 150–250 Hz away, +5 to +14 dB on real bands.
- **SSB at the default 2.7 kHz on a standard transmitter: essentially neutral** (+0.0 to +0.2 dB) — the default
  passband already fits a standard voice; AUTO trims the empty 0–125 Hz and little else. **It earns its place when the
  passband does not fit**: a listener who opened it wide (+0.8 to +1.0 dB, synthetic and off air), a narrow transmitter
  (+0.2), and a louder neighbour or a whistle above the voice (+2.2 to +3.8 dB).
- **Dead ends kept on the record** (found while building, before the first commit, so they are recorded here and in
  the commit messages rather than as separate commits; the NFM probe is its own commit): CW "strongest tone in reach" locked onto a louder neighbour
  (−17.5 dB) → nearest the beat note; CW re-centred on a neighbour in the wanted signal's pauses (40 m) → hold the tone;
  SSB three-band smoothing leaked the voice outward (kept 200–300 Hz for a 300 Hz transmitter, kept a louder second
  talker's band) → two-in-a-row; SSB fitted a passband to pure noise → "no voice, no cut"; the off-air coherence score
  read nothing because two SSB paths drift in phase → the power regression.

**NFM — not offered, and why.** `tools/auto-bw/nfm_probe.cpp` asked first whether ANY fixed narrower IF helps (if none
does, no automatic one can): pseudo-voice FM through the real pipeline, coherence score vs truth (FM's detector is not
linear, so here it is the right score), 12.5 / 11 / 9 / 7 kHz:

| signal | 12.5 k | 11 k | 9 k | 7 k |
|---|---|---|---|---|
| ±2.5 kHz, CNR 20 dB | 4.76 | 4.75 | 4.69 | 4.30 |
| ±2.5 kHz, CNR 10 dB | −0.73 | −0.72 | −0.72 | −0.85 |
| ±2.5 kHz, CNR 6 dB | −4.29 | −3.60 | −2.90 | −2.72 |
| ±2.5 kHz, CNR 3 dB | −10.67 | −9.24 | −7.29 | −5.45 |
| ±2.5 kHz, CNR 15, a neighbour one channel up, +20 dB | 2.65 | 2.65 | 2.59 | 2.24 |
| ±5 kHz, CNR 10 dB | 2.62 | 2.56 | 2.28 | 1.59 |

A narrower IF helps NFM **only below the FM threshold** (+1.4 to +5 dB at CNR 3–6) and costs up to 1 dB above it or on
a wide-deviation signal; the channel filter already rejects a neighbour one channel away. So an NFM AUTO would have to
know the CNR — and narrowing changes the very noise it would measure it by (a cut confirming itself), and the NFM
demodulator's gain is tied to its width (a level jump on every change). Not built in this session; NFM is left out of
`autoBwModes`, so its row is SYNC alone. The probe says what a correct one would be worth (§11.8 Q3).

### 11.5 CPU per listener (Pi 2 estimate, same method as §5)

`tools/auto-bw/bench.cpp`, a 48 kHz listener channel with a pseudo-voice USB signal and a keyed CW tone; the server's
armhf flags in `vibeserver-cross:bookworm-armhf`, `qemu-arm` + `insncount`, 10 s, signal generation subtracted;
% of a 900 MHz Cortex-A7 at 0.6–0.8 IPC — **an estimate, not a measurement on a Pi.**

| mode | Mac M5 (SIMD) | Pi 2 SIMD, M instr/s | Pi 2 SIMD, % core | Pi 2 SCALAR, M instr/s |
|---|---|---|---|---|
| USB (passband opened to 4 k) | 0.174 % | 45.9 | 6.4–8.5 | 107.1 |
| USB + AUTO | 0.210 % | 57.1 | 7.9–10.6 | 131.6 |
| CW | 0.248 % | 60.0 | 8.3–11.1 | 172.3 |
| CW + AUTO | 0.281 % | 69.1 | 9.6–12.8 | 192.7 |

- **AUTO adds ~11 M instr/s to SSB and ~9 M/s to CW — 1.3–2.1 % of a Pi 2 core per listener**, almost all of it the
  FIR (423 taps at this chain's 19.2 kHz channel for SSB, 397 at 12 kHz for CW); the measurement (a 256/512- or
  1024-point real FFT, 12–47 a second) is small. SIMD: 2.2× fewer instructions than scalar for AUTO's share.
- **Not vectorised**, and why: the per-band moments and the CW peak search (once per FFT frame, ≤ 60 bands / ≤ 103
  bins — nothing to gain). The FIR is the hot loop and runs on the existing NEON / SSE2 kernel; `test-fir-simd` checks
  that kernel against the direct convolution sample for sample. Scalar vs SIMD output: equal to float rounding (the dot
  product's summation order differs), and the `VIBE_FORCE_SCALAR` build passes test-auto-bw's same 20 checks.
- A cheaper SSB filter is available if wanted: a 250 Hz transition instead of 150 Hz is ~40 % fewer taps, for a softer
  cut edge (§11.8 Q4).

### 11.6 ECSS — "does it need a carrier, and would that make the signal better?" (Stuart)

**No, and ECSS stays as built.** Our ECSS-U/L lock a PLL to the station's own carrier and demodulate the chosen sideband
against a clean, noise-free local copy of it — the ideal limit of "exalting" the carrier (an exalted-carrier receiver
boosts the received carrier before an envelope detector precisely so that detector behaves like this). Re-inserting
an audible carrier on top of that adds one term: the carrier itself at 0 Hz in the detector's output — DC, a zero beat —
which carries no programme, cannot raise the programme's S/N, and is removed by the audio chain's own low edge and DC
handling anyway. Agreeing with the analysis, nothing was measured.

### 11.7 Checked, and not

- Run: test-auto-bw (SIMD and scalar), test-am-sync, test_am_sync.ts, test_bw_auto.ts, the vibedsp pipeline / demod /
  passband / NaN-recovery / adjacent tests, the vibeserver build (Mac), `build-web` (page header rebuilt and committed,
  as this branch already does); a local VibeServer (this Mac, loopback, a noise rtl_tcp) over its WebSocket: hwinfo
  lists the modes, `bwauto` on / off / per mode.
- **Seen**: the web bandwidth row in headless Edge (`--mute-audio`) at phone (390 px) and iPad (820 px) widths: LSB
  with AUTO above SYNC, cutting a 4 kHz passband to 3.5 kHz with the edges shown; NFM with SYNC alone. The web client
  has one look (no faceplates).
- **NOT seen**: the app. No simulator or device run under this session's CPU limits — the AUTO key on the default,
  silver and black faceplates, transparency on/off, phone and iPad, is code-reviewed and type-checked
  (`ModeSelector.tsx` clean) but not looked at. `SDRScreen.tsx` itself could not be type-checked (tsc runs out of
  memory at 512 MB on it); its changes are a few lines of state and props.
- Not run: an Android NDK build, the full run-tests.sh, a real C-QUAM station (§7), any of Stuart's VibeServers.
- RN Slider: a programmatic value change (AUTO's edge moving the thumb) must not count as a drag; both sliders ignore a
  value equal to the one drawn. Whether `@react-native-community/slider` raises onValueChange for a prop change on
  Android has not been checked on a device.

### 11.8 Questions for Stuart

1. An owner switch for AUTO (offer it at all, and its default for new listeners)? There is none today — `autobw` was
   always a listener control.
2. SSB/CW AUTO default ON for everybody after the update (as FM and AM are), or default OFF for SSB/CW until heard?
   CW's sound changes noticeably (±150 Hz).
3. NFM: worth building the CNR-steered narrowing (it would need its own noise measure outside the cut, and a width
   change that does not move the level)? Gains only on weak signals below the FM threshold.
4. SSB AUTO costs ~1.6–2.1 % of a Pi 2 core per listener; a softer cut edge would cost ~40 % less. Keep the sharp one?
5. Fine tuning SSB clears AUTO's verdict on every step (the "a limit must never travel with the dial" rule), so AUTO
   re-fits ~5 s after the last tune step. Keep, or let steps under ~100 Hz keep the verdict?

### 11.9 Server sessions (this afternoon)

| time (UTC) | server | what | restored |
|---|---|---|---|
| 12:06–12:11 | Stuart's UberSDR 192.168.86.11:8080 | iq48 8.957 + 13.264 MHz, 240 s (own channels) — 13.264 was empty by day; deleted | n/a — read-only |
| 12:15–12:17 | Stuart's UberSDR | 10–20 s iq48 probes of 5.450, 11.253, 5.505, 3.413 MHz (which VOLMETs are audible) | n/a |
| 12:17–12:22 | Stuart's UberSDR | iq48 3.413 + 5.505 MHz together, 240 s (the SSB two-path pair) | n/a |
| 12:25–12:45 | this Mac only | a local VibeServer on loopback with a noise / recorded rtl_tcp (protocol check, web pictures) | stopped |

No VibeServer of Stuart's (Pi 500, Lenovo, Pi 2) and no Android server was connected to, tuned or touched; no adb.
