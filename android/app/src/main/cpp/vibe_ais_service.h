// vibe_ais_service.h — V12 AIS mode's decoder service: the front end on its own thread, the ship list, the JSON the
// clients draw, a ship's track, and the gallery's copy. docs/v12/AIS-APRS.md.
//
// ★★★ OFF THE DSP THREAD, AND IT DROPS RATHER THAN WAITS — ADS-B's and ISM's rule: Network > Audio > Spectrum > Decoders.
//     feed() copies and returns; a full queue (half a second) loses the OLDEST block and counts it (the health pill's
//     "too heavy", vibe_health.h BehindWatch).
// ★★ ONE ROW PER MMSI, MERGED FROM EVERY MESSAGE: a ship's position (1/2/3 or 18/27) and its name, callsign, type, size
//    and voyage (5, or 19/24 for class B) arrive in different messages minutes apart; the row carries whatever has been
//    heard. Kept 60 min after last heard (an anchored class A reports every 3 min, its name every 6), at most 1000 rows.
#pragma once
#include "vibe_ais.h"
#include "vibe_ais_asm.h"     // ★ V12 2026-11-16 binary messages 6/8: met/hydro, area notices, text
#include "vibe_station_sink.h"
#include "vibe_adsb_capture.h"     // vibeadsb::CaptureMeter — the same live "capture N %" as ADS-B and ISM

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vibeais {

inline std::string jsonEsc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    return o;
}
inline std::string fmtNum(double v, int dp) { char b[32]; std::snprintf(b, sizeof b, "%.*f", dp, v); return b; }

/** ROT_AIS → degrees a minute (M.1371 Table 45: ROT = 4.733 √|rate|, signed). NaN for none / "turning, no indicator". */
inline double rotDegMin(int r) {
    if (r == -128 || r == 127 || r == -127) return NAN;
    const double v = r / 4.733;
    return r < 0 ? -v * v : v * v;
}

/* ★★★ V12 beta95 IMPLAUSIBLE POSITIONS (Stuart, 2026-11-25: "no way that can be right" — a yacht moored in Schiedam
 *  plotted in the Gulf of Guinea, 3 112 NM away, a dashed line from Rotterdam to it). The rules, in Stuart's words:
 *   · NO DISTANCE REJECTION ("i dont want to rule out distance" — he has heard Spanish FM in Newcastle on Sporadic-E).
 *     Distance never hides a ship. Further than kDxNm from the receiver = a DX CANDIDATE: "unconfirmed" (listed, no
 *     dot) until a second report AGREES (the same place, or reached at a believable speed), then plotted with a "DX" tag.
 *   · THE WHOLE-MINUTE CHECK rejects the fakes: a moving station's latitude AND longitude both on whole minutes
 *     (vibe_ais.h Msg::wholeMin) is a transponder with no fix sending a default — dropped, counted ("position
 *     implausible" while it has nothing better), never believed.
 *   · A SINGLE REPORT THAT JUMPS far from a ship's known track (faster than kShipMaxKn from the last believed fix) is HELD,
 *     not believed. A second report agreeing with the held one believes it (the ship really is there — its track starts
 *     again, no line across the map); one agreeing with the track drops it.
 *   · A position that is not believed never enters the trail, so never the ship store's track (vibe_ais_store.h). */
static constexpr double kDxNm = 75;        ///< further than this from the receiver: a DX candidate (AIS is ~20–40 NM)
static constexpr double kAgreeNm = 0.3;    ///< two reports this close agree, whatever the time between them
static constexpr double kShipMaxKn = 60;   ///< the fastest a surface station believably moves (fast craft ~45 kn)
static constexpr double kSarMaxKn = 600;   ///< a SAR aircraft (type 9)
inline double gcNm(double la1, double lo1, double la2, double lo2) {
    const double r = M_PI / 180, a = std::sin((la2 - la1) * r / 2), b = std::sin((lo2 - lo1) * r / 2);
    const double h = a * a + std::cos(la1 * r) * std::cos(la2 * r) * b * b;
    return 2 * 6371.0088 * std::asin(std::min(1.0, std::sqrt(h))) / 1.852;
}
/** Does a report at (la, lo, t) agree with one at (la0, lo0, t0) — the same place, or reached at ≤ maxKn? */
inline bool agrees(double la0, double lo0, double t0, double la, double lo, double t, double maxKn) {
    const double d = gcNm(la0, lo0, la, lo);
    if (d <= kAgreeNm) return true;
    return d / std::max(1.0, t - t0) * 3600.0 <= maxKn;
}
/** A station that moves (its own GPS): ships, SAR aircraft, SARTs. AtoNs and base stations send surveyed positions,
 *  which may well be round. */
inline bool movingType(int type) { return type == 1 || type == 2 || type == 3 || type == 18 || type == 19 || type == 9; }

