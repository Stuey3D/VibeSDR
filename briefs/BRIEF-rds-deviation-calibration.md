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
