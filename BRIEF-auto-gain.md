# BRIEF: Automatic gain that actually works — all bands, all tuners

**Project:** VibeSDR / VibeServer (shared C++, so Android local hardware gets it too)
**Author:** Stuart Carr (Stuey3D), 2026-07-25 — *"RTL-SDRs are notorious for having terrible auto
gain, but could we detect signal overload and do auto gain ourselves? Not for FM specifically but
for all bands."*
**Status:** DESIGN. Not started. Measurements below are real and were taken the night it was
raised.

---

## 1. Why

The tuner's own AGC is the thing being replaced. It optimises for "a signal is present", not for
signal-to-noise, and on a crowded band it habitually winds itself up until the front end is
generating more noise and intermodulation than the antenna is delivering signal. Every RTL-SDR
user learns to set gain by hand; most never find the right value, and the right value changes
with band, antenna and time of day.

Doing it ourselves is squarely in VibeSDR's remit: we already compute, every frame, the two
things the decision needs — the raw IQ and its spectrum.

## 2. ★ The measurement that shaped this (2026-07-25, real dongle, RTL-SDR Blog V4 / R828D)

Chasing RDS on a marginal station at 98.2 MHz, gain was swept across the tuner's whole range and
the *real* pipeline run over each capture (`tools`: `rtl_sdr` + `vibedsp_iq_probe`):

| tuner gain (dB) | 0 (auto) | 20.7 | 28.0 | 32.8 | 37.2 | 40.2 | 43.9 | 49.6 |
|---|---|---|---|---|---|---|---|---|
| RDS groups in 10 s | 0 | 0 | 0 | 0 | 1 | 6 | 2 | 0 |

Re-run at 20 s, both 32.8 and 40.2 gave **zero** — so the apparent peak was variance, not a gain
optimum. Two lessons, and they are the foundation of this design:

1. ★★ **Gain did not change the outcome, because EXTERNAL noise dominated.** When the noise
   arriving at the antenna is louder than the noise the receiver adds, more gain amplifies signal
   and noise equally and buys *nothing*. An auto-gain that keeps climbing in that state is not
   merely useless, it is harmful — it walks into overload chasing an SNR that cannot improve.
   **Recognising this state is the single most important thing this feature must do.**
2. ★ **Do not use a demodulator-domain metric to judge gain.** The FM pilot-lock amplitude sat at
   0.0777 at *every* gain from 0 to 49.6 dB, because FM demodulation is amplitude-limiting: the
   MPX level reflects the station's pilot injection, not the signal level or the SNR. Anything
   downstream of a limiter is blind to gain by construction.

### ★★ 2.1 The case that proves it: voices on 4582 kHz (Stuart, 2026-07-25)

Working HF RTTY at 4582 kHz, started at **22 dB** gain and had to come all the way down to
**8 dB**. The symptoms:

- **Voices audible across the RTTY.** A broadcast station that is nowhere near 4582 kHz, arriving
  there anyway. That is not a receiver "hearing too much" — it is the front end mixing strong
  out-of-band signals together and *manufacturing* a product that lands on the tuned frequency.
- ★★ **Stripes of overload across the ENTIRE spectrum, "one minute there, next gone."** Not a
  spur in one place — horizontal bands spanning the whole visible width, appearing and vanishing
  as the interferer faded. That is the *whole noise floor lifting at once*: broadband
  desensitisation, the front end being driven into compression by total power it cannot handle.
  It is exactly the signature §4's median-bin test is built to catch, and it is visible in the
  data we already compute — a sudden rise in the MEDIAN across the band with no corresponding
  rise in the peak.

Three design consequences, all of which this case makes concrete:

1. ★ **The right gain was 14 dB below where he started, and nothing was clipping.** An auto-gain
   that only avoids hitting the rails would have sat happily at 22 dB and produced exactly this.
   §4's noise-floor test is not a refinement — it is the whole point.
2. ★ **The culprit is OUT of channel, and the damage is BROADBAND.** The signal responsible was
   not the one being listened to, and the harm it did was spread across the whole spectrum rather
   than confined to a product. So the detector must watch the full captured band — the waterfall
   FFT already does, and a channel-power measurement never would — and it must track the median
   over TIME, because the tell is a floor that lifts and falls rather than one that sits high.
3. ★ **The correct gain changes with propagation.** It was right until the interferer faded up.
   That is the argument for a continuously running loop rather than set-and-forget — and for
   §5's cadence being slow enough not to pump, but not so slow it takes minutes to react to a
   band that has turned hostile.

