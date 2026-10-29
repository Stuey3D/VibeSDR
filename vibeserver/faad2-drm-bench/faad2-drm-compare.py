#!/usr/bin/env python3
# faad2-drm-compare.py <ref.f32> <test.f32> — two faad2-drm-bench outputs: SNR of ref against (test - ref), max |diff|,
# level difference. The bar the FAAD2 patch series is held to: >= 131 dB against stock FAAD2 built for the same target.
import sys, numpy as np
def rd(p):
    b = open(p, 'rb').read(); assert b[:6] == b'F32PCM'
    r, c = np.frombuffer(b[8:16], '<u4'); return int(r), int(c), np.frombuffer(b[16:], '<f4').astype(np.float64)
r1, c1, a = rd(sys.argv[1]); r2, c2, b = rd(sys.argv[2])
assert (r1, c1) == (r2, c2) and len(a) == len(b), ((r1, c1, len(a)), (r2, c2, len(b)))
d = b - a; e = (d * d).sum()
snr = float('inf') if e == 0 else 10 * np.log10((a * a).sum() / e)
lv = 10 * np.log10((b * b).sum() / (a * a).sum())
print(f"{sys.argv[2]}: {len(a)//c1/r1:.1f} s, SNR {snr:.2f} dB, max|diff| {np.abs(d).max():.3g} (peak {np.abs(a).max():.3g}), level {lv:+.5f} dB")
