// vibe_adsb_range_feed.h — b96-rangefeed: THE RANGE OUTLINE COUNTS EVERY ADS-B POSITION THIS SERVER HOLDS, not only the ones
// this radio's own demodulator decoded (docs/v12/RANGE-FEED.md).
//
// ★★ Stuart, 2026-11-27, the Lenovo: "range ring still too slow to update". Its radio has no ADS-B; its aircraft come from
//    a server-level Beast feed (vibe_feeds.h — PiAware in the same house). GET /adsb/range answered every bin 0 with 127
//    aircraft on the map: the outline (vibe_adsb_range.h) was fed only by vibe_adsb_service.h's own Tracker, so every map
//    started from nothing and drew only what that page had seen since it opened.
// ★★★ ONE OWNER EACH: the FRONT DOOR owns the feeds (vibe_feeds.h); the RADIO process owns its outline and answers
//     /adsb/range (each radio is its own process). They already meet in ONE place: the shared aircraft table
//     (vibe_aircraft_share.h, <runtime dir>/aircraft-live.dat, every writer's ADS-B positions with their fix times). So the
//     radio process READS it — every kEverySec, only while the table says some ADS-B is being decoded on this machine (its
//     header's stamp: a radio's or a feed's) — and offers each believed position to its own outline. Nothing crosses the
//     owners; the front door does nothing new. On a SIMPLE server (one process) and the Android server it is the same
//     table and the same code.
// ★★ WHAT COUNTS: a row whose position came from ADS-B (Mode S — never a datalink fix: ACARS / VDL2 / HFDL / ADS-C) and is
//    fresh (≤ kFreshSec old), on its SECOND consistent fix in a row (the step flown at ≤ 1 200 kt, at least 1 s assumed) —
//    the same posOk rule as the radio's own Tracker and the map's live outline, so one bad decode never makes a spike. A
//    row heard ONLY by this radio is left to its own service (it already offered every one of those, at a second's rate).
//    Everything else — a Beast / BaseStation ADS-B feed, the other radios of this server (the door's loopback ingest tags
//    theirs "feed" too) — counts, centred on THIS SERVER'S location (the receiver location every radio here shares).
//    ⚠ A feed's receiver is assumed to be AT this site. One elsewhere would stretch the outline toward it; the feed's
//    settings could carry an "at this site" flag later (RANGE-FEED.md).
// ★ BOUNDED: one 264 KB pread of a tmpfs file every kEverySec while ADS-B is live (each radio's writer already reads the
//   whole table every 2 s), and one small entry per aircraft for at most kForgetSec, never more than kMaxTracked.
#pragma once
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/stat.h>

#include "vibe_adsb_range.h"
#include "vibe_aircraft_share.h"
#include "vibe_log_radio.h"
#include "vibe_thread.h"

namespace vibeadsb {

class TableRangeFollower {
public:
    static constexpr int64_t kFreshSec = 10;     ///< a position older than this when read proves nothing about range now
    static constexpr int64_t kForgetSec = 120;   ///< an aircraft unheard this long starts its pair again
    static constexpr double kMaxKt = 1200;       ///< the posOk rule's flying limit (vibe_adsb.h, adsblayer.js RANGE_MAX_KT)
    static constexpr size_t kMaxTracked = 2 * vibeavshare::kCap;
    static constexpr double kEverySec = 2.0;     ///< the table's own write rate (vibeavshare::kFlushSec)

