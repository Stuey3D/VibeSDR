# The AGC across three RTL-SDR Blog V4s — measured, 2026-09-27

**12 runs: 3 boxes x 4 stations, 90 s each, driven over HTTP/WS only.** The probe captures the
SPEC frames at the widest span each box offers and tracks the **sea** (the median bin — with a
whole FM band in frame most bins lie BETWEEN carriers) against the gain the AGC chooses.
**Headroom = peak - sea.** Intermodulation raises the sea; more wanted signal does not. Stuart,
2026-09-27: *"you need to look at the surroundings to see for intermodulation, the big wavy sea of
signals, increase the VFO bandwidth to take in a wider sample."*

## ★★★ THE RESULT: EVERY EXTRA dB OF GAIN COSTS ABOUT 1 dB OF HEADROOM — ON ALL THREE BOXES

| run | dHeadroom per dB of gain | over |
|---|---|---|
| Pi 500 Northampton | **-0.97** | 3.7 -> 12.5 dB |
| Pi 500 Heart | -0.81 | 12.5 -> 19.7 |
| Pi 500 Flex | -0.66 | 8.7 -> 15.7 |
| Pi 500 R1 | **-1.05** | 12.5 -> 25.4 |
| Pi 2 Northampton | **-3.15** | 29.7 -> 42.1 |
| Pi 2 Heart | -1.24 | 25.4 -> 43.9 |
| Pi 2 Flex | -0.78 | 29.7 -> 43.4 |
| Pi 2 R1 | -1.15 | 29.7 -> 42.1 |
| XCover Northampton | -0.88 | 16.6 -> 44.5 |
| XCover Heart | -1.12 | 22.9 -> 44.5 |
| XCover Flex | -0.41 | 22.9 -> 44.5 |
| XCover R1 | +0.04 | 22.9 -> 44.5 |

**A slope of -1 means the gain bought NOTHING**: signal and intermod rose together, so the ratio a
listener actually hears stayed flat while the tuner moved closer to its limit. This is the
[[agc_climbs_because_rail_comes_after_intermod]] diagnosis, now with a number on it.

## ★★★ AND IT IS **NOT** A 32-BIT FAULT. THAT THEORY IS REFUTED BY MEASUREMENT.
Headroom the AGC gives away by where it parks, vs the best gain it actually visited:

| box | arch | worst run | chose | best | **given away** |
|---|---|---|---|---|---|
| **Pi 500** | **64-bit** | R1 99.7 | 22.9 dB (21.4) | 12.5 dB (31.8) | **10.4 dB** |
| **Pi 2** | **32-bit** | R1 99.7 | 40.2 dB (17.6) | 29.7 dB (32.9) | **15.3 dB** |
| **XCover** | **64-bit** | R1 99.7 | 43.9 dB (27.6) | 33.8 dB (31.1) | **3.5 dB** |

✗ **The Pi 500 — the box that "works fine" — gives away 10.4 dB.** It looks healthy only because
its amplified YouLoop means it rarely has to climb; asked for a weak station it fails the same way.
✓ **All three fail on the SAME STATION.** In 9 of 12 runs the AGC parks exactly at the best gain it
visited; all 3 failures are **BBC R1 99.7**, the weakest. **The variable is the station, not the
word size.**
★ The Pi 2 is worst by DEGREE — 15.3 dB given away and a -3.15 slope, ~3x the others — but that is
a difference of degree, not of kind, and this data cannot separate "32-bit" from "longwire".

