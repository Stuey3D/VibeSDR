# RTTY skimmer — spec

Stuart, 2026-11-14 ("this morning there was loads of RTTY on 40m"): a skimmer for RTTY like the CW and PSK skimmers —
**CPU-light and user friendly**. Agreed in conversation the same evening; not built yet.

## 1. What the listener sees

Same as the CW skimmer: one line per signal, the text scrolling along it; **selecting a line opens its card** with the
full messages.

```
7002 kHz: 45 baud / 170 Hz / Reverse | CQ CQ CQ TEST M1ABC
7005 kHz: RTTY detected — waiting for parameter detection
```

- A signal is listed **as soon as it is detected**, with "RTTY detected — waiting for parameter detection", before any
  text. Its text appears once its settings are found — from when it was first heard (see §3).
- The settings shown are the ones found: baud (45, 50, 75), shift (170, 200, 425, 450, 850 Hz), Normal / Reverse.
- A callsign heard in CQ / DE / TEST is picked out, as the CW and PSK skimmers do (shared callsign rules).
- Opened from the RTTY decoder's key — one signal, or a skimmer across 24–96 kHz (as CW). Every decoder has a toggle;
  its options sit under it.

## 2. Detect, then list (continuous, cheap)

ONE forward FFT over the window (PFFFT, as `decoders/psk_skimmer.h`) is both the finder and the channeliser.
An RTTY carrier is **two tones a standard shift apart that take turns** — one up while the other is down, their sum
steady. That test separates RTTY from two unrelated carriers 170 Hz apart (CW, PSK) and from noise. The pair's spacing
IS the shift (snapped to a standard one, as `rtty_auto` does) and its midpoint the centre — free from the shared FFT.
Each detected signal is listed at once and its narrow channel (cut from the same FFT, a few hundred samples a second)
starts being STORED from that moment.

## 3. Then set each signal's parameters (a queue, run by workers)

- Detected signals queue, **strongest first**. A worker takes one and runs, over its STORED channel — much faster than
  real time:
  1. the fast path: 45.45 baud at the measured shift, Normal and Reverse;
  2. only if that does not frame cleanly, the full contest (50 / 75 baud, 1 or 1.5 stop bits) — `rtty_auto`'s scoring
     (frames passing the start/stop check against frames failing it), shared with the one-signal decoder so both
     improve together.
- The winner is set for that signal and its text is decoded from the stored audio onward. Nothing frames → "not RTTY",
  dropped, not retried for a while.
- **Workers by device** (Stuart: "slower devices one at a time, more powerful devices more workers"): Pi 2 / R36S /
  old phones 1, Pi 4/5 / phones 2, PCs / Macs 4 — the server benchmark sets the default, the owner can change it
  (BENCHMARK SETS THE DEFAULT, NEVER HIDES). Decoder-priority threads (Network > Audio > Spectrum > Decoders).
- Settled signals keep only their one decoder. A new pair joins the queue; a settled signal whose errors climb or whose
  tones move is queued again.
- **Every signal has its own settings** — mixed baud / shift / polarity in one window is the normal case.

## 4. Cost (measured 2026-11-14, M5 Pro core, NEON)

| Part | Cost |
|---|---|
| Shared FFT, 24 kHz window (PSK skimmer engine, 0–30 signals) | 0.07–0.10 % of a core |
| Shared FFT, 96 kHz window (0–60 signals, 64 channels) | 0.22–0.25 % |
| One signal SEARCHING (`RttyAuto`, 12 lanes) | 0.11 % — only while queued work runs |
| One signal SETTLED (one `FskDecoder`, 4 kHz) | 0.009 % — lower again at the channel's rate |
| Busy 96 kHz window, 30 settled signals | ≈ 0.3–0.5 % |

Estimates (to MEASURE on the devices before shipping): Pi 2 ≈ 10–25 % of one core for a busy 96 kHz window, R36S under
10 %. Guards as CW/PSK: a channel cap, runs only when opened, "RTTY skimmer: TOO HEAVY" on the health pill if it drops
input, a lighter default width on slow boxes.

## 5. Proof before shipping

A real 40 m RTTY IQ capture (Stuart's UberSDR, standing OK) of the RTTY area: detection count, settings found, text
gate (no text on noise / non-RTTY data modes), and the CPU on the Pi 2 and the R36S.
