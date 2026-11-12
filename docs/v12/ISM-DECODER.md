# ISM sensors (V12): readable weather stations, sensors, doorbells, tyres

VibeServer's answer to OpenWebRX's "ISM" (rtl_433) mode, but **human-readable**:

```
Oregon Scientific THGR810 / THGN801 · 18.4 °C · 62 % RH · battery OK · ch 2
Fine Offset WH1080 · 12.5 °C · 90 % RH · wind 4 km/h S · gust 7 km/h · rain 23.4 mm · battery OK
TPMS #1 · Schrader · 32 PSI · 18 °C
Key fob #1 · KeeLoq · button 2 · battery OK
Doorbell / remote #1 · button A
```

Branch `v12-ism` (from `v12-experimental`). Decoder: `android/app/src/main/cpp/vibe_ism.h` (front end),
`vibe_ism_protocols.h` + `vibe_ism_protocols2.h` (protocols), `vibe_ism_service.h` (bands, privacy, list, JSON, thread).
Server: `local_sdr_shim.cpp` (ISM MODE block beside ADS-B's). Web: `web/client/src/ism.ts`, wiring in `main.ts`,
`spectrum.ts`, `mobile.ts`. Tests: `vibeserver/test-ism.cpp`, `scripts/test-server-ism.mjs`, `scripts/test-web-ism.mjs`.
Replay tool: `vibeserver/ism-offline.cpp`.

## ★★★ Clean room — no GPL code

rtl_433 is GPL-2. **No rtl_433 source, and no other GPL decoder source (OWRX, gr-*, pilight, RFLink), was read.** Every
decoder was written from a published description of the protocol — a datasheet, a paper, a protocol write-up — cited
above the decoder in the source and in the table below. Protocol facts are not copyrightable; code is. A protocol whose
only description is GPL source code is listed as "not done: no non-GPL description". The DSP (Hann-windowed FFT power, a
censored-mean noise floor, an NCO, a triangular decimator, a polar discriminator, hysteresis slicing, SipHash) is
textbook. Where a write-up left a detail open, the choice is marked **ASSUMED** in the code and in the table — each is a
thing a real capture will confirm or correct.

**★ From the weather pass (2026-11-11), with Stuart's agreement:** rtl_433's device files (`src/devices/*.c`) MAY be
read for protocol **facts** — frame layout, timing, sync, check polynomial / init / coverage, scaling — and were, for the
families in `vibe_ism_weather2.h` … `vibe_ism_weather10.h` and the fixes to the phase-1 weather decoders. No code, table
or identifier was copied: every decoder is ours, on our own slicers (`vibe_ism_wxkit.h`). Independent public write-ups
are cited first wherever one exists (the Oregon "RF Protocols II" PDF, the tfd.hu Auriol/Alecto PDF, ala-paavola.fi's
WT450H page, andreafabrizi's Bresser 5-in-1 notes, the f6fbb.org TX3 page, Ray Wang's AcuRite posts…). Each family was
scored on the rtl_433_tests recordings (`scripts/ism-corpus.mjs` — test input only, never in the repository).

**TPMS, 2026-11-11 (Stuart: *"we dont want to copy but if we can take inspiration"*).** For the tyre sensors
(`vibe_ism_tpms.h`) rtl_433's device notes WERE read — for the FACTS of each frame only (bit timing, modulation, sync,
field layout, check polynomial / start, scaling), most of which exist nowhere else. No code, table, comment or
identifier was copied: the framing engine and every decoder are our own, checked against real recordings. Each family
names its source above itself and in the table. The paragraph above still describes every decoder written before then.

**★ 2026-11-11 — the rule widened (Stuart: "we dont want to copy but if we can take inspiration … only decoding 3% is
pityful").** For the families that have NO other public description, the protocol notes at the head of rtl_433's device
files (and its `conf/` format descriptions) may be read for FACTS — layout, timing, line code, sync, check polynomial,
scaling — never code, tables, comments or identifiers; every decoder is still our own code on our own slicers, and each
cites where its facts came from (a datasheet, patent, FCC report or write-up first where one exists). rtl_433's test
recordings (`rtl_433_tests`) are used as test INPUT only, outside the repository — `scripts/ism-corpus.mjs`.

## Architecture — a whole-radio mode, not a passband decoder

**Decision: a whole-radio mode, ADS-B's shape** (save the listener's state, take the radio onto the band, give it back
exactly as found). Why not a decoder on the listener's DDC channel:

- Sensors are scattered: a 433 MHz sensor's crystal puts it anywhere in ±100 kHz of 433.92 and the band itself is
  433.05–434.79; 868 has two busy channels 650 kHz apart. A passband decoder hears one sensor in a dozen and makes the
  listener tune — exactly what Stuart found hard on OWRX ("I always found it quite hard to tune the ISM on OWRX").
- One capture of ≥ 0.9 MS/s holds the whole band; a cheap detector finds each burst where it is.
- It follows the ADS-B contract every client already speaks (capability, on/off, a report a second, `*_off`, `*_error`).

**No rate change, ever** (ADS-B's rule after the 32-bit re-rate teardown): the mode runs at whatever the radio captures,
≥ 0.9 MS/s. A faster capture is pre-decimated (an Airspy at 10 MS/s → 2.5 MS/s by an integer boxcar).

### The front end (`vibe_ism.h`)

1. **Detector** — a 256-point Hann FFT of one block every 320 µs (on every machine since 2026-10-31; 32-bit ARM used 640 µs before, and missed decodes), power per bin against a
   **censored-mean noise floor per bin** (a minimum-tracker settles at a fraction of an exponential mean and calls one bin
   in ten a signal). Bins ≥ 10 dB over their floor are grouped (one-bin gaps bridged); a lone bin must be ≥ 15 dB.
   Only the **band's own span** is searched (the tuner's filter skirts are where false detections clustered on the
   Pi 500's RTL), never the DC remnant (±15 kHz).
   **The seed and the look back (2026-11-12).** The first 64 frames (~20 ms at 2.4 MS/s, ~65 ms at 250 kS/s) are only
   STORED (64 KB, freed at once). Each bin's floor is then the **median** of its frames ×1.44 (an exponential's mean),
   and the stored frames are **replayed through the detector at their own sample positions** — a burst already on the
   air when ISM starts opens its channel from its own start (the mixer's ring still holds it). The old seed, a fast
   running mean while detecting, learnt such a burst as floor — and, under a strong one, the RTL's gain dip (every
   other bin ~18 dB down: the emonTx recordings), so every bin stood 10 dB over its floor after it. Not a lower
   quantile: the 25th/12th percentile learnt that dip and lost every emonTx. A burst filling over half the window would
   still be learnt, so a **busy stretch** — ≥ 3 adjacent bins ≥ 10 dB over the band's median seed whose power MOVES
   across the window (75th/25th percentile ratio ≥ 15; noise ~4.8, a steady spur ~1) — takes the band's floor; a spur
   keeps its own and opens nothing. Live, this is the first instant after ISM starts.
