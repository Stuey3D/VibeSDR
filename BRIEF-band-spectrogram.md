# BRIEF — the band spectrogram (idle snapshot + sweep)

**Status:** designed 2026-08-01 with Stuart, not started.
**Origin:** a Discord conversation with Saber/Orchid about Android power saving. It began as a
keep-alive trick and turned into a feature in its own right — Saber, on seeing the mock-up:
*"the one feature ive been lookin for"*.

★★ **IT IS NOT A POWER-SAVING MODE.** Selling it as one was wrong (see "The anti-kill myth"
below). It is a 24-hour band-activity record that happens to cost little power.

---

## What it is
A long-duration spectrogram: **one row per minute, 1440 rows = 24 hours**, built while the
receiver is otherwise idle. Reference shape is the UberSDR wideband spectrogram Stuart showed —
0–30 MHz, 4096 bins, 7.3 kHz/bin, newest row at the bottom.

## Two modes, one control
Stuart's framing: **the span decides the mode.** There is no separate toggle.

1. **Fixed profile** — span fits in one capture. Radio wakes, tunes the profile, takes one row,
   sleeps. Stuart's own case: *2.4 MHz around the 40m band.*
2. **Sweep** — span is wider than the radio can capture at once, so the row is stitched from
   several segments. Not HF-only: **FM broadcast and airband are explicitly wanted.**

★★★ **THE PROFILE IS PART OF THE IMAGE'S IDENTITY** — centre, span, gain and bin count. Every row
must share them. Stuart: *"no point snapshotting the last used frequency or you wont end up with a
clear spectrogram"* — inherit the live dial and a day's image becomes 40m for an hour, then FM,
then wherever someone left it at 2am. Not a spectrogram, just unrelated slices.
- **A profile change must START A NEW IMAGE**, never extend the old one. Appending 40m rows to an
  FM image corrupts a day's data silently, and nobody notices until they go looking.
- The profile lives in the SERVER'S SETTINGS, **independent of the live dial**, or it drifts every
  time someone tunes around.

## Gain
- ★★ **FIXED GAIN IS THE DEFAULT, and it is what makes a sweep usable.** With AGC on, each segment
  makes its own gain decision and the boundaries show up as vertical banding — a striped image
  instead of a continuous one. Offer AGC as an option; do not default to it.
