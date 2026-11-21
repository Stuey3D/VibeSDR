# The DIGI MODES MAP and the STATION STORE (V12, beta90)

Branch `b90-maps` (from `v12-experimental` be5b3f2af), 2026-11-21. Not merged, not pushed.

Stuart (2026-11-21), five decisions, all in scope (web AND app, server where needed):

1. **Background + live digital spots on ONE map** — one server-side STATION STORE fed by the background runs AND the
   listeners' live spots (FT8 / FT4 / FT2 / JS8 / WSPR / FST4 / Q65 / MSK144 / PSK / CW / RTTY …), each station once,
   filed per UTC day, bounded, an HTTP endpoint with a time window that keeps updating while spots are decoding.
2. **The DIGI MODES MAP** = the spots map (coloured by mode, legend) with layers: SPOTS and APRS ("aprs map gets combined
   with the rest of the digispots"). ONE opener on web and app: `openDigiModesMap({ layer?: 'spots' | 'aprs', … })`.
3. **VIEWS gets three map keys**: DIGI MODES · AVIATION · SHIPPING, each drawn only where the server has something for it.
4. **ONE aircraft map built on the ADS-B map** (adsblayer): rings, auto-fit, glide, heading from the last two fixes,
   every source drawn the ADS-B way.
5. **A scale bar on every map** (NM beside km on the marine and aviation maps).

## ★★★ THE C++ INTERFACE (for b90-report's PSK Reporter upload — stable from this commit)

Header: `android/app/src/main/cpp/vibe_station_store.h`, namespace `vibestations`. Header-only, no dependencies beyond
the standard library and POSIX (`flock`, `pread`, `pwrite`).

```cpp
namespace vibestations {

enum Origin : uint8_t { kBackground = 1, kLive = 2, kSatellite = 4 };

/** ONE RECEPTION, as a decoder heard it (what PSK Reporter uploads). */
struct Reception {
    std::string call;     // validated callsign, upper case ("G4ABC", "EA8/G4ABC/P")
    std::string grid;     // 4 or 6 character Maidenhead, upper case, "" when the message carried none
    std::string mode;     // "FT8" "FT4" "FT2" "JS8" "WSPR" "FST4" "Q65" "MSK144" "PSK31" "PSK63" "CW" "RTTY" …
    std::string band;     // "20m" … "" outside the band table
    double  rfHz = 0;     // the RF frequency it was heard on (dial + audio offset)
    int     snr  = 0;     // dB as the decoder reports it (FT8: in 2500 Hz)
    int64_t utc  = 0;     // UTC seconds — FT8-family: the slot's start
    uint8_t origin = 0;   // kBackground / kLive
    bool    newToday = false;   // the first reception of this call+mode+band in this UTC day (this process)
};

/** ONE STATION ROW: a call+mode+band, aggregated over one UTC day. */
struct Station {
    std::string call, grid, mode, band;
    double  freqHz = 0;          // the latest reception's
    int     bestSnr = 0, lastSnr = 0;
    uint32_t count = 0;
    int64_t first = 0, last = 0; // UTC seconds
    uint8_t origins = 0;         // Origin bits seen
    int64_t day = 0;             // the UTC day (seconds at 00:00)
};

class Store {
public:
    /** Feed one reception. Called on a DECODE thread; cheap (a map insert under a mutex). Invalid calls/grids dropped. */
    void spot(const Reception& r);
    /** ★ PSK REPORTER HOOK: called for EVERY accepted reception, synchronously, on the decoder's thread, AFTER the store
     *  has taken it, with no store lock held. Must be quick and must not call back into the store — queue it.
     *  Returns an id for unsubscribe(). */
    int  subscribe(std::function<void(const Reception&)> fn);
    void unsubscribe(int id);
    /** Every station row heard at or after `sinceUtc` (by its `last`), this machine's disk store + this process's
     *  unflushed rows, newest day first. `fn` returns false to stop. Rows are per call+mode+band per day. */
    size_t forEachSince(int64_t sinceUtc, const std::function<bool(const Station&)>& fn) const;
};

/** The process's store (the gallery directory once configured; RAM only before that / with no gallery). */
Store& shared();

}  // namespace vibestations
```

- **Per process.** Each radio is its own process (`vibeserver/main.cpp superviseRadios`). `subscribe` sees THIS radio's
  receptions only — which is what a PSK Reporter receiver report is (one receiver = one radio). `forEachSince` and the
  map read the merged disk store (every radio on the machine) plus this process's unflushed rows.
- **Fed from** `DecoderHost::emitSpot` (every digital-spots engine, Single and Band), the CW / PSK / RTTY skimmer and
  decoder spot paths — but only for a host whose `Env::stations` is set: the server's listener hosts (origin kLive) and
  background decoding (kBackground). The benchmark and the decoder-only sidecar leave it null: test spots never reach
  the map or PSK Reporter.

## What was built (2026-11-21, branch `b90-maps`)

| Part | Where | Tests |
|---|---|---|
| Station store | `android/app/src/main/cpp/vibe_station_store.h` | `vibeserver/test-station-store.cpp` **77/0** (`--write <dir>` = fixture) |
| Store wiring | `vibe_decoder_host.h` (`Env::stations`, `toStations_` from `emitSpot`, CW / PSK / RTTY skimmer + decoder, JTTY), `vibe_bgdecode.h` (`Params::stations`, origin kBackground), `local_sdr_shim.cpp` (`vsStations()`, listeners kLive, never the decoder-only sidecar; `GET /vibeserver/stations.json`), `vibe_gallery.h` (`sideBytes` counts `stations/`) | test-digi-spots (all passed), test-web-spots (store checks) |
| Web DIGI MODES MAP | `web/client/src/main.ts` (`openDigiModesMapImpl`, `spotsMapHtml` — layers, windows, polling, APRS, overlay), `web/client/src/digiMap.ts` (the opener), `src/utils/digiMapShared.ts` (pure parts, shared with the app) | test-web-spots **64+6**, test-web-ais-aprs **29/0** |
| VIEWS' three keys (web) | `index.html` (`#mapBtn` DIGI MODES, `#avMapBtn`, `#shipMapBtn`), `main.ts syncViewKeys / viewsProbe` | markup-nesting test unchanged (mapBtn still in viewsRow) |
| Gallery | `main.ts` — MAP on ft8 / cw / psk / rttyskim cards; the standing "DIGI MODES MAP · N stations heard" row (ALL and spots kinds, page 1) | — (not driven by a test yet) |
| One aircraft map | `web/mapkit/adsblayer.js` (`a.sparse`: tones on a 2 min / 10 min clock, glide 3.5 s up to 400 NM, heading from the last two fixes; `a.rowsExtra`; `shown()`), `src/services/aviationMap.ts avPlaneAdsb`, `web/client/src/tracks.ts` (TrackView `aircraft:true`), `MapOverlay.tsx` (kind 'avdata') | test-web-avmap **38/0** (5 new), test_adsb_layer 159/0 |
| Scale bars | `web/mapkit/vibemapgl.js createNow` + `vibemap.js attach` (on by default; `scale:'nautical'`), hosts: ADS-B, NDB, DSC, TrackView (marine/aero), MapOverlay | test-web-shipping-map **37/0** (km + NM), test-web-avmap, test-web-spots, test-web-ais-aprs |
| App | `SDRScreen.tsx` (station polling → `digiSpots`, APRS points, `openDigiModesMap` registered, VIEWS keys, AVIATION standalone + HFDL MAP → aviation map), `ModeSelector.tsx` (DIGI MODES label, AVIATION, SHIPPING), `MapOverlay.tsx` (age filter = server windows, APRS layer, overlay, adsblayer for avdata, scale), `src/services/digiStations.ts`, `src/services/digiModesMap.ts` | tsc clean, check-map-overlay passes; **no device run** |
| Copy | web tour (tutorial.ts DECODERS card), app MODE tour card, AboutOverlay release note | — |

### The endpoint

`GET /vibeserver/stations.json?window=1h|24h|7d|30d|all&since=<UTC s>` → `{now, window, since, total, count, truncated,
byMode:{MODE:n}, stations:[{call, grid, mode, band, freq, count, snr (best), lastSnr, first, last, src (1 bg | 2 live),
modes[], bands[], country, lat?, lon?, km?}]}`. One entry per call (the newest reception's mode/band/freq, the newest grid,
counts summed). `since` = only calls heard since (the live poll; `total` still counts the whole window). 7 d / 30 d start at
a UTC day boundary. A private gallery's stations need the owner's read ticket (403 otherwise). `{"off":1}` when the store
is switched off (`setEnabled(false)` — no owner switch wired yet, see decisions).

### Bounds and cost (measured on the M5 Pro, test-station-store `testCost`)

- ≤ 3072 rows a day in RAM per process and on disk (196 672 B a day file); a 4th radio's new stations on a full day are
  dropped and counted in the header. ≤ 5000 calls in an answer. Days pruned to a 16th of the gallery size (1–64 MB) and
  ≤ 400 files, counted inside the gallery's size.
- 200 000 spots into the RAM table: 35 ms (0.18 µs a spot). A full day's flush: 2.7 ms. A 30-day answer over a full day:
  5 ms / 387 KB; the past days are cached (one slot) so a live poll re-reads only today's file.
- Disk writes: one pwrite per changed row every 20 s, never per spot.

## RESUME HERE

**State 2026-11-21 (b90-maps, not merged, not pushed):** everything in the table above is built and committed; web tests
run against the real server (headless Edge, `--mute-audio`): test-web-spots, test-web-avmap 38/0, test-web-ais-aprs 29/0,
test-web-shipping-map 37/0; test-station-store 77/0; test_adsb_layer 159/0; tsc clean; check-map-overlay passes.
`android/app/src/main/cpp/vibe_web_page.h` was regenerated locally for the server tests and is NOT committed (the
coordinator's build-web at merge).

Owed, in order:
1. **App device run** — the DIGI MODES MAP on a VibeServer (age filter → windows, stored stations appear, APRS layer from the
   APRS box's MAP), the AVIATION MAP from VIEWS / HFDL MAP / DATALINK (aircraft drawn by adsblayer, gliding), the scale bars.
   Nothing app-side has run on a device.
2. **Live capture** — a background-decoding run on a real band for an hour, then `stations.json?window=24h` against the
   gallery's summary card counts (they should agree on calls; the store counts per call+mode+band per day).
3. The gallery's standing DIGI MODES MAP row and the cards' MAP keys have no test yet (test-web-gallery could add one with
   `test-station-store --write <galleryDir>` as the fixture).
4. The app's AVIATION key shows on capability only (ADS-B / DATALINK / HFDL here); the web also probes the shared aircraft
   table (another radio's aircraft). Same for SHIPPING (the app uses navAvail; the web probes the ship store too).
5. R36S (handheld-ui): no map keys there — nothing done, by design.

### Decisions for Stuart
- An owner switch for the store ("spotsmap", under Digital spots)? `Store::setEnabled` exists; nothing calls it — the store
  follows the gallery (no gallery = RAM only, today and yesterday).
- Station rows are per call+mode+band per DAY: the 1 H window shows calls whose LAST reception is within the hour, with the
  day's count.
- APRS on the web map: symbols, tracks, cards and weather — but no list panel or per-station track fetch as the old APRS
  TrackView had (the station list stays in the APRS box).
- Stations heard without a locator (CW, PSK, RTTY) are counted ("heard without a locator") but never placed.
