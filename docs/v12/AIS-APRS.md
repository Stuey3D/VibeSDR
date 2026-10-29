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
ITU's (M.585 / MARS) — `vibe_maritime_mid.h`, the ONE table DSC and AIS share. The coherent AIS detector is built from
textbook methods: Laurent's decomposition of CPM (IEEE Trans. Commun., 1986) for the matched filter, and data-free
carrier estimation by squaring (the classic non-data-aided estimator, applied to MSK's two-axis structure). GPL decoders were read for behaviour only; no code, table or structure was
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
- **AFC — the dongle's crystal (2026-10-29).** An RTL dongle without a TCXO is commonly 20–60 ppm off: 3–10 kHz at
  162 MHz, which pushes a burst against or past the ±7 kHz channel filter. Before: +6 kHz lost three quarters of the
  frames at 14 dB and **−9 kHz decoded nothing at all, even at 20 dB** — the likeliest single cause of a listener's
  "only one ship". Now each burst's mean frequency (a run of samples 3× over the floor, 120–1400 bits long — GMSK's
  spectrum is symmetric, so its mean IS the carrier) moves one shared correction half way; it is applied to each
  channel's NCO between that channel's bursts, never mid-burst, up to ±12 kHz. −9 kHz settles within 100 Hz in ~10
  bursts; the service keeps the value across restarts (`seedAfc`) and reports it (`afc`, Hz — 162 Hz = 1 ppm).
