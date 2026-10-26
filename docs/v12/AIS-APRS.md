# AIS and APRS (V12)

Branch `v12-ais-aprs` (from `v12-experimental`). Two VHF data decoders that share one HDLC back end.

- **AIS** — ships, aids to navigation, base stations, SAR aircraft and distress beacons on 161.975 / 162.025 MHz, as a
  **whole-radio mode** (ADS-B's and ISM's shape) with a full-screen ship map.
- **APRS** — amateur packet stations (positions, weather, objects, messages) as a **listener's decoder** on their NFM
  audio (1200 Bd, VHF) or USB audio (300 Bd, HF), with a station map.

| File | What |
|---|---|
| `android/app/src/main/cpp/vibe_hdlc.h` | NRZI, flags, bit-stuffing, the FCS-16, soft-decision repair of one channel bit, a PLL bit clock |
| `vibe_ais.h` | AIS front end (both channels from one capture), the GMSK demodulator, the M.1371 messages, the MID → flag table |
| `vibe_ais_service.h` | the AIS thread + drop-never-wait queue, the ship list, the JSON, a ship's track, the gallery's copy |
| `vibe_aprs.h` | the Bell 202 / HF 300 Bd AFSK demodulator, AX.25, every APRS 1.0.1 format, readable lines, the region's channel |
| `vibe_aprs_run.h` | one APRS decoder run (a listener's or a background VFO's): the duplicate rule, the station list, messages |
| `vibe_station_sink.h`, `vibe_station_log.h` | AIS and APRS in the gallery: the day's lines + a summary card of stations |
| `local_sdr_shim.cpp` | AIS MODE block (beside ISM's), the capability, `ais_trail` |
| `vibe_decoder_host.h`, `vibe_bgdecode.h` | the `aprs` extension and its gallery tap (`StationTap`) |
| `web/mapkit/tracklayer.js` | the ship/station map layer (one file, as `adsblayer.js`) — `scripts/gen-tracklayer-source.mjs` copies it |
| `web/client/src/tracks.ts` | the full-screen map + live list, AIS and APRS icons, rows and cards |
| `vibeserver/test-ais.cpp`, `test-aprs.cpp`, `scripts/test-server-ais-aprs.mjs`, `scripts/test-web-ais-aprs.mjs` | tests |

## Standards, and other people's code

Built from the published documents, written fresh: **ITU-R M.1371-5** (Annex 2: GMSK BT 0.4, 9600 bit/s, NRZI, HDLC,
training sequence; Annex 8: the message table), **ISO/IEC 13239** (HDLC framing, the FCS-16), **AX.25 v2.2** (TAPR) and
the **APRS Protocol Reference 1.0.1** (TAPR) with the 1.1 addenda (`!DAO!`, base-91 Mic-E altitude). The MID table is
ITU's (M.585 / MARS) — `vibe_maritime_mid.h`, the ONE table DSC and AIS share. GPL decoders were read for behaviour only; no code, table or structure was
copied. The APRS **symbols are our own drawings** (`tracks.ts APRS_GLYPH`): no third-party symbol artwork is used, so
there is no licence to carry.

## AIS

### The receiver (`vibe_ais.h`)

- **Where the radio sits:** 162.000 MHz exactly, so its DC remnant falls between the two channels (−25 / +25 kHz).
- **Shared chain:** integer FIR decimators (vibedsp `FirDecimator` — NEON on ARM, SSE2 on x86) of at most 8 each, each
  only as long as its own transition needs, down to ≥ 100 kS/s: 2.4 MS/s → 300 k → 100 k (36 + 50 taps); 2.048 MS/s →
  256 k → 128 k. Any capture ≥ 96 kS/s works. No resampler and no rate change, ever.
- **Each channel:** an NCO to 0 Hz, a Blackman FIR passing ±7 kHz and stopping by ±14 kHz, decimated to ≥ 38.4 kS/s
  (4+ samples a bit). The ±7/14 kHz width was the best of five measured (±9/22 let in 3 dB more noise).
- **Demodulator:** polar discriminator (vibedsp `fastAtan2`), a 33-tap post-detection low-pass (`dotReal`, SIMD), then
  the crystal's offset removed by a **peak/valley midpoint** that is restarted by a rise in power (a burst arriving),
  relaxes fast through the training sequence and slowly inside a frame, and never takes a sample more than two
  deviations from the middle (noise between frames swings to ±fs/2 and used to wreck the next frame's start).
- **Five fixed sampling phases, no PLL.** A frame is at most ~1000 bits and a transmitter's clock is good to a few ppm,
  so the bit timing cannot drift a tenth of a bit inside one: only the phase is unknown. Five clocks a fifth of a bit
  apart, each with its own deframer; one is always within a tenth of a bit of the eye's centre. (A zero-crossing PLL was
  tried first and lost a third of frames at high SNR: GMSK's own ISI moves crossings ±0.1 bit.)
- **The best copy, not the first:** the five phases close a frame within about a bit of each other; it is held two bits
  and the cleanest copy (unrepaired, highest mean |soft|) is kept.
- **Repair:** on a bad FCS the 8 weakest LEVELS are flipped one at a time (one wrong level is two wrong NRZI bits —
  `vibe_hdlc.h`), the frame re-decoded, and accepted only if its length fits its type. ★★★ **A repaired frame must name
  an MMSI already heard cleanly** (`Service::accept_`) — without that rule one garbage frame in ~3000 failed ones got
  through (measured at 14 dB); with it, none, and ships repeat every few seconds so the repairs still fill the gaps.

### Messages read

1/2/3 (class A position), 4 and 11 (base station, UTC), 5 (static and voyage: name, callsign, IMO, type, size, draught,
ETA, destination), 9 (SAR aircraft), 12/14 (safety text — shown in a strip on the map), 18/19 (class B), 21 (aid to
navigation, virtual / off position), 24 A/B (class B static), 27 (long-range position). Every other type with a valid
FCS and a plausible length is counted (`other`), not shown. A row per MMSI merges them all; kept 60 min after last heard,
at most 1000. The MID gives the flag (`flag`, `country`); `00MID` coast stations, `111MID` SAR aircraft, `98/99MID`
craft and AtoNs, `970/972/974` SART / MOB / EPIRB (shown red as DISTRESS BEACON).

Readable line: `WILSON GOOLE · cargo · 12 kn SW · at anchor → ROTTERDAM`.

### Server contract

**Capability** (`/vibeserver.json`): no `ais` field = the radio cannot reach 162 MHz, or it is an Airspy HF+ or an Icom
(no control at all — AGENTS.md). `"ais": true` = can run; `"ais": false` + `"aisWhy"` = greyed with the reason (the owner
switched AIS off, a locked centre on a per-VFO radio, a capture under 96 kS/s). `"modes"` lists `ais` when it can run.

**Client → server** (spectrum socket, gated by ask-before-tuning): `{"type":"ais","on":1}` / `{"type":"ais","on":0}`,
`{"type":"ais_trail","mmsi":N}` (a read, answered to the asker only). In AIS, `tune`, `mode`, `bandwidth`, `zoom`,
`reset`, `tunerbw`, `sampleRate` are ignored; AIS leaves through its own door when DAB, ADS-B or ISM is entered.

**Server → client**: `{"type":"ais", centre, rate, now, msgs, msgRate (/min), chA, chB, repaired, other, dropped,
capture, noise, gain?, gainAuto?, vessels:[…], safety:[…]}` once a second and at once to a joiner (its arrival means AIS
is on); `{"type":"ais_off"}` then a `config`; `{"type":"ais_error","why"}`; `{"type":"ais_trail","mmsi","pts":[[lat,lon,ageSec]…]}`.
A vessel: `mmsi kind(A|B|base|aton|sar|sart) name call dest imo flag country stype stypeName family nav navName aton
atonName virtual offpos lat lon sog cog hdg rot|turn draught len beam eta alt trend(+1/−1 — the card's ▲/▼) line ch
level snr seen age posAge count trail`.

**Gain:** ISM's rule (`vibeism::GainRule`) on the radio's own gain list: start 60 % up, down a rung when > 2 % of
samples clip, back up after two quiet minutes. A listener's gain pauses it; AUTO resumes it.

### The web client

AIS sits in the demodulator menu beside ISM (absent / greyed with the reason / live). Pressing it covers the page with
the ship map (`TrackView`): EXIT AIS · the channels · a status line (heard, on the map, msg/min, capture when short) ·
FIT; a live list in the corner, nearest first (flag, name, type → destination, speed + course, range + bearing), which
folds to its header (remembered); a safety strip above it when a type 12/14 arrives. Ships are hulls pointing along
their heading, coloured by type (cargo green, tanker red, passenger blue, fishing orange, tug cyan, leisure violet,
special yellow), a dot when still; AtoNs a yellow diamond (dashed when virtual, red when off position); base stations a
mast; SAR aircraft a plane. They **glide** to each fix (2.2 s, the ADS-B layer's), fade in and out, show their name at
closer zooms. A tap (or a row) selects one: its **track** (asked of the server: up to 360 points, one per 50 m or
minute) dashed behind it and its **card** — MMSI, flag, call, IMO, type, status, speed with ▲/▼, course and heading,
turning ◀/▶, size, draught, destination + ETA, position, range from the receiver, last heard, channel and SNR. The
waterfall is not drawn while the map covers it; no audio is sent (the page lets go of the output, as for ADS-B/ISM).

## APRS

### The demodulator (`vibe_aprs.h`)

- The listener's 48 kHz audio → 12 kS/s through a SIMD FIR (`dotReal`).
- One **quadrature correlator per tone over exactly one bit** (a matched filter for a tone burst), each tone's level
  followed by its own fast-attack / slow-release peak: a transmitter's pre-emphasis (or a receiver's de-emphasis) that
  puts the tones 6 dB apart is divided out.
- **Several slicers:** mark − g·space at five tone balances (−6…+6 dB), each with its own PLL bit clock (looser while
  hunting the preamble's flags, firmer inside a frame) and deframer. **300 Bd HF** (1600/1800 Hz) adds five tuning offsets
  (−60…+60 Hz in 30 Hz steps): an HF operator is never exactly on frequency (three offsets lost a quarter of frames half
  way between them). The cleanest copy of a frame is kept.
- Repair of one level, accepted only if the frame is still a valid AX.25 UI frame with the APRS PID and twelve-plus valid
  address characters (about one garbage frame in three million could pass that).

### Formats read

Positions plain (with ambiguity) and compressed (course/speed, altitude), `!DAO!`, `/A=` altitude, PHG, CSE/SPD;
**Mic-E** (latitude and message bits in the destination, the +100° longitude offset, speed/course, base-91 altitude, the
standard and custom messages); objects (live / killed), items; weather (with a position or positionless, every field —
shown in °C, km/h, mm, hPa); messages, acks, bulletins (`BLNn`), telemetry, status (with or without a timestamp),
capabilities, raw NMEA (`$GPRMC`/`$GPGGA`), third-party (`}` — the inner packet parsed as its own source, "via" the
gateway). Off-air text is cleaned to valid UTF-8 (Latin-1 comments are common) before it meets JSON.

The run applies the **APRS duplicate rule** (same source + information field within 30 s = one packet); a digipeated
copy only records the digipeater that carried it (`via`, the last `*`-marked call, the station before a `WIDEn` alias).
Stations kept 6 h, at most 500; tracks 120 points (a point per 20 m); the last 60 messages.

### Decoder contract

Attach `{"type":"audio_extension_attach","extension_name":"aprs"}` (+ `"baud_rate":300` for HF). It takes a decoder
slot like any other and the owner's `aprs` switch blocks it. Frames (JSON on the decoder socket):
`{"type":"aprs_packet","data":{key,from,kind,line,raw,lat?,lon?,to?,text?}}` per new packet;
`{"type":"aprs_list","data":{baud,frames,repaired,dupes,stations:[…],messages:[…]}}` every 2 s when it changed, and at
once (with each station's track) to a second window on the same decoder. A station: `key (call-SSID, or obj:NAME) from
kind name? sym (table+code, only once a position was heard) symName lat lon course speed(km/h) alt(m) comment status
micE path via raw line wx{tempC,hum,windDir,windKmh,gustKmh,rain1hMm,rainTodayMm,hPa} killed msg seen age count posAge
trail?`. Capability: `"aprs": true|false` and `"aprsHz"` — the region's channel from the server's position (the
Americas 144.390, Brazil 145.570, Argentina 144.930, Australia 145.175, New Zealand 144.575, Japan/China 144.640,
South Korea 144.620, Thailand 145.525, elsewhere 144.800).

### The web client

APRS is in the DECODERS menu (where the server offers it). Its settings: PACKET VHF 1200 / HF 300 and a TUNE key (the
region's channel in NFM, or 10.1476 MHz USB). The box lists the stations, newest first (colour by kind, the readable
line, range, last heard), the messages, and the packets as they arrive. **MAP** opens the same full-screen map as AIS
with each station's symbol (our own drawings; the overlay character of the alternate table on the badge, a dashed ring
for the alternate table), its track (from the list, then grown from each packet), and a card: symbol, Mic-E message,
speed and course, altitude with ▲/▼ (a balloon climbing), weather, comment/status, position and range, path and the
digipeater it came via, packet count and the raw packet. A row in the box opens the map on that station.

## The gallery

`vibe_station_log.h`: per UTC day, an `ais` (or `aprs`) **log** — one line per message that changed what the list says
("12:01:13  WILSON GOOLE · cargo · 12 kn SW → ROTTERDAM · MMSI 235012345 · 50.8123 -1.3099"; a moving ship at most once a
minute, a new name at once; APRS: every new packet, with its position and digipeater) — and a **summary card** of every
station heard that day ("AIS 2026-10-26 · 31 ships and stations: 22 class A ships, 6 class B vessels, 3 aids to
navigation"), rewritten at most once a minute, last line of each station, most heard, newest. Written every minute or
8 KB and when the decoder stops, never per message. Labelled live / background like every gallery item. Everything in
AIS and APRS is a public broadcast by design, so there is no privacy filter (unlike ISM).

## Tests (numbers on this Mac, 2026-10-26)

| Suite | Checks | Covers |
|---|---|---|
| `vibeserver/test-ais.cpp` | 51 | the FCS check value; repair of one weak level, refusal of a confident one and of two errors; the **gpsd AIVDM worked examples** (type 1 MMSI 477553000; type 5 EVER DIADEM / 3FOF8 / NEW YORK) decoded exactly; every type encoded from Annex 8 and back; the front end at 2.4 / 2.048 MS/s / 250 kS/s with crystal offsets; the Eb/N0 sweep; **10 minutes of noise: 0 frames**; the service (merge, line, JSON, track) |
| `vibeserver/test-aprs.cpp` | 52 | AX.25; **the APRS 1.0.1 worked examples** (plain/compressed positions, PHG, ambiguity, weather both ways, object, item, message, ack, bulletin, status, altitude, `!DAO!`), NMEA, third-party; Mic-E (the spec's `S32U6T` destination, +100°, altitude); the modem at 4–20 dB flat and with the tones ±6 dB apart; HF 300 Bd mistuned; **20 minutes of noise (both modems): 0 frames**; the run (duplicates, via, track, messages, weather in metric, Latin-1) |
| `scripts/test-server-ais-aprs.mjs` | 23 | the real server on fake rtl_tcp clips: capability, AIS on/off with the dial given back, both channels, merged rows, a track, a tune ignored, the gallery's AIS log + card; the APRS decoder on NFM: four stations (car, weather, object, message), duplicates, the gallery's APRS items |
| `scripts/test-web-ais-aprs.mjs` | 22 | headless Edge (`--mute-audio`) at 1280 and 390 px: the AIS menu entry, the ship map, rows, markers, a card, the list fitting a phone, no sideways scroll, EXIT AIS; the APRS button, rows, messages, packets, MAP, a weather card |

**AIS decode rate vs Eb/N0** (2.4 MS/s capture, 120 frames — a third of them 424-bit type 5s, both channels, 1.2 kHz
crystal offset; frames counted through the service's rule):

| Eb/N0 | 8 dB | 10 | 11 | 12 | 13 | 14 | 16 | 18 |
|---|---|---|---|---|---|---|---|---|
| no repair | 0 % | 1.7 | 6.7 | 19.2 | 50.0 | 74.2 | 96.7 | 99.2 |
| with repair | 0 % | 1.7 | 9.2 | 25.0 | 62.5 | 85.8 | 99.2 | 99.2 |
| wrong frames | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

**APRS 1200 Bd decode rate** (40 packets each; SNR in a 3 kHz audio band; flat / space tone +6 dB / space −6 dB): 4 dB
0/2/0, 6 dB 7/34/0, 8 dB 37/40/0, 10 dB 40/40/8, 14 dB 40/40/40, 20 dB 40/40/40 — never a wrong frame. Against a single
balanced slicer on the same audio (6/8/10 dB × flat/±6 dB, 360 packets): **one slicer 134, five slicers + repair 203**.
HF 300 Bd at 12 dB in 500 Hz: 12/12 on tune, 11/12 at −50 Hz, 12/12 at +30 Hz.

**CPU** (measured on this Mac, -O2, one core): AIS 0.6 % at 2.4 MS/s, 0.5 % at 2.048 MS/s; APRS 1200 Bd 0.07 %, 300 Bd
0.16 %. **Pi 2 (estimated, not measured):** a Cortex-A7 at 900 MHz runs these NEON float loops ~15–25× slower than one
Mac core, so AIS ≈ 9–15 % of one core at 2.4 MS/s (the shared FIR stages are ~80 % of it — a lower capture rate costs
proportionally less), APRS ≈ 1–2 %. Both are behind drop-never-wait queues registered with the health pill
(`BehindWatch`: "AIS", "APRS").

**RAM:** AIS's queue holds at most half a second of capture (2.4 MS/s × 8 B × 0.5 s = 9.6 MB worst case, normally near
empty); 1000 ships × 360 track points × 24 B = 8.6 MB at the very worst (a busy coast with every ship moving for an
hour), typically < 1 MB. APRS: < 1 MB (500 stations × 120 points). Trade-off stated: the AIS track costs RAM so the card
can draw an hour of a ship's path the moment it is tapped; cutting `kTrailMax` halves it.

**Real signals:** Stuart's UberSDR covers HF only, so **no real VHF AIS or 1200 Bd APRS could be tested here** — the
engines are proven on the published worked examples (gpsd AIVDM, APRS 1.0.1) and synthetic signals built from the
standards. **HF APRS (300 Bd) is real:** ten minutes of 30 m (10.1476 MHz USB) recorded from the UberSDR on 2026-10-26
(`~/VibeSDR-test-audio/aprs/hf-aprs-30m-10147.6-usb.wav`, 14 MB) decode to **2 packets, both from F5NVY** (an HF
gateway beacon, compressed position, and a Mic-E jeep, "Off Duty") — **a single balanced slicer on the same audio finds
0**: the extra tunings and balances are what caught them. Replay: `test-aprs --wav <file> 300`.

## Not done / owed

- **The app** (React Native) — BUILT 2026-10-26 (APP-PARITY-2026-10-26.md): the AIS key beside ISM (absent / dimmed
  with why / live, never on Apple TV), the ship map on the shared track layer with TrackLiveList, EXIT AIS, the audio
  released; APRS in CLIENT DECODERS (PACKET + TUNE, AprsView, the station map). `src/services/tracks.ts` mirrors the
  web's rules (scripts/test_ais_aprs_app.ts). Owed: a device run; an AIS box for Apple TV.
- **Background decoding:** APRS works as a background VFO's decoder through the host (its gallery tap is wired), but the
  setup page's decoder list for a VFO does not offer APRS yet, and an **AIS background profile** (as ISM's `"ism"`
  profile) is not built — `bgAis_` is in place and always false.
- **The setup page's "Modes and decoders" switches** do not list AIS and APRS yet; the server honours `ais`/`aprs` in
  `blockedModes` already.
- APRS on the **GPU map** draws via the Leaflet-shaped adapter (`vibemapgl-compat.js` gained `polyline`); only the Leaflet
  fallback was exercised in the headless tests (no map pack in the test server).
- AIS on a radio whose capture is far below 162.000 (e.g. an owner's tuning offset) is handled by the front end's NCO
  but was only tested centred.
