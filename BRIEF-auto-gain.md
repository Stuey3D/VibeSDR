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

**The test:** step the gain up and compare how the *median* bin power moves against the
*strongest* bin.
- Median rises **more slowly** than the peak → we are still recovering signal from below the
  receiver's own noise. Keep going.
- Median rises **as fast as** the peak → external noise now dominates. **Stop.** This is state (1)
  from §2 and there is nothing further to win.
- Median rises **faster** than the peak → the receiver is manufacturing noise and intermod.
  **Back off**, immediately and by more than one step.

★ Watch it over time as well as against gain. Overload driven by a *fading* interferer shows up
as the median lifting and dropping across the whole band while the wanted signal sits still —
the "stripes across the entire spectrum" of §2.1. A gain that is correct at one moment can be
wrong a minute later, so a broadband floor excursion is itself a reason to back off, without
waiting to try a gain step.

That single comparison is the heart of the feature, it needs no knowledge of the mode, and it
would have correctly refused to keep climbing on the night this was raised.

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

The harness already exists and was used for §2:

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