struct Vessel {
    uint32_t mmsi = 0;
    std::string kind = "A";          ///< A | B | base | aton | sar | sart
    std::string name, callsign, dest, text;
    uint32_t imo = 0;
    int shipType = -1, nav = -1, heading = -1, rot = -128, atonType = -1, altM = -1;
    double lat = NAN, lon = NAN, sog = NAN, cog = NAN, draught = NAN;
    int bow = -1, stern = -1, port = -1, stbd = -1;
    int etaMonth = 0, etaDay = 0, etaHour = 24, etaMin = 60;
    bool virt = false, offPos = false;
    int atonStatus = -1;             ///< ★ type 21's status byte (vibe_ais_asm.h atonStatus: light / RACON / alarm)
    // ★★ V12 2026-11-16 MET/HYDRO (binary message 8, DAC 1 FI 31 / 11): the station's latest readings, per SITE — one
    //    base station may send the weather of several places, each with its own position. At most kMetSites, the
    //    oldest site gives way. metAt = service time each was heard.
    static constexpr int kMetSites = 4;
    asmsg::Met met[kMetSites]; double metAt[kMetSites] = {}; int nMet = 0; double metLoggedAt = -1e9;
    double first = 0, last = 0, posAt = -1;
    int count = 0, ch = 0;
    float level = -120, snr = 0;
    // ★ the speed trend (the card's ▲/▼): the speed a minute ago
    double sogRef = NAN, sogRefAt = -1; int trend = 0;
    struct Pt { double t, lat, lon; };
    std::deque<Pt> trail;
    std::string logLine, loggedName; double loggedAt = -1e9;
    double storedAt = -1, storedTrailT = -1;   ///< ★ V12 the ship store (vibe_ais_store.h): what was last handed to it
    // ★★ beta95 implausible positions (kDxNm above): a report held, not believed (pAt < 0 = none); whole-minute fakes
    double pLat = NAN, pLon = NAN, pAt = -1;
    int held = 0, fakes = 0;
    bool dx = false;       ///< the believed position is further than kDxNm from the receiver (confirmed by two reports)
    bool coarse = false;   ///< the believed position came from type 27 (1/10-minute grid): no whole-minute test on it
};

