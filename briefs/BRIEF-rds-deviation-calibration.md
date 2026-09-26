# BRIEF — The RDS deviation reads low, and two references agree

**Status:** diagnosed 2026-09-26, not fixed — the decisive measurement is available and worth waiting for
**Reported by:** Onfliner (against MpxTool), corroborated by Hans's PIRA table from 2026-07-27
**Scope:** `vibedsp/rds.cpp` `rdsDeviationKHz()`

## What Onfliner reported

Three separate things, and they must not be run together:

1. **Total MPX deviation** — quiet speech station overmodulating: MpxTool ~81 kHz, ours lower; loud
   music station: MpxTool ~76 kHz, ours the same. ★ That is exactly
   [[deviation_meter_publishes_the_average]], on the OLD build. Fixed in Android 493. **Not a new
   fault**, and his own words ("on a loud music station… vibesdr shows the same values") are the
   control case that proves it.
2. **Pilot** — ours consistently **2–3 kHz LOW** against MpxTool.
3. **RDS** — ours low by 0.27–1.9 kHz over eleven stations, strong signal, minimal multipath.

## ★★★ THE PILOT IS CALIBRATED AND WE MATCH THE PIRA, NOT MpxTool

Hans's analyser, six Dutch stations ([[rds_pira_calibration]]):

| | RadioNL | TukkerFM | Oost | Qmusic | Radio 10 | 1Zwolle |
|---|---|---|---|---|---|---|
| PIRA | 7.0 | 7.2 | 7.4 | 5.5 | 5.1 | 6.8 |
| ours | 7.0 | 7.3 | 7.4 | 5.5 | 5.1 | 6.8 |

**Six for six.** So on pilot, MpxTool disagrees with a broadcast-standard analyser and we agree with
it. ✗ Do not "fix" the pilot towards MpxTool. ★ Our own UI calls 6.0 kHz *"low"* against a 6.75
nominal, so we are not biased high either. Worth telling Onfliner before he trusts that 2–3 kHz gap
— and worth asking WHAT MpxTool calls pilot deviation (peak? peak-to-peak? injection %?), because a
convention difference would explain it without either instrument being wrong.

## ★★★ THE RDS FIGURE, THOUGH, IS LOW AGAINST *BOTH* REFERENCES

The shipped fix changed the crest factor from a sinusoid's √2 to a **measured 1.520**
(`tools/rdsdev_cal.cpp`). Applying that to the PIRA table:

| station | PIRA | ours then | ours after the fix | PIRA / ours |
|---|---|---|---|---|
| RadioNL | 2.8 | 2.3 | 2.47 | 1.13 |
| TukkerFM | 3.5 | 2.9 | 3.12 | 1.12 |
| Oost | 2.6 | 2.0 | 2.15 | 1.21 |
| Qmusic | 4.5 | 3.5 | 3.76 | 1.20 |
| Radio 10 | 4.1 | 3.3 | 3.55 | 1.16 |
| 1Zwolle | 4.4 | 3.5 | 3.76 | 1.17 |

★★ **Even after the fix we would still read ~16 % low against the PIRA.** The constant was right to
change and did not close the gap. Onfliner's MpxTool comparison puts the gap larger still
(mean ratio 1.56; a constant **~1.0 kHz offset** fits his data better than a constant ratio,
SSE 1.84 vs 2.52). Two instruments, two eras, same direction.

## The mechanism, in order of suspicion

★★★ **1. The guard-band noise subtraction — added AFTER Hans could test, and explicitly never
calibrated.** `rdsDeviationKHz()` subtracts a noise floor measured in a guard band, which the pilot
path does **not** do. Its own comment says so plainly:

> *"No Pira analyser is available to calibrate against (and Hans can no longer test), so this
> deliberately does NOT invent an absolute scale. It makes the figure self-consistent with the
> decoder beside it."*

So Stuart's memory ("we calibrated this with Hans against his PIRA") is correct **for the pilot and
for the 1.520 constant**, and does not cover this path at all.