    /** One look at the table's rows. `ownBit`: this radio's bit in the table's registry (0 = unknown / none — then every
     *  ADS-B row counts; offering this radio's own again is harmless, the outline is a maximum). Returns how many
     *  positions were offered (`grew`: how many of those grew a bearing). */
    int tick(const std::vector<vibeavshare::Row>& rows, uint32_t ownBit, int64_t nowMs, RangeStore& st, int src, int* grew = nullptr) {
        int offered = 0, g = 0;
        const int64_t utc = nowMs / 1000;
        for (const auto& w : rows) {
            if (!w.icao || !(w.flags & 1) || w.posSrc != vibeavshare::kAdsb) continue;   // ★ ADS-B positions only
            if (!(w.lat == w.lat) || !(w.lon == w.lon) || std::fabs(w.lat) > 90 || std::fabs(w.lon) > 180) continue;
            if (ownBit && w.radios == ownBit) continue;                                  // ★ this radio's own: its service's
            if (nowMs - w.posMs > kFreshSec * 1000 || w.posMs > nowMs + 5000) continue;
            auto it = last_.find(w.icao);
            if (it != last_.end() && w.posMs <= it->second.posMs) continue;              // ★ the same fix again
            if (it != last_.end()) {
                const double dt = std::max(1.0, double(w.posMs - it->second.posMs) / 1000.0);
                const double nm = RangeOutline::distKm(it->second.lat, it->second.lon, w.lat, w.lon) / 1.852;
                if (nm / dt * 3600.0 <= kMaxKt) {
                    ++offered;
                    if (st.offer(src, w.lat, w.lon, utc)) ++g;
                }
            }
            last_[w.icao] = { w.lat, w.lon, w.posMs };
        }
        for (auto it = last_.begin(); it != last_.end();) {
            if (nowMs - it->second.posMs > kForgetSec * 1000) it = last_.erase(it); else ++it;
        }
        if (last_.size() > kMaxTracked) last_.clear();   // ★ never grows past its bound (a pair simply starts again)
        if (grew) *grew = g;
        return offered;
    }
    size_t tracked() const { return last_.size(); }
    void reset() { last_.clear(); }

private:
    struct Last { double lat, lon; int64_t posMs; };
    std::unordered_map<uint32_t, Last> last_;
};

/** This radio's bit in the table's registry (radios.idx beside it), 0 when it has none (no directory, no id yet). */
inline uint32_t ownRadioBit() {
    const std::string me = vibe::logradio::mine();
    if (me.empty()) return 0;
    const std::vector<std::string> reg = vibeavshare::table().radioRegistry();
    for (size_t b = 0; b < reg.size() && b < 32; ++b) if (reg[b] == me) return 1u << b;
    return 0;
}

/** ★ One pass, as the thread runs it (tests call it directly): reads the table if some ADS-B is live on this machine,
 *  keeps the outline centred on the server's location, offers the rows. Returns the positions offered (-1 = not read). */
inline int followTableOnce(TableRangeFollower& f, double lat, double lon, int64_t nowMs, int src = kRangeAdsb) {
    auto& t = vibeavshare::table();
    const std::string p = t.path();
    struct stat sb{};
    if (!p.empty() && ::stat(p.c_str(), &sb) != 0) return -1;   // ★ no table on this machine yet: never make one to read it
    int64_t liveMs = 0;
    std::vector<vibeavshare::Row> rows = t.read(nowMs, nowMs - TableRangeFollower::kForgetSec * 1000, nullptr, &liveMs);
    if (!vibeavshare::adsbLiveNow(liveMs, nowMs)) { f.reset(); return -1; }   // ★ no ADS-B here now: nothing to count
    if (!(lat == lat) || !(lon == lon) || (lat == 0 && lon == 0)) return -1;   // ★ no location: an outline has no centre
    rangeStore().setCentre(lat, lon, nowMs / 1000);   // ★ a no-op unless it moved > kMovedKm (then a new site's outline)
    return f.tick(rows, ownRadioBit(), nowMs, rangeStore(), src);
}

/** ★ Start the follower for this radio process (once; later calls do nothing). `where` gives the server's location
 *  (false = not known yet). The thread is a reader at the Decoders' priority: it never touches a DSP or audio thread. */
inline void startTableRangeFollower(std::function<bool(double&, double&)> where) {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    std::thread([where] {
        vibeDecoderThread("vibe-adsbrange");   // ★ decoder priority (vibe_thread.h): below network, audio and spectrum
        TableRangeFollower f;
        for (;;) {
            double lat = NAN, lon = NAN;
            if (where && where(lat, lon)) followTableOnce(f, lat, lon, vibeavshare::wallMs());
            std::this_thread::sleep_for(std::chrono::milliseconds(int(TableRangeFollower::kEverySec * 1000)));
        }
    }).detach();
}

} // namespace vibeadsb
