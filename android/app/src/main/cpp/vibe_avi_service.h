// vibe_avi_service.h — V12 aviation datalink: the service around the decoders (vibe_acars.h, vibe_vdl2.h, vibe_hfdl.h).
// Its own thread fed by copy, the aircraft table every message lands in, and the JSON the clients draw.
// docs/v12/AVIATION.md §Service. Wired: the shim's "Aircraft datalink" mode (ACARS / VDL2, a whole-radio mode) and the
// listeners' HFDL decoder (vibe_hfdl_run.h) both land in sharedTracker(); /addon/hfdl/* is served from its HFDL rows.
//
// ★★★ OFF THE DSP THREAD, AND IT DROPS RATHER THAN WAITS (ADS-B's, ISM's and AIS's rule: Network > Audio > Spectrum >
//     Decoders). feed() copies and returns; a queue over half a second loses the OLDEST block and counts it.
// ★★★ ONE ROW PER AIRCRAFT, from every decoder: VDL2 names it by its ICAO 24-bit address (the AVLC source), ACARS by its
//     registration and flight, HFDL by its ICAO address (logons) or flight (performance / frequency data). A row keeps
//     whatever has been heard — registration, flight, ICAO, the last position, the labels — so the ADS-B view can join
//     it to the same aircraft by ICAO address (or by registration through the aircraft database).
// ★★★ MESSAGE BODIES ARE THE OWNER'S CHOICE (docs/v12/DECODER-RESEARCH.md §4.3). Flight IDs, registrations, positions
//     and labels are shown by default; ACARS free text only when the owner has switched `bodies` on (off by default,
//     with the jurisdiction notice on the setup page), and never in a public gallery.
#pragma once
#include "vibe_acars.h"
#include "vibe_vdl2.h"
#include "vibe_hfdl.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vibeavi {

inline std::string jsonEsc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 0x20 || c == 0x7F) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    return o;
}

/** A short, plain-words name for the common ACARS labels (ARINC 620 is paid; these are the widely published meanings). */
inline const char* labelName(const std::string& l) {
    static const struct { const char* l; const char* n; } t[] = {
        { "_\x7f", "link test / ack" }, { "Q0", "link test" }, { "SQ", "ground station squitter" }, { "H1", "message to/from crew or ops" },
        { "H2", "weather report" }, { "16", "position report" }, { "15", "position report" }, { "10", "departure" },
        { "11", "arrival" }, { "5U", "weather request" }, { "5Z", "airline ops" }, { "B6", "ADS-C" }, { "BA", "CPDLC / ATS" },
        { "SA", "media advisory" }, { "QA", "out of gate" }, { "QB", "off the ground" }, { "QC", "on the ground" }, { "QD", "in at the gate" },
        { "80", "airline ops" }, { "1L", "airline ops" }, { "Q1", "OOOI" }, { "RA", "uplink to the printer" },
    };
    for (const auto& e : t) if (l == e.l) return e.n;
    return "";
}

struct Aircraft {
    uint32_t icao = 0;               ///< 0 = not known (ACARS alone)
    std::string reg, flight;
    double lat = NAN, lon = NAN, posAt = -1;
    double first = 0, last = 0;
    int count = 0;
    std::string via;                 ///< "VDL2 136.975", "ACARS 131.725", "HFDL 8977 (Reykjavik)"
    float level = -120;
    std::string lastLabel, lastText; ///< lastText: only kept when bodies are on
    // ★ HFDL: the last burst's ground station, frequency and level, and when (the map's /addon/hfdl/aircraft fields)
    int gsId = -1; double hfdlKhz = 0, hfdlAt = -1; float hfdlLevel = 0;
};

