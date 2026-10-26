#!/usr/bin/env python3
"""impair-capture.py in.vbu8 out.vbu8 snr_db doppler_hz secs [seed] — AWGN over the 1.536 MHz block plus Rayleigh flutter
(16-sinusoid sum, unit mean power) on a REAL captured multiplex, streamed. How the 12B "weak signal" logs for test-dab-faad
were made (snr 8 / fd 0, snr 30 / fd 1, snr 15 / fd 2). The capture and its outputs are broadcasts: they stay in
~/VibeSDR-test-audio/dab, never here."""
import sys, struct, numpy as np
src, dst, snr, fd, secs = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5])
seed = int(sys.argv[6]) if len(sys.argv) > 6 else 7
rng = np.random.default_rng(seed)
fi = open(src, 'rb'); hdr = fi.read(28); rate = struct.unpack('<d', hdr[8:16])[0]
fo = open(dst, 'wb'); fo.write(hdr)
# Rayleigh envelope: sum of 16 sinusoids (Jakes-like), unit mean power
M = 16; th = rng.uniform(0, 2*np.pi, M); ph = rng.uniform(0, 2*np.pi, M); ph2 = rng.uniform(0, 2*np.pi, M)
alpha = (2*np.pi*np.arange(M) + th) / (4*M)
# signal power estimate over the first second (in-block ≈ total for a strong mux)
blk = int(rate // 10); total = int(rate * secs); done = 0; t0 = 0
d = np.frombuffer(fi.read(2 * int(rate)), dtype=np.uint8); fi.seek(28)
x0 = (d[0::2].astype(np.float32) - 127.5) + 1j * (d[1::2].astype(np.float32) - 127.5)
p_sig = np.mean(np.abs(x0)**2) * (1.536 / 2.4) ** 0  # strong mux: total power ~ block power
nstd = np.sqrt(p_sig * (2.4/1.536) / 10**(snr/10) / 2)   # noise defined over the 1.536 MHz block
while done < total:
    d = np.frombuffer(fi.read(2 * blk), dtype=np.uint8)
    if len(d) < 2 * blk: break
    x = (d[0::2].astype(np.float32) - 127.5) + 1j * (d[1::2].astype(np.float32) - 127.5)
    t = (done + np.arange(blk)) / rate
    if fd > 0:
        hI = np.sum(np.cos(2*np.pi*fd*np.cos(alpha)[:, None]*t[None, :] + ph[:, None]), axis=0)
        hQ = np.sum(np.sin(2*np.pi*fd*np.sin(alpha)[:, None]*t[None, :] + ph2[:, None]), axis=0)
        h = (hI + 1j*hQ) / np.sqrt(M)
        x = x * h
    x = x + nstd * (rng.standard_normal(blk) + 1j * rng.standard_normal(blk))
    s = 30.0 / np.sqrt(p_sig * (1 + (2.4/1.536)/10**(snr/10)))
    y = np.empty(2 * blk, np.float32); y[0::2] = x.real * s; y[1::2] = x.imag * s
    fo.write(np.clip(np.round(y + 127.5), 0, 255).astype(np.uint8).tobytes())
    done += blk
print('wrote', dst, done / rate, 's')
