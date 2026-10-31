decoders-bench.vbu8 — the server benchmark's real-band decoder input (android/app/src/main/cpp/vibe_benchmark_more.h).

Why: the CW and PSK skimmers, the NDB skimmer and ISM's burst finder do work in proportion to what is ON the band, so
their benchmark rows run on a few seconds of real band, looped. Every other decoder row is synthetic (one signal costs
the same whatever else is on the air) and needs no download.

What is in it (five records, 7.9 MB, 3.8 MB gzipped — every record 8-bit since 2026-10-31):
  cw40    6 s, 48 kHz IQ u8, 7.020 MHz — Stuart's UberSDR (RX888), 2026-10-29 evening, 40 m CW
          (~/VibeSDR-test-audio/cw/uber_40m_7020000_iq48_2026-10-29.wav from 60 s)
  psk20   6 s, 48 kHz IQ u8, 14.0715 MHz — Stuart's UberSDR, 2026-10-26 16:05 UTC, 20 m PSK31
          (~/VibeSDR-test-audio/psk/uber_20m_14071500_iq48_afternoon.wav from 30 s)
  ndb     10 s, 96 kHz IQ u8, 352.5 kHz — Stuart's UberSDR, 2026-10-26 07:29 UTC, the NDB band
          (~/VibeSDR-test-audio/ndb/uber_ndb_352500_iq192_20261026T0729_330s.wav from 60 s, FFT-resampled to 96 kHz)
  ism433  0.5 s, 2.4 MS/s IQ u8, 433.92 MHz — Stuart's Pi 500 RTL-SDR (pi500-rtl4-433920-2400k-30s.u8), REAL FLOOR ONLY
  ism868  0.5 s, 2.4 MS/s IQ u8, 868.6 MHz — Stuart's Pi 500 RTL-SDR (pi500-rtl4-868600-2400k-30s.u8), REAL FLOOR ONLY
The skimmer windows are where the skimmer itself confirmed the most signals (a probe ran CwRun / PskRun over every
window): CW 4 signals, PSK 2 — the same from the 8-bit records as from the 16-bit ones they replaced (Stuart chose
the smaller download for slow links; the skimmers decode the same signals, test-bench-decoders with the clip). The 40 m PSK recording held no PSK at all (RTTY and a wide data signal), so 20 m is used.

Whose: all Stuart's own recordings. CW and PSK are amateur stations identifying themselves; NDBs are aeronautical
beacons sending their idents — public transmissions, sent for anyone to receive. No broadcast audio, no paging.

★★ ISM PRIVACY. A real 433/868 capture holds neighbours' sensor IDs, key-fob codes and tyre IDs. The ISM records hold
   NONE: make-ism-records picks a half-second window in which ISM's own burst detector, run on the WHOLE 2.4 MHz
   capture, finds no burst at all (and on 868, LoRa's detectors on every plan channel find no preamble), then adds
   SYNTHETIC devices — test-ism's Oregon THGR810, Fine Offset WH1080 and fixed-code remote, and test-wmbus's Sensus and
   Kamstrup frames, all with the tests' made-up IDs (documentation examples, no real device). What is real is the noise
   floor, the band's carriers and the tuner's spurs — what the burst finder's cost depends on. It refuses to write
   unless decoding the result gives back exactly the synthetic devices. Chosen 2026-10-31: 433 window at 1.0 s (decodes
   two THGR810 readings, the synthetic sensor's two repeats), 868 window at 0.5 s (decodes the two synthetic meters).

Hosting: like dab-bench.vbu8.gz and adsb-bench.vbu8.gz it is NOT in the repo (.gitignore). It must be uploaded, gzipped,
as a GitHub release asset at
    https://github.com/Stuey3D/VibeSDR/releases/download/bench-clip-v3/decoders-bench.vbu8.gz
(release tag bench-clip-v3) — the URL ensureDecoderClip (desktop/Linux, vibe_benchmark_adsb.h) and VibeBenchmark.kt
(Android) fetch on the first benchmark run, beside the DAB and ADS-B clips. Until it is there, or with no internet,
those five rows read "not measured" (never a guess) and every other row still runs. Checksums: sha256.txt
(the .gz made with gzip -9 -n, so it rebuilds byte-identical).

Rebuild (needs numpy, and the recordings in ~/VibeSDR-test-audio):
    c++ -std=c++17 -O2 -I vibeserver -I android/app/src/main/cpp -o make-ism-records \
        vibeserver/bench-clip/make-ism-records.cpp vibeserver/bench-clip/ism-part-433.cpp \
        vibeserver/bench-clip/ism-part-868.cpp -lpthread
    ./make-ism-records ~/VibeSDR-test-audio/ism/pi500-rtl4-433920-2400k-30s.u8 \
        ~/VibeSDR-test-audio/ism/pi500-rtl4-868600-2400k-30s.u8 ism.part
    python3 vibeserver/bench-clip/make-decoders-clip.py ~/VibeSDR-test-audio ism.part decoders-bench.vbu8
    gzip -9 -n -k decoders-bench.vbu8