★★ **This is not an RTL-SDR defect — it is physics, and every receiver has it.** Stuart has seen
the same overload signature across all the SDRs he has used: worst on the RTLs, but the SDRplays
"weren't immune" either. So this feature must NOT be written as "work around the RTL's bad AGC".
It is front-end protection for whatever hardware VibeSDR is driving, now or later — local RTL,
SpyServer, SDRplay, a future backend — and it must decide from MEASUREMENTS that any device
provides, never from a device-specific gain table. Per-device code should be confined to
enumerating what gain steps exist and applying one; everything above that is common.

★ Note that not every device presents a single gain control. SDRplay has an LNA state *and* IF
gain, and the right move differs between them — reducing the LNA helps overload, reducing IF gain
mostly does not. The abstraction therefore wants an ordered list of "front-end states, least to
most gain" that the device layer supplies, rather than a scalar in dB.

★ **Independently reproduced on OpenWebRX** with the same dongle and the same too-high gain.
That is worth recording twice over. It confirms the effect is the FRONT END, not anything in
VibeDSP — so no amount of DSP work would have found it. And it says that mainstream SDR software
does not protect its users from this either: the operator is simply expected to know. That is the
opportunity. Getting this right is a genuine differentiator rather than catching up, and the
"limited by overload / limited by band noise" readout in §7 is the part that turns a hard-won
piece of operator folklore into something the app just tells you.

This is also the honest user need behind the whole feature, in Stuart's words: *"Sometimes I find
it hard to set the gain and I am probably overloading the signals by giving it too much."* Too
much gain is the default mistake, it is invisible until you know the symptoms, and the penalty is
signals that appear to exist and do not.

## 3. What to measure — ADC domain, so it is band-agnostic

This must work for HF, airband, FM broadcast, ADS-B and anything else, so the decision is made
about the **converter**, not the mode. From the raw IQ block, cheaply (and vectorisable):

- **Peak** and **RMS** in dBFS.
- **Near-full-scale count** — samples within a whisker of the rails, as a fraction of the block.
- From the FFT we already compute for the waterfall: the **median bin power** (a robust noise
  floor, unlike a mean which any strong carrier drags upward) and the **strongest bin**.

Targets, as a starting point to be tuned on hardware:
- Peak below about **−3 dBFS** — headroom for FM peaks, impulsive noise and fading.
- RMS around **−20 dBFS**. Much below and ADC bits are being wasted; much above and the front
  end is being pushed for no gain in SNR.

## 4. ★ Detecting overload BEFORE it clips

Hard clipping is the *last* symptom, not the first. Intermodulation raises the noise floor across
the whole band long before any sample hits the rails, and on a band with a strong local
transmitter that is exactly how an RTL-SDR fails.

**The test — measure both against the gain step you just made.** This is the key: in a LINEAR
front end, a step of dG raises everything by exactly dG. Signal, noise, all of it. So the
diagnostic is not floor-versus-peak in the abstract, it is how each of them compares to dG:

| floor rise | peak rise | state | action |
|---|---|---|---|
| **< dG** | ~dG | ADC / quantisation limited — the receiver's own noise still dominates | **more gain helps, keep going** |
| **~dG** | ~dG | external noise dominates — signal and noise amplify together | **STOP.** Nothing further to win (§2) |
| **> dG** | ~dG | the receiver is manufacturing noise and intermod | **back off**, by more than one step |
| ~dG | **< dG** | the front end is compressing | **back off** |

All three failure states fall out of ONE experiment, needing no knowledge of the mode or the band.

★ The "floor" estimator is the fiddly part, and a median is not always right. On a quiet HF
segment most bins are noise and the median is an excellent floor; on a packed FM broadcast band
most bins contain signal and the median is not a floor at all. Prefer a LOW PERCENTILE (say the
10th-25th) of bin powers, and validate the choice per band against the rig in §8.

★ Watch it over time as well as against gain. Overload driven by a *fading* interferer shows up
as the median lifting and dropping across the whole band while the wanted signal sits still —
the "stripes across the entire spectrum" of §2.1. A gain that is correct at one moment can be
wrong a minute later, so a broadband floor excursion is itself a reason to back off, without
waiting to try a gain step.

That single comparison is the heart of the feature, it needs no knowledge of the mode, and it
would have correctly refused to keep climbing on the night this was raised.

## 4.1 ★ Prior art: ka9q-radio, and the one thing it does not have to solve

