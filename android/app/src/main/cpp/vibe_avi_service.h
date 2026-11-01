// vibe_avi_service.h — V12 aviation datalink: the service around the decoders (vibe_acars.h, vibe_vdl2.h, vibe_hfdl.h).
// Its own thread fed by copy, the aircraft table every message lands in, and the JSON the clients draw.
// docs/v12/AVIATION.md §Service. ★ NOT WIRED YET into local_sdr_shim.cpp — see AVIATION.md "RESUME HERE".
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
};

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
        if (!m.downlink || (!m.icao && m.flight.empty() && !m.hasAcars)) { ++other_; return; }
        Aircraft& a = row_(m.icao, m.hasAcars ? m.acars.reg : "", now, m.icao ? "" : m.flight);
        char via[64]; std::snprintf(via, sizeof via, "HFDL %.0f%s%s", m.hz / 1e3, *vibehfdl::gsName(m.dstId) ? " " : "", vibehfdl::gsName(m.dstId));
        a.via = via;
        if (!m.flight.empty()) a.flight = m.flight;
        if (!std::isnan(m.lat)) { a.lat = m.lat; a.lon = m.lon; a.posAt = now; }
        if (m.hasAcars && m.acarsCrcOk) acars_(a, m.acars);
        ++msgs_;
    }
    /** Rows heard in the last `keepS` seconds, newest first: {"aircraft":[{icao, reg, flight, lat, lon, via, n, age, label, text}]}. */
    std::string json(double now, double keepS = 3600) {
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
            if (bodies_ && !a.lastText.empty()) o += "\"text\":\"" + jsonEsc(a.lastText) + "\",";
            std::snprintf(b, sizeof b, "\"level\":%.1f,\"n\":%d,\"age\":%.0f,", a.level, a.count, now - a.last); o += b;
            o += "\"via\":\"" + jsonEsc(a.via) + "\"}";
        }
        o += "]}";
        return o;
    }
    size_t size() { std::lock_guard<std::mutex> lk(m_); return rows_.size(); }

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
    std::mutex m_;
    std::deque<Aircraft> rows_;
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
        { std::lock_guard<std::mutex> lk(mtx_); queue_.clear(); queued_ = 0; dropped_ = 0; run_ = true; }
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
                const double t = now_();
                if (acars_) acars_->feed(b.data(), b.size(), [&](const vibeacars::Block& x) { tr_.addAcars(x, t); ++msgs_; });
                if (vdl2_) vdl2_->feed(b.data(), b.size(), [&](const vibevdl2::Frame& x) { tr_.addVdl2(x, t); ++msgs_; });
                lk.lock();
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
    uint64_t dropped_ = 0;
    bool run_ = false;
    std::atomic<uint64_t> msgs_{0};
    std::thread worker_;
};

}  // namespace vibeavi