/** ★ The HFDL ground stations (the public HFDL station list: id, name, position to a tenth of a degree). */
struct GroundStation { int id; const char* name; double lat, lon; };
inline const std::vector<GroundStation>& groundStations() {
    static const std::vector<GroundStation> v = {
        { 1, "San Francisco", 37.4, -122.4 }, { 2, "Molokai", 21.2, -157.2 }, { 3, "Reykjavik", 64.1, -21.9 },
        { 4, "Riverhead", 40.9, -72.6 }, { 5, "Auckland", -37.0, 174.8 }, { 6, "Hat Yai", 6.9, 100.4 },
        { 7, "Shannon", 52.7, -8.9 }, { 8, "Johannesburg", -26.1, 28.1 }, { 9, "Barrow", 71.3, -156.8 },
        { 10, "Muan", 35.0, 126.4 }, { 11, "Albrook", 9.0, -79.6 }, { 13, "Santa Cruz", -17.7, -63.1 },
        { 14, "Krasnoyarsk", 56.2, 92.5 }, { 15, "Al Muharraq", 26.3, 50.6 }, { 16, "Agana", 13.5, 144.8 },
        { 17, "Canarias", 27.9, -15.4 },
    };
    return v;
}

/** The table: one row per aircraft, merged from every decoder. Thread-safe. */
class Tracker {
public:
    explicit Tracker(bool bodies = false) : bodies_(bodies) {}
    void setBodies(bool on) { std::lock_guard<std::mutex> lk(m_); bodies_ = on; if (!on) for (auto& a : rows_) a.lastText.clear(); }

    void addVdl2(const vibevdl2::Frame& f, double now) {
        std::lock_guard<std::mutex> lk(m_);
        const bool fromAc = f.src.type == 1;
        const uint32_t ic = fromAc ? f.src.addr : (f.dst.type == 1 ? f.dst.addr : 0);
        if (!ic) { ++other_; return; }
        Aircraft& a = row_(ic, f.hasAcars ? f.acars.reg : "", now);
        char via[48]; std::snprintf(via, sizeof via, "VDL2 %.3f", f.hz / 1e6);
        a.via = via;
        if (fromAc) a.level = f.levelDb;
        if (f.hasAcars && f.acarsCrcOk) acars_(a, f.acars);
        ++msgs_;
    }
    void addAcars(const vibeacars::Block& b, double now) {
        std::lock_guard<std::mutex> lk(m_);
        if (b.reg.empty()) { ++other_; return; }
        Aircraft& a = row_(0, b.reg, now);
        char via[48]; std::snprintf(via, sizeof via, "ACARS %.3f", b.hz / 1e6);
        a.via = via; a.level = b.levelDb;
        acars_(a, b);
        ++msgs_;
    }
    void addHfdl(const vibehfdl::Message& m, double now) {
        std::lock_guard<std::mutex> lk(m_);
        heardGs_(m.downlink ? m.dstId : m.srcId, m.hz / 1e3, m.snrDb, now);
        if (!m.downlink || (!m.icao && m.flight.empty() && !m.hasAcars)) { ++other_; return; }
        Aircraft& a = row_(m.icao, m.hasAcars ? m.acars.reg : "", now, m.icao ? "" : m.flight);
        char via[64]; std::snprintf(via, sizeof via, "HFDL %.0f%s%s", m.hz / 1e3, *vibehfdl::gsName(m.dstId) ? " " : "", vibehfdl::gsName(m.dstId));
        a.via = via;
        a.gsId = m.dstId; a.hfdlKhz = m.hz / 1e3; a.hfdlAt = now; a.hfdlLevel = m.snrDb;
        if (!m.flight.empty()) a.flight = m.flight;
        if (!std::isnan(m.lat)) { a.lat = m.lat; a.lon = m.lon; a.posAt = now; }
        if (m.hasAcars && m.acarsCrcOk) acars_(a, m.acars);
        ++msgs_;
    }
    /** Rows heard in the last `keepS` seconds, newest first: {"aircraft":[{icao, reg, flight, lat, lon, via, n, age, label, text}]}. */
    /** `withText`: the asker may see message bodies (the owner's socket) — still only when the owner switched them on. */
    std::string json(double now, double keepS = 3600, bool withText = true) {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<const Aircraft*> v;
        for (const auto& a : rows_) if (now - a.last <= keepS) v.push_back(&a);
        std::sort(v.begin(), v.end(), [](const Aircraft* x, const Aircraft* y) { return x->last > y->last; });
        std::string o = "{\"msgs\":" + std::to_string(msgs_) + ",\"aircraft\":[";
        char b[160];
        for (size_t i = 0; i < v.size(); ++i) {
            const Aircraft& a = *v[i];
            if (i) o += ',';
            o += '{';
            if (a.icao) { std::snprintf(b, sizeof b, "\"icao\":\"%06X\",", a.icao); o += b; }
            if (!a.reg.empty()) o += "\"reg\":\"" + jsonEsc(a.reg) + "\",";
            if (!a.flight.empty()) o += "\"flight\":\"" + jsonEsc(a.flight) + "\",";
            if (!std::isnan(a.lat)) { std::snprintf(b, sizeof b, "\"lat\":%.4f,\"lon\":%.4f,\"posAge\":%.0f,", a.lat, a.lon, now - a.posAt); o += b; }
            if (!a.lastLabel.empty()) o += "\"label\":\"" + jsonEsc(a.lastLabel) + "\",\"labelName\":\"" + jsonEsc(labelName(a.lastLabel)) + "\",";
            if (bodies_ && withText && !a.lastText.empty()) o += "\"text\":\"" + jsonEsc(a.lastText) + "\",";
            std::snprintf(b, sizeof b, "\"level\":%.1f,\"n\":%d,\"age\":%.0f,", a.level, a.count, now - a.last); o += b;
            o += "\"via\":\"" + jsonEsc(a.via) + "\"}";
        }
        o += "]}";
        return o;
    }
    size_t size() { std::lock_guard<std::mutex> lk(m_); return rows_.size(); }
    uint64_t messages() { std::lock_guard<std::mutex> lk(m_); return msgs_; }
    /** Forget every row (a fresh entry into the mode keeps nothing from a radio that has moved on). */
    void clear() { std::lock_guard<std::mutex> lk(m_); rows_.clear(); gs_.clear(); msgs_ = other_ = 0; }