inline std::string displayName(const Vessel& v) {
    if (!v.name.empty()) return v.name;
    if (!v.callsign.empty()) return v.callsign;
    return "MMSI " + std::to_string(v.mmsi);
}
inline std::string compass8(double deg) {
    static const char* p[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return p[int(std::floor(std::fmod(deg + 22.5 + 3600.0, 360.0) / 45.0)) & 7];
}
/** ★ THE READABLE LINE (the list, the gallery): "WILSON GOOLE · cargo · 9.8 kn W · under way using engine → ROTTERDAM". */
inline std::string readable(const Vessel& v) {
    static const char* const dot = " \xc2\xb7 ";
    std::string s = displayName(v);
    const Mid* f = flagOf(v.mmsi);
    if (v.kind == "aton") {
        s += dot; s += atonTypeName(v.atonType)[0] ? atonTypeName(v.atonType) : "aid to navigation";
        if (v.virt) s += " (virtual)";
        if (v.offPos) s += dot + std::string("OFF POSITION");
        const asmsg::AtonStatus st = asmsg::atonStatus(v.atonStatus);
        if (st.valid && st.light == 3) s += dot + std::string("LIGHT FAULT");
        if (st.valid && st.racon == 3) s += dot + std::string("RACON FAULT");
        if (st.valid && st.alarm) s += dot + std::string("ALARM");
        return s;
    }
    if (v.kind == "base") { s += dot + std::string("base station"); if (f) s += dot + std::string(f->name); return s; }
    if (v.kind == "sar") { s += dot + std::string("SAR aircraft"); if (v.altM >= 0) s += dot + std::to_string(v.altM) + " m"; }
    if (v.kind == "sart") { s += dot + std::string("AIS-SART / MOB / EPIRB — DISTRESS BEACON"); return s; }
    const std::string ty = shipTypeName(v.shipType);
    if (!ty.empty()) s += dot + ty;
    if (v.sog == v.sog) {
        s += dot + fmtNum(v.sog, v.sog < 10 ? 1 : 0) + " kn";
        if (v.sog >= 0.5 && v.cog == v.cog) s += " " + compass8(v.cog);
    }
    if (v.nav >= 0 && navStatusName(v.nav)[0] && v.nav != 0) s += dot + std::string(navStatusName(v.nav));
    if (!v.dest.empty()) s += " \xe2\x86\x92 " + v.dest;   // →
    if (v.dx) s += dot + std::string("DX");
    else if (v.lat != v.lat && v.pAt >= 0) s += dot + std::string("DX? unconfirmed position");
    else if (v.lat != v.lat && v.fakes > 0) s += dot + std::string("position implausible");
    return s;
}

struct Health { double noiseDb = -120; double clipFrac = 0; uint64_t msgs = 0; bool valid = false; };

class Service {
public:
    static constexpr size_t kMaxVessels = 1000;
    static constexpr double kKeepSec = 3600;
    static constexpr size_t kTrailMax = 360;

    ~Service() { stop(); }

    /** ★ beta95 the receiver's place (the DX rule — kDxNm). NaN = unknown: then no report is a DX candidate. */
    void setReceiver(double lat, double lon) {
        std::lock_guard<std::mutex> lk(mtx_);
        const bool ok = lat == lat && lon == lon && std::fabs(lat) <= 90 && std::fabs(lon) <= 180 && !(lat == 0 && lon == 0);
        rxLat_ = ok ? lat : NAN; rxLon_ = ok ? lon : NAN;
    }

    /** Start with the radio tuned to `hwCentreHz` at `rateHz`. `keepList`: a restart on the same channels (the rate
     *  changed underneath) keeps the ships; entering the mode clears. `sink`: the gallery's day log (or null). */
    void start(double hwCentreHz, double rateHz, bool keepList = false, std::shared_ptr<vibestation::Sink> sink = nullptr) {
        stop();
        std::lock_guard<std::mutex> lk(mtx_);
        sink_ = std::move(sink);
        hwCentre_ = hwCentreHz; rate_ = rateHz;
        fe_.reset(new Front(rateHz, hwCentreHz));
        fe_->seedAfc(afcHz_);   // ★ the crystal's error from the last run (Front::afcStep_): no settling again
        queue_.clear(); queued_ = 0; dropped_ = 0;
        if (!keepList) { t0_ = std::chrono::steady_clock::now(); ships_.clear(); msgs_ = 0; fakePos_ = held_ = relocated_ = 0; perCh_[0] = perCh_[1] = 0; repaired_ = 0; other_ = 0; safety_.clear(); notices_.clear(); binary_ = 0; rateWin_.clear(); }
        cap_.reset(rateHz, now());
        storeLast_ = now();
        run_ = true;
        worker_ = std::thread([this] { loop(); });
    }
    /** ★★ V12 2026-11-02 THE SHIP STORE (vibe_ais_store.h): every kStoreEverySec, and when AIS stops, the ships that
     *  changed since the last hand-over are COPIED out (the state and the trail points since) and given to `fn` with
     *  the service's lock RELEASED — the disk write never holds up feed() (the DSP thread). Null = no store (the owner
     *  keeps no gallery, or switched the ship store off). Set before start(). */
    using StoreFn = std::function<void(std::vector<Vessel>&& changed, double svcNow)>;
    static constexpr double kStoreEverySec = 30;
    void setStore(StoreFn fn) { std::lock_guard<std::mutex> lk(mtx_); storeFn_ = std::move(fn); storeLast_ = now(); }
    /** ★ For tests: hand over now. */
    void flushStoreForTest() { flushStore_(); }
    void stop() {
        { std::lock_guard<std::mutex> lk(mtx_); if (!run_) return; run_ = false; }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        flushStore_();
        std::shared_ptr<vibestation::Sink> s;
        { std::lock_guard<std::mutex> lk(mtx_); s.swap(sink_); }
        if (s) s->finish();
    }
    bool running() const { return run_.load(std::memory_order_relaxed); }
    uint64_t droppedBlocks() { std::lock_guard<std::mutex> lk(mtx_); return dropped_; }
    size_t vesselCount() { std::lock_guard<std::mutex> lk(mtx_); return ships_.size(); }
    Health health() { std::lock_guard<std::mutex> lk(mtx_); return health_; }

    /** From the DSP thread: copy and return. Never blocks on the decoder. */
    void feed(const cf* iq, size_t n) {
        if (!run_.load(std::memory_order_relaxed) || !n) return;
        std::vector<cf> b(iq, iq + n);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            cap_.add(n, now());
            queued_ += n;
            queue_.push_back(std::move(b));
            while (queued_ > size_t(rate_ * 0.5) && queue_.size() > 1) { queued_ -= queue_.front().size(); queue_.pop_front(); ++dropped_; }
        }
        cv_.notify_one();
    }

    /** ★ For tests and the offline tool: one frame as the worker takes it (channel 0/1), with no thread. */
    void acceptForTest(int ch, const RawFrame& f) { std::lock_guard<std::mutex> lk(mtx_); accept_(ch, f, now()); }
    /** ★ beta95 a frame at a service time of the test's choosing (the jump / DX rules judge speed over time). */
    void acceptAtForTest(int ch, const RawFrame& f, double t) { std::lock_guard<std::mutex> lk(mtx_); accept_(ch, f, t); }
    /** ★ beta95 one ship's row as the report draws it ('' = not heard). */
    std::string vesselJsonForTest(uint32_t mmsi) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = ships_.find(mmsi);
        if (it == ships_.end()) return std::string();
        std::string o; vesselJson(it->second, now(), o); return o;
    }

    /** {"type":"ais", "centre", "rate", "now", "msgs", "msgRate", "chA", "chB", "repaired", "dropped", "capture", "noise", "afc",
     *  "vessels":[…], "safety":[…]} */
    std::string report() {
        std::lock_guard<std::mutex> lk(mtx_);
        const double t = now();
        prune_(t);
        while (!rateWin_.empty() && t - rateWin_.front() > 60) rateWin_.pop_front();
        const double span = std::min(60.0, std::max(1.0, t));
        char b[400];
        std::snprintf(b, sizeof b, "{\"type\":\"ais\",\"centre\":%.0f,\"rate\":%.0f,\"now\":%.1f,\"msgs\":%llu,\"msgRate\":%.2f,"
                      "\"chA\":%llu,\"chB\":%llu,\"repaired\":%llu,\"other\":%llu,\"dropped\":%llu,\"capture\":%d,\"noise\":%.1f,\"afc\":%.0f,"
                      "\"implausible\":%llu,\"heldPos\":%llu,\"vessels\":[",
                      kCentreHz, rate_, t, (unsigned long long)msgs_, rateWin_.size() / span * 60.0,
                      (unsigned long long)perCh_[0], (unsigned long long)perCh_[1], (unsigned long long)repaired_,
                      (unsigned long long)other_, (unsigned long long)dropped_, cap_.percent(t), health_.noiseDb, afcHz_,
                      (unsigned long long)fakePos_, (unsigned long long)held_);
        std::string o = b;
        std::vector<const Vessel*> v;
        for (const auto& kv : ships_) v.push_back(&kv.second);
        std::sort(v.begin(), v.end(), [](const Vessel* a, const Vessel* c) { return a->last > c->last; });
        bool first = true;
        for (const Vessel* s : v) { if (!first) o += ','; first = false; vesselJson(*s, t, o); }
        o += "],\"safety\":[";
        for (size_t i = 0; i < safety_.size(); ++i) {
            if (i) o += ',';
            o += "{\"age\":" + fmtNum(t - safety_[i].t, 0) + ",\"mmsi\":" + std::to_string(safety_[i].mmsi)
               + ",\"name\":\"" + jsonEsc(safety_[i].name) + "\",\"text\":\"" + jsonEsc(safety_[i].text) + "\""
               + (safety_[i].to ? ",\"to\":" + std::to_string(safety_[i].to) : std::string()) + "}";
        }
        // ★ V12 2026-11-16 area notices and text descriptions (binary 6/8, DAC 1 FI 22/23/29/30), newest last
        o += "],\"notices\":[";
        for (size_t i = 0; i < notices_.size(); ++i) {
            const NoticeRec& n = notices_[i];
            if (i) o += ',';
            o += "{\"age\":" + fmtNum(t - n.t, 0) + ",\"mmsi\":" + std::to_string(n.mmsi) + ",\"name\":\"" + jsonEsc(n.name)
               + "\",\"text\":\"" + jsonEsc(n.line) + "\",\"fi\":" + std::to_string(n.n.fi);
            if (n.n.what >= 0) o += ",\"what\":" + std::to_string(n.n.what);
            if (n.n.lat == n.n.lat) o += ",\"lat\":" + fmtNum(n.n.lat, 5) + ",\"lon\":" + fmtNum(n.n.lon, 5);
            if (n.n.to) o += ",\"to\":" + std::to_string(n.n.to);
            o += "}";
        }
        char bb[64]; std::snprintf(bb, sizeof bb, "],\"binary\":%llu}", (unsigned long long)binary_);
        return o + bb;
    }
    /** {"type":"ais_trail","mmsi":N,"pts":[[lat,lon,ageSec],…]} — oldest first. Empty pts for an unknown ship. */
    std::string trailJson(uint32_t mmsi) {
        std::lock_guard<std::mutex> lk(mtx_);
        const double t = now();
        std::string o = "{\"type\":\"ais_trail\",\"mmsi\":" + std::to_string(mmsi) + ",\"pts\":[";
        auto it = ships_.find(mmsi);
        if (it != ships_.end()) {
            bool first = true;
            for (const auto& p : it->second.trail) {
                if (!first) o += ','; first = false;
                o += "[" + fmtNum(p.lat, 5) + "," + fmtNum(p.lon, 5) + "," + fmtNum(t - p.t, 0) + "]";
            }
        }
        return o + "]}";
    }