★★ **It is also capped, and a saturated cap is a silent constant error.**
`return std::max(corrected, raw * 0.707f)` — the correction may take at most 3 dB. If the guard band
over-reads (a neighbour, the RDS sidebands themselves, anything that is not our noise floor), every
reading is pinned at **0.707 × raw**, a 29 % under-read that looks stable and plausible. ★ The MPX
deviation path has a note about exactly this failure mode — *"a neighbour in the guard band is not
noise"* — and it cost a day there. Same shape, different band.
▶ Onfliner chose "stations with a strong signal and minimal multipath", which should make a genuine
noise subtraction NEGLIGIBLE. That it did not shrink is itself evidence the guard is measuring
something that is not noise.

**2. A residual estimator bias (~16 %).** Present in the PIRA table *before* the guard band existed,
so it is separate from (1) and would remain after fixing it. `rdsRms_` is a mean of the envelope and
1.520 is its crest factor through our own ±2.4 kHz filter — right in principle, but the filter, the
smoothing and the spec shaping all sit inside that constant.

## ★★★ THE DECISIVE MEASUREMENT IS BEING OFFERED — ASK FOR IT PROPERLY

Onfliner: *"Later, I will run my transmitter with an mpx input and compare the deviation level of
the pilot-tone and RDS more accurately."*

**That is an absolute reference and it outranks every receiver-versus-receiver comparison in this
brief.** Comparing two receivers can only ever show they disagree; a known MPX input says which is
right. Hans can no longer test, so this is now the only route to a true calibration. Ask him for:

- **Pilot at a known injection** (e.g. exactly 6.75 kHz = 9 %), RDS off — settles pilot outright.
- **RDS at two known deviations** (e.g. 2.0 and 4.0 kHz), pilot off — two points give scale AND
  offset, which one point cannot separate. ★ This is the whole question: our data fits an offset
  better than a ratio, and only two known points can tell those apart.
- **Both together**, to check they do not interact.
- Each at **high SNR and again at low**, which directly exercises the guard-band subtraction.

## Before any of that, one cheap step

Expose the **uncorrected** figure (`raw`) beside the corrected one, even just in the admin payload.
If `raw` matches MpxTool and `corrected` does not, suspicion (1) is confirmed in one reading and no
transmitter is needed. ✗ Do not re-fit the constant against Onfliner's table — that is fitting to a
second receiver, which is how the last "0.75x + 0.2" artefact happened
([[rds_pira_calibration]]: *"MEASURE the constant, don't fit it to the data"*).

## ★★★ 2026-09-26 — THE MAIN DEVIATION MONITOR IS CONFIRMED. THE RDS FIGURE IS NOT.
Two independent checks landed the same evening:

1. **Saber, against MpxTool:** *"within 1Khz"* — a reference instrument, a different operator, a
   different receiver. Stuart: *"SO now I'm not as concerned about the deviation monitor."*
2. **Stuart's own RTL vs RSP A/B**, same transmitter, same moment, three stations:

| | 100.4 | 96.6 | 96.1 |
|---|---|---|---|
| dev avg RTL / RSP | 51 / 49 | 66 / 65 | 78 / 80 |
| peak RTL / RSP | 58 / 58 | 75 / 80 | 92 / 94 |
| **pilot** RTL / RSP | 6.8 / 6.8 | 6.5 / 6.1 | **6.4 / 4.9** |
| RDS lock | both | both | clean 43 % / **no lock** |

★★ **TOTAL MPX DEVIATION AGREES WITHIN 1–2 kHz ACROSS DIFFERENT HARDWARE.** That is the property
that matters: the figure is of the SIGNAL, not of the radio. ✓ Closed.

★★★ **BUT THE PILOT DOES NOT AGREE WITH ITSELF** — 6.4 vs 4.9 kHz on 96.1, a 23 % spread on one
transmitter at one instant. It tracks SIGNAL QUALITY (the RSP showed SNR 35 dB and **RDS no lock**
there; the RTL got a clean 43 % scatter), and the two radios agree EXACTLY on 100.4 where both are
strong. ▶ So the pilot estimate appears to DEGRADE WITH SNR rather than be miscalibrated — which is
a different fault from the MpxTool disagreement and needs its own measurement.
✗ Still do not "fix" the pilot towards MpxTool: it matches Hans's PIRA six for six.

