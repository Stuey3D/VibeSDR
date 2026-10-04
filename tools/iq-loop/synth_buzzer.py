#!/usr/bin/env python3
"""synth_buzzer.py — a synthetic Buzzer capture to test make_loop.py before the real one exists.
Carrier at 4625.000 kHz, buzzes ~0.9-1.2 s every ~1.9-2.6 s (jittered, never periodic) as a comb above the
carrier, slow fading, white noise. Writes cf32 at --rate, centred on --centre."""
import argparse, numpy as np
ap = argparse.ArgumentParser(); ap.add_argument('--rate', type=int, default=192000)
ap.add_argument('--centre', type=float, default=4626000); ap.add_argument('--secs', type=float, default=90)
ap.add_argument('--out', required=True); a = ap.parse_args()
rng = np.random.default_rng(7); fs = a.rate; n = int(a.secs * fs); t = np.arange(n) / fs
car = 4625000 - a.centre
x = 0.05 * np.exp(2j * np.pi * car * t)
env = np.zeros(n); s = 0.5
while s < a.secs - 2:
    d = rng.uniform(0.9, 1.2); i0, i1 = int(s * fs), int((s + d) * fs)
    ramp = np.minimum(1, np.minimum(np.arange(i1 - i0), np.arange(i1 - i0)[::-1]) / (0.01 * fs))
    env[i0:i1] = ramp; s += rng.uniform(1.9, 2.6)
buzz = sum(np.exp(2j * np.pi * (car + k * 160) * t + 1j * k) for k in range(1, 13)) / 12
fade = 1 + 0.3 * np.sin(2 * np.pi * t / 37)
x = x * fade + 0.4 * env * fade * buzz + (rng.standard_normal(n) + 1j * rng.standard_normal(n)) * 0.004
inter = np.empty(2 * n, np.float32); inter[0::2] = x.real; inter[1::2] = x.imag; inter.tofile(a.out)
print('wrote', a.out, n, 'samples')
