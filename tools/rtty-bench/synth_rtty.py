#!/usr/bin/env python3
"""synth_rtty.py  (env BAUD / CF / SHIFT override DWD's 50 / 1000 / 450)
 — a DWD-style RTTY test signal (50 baud, 450 Hz shift, 5N1.5) with noise and SELECTIVE FADING
(mark and space fade independently, as on HF). For checking rtty_wav before and after a decoder change.
  python3 synth_rtty.py out.wav [snr_db=10] [fade_depth_db=20] [seconds=120] [inverted=1] [seed=0]"""
import sys, wave, numpy as np
out = sys.argv[1]; snr = float(sys.argv[2]) if len(sys.argv) > 2 else 10; fd = float(sys.argv[3]) if len(sys.argv) > 3 else 20
secs = float(sys.argv[4]) if len(sys.argv) > 4 else 120; inv = int(sys.argv[5]) if len(sys.argv) > 5 else 1
seed = int(sys.argv[6]) if len(sys.argv) > 6 else 0
import os
FS, BAUD, CF, SH = 48000, float(os.environ.get('BAUD', 50.0)), float(os.environ.get('CF', 1000.0)), float(os.environ.get('SHIFT', 450.0))
L = {'E':1,'\n':2,'A':3,' ':4,'S':5,'I':6,'U':7,'\r':8,'D':9,'R':10,'J':11,'N':12,'F':13,'C':14,'K':15,'T':16,'Z':17,'L':18,'W':19,'H':20,'Y':21,'P':22,'Q':23,'O':24,'B':25,'G':26,'M':28,'X':29,'V':30}
F = {'3':1,'-':3,'8':6,'7':7,'4':10,',':12,'!':13,':':14,'(':15,'5':16,'"':17,')':18,'2':19,'6':21,'0':22,'1':23,'9':24,'?':25,'&':26,'.':28,'/':29,';':30}
LTRS, FIGS = 31, 27
text = ('RYRYRYRYRYRYRYRYRYRY\r\nCQ CQ CQ DE DDK2 DDH7 DDK9\r\nFREQUENCIES 4583 KHZ 7646 KHZ 10100.8 KHZ\r\n'
        'SYNOP 10384 41/96 82508 10118 20072 30115 40206 57012 8807/ 333 10173 20065 =\r\n') * 40
codes, fig = [LTRS, LTRS], False
for ch in text:
    if ch in F and not (ch in L):
        if not fig: codes.append(FIGS); fig = True
        codes.append(F[ch])
    elif ch in L:
        if fig and ch not in ' \r\n': codes.append(LTRS); fig = False
        codes.append(L[ch])
spb = FS / BAUD
bits = []
for c in codes:
    bits += [(0, 1.0)] + [((c >> i) & 1, 1.0) for i in range(5)] + [(1, 1.5)]
n_total = int(secs * FS)
tone = np.zeros(n_total); mark_on = np.zeros(n_total, bool)
pos = 0.0
for v, dur in bits:
    a, b = int(pos), int(pos + dur * spb)
    if a >= n_total: break
    mark_on[a:min(b, n_total)] = v == 1; pos += dur * spb
t = np.arange(n_total) / FS
fm = CF + (SH / 2 if not inv else -SH / 2); fs_ = CF - (SH / 2 if not inv else -SH / 2)
f_inst = np.where(mark_on, fm, fs_)
phase = 2 * np.pi * np.cumsum(f_inst) / FS
rng = np.random.default_rng(1 + 1000 * seed)
def fade(seed):   # slow independent fading, 0 dB to -fd dB, ~0.3 Hz
    r = np.random.default_rng(seed); k = r.standard_normal(int(secs * 2) + 4)
    x = np.interp(t, np.arange(len(k)) / 2, k); x = (x - x.min()) / (x.max() - x.min())
    return 10 ** (-fd * x / 20)
gm, gs = fade(11 + 1000 * seed), fade(23 + 1000 * seed)
sig = np.where(mark_on, gm, gs) * np.cos(phase)
noise = rng.standard_normal(n_total) * np.sqrt(0.5) * 10 ** (-snr / 20) * np.sqrt(FS / 3000)   # SNR in 3 kHz
y = sig + noise; y = y / np.max(np.abs(y)) * 0.8
with wave.open(out, 'wb') as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(FS); w.writeframes((y * 32767).astype('<i2').tobytes())
open(out + '.txt', 'w').write(text)
print('wrote', out, f'{secs}s snr {snr} dB fade {fd} dB')
