#!/usr/bin/env python3
"""synth_ascii.py — an ASCII RTTY test signal (the full RTTY spec, 2026-10-04): any data bits 7/8, parity N/E/O/M/S,
stop 1/2, with idle mark between characters.  synth_ascii.py out.wav <framing e.g. 7E1> <baud> [shift=170] [cf=1500]"""
import sys, wave, numpy as np
out, fr, baud = sys.argv[1], sys.argv[2], float(sys.argv[3])
shift = float(sys.argv[4]) if len(sys.argv) > 4 else 170; cf = float(sys.argv[5]) if len(sys.argv) > 5 else 1500
nd, par, stop = int(fr[0]), fr[1], float(fr[2:]); FS = 48000
text = 'THE QUICK BROWN FOX 0123456789 de PBB test {ok}\r\n' * 6
bits = [(1, 3.0)]
for ch in text:
    v = ord(ch); d = [(v >> k) & 1 for k in range(nd)]; ones = sum(d)
    bits.append((0, 1))
    bits += [(b, 1) for b in d]
    if par != 'N': bits.append(({'E': ones & 1, 'O': 1 - (ones & 1), 'M': 1, 'S': 0}[par], 1))
    bits.append((1, stop + 0.7))                 # stop + a little idle mark
spb = FS / baud; n = int(sum(d for _, d in bits) * spb) + FS
mark = np.ones(n, bool); pos = 0.0
for v, dur in bits:
    a, b = int(pos), int(pos + dur * spb); mark[a:b] = v == 1; pos += dur * spb
f = np.where(mark, cf + shift / 2, cf - shift / 2)
y = np.cos(2 * np.pi * np.cumsum(f) / FS) + np.random.default_rng(3).standard_normal(n) * 0.05
with wave.open(out, 'wb') as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(FS); w.writeframes((y / np.max(np.abs(y)) * 0.8 * 32767).astype('<i2').tobytes())
open(out + '.txt', 'w').write(text)