Phil Karn's `radiod` (ka9q-radio) is the best-regarded automatic gain in the amateur SDR world,
and Stuart rates the gain behaviour of the RX888 he hears on UberSDR.

★ ATTRIBUTION UNVERIFIED: whether that good behaviour is `radiod`'s AGC or something UberSDR does
on top of it has NOT been established — Stuart's own words were "unless the auto gain is
UberSDR's, I dunno". Do not repeat the claim as fact without checking. What follows is read
directly from `radiod`'s source and IS factual about `radiod`:

- Measures **IF power** — summed squared samples after DC removal, exponentially smoothed.
- Targets the midpoint of `AGC_UPPER_LIMIT` −15 dBFS and `AGC_LOWER_LIMIT` −26 dBFS, so
  **≈ −20.5 dBFS RMS**. (§3 arrived at ≈ −20 dBFS independently, which is reassuring.)
- Corrects proportionally: `new_gain = rf_gain - (new_dBFS - target_level)`, snapped to the
  hardware's discrete steps.
- Counts hard clipping (`overranges`, samples at ±32767) and uses the −15 dBFS upper threshold as
  a preventive limit.
- Runs once a second.

**Adopt:** the level target, the proportional correction, the snap-to-available-steps, and the
roughly one-second cadence. There is no reason to re-derive any of that.

★★ **But it does not need an intermod test, and we do.** The RX888 is a 16-bit DIRECT-SAMPLING
receiver — no tuner, no mixer, so its dominant failure mode really is ADC headroom, and managing
level is very nearly the whole job. An RTL-SDR or an SDRplay has a mixer-based front end that
generates intermodulation and desensitises **long before any sample reaches the rails** — that is
exactly the 4582 kHz case in §2.1, where 22 dB was badly overloaded and nothing was clipping. A
pure level-targeting AGC will sit there contentedly.

So: ka9q's loop for level, PLUS §4's median-versus-peak test for the failure it cannot see. The
two are complementary, not alternatives.

★ Worth noting that even the RX888 under `radiod` is widely reported as hard to get right, so
"copy the best implementation" is not on its own a solution. That is the argument for §7's
readout: telling the operator *which limit they are against* is the part nobody does.

★ **Licence: ka9q-radio is GPL-3.0.** Same trap as FM-DX Webserver (see `README.md` credits): the
App Store exception rests on Stuart being sole copyright holder, so incorporating third-party GPL
code would break it. Read it, learn from it, credit it — do not copy it.

## 5. The loop

1. **Coarse:** from a cold start (or a band change) set gain from the peak/RMS targets in §3.
2. **Refine:** occasionally try one step either way and keep whichever measures better on §4's
   comparison. Hill-climbing, not a full sweep.
3. **Hysteresis and cadence.** Require a margin before moving, and move slowly — seconds, not
   frames. An auto-gain that pumps is worse than one that is 3 dB wrong.
4. **Ramp, never jump.** A gain step is an instant level change in the audio and a visible seam in
   the waterfall. Ramp it, and mark the waterfall rows so the display can compensate.
5. **Never adjust during a recording**, and never while a decoder is mid-frame (WEFAX/SSTV images
   would tear, FT8 would lose a cycle).

## 6. Traps, per tuner and per band

- ★ **Enumerate the gain table, never assume it.** `rtlsdr_get_tuner_gains` returns the actual
  steps. The R820T table is widely copied and is NOT what Stuart's V4 has — that is an **R828D**.
- ★ **The RTL2832's digital AGC is a separate control** (`rtlsdr_set_agc_mode`) from tuner gain.
  Leaving it on while we drive tuner gain gives two loops fighting each other. Ours should own
  both, and the digital AGC should normally be off.
- **HF on the V4 is direct sampling** — different gain behaviour entirely; the tuner gain table
  may not even apply. Treat HF as its own case and verify separately.
- **Bias-T / external LNA** changes the whole picture: the optimum tuner gain drops when there is
  gain ahead of it. Do not fight a user's LNA.
- **Do not chase fades.** A signal that fades 10 dB for two seconds must not drag the gain with
  it; that is what the slow cadence in §5 is for.

## 7. Surface

`vibeserver_api.h` already carries `gainTenthDb` with `< 0 = automatic`, so there is a natural
home. Three modes, named honestly:

- **Auto (VibeSDR)** — this design. The default.
- **Auto (tuner)** — the dongle's own AGC, kept so it can be compared and blamed.
- **Manual** — the slider, unchanged.

