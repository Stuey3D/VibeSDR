adsb-bench.vbu8 — the server benchmark's ADS-B input (android/app/src/main/cpp/vibe_benchmark_adsb.h).

What it is: one second of Stuart's own 1090 MHz recording, made 2026-10-24 ~09:13 UTC on his FlightAware PiAware
antenna (RTL-SDR, rtl_sdr 8-bit IQ, 3.2 MS/s, tuner gain 49.6 dB), resampled to the two rates ADS-B runs at —
2.4 MS/s and 3.0 MS/s — by make-adsb-clip.py. It is his own reception of public ADS-B broadcasts, which every
aircraft transmits unencrypted for anyone to receive; nothing in it belongs to anyone else. Fine to ship (Stuart).

Hosting: like dab-bench.vbu8.gz, it is NOT in the repo (10.8 MB, 6.5 MB gzipped). It must be uploaded, gzipped, as a
GitHub release asset at
    https://github.com/Stuey3D/VibeSDR/releases/download/bench-clip-v2/adsb-bench.vbu8.gz
(release tag bench-clip-v2) — the URL ensureAdsbClip (desktop/Linux) and VibeBenchmark.kt (Android) fetch on the
first benchmark run. Until it is there the ADS-B rows are simply absent from the results, never guessed.
Checksums: sha256.txt.

Rebuild: python3 make-adsb-clip.py <flightaware-2026-10-24-3.2msps.u8> adsb-bench.vbu8 && gzip -9 -k adsb-bench.vbu8
(needs numpy; the recording lives in ~/VibeSDR-test-audio/adsb/).
