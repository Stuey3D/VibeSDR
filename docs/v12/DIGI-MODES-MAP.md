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

## RESUME HERE

(being written — see the commits on `b90-maps`)