## ▶ WHAT THIS MEANS FOR THE FIX
The climb has no idea the sea is rising with it. Its instruments (ADC railing, separation, band
contrast) all read "fine" because intermod lifts wanted and unwanted together — and **headroom is
exactly the number that does not**. It is measurable from frames the server already computes.
▶ Stop the climb when dHeadroom/dGain goes to zero: that is the knee, per band and per antenna,
discovered with no owner input — which is the requirement (*"the whole idea of the AGC is so that
novice users dont have to think of gain limits"*, *"AGC cannot simply work on one antenna combo"*).
▶ ✗ Constrained by [[never_limit_permanently]]: it must DECAY and re-probe, never become a cap.

## ✗ CAVEATS — READ BEFORE QUOTING ANY NUMBER
- **Spans differ** (Pi 500 2.4 MHz, Pi 2 2.048, XCover 1.2), so the ABSOLUTE sea values are NOT
  comparable across boxes — a narrower span holds proportionally more strong adjacent signal. The
  **within-run slope is sound**, because the span is constant through a run. ✗ Do not build a
  cross-box claim on absolute sea.
- One run each, 90 s, not repeated. Two Pi 500 probes overlapped a retune.
- The 32-bit split stands for the **DAB re-rate teardown** ([[dab_crash_was_the_dongle_not_the_code]]).
  ✗ It does NOT carry over to the AGC. They were assumed to be one root cause; they are not.

## ✓ VALIDATED ON AIR — Pi 2, 5.6.65, 2026-09-27 morning
The fix is the band-contrast veto firing without its separation gate. Same probe, same stations,
same 90 s runs. **EXCURSION = how far headroom DIVED below where it settled** — Stuart's own
description of the fault: *"it bumped it up to almost maximum which caused huge intermodulation,
overload warning came up and gain went to 44 dB which cleaned up"*. The loop reaches a good place
by first manufacturing the mush and being rescued.

| station | dive BEFORE | dive AFTER | settled before -> after |
|---|---|---|---|
| Northampton 104.2 | **39.1 dB** | **0.2** | 29.7 -> 28.0 dB |
| Heart 96.6 | **22.9 dB** | **0.0** | 25.4 -> 28.0 dB (headroom 52.4 -> 55.4, BETTER) |
| Flex 96.1 | **10.7 dB** | **0.0** | 29.7 -> 28.0 dB |
| R1 99.7 | 0.0 | 0.6 | 40.2 -> 43.9 dB |

★★★ **THE DIVE IS GONE ON ALL THREE STATIONS WHERE THE METRIC IS SOUND.** It now walks to the same
destination instead of vaulting past the cliff and being dragged back.

## ✗✗ R1 99.7 IS NOT A VALID TEST CASE, AND I NEARLY PUBLISHED IT AS A REGRESSION
I reported R1 as worse (headroom 17.6 -> 13.6). ✗ **Withdrawn.** Two faults in that reading:
1. **`peak` is the strongest bin in the WHOLE SPAN, not the tuned station.** So "headroom" is band
   contrast, and where a neighbourhood is quiet it collapses for lack of anything strong in view —
   which my metric cannot distinguish from a band being FILLED IN. At 99.7 both Pis measure the
   neighbourhood 25-30 dB quieter than around 104.2, so that is exactly the ambiguous case.
2. ★★★ **I CALLED R1 "THE WEAKEST STATION" BECAUSE THE AGC GAVE IT THE MOST GAIN** — inferring the
   signal from the very judgement under test. Stuart: *"99.7 is not a weak station though it doesnt
   need that much gain"*, and *"there are multiple similar strength stations nearby"*.
★ **Ground truth from Stuart: the XCover sits at 43.9 dB on 99.7 and is CLEAN.** The Pi 2 settles
at the same 43.9 dB. So the landing point is not condemned by anything but my own number.
▶ **A PROXY IS NOT THE TEST** [[test_proxy_is_not_the_test]]. Before quoting headroom again, check
there IS a strong signal in the span for `peak` to be about.

## ★★★ THE AGC'S ANSWER DEPENDS ON WHICH DIRECTION IT ARRIVES FROM
My probe measured the XCover settling at **44.5 dB** on Heart 96.6. Stuart, manually, watched the
same box and station settle at **22.9 dB** — **21 dB apart, same build**. He arrived from 95.5 with
the gain already high, overloaded, and was trimmed DOWN to a good spot; my probe tuned in cold and
climbed UP, stopping early and high. ★★ Climbing finds a worse answer than falling does, and the
overload excursion is what DELIVERS the good result where it happens at all.
▶ So "what gain does this station need" has no single answer today. That is fatal for set-and-forget,
and it is the next thing to fix: **the climb needs a reason to stop that does not require an
overload first**, so the same place is reached from either direction.

## ★★★ AND WHERE THE RAIL NEVER COMES, NOTHING RESCUES IT (Stuart's hypothesis, CONFIRMED)
*"I wonder if the intermodulation on the wire antenna is not high enough to trigger the overload
warning as I never see it on the Pi2."* Measured across all 12 baseline runs: **all four runs where
the ADC never reached the rail are among the five worst headrooms** (17.8, 18.6, 18.3, 26.9 dB),
with adcPeak stuck at -1.6 to -3.0 dBFS. The intermod is real and the converter is still short of
full scale, so the overload never fires.
✗ **NOT a Pi 2 or a 32-bit property**: the Pi 500 does it too. ★ Railing is not sufficient either
(XCover Heart railed at 21.6 dB headroom) — but NOT railing is reliably fatal.