private:
    double now() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count(); }

    void loop() {
        std::unique_lock<std::mutex> lk(mtx_);
        double winStart = 0;
        while (run_) {
            cv_.wait_for(lk, std::chrono::milliseconds(250), [this] { return !run_ || !queue_.empty(); });
            while (run_ && !queue_.empty()) {
                std::vector<cf> blk = std::move(queue_.front());
                queue_.pop_front();
                queued_ -= blk.size();
                Front* fe = fe_.get();
                lk.unlock();
                std::vector<std::pair<int, RawFrame>> got;
                // ★ near full scale, on every 4th sample: the gain rule's input (ISM's vibeism::GainRule)
                size_t clip = 0, seen = 0;
                for (size_t i = 0; i < blk.size(); i += 4, ++seen) if (std::fabs(blk[i].real()) > 0.93f || std::fabs(blk[i].imag()) > 0.93f) ++clip;
                fe->feed(blk.data(), blk.size(), [&](int c, const RawFrame& f) { got.emplace_back(c, f); });
                const double afc = fe->afcHz();
                lk.lock();
                afcHz_ = afc;
                clipN_ += clip; clipSeen_ += seen;
                const double t = now();
                std::vector<vibestation::Heard> heard;
                for (auto& g : got) accept_(g.first, g.second, t, sink_ ? &heard : nullptr);
                if (!heard.empty() && sink_) { auto s = sink_; lk.unlock(); for (const auto& h : heard) s->heard(h); lk.lock(); }
            }
            const double t = now();
            if (t - winStart >= 1.0) {
                winStart = t;
                if (sink_) { auto s = sink_; lk.unlock(); s->tick(); lk.lock(); }
                if (fe_) {
                    health_.noiseDb = std::min(fe_->noiseDb(0), fe_->noiseDb(1)); health_.msgs = msgs_; health_.valid = true;
                    health_.clipFrac = clipSeen_ ? double(clipN_) / double(clipSeen_) : 0; clipN_ = clipSeen_ = 0;
                }
                prune_(t);
                if (storeFn_ && t - storeLast_ >= kStoreEverySec) { lk.unlock(); flushStore_(); lk.lock(); }
            }
        }
    }
    /** The changed ships out to the store — copied under the lock, written without it. */
    void flushStore_() {
        StoreFn fn; std::vector<Vessel> out; double t;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!storeFn_) return;
            fn = storeFn_; t = now(); storeLast_ = t;
            for (auto& kv : ships_) {
                Vessel& v = kv.second;
                if (v.last <= v.storedAt) continue;
                Vessel c;   // ★ the state without the hour-long trail: only the points since the last hand-over
                c.mmsi = v.mmsi; c.kind = v.kind; c.name = v.name; c.callsign = v.callsign; c.dest = v.dest; c.imo = v.imo;
                c.shipType = v.shipType; c.nav = v.nav; c.heading = v.heading; c.rot = v.rot; c.atonType = v.atonType; c.altM = v.altM;
                c.lat = v.lat; c.lon = v.lon; c.sog = v.sog; c.cog = v.cog; c.draught = v.draught;
                c.bow = v.bow; c.stern = v.stern; c.port = v.port; c.stbd = v.stbd;
                c.etaMonth = v.etaMonth; c.etaDay = v.etaDay; c.etaHour = v.etaHour; c.etaMin = v.etaMin;
                c.virt = v.virt; c.offPos = v.offPos; c.atonStatus = v.atonStatus; c.first = v.first;
                c.dx = v.dx; c.coarse = v.coarse; c.fakes = v.fakes;   // ★ beta95 (never the held report: it is not believed)
                c.nMet = v.nMet; for (int k = 0; k < v.nMet; ++k) { c.met[k] = v.met[k]; c.metAt[k] = v.metAt[k]; } c.last = v.last; c.posAt = v.posAt; c.count = v.count; c.ch = v.ch;
                for (const auto& p : v.trail) if (p.t > v.storedTrailT) c.trail.push_back(p);
                if (!v.trail.empty()) v.storedTrailT = v.trail.back().t;
                v.storedAt = v.last;
                out.push_back(std::move(c));
            }
        }
        if (!out.empty()) fn(std::move(out), t);
    }

    void accept_(int ch, const RawFrame& f, double t, std::vector<vibestation::Heard>* heard = nullptr) {
        Msg m;
        if (!decode(f.bytes, m)) return;
        /* ★★★ A REPAIRED FRAME MUST NAME A STATION ALREADY HEARD CLEANLY. Sixteen bits of FCS, eight tries and five
         *  phases would let roughly one garbage frame in 3000 failed ones through as "repaired" (measured: one at 14 dB in
         *  test-ais before this rule). A garbage frame's MMSI is 30 random bits; the chance it matches one of a few
         *  hundred ships heard is one in millions. Ships repeat every few seconds, so the first clean frame opens the
         *  door and the repairs then fill the gaps — the gain is kept, the false frames are not. */
        if (f.repaired && !ships_.count(m.mmsi)) { ++repairUnknown_; return; }
        ++msgs_; ++perCh_[ch & 1]; rateWin_.push_back(t);
        if (rateWin_.size() > 20000) rateWin_.pop_front();
        if (f.repaired) ++repaired_;
        // ★ V12 2026-11-16 binary messages: the ones we can read (vibe_ais_asm.h) go on the sender's row
        asmsg::Kind bk; asmsg::Met bmet; asmsg::Notice bnote; int bin = 0;
        if (m.type == 6 || m.type == 8) { ++binary_; bin = asmsg::parse(f.bytes, bk, bmet, bnote); }
        const bool shown = (m.type >= 1 && m.type <= 5) || m.type == 9 || m.type == 11 || m.type == 12 || m.type == 14
                        || m.type == 18 || m.type == 19 || m.type == 21 || m.type == 24 || m.type == 27 || bin > 0;
        if (!shown || m.mmsi == 0) { ++other_; return; }
        Vessel& v = ships_[m.mmsi];
        if (v.count == 0) { v.mmsi = m.mmsi; v.first = t; }
        ++v.count; v.last = t; v.ch = ch; v.level = f.levelDb; v.snr = f.snrDb;
        switch (m.type) {
            case 1: case 2: case 3: case 5: v.kind = "A"; break;
            case 18: case 19: case 24: v.kind = "B"; break;
            case 4: case 11: v.kind = "base"; break;
            case 21: v.kind = "aton"; break;
            case 9: v.kind = "sar"; break;
            default: break;
        }
        if (isBeacon(m.mmsi)) v.kind = "sart";
        if (bin) {
            // ★ a station first heard by its binary message: what its MMSI says it is (M.585: 99… AtoN, 00… coast)
            if (v.count == 1) { if (m.mmsi / 10000000 == 99) v.kind = "aton"; else if (m.mmsi < 10000000) v.kind = "base"; }
            if (bin == 1) { addMet_(v, bmet, t, ch, heard); return; }
            addNotice_(v, bnote, t, ch, heard);
            return;
        }
        if (m.type == 12 || m.type == 14) {
            Safety s{t, m.mmsi, displayName(v), m.text, m.type == 12 ? m.destMmsi : 0};
            safety_.push_back(s);
            if (safety_.size() > 20) safety_.erase(safety_.begin());
            if (heard) heard->push_back({ std::to_string(m.mmsi) + "#txt", "safety", displayName(v),
                                          displayName(v) + " (MMSI " + std::to_string(m.mmsi) + ") \xc2\xb7 safety message: " + m.text, kChanHz[ch & 1] });
            return;
        }
        if (!m.name.empty()) v.name = m.name;
        if (!m.callsign.empty()) v.callsign = m.callsign;
        if (!m.dest.empty()) v.dest = m.dest;
        if (m.imo) v.imo = m.imo;
        if (m.shipType > 0) v.shipType = m.shipType;
        if (m.bow >= 0 && (m.bow + m.stern) > 0) { v.bow = m.bow; v.stern = m.stern; v.port = m.port; v.stbd = m.stbd; }
        if (m.draught == m.draught) v.draught = m.draught;
        if (m.type == 5) { v.etaMonth = m.etaMonth; v.etaDay = m.etaDay; v.etaHour = m.etaHour; v.etaMin = m.etaMin; }
        if (m.type == 21) { v.atonType = m.atonType; v.virt = m.virt; v.offPos = m.offPos; v.atonStatus = m.atonStatus; }
        if (m.type == 9) v.altM = m.altM;
        if (m.nav >= 0) v.nav = m.nav;
        if (m.type == 1 || m.type == 2 || m.type == 3) v.rot = m.rot;
        bool posOk = m.hasPos;
        // ★★ beta95 the whole-minute fake (see kDxNm): dropped and counted, never believed
        if (posOk && m.wholeMin && movingType(m.type)) { ++v.fakes; ++fakePos_; posOk = false; }
        if (posOk) posOk = believe_(v, m, t);
        if (posOk) {
            v.lat = m.lat; v.lon = m.lon; v.posAt = t; v.coarse = m.type == 27;
            v.dx = rxLat_ == rxLat_ && gcNm(rxLat_, rxLon_, m.lat, m.lon) > kDxNm;
            v.sog = m.sog; v.cog = m.cog; v.heading = m.heading;
            // ★ the trail: a point when it has moved 50 m or a minute has passed, at most kTrailMax
            bool add = v.trail.empty();
            if (!add) {
                const auto& p = v.trail.back();
                const double dy = (m.lat - p.lat) * 111320.0, dx = (m.lon - p.lon) * 111320.0 * std::cos(m.lat * M_PI / 180);
                add = dx * dx + dy * dy > 2500 || t - p.t >= 60;
            }
            if (add) { v.trail.push_back({ t, m.lat, m.lon }); while (v.trail.size() > kTrailMax) v.trail.pop_front(); }
            if (v.sog == v.sog) {
                if (v.sogRefAt < 0) { v.sogRef = v.sog; v.sogRefAt = t; }
                else if (t - v.sogRefAt >= 60) {
                    const double d = v.sog - v.sogRef;
                    v.trend = d > 0.5 ? 1 : d < -0.5 ? -1 : 0;
                    v.sogRef = v.sog; v.sogRefAt = t;
                }
            }
        }
        // ★ the gallery: a line only when what it says changed, and a moving ship at most once a minute
        if (heard) {
            const std::string line = readable(v);
            // ★ …but a new NAME (a type 5 / 24 after the first positions) is logged at once: the log should say who
            const bool named = displayName(v) != v.loggedName;
            if (line != v.logLine && (named || t - v.loggedAt >= (v.sog == v.sog && v.sog >= 0.5 ? 60 : 0))) {
                std::string full = line + " \xc2\xb7 MMSI " + std::to_string(v.mmsi);
                if (v.lat == v.lat) full += " \xc2\xb7 " + fmtNum(v.lat, 4) + " " + fmtNum(v.lon, 4);
                heard->push_back({ std::to_string(v.mmsi), v.kind, displayName(v), full, kChanHz[ch & 1] });
                v.logLine = line; v.loggedAt = t; v.loggedName = displayName(v);
            }
        }
    }
    /** ★★ beta95 believe this position? (the rules at kDxNm). False = held as the ship's pending report. */
    bool believe_(Vessel& v, const Msg& m, double t) {
        const bool moving = movingType(m.type);
        const double maxKn = m.type == 9 ? kSarMaxKn : moving ? kShipMaxKn : 0;   // ★ a fixed station agrees only in place
        const bool pend = v.pAt >= 0 && agrees(v.pLat, v.pLon, v.pAt, m.lat, m.lon, t, maxKn);
        const bool far = rxLat_ == rxLat_ && gcNm(rxLat_, rxLon_, m.lat, m.lon) > kDxNm;
        bool ok;
        if (moving && v.lat == v.lat && v.posAt >= 0) {
            if (agrees(v.lat, v.lon, v.posAt, m.lat, m.lon, t, maxKn)) ok = true;
            else if (pend) { ok = true; v.trail.clear(); ++relocated_; }   // ★ two reports agree on the new place: no line across
            else ok = false;
        } else if (!moving && v.lat == v.lat && agrees(v.lat, v.lon, v.posAt, m.lat, m.lon, t, 0)) {
            ok = true;   // ★ an AtoN / base station where it was
        } else {
            // ★ a first position (or a fixed station's own position, which beats one taken from its weather readings):
            //   near = believed at once, far = a DX candidate until a second report agrees
            ok = !far || pend;
        }
        if (ok) { v.pAt = -1; v.pLat = v.pLon = NAN; }
        else { v.pLat = m.lat; v.pLon = m.lon; v.pAt = t; ++v.held; ++held_; }
        return ok;
    }
    /** ★ A met report onto its sender: the same SITE (within 500 m, or both without a position) is replaced, a new one
     *  added, the oldest of kMetSites given up. A station with no position of its own is drawn at the readings'. */
    void addMet_(Vessel& v, const asmsg::Met& m, double t, int ch, std::vector<vibestation::Heard>* heard) {
        int slot = -1;
        for (int k = 0; k < v.nMet; ++k) {
            const asmsg::Met& o = v.met[k];
            const bool bothNone = o.lat == asmsg::kNoPos32 && m.lat == asmsg::kNoPos32;
            bool near = false;
            if (o.lat != asmsg::kNoPos32 && m.lat != asmsg::kNoPos32) {
                const double dy = (o.lat - m.lat) * 1e-7 * 111320.0, dx = (o.lon - m.lon) * 1e-7 * 111320.0 * std::cos(m.lat * 1e-7 * M_PI / 180);
                near = dx * dx + dy * dy < 500.0 * 500.0;
            }
            if (bothNone || near) { slot = k; break; }
        }
        if (slot < 0 && v.nMet < Vessel::kMetSites) slot = v.nMet++;
        if (slot < 0) { slot = 0; for (int k = 1; k < v.nMet; ++k) if (v.metAt[k] < v.metAt[slot]) slot = k; }
        v.met[slot] = m; v.metAt[slot] = t;
        if (v.lat != v.lat && m.lat != asmsg::kNoPos32) { v.lat = m.lat / 1e7; v.lon = m.lon / 1e7; v.posAt = t; }
        // ★ the gallery: a station's weather at most every 30 min (it repeats every few minutes)
        if (heard && t - v.metLoggedAt >= 1800) {
            const std::string line = asmsg::metLine(m);
            if (!line.empty()) {
                heard->push_back({ std::to_string(v.mmsi) + "#met", "met", displayName(v),
                                   displayName(v) + " (MMSI " + std::to_string(v.mmsi) + ") \xc2\xb7 weather: " + line, kChanHz[ch & 1] });
                v.metLoggedAt = t;
            }
        }
    }
    /** ★ An area notice or text description: the strip keeps the newest kNotices; the same words from the same station
     *  (they repeat every few minutes) only refresh the time. */
    static constexpr size_t kNotices = 20;
    void addNotice_(Vessel& v, const asmsg::Notice& n, double t, int ch, std::vector<vibestation::Heard>* heard) {
        const std::string line = asmsg::noticeLine(n);
        if (line.empty()) return;
        for (auto it = notices_.begin(); it != notices_.end(); ++it) {
            if (it->mmsi == v.mmsi && it->line == line) { NoticeRec r = *it; r.t = t; notices_.erase(it); notices_.push_back(r); return; }
        }
        notices_.push_back({ t, v.mmsi, displayName(v), line, n });
        while (notices_.size() > kNotices) notices_.erase(notices_.begin());
        if (heard) heard->push_back({ std::to_string(v.mmsi) + "#note", "notice", displayName(v),
                                      displayName(v) + " (MMSI " + std::to_string(v.mmsi) + ") \xc2\xb7 notice: " + line, kChanHz[ch & 1] });
    }
    void prune_(double t) {
        for (auto it = ships_.begin(); it != ships_.end();) { if (t - it->second.last > kKeepSec) it = ships_.erase(it); else ++it; }
        while (ships_.size() > kMaxVessels) {
            auto old = ships_.begin();
            for (auto it = ships_.begin(); it != ships_.end(); ++it) if (it->second.last < old->second.last) old = it;
            ships_.erase(old);
        }
    }
