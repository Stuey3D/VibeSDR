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
| Modulation | SDPSK — ±90° a symbol — after NRZ-M (bₙ = bₙ₋₁ ⊕ Iₙ); 4800 bit/s; RRC α 0.4 (≈ 6.7 kHz wide) |
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
  NRZ-M → the framer. Acquisition is an x⁴ FFT at **four points a symbol with a three-line comb**: SDPSK's x⁴ has side
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

<!-- RESULTS -->

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