The UI should show the gain the algorithm actually chose, and *why* in a word — "limited by
overload" vs "limited by band noise". That second state is genuinely useful information: it tells
the operator that their problem is the antenna or the noise floor, not the receiver, which is
exactly the conclusion that took a full evening to reach by hand.

## 8. How to test it

### ★★ 8.1 The twin-dongle rig (Stuart's design, 2026-07-25)

**Two identical RTL-SDR V4s on the same antenna through a splitter, captured simultaneously.**
This exists because of a hard limitation: **you cannot validate overload from recordings.** A
capture taken at 22 dB cannot tell you what 8 dB would have looked like — the nonlinearity is in
the hardware, so there is nothing to replay. Sequential captures do not work either, because the
band changes between them; that is exactly how a phantom "gain optimum" appeared in §2 and then
evaporated on a re-run.

★ **The point is not A/B — it is a CLEAN REFERENCE CHANNEL.** Hold one dongle at a known-good low
gain and sweep the other. Anything present on the swept channel and absent from the reference is
*unambiguously self-generated by the receiver*. That turns intermod detection from a statistical
inference into a direct observation, and it is the only way to grade the algorithm's judgement
rather than merely its behaviour. The 4582 kHz case (§2.1) would have been settled in a single
capture: voices on the 22 dB channel, silence on the 8 dB one.

**Protocol — all four steps matter:**

1. **Calibrate the pair.** Both dongles at the SAME gain, same frequency. Record the difference in
   noise floor, peak and crystal offset. Every later comparison subtracts this baseline. Without
   it, device variance is indistinguishable from the effect being measured.
2. **Reference + sweep.** One held low, the other stepped across the gain table, capturing both
   at once.
3. ★ **Swap roles and repeat.** A-low/B-high, then A-high/B-low. A conclusion that survives the
   swap is real; one that flips was device variance or thermal drift — which cancels both of
   Stuart's stated worries at the cost of doubling the captures, i.e. nothing.
4. **Judge on aggregates, not samples.** Noise floor, peak, decoded-group counts over seconds.
   The two dongles need neither a shared clock nor a synchronised start.

★ **TRAP: the two dongles leak LO into each other.** Isolation on a cheap resistive splitter is
poor, and two dongles nominally on the same frequency have slightly different real LOs (crystal
tolerance), so the leakage can beat and put a spurious tone right in the passband. Use a splitter
with decent port-to-port isolation, and sanity-check by retuning ONE dongle: a real signal stays
put, a leakage artefact moves. Budget an hour of confusion for this if it is not checked first.

Also note both dongles must be individually addressable (`rtl_sdr -d 0 / -d 1`); V4s often ship
with identical serial numbers, in which case use the device index.

### 8.2 Fallback with no extra hardware: gain dithering

Alternate a single dongle between two gain settings roughly every second, discard ~100 ms after
each change for settling, and compare the interleaved segments. This cancels slow drift and band
variation, costs nothing, and works today. It is strictly weaker — there is no simultaneous clean
reference, so intermod must still be inferred rather than observed — but it is enough to develop
against before the second dongle exists.

### ★★ 8.3 The reference signals (Stuart, 2026-07-25)

Do not test on "a station". Test on signals that are **known, continuous and DECODABLE**, so the
score is objective rather than "does it look cleaner". Stuart's set, all receivable at his
location:

| kHz | signal | role |
|---|---|---|
| 4582 | RTTY | the original overload case (§2.1) |
| 4608.1 | WEFAX | image integrity — tearing and line-sync loss are visible and unambiguous |
| 4625 | "The Buzzer" (UVB-76) | ★ **the VICTIM** — weak here, continuous, narrowband |
| 5357 | FT8 (60 m) | ★ **the AGGRESSOR** — dominant here, and self-chopping |
| 3573 | FT8 (80 m) | second aggressor, different band edge |

★ **Roles are QTH-specific — identify them, do not assume.** This brief first had these the other
way round on the reasonable guess that a Russian military transmitter would dominate a ham band.
At Stuart's location it is the reverse: the Buzzer is fairly weak and FT8 dominates. Before any
test run, look at the waterfall and decide which signal is strong and which is weak *here*.

★ **They all fit in ONE capture.** Centred on ~4605 kHz: 2.048 MSPS reaches everything except 80 m
FT8; **2.4 MSPS reaches all five**. So a single recording carries five different modulations
through the same front end at the same instant — and combined with §8.1's reference channel, the
same five through TWO front ends at two gains at the same instant. (2.4 MSPS is the noisier rate
on an RTL, but coverage matters more than a dB here, and both channels suffer it equally.)

