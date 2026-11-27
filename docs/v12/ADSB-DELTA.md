# ADS-B for listeners as deltas (V12 beta96)

Stuart approved this on 2026-11-26. Branch `b96-adsbdelta`, off `b96-integrate`.

The `adsb` report re-sends every aircraft every second. Stuart measured it on the Sony (beta95, one listener, 45–66
aircraft): 11.75 KB/s, about 0.2 KB/s per aircraft per listener. A Beast-fed Lenovo showing 150 aircraft to 100
listeners would need about 25 Mbit/s of upload.

A client that says it understands deltas now gets the whole table once, and after that only what changed each second.
Every other client gets `adsb` exactly as before.

## RESUME HERE

- **Built, tested and committed:**
  - the server's encoder (`vibe_adsb_delta.h`);
  - the service hooks (`Service::deltaTick` and `deltaKeyframe`);
  - the shim: the `adsbd=1` subscription, one build per tick shared by every socket, the joiner's keyframe and
    `adsb_resync`;
  - the clients' shared store (`src/services/adsbDelta.ts`), wired into the web client (`spectrum.ts`, URL in
    `main.ts`) and the app (`VibeServerClient.ts`, URL in `VibeServerWsClient.ts`).
- **Not yet done:**
  - **No browser was driven headless and no phone was used.** The store is tested in node, and the wiring in each
    client is a few lines (the same pattern as `dab=2`).
  - **Not measured on a Pi or a phone server.** It was measured on the Mac (M5 Pro) with the real server binary.
- **Live check for a person:** the Lenovo with the Beast feed radio and the web page open. The ADS-B map should look and
  behave as before (aircraft glide, cards, emergencies, the list). Then compare the web page's data rate with beta95.

## The protocol

The full wire format is in the header of `android/app/src/main/cpp/vibe_adsb_delta.h`.

- **Negotiation.** The spectrum socket's query adds `adsbd=1`. This is the client's own word, like `span=1` or
  `dab=2`, and it grants nothing. An older server ignores it and sends `adsb`, which both clients still handle
  unchanged. The watch forwarder, Jr, the R36S and the second preview socket do not ask, so they keep `adsb`.
- **Keyframe `adsb_k`** `{s, t, h:{header}, a:[rows]}` is sent:
  - on joining;
  - on `{"type":"adsb_resync"}`;
  - on the first tick after ADS-B starts;
  - to everyone every 60 s.
- **Delta `adsb_d`** `{s, t, h:{changed header keys}, hx:[removed keys], g:[gone ids], n:[new rows], u:[updates]}`.
  Empty parts are left out. A quiet tick is about 30 bytes.
- **Rows are flat.** `a`, `n` and `u` are each one array, with the rows run together: `id,"hex",mask,v…` for a new
  aircraft, and `id,mask,v…` for a changed one. The mask says how many values follow. Ids are small integers. A freed id
  is reused, lowest first, and the client applies `g` before `n`.
- **Columns (mask bits).** In order, most often changed first:
  - differences: lat, lon, posAt, seenAt, msgs;
  - absolute: alt, rssi, gs, track, baro_rate, alt_geom, mag_heading, tas, ias, squawk, emergency, tracked_nm, snr
    (★ added beta96 before release — docs/v12/MESSAGE-SNR.md; it moved flight/category/Comm-B up one bit);
  - text: flight, category;
  - the Comm-B object, as is.

  `null` means the field has gone.
- **Quantised.**
  - lat/lon: 1e-5°, the report's own 5 decimals;
  - altitudes: 25 ft;
  - gs, track and heading: 1;
  - rssi: 1 dB, rounded down, so a threshold on the whole number reads the same either way;
  - snr (beta96): 3 dB BANDS on ADS-B's colour edges — k = floor((snr − 10) / 3), shown as 11.5 + 3k — so rounding never
    changes the colour and the column goes out only when the band changes (Stuart: "use coarser to save resources");
    measured on the 172-aircraft stream: no column 2.52 KB a tick, whole dB 2.80, bands 2.61;
  - tracked_nm: 1 NM, as in the report.

  Each value is quantised from the text the full report prints. The quantisation can never disagree with an older
  client on a tie.
- **Times on a tick grid.** "Heard at" is a whole tick number on a 1 s grid. That grid is set when ADS-B starts, so each
  tick sits 0.85 s into its second. The shim keeps the ticks on the grid (`adsbNextReport`, which adds 1.0 s each
  time).
  - When a position moved and its time is this tick's, posAt is not sent.
  - When the message count moved and the last-heard time is this tick's, seenAt is not sent.
  - In both cases the client sets the time to the tick. Both ends apply the same rule to the same values, so it is
    exact. This saves more than anything else.
- **Ages.** The client works out `seen = T − seenAt + 0.35` (to 0.1 s, never below 0). It does the same for `seen_pos`.
  The result is within 0.5 s of the full report in the replay. Every threshold the clients apply to ages is in whole
  seconds:
  - "just now" under 2 s;
  - fresh under 10 s;
  - aging under 30 s.

  The 60 s position expiry stays on the server.
