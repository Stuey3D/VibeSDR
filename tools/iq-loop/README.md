# iq-loop — the website's Buzzer demo clip

BRIEF-waterfall-demo-iq-capture.md (v2). Regenerate the clip from the raw capture:

1. **Capture** (Pi 500, Airspy HF+), with that radio's VibeServer stopped (one process owns the USB device):

       sudo systemctl stop vibeserver@DD52B980BE4946DA
       ~/buzzer/hfplus_capture DD52B980BE4946DA 4626000 192000 240 <att> <lna> buzzer-raw.cf32
       sudo systemctl start vibeserver@DD52B980BE4946DA

   `hfplus_capture.c` sets the radio's own HF AGC **off** at a fixed attenuator/LNA and writes complex float32.
   VibeServer's raw IQ out is not used: it is refused on a shared dial, is unsigned 8-bit, and carries a slow
   auto-level (instant attack, 3 dB/s release) that pumps the floor on every buzz.
   Build on the Pi: `cc -O2 -o hfplus_capture hfplus_capture.c -lairspyhf -lm`.

2. **Loop**: `python3 make_loop.py --in buzzer-raw.cf32 --rate 192000 --centre 4626000 --recorded <UTC> --out ../../demo`
   writes `buzzer.cs8` (signed int8 I/Q, 6 kS/s, headerless), `buzzer.cs8.br`, `buzzer.json` and `seam-check.png`,
   and prints every step: the decimation filters, the buzz intervals, the chosen onsets, the seam interval
   against the loop's own range, and the int8 scale / noise floor in LSB / clip rate. Exit status 1 if the seam
   interval falls outside the range.

3. `synth_buzzer.py` makes a synthetic capture for testing the pipeline without a radio.

Departure from the brief: the seam crossfade is **equal-gain**, not equal-power. The overlaps are phase-aligned, so
the carrier is coherent across them, and equal-power lifted it by up to 3 dB mid-fade (a visible "+" on the carrier
at each join in the first synthetic seam check).