★★ **The decisive experiment: FT8 is a built-in chopper.** FT8 transmits in 15-second slots
aligned to UTC, so a dominant FT8 signal stresses the front end for 15 seconds, stops for 15, and
repeats — a square-wave aggressor we neither have to generate nor control.

So: **measure the Buzzer's SNR during FT8 slots versus during the gaps, within ONE capture at ONE
fixed gain.** If the weak signal's SNR drops in step with the FT8 cycle, the receiver is being
desensitised by the strong one. That is a direct measurement of the entire failure mode, and it
is *self-calibrating* — the comparison is between two moments seconds apart in the same recording,
on the same hardware, at the same temperature, through the same antenna. Nothing left to confound.

Then sweep gain and plot that SNR drop against it. **The gain at which the drop disappears is the
correct gain, measured rather than judged**, and the algorithm's answer can be graded against it
directly.

The Buzzer is the better victim precisely because it is continuous and narrowband: its SNR can be
measured every few milliseconds from a single FFT bin, where FT8 yields one number per 15 seconds.
FT8's decode count and per-decode SNR remain a useful second metric on the aggressor side — is the
strong signal ALSO being damaged — and VibeSDR already ships the decoder (ft8_lib), so that needs
no new machinery.

★ Align to the UTC 15-second grid rather than hunting for the edges: the slot boundaries are known
in advance, which makes the on/off segmentation exact instead of inferred.

### ★★ 8.3.1 The aggressor does not have to be in the capture — 40 m FT8 (7074 kHz)

40 m is Stuart's best band, so **7074 kHz FT8 is the strongest aggressor available**. It does not
fit alongside the 4.6 MHz references: that would need ~2.5 MHz of span and an RTL is only
dependable to 2.4 MSPS.

★ **It does not need to.** Front-end overload is caused by TOTAL POWER AT THE ANTENNA PORT,
whether or not that part of the spectrum is being digitised. A strong 40 m signal desensitises the
receiver while you are tuned to 4625 and capturing a single megahertz around it. That is not a
compromise, it is the REALISTIC case — the thing hurting you is normally out of band, exactly as
in §2.1 where broadcast voices landed on 4582 from well outside the tuned range. A test where the
aggressor is conveniently inside the capture is the easier problem.

★★ **And this is what the second dongle is really for.** Point dongle A at the victims (~4605 kHz,
1.024 MSPS is plenty) and dongle B at the aggressor (7074 kHz). Same antenna, same splitter, same
instant. Then correlate: does A's Buzzer SNR dip whenever B sees an FT8 slot? That gives an
independently observed aggressor rather than one inferred from the clock — and it means the
question "was the band actually busy just then?" is answered by measurement instead of assumption.

With one dongle only, fall back to the UTC 15-second grid: FT8 slot timing is deterministic, so
the segmentation still works, you just cannot confirm how hard the band was being driven.

★★ **80 m is strong here too, and that gives BOTH classes of aggressor from one rig.** 3573 kHz
sits INSIDE the 4605-centred capture; 7074 kHz sits OUTSIDE it. That distinction is the whole test
of whether a detector is any good:

- **In-band aggressor (3573).** The floor lift and the victim's damage are both visible in the
  same recording — a self-contained case, and the easy one.
- **Out-of-band aggressor (7074).** Nothing about it appears in the captured spectrum at all; only
  its damage does. ★ This is the case that catches naive implementations, because any measurement
  scoped to the tuned channel — or even to the whole digitised span — misses it entirely while the
  receiver is being flattened. If the algorithm handles only the in-band case it has not solved the
  problem; §2.1 was out-of-band.

### ★★ 8.3.2 The real aggressor: Radio Romania

Stuart receives Radio Romania International **by holding the coax in his hand, in the attic**.
That is an enormous signal — no antenna, no matching, no ground, and still overwhelming. It is
almost certainly the culprit in §2.1: "voices audible across the RTTY" and a broadcast station
that strong are very likely the same fact. Worth confirming by listening to what the intermod
product actually says.

This makes it the **primary aggressor**, ahead of FT8:

- **Broadcast power, so it overloads at any gain worth using.** The condition where "just turn the
  gain down" stops being a nuisance and starts being mandatory.
