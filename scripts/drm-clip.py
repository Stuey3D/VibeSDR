#!/usr/bin/env python3
# drm-clip.py — a fake-rtl_tcp replay clip from a DRM recording (scripts/test-server-drm.mjs, docs/v12/DRM.md).
#   python3 scripts/drm-clip.py <ubersdr iq48 .wav> <out.cu8> <seconds>
# The 48 kHz I/Q is interpolated ×6 to 288 kS/s, the station moved 30 kHz above the clip's centre (off the dongle's DC),
# and quantised to unsigned 8-bit I/Q as rtl_tcp sends it. ★ Linear interpolation leaves images 48 kHz away — far outside
# the 10 kHz DRM channel the server cuts out, so they cost nothing there. Needs numpy.
import numpy as np, wave, sys
src, out, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])
w = wave.open(src); n = min(w.getnframes(), int(48000*secs))
d = np.frombuffer(w.readframes(n), dtype='<i2').reshape(-1, 2).astype(np.float32)
x = d[:, 0] + 1j * d[:, 1]
R = 6; t = np.arange(len(x) * R) / R
y = np.interp(t, np.arange(len(x)), x.real) + 1j * np.interp(t, np.arange(len(x)), x.imag)
y *= np.exp(2j * np.pi * 30000 * np.arange(len(y)) / 288000)   # the station 30 kHz above the clip's centre
y = y / np.percentile(np.abs(y), 99.9) * 90
u = np.empty(len(y) * 2, dtype=np.uint8)
u[0::2] = np.clip(np.round(y.real + 127.5), 0, 255); u[1::2] = np.clip(np.round(y.imag + 127.5), 0, 255)
u.tofile(out); print(out, len(y) / 288000, 's')
