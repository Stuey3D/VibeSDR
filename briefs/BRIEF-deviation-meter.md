# BRIEF — The deviation meter reads low on quiet programme

**Status:** diagnosed 2026-09-25, fix not applied — one presentation decision is Stuart's
**Reported by:** Onfliner, 2026-09-25
**Scope:** `vibedsp/pipeline.cpp` (the measurement) · web client + app panels (the display)

## The report

*"Deviation is measured correctly only for loud radio stations (in terms of sound processing), while
for quiet stations the readings are underestimated (jazz, classical, speech programmes). I hope that
by solving this problem, the issue of the slow display of the deviation scale will also be resolved."*

★ His last sentence is right: **it is one fault with two symptoms.**

## The quantity is correct. The statistic is not.

The obvious theory — that we meter the *audio* instead of the MPX — is **wrong**, and worth recording
so nobody spends a day on it. `pipeline.cpp:1531` reads `demodBuf_`, which is the raw discriminator
output: after the demod and the ~1 Hz DC blocker (which removes tuning error, not deviation) and
**before** de-emphasis, before the 15 kHz audio low-pass, before the stereo matrix. It is then band-
limited to 66 kHz (`mpxLp_`, `:1563`) because the unfiltered peak once read "106 kHz OVERMODULATED"
on a clean station from broadband noise above the composite. Full MPX, peak of |x|, correctly scaled.

**What we publish is the 1.5 s exponential average of 50 ms window peaks** — `pipeline.cpp:1629`:

```cpp
mpxDevSm_ += aSm * (pk - mpxDevSm_);       // aSm = 1 - exp(-0.05/1.5), SYMMETRIC
```

A single pole with τ = 1.5 s **in both directions**. So:

- **Processed programme** (crest factor 2–4 dB, modulation almost continuous): nearly every 50 ms
  window peaks at the same value, average ≈ peak, and the reading is right.
- **Jazz, classical, speech** (crest factor 10–20 dB, long gaps between peaks): most windows contain
  no peak at all, so the average sits far below the true peak. On an assumed speech duty cycle a
  **75 kHz peak reads ≈ 38 kHz**.
- **And the same pole is the slow scale**: from zero, 63 % in 1.5 s, 95 % in 4.5 s, plus a 1.5 s
  post-retune blanking that zeroes the accumulator (`:1626`) — ~5–6 s before the number means
  anything. Ballistically that is a VU meter, where the instrument wanted is a modulation monitor
  (instant attack, held peak).