public:
    /** One vessel in the report's shape (also the ship store's — vibe_ais_store.h toJson). */
    static void vesselJson(const Vessel& v, double t, std::string& o) {
        o += "{\"mmsi\":" + std::to_string(v.mmsi) + ",\"kind\":\"" + v.kind + "\"";
        auto str = [&](const char* k, const std::string& s) { if (!s.empty()) { o += ",\""; o += k; o += "\":\"" + jsonEsc(s) + "\""; } };
        auto num = [&](const char* k, double x, int dp) { if (x == x) { o += ",\""; o += k; o += "\":" + fmtNum(x, dp); } };
        auto inum = [&](const char* k, int x) { if (x >= 0) { o += ",\""; o += k; o += "\":" + std::to_string(x); } };
        str("name", v.name); str("call", v.callsign); str("dest", v.dest);
        if (v.imo) o += ",\"imo\":" + std::to_string(v.imo);
        if (const Mid* f = flagOf(v.mmsi)) { o += ",\"flag\":\""; o += f->iso; o += "\",\"country\":\"" + jsonEsc(f->name) + "\""; }
        inum("stype", v.shipType);
        if (v.shipType >= 0) { str("stypeName", shipTypeName(v.shipType)); o += ",\"family\":\""; o += shipFamily(v.shipType); o += "\""; }
        if (v.nav >= 0) { inum("nav", v.nav); str("navName", navStatusName(v.nav)); }
        if (v.kind == "aton") { inum("aton", v.atonType); str("atonName", atonTypeName(v.atonType)); if (v.virt) o += ",\"virtual\":1"; if (v.offPos) o += ",\"offpos\":1"; }
        {   // ★ V12 2026-11-16 the AtoN status byte (IALA A-126 page 7): light, RACON, alarm
            const asmsg::AtonStatus st = asmsg::atonStatus(v.atonStatus);
            if (st.valid) { inum("light", st.light); inum("racon", st.racon); if (st.alarm) o += ",\"alarm\":1"; }
        }
        if (v.nMet > 0) {   // ★ V12 2026-11-16 met/hydro readings, newest first
            int idx[Vessel::kMetSites]; for (int k = 0; k < v.nMet; ++k) idx[k] = k;
            std::sort(idx, idx + v.nMet, [&](int a, int b) { return v.metAt[a] > v.metAt[b]; });
            o += ",\"met\":[";
            for (int k = 0; k < v.nMet; ++k) { if (k) o += ','; asmsg::metJson(v.met[idx[k]], t - v.metAt[idx[k]], o); }
            o += "]";
        }
        num("lat", v.lat, 5); num("lon", v.lon, 5); num("sog", v.sog, 1); num("cog", v.cog, 1);
        inum("hdg", v.heading);
        { const double r = rotDegMin(v.rot); if (r == r) num("rot", r, 1); else if (v.rot == 127 || v.rot == -127) o += std::string(",\"turn\":") + (v.rot > 0 ? "1" : "-1"); }
        num("draught", v.draught, 1);
        if (v.bow >= 0 && v.bow + v.stern > 0) { o += ",\"len\":" + std::to_string(v.bow + v.stern) + ",\"beam\":" + std::to_string(v.port + v.stbd); }
        if (v.etaMonth > 0 && v.etaDay > 0) { char b[24]; std::snprintf(b, sizeof b, "%02d-%02d %02d:%02d", v.etaMonth, v.etaDay, v.etaHour < 24 ? v.etaHour : 0, v.etaMin < 60 ? v.etaMin : 0); str("eta", b); }
        if (v.altM >= 0) inum("alt", v.altM);
        if (v.trend) o += ",\"trend\":" + std::to_string(v.trend);
        // ★★ beta95 (kDxNm): dx = believed and far (two reports agreed); unconf = a far first report held, uLat/uLon where
        //    it claims (listed, never a dot); held = a jump from the track held; fakes = whole-minute reports dropped
        if (v.dx) o += ",\"dx\":1";
        if (v.pAt >= 0) {
            if (v.lat != v.lat) o += ",\"unconf\":1,\"uLat\":" + fmtNum(v.pLat, 5) + ",\"uLon\":" + fmtNum(v.pLon, 5);
            else o += ",\"held\":1";
        }
        if (v.fakes > 0) o += ",\"fakes\":" + std::to_string(v.fakes);
        o += ",\"line\":\"" + jsonEsc(readable(v)) + "\"";
        char b[160];
        std::snprintf(b, sizeof b, ",\"ch\":\"%s\",\"level\":%.1f,\"snr\":%.1f,\"seen\":%.0f,\"age\":%.0f,\"posAge\":%.0f,\"count\":%d,\"trail\":%zu}",
                      v.ch ? "B" : "A", v.level, v.snr, t - v.last, t - v.first, v.posAt >= 0 ? t - v.posAt : -1.0, v.count, v.trail.size());
        o += b;
    }
