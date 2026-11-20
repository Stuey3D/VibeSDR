# vsrec — record raw IQ from a VibeServer radio

Runs ON the server (the per-radio ports listen on 127.0.0.1 only), standard library Python only.

    SETTLE=15 BW=12500 python3 vsrec.py PORT FREQ_HZ MODE RATE SECONDS OUT.cu8 [--back-to-adsb]

- Leaves ADS-B if it is on, tunes, sets the passband to ±BW Hz (default: the mode's own), waits SETTLE s for the AGC,
  asks for raw IQ out at RATE, writes the rtl_tcp stream's 8-bit IQ (the 12-byte header dropped), pings the spectrum
  socket every 5 s (a session with no application message for 30 s is released), then turns IQ off.
- `--back-to-adsb` puts a radio that lives in ADS-B back there afterwards.
- ★ It TAKES THE DIAL. Check the radio's `listeners` is 0 first (vibeserver.json) — and remember your own previous run
  is held for 180 s and counts as one.

VOR: the AM passband (±6 kHz) cuts the 9960 Hz subcarrier — use BW=12500. Replay with tools/vor-replay.