★★★ **AND THE VERDICT FLIPS ON A BOUNDARY, WHICH IS A UX FAULT.** On 96.6 the two radios read peak
75 and 80 — 7 % apart — and printed **"nominal"** and **"over the limit"**. The numbers agree; the
WORDS contradict. A listener comparing two receivers will trust neither.
▶ Needs hysteresis, a wider band, or a qualifier when SNR is marginal. A boundary that a 5 kHz
difference can cross must not speak in absolutes.

## ▶ STILL OPEN: the RDS SUBCARRIER figure (~16 % low against BOTH references)
Unchanged by any of the above — it is a different number. The guard-band subtraction remains the
prime suspect and **the cheap diagnostic has still not been done**: expose the UNCORRECTED `raw`
beside the corrected one. If `raw` matches and `corrected` does not, it is confirmed in one reading
and no transmitter is needed.

## 2026-09-26 — THE CONTROL ARM IS BUILT (5.6.56, commit 24e2d453)

The cheap diagnostic this brief has recommended since it was written now exists.
`RdsDemod::rdsDeviationRawKHz()` returns the averaged estimate with the guard-band noise
subtraction **skipped** and nothing else changed; it is published as `rdsDevRaw` beside `rdsDev`
and drawn in the ADV RDS row only where the two differ by more than 2 %.

★★★ **WHY THIS AND NOT A NEW CONSTANT.** `rdsDeviationKHz()` does TWO separable things — applies
1.520, and subtracts a floor in power. Both are suspects for the ~16 % deficit, and bundled into
one figure neither is testable. That is why "about 1.3 dB low against a Pira" survived two months
as a known-but-unresolved note: nobody could tell the halves apart.

### How to read it, on a live station, no transmitter needed
| observation | conclusion |
|---|---|
| `raw` lands on MpxTool, `avg` ~16 % under | **the guard band is eating signal** — consistent with the four-radio wide/narrow result in `rds.cpp` (both WIDE radios read 0.0 "no subcarrier" at 0 % block errors). 1.520 is innocent; fix the guard. |
| `raw` and `avg` sit ~16 % low **together** | the subtraction is fine and **the constant is the suspect** — and that case needs a KNOWN MPX INPUT, not a fit. |
| `raw` == `avg` and no `raw` shown | the operator has the guard band **off**; turn it on or the experiment cannot run. |

### What was deliberately NOT done
- ✗ **1.520 was not changed to 1.770.** That fits a constant to six of Hans's stations. The Pira
  session's own rule stands: *measure the constant, don't fit it*.
- ✗ **The percentile in `rdsDeviationPeakKHz()` was not retuned.** On 104.2 it puts peak/avg at
  1.431 where Hans's table implies 1.164 — ~23 % over, with much wider scatter than PIRA's. Tuning
  it until it lands on the table is the same mistake in a new parameter.
- ✗ **`rdsDeviationKHz()` itself is untouched**, and keeps the verdict wording. Stuart, 2026-09-26:
  *"we must however also preserve our PIRA tested numbers"*, and *"we do need the average as it was
  what made the number move like a stopwatch"*.
- ✗ **Not implemented as a flag/default parameter on `rdsDeviationKHz()`.** A default parameter is
  precisely what let three "written and never read" fields ship in this project. It duplicates one
  line and says so, so the 1.520 cannot move in one branch only.

### One trap worth keeping
The control arm rides the **same 1.5 s smoother** as the average. Taking it live beside a smoothed
figure would put the filter's own wobble into a difference that is only 16 % — the same order. Both
share one sentinel (`agg_.groupTotal <= 0` is the ONLY -1 path in either), so one branch carries
both. ★ Compare like with like or the experiment measures the filter.

▶ **NEXT:** get one reading off a station with the guard band ON, beside MpxTool. That single
reading picks a row in the table above and ends the ambiguity.

## 2026-09-26, LATER — THE CONTROL ARM ANSWERED IN ONE EVENING: IT IS THE GUARD BAND

