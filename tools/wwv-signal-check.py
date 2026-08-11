#!/usr/bin/env python3
"""Is WWV actually IN this audio? Run before blaming the decoder.

    /tmp/wwv-decode capture.bin WWV /tmp/wwv.s16     # writes mono s16
    python3 tools/wwv-signal-check.py /tmp/wwv.s16 [--rate 48000]

★★★ "NO TIMESTAMP" IS THE SYMPTOM OF BOTH A FRAMING BUG AND A DEAD BAND, and they want opposite
    work. The first capture I took looked plausible — 200 s, every packet decoding — and contained
    no WWV whatsoever: the loudest things in it were 120/180/240 Hz, which is US mains hum, not a
    time station. An hour spent on the framing code would have been an hour spent on the wrong half.

WWV's audio fingerprint, in the order this checks for it:
  • 100 Hz subcarrier — the TIMECODE itself rides this, so no 100 Hz means no timecode however
    strong the rest of the station is.
  • 1000 Hz second ticks (WWVH uses 1200 Hz) — 5 ms bursts, once a second, absent on second 29
    and 59. Their PERIODICITY is the giveaway, not their level.
  • 500 / 600 Hz standard audio tones, alternating minute by minute.
"""
import argparse, sys
import numpy as np


def tone_db(x, sr, hz, width=4.0):
    """Level of a narrow band around hz, in dB relative to the whole-signal RMS."""
    n = 1 << 18
    seg = x[: n] if len(x) >= n else np.pad(x, (0, n - len(x)))
    S = np.abs(np.fft.rfft(seg * np.hanning(n)))
    f = np.fft.rfftfreq(n, 1 / sr)
    band = (f > hz - width) & (f < hz + width)
    ref = np.median(S) + 1e-9
    return 20 * np.log10((S[band].max() + 1e-9) / ref)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--rate", type=int, default=48000)
    a = ap.parse_args()
    x = np.fromfile(a.path, dtype=np.int16).astype(np.float32) / 32768
    sr = a.rate
    if len(x) < sr:
        print("too short", file=sys.stderr)
        return 2
    print(f"{len(x)/sr:.1f} s, rms {x.std():.4f}")

    for hz, what in [(100, "timecode subcarrier"), (1000, "WWV second tick"),
                     (1200, "WWVH second tick"), (500, "500 Hz tone"), (600, "600 Hz tone")]:
        print(f"  {hz:5d} Hz  {tone_db(x, sr, hz):6.1f} dB   {what}")

    # ★★ THE TICKS ARE PERIODIC, AND THAT IS WHAT IDENTIFIES THEM. A 1000 Hz carrier from anywhere
    #    is a tone; a 1000 Hz burst arriving every 1.000 s is a time station. Envelope-detect the
    #    tick band and look for the 1 Hz line in ITS spectrum.
    b = np.fft.rfft(x[: len(x) // sr * sr].reshape(-1, sr), axis=1)
    f = np.fft.rfftfreq(sr, 1 / sr)
    tick = np.abs(b[:, (f > 950) & (f < 1050)]).sum(axis=1)
    if len(tick) > 8:
        t = tick - tick.mean()
        print(f"  tick-band energy per second: mean {tick.mean():.1f}, "
              f"std/mean {t.std()/(tick.mean()+1e-9):.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
