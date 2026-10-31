#!/usr/bin/env python3
"""Build decoders-bench.vbu8: the benchmark's real-band decoder input (android/app/src/main/cpp/vibe_benchmark_more.h).

  python3 make-decoders-clip.py <test-audio dir> <ism.part from make-ism-records> [out.vbu8]

★★★ STUART'S OWN RECORDINGS ONLY (~/VibeSDR-test-audio, README-decoders.txt): his UberSDR (RX888) on 40 m CW and PSK and
    on the NDB band, and his Pi 500's RTL-SDR on 433 and 868 MHz. Amateur and aeronautical-beacon transmissions are
    public identifications, sent for anyone to receive; the ISM records hold NO real device (make-ism-records).
★★ WHY A REAL BAND. The CW and PSK skimmers decode every signal they find and the NDB skimmer every carrier — their cost
   is what is on the band. A few seconds of a real busy band, looped, is that; a synthetic band would be a guess.
★ The skimmers' windows are where the skimmer itself confirmed the most signals (see CW_START_S below), so the row
  measures a working band rather than a lull. Levels are kept as recorded (scaled only to fit the sample format).
★ Every record is 8-bit (u8 offset binary, as rtl_sdr writes it), peak scaled to ±120: 3.8 MB gzipped, for hosts on
  slow links. The CW and PSK skimmers decode the same signals from it as from 16-bit (checked 2026-10-31).
★ Format: "VIBEDC81", then per record: char id[8], double rate, double centreHz, uint32 bits (8 = u8 offset binary,
  16 = s16), uint32 nComplex, the I/Q samples (little-endian).
"""
import struct, sys, wave, os
import numpy as np

CW_START_S, PSK_START_S = 60.0, 30.0

def read_wav(path, start_s, secs):
    w = wave.open(path)
    assert w.getnchannels() == 2 and w.getsampwidth() == 2, path
    fs = w.getframerate()
    w.setpos(int(start_s * fs))
    raw = np.frombuffer(w.readframes(int(secs * fs)), dtype='<i2').astype(np.float64)
    return fs, raw[0::2] + 1j * raw[1::2]

def record(f, rid, rate, centre, x, bits):
    if bits == 16:
        pk = np.max(np.abs(np.concatenate([x.real, x.imag]))) or 1.0
        g = 32000.0 / pk
        iq = np.empty(2 * len(x)); iq[0::2] = np.round(x.real * g); iq[1::2] = np.round(x.imag * g)
        b = iq.astype('<i2').tobytes()
    else:
        pk = np.max(np.abs(np.concatenate([x.real, x.imag]))) or 1.0
        g = 120.0 / pk
        iq = np.empty(2 * len(x)); iq[0::2] = np.clip(np.round(x.real * g + 127.5), 0, 255); iq[1::2] = np.clip(np.round(x.imag * g + 127.5), 0, 255)
        b = iq.astype(np.uint8).tobytes()
    f.write(rid.encode().ljust(8, b'\0'))
    f.write(struct.pack('<ddII', rate, centre, bits, len(x)))
    f.write(b)
    print(f'{rid}: {len(x) / rate:.1f} s at {rate / 1e3:.0f} kHz, {bits}-bit, {len(b) / 1e6:.2f} MB')

def main():
    d = sys.argv[1]; ism = sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else 'decoders-bench.vbu8'
    with open(out, 'wb') as f:
        f.write(b'VIBEDC81')
        # ★ The windows: the 6 s in which the skimmer itself CONFIRMED the most signals (fed twice, as the row feeds it),
        #   found by running CwRun / PskRun over every window of the recording (2026-10-31: CW 4 at 60 s; PSK 2 at 30 s).
        #   A "most energy" pick chose RTTY and a wide data signal on 40 m, where there was no PSK at all (docs/v12/PSK.md).
        # CW: 40 m at 7.020 MHz, 48 kHz IQ (2026-10-29 evening)
        p = os.path.join(d, 'cw/uber_40m_7020000_iq48_2026-10-29.wav')
        fs, x = read_wav(p, CW_START_S, 6.0); record(f, 'cw40', fs, 7020000.0, x, 8)
        # PSK: 20 m at 14.0715 MHz, 48 kHz IQ (2026-10-26 16:05 UTC — four PSK31 stations; docs/v12/PSK.md "Real signals")
        p = os.path.join(d, 'psk/uber_20m_14071500_iq48_afternoon.wav')
        fs, x = read_wav(p, PSK_START_S, 6.0); record(f, 'psk20', fs, 14071500.0, x, 8)
        # NDB: 192 kHz around 352.5 kHz (2026-10-26 07:29 UTC) → 96 kHz around 352.5 kHz, 10 s, 8-bit (beacons are strong)
        p = os.path.join(d, 'ndb/uber_ndb_352500_iq192_20261026T0729_330s.wav')
        fs, x = read_wav(p, 60.0, 10.0)
        X = np.fft.fft(x); m = len(x) // 2
        Y = np.concatenate([X[:m // 2], X[-(m - m // 2):]])          # keep ±48 kHz: an FFT resample to 96 kHz
        y = np.fft.ifft(Y) * (m / len(x))
        record(f, 'ndb', fs / 2, 352500.0, y, 8)
        # ISM: the records make-ism-records wrote (real floor, synthetic devices only)
        f.write(open(ism, 'rb').read())
        print('ism433 + ism868: from', ism)
    print(f'{out}: {os.path.getsize(out) / 1e6:.2f} MB')

if __name__ == '__main__':
    main()