Five stations, Pi 500 / Airspy HF+ / Northampton, all read off the ADV RDS row:

| station | SNR | errors | avg | peak | raw | raw x 0.707 | pinned at the clamp? |
|---|---|---|---|---|---|---|---|
| Flex FM 96.1  | 29 | 14% | 2.6 | 5.8 | 3.6 | 2.55 | **YES** |
| Heart 96.6    | **58** | **2%** | 1.4 | 2.6 | 2.0 | 1.41 | **YES** |
| BBC R1 99.7   | 36 | 4%  | 1.2 | 2.4 | 1.8 | 1.27 | **YES** |
| Classic 100.4 | 37 | **0%** | 2.3 | 3.5 | 2.6 | 1.84 | no |
| BBC Nhtn 104.2| **68** | **0%** | 1.7 | 2.0 | 1.8 | 1.27 | no |

### ★★★ FINDING 1 — THE PUBLISHED AVERAGE IS OFTEN THE CLAMP, NOT A MEASUREMENT
`rdsDeviationKHz()` ends `return std::max(corrected, raw * 0.707f)`. On **three of five stations the
answer IS that floor** — the guard-band subtraction wanted to remove more than half the power and
was capped. The clamp's own comment says a correction that deep "is evidence that the guard band is
seeing something other than our noise". That evidence is now in.

★★ **AND NOT ONLY ON WEAK SIGNALS.** Heart 96.6 is 58 dB SNR at 2 % block errors and is pinned. My
first reading was Flex FM (29 dB, 14 % errors) and I wrote it off as a weak-signal artefact; four
more samples killed that. ✗ Do not re-explain this as noise — a clean strong station does it too.

▶ **So the ~16 % deficit Onfliner measured lives in the NOISE SUBTRACTION, not in the 1.520 crest
factor.** The note on `rdsDeviationKHz()` has blamed the constant since 2026-07-27 and pointed every
investigation the wrong way. ✗ **Do NOT change 1.520.**

### ★★★ FINDING 2 — THE MEASURED PEAK IS RIGHT ON A CLEAN SIGNAL
BBC Northampton (68 dB, 0 % errors, RDS-to-pilot "99 % steady") gives **peak/avg = 2.0/1.7 = 1.18**,
against the **1.164** Hans's PIRA table implies the correction should be. Within reading noise of
exact agreement. The earlier "peak over-reads ~23 % with wide scatter" was measured on NOISIER
signals — 104.2 at the time was not the clean case it is here.
▶ `rdsDeviationPeakKHz()` may be sound and simply needs a SIGNAL-QUALITY GATE (block errors ~0,
phase steady) rather than being shown unconditionally or distrusted unconditionally.

### ▶ NEXT, and none of it needs MpxTool
Stuart does **not** have MpxTool — Defender quarantined it as `Wacatac.A!ml` (a generic ML
heuristic). ✗ Do not suggest installing or unblocking it. Neither finding above needs it: the clamp
result is arithmetic against our own output, and the peak result is checked against Hans's table.
1. **Count the clamp.** Publish how often `raw * 0.707f` wins. If it is most of the time, the guard
   band is not measuring a noise floor at all and the correction should be reconsidered whole.
2. **Ask WHY 63 kHz is hot** on a clean strong station. The four-radio wide/narrow result already
   said a wide IF admits adjacent-channel energy into the guard; these five say it happens at 195 k
   too (every row above ran IF 186-195 k).
3. **Onfliner is the absolute anchor**, not Stuart — he measured the eleven-station deficit on his
   own MpxTool and can re-read `raw` beside it now that it is published.

### ★★★ FINDING 3 — TWO DIFFERENT RADIOS AGREE, SO IT IS NOT THE RECEIVER
Same five stations re-read on the Pi 500 with an **RTL-SDR Blog V4** in place of the Airspy HF+:

| station | Airspy avg/raw | V4 avg/raw | clamped |
|---|---|---|---|
| Heart 96.6   | 1.4 / 2.0 | **1.4 / 2.0** | both |
| Flex 96.1    | 2.6 / 3.6 | **2.6 / 3.6** | both |
| BBC R1 99.7  | 1.2 / 1.8 | 1.3 / 1.8 | both |
| Classic 100.4| 2.3 / 2.6 | 2.0 / 2.2 | neither |
| BBC Nhtn 104.2| 1.7 / 1.8 | 1.1 / 1.5 | Airspy NO, V4 YES |

