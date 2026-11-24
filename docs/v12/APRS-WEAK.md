# APRS weak signals (2026-11-24) — branch `aprs-weak`

Stuart, 24 Nov: *"the signals are a little weaker now im back on the loop but are clearly there and need to address the
weak signal handling of our decoder"* — the XCover (Android server, Airspy HF+ at 912 kS/s, a loop) showing clear bursts on
144.800 at about S5, and almost nothing decoded (one packet, M0UNC-9).

## What changed

| Where | Change | Why |
|---|---|---|
| IQ receiver (`vibe_aprs_run.h` `mfSetup_` / `mfRun_`) | **The FM tone bank** (`FmToneBank`, `vibe_aprs.h`): a fourth IQ path beside the three discriminator filters. The AFC-corrected IQ, cut to ±8 kHz at ~20 kS/s, is correlated over each bit against exp(jβ·sin(2πft + θ)) for both tones — 4 mark β (1.5, 2.5, 3.6, 5) and 3 space β (0.9, 1.6, 2.6), 12 tone phases each — non-coherently; the largest \|sum\| per tone is its level, fed RAW (no peak normalisation: that cost 1.4 dB) to the same slicers, PLLs and deframers. `APRS_MF=0` turns it off (developer comparisons only). | A discriminator breaks into clicks below ~9 dB CNR and nothing after it can undo that. A Bell 202 bit on FM is a known waveform in the IQ, so a matched filter is not bound by the FM threshold. |
| `toneBankRow` | The bank's kernel — NEON, SSE2, scalar — over side-by-side template rows. | CPU (house rule). |
| Audio path (`AfskDemod`) | **The two tones fitted together**: each one-bit window is least-squares fitted by 1200 and 2200 Hz (cos and sin of each), each tone's level its own fitted amplitude. The 4 × 4 Gram inverse is tabled by sample phase where the tones repeat (P = 60 at 12 kS/s), else solved per sample from running sums. Five more slicers (the same balances) run on the fitted levels, beside the five on the plain correlators. | Over one bit the tones are not orthogonal: 1200 Hz leaks ~−14 dB into the 2200 Hz correlator, so with the tones far apart the leak WAS the weak tone's level. Both sets, because the fit wins big on tilted tones but alone lost a little on balanced tones through a listener's NFM chain. |
| `AfskDemod` slicers | Two **one-tone slicers** (mark − ½, ½ − space, against each tone's own peak). | Tones 9–12 dB apart: the strong tone still keys on and off cleanly. |
| `resolveCandidates`, `offerCandidate`, `Run::offer1200_` / `flushHeld_` | **Copies vote.** Copies of one transmission (same length, ≤ 8 bytes different) from every slicer and path are held 0.3 s of signal time; each copy is a vote; the variant with ≥ 1.5 × anyone else's votes goes out, otherwise nothing of that transmission. A level surer than half the frame's mean is never flipped by the repair. | See "False decodes": the shipping decoder emits wrong frames. |
| `test-aprs-onair` | `--curve`, `--false`, `--cpu-paths`, `--wav`, `--listener-wav`; **every logged frame checked byte for byte**; regression rows below the FM threshold and random data on FM. | |
| `test-aprs` | Tones ±9 / ±12 dB apart; the kernel against complex arithmetic; the vote. | |
| `test-server-aprs-shared.mjs` | `VIBE_APRS_RATE`; run-tests also runs it at 960 kS/s. | |

**Tried and not kept** (measured): PLL hold 0.6 / 0.9 / 0.95 in frame (0.75 stays); 16 or 24 tone phases; one β per burst
(chosen by energy: far worse; by contrast: no better); summing hypotheses instead of the max; FCS repair of PAIRS of weak levels
(+1 frame in 360 on the modem table, 0 on the IQ curve, and more tries is more false-pass risk); AFC variants (longer estimate,
carrier-fraction correction, no leak on a carrier: ≤ 0.2 dB); weighting repairs lower / passing a lone repair only when
"sure" (flipped level ≤ 0.3 of the mean, ≤ 2 weak levels) — on `--curve 100 7 10 ab` it cost the audio path 123 of 1582
packets AND let one wrong frame through that the plain vote stops (a chance-pass clean copy that a right repair outvotes).

## The weak-signal curve (`test-aprs-onair --curve 50 2 12 abc`, 50 packets a row)

CNR in 12.5 kHz; the server's chain (240 kS/s → RxPipeline NFM 12.5 kHz ALL 50 µs → `Run` AUTO). 50 % points by
interpolation, **before (aprs-audio-back d4e873bc3) → after**:

| TX | deviation, tuning | AUTO (audio + IQ) | audio alone |
|---|---|---|---|
| flat | ±3 kHz, on tune | 7.15 → **3.64** dB (−3.5) | 7.65 → 7.61 |
| pre-emphasised (+5 dB) | ±3 kHz, on tune | 7.12 → **3.78** (−3.3) | 7.79 → 7.63 |
| de-emphasised (−5 dB) | ±3 kHz, on tune | 7.10 → **3.90** (−3.2) | 7.61 → 7.53 |
| flat | ±4.5 kHz, +1 kHz off | 7.50 → **6.33** (−1.2) | 8.55 → 8.43 |
| pre-emphasised | ±4.5 kHz, +1 kHz off | 7.48 → **5.36** (−2.1) | 8.57 → 8.16 |
| de-emphasised | ±4.5 kHz, +1 kHz off | 7.12 → **6.30** (−0.8) | 8.20 → 7.81 |
| flat | ±6 kHz, −2 kHz off | 7.71 → 7.48 (−0.2) | — (the listener's 12.5 kHz cuts it) |
| pre-emphasised | ±6 kHz, −2 kHz off | 7.81 → 7.59 (−0.2) | — |
| de-emphasised | ±6 kHz, −2 kHz off | 7.75 → 7.29 (−0.5) | — |

(IQ alone = AUTO to 0.05 dB in every row.) Packets over the whole table: AUTO **2280 → 3036** (+33 %), audio alone 1323 →
1392; wrong frames 0 → 0. Flat ±3 kHz on tune, row by row: 3 dB 0 → 2, 4 dB 0 → 38, 5 dB 0 → 48, 6 dB 0 → 50, 7 dB 21 → 50.

- **On tune the gain is 3.2–3.5 dB. Off tune it shrinks**: the bank needs the carrier within ~250 Hz over a bit, and at these
  CNRs the AFC settles ~200–250 Hz short of a 2 kHz error and jitters ±150 Hz (at 15 dB it settles on −1920 for −2000; at 5 dB
  about −1750). A ±6 kHz tracker 2 kHz off gets nothing from the bank; the discriminator filters carry it as before.
- **The audio path** (`test-aprs`, AWGN audio, SNR in 3 kHz, of 40): the 360-frame table 203 → 237; flat 6 dB 7 → 15;
  2200 Hz 6 dB down at 10 dB 8 → 29; 9 dB down at 12 dB 4 → 39; 12 dB down 0 → 34; 6 dB up at 4 dB 2 → 27.

## The real recordings

- **XCover HF+ REC, 2026-11-24** (`~/VibeSDR-test-audio/aprs/xcover-hfplus-2026-11-24/…10-38-31.wav`, 160 s of the
  listener's NFM audio): **0 packets before, 0 after** (`test-aprs-onair --wav`). It is not a tilt problem: the "2200 Hz 13–14
  dB down" at 110 / 121 s and "1200 Hz 7.5 dB down" at 41 s are the FLAG PREAMBLES (0x7E in NRZI is 7 bits of one tone and 1
  of the other every 6.7 ms — the dips show in a 5 ms trace, with side tones ±150 Hz). The data after them has both tones.
  The audio shows a signal **at or below the FM threshold**: the hiss quiets only ~2 dB in the packets (3–3.8 kHz), against
  8–13 dB for our synthetic packets at 5–9 dB CNR through the same chain (`--listener-wav`), and its failing frames have a
  mean \|soft\| of ~0.3 with ~100 unsure levels each (decodable ones: 0.6–1.0, a handful). Our audio path needs ~7–8 dB
  (`--listener-wav` 5 dB: 0/8, 7 dB: 2/8, 9 dB: 8/8). **No audio demodulator gets these back; the IQ tone bank is built for
  this level** (4–5 dB: 38–48 of 50). The live server feeds the IQ receiver the decoder tap, so these should now decode there;
  a REC cannot show it.
- **Pi 500 V4L, 2026-11-23** (`aprs-captures/…48k.cu8`, 295 s): 0 before, 0 after (bursts +2.5 to +6 dB in 48 kHz ≈ 6–9 dB
  CNR at best, 8-bit; `--cu8` drives the listener-IQ path, not the tap). No other APRS recordings exist (`aprs/` held only the
  log of a deleted HF WAV).

## False decodes

The exact frame check found **the shipping decoder emitting wrong frames**: at ±4.5 kHz, +1 kHz, 9 dB, 2 wrong frames in 100
packets (e.g. `G4ABC-2>APRS,WIDE1-1,WIDE28-1:!5338.86N/00854.Y8W>… .!typical`), each THREE level errors — once a clean chance
pass, once a repair. The FCS is weaker than it looks against NRZI: a level error is two adjacent bit errors, (1 + x)·e(x), so
the generator's (1 + x) factor is spent and only its degree-15 primitive factor checks: one random 3-level pattern in 32 768
passes, and with 12 slicers × 5 paths × every weak packet it happens. Every wrong frame seen arrived BESIDE the right copy
from other slicers or paths, and the vote removes it.

| Check | Before | After |
|---|---|---|
| The margin campaign `--curve 100 7 10 ab` (2 400 packets × AUTO / IQ / audio) | **2 wrong**, AUTO 1955, audio 1473 | **0 wrong**, AUTO 2354, audio 1582 |
| The full curve above (3 300 packets × 3 runs) | 0 | 0 |
| Noise, 60 min into AUTO's IQ receiver + a 1200 run's audio | — | 0 |
| Random data keyed on FM, 4 / 8 / 12 / 20 dB, 200 bursts of ~2.5 s each | — | 0 |
| test-aprs modem tables, 600 s noise per modem | 0 | 0 |

Residual risk, honestly: a chance-pass frame with NO right copy beside it (one slicer of one path, every other copy lost)
cannot be told from a right one by any rule we found; on the synthetic it did not occur once the votes were in.

## CPU (M5 Pro, one core; `test-aprs-onair --cpu-paths`)

| Path | Before | After |
|---|---|---|
| Audio (one AfskDemod at 48 kHz: 12 slicers now, the fit tabled) | 0.07 % | 0.09–0.12 % |
| IQ receiver at a 40 kS/s tap (3 discriminator filters + AFC) | 0.33 % | 0.38–0.50 % |
| + the tone bank | — | +0.12 % |
| **1200 Bd per listener, both paths** | **0.40 %** | **~0.6–0.75 %** (the higher figure with the machine busy) |
| 300 Bd (test-aprs) | 0.13 % | 0.17 % |

Pi 2 (Cortex-A7 900 MHz, NEON): perhaps ×15–20 an M5 core for this code — roughly 7 % → 12 % of one core per 1200 Bd
listener (an estimate, not measured on a Pi). Where the tap's rate gives no short tone period (e.g. 40 540.5 S/s) the fit
falls back to a Cholesky solve per sample (a little more).

## Tests

| Test | Result |
|---|---|
| `test-aprs` | 69 / 0 (was 63) |
| `test-aprs-onair` | 23 / 0 (was 20) — incl. tone bank 5 dB 43/48 and 4.5 dB 37/48 (discriminators alone: 0 and 0), random data 0 |
| `test-ax25hs`, `test-dec-dump`, `test-ais`, `test-decoder-hosts` | pass |
| `test-server-aprs-shared.mjs` at 1.2 MS/s, 2.4 MS/s, 960 kS/s | 27 / 0 each (a Mac build of this branch without FAAD2 / xaac / RADE) |

## Open decisions for Stuart

1. **Off-tune trackers.** The 3+ dB is for trackers within a few hundred Hz (the HF+ is accurate; most trackers are). At 1 kHz
   off it is 1–2 dB, at 2 kHz with wide deviation none. Next lever: frequency hypotheses in the bank (±600 Hz, ×3 its CPU) or a
   better low-CNR AFC. Not built.
2. **Proof on air.** The XCover REC cannot show the tone bank. A decoder capture of the tap's IQ (`VIBE_DEC_AUDIO_DUMP`
   `.cf32`) from a server with this build — or simply this build on the XCover — is the real test.
3. CPU on the smallest boards: about +0.3 % of an M5 core per APRS listener; if a Pi 2 profile minds, the tone bank (or the
   second slicer set) could be a benchmark-set default.