- ★★ **THE GAIN FIGURE IS RADIO-SPECIFIC** and a saved profile is therefore NOT portable. RTL has
  a gain table (Stuart's example: 29.6 dB); the **Airspy HF+ has no variable gain at all**
  (attenuator + preamp); the RSP uses IF gain REDUCTION. Store per-driver or refuse a profile
  captured on other hardware. Same branching `LocalHardwarePanel` already does — and the same rule
  as AGENTS.md's "a control that only works on one radio should not be there".

## Band coverage is PER RADIO — and per DRIVER
★ I initially wrote "an RTL-SDR cannot reach HF". **Wrong, and Stuart caught it.**

| Radio | HF sweep |
|---|---|
| **RTL-SDR Blog V4** | ✅ native — built-in upconverter, tune directly |
| **RTL-SDR V3** | via direct sampling only |
| **Airspy HF+** | ✅ native |
| **SDRplay RSP** | ✅ native |

★★ **The bundled `librtlsdr` IS the rtl-sdr-blog fork** — verified by `strings`:
`is_rtlsdr_blog_v4`, `RTL-SDR Blog V4 Detected`, `upconvert_freq`. Stock librtlsdr would not do
this. So 0–30 MHz is available on THREE of the four radios, not one.
★ Ask the device, never a table keyed on "is it a dongle" — [[one_radio_assumption_family]].

## Sizing (all comfortable)
- **Sweep time**: 2.4 MHz bites → HF ≈ 13 segments, FM broadcast (87.5–108) ≈ 9, airband
  (118–137) ≈ 8. At ~100–200 ms per segment (retune + PLL settle + FFT samples) a full HF sweep is
  **2–3 seconds**. On a 60 s cadence that is ~4% duty — nearly full-sleep power, with a picture.
- ★★ **FIX THE ROW WIDTH (e.g. 4096 bins) REGARDLESS OF SPAN.** A row is then ~4 KB: **<6 MB/day,
  ~170 MB/month**. Keep native resolution instead and a wide sweep balloons to tens of MB/day and
  the retention question turns ugly.
- **One sample rate for the whole sweep**, or bins are uneven across the stitched row.

## Three rules to build in from the start
1. ★★★ **THE LISTENER ALWAYS WINS.** Someone connecting mid-sweep gets the radio immediately —
   abort the pass. A logger must never degrade the thing the server is for.
2. ★★★ **RESTORE THE USER'S FREQUENCY AFTERWARDS.** The snapshot deliberately moves the dial to
   reach the profile, so putting it back is CORRECTNESS, not tidiness — otherwise every idle
   period silently retunes the server to the logging frequency. Same class as the Airspy
   rate-change bug fixed 2026-08-01 (changed state, never restored).
3. **Never render absence as data** — see below.

## ★★★ Gaps are LABELLED, never blank
Stuart: *"we have gaps labelled as in use too so a spectrogram is never empty, there is a reason
for the gaps"*. A black band is a factual claim — "nothing was transmitting" — and if the truth is
"we were not listening", the image lies. Same shape as the audio bug where the server reported a
healthy radio while sending nothing.

- **Solid full-width row in a reserved colour**, not hatching (Stuart). At 1 row/min a 5-minute
  session is a 5 px stripe — visible, honest, not disruptive.
- ★★ **The colour must be OUTSIDE the waterfall palette's range** or it reads as signal. The
  palettes run phosphor green → yellow/white, so a desaturated violet/magenta is safe. **Avoid
  amber** — it already means squelch/warning elsewhere in the UI.
- **Store the reason PER ROW**: listener connected · radio in use by another application (Saber's
  OWRX case, see [[sharing_one_sdr_with_another_app]]) · device asleep / no power · no radio
  attached · profile changed.
- **Legend shows only reasons actually present** in the visible window — listing statuses that
  never happened sends people hunting for stripes that are not there.
- **Hover/tap must report the reason** ("14:32–14:38 — SDR in use"). A 1 px row cannot hold text,
  and colour alone is unreadable for colourblind users.
- ★ Free side-effect: for a public receiver the in-use stripes ARE the "when is my server actually
  used" chart.
- ★ Caution: on a BUSY receiver most of the day becomes stripes. The feature shines on the
  mostly-idle remote box — which is the solar-Pi case it is aimed at.

## Later optimisation (NOT v1)
When a listener is connected the FFT is already running for their waterfall. If their capture
window **fully contains the profile band at equal or better resolution**, harvest that row for
free and have no gap at all. Only when it genuinely matches, or you are back to incoherent rows.

## ★★★ DEPENDENCY — the stop/start fix gates this
The radio must actually be released when idle. Today [[idle_park_never_stops_radio]] means we drop
samples but never close the device, and stop/start is where
[[vibeserver_idle_resume_libusb_crash]] lives — mitigated, not closed. This feature would run that
transition **~1,440 times a day, unattended, on a phone in a loft**. At a 1-in-1000 failure rate
that is a daily crash.

★ Fix that FIRST. It gates four things: the resume crash, real "sleep" power saving, this, and
sharing one radio with another app.
★ Design note: **reuse ONE radio session for all segments of a sweep.** Retuning is cheap and
safe; open/close is the dangerous operation.

## The anti-kill myth — do not repeat it
The original pitch was that snapshotting keeps Android from terminating the app. **It does not.**
VibeServer already runs as a **foreground service** (`foregroundServiceType="connectedDevice"`)
with a persistent notification, a PARTIAL_WAKE_LOCK and a WifiLock. Android does not kill
foreground services for being idle and has no visibility into whether the USB radio is
transferring. Keeping the radio spinning costs ~80% of the battery (10.9%/hr measured on the Moto)
and buys nothing in survival terms.

★★ What actually kills them: **OEM battery managers** (Xiaomi/Samsung/Huawei/OnePlus), and the
missing **`REQUEST_IGNORE_BATTERY_OPTIMIZATIONS`** — which appears **ZERO times** in the manifest
and zero times in the Kotlin. We have never asked for it. That is the real anti-kill lever and it
is a separate, small job.