- **Out of band relative to the 4.6 MHz victims**, so it exercises the hard case from §8.3.1.
- **Continuous**, so it gives no built-in chopper the way FT8 does — but it is
  **SCHEDULE-DRIVEN**, and switches on and off at broadcast schedule boundaries. That is a coarse
  chopper: capture across a start or an end time and the aggressor appears or vanishes while
  everything else stays put. Slower than FT8's 15 seconds, but a much bigger step.
- ★ **VibeSDR already carries the EiBi schedules** (used for live station bookmarks), so the test
  harness can know in advance when a strong broadcaster starts and stops — the on/off boundary can
  be scheduled rather than stumbled upon.

★★ **The rig configuration, concretely.** Radio Romania sits around 7200 kHz — outside the
victims' capture, as it should be. But it lands close to 40 m FT8, so **one dongle can watch both
aggressors**:

| dongle | centre | rate | sees |
|---|---|---|---|
| **A — victims** | 4590 kHz | 2.048 MSPS | 3573 FT8 (80 m, in-band aggressor), 4582 RTTY, 4608.1 WEFAX, 4625 Buzzer, 5357 FT8 (60 m) |
| **B — aggressors** | 7140 kHz | 2.048 MSPS | 7074 FT8 (40 m), ~7200 Radio Romania |

Five victims and both out-of-band aggressors, on one antenna, at one instant. 2.4 MSPS each also
covers it, but two dongles at 2.4 on a shared USB 2 controller drop samples — use separate
controllers if you want the wider span, otherwise 2.048 is the safer choice and loses nothing here.

★ Note this is a DIFFERENT use of the second dongle from §8.1's reference channel: there, both
dongles watch the same spectrum at two gains; here they watch different spectrum to observe cause
and effect together. Both are worth running — §8.1 grades the detector, this configuration proves
what actually caused the damage. Confirm Radio Romania's frequency from the schedule on the night;
it moves seasonally.

★ The ideal capture is therefore across an RRI schedule boundary, with the 4.6 MHz victims in
band: the front end goes from hammered to clear with nothing else changing, at a fixed gain. If
the weak signals' SNR jumps when the broadcaster leaves the air, that is desensitisation measured
against a step change the transmitter made for us.

★ Caveat: FT8 on 80 m and 40 m both stress on the SAME UTC 15-second grid and timing alone cannot
attribute the damage to one or the other. Use the second dongle to watch each band and attribute
by observation, or capture at a time when only one of the two bands is open.

★ These are propagation-dependent — the Buzzer is an evening signal at Stuart's location. So
CAPTURE AND KEEP. A good night's recording becomes a permanent regression fixture; the band will
not reproduce it on demand.

### 8.4 Offline fixtures

For the parts that CAN be judged from a recording — does the floor estimator correctly identify a
capture already known to be overloaded, does a percentile beat a median on a packed band:

```bash
rtl_sdr -f <centre> -s 1024000 -g <gain> -n 20480000 cap.iq
./build/vibedsp_iq_probe cap.iq 1024000 <offsetHz>
```

`vibedsp_iq_probe` runs the real `RxPipeline` over a capture, so a gain policy can be judged
offline, repeatedly, against the same recorded signal — which is the only way to compare fairly
when the band itself changes minute to minute. Capture a set at several gains on several bands
(broadcast FM, airband, a crowded HF segment, and one with a strong local transmitter present)
and keep them as fixtures.

## 9. Acceptance criteria (draft)

1. On a band where external noise dominates, the algorithm **settles and stops climbing**, and
   reports that it is band-noise-limited.
2. With a strong local transmitter present, it backs off before the noise floor rises, and a weak
   adjacent signal remains decodable — the case the tuner's own AGC fails.
3. No audible pumping over a 10-minute session on a fading signal.
3a. ★ The 4582 kHz case (§2.1) is reproduced and fixed: with a strong out-of-band interferer
    present, the algorithm lands near 8 dB rather than 22 dB, and no intermodulation product
    appears in the tuned channel. If it cannot pass this, it has not solved the problem that was
    actually reported.
4. Never changes gain during a recording or mid-decoder-frame.
5. Measurably beats both "Auto (tuner)" and a fixed mid-table gain on the stored fixtures from
   §8, on at least: FM RDS group rate, and HF SSB intelligibility at a fixed volume.
6. Works unchanged on a tuner whose gain table differs from the R820T's, and on a device with
   more than one gain stage (e.g. an SDRplay's LNA state plus IF gain) — no algorithm change,
   only a different ordered list of front-end states from the device layer.