★★ **Flex is identical to the decimal on both radios** (avg 2.6 · peak 5.8 · raw 3.6) despite 14 %
vs 56 % block errors and 29 vs 26 dB SNR. RDS injection is a transmitter property and both
receivers agree on it — **the meter is reproducible across front ends**, which is a good result for
the instrument and a bad one for any "it's that radio" explanation.
▶ The SAME stations clamp on BOTH. So the clamp is not an Airspy characteristic and not an RTL one:
it is the guard-band measurement, or what reaches it.

### ✗ THE IF-WIDTH PREDICTION WAS NOT TESTED — DO NOT RECORD IT AS DISPROVED
I predicted a narrower IF would clamp less. **Both runs were WIDE** — the ADV RDS row read
"IF NARROW: wide · 110k would cost −16.4 dB" on the V4 and the same on the Airspy. ★ The V4's
footer "IF 2800 kHz" is the TUNER CAPTURE width, not the demodulator IF; do not read it as narrow.

### ▶ THE ONE CHEAP EXPERIMENT LEFT, before touching any of this code
**Heart 96.6 (59 dB, 0 % errors — the cleanest clamped case), force IF NARROW to 110k or 168k, and
re-read `avg` and `raw`.**
- `avg` comes off `raw * 0.707` ⇒ adjacent-channel energy IS landing in the 63 kHz guard; fix how
  the guard is measured, or gate the correction on IF width.
- still clamped at 110k ⇒ the guard is not being contaminated from outside at all and **the
  subtraction itself is wrong** — a deeper fix, and a different one.

## 2026-09-26, FINAL — THE IF-WIDTH THEORY IS DISPROVED; IT IS THE SUBTRACTION

Heart 96.6 swept across every width on the RTL V4 (SNR 59 throughout):

| bandwidth | errors | MPX S/N | avg / raw | raw x 0.707 | clamped |
|---|---|---|---|---|---|
| 110k (+/-55k) | unmeasurable | 5 dB | 3.3 / 4.7 | 3.32 | yes (void — signal destroyed) |
| 140k (+/-70k) | 39% | 29 dB | 1.2 / 1.7 | 1.20 | yes |
| 187k | 54% | 23 dB | 1.3 / 1.8 | 1.27 | yes |
| wide | 0% | 32 dB | 1.4 / 2.0 | 1.41 | yes |
| **MAXIMUM (+/-250k)** | **0%** | **32 dB** | **1.5 / 2.2** | **1.55** | **yes** |

★★★ **THE CLAMP FIRES AT EVERY WIDTH, INCLUDING MAXIMUM ON A PRISTINE SIGNAL** — 0 % block errors,
pilot locked 6.7 kHz nominal, "clean · no treatment". At maximum the passband admits MORE
adjacent-channel energy than any other setting and the correction is no deeper. ✗ **IF width does
not move it. The leakage-into-the-guard theory is DISPROVED, not merely untested.**

### ✗ FOUR WRONG THEORIES — DO NOT RE-RUN THEM
1. ✗ The receiver — two radios give identical figures.
2. ✗ IF width / adjacent-channel leakage — disproved above.
3. ✗ The 1.520 crest factor — `raw` uses it too and behaves sanely.
4. ✗ **The guard is at the wrong frequency.** I claimed the rotation's sign put it at 51 kHz inside
   the stereo subcarrier. **WRONG — I had the sign backwards.** After downconversion 63 kHz sits at
   **+6 kHz**, and `x·e^-jθ` shifts DOWN, moving +6 kHz to DC. The guard IS at 63 kHz as intended.
5. ✗ Mismatched filters — `lpfI_` and `lpfGI_` are built from the SAME taps and SAME decim.