- **Demodulator: COHERENT (2026-10-29, `CohPath`).** Written from the textbook: MSK/GMSK at a bit boundary sits on an
  axis (θ + k·π/2), so after turning sample k back by k quarter-turns the signal is ±A·e^{jθ}, and its SQUARE carries no
  data. Per sampling phase:
  1. a receive filter **matched to GMSK's main Laurent pulse C0** (Laurent, IEEE Trans. Commun. 1986; BT 0.4, ~99 % of
     the energy) gives the symbol sample;
  2. the carrier's **frequency** is the turn of the squared channel-filtered sample **two bits apart** (one bit apart is
     biased by GMSK's shrunken swing on alternating levels — it jumped 2400 Hz in every type 5's name padding), averaged
     over ±24 bits; its 2400 Hz alias is resolved by the mean phase step over the same window;
  3. the carrier's **phase** is half the angle of the summed squares over ±6 bits, followed continuously (no false π);
  4. the symbol is the real part, ±1, and the NRZI data bit is symbol k against symbol k−2 (`vibehdlc` `pairDist = 2`), so
     the π ambiguity left by squaring does not matter.
  Data-free, so no training-sequence search, no loop to lose lock, and a burst's ramp-up or a synthesiser still
  settling is followed within the window. The squares are taken of the channel-filtered sample, not the matched filter's
  output: the matched filter puts ~45 % of the neighbours in quadrature, which the real part ignores but a square does
  not. Windows (W ±6, Wf ±24 bits) and four phases are the measured best (the doc's tables; `vibeais::Tuning`).
- **Four fixed sampling phases, no PLL.** A frame is at most ~1000 bits and a transmitter's clock is good to a few ppm,
  so the bit timing cannot drift a tenth of a bit inside one: only the phase is unknown. Four phases a quarter of a bit
  apart, each with its own detector and deframer. (5 gained ~5 % of frames at 9 dB for 20 % more CPU; 3 lost ~15 %.)
- **The discriminator** that was the receiver until 2026-10-29 (polar discriminator, post-detection low-pass, a
  peak/valley DC tracker restarted by a rise in power) is still in `GmskDemod` behind `Tuning::disc`, **off**: run
  beside the coherent detector it added one frame in 600 (the compound case at 16 dB) for as much CPU again.
- **The best copy, not the first:** the phases close a frame within about a bit of each other; it is held two bits and
  the cleanest copy (unrepaired, highest mean |soft|) is kept.
- **Repair:** on a bad FCS the 8 weakest symbols are flipped one at a time (one wrong symbol is two wrong NRZI bits —
  `vibe_hdlc.h`), the frame re-decoded, and accepted only if its length fits its type. ★★★ **A repaired frame needs a
  second witness** (2026-10-29): another deframer must produce the same bytes within W + Wf + 6 bits, or it is dropped.
  The harsher test caught the old rule out — a type 5 of a KNOWN ship with a wrong field (the error was past the MMSI,
  so "the MMSI must be known" let it in; the old receiver produced one too in the compound case). A chance FCS match is
  not reproduced by a neighbouring phase; a real single error is. ★★★ The service's rule still applies on top: **a
  repaired frame must name an MMSI already heard cleanly** (`Service::accept_`).

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
capture, noise, afc (Hz: the radio's crystal error at 162 MHz as the AFC found it), gain?, gainAuto?, vessels:[…], safety:[…]}` once a second and at once to a joiner (its arrival means AIS
is on); `{"type":"ais_off"}` then a `config`; `{"type":"ais_error","why"}`; `{"type":"ais_trail","mmsi","pts":[[lat,lon,ageSec]…]}`.
A vessel: `mmsi kind(A|B|base|aton|sar|sart) name call dest imo flag country stype stypeName family nav navName aton
atonName virtual offpos lat lon sog cog hdg rot|turn draught len beam eta alt trend(+1/−1 — the card's ▲/▼) line ch
level snr seen age posAge count trail`.

**Gain:** ISM's rule (`vibeism::GainRule`) on the radio's own gain list, **started 85 % up** (2026-10-29; it was 60 %,
ISM's): down a rung when > 2 % of samples clip, back up (to the start) after two quiet minutes. A listener's gain pauses
it; AUTO resumes it. ★ On an R820T 60 % was 32.8 dB, where the dongle's own noise and its 8-bit converter still sit
near the aerial's and a far ship's burst is lost under them; 85 % is 43.4 dB, the region AIS is normally received in.
The digital AGC stays off (it would chase the bursts).

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

## Tests (numbers on this Mac, 2026-10-26; AIS receiver 2026-10-29)

| Suite | Checks | Covers |
|---|---|---|
| `vibeserver/test-ais.cpp` | 84 | the FCS check value; repair of one weak level, refusal of a confident one and of two errors; the **gpsd AIVDM worked examples** (type 1 MMSI 477553000; type 5 EVER DIADEM / 3FOF8 / NEW YORK) decoded exactly; every type encoded from Annex 8 and back; the front end at 2.4 / 2.048 MS/s / 250 kS/s with crystal offsets; the Eb/N0 sweep; **the real-world conditions table** (below; `--conditions` for all of it); **10 minutes of noise: 0 frames**; the service (merge, line, JSON, track). Replays: `--cu8 <file> <rate>` (an rtl_sdr capture centred on 162.000), `--wav <file> <Hz>` (one channel, 16-bit IQ WAV; `AIS_WAV_NOISE=σ` adds noise) |
| `vibeserver/test-aprs.cpp` | 52 | AX.25; **the APRS 1.0.1 worked examples** (plain/compressed positions, PHG, ambiguity, weather both ways, object, item, message, ack, bulletin, status, altitude, `!DAO!`), NMEA, third-party; Mic-E (the spec's `S32U6T` destination, +100°, altitude); the modem at 4–20 dB flat and with the tones ±6 dB apart; HF 300 Bd mistuned; **20 minutes of noise (both modems): 0 frames**; the run (duplicates, via, track, messages, weather in metric, Latin-1) |
| `scripts/test-server-ais-aprs.mjs` | 23 | the real server on fake rtl_tcp clips: capability, AIS on/off with the dial given back, both channels, merged rows, a track, a tune ignored, the gallery's AIS log + card; the APRS decoder on NFM: four stations (car, weather, object, message), duplicates, the gallery's APRS items |
| `scripts/test-web-ais-aprs.mjs` | 22 | headless Edge (`--mute-audio`) at 1280 and 390 px: the AIS menu entry, the ship map, rows, markers, a card, the list fitting a phone, no sideways scroll, EXIT AIS; the APRS button, rows, messages, packets, MAP, a weather card |

**AIS decode rate vs Eb/N0** (2.4 MS/s capture, 120 frames — a third of them 424-bit type 5s, both channels, 1.2 kHz
crystal offset; frames counted through the service's rule). Good non-coherent AIS receivers are quoted at 50 % around
9–10 dB; the coherent detector puts it at ~8.5 dB.

| Eb/N0 | 8 dB | 10 | 11 | 12 | 13 | 14 | 16 | 18 |
|---|---|---|---|---|---|---|---|---|
| before (discriminator, 2026-10-26), with repair | 0 % | 1.7 | 9.2 | 25.0 | 62.5 | 85.8 | 99.2 | 99.2 |
| **coherent (2026-10-29)**, no repair | 31.7 % | 94.2 | 97.5 | 100 | 100 | 100 | 100 | 100 |
| **coherent**, with repair | 31.7 % | 95.0 | 97.5 | 100 | 100 | 100 | 100 | 100 |
| wrong frames (both) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

The repair now adds little (the coherent detector rarely leaves exactly one weak symbol wrong); with its second-witness
rule it never costs a frame and has never let a wrong one through.

**The real world, harsher** (`test-ais --conditions`; 60 frames a cell, % decoded, repair on, the service's rule; each
run starts with 16 stronger bursts from other ships so the AFC has settled, as it long has on a running receiver — a
cold start costs ~10 bursts). Crystal = the receiving dongle's error; "tx" = each transmitter ±500 Hz with an 8-bit
power ramp (M.1371 Annex 2 allows up to 8) and a synthesiser still settling (+1 kHz decaying over 3 bits); 2nd path = a
second ray 6 dB down and a quarter of a bit late (its energy is not counted in Eb/N0, hence the head start at 8 dB);
adjacent = FM carriers 40 dB over the burst on 161.950 and 162.050; previous slot = a ship 15 dB stronger ending 2 ms
before, same channel; all = everything at once with a 2.5 kHz crystal.

| condition | before: 8 dB | 9 | 10 | 11 | 12 | 14 | 16 | 20 | **after: 8 dB** | **9** | **10** | **11** | **12** | **14** | **16** | **20** |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| plain (1.2 kHz) | 0 | 0 | 0 | 10 | 25 | 82 | 97 | 100 | 43 | 72 | 97 | 98 | 100 | 100 | 100 | 100 |
| crystal −3 kHz | 0 | 0 | 0 | 5 | 33 | 82 | 100 | 100 | 43 | 67 | 88 | 100 | 100 | 100 | 100 | 100 |
| crystal +3 kHz | 0 | 0 | 0 | 5 | 42 | 87 | 100 | 100 | 45 | 72 | 97 | 97 | 100 | 100 | 100 | 100 |
| crystal +6 kHz | 0 | 0 | 0 | 0 | 3 | 20 | 78 | 98 | 47 | 82 | 93 | 98 | 100 | 100 | 100 | 100 |
| crystal −9 kHz (55 ppm) | 0 | 0 | 0 | 0 | 0 | **0** | **0** | **0** | 47 | 70 | 92 | 100 | 100 | 100 | 100 | 100 |
| tx ±500 Hz, ramp, settle | 0 | 0 | 2 | 8 | 30 | 87 | 100 | 100 | 37 | 70 | 97 | 98 | 100 | 100 | 100 | 100 |
| 2nd path −6 dB, ¼ bit | 5 | 8 | 3 | 47 | 52 | 67 | 70 | 100 | 60 | 65 | 68 | 85 | 88 | 98 | 100 | 100 |
| adjacent FM +40 dB | 0 | 0 | 0 | 10 | 33 | 85 | 100 | 100 | 45 | 70 | 90 | 100 | 100 | 100 | 100 | 100 |
| previous slot +15 dB | 0 | 0 | 2 | 8 | 30 | 67 | 98 | 98 | 40 | 72 | 90 | 98 | 100 | 100 | 100 | 100 |
| all of it (+2.5 kHz) | 0 | 5 | 13 | 18 | 45 | 62 | 78 | 98 | 60 | 62 | 60 | 70 | 73 | 100 | 97 | 100 |

Wrong frames: before, **one** (the "all of it" row — a repaired type 5); after, none in any cell.

What the harsher test found in the old receiver, in order of likely harm to a listener with an RTL: a dongle 6–9 kHz
off loses most or ALL ships (fixed: AFC); the start gain of 60 % on an R820T (fixed: 85 %); a repaired frame of a known
ship with a wrong field (fixed: the second witness); and ~4 dB of sensitivity (fixed: the coherent detector). Multipath
of a quarter bit is the case left weakest — an equaliser (MLSE over the channel) would be the next step, at a real CPU
cost, and VHF sea paths rarely spread that far.

**On a real recording.** The only freely downloadable AIS IQ found is the Signal Identification Wiki's
[`AIS_IQ.zip`](https://www.sigidwiki.com/wiki/File:AIS_IQ.zip) (uploaded 2016-01-04: a 42.9 s 16-bit stereo IQ WAV at
48 kS/s, one channel at its centre, six stations incl. HOHEBANK). **Its file page states no licence**, so it was used
on this Mac only to test against and is **not** in the repo; nothing else searched (SigMF examples, Zenodo, IEEE
DataPort — whose AIS sets are decoded NMEA, not IQ) had IQ with a usable licence. `test-ais --wav "AIS IQ.wav" 0`, with
white noise added on top (σ per dimension, full scale 1):

| added noise σ | 0 | 0.002 | 0.003 | 0.004 | 0.005 | 0.007 | 0.01 |
|---|---|---|---|---|---|---|---|
| before (discriminator) — frames / stations | 11 / 6 | 12 / 6 (3 repaired) | 5 / 3 | 2 / 1 | 0 | 0 | 0 |
| **coherent** — frames / stations | **12 / 6** | 12 / 6 | 11 / 6 | 6 / 5 | 5 / 4 | 2 / 1 | 1 / 1 |

About 3 dB more noise for the same ships on real bursts — the synthetic gain, seen on air. Every frame is a genuine
station (the same six MMSIs as the clean file); no wrong frames.

**Wanted: a capture from a listener's own RTL** (it would test the AFC and the gain on the very dongle). The request:

> Could you record 60 seconds of raw IQ for us? Stop VibeServer first (it holds the dongle), then run
> `rtl_sdr -f 162000000 -s 288000 -g 43.4 -n 17280000 ais.cu8` (162.000 MHz is exactly between the two AIS channels;
> 288 kS/s for 60 s; gain 43.4 dB), start VibeServer again, and send us `ais.cu8` (~35 MB — a file-sharing link is
> fine). If you can, say roughly where the aerial is and how many ships your other decoder showed in the same minute.
> We will only use it to test the decoder, and only keep it in the repo if you say so.

It is replayed with `test-ais --cu8 ais.cu8 288000` (prints the frames, the stations and the dongle's error in ppm).

**APRS 1200 Bd decode rate** (40 packets each; SNR in a 3 kHz audio band; flat / space tone +6 dB / space −6 dB): 4 dB
0/2/0, 6 dB 7/34/0, 8 dB 37/40/0, 10 dB 40/40/8, 14 dB 40/40/40, 20 dB 40/40/40 — never a wrong frame. Against a single
balanced slicer on the same audio (6/8/10 dB × flat/±6 dB, 360 packets): **one slicer 134, five slicers + repair 203**.
HF 300 Bd at 12 dB in 500 Hz: 12/12 on tune, 11/12 at −50 Hz, 12/12 at +30 Hz.

**CPU** (measured on this Mac, -O2, one core): AIS 0.6 % at 2.4 MS/s, 0.6 % at 2.048 MS/s; APRS 1200 Bd 0.07 %, 300 Bd
0.16 %. The demodulators alone (both channels at 50 kS/s, noise, best of 3): **25 ns a channel sample before, 33 ns
now** (+8 ns: +0.08 % of a Mac core) — the coherent detector's frequency is worked out every 4th bit and its sums are
float; 5 phases would be 39 ns, coherent + discriminator 61 ns. **Pi 2 (estimated, not measured):** a Cortex-A7 at
900 MHz runs these float loops ~15–25× slower than one Mac core, so the coherent detector adds **≈ 1.2–2 % of one Pi 2
core**, and AIS as a whole is ≈ 10–17 % at 2.4 MS/s (the shared FIR stages are most of it — a lower capture rate costs
proportionally less), APRS ≈ 1–2 %. Both are behind drop-never-wait queues registered with the health pill
(`BehindWatch`: "AIS", "APRS").

**RAM:** AIS's queue holds at most half a second of capture (2.4 MS/s × 8 B × 0.5 s = 9.6 MB worst case, normally near
empty); 1000 ships × 360 track points × 24 B = 8.6 MB at the very worst (a busy coast with every ship moving for an
hour), typically < 1 MB. APRS: < 1 MB (500 stations × 120 points). Trade-off stated: the AIS track costs RAM so the card
can draw an hour of a ship's path the moment it is tapped; cutting `kTrailMax` halves it.

**Real signals:** Stuart's UberSDR covers HF only, so no VHF AIS could be RECORDED here; AIS has been tested on one
real recording (above; no licence, local only) and 1200 Bd APRS on none — otherwise the engines are proven on the
published worked examples (gpsd AIVDM, APRS 1.0.1) and synthetic signals built from the standards. **HF APRS (300 Bd) is real:** ten minutes of 30 m (10.1476 MHz USB) recorded from the UberSDR on 2026-10-26
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
  but was only tested centred. ★ Not checked: an owner's CONVERTER offset (`convOffsetHz`, e.g. an upconverter) is
  applied by `tuneHw` to the AIS centre as well, and `vsAisWhyNot` does not look at it.
- A listener's own RTL capture (the request above) — the AFC and the 85 % gain have not met a real dongle yet.
- Multipath of a quarter bit or more is the weakest case left (an equaliser would be the next step).
