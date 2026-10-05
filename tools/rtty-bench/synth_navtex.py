#!/usr/bin/env python3
"""synth_navtex.py — a NAVTEX (SITOR-B FEC, CCIR 476) test signal with noise, IMPULSE BURSTS and selective fading.
For checking rtty_wav's CCIR476 path before and after a decoder change (scored with score.py, like synth_rtty.py).

  python3 synth_navtex.py out.wav [snr_db=15] [fade_depth_db=0] [seconds=120] [bursts_per_min=0] [burst_db=10] [seed=0]

 * 100 baud, 170 Hz shift centred on 500 Hz (env CF / SHIFT / BAUD override), mark = the higher tone (the decoder's
   default, not inverted).
 * SITOR-B FEC as transmitted (ITU-R M.476/M.625, and fldigi's create_fec): every character is sent twice, the DX copy
   first and the RX copy five character slots (35 bits, 350 ms) later, DX and RX slots interleaved. Phasing at the start
   and between messages = DX slots carry 0x66 (rep), RX slots 0x0F (alpha).
 * Codes are 7-bit 4-of-7, least significant bit first; the table is the decoder's own (decoders/fsk_decoder.cpp,
   Ccir476 — identical to fldigi's code_to_ltrs / code_to_figs).
 * Bursts: Poisson arrivals, each 20-200 ms of white noise burst_db above the SIGNAL (lightning crashes, ignition).
 * Fading: mark and space fade independently, 0 to -fade_depth dB, ~0.3 Hz (as synth_rtty.py).
 * ★ (2026-10-05) env JOIN=<s>: the receiver joins mid-transmission — the first <s> seconds are cut from the audio
   and the reference holds only the characters whose RX copy starts after the cut (no phasing to lock on).
   env INV=1: the tones swapped (space the higher) — a station or receiver in the other sideband.
   Mistune: synthesise with CF=<Hz> and decode at the nominal centre.
 * The message carries a FIGS BEL (0x4B) — the reference text holds an apostrophe there, which is what the decoder
   must print (fldigi's filter_print); a raw 0x07 in the output is a failure.
Writes out.wav (48 kHz mono int16) and out.wav.txt (the text a perfect decoder prints, '\\r' removed by score.py)."""
import sys, os, wave, numpy as np
a = sys.argv
out = a[1]; snr = float(a[2]) if len(a) > 2 else 15; fd = float(a[3]) if len(a) > 3 else 0
secs = float(a[4]) if len(a) > 4 else 120; bpm = float(a[5]) if len(a) > 5 else 0
bdb = float(a[6]) if len(a) > 6 else 10; seed = int(a[7]) if len(a) > 7 else 0
FS, BAUD = 48000, float(os.environ.get('BAUD', 100.0))
CF, SH = float(os.environ.get('CF', 500.0)), float(os.environ.get('SHIFT', 170.0))
JOIN, INV = float(os.environ.get('JOIN', 0)), os.environ.get('INV', '0') == '1'

# code -> char, from fsk_decoder.cpp's Ccir476 tables (rows of 16)
LT = ('________________' '_______J___F_CK_' '_______W___Y_PQ_' '_____G___MX_V___'
      '_______A___S_IU_' '___D_RE__N__ ___' '___Z_L___H__\n___' '_OB_T___\r_______')
FG = ('________________' "_______'___!_:(_" '_______2___6_01_' '_____&___./_;___'
      '_______-___\x07_87_' '___$_43__,__ ___' '___"_)___#__\n___' '_9?_5___\r_______')
assert len(LT) == 128 and len(FG) == 128
L = {c: i for i, c in enumerate(LT) if c != '_'}
F = {c: i for i, c in enumerate(FG) if c != '_'}
for c in L: assert bin(L[c]).count('1') == 4, c
for c in F: assert bin(F[c]).count('1') == 4, repr(c)
LTRS, FIGS, ALPHA, REP, CHAR32 = 0x5A, 0x36, 0x0F, 0x66, 0x6A
BEL = '\x07'

import random
WORDS = ('WIND SEA SWELL GALE STORM VIS POOR GOOD RAIN FOG SHOWERS BUOY LIGHT UNLIT WRECK DRIFTING CONTAINER '
         'DOVER THAMES HUMBER WIGHT PORTLAND PLYMOUTH BISCAY CANCEL WARNING NAVAREA TOWING VESSEL KEEP CLEAR '
         'SUBMARINE EXERCISES MOORED RIG ESTABLISHED REPORTED MISSING SEARCH AND RESCUE').split()
def message(k):
    """Message k: its own text (a fixed generator, independent of the noise seed), so the scorer's alignment has
    anchors — two messages repeated over and over let difflib match a decode against the wrong copy."""
    r = random.Random(1000 + k)
    def pos(): return (f'{r.randint(48, 61)}-{r.randint(0, 59):02d}.{r.randint(0, 9)}N '
                       f'{r.randint(0, 9):03d}-{r.randint(0, 59):02d}.{r.randint(0, 9)}{r.choice("EW")}')
    def words(n): return ' '.join(r.choice(WORDS) for _ in range(n))
    lines = [f'ZCZC {"ABEG"[k % 4]}{"ABCDLE"[r.randint(0, 5)]}{r.randint(0, 99):02d}',
             f'{r.choice(["GB", "NETHERLANDS", "OOSTENDE"])} NAV WARNING {r.randint(100, 399)}/26 {r.randint(1, 31):02d}{r.randint(0, 23):02d}{r.randint(0, 59):02d} UTC',
             f'{words(4)} {pos()}.',
             f'{words(3)}, {r.randint(2, 12)} TO {r.randint(3, 12)} ({words(2)}): {r.randint(1, 9)}.{r.randint(0, 9)}M?',
             f'{words(5)} & {r.randint(100, 399)}/26 {pos()}',
             (BEL if k % 2 == 0 else '') + 'NNNN']
    return '\r\n'.join(lines) + '\r\n'