### ★★★ WHAT THE ARITHMETIC DEMANDS
The clamp fires when `sqrt(sigPow) * 1.381 < rdsRms_ * 1.520 * 0.707`, i.e. `sigPow < 0.605 *
rdsRms_^2`. With `rdsPow_ = k * rdsRms_^2` (k ~ 1.0-1.5 for a biphase envelope), that needs
**guardPow_ to be 40-60 % of rdsPow_**. On a 0 %-error, 32 dB signal that is NOT a noise floor.
▶ So either the guard really does see that much at 63 kHz, or **rdsPow_ and guardPow_ are not on
the same scale** (a gain/normalisation applied in the RDS symbol loop but not the guard loop).

### ▶ NEXT — PUBLISH THE RATIO, DO NOT REASON ABOUT IT
★★★ Expose `guardPow_/rdsPow_` exactly as `raw` was exposed. One field, one reading, and it
separates the two branches above. **The `raw` control arm answered a two-month-old question in one
evening; reasoning from the source got it wrong four times in one night.** [[test_proxy_is_not_the_test]]

## ▶ WHY ONFLINER WANTS +/-250k ALWAYS AVAILABLE — tonight is the evidence
Maximum bandwidth was the ONLY setting that gave a clean measurement (0 % errors, 32 dB, pilot
locked, "no treatment"). Every narrower one degraded the thing being measured; 110k left the panel
unable to measure anything. ★ For deviation/RDS work the widest setting is not a luxury, it is the
only width where the instrument is trustworthy. Record this as the REASON, not just the request.

## 2026-09-26 — ONFLINER'S CALIBRATED SWEEP: A KNOWN TRANSMITTER, 22 POINTS

Airspy Mini -> VibeSDR, RTL-SDR -> MpxTool, RDS level set on his own transmitter with Stereo Tool.
**This is a CALIBRATED SOURCE, not field data — it outranks every field reading in this brief.**

```
fit   mpxtool = 1.1852 * vibesdr + 0.12      (max residual 0.154 kHz; quantisation +/-0.05)
through origin           = 1.2173 * vibesdr  -> we under-read by 17.8 %
top half of the range    = 1.2086
```

### ★★★ IT IS A SCALE ERROR, AND THE SUBTRACTION IS ONLY A SMALL TERM
The gap GROWS with level (0.1 kHz at 0.2, 1.1 kHz at 5.5) and the ratio settles flat at **1.20**.
An over-subtraction would show as a roughly CONSTANT gap; this is multiplicative.
★★ The intercept is **+0.12 kHz**: at zero deviation MpxTool would read 0.12 where we read 0, i.e.
we subtract slightly MORE noise than MpxTool. So the guard-band clamp found earlier tonight is
worth ~**0.12 kHz, not 18 %**. ▶ I over-weighted the clamp; it is real but it is not the main fault.

### ★★★ ONLY RDS IS WRONG — HIS OWN CONTROLS PROVE IT
- **Pilot deviation: 0.1 kHz low**, consistently (~1.5 % of 6.7).
- **75 kHz deviation, 400 Hz test tone: we read 1 kHz HIGH** (~1.3 %).
Two controls, both within a couple of percent, OPPOSITE signs. ▶ The MPX chain and the deviation
meter are effectively VALIDATED. The fault is specific to the RDS path. ✗ Do not go looking for a
global scaling bug.

### ▶ THE ONE READING THAT CLOSES THIS — ask Onfliner, he has the transmitter
Repeat four or five points on **5.6.60** and report **`raw` beside `avg`** (the row now prints
`avg · peak · raw`):
- `raw/avg ~ 1.41` ⇒ he is CLAMPED; `mpxtool/avg` is only 1.20, so `raw` over-reads by ~18 %
  ⇒ **1.520 is TOO HIGH**.
- `raw ~ avg`     ⇒ not clamped, the subtraction is irrelevant, and the true mean-envelope crest
  factor is `1.520 * 1.209` = **1.84** — just above Hans's PIRA-implied 1.770 +/- 0.048, from an
  INDEPENDENT calibrated source. Two sources agreeing is a MEASURED constant, not a fit.
★ Also from Onfliner: 10.5 (490) fixed the sensitivity and linear-gain sliders — they work properly.