★★★ **THE HEADER STILL DESCRIBES THE METER WE USED TO HAVE.** `vibedsp.h:2453` says "FAST ATTACK,
SLOW DECAY" and `:2458` says `mpxDevSm_` "rises INSTANTLY to a new peak". It does neither. The
*in-code* comment at `pipeline.cpp:1605` records the change correctly ("the bar is the AVERAGE of
window maxima… Stuart: 'average it the same as the other measurements'"). One rule, two readers,
again — and the stale reader is the design note, so anyone checking `vibedsp.h` concludes the meter
is already a peak meter and looks elsewhere. **That is why this took an outside tester to find.**

★ The 39→74 kHz syllable swing that the averaging was added to tame is recorded at `vibedsp.h:2456`.
It was not noise. It was the measurement.

## Two smaller under-reads on top

1. **The percentile scales with the sample rate.** `pipeline.cpp:1618`: `skip = devWinCnt_ * 0.0003`
   discards the top ~3–4 samples of every window at 250 kHz. A sustained tone has hundreds of samples
   up there and loses nothing; a **sparse orchestral transient is a few samples wide and is discarded
   whole**. The bench figures that justified it were taken on a spiky multi-tone, not real programme.
   Fix: a fixed `skip` (≈2), not a proportional one.
2. **The noise subtraction compounds the error.** `:1649`: `sqrt(mpxDevSm_² − 4.5²σ²)`. Because
   `mpxDevSm_` is already depressed, the subtraction removes a larger *fraction* on quiet programme.
   The guard only trips past 60 % removal, so anything under that passes silently, and `kC = 4.5`
   (`:1637`) is a bench fit for the *averaged* statistic.

## Calibration is fine — checked, so nobody re-checks it

`FmDemod((float)(chFs_ / (2π·75000)))` (`pipeline.cpp:626`) is a phase-difference discriminator, so
output 1.0 ≡ 75 kHz peak **at every sample rate** — the `chFs_` cancels the per-sample phase step.
`mpxDevKHz = mpxDevOut_ * 75` agrees, the histogram is 0.1875 kHz/bin over 0–96 kHz, and the guard-
band correction is correctly disabled below a 180 kHz channel rate. **No scale factor drifts with IF
bandwidth or sample rate.**

★ One *physical* under-read is not ours: below ~180–200 kHz IF bandwidth the channel filter clips the
FM sidebands (Carson ≈ 2·(75+15) kHz) and the deviation really is reduced. Worth asking a reporter
what bandwidth they ran before treating a low reading as a bug.

## What the references do

- **PIRA CZ P75/P175/P275** — the family this repo already calibrates RDS deviation against: peak
  holds over a **50 ms window, 20 times a second**, from which it shows **MAX, AVE and MIN**.
  ★★★ **Our 50 ms window is already exactly PIRA's. We just publish the AVE and label it
  "deviation".** The figure a broadcast engineer reads is the MAX. (PIRA also separates +peak from
  −peak; we take |d| and so cannot show asymmetry.)
- **MpxTool** (Onfliner's reference): an FCC-accurate modulation monitor with an adjustable response
  time and a peak flasher — a true peak instrument. The FCC/ITU convention is *instantaneous peak*,
  so a quiet station hitting ±75 kHz on its loudest note reads 75 whatever its average level.
- ✗ Not to be confused with **MPX power (ITU-R BS.412)**, which is RMS over a rolling 60 s.

## The proposed fix — and the one decision that is Stuart's

Keep the quantity. Publish **three figures from the window array we already have**, as PIRA does:

1. `mpxDevKHz` becomes a **true peak meter** — instant attack, ~0.9 s decay:
   `mpxDevSm_ = (pk > mpxDevSm_) ? pk : mpxDevSm_ * expf(-dtW / 0.9f);`
   ★ which is what `vibedsp.h:2458` already claims, so the header becomes true instead of needing a
   rewrite.
2. Add `mpxDevAvgKHz` carrying the **present 1.5 s average**, so nothing is lost and the panel can
   read **"peak 74 · avg 41 kHz"**. Stuart's original *"average it the same as the other
   measurements"* is still honoured — it is relabelled, not removed.
3. `mpxDevHoldKHz` keeps its 6 s decay as the excursion memory (PIRA's MAX). A user-resettable
   infinite hold would match MpxTool's peak flasher.
4. Post-retune blanking 1.5 s → 0.3–0.5 s; that was sized for a slow meter.
5. Emit rate stays 6 Hz: with instant attack the server value already peak-holds between frames.

★★★ **THE DECISION.** A peak meter twitches, and the yo-yo Stuart disliked comes back — but it comes
back *as information*, with the steady number beside it. Every real modulation monitor shows both.
This is a presentation choice, not a DSP one, so it is his call whether the bar reads peak with avg
beside it, or the reverse.

★★★ **DO NOT SHIP THE FAST ATTACK WITHOUT RE-FITTING `kC`.** With instant attack the noise
contribution to a peak is not σ-like, and an uncorrected peak on a weak signal is exactly the
"106 kHz OVERMODULATED" failure `vibedsp.h:2461` was written to prevent. Safest order: keep the
subtraction on the average path, and for the peak either re-fit `kC` on the bench or subtract the
same absolute kHz that was taken off the average.

## How to judge it without a dial

`vibedsp/test/bench_eye.cpp:34` already captures `mpxDevKHz`, `mpxDevHoldKHz`, `mpxDevNoiseKHz` and a
`devs` series — the harness exists. A **captured MPX trace of classical programme** replayed through
it settles the size of the under-read on the bench, which is how the auto squelch was tuned
([[BRIEF-auto-squelch]]: *"do not do this by ear"*).

## Also to fix, both misleading to the next reader

- `vibedsp.h:2453-2460` — describes a fast-attack meter that no longer exists.
- `main.ts:10156` and `AdvRdsPanel.tsx:589` — the comment says "THE BAR SHOWS THE PEAK HOLD, NOT THE
  PROGRAMME LEVEL"; the bar fill is the smoothed level and only the tick is the hold.

## One implementation, and that is the good news

Both clients are pure display of the server's `mpxDev`/`mpxHold`/`mpxNoise` and they **agree** — same
10 dB in / 8 dB out S/N hysteresis, same 75/82 kHz verdicts. Jr has no deviation surface. So this is
one fix in one place: `local_sdr_shim.cpp:10298` serialises it, `main.ts:10050` and
`AdvRdsPanel.tsx:571` draw it.

★ `eyeDevKHz` (`pipeline.cpp:1880`) is the eye plot's autoscale and is **documented as not a
calibrated deviation** — it under-reads when the pilot dominates. If a reporter compares two of our
own numbers, that caption is the likely pair, and it is meant to differ.