    /** ★ /addon/hfdl/aircraft — the HFDL map's shape (src/components/MapOverlay.tsx fetchAC): an array of
     *  {key, icao, flight, reg, lat, lon, last_seen (unix s), gs_id, freq_khz, msg_count}. ★ No sig_level: the map reads
     *  it as dBFS and the demodulator measures an SNR — a wrong unit on a meter is worse than no meter. Rows HFDL has heard
     *  in the last `keepS` seconds; positions only where one was reported. `nowUnix` is the wall clock (the map ages by it). */
    std::string hfdlAircraftJson(double now, double nowUnix, double keepS = 3600) {
        std::lock_guard<std::mutex> lk(m_);
        std::string o = "[";
        char b[200];
        bool first = true;
        for (const auto& a : rows_) {
            if (a.hfdlAt < 0 || now - a.hfdlAt > keepS) continue;
            o += first ? "{" : ",{"; first = false;
            if (a.icao) std::snprintf(b, sizeof b, "\"key\":\"%06X\",\"icao\":\"%06X\",", a.icao, a.icao);
            else std::snprintf(b, sizeof b, "\"key\":\"%s\",", jsonEsc(!a.flight.empty() ? a.flight : a.reg).c_str());
            o += b;
            if (!a.flight.empty()) o += "\"flight\":\"" + jsonEsc(a.flight) + "\",";
            if (!a.reg.empty()) o += "\"reg\":\"" + jsonEsc(a.reg) + "\",";
            if (!std::isnan(a.lat)) { std::snprintf(b, sizeof b, "\"lat\":%.4f,\"lon\":%.4f,", a.lat, a.lon); o += b; }
            std::snprintf(b, sizeof b, "\"last_seen\":%.0f,\"gs_id\":%d,\"freq_khz\":%.0f,\"msg_count\":%d}",
                          nowUnix - (now - a.hfdlAt), a.gsId, a.hfdlKhz, a.count);
            o += b;
        }
        return o + "]";
    }
    /** ★ /addon/hfdl/groundstations — every station on the public list with its position; `frequencies` are the ones
     *  THIS receiver has heard it on (never a list of our own), `last_heard` (unix s) when heard (no dBFS level: see above). */
    std::string hfdlGroundStationsJson(double now, double nowUnix) {
        std::lock_guard<std::mutex> lk(m_);
        std::string o = "[";
        char b[200];
        bool first = true;
        for (const auto& g : groundStations()) {
            std::snprintf(b, sizeof b, "%s{\"gs_id\":%d,\"location\":\"%s\",\"lat\":%.1f,\"lon\":%.1f,\"frequencies\":[", first ? "" : ",",
                          g.id, g.name, g.lat, g.lon);
            o += b; first = false;
            auto it = gs_.find(g.id);
            if (it != gs_.end()) {
                bool f1 = true;
                for (double k : it->second.khz) { std::snprintf(b, sizeof b, "%s{\"freq_khz\":%.0f,\"enabled\":true}", f1 ? "" : ",", k); o += b; f1 = false; }
                std::snprintf(b, sizeof b, "],\"last_heard\":%.0f}", nowUnix - (now - it->second.at));
                o += b;
            } else o += "]}";
        }
        return o + "]";
    }

private:
    void acars_(Aircraft& a, const vibeacars::Block& b) {
        if (!b.reg.empty()) a.reg = b.reg;
        if (!b.flight.empty()) a.flight = b.flight;
        a.lastLabel = b.label;
        if (bodies_) a.lastText = b.text; else a.lastText.clear();
    }
    /** The row for this aircraft: by ICAO address when known, else by registration, else by flight. Adopts a row found by
     *  registration when the ICAO address arrives later (VDL2 then ACARS for the same aircraft, or the other way). */
    Aircraft& row_(uint32_t icao, const std::string& reg, double now, const std::string& flight = "") {
        Aircraft* hit = nullptr;
        for (auto& a : rows_) {
            if ((icao && a.icao == icao) || (!reg.empty() && a.reg == reg) || (!icao && reg.empty() && !flight.empty() && a.flight == flight)) { hit = &a; break; }
        }
        if (!hit) {
            if (rows_.size() >= 2000) rows_.pop_front();
            rows_.emplace_back(); hit = &rows_.back(); hit->first = now;
        }
        if (icao) hit->icao = icao;
        if (!reg.empty()) hit->reg = reg;
        hit->last = now; hit->count++;
        return *hit;
    }
    struct GsHeard { double at = 0; float level = 0; std::vector<double> khz; };
    void heardGs_(int id, double khz, float level, double now) {
        if (id < 0 || id > 127) return;
        GsHeard& g = gs_[id];
        g.at = now; g.level = level;
        const double k = std::round(khz);
        if (std::find(g.khz.begin(), g.khz.end(), k) == g.khz.end()) {
            if (g.khz.size() >= 16) g.khz.erase(g.khz.begin());   // ★ bounded: a station has at most a dozen
            g.khz.push_back(k);
        }
    }
    std::mutex m_;
    std::deque<Aircraft> rows_;
    std::map<int, GsHeard> gs_;   // ★ ≤ 128 ids
    bool bodies_;
    uint64_t msgs_ = 0, other_ = 0;
};