def encode(text):
    """-> [(code, the text it prints or '')] — shift codes print nothing."""
    codes, fig = [], False
    for ch in text:
        pr = "'" if ch == BEL else ch
        if ch in ' \r\n':
            codes.append((L[ch], pr)); continue                # the same code in both shifts
        if ch in L and not fig: codes.append((L[ch], pr)); continue
        if ch in F and fig: codes.append((F[ch], pr)); continue
        if ch in F: codes += [(FIGS, ''), (F[ch], pr)]; fig = True; continue
        codes += [(LTRS, ''), (L[ch], pr)]; fig = False
    return codes

def fec(codes, phasing_pairs):
    """Interleave DX/RX: DX of char i in slot 2i, its RX in slot 2i + 5 (fldigi create_fec, offset 2).
       -> slots, and for each code the slot of its RX (later) copy."""
    s = [REP, ALPHA] * phasing_pairs
    rx = [0] * len(codes)
    for i, c in enumerate(codes):
        s.append(c)
        if i >= 2: rx[i - 2] = len(s)
        s.append(codes[i - 2] if i >= 2 else ALPHA)
    s += [CHAR32, codes[-2]]; rx[-2] = len(s) - 1
    s += [CHAR32, codes[-1]]; rx[-1] = len(s) - 1
    return s, rx

slots, ref = [], ''
n_slots = int(secs * BAUD / 7)
k = 0
while len(slots) < n_slots:
    m = message(k); k += 1
    # LTRS first: the receiver's shift is unknown at the start of every message
    enc = [(LTRS, '')] + encode(m)
    s, rx = fec([c for c, _ in enc], 40 if not slots else 15)   # 5.6 s of phasing first, 2.1 s between
    # the reference holds only what was SENT in full: a character whose RX copy is past the end is not counted
    j0 = int(JOIN * BAUD / 7) + 1     # the first whole slot after the join
    ref += ''.join(pr for (_, pr), r in zip(enc, rx) if j0 <= len(slots) + r < n_slots - 1)
    slots += s
bits = []
for c in slots[:n_slots]:
    bits += [(c >> i) & 1 for i in range(7)]
spb = FS / BAUD
n_total = int(secs * FS)
idx = np.minimum((np.arange(n_total) / spb).astype(int), len(bits) - 1)
mark_on = np.array(bits, bool)[idx]
t = np.arange(n_total) / FS
f_inst = np.where(mark_on != INV, CF + SH / 2, CF - SH / 2)
phase = 2 * np.pi * np.cumsum(f_inst) / FS
rng = np.random.default_rng(7 + 1000 * seed)
def fade(sd):
    if fd <= 0: return np.ones(n_total)
    r = np.random.default_rng(sd); kk = r.standard_normal(int(secs * 2) + 4)
    x = np.interp(t, np.arange(len(kk)) / 2, kk); x = (x - x.min()) / (x.max() - x.min())
    return 10 ** (-fd * x / 20)
gm, gs = fade(31 + 1000 * seed), fade(43 + 1000 * seed)
sig = np.where(mark_on, gm, gs) * np.cos(phase)
noise = rng.standard_normal(n_total) * np.sqrt(0.5) * 10 ** (-snr / 20) * np.sqrt(FS / 3000)   # SNR in 3 kHz
nb, tb = 0, 0.0
if bpm > 0:
    tt = rng.exponential(60.0 / bpm)
    while tt < secs:
        d = rng.uniform(0.020, 0.200); a0, a1 = int(tt * FS), min(n_total, int((tt + d) * FS))
        # white noise burst_db above the signal, measured in 3 kHz like the SNR (so 0 dB = a burst as loud as the signal)
        noise[a0:a1] += rng.standard_normal(a1 - a0) * np.sqrt(0.5) * 10 ** (bdb / 20) * np.sqrt(FS / 3000)
        nb += 1; tb += d; tt += d + rng.exponential(60.0 / bpm)
# ★ A FIXED signal level (-12 dBFS peak), clipped — NOT normalised to the loudest sample: a normalised file puts the
#   signal wherever the biggest burst leaves it, and at +10 dB bursts that sank it towards the decoder's audio floor
#   (a NoSignal reset per fade), which measures the file, not the decoder. Radio audio clips a crash the same way.
y = np.clip((sig + noise) * 0.25, -1.0, 1.0)[int(JOIN * FS):]
with wave.open(out, 'wb') as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(FS); w.writeframes((y * 32767).astype('<i2').tobytes())
# the reference: every message begun (score.py trims it to the length decoded)
open(out + '.txt', 'w').write(ref)
print('wrote', out, f'{secs}s snr {snr} dB fade {fd} dB bursts {nb} ({tb:.1f} s, {bdb} dB over signal)')
