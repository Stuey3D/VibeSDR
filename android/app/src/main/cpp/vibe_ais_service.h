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
#include "vibe_station_sink.h"
#include "vibe_adsb_capture.h"     // vibeadsb::CaptureMeter — the same live "capture N %" as ADS-B and ISM

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
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
    double first = 0, last = 0, posAt = -1;
    int count = 0, ch = 0;
    float level = -120, snr = 0;
    // ★ the speed trend (the card's ▲/▼): the speed a minute ago
    double sogRef = NAN, sogRefAt = -1; int trend = 0;
    struct Pt { double t, lat, lon; };
    std::deque<Pt> trail;
    std::string logLine; double loggedAt = -1e9;
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
    return s;
}

struct Health { double noiseDb = -120; double clipFrac = 0; uint64_t msgs = 0; bool valid = false; };

class Service {
public:
    static constexpr size_t kMaxVessels = 1000;
    static constexpr double kKeepSec = 3600;
    static constexpr size_t kTrailMax = 360;

    ~Service() { stop(); }

    /** Start with the radio tuned to `hwCentreHz` at `rateHz`. `keepList`: a restart on the same channels (the rate
     *  changed underneath) keeps the ships; entering the mode clears. `sink`: the gallery's day log (or null). */
    void start(double hwCentreHz, double rateHz, bool keepList = false, std::shared_ptr<vibestation::Sink> sink = nullptr) {
        stop();
        std::lock_guard<std::mutex> lk(mtx_);
        sink_ = std::move(sink);
        hwCentre_ = hwCentreHz; rate_ = rateHz;
        fe_.reset(new Front(rateHz, hwCentreHz));
        queue_.clear(); queued_ = 0; dropped_ = 0;
        if (!keepList) { t0_ = std::chrono::steady_clock::now(); ships_.clear(); msgs_ = 0; perCh_[0] = perCh_[1] = 0; repaired_ = 0; other_ = 0; safety_.clear(); rateWin_.clear(); }
        cap_.reset(rateHz, now());
        run_ = true;
        worker_ = std::thread([this] { loop(); });
    }
    void stop() {
        { std::lock_guard<std::mutex> lk(mtx_); if (!run_) return; run_ = false; }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
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

    /** {"type":"ais", "centre", "rate", "now", "msgs", "msgRate", "chA", "chB", "repaired", "dropped", "capture", "noise",
     *  "vessels":[…], "safety":[…]} */
    std::string report() {
        std::lock_guard<std::mutex> lk(mtx_);
        const double t = now();
        prune_(t);
        while (!rateWin_.empty() && t - rateWin_.front() > 60) rateWin_.pop_front();
        const double span = std::min(60.0, std::max(1.0, t));
        char b[400];
        std::snprintf(b, sizeof b, "{\"type\":\"ais\",\"centre\":%.0f,\"rate\":%.0f,\"now\":%.1f,\"msgs\":%llu,\"msgRate\":%.2f,"
                      "\"chA\":%llu,\"chB\":%llu,\"repaired\":%llu,\"other\":%llu,\"dropped\":%llu,\"capture\":%d,\"noise\":%.1f,\"vessels\":[",
                      kCentreHz, rate_, t, (unsigned long long)msgs_, rateWin_.size() / span * 60.0,
                      (unsigned long long)perCh_[0], (unsigned long long)perCh_[1], (unsigned long long)repaired_,
                      (unsigned long long)other_, (unsigned long long)dropped_, cap_.percent(t), health_.noiseDb);
        std::string o = b;
        std::vector<const Vessel*> v;
        for (const auto& kv : ships_) v.push_back(&kv.second);
        std::sort(v.begin(), v.end(), [](const Vessel* a, const Vessel* c) { return a->last > c->last; });
        bool first = true;
        for (const Vessel* s : v) { if (!first) o += ','; first = false; vesselJson_(*s, t, o); }
        o += "],\"safety\":[";
        for (size_t i = 0; i < safety_.size(); ++i) {
            if (i) o += ',';
            o += "{\"age\":" + fmtNum(t - safety_[i].t, 0) + ",\"mmsi\":" + std::to_string(safety_[i].mmsi)
               + ",\"name\":\"" + jsonEsc(safety_[i].name) + "\",\"text\":\"" + jsonEsc(safety_[i].text) + "\""
               + (safety_[i].to ? ",\"to\":" + std::to_string(safety_[i].to) : std::string()) + "}";
        }
        return o + "]}";
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
                lk.lock();
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
            }
        }
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
        const bool shown = (m.type >= 1 && m.type <= 5) || m.type == 9 || m.type == 11 || m.type == 12 || m.type == 14
                        || m.type == 18 || m.type == 19 || m.type == 21 || m.type == 24 || m.type == 27;
        if (!shown || m.mmsi == 0) { ++other_; return; }
        Vessel& v = ships_[m.mmsi];
        if (v.count == 0) { v.mmsi = m.mmsi; v.first = t; }
        ++v.count; v.last = t; v.ch = ch; v.level = f.levelDb; v.snr = f.snrDb;
        const char* what = nullptr; midOf(m.mmsi, &what);
        switch (m.type) {
            case 1: case 2: case 3: case 5: v.kind = "A"; break;
            case 18: case 19: case 24: v.kind = "B"; break;
            case 4: case 11: v.kind = "base"; break;
            case 21: v.kind = "aton"; break;
            case 9: v.kind = "sar"; break;
            default: break;
        }
        if (what && (std::strcmp(what, "AIS-SART") == 0 || std::strcmp(what, "man overboard") == 0 || std::strcmp(what, "EPIRB-AIS") == 0)) v.kind = "sart";
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
        if (m.type == 21) { v.atonType = m.atonType; v.virt = m.virt; v.offPos = m.offPos; }
        if (m.type == 9) v.altM = m.altM;
        if (m.nav >= 0) v.nav = m.nav;
        if (m.type == 1 || m.type == 2 || m.type == 3) v.rot = m.rot;
        if (m.hasPos) {
            v.lat = m.lat; v.lon = m.lon; v.posAt = t;
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
            if (line != v.logLine && t - v.loggedAt >= (v.sog == v.sog && v.sog >= 0.5 ? 60 : 0)) {
                std::string full = line + " \xc2\xb7 MMSI " + std::to_string(v.mmsi);
                if (v.lat == v.lat) full += " \xc2\xb7 " + fmtNum(v.lat, 4) + " " + fmtNum(v.lon, 4);
                heard->push_back({ std::to_string(v.mmsi), v.kind, displayName(v), full, kChanHz[ch & 1] });
                v.logLine = line; v.loggedAt = t;
            }
        }
    }
    void prune_(double t) {
        for (auto it = ships_.begin(); it != ships_.end();) { if (t - it->second.last > kKeepSec) it = ships_.erase(it); else ++it; }
        while (ships_.size() > kMaxVessels) {
            auto old = ships_.begin();
            for (auto it = ships_.begin(); it != ships_.end(); ++it) if (it->second.last < old->second.last) old = it;
            ships_.erase(old);
        }
    }
    static void vesselJson_(const Vessel& v, double t, std::string& o) {
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
        num("lat", v.lat, 5); num("lon", v.lon, 5); num("sog", v.sog, 1); num("cog", v.cog, 1);
        inum("hdg", v.heading);
        { const double r = rotDegMin(v.rot); if (r == r) num("rot", r, 1); else if (v.rot == 127 || v.rot == -127) o += std::string(",\"turn\":") + (v.rot > 0 ? "1" : "-1"); }
        num("draught", v.draught, 1);
        if (v.bow >= 0 && v.bow + v.stern > 0) { o += ",\"len\":" + std::to_string(v.bow + v.stern) + ",\"beam\":" + std::to_string(v.port + v.stbd); }
        if (v.etaMonth > 0 && v.etaDay > 0) { char b[24]; std::snprintf(b, sizeof b, "%02d-%02d %02d:%02d", v.etaMonth, v.etaDay, v.etaHour < 24 ? v.etaHour : 0, v.etaMin < 60 ? v.etaMin : 0); str("eta", b); }
        if (v.altM >= 0) inum("alt", v.altM);
        if (v.trend) o += ",\"trend\":" + std::to_string(v.trend);
        o += ",\"line\":\"" + jsonEsc(readable(v)) + "\"";
        char b[160];
        std::snprintf(b, sizeof b, ",\"ch\":\"%s\",\"level\":%.1f,\"snr\":%.1f,\"seen\":%.0f,\"age\":%.0f,\"posAge\":%.0f,\"count\":%d,\"trail\":%zu}",
                      v.ch ? "B" : "A", v.level, v.snr, t - v.last, t - v.first, v.posAt >= 0 ? t - v.posAt : -1.0, v.count, v.trail.size());
        o += b;
    }

    struct Safety { double t; uint32_t mmsi; std::string name, text; uint32_t to; };
    std::mutex mtx_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> run_{false};
    double hwCentre_ = 0, rate_ = 0;
    std::unique_ptr<Front> fe_;
    std::deque<std::vector<cf>> queue_;
    size_t queued_ = 0;
    uint64_t clipN_ = 0, clipSeen_ = 0, repairUnknown_ = 0;
    uint64_t dropped_ = 0, msgs_ = 0, perCh_[2] = {0, 0}, repaired_ = 0, other_ = 0;
    std::deque<double> rateWin_;
    vibeadsb::CaptureMeter cap_;
    std::chrono::steady_clock::time_point t0_ = std::chrono::steady_clock::now();
    std::map<uint32_t, Vessel> ships_;
    std::vector<Safety> safety_;
    Health health_;
    std::shared_ptr<vibestation::Sink> sink_;
};

/** ★ AIS's gain: ISM's rule — start 60 % up the radio's own list, step down when the capture clips, back up after two
 *  quiet minutes. The shim applies it (vibeism::GainRule); nothing AIS-specific beyond the channels. */

} // namespace vibeais