/** The whole-radio decoder (ACARS or VDL2) on its own thread. `kind` "acars" | "vdl2". */
class Service {
public:
    explicit Service(Tracker& t) : tr_(t) {}
    ~Service() { stop(); }
    void start(const std::string& kind, double hwCentreHz, double rateHz, const std::vector<double>& chans) {
        stop();
        kind_ = kind; rate_ = rateHz;
        if (kind == "acars") acars_.reset(new vibeacars::Receiver(rateHz, hwCentreHz, chans));
        else vdl2_.reset(new vibevdl2::Receiver(rateHz, hwCentreHz, chans));
        { std::lock_guard<std::mutex> lk(mtx_); queue_.clear(); queued_ = 0; dropped_ = 0; run_ = true; health_ = Health(); clipN_ = clipSeen_ = 0; winStart_ = 0; }
        worker_ = std::thread([this] { loop_(); });
    }
    void stop() {
        { std::lock_guard<std::mutex> lk(mtx_); run_ = false; }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        acars_.reset(); vdl2_.reset();
    }
    bool running() { std::lock_guard<std::mutex> lk(mtx_); return run_; }
    /** From the DSP thread: copy and return. Over half a second queued, the OLDEST block goes (and is counted). */
    void feed(const cf32* iq, size_t n) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!run_) return;
            queue_.emplace_back(iq, iq + n);
            queued_ += n;
            while (queued_ > size_t(rate_ * 0.5) && queue_.size() > 1) { queued_ -= queue_.front().size(); queue_.pop_front(); ++dropped_; }
        }
        cv_.notify_one();
    }
    uint64_t dropped() { std::lock_guard<std::mutex> lk(mtx_); return dropped_; }
    uint64_t messages() const { return msgs_.load(); }
    const std::string& kind() const { return kind_; }
    /** ★ The gain rule's input (ISM's / AIS's vibeism::GainRule): the share of samples near full scale over the last
     *  second, measured on the service thread (every 4th sample), never the DSP's. `valid` once a second has passed. */
    struct Health { double clipFrac = 0; uint64_t msgs = 0; bool valid = false; };
    Health health() { std::lock_guard<std::mutex> lk(mtx_); return health_; }

