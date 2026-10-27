# VFO A/B — two frequencies at once (V12)

GitHub discussion #57 (Gesiima): *"listen to two or more frequencies at the same time … monitoring a calling frequency
and a second channel without having to switch back and forth."* Stuart, 2026-10-27, chose the first step: dual
frequencies per radio, like VFO A/B on a rig. Two dongles at once is a later step and is not built.

## What the listener gets

- **Frequency card (tap the readout) → VFO A/B** (on/off; in the tuning-step sheet until V12 build 498). On creates VFO B at VFO A's frequency, mode and width; off removes it. Per
  session: a reconnect starts with one VFO.
- **Switching which VFO the controls tune**: the A/B key between the tune buttons (buttons mode); press-and-hold the
  radio trapezoid on the drum (drum mode); a pad/keyboard shortcut. Everything — frequency, mode, passband, squelch,
  the readout — follows the selected VFO. The other is drawn as a second needle and passband, slightly dimmer.
- **Both are heard at once, each with its own squelch.** With **Stereo** on (the default, in the Audio menu beside
  Binaural) the **lower** frequency is in the **left** ear and the **higher** in the right — whichever is A or B, so the
  waterfall and the ears always agree. Stereo off, or a mono output: both VFOs in both ears.
- Every move is smooth: adding B pans A from the middle to its ear while B fades in; removing B fades it out and pans
  A home; tuning one across the other swaps the ears. 400 ms, raised-cosine, equal-power (no loudness dip, no click),
  100 Hz of hysteresis so a needle wobbling over the other does not flutter, within 5 Hz = both in both ears.
- Both VFOs must lie **inside the radio's current window**. A frequency outside it is refused with the window in the
  note — never honoured by moving the radio. If the radio moves (VFO A's own tune on a personal receiver, the shared
  dial, the owner) and leaves B outside, B is parked silent with a note until it is back inside.

## Owner

- **Setup page → "Let listeners use VFO A/B"** (config `vfoB`, per radio: 1 default, 0 off). Android: the same key in
  the server config (VibeServerBoot → `VibeLocalSDR.setVfoB`). Live: `{"type":"vfoB","on":0|1}` (admin).
- Off, or a radio/mode where it cannot run (an Icom — one physical receiver; a SpyServer source; DAB, ADS-B, ISM, AIS),
  hwinfo says `"watch":0` and no client offers the toggle. Older servers do not send `watch` at all: same thing.
- **Slots**: on a radio with a listener limit a VFO B takes a listener's place (it costs about one listener). A VFO B
  asked for on a full radio is refused; a *listener* arriving at a full radio closes the newest VFO B, with a note — a
  person always beats a second channel.

## Architecture (as built)

| piece | where |
|---|---|
| watch channel (own RxPipeline, AGC, squelch gate with open/closed + when, FIFO) | `vibe_watch.h` `WatchChannel`, `SquelchGate`, `PcmFifo` |
| the pan mixer (N sources ranked by frequency, ramps, equal-power, hysteresis, stereo/mono) | `vibe_watch.h` `PanMixer` |
| one listener's set (session-keyed: chans, fading, stereo, monoOut, mixer, private Opus) | shim `Impl::WatchSet` |
| range-locked radio: one more slice of the shared forward FFT, on the listener's own thread | shim `feedWatchFromBins` (from `feedOneClient`) |
| one-pipeline radio (personal receiver, shared dial): full-rate tune + decimate on its own AUDIO-priority thread; the DSP thread only copies | `vibe_watch.h` `FullRateFeeder`, shim `watchFeedFull` |
| mix into the listener's own stream (no second socket, no second Opus stream to decode) | shim `watchMix`; `onClientAudio` (range-locked); `watchDivertShared` → `watchSendPrivate` (one pipeline: the session leaves the shared encode while mixing and gets its own) |
| levels, squelch, window — off the same averaged wide row as the listener's own squelch and S meter | shim `watchOnFrame` (from `onSpectrum`) |
| control, refusal, slot accounting, housekeeping | shim `handleWatch`, `watchHousekeep` |

**Hand-back with no step.** The mixer's equal-power law is normalised so the centre is 1.0 in each ear — exactly what a
listener with one VFO hears today — and a hard-panned source is √2 (+3 dB) in one ear (the AGC's 0.25 setpoint leaves
9 dB of headroom). When B has faded and A is home, the mixer would output today's mono, and only then does the
ordinary path take over.

## Wire