2. **Candidate → channel** — a detection is a candidate until seen in a second frame; then a channel opens at the
   group's power centroid, **from a 6 ms pre-roll** kept in a 2¹⁸-sample ring, so the burst's start is not lost.
   Strongest first; a newcomer 25 dB under an open burst is its splatter (a strong OOK transmitter's keying edges).
   Up to 4 channels at once. A channel closes 25 ms after the last detection (PT2262's sync gap is ~15 ms), or at 1.6 s.
   (2026-11-12) A sighting goes to the **nearest** open channel within 120 kHz (the first in slot order stole a second
   burst's sightings). A sighting **20 dB over** everything an open channel has seen, > 10 kHz from its mix centre,
   **takes it over**: the noise channel is cancelled and the burst opens its own, keeping the old start (≤ 50 ms back)
   — a promoted noise blip used to swallow a burst 30–60 kHz away and mix it off-centre (oil gauges, Fine Offset WH2).
   **Wide-deviation FSK is one channel** (±60–90 kHz: TPMS, an RFM12 emonTx at ±90 kHz): two groups BORN in the same
   frame (no channel near either), each ≥ 15 dB over its floor, within 6 dB of each other and 100–200 kHz apart are one
   sighting at their midpoint; a channel born on one tone is re-centred once (start kept) when the other tone is born
   within 20 ms while it is on the air (a preamble may hold one tone ~4 ms). Only births pair: a 10 dB / 40 kHz rule
   paired an OOK burst with the DC remnant its keying moves, and two oil gauges sending at once (−69 on the corpus).
3. **Channel** — NCO mix and a **triangular (CIC-2) decimator** to ~250 kS/s. (A plain integrate-and-dump leaked a
   strong burst one output rate away onto DC: test-ism's TPMS decoded twice.) A wide pair whose half-spacing is over
   0.22 of the channel rate is **mixed at twice the rate** (~500 kS/s): ±90 kHz tones at 250 kS/s turn the phase
   ~2.2 rad a sample and noise wraps it past π. The slicer works in µs, so only that channel's cost changes.
4. **Slicer, at the burst's end** — the envelope (OOK) and the discriminator (FSK, when the envelope is ≥ 85 % on
   across the burst and the tones are ≥ 4 kHz apart), each sliced with hysteresis at the dB midpoint between the
   pre-roll's level and the burst's, **at four smoothings** (12, 36, 100, 244 µs for OOK; three for FSK). A 17 kbps FSK
   bit and a 1 kHz Oregon pulse want very different filters; the decoders try each, finest first. Output: signed run
   lengths in µs (+mark / −space). The burst's carrier (or the tones' midpoint) refines the reported frequency.
   A burst under 6.5 dB, or with > 6000 transitions, is not decoded (noise reads ~4.5 dB — measured).
5. **Bit slicers** (shared by every protocol): PWM, PPM, NRZ, Manchester (both conventions, re-phasing on a violation),
   differential Manchester; CRC-8 / CRC-16 (MSB-first) and the helpers. **The weather kit** (`vibe_ism_wxkit.h`,
   2026-11-11) adds the lenient forms real sensors need — a run is short or long by which side of the midpoint it
   falls, only a run past the gap limit ends a row (a stretched pulse no longer costs the frame); PPM can absorb noise
   slivers inside a space; PCM (NRZ / RZ), "first edge = 0" Manchester, biphase (DMC), repeated-row search, CRC-4,
   reflected CRC-8/16, the LFSR digests (8 / 8-reflected / 16), and `fskRows` — NRZ from the discriminator AND from a
   tone's own envelope (wide-deviation FSK opens a channel per tone: each is OOK of one tone).

### The protocols (`vibe_ism_protocols*.h`) — see the coverage table below

`decodeAll` tries every decoder on every smoothing; the first that accepts a burst wins. Weak checks are marked
`confirm` and are shown only once heard twice alike within 90 s (the Tracker).

### The service (`vibe_ism_service.h`)

Its own thread, fed by copy from the DSP loop through a half-second queue that drops the OLDEST block (Network > Audio >
Spectrum > Decoders). The Tracker keeps one row per device: newest first, repeats within 3 s folded, sensors kept 2 h
after last heard, tyres/fobs/remotes 15 min, at most 300. The live `capture N %` is ADS-B's CaptureMeter.

## The bands — the tuning controls step BANDS (Stuart, 2026-10-25)

| id | readout | centre | the radio is tuned to | holds |
|---|---|---|---|---|
| `315` | 315 MHz ISM | 315.00 | +40…250 kHz (rate-dependent) | US/Japan TPMS, fobs, remotes |
| `433` | 433.92 MHz ISM | 433.92 | 434.13 at 2.4 MS/s (51 kHz above at 2.048) | 433.05–434.79: EU/UK weather, sensors, doorbells, TPMS |
| `868` | 868 MHz SRD | 868.60 | 868.60 (DC between the two channels) | 868.3 + 868.95: LaCrosse IT+, Fine Offset FSK, Bresser |
| `915` | 915 MHz ISM | 915.00 | +40…250 kHz | US LaCrosse, Ambient, Fine Offset 915 |

- The radio sits a little **above** the band's middle so its DC remnant misses the busy channel, never so far that the
  band's far edge leaves the capture (`ismHwCentre`). The readout shows the band's centre and name, never the offset.
- **Each tune step** — the arrows, the drum, the keyboard's tune keys, a pad's tune — **moves ONE band** among those
  this radio reaches, **clamped** at the ends (as DAB's blocks). The arrows' captions name the next band.
- **An exact frequency**: typing in the frequency box (the app: also a long press) puts the capture on that frequency
  for an odd device — `{"type":"ism","on":1,"hz":N}`, or a plain `tune` (old clients); the list is kept.
- **The server owns the band** (the shared-dial contract): a client sends a change only on a user action; every
  listener's view follows the report.
- **Landing band** = the one nearest the server's location (`ismLanding`): the Americas (longitude −170…−25) → 315 MHz
  (TPMS and fobs), else 433.92; then whichever this radio reaches. ⚖ Stuart to confirm for the US: AcuRite and Ambient
  weather stations also sit at 433.92 there.
- **Radio gating**: a band is offered only if the radio can tune all of it (`vsIsmReach`, from the EFFECTIVE tunable
  ranges). **The Airspy HF+ reaches none (it tops out near 260 MHz): ISM is not offered on it at all** — no menu entry,
  no reason, nothing (AGENTS.md: a control that cannot work on this radio is not drawn). An Icom: not offered. Locked
  centre with a shared dial / a capture under 0.9 MS/s: drawn **greyed with the reason**.
- The gain is ISM's own rule on the radio's OWN gain list (RTL table, RSP 0–49 dB, Airspy list): start 60 % up, step down
  when > 2 % of detector frames near full scale, back up one rung after two quiet minutes, never past the start. VibeAGC
  stands down (on the Pi 500's RTL at 433.92 it drove the noise floor up ~27 dB in 9 s). A listener's gain pauses it;
  AUTO resumes it. Nothing is persisted.

## Server contract

**Capability** — `/vibeserver.json` (each radio's own, `/r/<id>/vibeserver.json`):

| field | meaning |
|---|---|
| no `ism` field | this radio reaches no ISM band (the HF+), or an older server: **no control** |
| `"ism": true` | ISM can run now |
| `"ism": false` + `"ismWhy"` | it cannot; show the control greyed, the reason on a tap |
| `"ismBands": [{id, name, hz}]` | the bands THIS radio reaches, in step order |
| `"ismLanding": "433"` | where a bare `{"type":"ism","on":1}` enters |
| `"modes"` contains `"ism"` | same as `ism: true` (directory badge) |

**Client → server** (spectrum socket; gated by ask-before-tuning):
`{"type":"ism","on":1}` (enter: where ISM last was, else `ismLanding`) · `{"type":"ism","on":1,"band":"868"}` ·
`{"type":"ism","on":1,"step":1|-1}` (clamped) · `{"type":"ism","on":1,"hz":433420000}` (exact) · `{"type":"ism","on":0}`.
Missing `on` = OFF. While in ISM: `tune` = an exact frequency; `mode`, `bandwidth`, `zoom`, `reset`, `tunerbw`,
`sampleRate` are ignored (clients grey them). ISM leaves DAB / ADS-B through their own doors, and they leave ISM.

**Server → client**: `{"type":"ism", "band":"433", "bandName":"433.92 MHz ISM", "centre":Hz, "custom":0|1, "rate", "now",
"bursts", "decoded", "unknown", "noise", "dropped", "capture", "gain", "gainAuto", "devices":[…]}` once a second and at
once to a joiner — **its arrival means ISM is on**. `{"type":"ism_off"}` then a `config` — the radio is back.
`{"type":"ism_error","why":"…"}` — refused.

**A device**: `key` (`d<n>`, opaque) · `kind` (weather | tpms | sensor | doorbell | energy | keyfob | suspectfob) · `make` · `model` ·
`name` · `line` (the readable text, metric, tyres in PSI) · `nick` (secret kinds only) · `id`, `ch` (public kinds only) ·
`temp` °C · `hum` % · `wind`, `gust` km/h · `dir` ° · `rain` mm · `rainRate` mm/h · `kpa` · `uv` · `watts` · `baro` hPa ·
`batt` 1/0 · `button` · `note` · `extra` · `freq` Hz · `level` dBFS · `snr` dB · `seen` s since last heard · `age` s since
first · `count` times heard. Absent = not carried.

## ★★★ Privacy (Stuart, 2026-10-25 — overrides DECODER-RESEARCH §4.1's exclusion)

- **TPMS** shows make/family, pressure, temperature, battery/flags — **never the sensor ID**.
- To tell cars apart: **a per-server nickname** ("TPMS #2"), a number handed out in first-heard order, keyed by a
  **SipHash-2-4 of the ID under a 128-bit salt drawn at every start and held only in the process's memory** — never sent,
  logged or stored. The raw ID is wiped from the reading the moment it is hashed (`Tracker::accept`). A restart
  renumbers. The make goes in the line, never the ID: "TPMS #2 · Toyota PMV-C210 · 34 PSI · 29 °C".
- **One frame or two** (2026-11-11): a tyre whose frame carries a strong check (a CRC-8 or CRC-16 behind an exact sync,
  or a CRC plus an inverted copy of the pressure) is listed from its first frame — a car passing at 50 km/h may be heard
  once. A weak check (8-bit sum or XOR, 2- or 5-bit check, none) is listed only once heard twice alike within 90 s, and
  an unconfirmed frame takes no nickname. Each family below says which it is.
- **Key fobs** show only "Key fob #n · KeeLoq · button 2" — the button bits travel in clear on KeeLoq; **never the code,
  rolling or fixed, never raw bits**.
- **Suspected key fobs** (2026-10-25, Stuart: "add them to a list that maybe says suspected keyfob"): an UNDECODED press
  shaped like a car key — FSK 10-120 kHz deviation, ≥ 10 dB, 15-200 ms, Manchester-shaped runs, the same frame again
  60-600 ms later — is listed as "Suspected key fob #n · button press" (`kind` suspectfob; the KEY FOB filter shows it).
  No bits are read. One press = one count (a held button, the dongle's overload images ±120 kHz); anything on a timer
  (three intervals alike, ≥ 8 s) is a sensor and its row goes. Modelled on Stuart's key recorded on the Pi 500 (434.134
  MHz, ±42.7 kHz, 200 µs Manchester halves, 68 ms frames, three 161 ms apart): 16 presses of 16 found, nothing else on
  434 MHz flagged (`vibe_ism_service.h` FobWatch; test-ism "suspected key fobs").
- **Fixed-code remotes/doorbells** (EV1527/PT2262 — the key of many gates, garages and alarms) get the fob treatment:
  "Doorbell / remote #n · button A", the 20-bit code hashed like a TPMS ID.
- **Weather stations, sensors, energy meters** may show a channel and a short ID (how an owner finds their own).
- Public **and** private servers, every API and JSON the server sends. `ism-offline` never prints a secret either.
- Tested: `test-ism` (the JSON never contains the IDs/codes in hex or decimal; two cars two nicknames; per-server
  numbering) and `test-server-ism.mjs` (the same, **on the wire** of the real server).

## The readable-format rules

`Name · readings · battery · ch N` in a fixed order: tyre pressure, temperature (0.1 °C; whole degrees for tyres),
humidity (% RH), wind (km/h + compass), gust, rain (0.1 mm), rain rate (mm/h, only when > 0), UV, pressure (hPa), power
(W / kW), the button, extras ("13 bucket tips", "pairing", "new battery"), battery, channel. **Units:** °C, mm, km/h and
PSI by default; the web box switches km/h ↔ mph and PSI → kPa → bar per listener (localStorage) and rebuilds the line
from the numbers in the same order (`ism.ts ismLine`). There is no server-wide locale setting for units today; if one is
added, the server's `readable()` takes a `Units` already (`vibe_ism_service.h`).

## The UI

**Web client** (built, tested): ISM sits beside DAB and ADS-B in the demodulator menu (absent / greyed / live as above).
The waterfall stays live — the bursts on it are half the point — with no LISTEN needle. A box just above the control deck
(placed from the deck's real height, so it fits a phone): EXIT ISM · ◀ band ▶ · status (`433.92 MHz ISM · 4 devices ·
9 bursts · capture 100 %`) · filter chips ALL / WEATHER / TPMS / SENSOR / DOORBELL / ENERGY / KEY FOB · units · the list,
newest first, "now / 12 s / 3 min" last heard, the frequency and SNR on hover. The readout shows `433.920 MHz · ISM 433`.
Zoom, FIT, step and the IF filter are greyed with the reason; the arrows step bands and name the next one.

**App** (built 2026-10-25, branch `v12-app-parity` — docs/v12/APP-PARITY-2026-10-25.md): `src/services/ism.ts` (the web's
pure rules, held to `web/client/src/ism.ts` by `scripts/test_ism_app.ts`), `VibeServerClient.ism()` + the `ism` / `ism_off` /
`ism_error` messages, `ismHeld` on the tune / zoom / pan / reset gates, the ISM key in the Mode sheet beside ADS-B (absent /
greyed with `ismWhy` / live), `IsmPanel` (EXIT ISM · ◀ band ▶ · status · kind chips · units · the list), every tune gesture
stepping one band (keys, drum, lock-screen skip; clamped), the frequency box sending `{hz}`, the native audio output
released while it runs (iOS `setQuiet`), the box by pad / keyboard (it takes the keys on arrival as the DAB box does;
every key, chip and row a stop — `ismNavKeys`), and no LISTEN needle in ISM (`WaterfallView needleHidden`, every renderer
path through `utils/needleGeometry`). What the brief asked for, kept for reference:
- Read `ism` / `ismWhy` / `ismBands` / `ismLanding` per radio; an ISM entry in the mode selector beside DAB/ADS-B, absent
  when `ism` is missing (HF+), greyed with the reason when false.
- On an `ism` report: a bottom sheet / panel like the DAB box (`AdsbLiveList`'s patterns: PanelNav reachable, D-pad
  rows), the filter chips and the units, `src/services` logic shared with the web's `ism.ts` (`ismLine`, `ismAgo`,
  `ismStatus` are pure — move them to `src/services/ism.ts` and import from both, the adsbInfo pattern).
- **Every tune gesture steps a band**: tuner buttons, the drum (one detent = one band), D-pad / game-controller tune,
  keyboard tune keys — the `DabBlockStepper` pattern, clamped. A **long press** on the frequency (and typing in it) sends
  `{hz}`. The readout shows the band name.
- Hold the audio self-heal while reports arrive (no audio is sent), as for ADS-B.
- **Tour/About copy** (AGENTS.md "when a control moves, fix the copy"): when the app gains ISM, the `sdrTour` card that
  explains the tuning controls must say that in DAB and ISM the arrows step blocks/bands, and AboutOverlay's V12 list
  gets the ISM line. Neither was changed now: the app has no ISM yet, and the web tour does not describe the arrows'
  DAB behaviour either (checked `web/client/src/tutorial.ts` — nothing there misdescribes ISM).
- The watch: nothing (no ISM there).

## Wireless M-Bus meters (V12, 2026-10-26)

Water, heat and gas meters on the 868 band — T1, C1 (frame A and B) and S1, decoded from the channel's samples, kind
`meter`, the METER chip, serials shown (Stuart), never decrypted. On the 868 band the burst channel is ~500 kS/s and the
slicer's smoothing is in µs, so every family above slices as before. Everything about it: `WMBUS-DECODER.md`.

## Protocol coverage (phase 1)

| Family | Band / mod | Status | Source (non-GPL) | Assumed / open |
|---|---|---|---|---|
| Oregon Scientific v2.1 (THGR122NX/THGN123N 1D20, THN132N EC40…) | 433 OOK Manchester | **done** | "Oregon Scientific RF Protocol Description (v1.0/2.1/3.0)", June 2011 | — (both worked examples in the doc pass) |
| Oregon Scientific v3 (THGR810/THGN801 F824/F8B4, UVN800, WGR800 wind, PCR800 rain, RGR968, BTHR968) | 433 OOK Manchester | **done** | same | BTHR968 barometer not read (doc itself unsure) |
| Fine Offset WH1080/WH1081 (OOK) and the many rebadges | 433/868 OOK PWM | **done** | K. Sangeelee, SusaNET 2012-08-17 (archived) | — (3 worked examples) |
| Fine Offset WH1080 V2 (FSK) | 868/915 FSK NRZ | **done** | "WH1080 protocol V2 – FSK", sevenwatt.com 2013-03-02 | bit rate measured from the preamble; wind scale as OOK |
| Fine Offset WH2 (Telldus etc.) | 433 OOK PWM | **done** | Luc Small, lucsmall.com 2012-04-29 | CRC init 0 |
| Nexus-type T/H (Nexus, Sencor, Digoo, Rubicson-alikes) | 433 OOK PPM | **done** | aquaticus/nexus433 README prose (code not read) | two's-complement temperature |
| La Crosse IT+ TX29-IT / TX29DTH-IT / TX35 | 868.3 FSK 17.24 kbps | **done** | fredboboss.free.fr "TX29 Protocol" 2011; sevenwatt.com comments | which tone is 1 (both tried) |
| AcuRite 592TX-family T/H | 433 OOK PWM | **done** | Ray Wang, rayshobby.net parts 1–3 (CC BY-SA text) | temperature offset 1000 (author's empirical 1005); checksum identified by us on his examples |
| AcuRite 00899 rain gauge | 433 OOK PWM | **done** (tips) | same | mm per tip unpublished — shown as "N bucket tips" |
| AcuRite 00782 temperature | 433 OOK PPM | **done** | same | no checksum: 3 identical frames required |
| Current Cost transmitters (electricity), EnviR, impulse counters (gas / water / electricity) | 433 FSK 3918 bps | **done** (corpus 31/31) | Jack Kelly, rfm_edf_ecomanager wiki; EnviR preamble + counter layout from rtl_433's notes | — |
| KeeLoq code-hopping fobs/remotes (Microchip HCS200/301 class) | 315/433 OOK PWM | **done, privacy** (corpus: HCS200 12/12 with the button) | Microchip HCS301 datasheet DS21143B | make unknown from the air: "KeeLoq" |
| EV1527 / PT2262 fixed-code doorbells, PIRs, contacts, gate remotes | 315/433 OOK PWM | **done, privacy** | EV1527/SC1527 and PT2262 datasheets | 2 identical words required |
| **TPMS — 34 decoders (≈ 40 sensor models) in `vibe_ism_tpms.h`** (2026-11-11) | | | rtl_433 device notes (`src/devices/tpms_*.c`, `schraeder.c`) **read for facts only** — layout, timing, check — plus FCC filings and the IHP/DLR paper where they exist; our own framing engine and code (see below) | corpus = the rtl_433_tests recordings (`scripts/ism-corpus.mjs`): match / of the folder's keyed recordings |
| Schrader GG4 (EU/US OE) | 433 OOK Manch. 120 µs | **done, strong** (CRC-8) | schraeder.c notes | corpus 53/54 |
| Schrader EG53MA4 (Opel/Vauxhall/Saab/GM) | 433 OOK Manch. 123 µs | **done, weak** (sum) | schraeder.c notes | corpus 10/10 (incl. the PA66GF35 type B misread); framed from the END |
| Schrader SMD3MA4 (Subaru) / 3039 (Nissan, Infiniti, Renault Koleos) | 315 OOK PCM 120 µs | **done, weak** (2-bit) — no pressure | schraeder.c notes, JoeSc/Subaru-TPMS-Spoofing | same frame, two pressure scales: none shown; corpus 7/7 decoded |
| Nissan (Schrader FSK) | 315 FSK 120 µs | **done, weak** (2-bit) | tpms_nissan.c notes | corpus 10/10 |
| Schrader RDC3 motorcycle | 433 OOK Manch. 122 µs | **done, strong** (CRC-8) | tpms_schrader_motorcycle.c notes, FCC MRXRDC3 | not in corpus (test-ism only) |
| Porsche 987 (was "type A") | 433 FSK diff. Manch. 52 µs | **done, strong** (CRC-16) — now with pressure | IHP/DLR paper + tpms_porsche.c notes | the notes' 2 worked frames in test-ism |
| Toyota PMV-C210 (Pacific) | 433 FSK diff. Manch. 52 µs | **done, strong** (CRC-8 + inverted pressure) | tpms_toyota.c notes, issue #706 | corpus 8/15 — 6 old captures with ±75 kHz deviation the detector splits into two silent tones |
| Toyota PMV-107J (Pacific, US) | 315 FSK diff. Manch. 100 µs | **done, strong** (CRC-8 + inverted pressure) | FCC PAXPMV107J + tpms_pmv107j.c notes | corpus 27/29 |
| Renault (Clio, Captur, Zoe) | 433 FSK Manch. 52 µs | **done, strong** (CRC-8) | tpms_renault.c notes | corpus 2/4 (wide-deviation captures) |
| Renault 0435R (VDO: Mégane III, Scénic III, Laguna III, Zoe) | 433 FSK Manch. | **done, weak** (XOR) | tpms_renault_0435r.c notes, VDO 2017 list | not in corpus; tried before Abarth (same XOR frame shape) |
| Hyundai / Kia VDO TG1C (i30 …) | 433 FSK Manch. | **done, strong** (CRC-8) | tpms_hyundai_vdo.c notes, FCC KR5TIS-01 | not in corpus |
| Kia Rio III / some Hyundai | 433 FSK Manch. 50 µs | **done, weak** (5-bit CRC) | tpms_kia.c notes | not in corpus |
| Hyundai Elantra 2012 / Honda Civic (TRW GQ4-44T) | 315 FSK Manch. | **done, strong** (CRC-8) | tpms_elantra2012.c notes | corpus 4/4 |
| Honda TRW PPA-GF33 (GQ4-36T) | 315 FSK Manch. 50 µs | **done, strong** (CRC-8) | tpms_honda.c notes | corpus 1/1 |
| TRW GQ4-70T (Chrysler / Jeep / Dodge / Ram) | 315/433 OOK + FSK Manch. 52 µs | **done, strong** (CRC-8) | tpms_trw.c notes, FCC GQ4-70T | not in corpus |
| Citroën / Peugeot / Jeep (VDO) | 433 FSK Manch. | **done, weak** (XOR) — temperature only | tpms_citroen.c, tpms_jeep.c notes | identical frames, pressure scale ×1 vs ×2: none shown; corpus 3/3 |
| Ford (VDO S180084730Z) | 315/433 FSK Manch. | **done, weak** (sum + state) | tpms_ford.c notes | corpus 2/3; learn-tool frames shown without pressure |
| Abarth 124 / Fiat 124 / Mazda MX-5 ND (VDO TG1C) | 433 FSK Manch. | **done, weak** (XOR) | tpms_abarth124.c notes | corpus 4/4 |
| Shenzhen EGQ Q85 | 433 FSK Manch. | **done, strong** (CRC-16) | tpms_abarth124.c notes (#2556) | corpus 6/6 (needed the gated discriminator, below) |
| BMW Gen4/Gen5 (Huf/Beru, Schrader-Sensata, Continental), Audi alert | 433 FSK Manch. 25 µs | **done, strong** (CRC-8 0x2F) | tpms_bmw.c notes | corpus: `continental_tpms` 3/4 (no answer key — IDs and values agree with its README); the BMW .cs16 captures start too close to the frame |
| BMW Gen2 / Gen3 | 433 FSK diff. Manch. 52 µs | **done, strong** (CRC-16) | tpms_bmw_g3.c notes | not in corpus |
| Mercedes-Benz Sprinter (2025) | 433 FSK Manch. 25 µs | **done, strong** (CRC-8 0x2F) | tpms_mercedes_benz.c notes | corpus 1/2 |
| Truck solar TPMS | 433 FSK Manch. | **done, weak** (XOR) | tpms_truck.c notes | corpus 5/6 |
| Jansite Solar | 433 FSK Manch. | **done, strong** (CRC-16) | tpms_jansite_solar.c notes | corpus 1/1 |
| Jansite TY02S | 433 FSK Manch. | **done, weak** (no check — a fixed 16-bit trailer) | tpms_jansite.c notes (off in rtl_433: last byte unknown) | corpus 29/29 |
| Airpuxem TYH11 (Senasic SNP739D) | 433 FSK Manch. | **done, strong** (CRC-8) | tpms_airpuxem.c notes | not in corpus |
| Steelmate TP-S15 / R-Lake | 433 FSK Manch. 50 µs, LSB first | **done, weak** (sum) | steelmate.c notes (#3200) | corpus 24/31 (the rest start too close to the capture's start) |
| SmarTire Vantage (Aston Martin DB9) | 433 OOK PCM 167 µs, diff. Manch. | **done, weak** (CRC-7) | tpms_smartire.c notes | corpus 1/1 |
| EezTire E618 / Carchet / TST 507 | 433 OOK Manch. 50 µs | **done, weak** (sum) | tpms_eezrv.c notes | corpus 4/4 (EEZ + tst507) |
| Gear Hive | 433 OOK Manch. 104 µs | **done, weak** (no check — 10 fixed bits) | tpms_gear_hive.c notes | corpus 2/2 |
| GM aftermarket (EL-50448) | 433 OOK Manch. 120 µs | **done, weak** (sum) | tpms_gm.c notes | corpus 2/2 |
| Sefis M3 / Careud / Sykik | 433 FSK Manch. | **done, strong** (CRC-16) — no ID in the frame | tpms_sefis_m3.c notes | corpus 2/2; all Sefis tyres share one line |
| AVE | 433 FSK diff. Manch. 100 µs | **done, strong** (CRC-8) | tpms_ave.c notes | not in corpus |
| TyreGuard 400 (Davies Craig, 434.1 MHz) | 433 OOK Manch. 100 µs | **done, strong** (CRC-8) | tpms_tyreguard400.c notes | not in corpus |
| "Type B" (OOK, CRC-8 searched over every offset) | — | **retired** | — | on the corpus it read only weather sensors, remotes and doorbells — all 12 WRONG of the baseline — and misread real Schrader frames |
| Schrader MRXBC5A4 (BMW) | 433 | not done | schraeder.c notes | its 4 captures are FSK where the notes say OOK; the framing could not be confirmed on them |
| iMars T240 | 433 OOK | not done | tpms_imars_t240.c notes | no pressure/temperature mapping known, and its "checksum" is a constant per unit |
| Jansite TY468 / TY588 | 433 | not done | — | no recordings, only code lists |
| Efergy e2 / Elite | 433 FSK | **done 2026-11-11** — see the home families below | | |

Twelve families with their variants (≈ 25 models) in phase 1; 34 tyre-sensor decoders added 2026-11-11.

**TPMS framing (`vibe_ism_tpms.h`, 2026-11-11).** Every tyre family is a chip sync + a line code + a check at one of a
handful of chip widths (25, 50-52, 100-104, 120-123, 167 µs). A burst's runs become fixed-width CHIPS — the width
measured from the burst (the shortest marks and the shortest spaces measured apart, so an OOK transmitter's mark
stretch comes out) and used when within ±25 % of a family's nominal — and each family searches its sync, reads
Manchester or differential Manchester, and checks. FSK families try both polarities and also the burst's ENVELOPE:
a wide-deviation sensor (±60-75 kHz) arrives as two OOK channels, one per tone, and each tone's on/off IS the frame.
The front end's discriminator now also runs over a burst whose carrier drops BETWEEN repeated frames (EGQ Q85, Jansite
Solar, Sefis: three frames 11 ms apart) — gated to the carrier's long stretches, the gaps read as the low tone.
Sources: Stuart (2026-11-11) — *"we dont want to copy but if we can take inspiration"*: rtl_433's device notes were read
for the FACTS of each frame; no code, table, comment or identifier was copied.

**Score on the rtl_433_tests corpus** (`node scripts/ism-corpus.mjs`, .cu8 recordings): before the TPMS work 162 match ·
259 decoded · 12 WRONG (all "type B") · 0 false alarms; after: 363 match · 254 decoded · 0 WRONG · 0 false alarms. The
13 fewer "decoded" were type B's false TPMS on remotes and doorbells.

### Weather and environment — the weather pass (2026-11-11)

Scored on the rtl_433_tests corpus (3 724 recordings): **match 162 → 1 040** across the whole corpus (decoded-with-
nothing-to-compare 259 → 350), **WRONG 12 → 5** (the five left are all TPMS "type B" on non-weather recordings),
**false alarms 0 → 0**. test-ism: 245 checks (17 new end-to-end weather families). "Check" is what tells a frame from
noise; weak checks are shown once heard twice (`confirm`). Header per family = the frame facts and their sources.

| Family (models) | Band / mod | Check | File |
|---|---|---|---|
| Oregon v2.1 / v3 — **added** THGR968, THGR328N, RTGN318 / RTGN129 / RTGR328N (first ID nibble varies), THN129 / RTHN129, BTHR918 / BTHGN129 (+ barometer), WGR968, AWR129 BBQ (hundreds digit), THGR810 rolling IDs, PCR800 AU; sign = bit 3 of nibble 11 | 433 OOK Manchester | nibble sum | `vibe_ism_protocols.h` |
| Oregon SL109H | 433 OOK PPM 2/4 ms | 4-bit sum + repeat (confirm) | `weather2` |
| Fine Offset / Ecowitt FSK: WH24, WH65, WS69 (+ pressure), WH25, WH32 / WH32B, WH0290 PM2.5, WH51 soil, WH31E / WH31B, WH40 / WN20 rain, WS68 wind/light, the WWVB clock frame | 433/868/915 FSK 58 µs | CRC-8 + byte sum | `weather2` |
| Fine Offset WH1080 / WH3080 — **fixed**: 7-one preamble, 10-bit OOK temperature, UV/light (type 7) and DCF77 time (type B) frames, frame = whole row; **added** WH1050 / TFA Stratos (OOK + FSK), WH0530, Alecto WS-1200 v1/v2 | 433/868 OOK PWM, FSK | CRC-8 (+ sum) | `protocols2` |
| La Crosse IT+ — **fixed**: 9.58 kbps (TX35 / TX25U), short preamble; **added** TX34-IT rain | 868 FSK | CRC-8 | `protocols2` |
| La Crosse TX141-Bv2/Bv3, TX141TH-Bv2/Bv3, TX141W / TX145wsdth; WS-2310 / WS-3600; TX-3 / TX-6U / TX-7U / TX5U; TX22U-IT | 433 OOK PWM, 915 FSK | LFSR digest / CRC-8 / nibble sums + parity | `weather3` |
| AcuRite TXR family: 592TXR tower, 5-in-1, 3-in-1, Atlas (± lightning), Optimus, 515, 6045M, 899, 1190 leak; 00275 / 00276RM; 609TXC; 606TX / **00782** (the same format — its 4th byte is the 606 digest); 590TX; 985 / 986 fridge | 433 OOK PWM / PPM | byte sum + parity / CRC-16 / LFSR / CRC-8 | `weather4` |
| S3318P family (Conrad, Esperanza EWS, Kedsum, Pearl), Digitech XC-0324, Hideki (TS04, Cresta, Bresser 5CH, Mebus, Sigma), GT-WT-02 / -03, WT0122 / WT0124 pool, Cotech 36-7959 (SwitchDoc, Sainlogic), TFA 30.3160 pool | 433 OOK PPM / PWM / DMC / Manchester | CRC-4 / XOR / CRC-8 / sums | `weather5` |
| Auriol AFW 2 A1, AFT 77 B2, AHFL, HG02832 / Rubicson 48957; Vauno EN8822C; AlectoV1 (T/H, rain, wind); Prologue / ThermoPro TX2 (two sensors in one burst are both read) | 433 OOK PPM / PWM | sums / LFSR / CRC-8 / repeats | `weather6` |
| WS 7000 / WS 2500 (ELV, La Crosse); Ambient F007TH family; Ambient TX-8300 / TFA 30.3211 | 433 OOK PWM / Manchester / PPM | XOR + sum / LFSR / inverted copy + Fletcher | `weather7` |
| GT-TMBBQ-05; TFA Twin Plus 30.3049 / KW9010; Philips AJ7010 / AJ3650; TFA 30.3196 (868) | 433 OOK, 868 FSK | sums / XOR / CRC-4 / repeats | `weather8` |
| Eurochron EFTH-800; ThermoPro TP11 / TP08 / TP12 / TP20; Rubicson / TFA 30.3197 / inFactory (before Nexus); Bresser 3CH; WT450 / WT260H / WT405H | 433 OOK | CRC-8 / LFSR / sums / parity | `weather9` |
| TFA Drop 30.3233 rain; Inkbird ITH-20R / IBS-P01R | 433 OOK PWM, 433 FSK 100 µs | LFSR digest + repeat / CRC-16 | `weather11` |
| Bresser 6-in-1 family (7-in-1 indoor, new 5-in-1, soil, pool, 3-in-1 wind, Froggit WH6000, Ventus, Sencor), 7-in-1 / 3-in-1 / 8-in-1 + PM / CO2 / HCHO, 5-in-1; Maverick ET-732/733; Companion WTR001 | 868 FSK 124 µs, 433 OOK | LFSR-16 + sum / inverted copy / repeats | `weather10` |

**ASSUMED / open:** the Philips AJ7010 °C formula (raw / 353 − 9.2 — the only published fit); the TFA 30.3196's 16-bit
check key is unknown (twice alike + confirm instead); the WH24 / WH65 split by preamble length; the Maverick digest
hides a per-reset ID (raw pattern + confirm). **Left (and why):** XC-0324 recordings that start mid-transmission — the
detector's first frames seed its floor, so a sensor on the air from sample 0 is never detected (`ism_replay --lead 0.3`
reads all 58: a corpus artefact, not a live one); Cotech 36-9231 is LoRa (not this path); WH51 at 915 MHz (a ~430 kHz
tone spacing — two channels, neither the whole signal); Oregon v1 / WMR500; La Crosse LTV (WR1, R1, TH2/3, W1,
BreezePro) and WS-6868; Emax W6 family, Govee, ThermoPro TX7B / 211B / TP28B, TS-FT002, Oria, Esun, inFactory,
Thermor, Holman — not yet written. Oil-tank level sensors (Watchman, ~380 recordings) were left to the home/energy pass.

### Home families (2026-11-11): meters, alarm sensors, remotes — `vibe_ism_meters.h`, `vibe_ism_security.h`, `vibe_ism_remotes.h`

Corpus = the rtl_433_tests recordings that decode (`scripts/ism-corpus.mjs`); "oracle" = the recording decodes when its
channel is cut by hand, but the bench's detector misses the burst (it starts inside the detector's 64-frame floor seed —
see Tests). Alarm sensors are kind `security` ("Alarm sensor #n"), remotes `doorbell`, car / gate fobs `keyfob`: all
**secret** (identity hashed, never shown; rolling codes never decoded or shown).

| Family | Band / mod | Corpus | Facts from | Check |
|---|---|---|---|---|
| Oil-tank gauges: Watchman Sonic, Apollo Ultrasonic Smart / Standard, Tekelek TEK377, Watchman Sonic Advanced | 433 FSK (or OOK) 500 µs / 1 ms chips, Manchester | **351/383** | rtl_433 oil_*.c notes | CRC-8 / CRC-16; Standard has none → confirm |
| OpenEnergyMonitor emonTx (JeeLib RF12) | 433 FSK 49.2 kbit/s | test only (recordings in the seed) | jeelabs.org RF12 packet format | CRC-16 |
| Efergy e2 classic clamp, Efergy optical | 433 FSK PWM | **11/19** | rtl_433 efergy_*.c notes | sum / CRC-16 |
| Itron ERT SCM, SCM+, IDM / NetIDM | 912 OOK Manchester 30.5 µs | oracle 5/7 | rtlamr wiki "Protocol" | BCH / CRC-16 |
| Neptune R900 | 912 OOK 30 µs | test only (envelope does not slice) | rtlamr wiki; rtl_433 notes | 42 symbols of 6 valid codes |
| Wireless M-Bus (existing) | 868 | **38/43 + 2/2** (was 16/43: the bench read a `_0M_` name as 0 Hz) | EN 13757 | — |
| KNX-RF (named, no address) | 868 | 11/11 | KNX RF 1.1 (Esc 0xFF) | wM-Bus block CRCs |
| Interlogix / GE / UTC (ITI) | 319.5 OOK PPM | **30/38** | US patent 5 761 206; rtl_433 notes | 2 parity bits → confirm; marks checked |
| Honeywell / Ademco 5800, 2GIG | 345 OOK Manchester 136 µs | **27/29** | L. Lamb, DEF CON 22 "Home Insecurity" | CRC-16 |
| DSC contacts / motion | 433 OOK RZ 500 µs | **8/8** | FCC F5300NB912 report; rtl_433 notes | CRC-8 |
| X10 security | 310/433 OOK NEC-like | **5/5** | rtl_433 notes | complements + parity |
| SimpliSafe sensors (never keypad PINs) | 433 OOK PIWM | **5/10** (2026-11-12: the row was inverted twice; the 5 left are keypads — one AGC-saturated carrier) | rtl_433 notes | sum + twice alike |
| Generic 433.3 MHz motion kit | 433 OOK PWM | **14/14** | rtl_433 notes | none: 3 copies, every gap agreeing with its mark |
| LightwaveRF | 433 OOK | **43/43** | So Make It wiki | 16-of-256 symbols, whole frame, twice |
| Nexa / KlikAanKlikUit / Proove / Intertechno self-learning | 433 OOK PPM | **17/17** | elektronikforumet wiki | pairs, twice |
| X10 RF | 310/433 | **4/4** | WGL Designs W800RF32 doc | complements + fixed bits |
| GE Color Effects | 433 FSK 52 µs | **14/16** | deepdarc.com; lukecyca.com | twice |
| Somfy RTS | 433.42 OOK | 1/2 | pushstack.wordpress.com; US 8 189 620 | nibble XOR |
| Microchip HCS361 (car / gate) | 315 OOK PWM or VPWM | **20/22** (2026-11-12: + the VPWM data format and the 1/3-duty preamble sync; 2 have no burst) | DS40146F; rtl_433 hcs361.c notes | 2-bit CRC → confirm |
| Clemsa, Ansonic, BETT, Elplast, Linear MultiCode / Delta-3, Roger, Allstar Firefly, Nice FLO (12-bit) | 433 OOK PWM | **13/27** | rtl_433 conf/ descriptions | none: whole words, twice |
| Car fobs: Continental KR5V2X (Honda), Chrysler | 313.8 FSK Manchester / 315 OOK PWM | **7/7, 7/7** | rtl_433 *_car_remote.c notes (FCC IDs) | XOR / sum; Chrysler rows exactly one packet |
| Car fobs: MIC 6SC2, GM ABO1502T | 315 OOK | **6/6, 5/5** (2026-11-12: 6SC2's zero-bit first half restored; GM's 112 bits end at the row's end) | same | XOR / sums; confirm |
| Paradox Magellan sensors and remotes | 433 OOK PWM | **5/5** | Flipper Zero protocol via rtl_433's notes | CRC-8 |
| Honeywell ActivLink chimes / PIR / contacts | 916.8 OOK, 868 FSK, PWM | **5/6** | K. Lohner, honeywell-wireless-doorbell | parity + 15 zero bits, twice |
| RadioHead ASK packets, Sensible Living moisture | 433 OOK NRZ | **9/10** | RadioHead RH_ASK docs | CRC-16 (data never shown) |
| Vaillant VRT 340f thermostat | 868 OOK differential Manchester | **6/7** | R. Kainhofer's wiki | 16-bit checksum, HDLC stuffing |

Not done (home area), and why: Owl CM130/160/180 (inside the Oregon v3 path the weather work owns), IKEA Sparsnäs
(encrypted with its ID — not decrypted), Insteon, EC3k, ESIC EMT7110, Elster, Honeywell CM921, Danfoss, Funkbus,
Siemens (Security+, Chamberlain, the car remotes, Vivint, Visonic, Cavius, HCS362: see the second pass below), Kerui/Chuango/Waveman naming (they ARE
EV1527/PT2262 — told apart only by data patterns), 24-bit Nice FLO (a PT2262 word on the air) and ADLM (a La Crosse
length) — the last two were tried and dropped for naming other devices.

### Second security / remotes pass (2026-11-12) — `vibe_ism_security2.h`, `vibe_ism_fobs.h`

Facts from the notes at the head of rtl_433's device files and `conf/` descriptions (GPL: read for frame layout, timing
and checks only — no code, tables or comments copied; button-name tables were NOT copied, so TV remotes and remote
starters show the button number), the Microchip datasheets, and the rtl_433_tests READMEs. Scores are on the corpus with
the ism2-front front end. Everything here is **secret** (identity hashed by the Tracker, never in any output; test-ism
checks the JSON for each family's code in hex and decimal). Rolling / hopping codes are never read: KeeLoq hop words are
skipped, the Security+ 2.0 rolling digits are checked for validity and thrown away, never assembled.

| Family | Band / mod | Corpus | Facts from | Check |
|---|---|---|---|---|
| Vivint door/window, glass-break, motion | 345 OOK Manchester 135 µs | **6/6** | vivint.c notes | packed 12-bit CRC 0x8050; status enciphered → only the sensor kind shown |
| Visonic PowerCode | 433 OOK PWM 400/800 | **2/2** | visonic_powercode.c (MCR-300 manual) | XOR nibble, twice |
| Cavius smoke / heat / water alarms, door sensors | 869.67 FSK Manchester 206 µs / 868.7 NRZ 415 µs | **3/3** | cavius.c notes | CRC-8 Maxim + complement / CRC-8 0x07 |
| Chamberlain CWPIRC motion | 915 FHSS FSK 25 µs | 4/4 decode — **HELD, not called** | chamberlain_cwpirc.c notes | CRC-16/XMODEM; corpus folder 02 has an EMPTY key over 4 CRC-valid frames of the same sensor → would score a false alarm; for Stuart |
| Security+ 2.0 garage remotes | 310/315/390 OOK Manchester 250 µs | **1/1** | secplus_v2.c notes | both halves' order/invert/trit validity; fixed part only |
| Chamberlain / LiftMaster DIP (7/8/9 switches) | 300–390 OOK 1 ms slots | **3/3** | chamberlain_code.c (Flipper Zero) | none: symbol + stop places, twice |
| Microchip HCS362 (PWM and Manchester) | 315/433/868 | **4/4** | DS40189E; hcs362.c notes | 2-bit CRC → confirm |
| 2GIG KEY2E (encrypted) | 345 OOK Manchester | **4/4** | twogig_key2e.c notes | CRC-16 0x8005 init 0x4C57; all enciphered → only the make |
| Ford (EU) car keys | 433 OOK DMC 250 µs | **6/6** (decoded) | fordremote.c notes | none: confirm; no button / code shown |
| Honda (EU) car keys — also Nidec OUCG8D (Honda OEM) | 433 / 313.8 FSK PWM 250/500 | **3/3** + nidec 4/6 | hondaremote.c notes | none: 26 fixed bits + length, confirm |
| Code Alarm FRDPC2002 | 433 OOK zero-bit Manchester 550 µs | **5/5** | code_alarm_car_remote.c | none: confirm |
| Astroflex Astrostart 2000/3000 | 372.4 OOK PPM | **5/5** | astrostart_2000.c | complement + 4-bit sum, confirm |
| Compustar 1WG3R | 433 OOK PWM 740/1110 | **5/5** | compustar_1wg3r.c | complement byte + fixed bits, twice |
| Audiovox PRO-OE3B | 303 OOK PWM 450/900 | **3/3** | audiovox_pro_oe3b.c | fixed bits, twice / confirm |
| Alps FWB1U545 (Honda OEM) | 433 FSK zero-bit Manchester 500 µs | **4/4** | alps_fwb1u545.c | 55 + copied byte + length, confirm |
| GangQi / Hollarm alarm remotes | 433 OOK PWM / PPM | **3/3, 3/3** | gangqi.c, hollarm.c (Flipper Zero) | 8-bit sums, twice / confirm |
| King Gates Stylo 4k | 433 OOK | **2/2** | kinggates.c (Flipper Zero) | none: constants + two identical messages |
| iDo 111/117 | 433 OOK PPM | **2/2** | ido.c (Flipper Zero) | none: two messages, same fixed part |
| HT680-based remotes | 433 OOK PWM | 0/2 (written; the recordings slip bits) | ht680.c notes | sync + 12 fixed pin bits, confirm |
| Revers RB2 | 433 OOK zero-bit Manchester 250 µs | **2/2** (decoded) | conf/Revers-RB2.conf | none: the word 3+ times with its 32 ones |
| KeyFinder tags | 433 OOK PWM 400/1300 | **3/3** | conf/KeyFinder.conf | none: twice alike |
| Roger, Nice FLO 12-bit (first-pass rows) | 433 OOK PWM | **3/3**, 1/3 | conf/ | one word a burst → confirm (only these two: doing it for every family misread weather / TPMS) |
| DirecTV RC66RX, Dish UHF 6.3 TV remotes | 433 FSK / 384 OOK PPM | **4/4, 3/3** | directv.c (K. Lohner), dish_remote_6_3.c | nibble sum / fixed bits, twice |

Left, and why: **Security+ 1.0** (no check, and the fixed ID cannot be separated from the rolling code without
decoding it — privacy rule); **Cardin S466** (27.195 MHz — no band we listen on); **Kidde RF-SM** and **Honeywell
5890PI** (the slice is broken on these recordings — glitched envelope / a saturated carrier); **Corradi HCS200** 2/3 (the
preamble burst is distorted, the data burst alone has no sync); **SimpliSafe keypads** (saturated carrier; PIN frames
never decoded anyway); **Allstar Firefly** and the other first-pass PWM rows one word a burst (single words of those
shapes appear in other devices' recordings); **24-bit Nice FLO** (a PT2262 word); **Lynx / generic 1000-3000
doorbells, generic 4-channel 315 remotes** (EV1527/PT2262-shaped, no check — not attempted this pass); **Opus XT300,
Shenzhen Wale, Blueline, pt20tx (Bresser), axel (Esperanza)** — weather / energy, the other passes'.
Also seen: in 6sc2/01/g001 two ~50 µs OOK bursts read as **Toyota PMV-C210 TPMS** (not 6SC2 frames — those are
250 µs; rtl_433's key lists none): a TPMS-side question for whoever owns vibe_ism_tpms.h.

## The gallery (2026-10-25)

Stuart: *"ISM needs to be in the gallery too … same as OWRX does it but in our readable format."* `vibe_ism_gallery.h`
(`DayLog`), fed by `vibe_ism_sink.h` from `Tracker::accept` — only what the LIST counts as a new reading (the 3 s repeat
fold holds) reaches the gallery, and only its kind, name and readable line (nothing an ID could travel in).

Two items per band per UTC day, whoever was listening — a listener in ISM mode (`live`, only while the owner keeps
listeners' decodes) or a background-decoding ISM profile (`background`); a day written by both is marked `both`:

- **The day's log** — downloads as `ISM 433.92 2026-10-25.txt` (index field `dn`). One line per reading, the list's line
  exactly (server units: metric, PSI), prefixed with the UTC time and the burst's frequency in MHz:
  `17:01:12  433.920  Oregon Scientific THN132N · 10.5 °C · battery low · ch 1`. **Never a write per reading:** held in
  RAM, appended every minute or 8 KB, at UTC midnight, and when ISM stops (a band change, ISM off, a listener arriving
  over background, a profile switch, a clean shutdown — `g_ism.stop()` in the shim's stop). A day past 4 MB splits into
  parts (`… (2).txt`).
- **The summary card** — FT8's rolling card mirrored: ONE standing card per band per day (`Store::replace`, tmp +
  rename), rewritten at most once a minute and only when something new was heard, moved to the top; written once more
  when ISM stops. "ISM 433.92 MHz · 2026-10-25 · 14 devices: 6 weather, 5 TPMS, 2 key fobs, 1 doorbell / remote", the
  readings, the most heard, the newest, and **the last reading of each device** (last heard · first heard · count ·
  kind · line), at most 2000 rows (the least recently heard go, counted as "not listed").
- **A restart:** the day's log is found again (`Store::adoptSlot`, `anyOrigin`) and appended to; the card's own table
  is read back (it is its own record), so the counts carry on. The nicknames cannot carry over — the salt is never
  stored — so a car heard either side of a restart is two rows; new secret devices are numbered ABOVE the ones the day
  already named (`DayLog::nickFloor` → `Tracker::reserveNicks`), so "TPMS #2" never means two cars on one card.
- **Privacy:** the gallery is handed the Tracker's own names and lines and writes nothing else; `test-ism-gallery` and
  `test-server-ism-gallery.mjs` search every file and the index for the clip's IDs and codes in decimal and hex.
- **Clients:** an ISM chip in the web gallery (after FT8) and the app's GalleryOverlay; the open item's DOWNLOAD /
  OPEN / SAVE uses the item's own name; no TRANSLATE (our English). "both" is tagged BACKGROUND + LISTENER
  (`src/utils/galleryOrigin.ts`, one rule for both clients).

### In background decoding

A profile may decode **ISM on a band** instead of an RF centre + VFOs (`Profile::ism`, `"ism":"433"`): setup → the radio
→ Background Decoding → "Decodes: ISM sensors on a whole band" → the band (only those this radio reaches, `ismHwBands`).
ISM takes the whole capture, so it runs **only with nobody here**: after the wait, the shim enters the listener's own ISM
mode on the band (`bgIsmStartLocked` → `ismEnter`, the radio's state saved exactly as the ISM button saves it); a
listener arriving ends it through `ismLeave`, which gives the radio back exactly as found — the newcomer lands on the
radio's own dial, never on the band. Never on a locked radio (refused at the save; background never moves one), never
over another whole-radio mode (a listener's ISM or ADS-B in its leave grace — it waits), never where ISM cannot run
(`vsIsmWhyNot`: blocked, < 0.9 MS/s, the band outside the allowed ranges — the admin line says "cannot run — why", tried
again every minute, and the idle park is not held off for it). Kept-running ADS-B still owns the idle radio. Time slots
mix freely: NAVTEX by day, ISM 868 by night.

## CPU

Measured with `test-ism`'s CPU row and a front-end profile (10 s of 2.4 MS/s), niced:

| machine | idle band (noise) | busy band (10 bursts / 10 s) |
|---|---|---|
| Apple M4 (dev Mac), radix-2 | 0.8 % of one core | 1.0–1.3 % |
| Pi 500, Cortex-A76, arm64, PFFFT (the server's build) | 2.0–2.3 % | 2.9 % |
| Pi 500, the old 32-bit-ARM hop (640 µs, until 2026-10-31), PFFFT | 1.3–1.5 % | — |
| Pi 500 real 433.92 capture under VibeAGC's high gain (`ism-offline`, M4) | — | 4.3 % of real time on the M4 (52 false bursts in 30 s) |

★ **The Pi 2 (900 MHz Cortex-A7, armhf, the minimum spec) is NOT measured** — I was told never to ssh into it. Estimate:
an A7 at 900 MHz is ~8–10× slower than the Pi 500's A76 on this kind of code, so **~10–20 % of one Pi 2 core** at the
armhf hop, more on a noisy band; ISM mode switches the audio chain off, which pays part of it back. To measure:
`scripts/run-tests.sh` on the Pi 2 prints the test-ism CPU row, or `ism-offline <capture> 2400000 433920000` prints
"% of real time".

## Tests

- `vibeserver/test-ism.cpp` (run-tests.sh, 105 checks; passes on x86-64, arm64 (Pi 500) and at the armhf hop):
  every published worked example (Oregon ×2 + a bad checksum, WH1080 ×3, WH1080 FSK, AcuRite 592 ×3, AcuRite 899 ×3,
  AcuRite 782 ×2, Current Cost); every family **encoded from its spec** (independently of the decoder's path), modulated
  into noisy IQ at 2.4 MS/s +120 kHz, through the real front end; drift −150…+150 kHz; capture rates 1.024, 2.048, 2.4,
  2.56, 3.0, 6.0, 10 MS/s; sensitivity (Oregon decodes 6/6 down to +4 dB channel SNR, 2/6 at −1 dB); three transmitters
  at once; 20 s of pure noise and a CW carrier decode nothing; the privacy rule on the JSON; repeats, units, landing bands.
- ★ 2026-11-11, the home families: `test-ism` encodes each from its frame facts (oil ×4, emonTx, Efergy, Current Cost
  counter, Interlogix, Honeywell, DSC, generic motion, LightwaveRF, Nexa, X10 + X10 security, GE Color Effects, ERT SCM,
  Somfy, Clemsa, Continental, Chrysler, RadioHead) through the real front end, the secret rule on each (225 checks).
  **The real-recording score** is `scripts/ism-corpus.mjs` over rtl_433_tests (test input only): home area 56 → 678
  of 884 recordings read; every family's cross-reads audited (none but real background devices: a LightwaveRF TRV in two
  La Crosse captures and an Oregon one, a neighbour's ERT meter in a 915 MHz Ambient capture). ★ The bench cannot see a
  burst that starts inside the detector's first 64 frames (~65 ms at 250 kS/s, ~20 ms at 2.4 MS/s — the floor seed)
  [★ 2026-11-12: fixed in the detector — the median seed and the look back, §front end; XC-0324 14 → 58/58, emonTx 0 → 76/76;
  the front-end pass took the corpus from match 1896 · decoded 341 · miss 886 to 2072 · 350 · 701, WRONG 0, false alarms
  0; test-ism's "front end" block: a frame from sample 0 and 4 ms in, a WH2 40 ms in at 250 kS/s, a spur from sample 0
  opening nothing, a Toyota at ±75 kHz as ONE burst, an emonTx at ±90 kHz]:
  every emonTx, SimpliSafe, ERT/IDM/SCM+ capture and many short ones — a bench limit, not a live one (live, the seed is
  the first instant after a start). Two front-end fixes came out of the corpus: the FSK span ignores pre-roll blips
  (+61 decodes, none lost), and the bench reads `_2500k` / decimal rates, `samplerate` files and README frequencies.
- `scripts/test-server-ism.mjs` (27 checks): the real server on `fake-rtl-tcp.mjs --ism` replaying `test-ism
  --write-clip`: capability, entering, the four devices and their exact lines, the privacy rule **on the wire**, steps
  433 → 868 → 915 (clamped), back by name, an exact frequency, a mode change ignored, EXIT giving the dial back.
- `vibeserver/test-ism-gallery.cpp` (56 checks): the day's line exactly; nothing on disk before a minute or 8 KB; one
  append; finish() writes the rest; the Tracker's fold reaching the file; the card (kinds, most heard, newest, last
  reading per device, a 120-byte title, at most once a minute, only when changed); privacy in every file; midnight →
  two logs and two cards; background + listener on one day = one file, "both"; another band, another file; a restart
  appending to the log, reading the card back and numbering TPMS above the day's; the 4 MB parts; the ISM profile's
  parse, round trip and refusals.
- `scripts/test-server-ism-gallery.mjs` (33 checks): a listener's session through the real server writes the day's log
  and card (nothing while it ran), lines equal to the list's, the download name, no ID anywhere; then an ISM profile
  with nobody here: ISM mode by itself, the admin line, a listener arriving stops it and lands on 96.1 WFM, the same
  day's items now "both", TPMS #2 after the restart.
- `scripts/test-web-ism-gallery.mjs` (headless Edge `--mute-audio`): the ISM chip, the two rows, the card and the log
  opening as text, the download names, no TRANSLATE, and at 390 px nothing scrolls sideways; four screenshots.
- `scripts/test-setup-bg-ism.mjs` (18 checks): the setup editor's ISM profile — offered only where ISM can run, only the
  reachable bands, saved as `{"ism":"433"}` and read back, an ordinary profile byte for byte as before.
- `scripts/test-web-ism.mjs` (18 checks, headless Edge `--mute-audio`): the menu entry, the box, the lines, the
  readout, zoom held, ▶ and the card's arrow stepping bands, the top band greying ▶, the filter, kPa, a phone-width fit
  clear of the deck, EXIT.
- **Real signals**: 30 s at 433.92 and 30 s at 868.6 MHz from the Pi 500's RTL `00000004` (0 listeners, checked first;
  full-rate raw IQ; its antenna is the MW one it lands on) — `~/VibeSDR-test-audio/ism/`. **No sensor was heard in
  either**, and the decoder printed **0 readings** (0 false decodes); the 433 capture taught the SNR floor, the run cap
  and the band-span limit above (299 → 52 bursts). A real validation still needs a capture with sensors in it.
