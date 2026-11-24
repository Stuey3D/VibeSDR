# ORBCOMM — a regular satellite (V12, 2026-11-24)

Stuart, 2026-11-24: *"we can treat orbcomm as a regular satellite, just put the caveat that encrypted messages not
decoded but we can show the raw data"*.

So an ORBCOMM bird is now an ordinary SATELLITE-mode pick — listed with its passes, Doppler-tracked, decoder attached —
instead of beta91's label-only name (docs/v12/SAT-LABELS.md). What is decoded is the satellite's own broadcast
housekeeping, which every subscriber terminal reads. **Subscriber messages are never decoded**: they are private user
traffic, sent "encoded, compressed or encrypted in an unknown manner" (Kenny), and are shown only as raw bytes, labelled
"subscriber message — not decoded".

## RESUME HERE

Branch `orbcomm` (off v12-experimental 999fb2d8d). Server, tests, docs by the coordinator's agent; clients (app + web
SATELLITE box, sky plot, gallery) by a helper agent on the same branch. Not merged, not released, no directory change.

Proven: every packet field against Kenny's own published example packets; the receiver on synthetic SDPSK (offsets,
Doppler slope, clock error, two carriers, Es/N0 sweep); a synthetic pass end to end (named by its own position report,
sky dots within 1° of SGP4). **On-air: see "Real data" below.**

## The signal (published facts)