private:

    struct Safety { double t; uint32_t mmsi; std::string name, text; uint32_t to; };
    struct NoticeRec { double t; uint32_t mmsi; std::string name, line; asmsg::Notice n; };
    std::vector<NoticeRec> notices_;
    uint64_t binary_ = 0;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> run_{false};
    double hwCentre_ = 0, rate_ = 0;
    double afcHz_ = 0;   ///< ★ the radio's crystal error at 162 MHz as the AFC found it (Hz; kept across restarts)
    std::unique_ptr<Front> fe_;
    std::deque<std::vector<cf>> queue_;
    size_t queued_ = 0;
    uint64_t clipN_ = 0, clipSeen_ = 0, repairUnknown_ = 0;
    uint64_t fakePos_ = 0, held_ = 0, relocated_ = 0;   ///< ★ beta95 whole-minute fakes dropped · reports held · ships relocated
    double rxLat_ = NAN, rxLon_ = NAN;                 ///< ★ beta95 the receiver (setReceiver) — the DX rule
    uint64_t dropped_ = 0, msgs_ = 0, perCh_[2] = {0, 0}, repaired_ = 0, other_ = 0;
    std::deque<double> rateWin_;
    vibeadsb::CaptureMeter cap_;
    std::chrono::steady_clock::time_point t0_ = std::chrono::steady_clock::now();
    std::map<uint32_t, Vessel> ships_;
    std::vector<Safety> safety_;
    Health health_;
    std::shared_ptr<vibestation::Sink> sink_;
    StoreFn storeFn_;
    double storeLast_ = 0;
};

/** ★ AIS's gain: ISM's rule — start 60 % up the radio's own list, step down when the capture clips, back up after two
 *  quiet minutes. The shim applies it (vibeism::GainRule); nothing AIS-specific beyond the channels. */

} // namespace vibeais
