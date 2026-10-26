# ISM sensors (V12): readable weather stations, sensors, doorbells, tyres

VibeServer's answer to OpenWebRX's "ISM" (rtl_433) mode, but **human-readable**:

```
Oregon Scientific THGR810 / THGN801 · 18.4 °C · 62 % RH · battery OK · ch 2
Fine Offset WH1080 · 12.5 °C · 90 % RH · wind 4 km/h S · gust 7 km/h · rain 23.4 mm · battery OK
TPMS #1 · type B · 32 PSI · 18 °C
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

1. **Detector** — a 256-point Hann FFT of one block every 320 µs (640 µs on 32-bit ARM), power per bin against a
   **censored-mean noise floor per bin** (a minimum-tracker settles at a fraction of an exponential mean and calls one bin
   in ten a signal). Bins ≥ 10 dB over their floor are grouped (one-bin gaps bridged); a lone bin must be ≥ 15 dB.
   Only the **band's own span** is searched (the tuner's filter skirts are where false detections clustered on the
   Pi 500's RTL), never the DC remnant (±15 kHz).
2. **Candidate → channel** — a detection is a candidate until seen in a second frame; then a channel opens at the
   group's power centroid, **from a 6 ms pre-roll** kept in a 2¹⁸-sample ring, so the burst's start is not lost.
   Strongest first; a newcomer 25 dB under an open burst is its splatter (a strong OOK transmitter's keying edges).
   Up to 4 channels at once. A channel closes 25 ms after the last detection (PT2262's sync gap is ~15 ms), or at 1.6 s.
3. **Channel** — NCO mix and a **triangular (CIC-2) decimator** to ~250 kS/s. (A plain integrate-and-dump leaked a
   strong burst one output rate away onto DC: test-ism's TPMS decoded twice.)
4. **Slicer, at the burst's end** — the envelope (OOK) and the discriminator (FSK, when the envelope is ≥ 85 % on
   across the burst and the tones are ≥ 4 kHz apart), each sliced with hysteresis at the dB midpoint between the
   pre-roll's level and the burst's, **at four smoothings** (12, 36, 100, 244 µs for OOK; three for FSK). A 17 kbps FSK
   bit and a 1 kHz Oregon pulse want very different filters; the decoders try each, finest first. Output: signed run
   lengths in µs (+mark / −space). The burst's carrier (or the tones' midpoint) refines the reported frequency.
   A burst under 6.5 dB, or with > 6000 transitions, is not decoded (noise reads ~4.5 dB — measured).
5. **Bit slicers** (shared by every protocol): PWM, PPM, NRZ, Manchester (both conventions, re-phasing on a violation),
   differential Manchester; CRC-8 / CRC-16 (MSB-first) and the helpers.

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
  renumbers. (Our TPMS formats do not name a make — see the table; the nickname then reads "TPMS #2".)
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
| Current Cost transmitters (electricity) | 433 FSK 3918 bps | **done** | Jack Kelly, rfm_edf_ecomanager wiki | counter (gas/water) packets not shown yet |
| KeeLoq code-hopping fobs/remotes (Microchip HCS301 class) | 315/433 OOK PWM | **done, privacy** | Microchip HCS301 datasheet DS21143B | make unknown from the air: "KeeLoq" |
| EV1527 / PT2262 fixed-code doorbells, PIRs, contacts, gate remotes | 315/433 OOK PWM | **done, privacy** | EV1527/SC1527 and PT2262 datasheets | 2 identical words required |
| TPMS "type A" | FSK diff. Manchester | **done, privacy** (temperature) | Krstic et al., "Applying TPMS Devices for Traffic Management Purposes" (IHP/DLR) | **pressure scale not published** — shown without pressure; make not named |
| TPMS "type B" | OOK, 240 µs symbols | **done, privacy** (kPa = raw × 2.5, °C) | same paper | **line code and preamble not published**: Manchester/NRZ both tried, frame after an 8-bit run-in, latest start wins; confirm-twice |
| TPMS "type C" | FSK Manchester | not done | same paper | XOR checksum too weak without a published preamble; pressure scale not given |
| TPMS by manufacturer (Schrader, Continental/VDO, Huf/Beru, Pacific/Toyota, TRW…) | 315/433 | **not done: no non-GPL description** | — | **so "Toyota TPMS" cannot be named yet** |
| LaCrosse TX141TH-Bv2 / TX141-Bv3 | 433 OOK | not done: no non-GPL description | — | |
| AcuRite 5-in-1 (VN1TXC), 606TX | 433 OOK | not done: no non-GPL description | — | |
| Bresser 5/6/7-in-1 | 868 FSK | not done: no field description outside GPL-derived code | — | |
| Ambient Weather F007TH | 433 Manchester | not done: half-bit period and the digest's byte range unpublished | forum + README (partial) | |
| Efergy e2 / Elite | 433 FSK | not done: framing (preamble/sync) and bit rate unpublished | Elijah / Gough Lui blogs (layout only) | |
| Fine Offset WH24/WH65/WS68/WH51, Ecowitt | 433/868 FSK | not done: not found in the research pass | — | worth a second look |

Twelve families with their variants (≈ 25 models) in phase 1.

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
over another whole-radio mode (a listener's ISM or ADS-B in its 5 s leave grace — it waits), never where ISM cannot run
(`vsIsmWhyNot`: blocked, < 0.9 MS/s, the band outside the allowed ranges — the admin line says "cannot run — why", tried
again every minute, and the idle park is not held off for it). Kept-running ADS-B still owns the idle radio. Time slots
mix freely: NAVTEX by day, ISM 868 by night.

## CPU

Measured with `test-ism`'s CPU row and a front-end profile (10 s of 2.4 MS/s), niced:

| machine | idle band (noise) | busy band (10 bursts / 10 s) |
|---|---|---|
| Apple M4 (dev Mac), radix-2 | 0.8 % of one core | 1.0–1.3 % |
| Pi 500, Cortex-A76, arm64, PFFFT (the server's build) | 2.0–2.3 % | 2.9 % |
| Pi 500, the 32-bit-ARM hop (640 µs), PFFFT | 1.3–1.5 % | — |
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