Client → server (spectrum socket):

```
{"type":"watch","id":1,"on":true}                         create at VFO A's frequency/mode/width
{"type":"watch","id":1,"frequency":Hz,"mode":"usb","bandwidthLow":-..,"bandwidthHigh":..}   re-aim
{"type":"watch","id":1,"sql":-80,"sqlOn":true}            B's own squelch
{"type":"watch","stereo":false}                           the Stereo switch (any time, ramps)
{"type":"watch","monoOut":true}                           the client's output is mono
{"type":"watch","id":1,"off":true}                        remove (fades)
```

Server → client: `{"type":"watch","max":1,"lo":Hz,"hi":Hz,"stereo":true,"monoOut":false,"chans":[{"id":1,"freq":..,
"mode":"am","bw":..,"sqlOn":..,"sql":..,"open":true,"since":<wall ms>,"lvl":dB,"parked":false}]}` on every change and
twice a second; plus `"refused":"outside|full|off|mode|id|session","note":"…"` when it says no. hwinfo: `"watch":N`.

Old clients never send `watch` and are untouched; a new client on an old server sees no `watch` in hwinfo and offers
nothing.

## Hooks for the scanner (not built — Stuart: "this all fits into the scanner work too")

- **The channel count is a parameter**, `vibewatch::kMaxPerListener` (1 today). The DSP, the mixer (N sources ranked by
  frequency and spread evenly between the ears) and the wire (`chans[]`, `id`) already take N; only the clients' A/B
  UI assumes one.
- **Priority watch**: each channel's `SquelchGate` carries `open` and `changedAt`, evaluated with hysteresis (2 dB) and
  a 250 ms tail so it does not chatter — the event a priority watch acts on, already on the wire per channel.
  Switching the audio *to* a channel when it opens is a mixer policy (gain per source), not new DSP.
- **Channel stepping**: re-aiming a channel is `WatchChannel::want()` — a frequency change moves the oscillator inside
  the same pipeline (no rebuild unless the width needs a wider slice), proven by test-watch §1 ("re-aimed, it hears the
  other carrier").
- **Sampling without listening**: a channel's level and squelch are measured off the shared wide row every spectrum
  frame whether or not its audio is mixed — a scanner can watch levels on channels that are not producing audio.

## Cost (measured on this Mac, 2.4 MS/s; `test-watch-cost`)

| mode | one-pipeline: A | +B | B/A | range-locked: A | +B | B/A |
|---|---|---|---|---|---|---|
| AM  | 0.35 % | 0.35 % | 1.00× | 0.10 % | 0.10 % | 1.01× |
| USB | 0.32 % | 0.33 % | 1.05× | 0.14 % | 0.16 % | 1.08× |
| NFM | 0.39 % | 0.38 % | 0.99× | 0.11 % | 0.11 % | 1.02× |
| WFM | 1.67 % | 1.66 % | 1.00× | 1.57 % | 1.56 % | 1.00× |

The mixer: 0.03 % of a core. The whole vibeserver process (fake rtl_tcp at 2.4 MS/s, one listener) read 2.4–3.6 % of a
core with and without B — the difference is inside the noise of the measurement. RAM: one more RxPipeline (a few
hundred kB) and a FIFO of at most 200 ms per VFO B; on a one-pipeline radio, up to 0.5 s of queued IQ (≈ 9.6 MB at
2.4 MS/s, cf32) while its thread is behind — normally a block or two.

**Pi 2 (estimate, not measured):** a 900 MHz Cortex-A7 is roughly 15–20× slower than this Mac per core for this work,
so a VFO B on AM/SSB/NFM costs about 5–8 % of one Pi 2 core on a personal receiver and 2–3 % on a range-locked one;
on WFM about 25–33 %. On the Pi 2 a WFM VFO B on top of WFM VFO A is the case to avoid; the narrow modes (the calling-
channel use Gesiima describes) are cheap. Nothing at all runs for a listener with VFO B off — the per-block gate is one
relaxed atomic read.

## Known limits

- VFO A on stereo WFM is folded to mono while B is on (the ears are B's and A's now); it returns to stereo when B is
  removed.
- On a one-pipeline radio, the session's stream switches from the shared Opus encode to its own when B is added, and
  back when the pan has come home. The content at both moments is the same (A centre), so the change of encoder is
  the only discontinuity.
- The shared path trims AM by 6 dB and a listener's own channel does not (an older inconsistency); B takes whichever
  trim A has, so the two always match.