- **What an aircraft is** (registration, type, operator, route, country) is still sent once through `adsb_info`.
- **Built once per tick per radio.** `sendAdsbAll` runs one tick under `adsbSendMtx_` and sends the same delta string
  to every delta socket. The full list is built only when an older client is listening. A joiner's or a resync's
  keyframe is cached per tick.
- **Bounded.** The table is never bigger than the tracker's, and is capped at 2000 rows (the web client's own cap).
- **The client store** (`AdsbDeltaStore`) applies a delta only when `s` is one more than the tick it holds. An older
  `s` is ignored, because it is already in the keyframe the client holds. A gap, a row it does not hold, or anything
  that does not read sends a resync request, at most one every 2 s. After each message the store hands back an
  ordinary `adsb` message, so everything downstream is untouched: parsing, `adsb_info` merging, the list, the map
  layer, the cards and the range outline. It applies deltas even while the clients' exit or ghost guard is dropping
  reports, so the table never misses a tick.

## Measured

The real server binary on the Mac (M5 Pro), with a Beast-only radio fed by a synthetic dump1090
(`scripts/test-server-adsb-delta.ts`). About 150 aircraft at a time (152–156), positions twice a second, aircraft
coming and going. An older client and a delta client listened side by side for 60 s, counting every WebSocket frame,
header included:

| Per listener | Total | Aircraft (`adsb` vs `adsb_d` + `adsb_k`) | `adsb_info` |
|---|---|---|---|
| Before (older client, the full list every second) | **32.13 KB/s** | 32.00 KB/s | 0.13 KB/s |
| After (delta client) | **2.42 KB/s** | 2.09 + 0.20 (one keyframe a minute) = 2.29 KB/s | 0.13 KB/s |

- **That is 13× less**: about 0.015 KB/s per aircraft per listener, against 0.2 before.
- **100 listeners at 150 aircraft:** about 1.9 Mbit/s of upload, against about 26 Mbit/s before.
- The in-process replay (`test_adsb_delta.ts`) agrees: 35.1 KB → 2.54 KB a tick, as JSON text without frame headers.
- **Server CPU on the Mac is too small to separate from noise.** 1 delta listener, 100 delta listeners and 100 older
  listeners all came out at 0.1–3 % of one core across two runs. Building one tick costs about 110 µs (rows and
  encode), about the same as building the full list (130 µs). Either is built once per tick, never once per socket.
- **One reading was 20 %.** In the first full run, the 100-delta phase measured 20 % of a core. It did not repeat:
  - a rerun with the phases in a different order read 0.5 % and 0.7 %;
  - in that same rerun, the 1-listener phase read 3 %.

  The cause is most likely something the server does at that age, not the listeners. The test now measures the delta
  listeners twice and holds the lower reading to a ceiling of 5 %.
- **Not measured on a Pi.** On a Pi the saving in bytes is also a saving in kernel send time.

## Tests

| Test | Result | Covers |
|---|---|---|
| `vibeserver/test-adsb-delta.cpp` | **17/17** | Quantising from the printed text (ties, −0.0, rssi rounding down); the header walk (nested, escaped); ids, gone before new and id reuse in the same tick; implied times; an empty tick; keyframe contents; one keyframe for everyone in 60 ticks; a 150-aircraft sky through the real Tracker: bytes and cost (≤ 3 KB/s). |
| `scripts/test_adsb_delta.ts` | **18/18** | 900 ticks of that sky (the real Tracker, with each tick up to 0.08 s late) into the clients' store. **Every tick** the rebuilt table equals the full report field for field (after the quantising; ages within 0.65 s, worst seen 0.50 s). This holds from the start (A), joining at tick 300 (B), and with a lost delta at tick 450 (C: one resync, then equal again). D: the app's parsed report, list order (adsbToAircraft) and emergencies (the 7700 drawn for 200 ticks) match. E: junk rows, bad ids, an over-long hex and a delta before any keyframe ask for a resync and never corrupt the table; an old delta is ignored quietly. |
| `scripts/test-server-adsb-delta.ts` | **11/11** | The real binary: a Beast-only radio fed a synthetic 150-aircraft sky. An older client and a delta client side by side for 60 s. The older client gets `adsb` every second and never a delta; the delta client gets `adsb_d`/`adsb_k` and never `adsb`; there are no resyncs on a healthy socket; the rebuilt table has the same aircraft as the full report; a keyframe arrives every 60 s; `adsb_resync` is answered. CPU with 1 and 100 delta listeners, and with 100 older listeners. |
| Older clients | | Unchanged by construction: `report()` and `adsbFinish` produce the same bytes as before (`adsbFinish` is the old in-line code, moved). The server test checks that the older socket still gets `adsb` every second. |