private:
    static double now_() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
    void loop_() {
        std::unique_lock<std::mutex> lk(mtx_);
        while (run_) {
            cv_.wait_for(lk, std::chrono::milliseconds(250), [this] { return !run_ || !queue_.empty(); });
            while (run_ && !queue_.empty()) {
                std::vector<cf32> b = std::move(queue_.front());
                queue_.pop_front();
                queued_ -= b.size();
                lk.unlock();
                size_t clip = 0, seen = 0;
                for (size_t i = 0; i < b.size(); i += 4, ++seen) if (std::fabs(b[i].real()) > 0.93f || std::fabs(b[i].imag()) > 0.93f) ++clip;
                const double t = now_();
                if (acars_) acars_->feed(b.data(), b.size(), [&](const vibeacars::Block& x) { tr_.addAcars(x, t); ++msgs_; });
                if (vdl2_) vdl2_->feed(b.data(), b.size(), [&](const vibevdl2::Frame& x) { tr_.addVdl2(x, t); ++msgs_; });
                lk.lock();
                clipN_ += clip; clipSeen_ += seen;
                if (t - winStart_ >= 1.0) {
                    if (winStart_ > 0) { health_.clipFrac = clipSeen_ ? double(clipN_) / double(clipSeen_) : 0; health_.msgs = msgs_.load(); health_.valid = true; }
                    winStart_ = t; clipN_ = clipSeen_ = 0;
                }
            }
        }
    }
    Tracker& tr_;
    std::string kind_;
    double rate_ = 0;
    std::unique_ptr<vibeacars::Receiver> acars_;
    std::unique_ptr<vibevdl2::Receiver> vdl2_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::vector<cf32>> queue_;
    size_t queued_ = 0;
    uint64_t dropped_ = 0, clipN_ = 0, clipSeen_ = 0;
    double winStart_ = 0;
    Health health_;
    bool run_ = false;
    std::atomic<uint64_t> msgs_{0};
    std::thread worker_;
};

/** ★ THE ONE TABLE of this radio process: the datalink mode (ACARS / VDL2) and every listener's HFDL land here, so an
 *  aircraft heard on VHF and on HF is one row, and /addon/hfdl/* and the mode's report read the same rows. Bounded
 *  (2000 rows, 128 stations). Bodies start OFF: the shim sets the owner's choice before anything is decoded. */
inline Tracker& sharedTracker() { static Tracker t(false); return t; }

}  // namespace vibeavi