| | |
|---|---|
| Band | 137–138 MHz, channel N = 137.0 MHz + N × 2.5 kHz (in use N ≈ 80–320, 137.2–137.8 MHz); each satellite sends on one or two |
| Modulation | SDPSK — ±90° a symbol, **the step itself is the bit** (Kenny's table: 1 = −90°, 0 = +90°; the step is the differential coding he calls NRZ-M — there is no second XOR); 4800 bit/s; RRC α 0.4 (≈ 6.7 kHz wide) |
| Framing | LSB first; a minor frame = 600 bytes = 1 s = 50 packets of 12 bytes (an ephemeris is 24) — sync, a zero fill, the rest |
| Check | Fletcher (ORBCOMM E80050015 App. F): both running sums zero over a good packet |
| Doppler | ±3.3 kHz at 137 MHz, ≤ ~35 Hz/s |

| Type | What | Decoded as |
|---|---|---|
| 65 | sync `65 A8 F9`, spacecraft ID, downlink channel, minor-frame counter | ID, channel → MHz, frame number |
| 1F | ephemeris (every 4 s): ID, ECEF x y z ẋ ẏ ż (20-bit offset values over ±8 378 155 m / ±7 700 m/s), GPS time of week + week | position (lat/lon/alt), speed, the satellite's clock vs ours |
| 1C | downlink channel list (sets of 3 packets, 5 channels each) | MHz list (the first is the satellite's own) |
| 1B | uplink channels, retries, access slots | MHz list (148.0 MHz + N × 2.5 kHz), counts |
| 22 | orbital elements (every ~4 min) for a spacecraft: mean anomaly, mean motion | degrees, rev/day |
| 1D | network control centre (gateway) information | shown as hex — its fields are not published |
| 1E | fill | counted only |
| **1A** | **subscriber message** | **never decoded — raw hex, "not decoded"** |

## Sources

- M. Kenny, *Ever wondered what is on the Orbcomm Satellite Downlink?* (May 2000, corrected Dec 2002) — the format,
  the packet types and fields, the channel plan, example packets (the tests use them), and the Fletcher routine from
  ORBCOMM's Serial Interface Specification E80050015 Appendix F.
- ORBCOMM Serial Interface Specification E80050015 Rev F — the ephemeris scaling, as used in F. Bieberly's
  MIT-licensed ORBCOMM-receiver (read for facts only; our code is our own).
- J. Harms, *The Orbcomm Experience* (OHB, 2004); OrbID, arXiv 2503.02118 (2025) — that the second-generation (OG2)
  satellites send the same format, and the sync packet's ID / channel fields.

## How it works

- **The pick** (`vibe_sat_engine.h`): today's directory mirror sends each ORBCOMM bird label-only (mode `"label"`, its
  own channel when SatNOGS lists exactly one alive downlink, else the 137.2–137.8 MHz band). The server turns such a bird
  (`orbcommBird`: name/id starts "ORBCOMM") into a regular one: mode + consumer `"orbcomm"`, loop `none`, use `packet`,
  decoder `orbcomm`. **Works with today's mirror — no directory change needed.** A future mirror may send mode
  `"orbcomm"` itself (accepted as is). Band labels keep the beta91 `b` flag so the clients still merge them into one.
- **The slice** (`decoderOf`): one listed channel → a 40 kHz IQ slice at the dial; the band → up to 640 kHz.
- **The receiver** (`vibe_orbcomm.h`): a scanner (an FFT every ~50 ms, averaged ~1 s) finds ORBCOMM-shaped carriers;
  each gets a `Chan` (≤ 4 AUTO, 1–6 manual `max_carriers`): NCO → SIMD decimating low-pass (NEON / SSE2 / scalar) →
  `vibelrpt::PolyRrc` matched filter + interpolator → Gardner → differential detection with a d⁴ frequency loop →
  the step's sign (either polarity: the sync word says which) → the framer. Acquisition is an x⁴ FFT at **four points a symbol with a three-line comb**: SDPSK's x⁴ has side
  lines at ± the symbol rate, and at two points a symbol the side line aliases onto 4Δf + 4800 — a 1200 Hz error (a
  quarter turn a symbol, invisible to differential detection). A failed packet gets a small Chase repair (the 7 weakest
  symbols, singly and in pairs: false-pass odds ≈ 28 / 65 536).
- **Which bird** (`vibe_orbcomm_id.h`): the sync ID is a byte, not a catalogue number, and no public table maps OG2 IDs.
  So an ephemeris packet's GPS position is compared with every ORBCOMM bird's SGP4 position: the nearest within 400 km
  names the carrier ("position") and the ID is learnt ("id"). Before one, the sync's channel + the measured Doppler vs
  each bird's prediction (within 900 Hz) names it per packet ("doppler").
- **The run** (`vibe_orbcomm_run.h`, the DecoderHost's `orbcomm` extension): `orbcomm_status` once a second, an
  `orbcomm_pkt` + a text line per packet worth one (housekeeping only when it changes / once a minute; ≤ 12 a second),
  sky dots (docs/v12/SKY-PLOT.md), and at the pass's end (the pick sets) or a stop, this radio's ORBCOMM sky plot PNG to
  the gallery (kind `orbcomm`).
- Capability `orbcomm` (reach-gated 137.19–137.81 MHz), the setup page's decoder switch, unattended passes (passKeepable).

## Real data

**2026-11-24 08:31 UTC capture (Pi 500, RTL-SDR V4, YouLoop, via the server's rtl_tcp IQ out): NOT usable.** It held only
a ~4.4 kHz channel at DC (the listener's passband resampled — a server bug, fixed separately), so no ORBCOMM carrier was
in it: 0 packets, correctly. A raw `rtl_sdr` capture (1.024 MS/s, 137.5 MHz) was made after; results below.

**2026-11-24 09:38:16 UTC raw capture** (`~/VibeSDR-test-audio/orbcomm/orbcomm-20261124T093816Z.cu8`: `rtl_sdr` on the
Pi 500's RTL-SDR V4 + YouLoop, 1.024 MS/s, 137.500 MHz, gain 40, 600 s, no header). **No ORBCOMM bird was up**: by
CelesTrak's current ORBCOMM elements only FM108 rose, reaching 3.9° in the last seconds; the spectrum shows no carrier
(only the RTL's ~3 dB ripple every ~40 kHz). So it proves no decode — but it found a real fault: the scanner adopted
that ripple as **268 "carriers" in 10 minutes**, each demodulated for 8 s. Fixed: a candidate must show an SDPSK line in
its x⁴ comb (`Chan::kAcqMinRatio` 16; measured: noise across this capture at most 10.7, synthetic 6 dB 24, 8 dB 75)
or it is dropped and banned at once. Re-run: 0 carriers, 0 packets — correct for an empty sky, at no cost.
**Still unproven on air: everything that needs a bird.** Next good passes over Luton that day (UTC): FM117 47° at 10:46,
FM32 40° at 11:05, FM108 49° at 11:37 — record ~10 minutes around one.

**2026-11-24 15:09 UTC, the first LIVE pick** (FM116, Pi 500 V4 + YouLoop + LNA, test build 8f365e138): the carriers were
found (137.7975 MHz, 137.6600 MHz, SNR 6–8 dB) but none ever framed — `lock:false ok:0 bad:0`, carrier ids past 600, `good:0`
all pass; steady spurs at 137.474 / 137.282 / 137.666 MHz were found again and again. Causes, fixed:

1. **The bit is the phase step itself.** Kenny's own table (1 = −90°, 0 = +90°) is the whole coding; the first build XORed
   successive steps as well (reading "NRZ-M" as a second layer) and its synthetic transmitter did the same, so every
   synthetic test passed and no real sync could ever be found. Either polarity is now accepted (the sync word says which).
   (F. Bieberly's MIT-licensed receiver, which decodes real passes, also takes the step's sign directly.)
2. **Spurs**: the scanner cuts lines narrower than a few bins before it looks for carriers; a carrier whose symbol steps
   sit at 0° (mean |sin| < 0.5) is a steady line — refused and banned ±2 kHz for 4× longer.
3. **The acquisition gate** (x⁴ line over the comb) refused real carriers once the line was smeared by a Doppler rate or an
   oscillator's wander; it is now only reported. The gate is the steps' mean |sin| half a second in (SDPSK ≥ 0.80 from
   6 dB, noise 0.64). The comb scores the weaker side line plus the centre (a spur fills one slot, never both sides; the
   centre breaks the 2400 Hz tie), and a carrier found a quarter turn out (n × 1200 Hz, |cos| high) is moved 1200 Hz
   towards the scanner's estimate and looked at again.
4. **Strikes**: a place refused again doubles its ban (≤ 5 min) — the empty-sky capture went from ~965 short-lived
   candidates in 10 minutes to 187, with 0 locks and 0 packets.

`orbcomm_status` now also carries `refused: {spurs, notSdpsk}`. Synthetic (test-orbcomm "live-like"): 6.5 dB, −30 Hz/s,
oscillator wander, 30 ppm clock, a 10 dB spur 6 kHz beside the carrier and two more across a 1.024 MS/s slice — decodes,
no spur ever opened. Es/N0 6 dB now 250 of ~500 packets, 8 dB 447. **Still unproven on a real pass** until the FM113
capture (15:43–15:57 UTC, bias-T on) is run.

**2026-11-24 16:53 UTC — THE FIRST REAL DECODE** (`orbcomm-FM116-v4l4-youloop-2400k-137500k-20261124T165319Z.cu8`: the Pi 500's
other RTL-SDR V4, 00000004, on the YouLoop with a LaNA, 2.4 MS/s, 137.500 MHz, 446 s):

| | |
|---|---|
| Carriers | both FM116 channels LOCKED: DCN 265 = 137.6625 MHz (SNR up to 13.8 dB), DCN 320 = 137.800 MHz (12.4 dB) |
| Packets | ≈ 16 500 good (1806 distinct subscriber-message packets, raw only); types also seen that no description names: 0A 0B 0D 0E 13 (≈ 7 %, most clean) — shown raw |
| Sync | `65 A8 F9 2E 20 09 m1`: ID 0x2E, byte 4 = 0x20 (Kenny: 0x10), F = 0x09 → Kenny's rule gives 0x109 = 265 = 137.6625 ✔; F = 0x40 → 320 = 137.800 ✔ |
| Naming | FM116 by its own position report; learnt ID 0x2E → 41189 (a "0x1E" seen once was a 2-symbol REPAIR that passed the check with the ID byte wrong — repaired packets no longer teach IDs) |
| Position | 700.2 km up, 7.161 km/s; **within 4.8–6 km of SGP4** once the clock is read as UTC |
| **Clock** | **the satellite's GPS week + TOW count UTC, not GPS time**: read as GPS − 18 s the clock sat 17.7–18.7 s behind the recording and the position missed by 124 km (18 s × 7.16 km/s); read as UTC, Δ = +0.3 s (whole seconds) and 5 km. `kOrbClockMinusUtc = 0` |
| Doppler | measured vs predicted at 137.6625 MHz: mean −31 Hz, rms 57 Hz over the pass (the dongle's ppm + the elements) |
| Sky | 48 five-second dots from 200°/46° to 109°/19° — decoding stopped below ≈ 19° on the YouLoop |

Fixed from it: **the AFC now reaches the mixer at once** — the NCO's step was set once per block, so on 48 kS/s blocks of
0.34 s the frequency loop wound up and ran away by kHz (the 12-s fixture went 3 → 571 good packets, 0 bad); the satellite
clock as UTC; IDs only from unrepaired packets; the undescribed types shown raw and deduplicated.

The same pass on the **other V4L (00000001, LaNA + FM-stop, the ADS-B aerial)**: no FM116 energy at all — +0.1 dB at
137.6625 MHz while the YouLoop dongle had 13.8 dB SNR; 137.800 MHz shows only the steady spur (+3 dB, constant). A hardware
answer: that chain does not hear 137 MHz.

**Regression on real signal:** `test/fixtures/orbcomm/fm116-20261124T165523Z-137662500-48k.cu8` (12 s, channel 265 mixed to
DC, 48 kS/s cu8, 1.1 MB) + FM116's CelesTrak elements of that day; `test-orbcomm-run` §3 asserts lock, 400–650 packets,
ID 0x2E, channel 265, named by position, ephemeris within 15 km of SGP4, the clock within 1.5 s, raw-only messages, dots at
the right elevation.

To check a capture: `vibeserver/tool-orbcomm-file <file.cu8> --rate 1024000 --centre 137500000 --start
<UTC ISO> --tle orbcomm.tle --lat … --lon … --csv out.csv` (a raw `rtl_sdr` file has no header; the tool handles both).
Look for: carriers found, sync/ephemeris counts, "learnt IDs" (sync ID → catalogue number), the ephemeris match in km
(the E80050015 scaling — if it is wrong the match is thousands of km and naming falls back to Doppler), the satellite
clock vs ours (GPS − 18 s leap seconds assumed), and whether the DCN rule (F < 0x50 → +0x100) gives the observed channel.

## Cost

M5 (one core): four carriers in a 1.2 MS/s slice = 2.6 % of a core; a 75 kS/s slice 0.2 %. The cost is the per-carrier
NCO at the slice rate (scalar, double) and the decimating low-pass (SIMD). **Pi 2: not measured** — by the usual
15–25× ratio a 640 kHz band pick with four carriers would be ~40–65 % of one core; a single-channel 40 kHz pick a few %.
If the Pi 2 needs it: a float NCO, or an AUTO of 2 carriers on armv7.

## Tests

| test | what |
|---|---|
| `vibeserver/test-orbcomm.cpp` | Kenny's example packets (sync, fill, uplink, downlink, elements, ephemeris) and Fletcher; the receiver on synthetic SDPSK at 0 / −47.5 / +61 kHz, ±35 Hz/s, ±60 ppm, 12–25 dB; two carriers; noise (0 packets); Es/N0 sweep |
| `vibeserver/test-orbcomm-run.cpp` | a synthetic pass over IO92 end to end: named by position, raw messages "not decoded", housekeeping lines, dots within 1°, the store, JSON, PNG |
| `vibeserver/test-sat-labels.cpp`, `test-satcat.cpp` | updated: ORBCOMM from the mirror's label shape → regular picks, decoder `orbcomm`, 40 kHz / 640 kHz slices, band labels still merge |
| `vibeserver/tool-orbcomm-file.cpp` | developer replay of a cu8 recording (CelesTrak TLEs, location; clock anchored on the satellites' GPS time when a capture drops samples) — not a test |

## Open / decisions for Stuart

- Directory: nothing required. Optional later: send mode `"orbcomm"` instead of `"label"` (servers before this branch
  would then skip ORBCOMM entirely — today's label shape keeps old servers labelling it).
- AUTO carriers = 4 everywhere (Pi 2 cost unmeasured).
- Grey "nothing decoded" dots only for the PICKED bird and only once it has decoded this pass (a switched-off bird or one
  on a channel outside the slice must not blame the aerial).
- NCC (1D) packets shown as hex: their fields are not published.
