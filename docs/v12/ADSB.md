# ADS-B 1090 (V12): the server contract and the client behaviour

Our own decoder (`android/app/src/main/cpp/vibe_adsb.h`, service `vibe_adsb_service.h`), wired into VibeServer as a
whole-radio mode like DAB. Branch `v12-adsb`. Memory: `adsb_v12`.

## Server contract

**Capability** — `/vibeserver.json` (ask EACH radio, `/r/<id>/vibeserver.json`, never the front door — as for DAB):

| field | meaning |
|---|---|
| `"adsb": true` | this receiver can run ADS-B now |
| `"adsb": false` + `"adsbWhy": "…"` | it cannot; the reason in words. Show the control **disabled with this reason** (never hidden) |
| no `adsb` field | an older server: no control |
| `"modes"` contains `"adsb"` | same as `adsb: true` (directory badge) |

Reasons the server gives: the owner switched it off; a transceiver; centre locked by the owner; the radio cannot
tune to 1090 MHz; **the capture is below 2.4 MS/s** ("ADS-B needs a capture of 2.4 MS/s or more — this receiver runs
at 2.048 MS/s"). ADS-B **never changes the sample rate** (Stuart, 2026-10-24: the 32-bit re-rate teardown).

**Client → server** on the spectrum socket: `{"type":"adsb","on":1}` / `{"type":"adsb","on":0}` (missing `on` = off).
While ADS-B is on, `tune`, `mode`, `bandwidth`, `zoom`, `reset`, `tunerbw`, `sampleRate` are **ignored** — the
client must disable those controls (as in DAB). Entering ADS-B from DAB is allowed: the server leaves DAB itself
(clients get `dab_off`). Entering DAB from ADS-B likewise sends `adsb_off` first. Gated by ask-before-tuning.

**Server → client** (JSON text frames, to every spectrum socket):

- `{"type":"adsb", "now":s, "messages":n, "aircraft":[…], "rate":Hz, "noise":dBFS, "msgRate":per s, "dropped":n}`
  once a second while on, and immediately to a socket that joins. **Its arrival means ADS-B is on: open the box.**
- `{"type":"adsb_off"}` then a `config` — close the box, the receiver is back where it was.
- `{"type":"adsb_error","why":"…"}` — refused; show the reason.

**An aircraft** (dump1090 `aircraft.json` field names; absent = unknown):

| field | | field | |
|---|---|---|---|
| `hex` | ICAO address, 6 hex (`~` prefix = non-ICAO) | `gs` | ground speed, kt |
| `flight` | callsign | `track` | track over ground, ° |
| `category` | `A0`–`D7` emitter category | `mag_heading` | heading, ° (airspeed messages) |
| `lat`,`lon` | position (≤ 60 s old) | `tas` / `ias` | airspeed, kt |
| `seen_pos` | seconds since the position | `baro_rate` | vertical rate, ft/min |
| `alt_baro` | ft, or `"ground"` | `squawk` | 4 digits, string |
| `alt_geom` | GNSS height, ft | `emergency` | non-zero = emergency state |
| `seen` | seconds since any message | `messages`, `rssi` | count, dBFS |

Aircraft not heard for 60 s are dropped by the server.

**Category → icon** (DO-260B emitter category):
A1 light (< 7 t: light prop) · A2 small (7–34 t: regional/bizjet) · A3 large (34–136 t: narrow-body jet) ·
A4 high-vortex large (B757) · A5 heavy (> 136 t: wide-body/jumbo) · A6 high performance (> 5 g, > 400 kt: fighter) ·
A7 rotorcraft (helicopter) · B1 glider · B2 lighter-than-air (balloon) · B3 parachutist · B4 ultralight · B6 UAV ·
B7 space vehicle · C1 emergency surface vehicle · C2 service surface vehicle · C3 obstacle · A0/none = generic jet.

## Client behaviour (Stuart, 2026-10-23/24)

- **Like DAB:** choosing ADS-B tunes the whole radio to 1090 and **opens the decoder box**; the box lists aircraft
  (the existing `AircraftPanel` in the app) and has a **MAP button in its header**.
- **The map** (our own MapLibre maps, GPU — the same stack as the HFDL map; in the web client too):
  - **auto-zoom** (toggle): the view always just contains every aircraft with a position; one appearing 200 NM out
    pulls the view out; when it drops off the view comes back in to the next furthest. Smooth, not jumpy.
  - **fit to aircraft** (toggle): the receiver need not be centred — if traffic is mostly to the south-east the
    receiver moves towards the top-left so more of it fits at a closer zoom. Off = receiver centred.
  - **range rings** (toggle) in NM around the receiver.
  - The three toggles live **inside the map**.
  - **New aircraft breathe** softly (or a ring as on the HFDL map) — **only after the initial load**, never the
    first batch, or everything flashes.
  - **Type icons** from `category` (helicopter, heavy, medium/large jet, light prop, fighter, …), rotated to track.
  - **Smooth glide** between position updates.
  - **Airports and runways** as on the HFDL map.
- The receiver position is the server's own location (`/location`), as the HFDL/spots maps already use.
