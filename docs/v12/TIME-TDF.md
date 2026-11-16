# TDF / ALS162 time code (V12, 2026-11-16)

Allouis, France, 162 kHz (ANFR). A station of the TIME decoder beside MSF, DCF77, WWV/WWVB, RWM and JJY.

| File | What |
|---|---|
| `android/app/src/main/cpp/decoders/tdf_phase.h` | the phase front end: tone search, mixer, slow PLL, matched filter, "carrier low" envelope |
| `decoders/time_decoder.{h,cpp}` | `Station::TDF`: the DCF77 reader behind the front end; tag "TDF"; leap warning from bits 1–2 |
| `vibe_decoder_host.h` | `tdf` extension → `Station::TDF`; time_sync with CET/CEST and the front end's delay |
| `local_sdr_shim.cpp` | `"timeTdf":true` in the capabilities; `tdf` counted as TIME for the owner's switch |
| `web/client/index.html`, `main.ts` | the TDF 162k station key (shown only where `timeTdf`) and its hint |
| `src/services/DecoderClient.ts`, `ModeSelector.tsx`, `SDRScreen.tsx`, `sdrTypes.ts`, `knownSignals.ts` | the app: AUTO picks TDF at 162 kHz on a server that reads it; the STATION row offers it there only |
| `handheld-ui/src/dec_state.h` (+ generated `known_signals_gen.h`) | the R36S station list and the dial's known signal |
| `vibeserver/test-tdf.cpp` | tests (below) |

## The format (published)

- Constant carrier, **phase-modulated**. Each element: +1 rad over 25 ms, −2 rad over 50 ms, +1 rad over 25 ms. The
  zero crossing in the middle of the first element is the top of the second (the element starts 50 ms before it),
  preceded by 100 ms with no modulation.
- One element = **0**, two in a row = **1**; second **59** carries nothing (the minute mark).
- The time code is DCF77's from bit 15 on (bits 17/18 CEST/CET, 20 start of time, 21–58 minute, hour, day, weekday,
  month, year with DCF77's three even parities). Bits 1 and 2 warn of a positive / negative leap second (DCF77 uses
  19, which is 0 on TDF); 3–6 a check weight over 21–58; 13–14 French public holidays.
- "Additional non-timing data" is said to be sent by phase modulation later in each second; its format is not in the
  material we have.

## How it is read

The listener tunes **162.000 kHz in CW** (or USB with the dial below the carrier): the carrier becomes a beat note,
with its phase intact. In AM the code is invisible (the envelope is flat) — every hint says so.

`TdfPhase` finds the beat note (Goertzel, 300–3000 Hz, 10 Hz steps + a parabola, once every 2 s until found), mixes
it to 0 Hz, low-passes (3 × one-pole, 120 Hz) and decimates to 1 kHz. A slow PLL (3 Hz while acquiring, 0.7 Hz after
3 s) follows the carrier and the receiver's drift but not the 10 Hz triangle, whose mean phase is zero. Its phase
error is the modulation: correlated with one element's triangle (101 taps), then "carrier low" wherever an element
centre is within ±35 ms. A 0 is then a 100 ms dip and a 1 a 200 ms dip, the shape the **DCF77 reader** already reads —
so TDF gets its one-second flywheel, templates, multi-minute vote, fields-as-they-arrive and two-minute corroboration
unchanged. The reader runs as DCF77; only the tag ("TDF") and the leap-second bits differ.

★ ±35 ms rather than ±50: the correlation itself stays high about ±15 ms round a centre; with ±50 a 0 read as 125 ms
and the reader's confidence fell to 0.28 (below its 0.4 read threshold) — nothing decoded. With ±35: 0.83 / 0.92.

★ Timing: the dip opens `kDipAfterSecondMs` = 35 ms after the top of the second (measured against DCF77's AM read by
the same reader; test-tdf fails if it moves). The host adds it to time_sync's `rxDelayMs` (31 → 66 ms for TDF), so
SET CLOCK FROM TDF lands on the second.

Cost: ~0.04 % of one M5 core for an hour of 48 kHz audio (scalar; a mixer, three one-poles and a 101-tap correlator
at 1 kHz — not a hot loop). The search is ~0.7 M multiply-adds a second only while no tone is found.

## Tests (`vibeserver/test-tdf.cpp`, in scripts/run-tests.sh)

All synthetic from the format above, 31 checks:
- clean at 500 / 1000 / 2200 Hz beat notes: 3 of 4 minutes announced (the first is the hunt), all right, tag TDF, CET;
- SNR in 2.5 kHz: +20 / +10 / +5 / 0 dB → 4 / 4 / 4 / 5 of 6 minutes; −3 dB → 2; −6 dB → 0; **never a wrong time**;
- the beat note drifting 3 Hz in 10 min; ±0.3 rad (and ±1 rad, 4 of 6) of other phase activity at 400–900 ms;
- ten minutes of noise, DCF77's AM given to TDF, TDF given to DCF77, a bit flipped after the parity: nothing wrong;
- bit 1 → "leap second pending"; the +35 ms timing against DCF77; CPU.
- `test-time-decoder` (all the other stations) still passes.

## On air — DONE 2026-11-16 (one recording)

Recorded from Stuart's own UberSDR (RX888, standing permission): `tools/ubersdr-rec/ubersdr-rec.mjs 192.168.86.11:80
161000 usb 420 …` — 7 minutes, USB with the dial 1 kHz below the carrier, 12 kHz, kept at
`~/VibeSDR-test-audio/tdf/tdf-162k-usb-2026-11-16.wav` (start_utc 1794815529.079, ≈ 07:52 UTC, a Monday morning).

    test-tdf --wav ~/VibeSDR-test-audio/tdf/tdf-162k-usb-2026-11-16.wav 1794815529.079

Carrier 26 dB as the reader sees it; **5 minutes announced (08:55 … 08:59 CET), all matching the recording's own clock,
0 failed, 0 wrong.** So whatever the "additional non-timing data" is, it did not disturb the reader on this signal.

Not yet checked: a weak or night-time signal (skywave fading on 162 kHz), a listener's RTL dongle in direct sampling,
summer time (CEST), a leap-second warning on air, the SET CLOCK offset against a GPS clock, and the bits 3–6 weight
(not used as a check — it would only reject minutes).
