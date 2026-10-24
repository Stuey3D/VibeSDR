#!/usr/bin/env python3
"""Build adsb-bench.vbu8: the benchmark's ADS-B input (android/app/src/main/cpp/vibe_benchmark_adsb.h).

  python3 make-adsb-clip.py <flightaware-2026-10-24-3.2msps.u8> [out.vbu8]

★★★ STUART'S OWN SKY. One second of his own 1090 MHz recording (rtl_sdr, 8-bit IQ, RTL-SDR on his FlightAware PiAware
    antenna, 2026-10-24 ~09:13 UTC, tuner gain 49.6 dB) — his own reception of public ADS-B broadcasts, which every
    aircraft transmits unencrypted for anyone to receive; fine to ship (Stuart, 2026-10-24).
★★ TWO RECORDS FROM THE SAME SECOND, the two rates ADS-B runs at: 2.4 MS/s (an RTL, an Airspy, a HackRF under the rate
   switch) and 3.0 MS/s (an SDRplay RSP). Both RESAMPLED from the 3.2 MS/s recording (an FFT resample: the band kept to
   the narrower rate's Nyquist, nothing else touched), so the two rows compare the CPU's cost on the same replies.
★ Format: "VIBEAB81", then per record: double rate, double centreHz, uint32 nBytes, nBytes of u8 I/Q (little-endian).
"""
import struct, sys
import numpy as np

SRC_RATE, START_S, SECS = 3.2e6, 10.0, 1.0

def main():
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else 'adsb-bench.vbu8'
    n = int(SRC_RATE * SECS)
    raw = np.fromfile(src, dtype=np.uint8, count=2 * n, offset=int(SRC_RATE * START_S) * 2)
    x = (raw[0::2].astype(np.float64) - 127.5) + 1j * (raw[1::2].astype(np.float64) - 127.5)
    X = np.fft.fft(x)
    with open(out, 'wb') as f:
        f.write(b'VIBEAB81')
        for rate in (2.4e6, 3.0e6):
            m = int(rate * SECS)
            Y = np.zeros(m, dtype=complex)
            h = m // 2
            Y[:h] = X[:h]; Y[-(m - h):] = X[-(m - h):]
            y = np.fft.ifft(Y) * (m / n)
            iq = np.empty(2 * m)
            iq[0::2] = np.clip(np.round(y.real + 127.5), 0, 255)
            iq[1::2] = np.clip(np.round(y.imag + 127.5), 0, 255)
            b = iq.astype(np.uint8).tobytes()
            f.write(struct.pack('<ddI', rate, 1090e6, len(b)))
            f.write(b)

if __name__ == '__main__':
    main()
