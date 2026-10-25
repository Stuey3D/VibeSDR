// vibe_ism_service.h — V12 ISM mode's decoder service: the bands, the front end + protocols on their own thread, the
// privacy rule (per-server nicknames from a salted in-memory hash), the device list and the JSON the clients draw.
// docs/v12/ISM-DECODER.md.
//
// ★★★ OFF THE DSP THREAD, AND IT DROPS RATHER THAN WAITS — ADS-B's rule (vibe_adsb_service.h): Network > Audio >
//     Spectrum > Decoders. feed() copies and returns; a full queue loses the OLDEST block and counts it.
// ★★★ PRIVACY (Stuart, 2026-10-25). A TPMS, a key fob, a fixed-code remote is NEVER identified in anything this file
//     sends, logs or stores: no ID, no code, no raw bits. It is "TPMS #2" — a number handed out in the order the server
//     first heard each one, keyed by a SipHash of its ID under a salt drawn fresh at every start and held only in this
//     process's memory. The ID itself is wiped from the Reading the moment it is hashed. Public and private servers alike.
#pragma once

#include "vibe_ism_protocols.h"
#include "vibe_adsb_capture.h"     // vibeadsb::CaptureMeter — the same live "capture N %" as ADS-B
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vibeism {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE BANDS — the tuning controls step between these in ISM mode (Stuart, 2026-10-25: "I always found it quite hard
//  to tune the ISM on OWRX"). Each is one whole sub-band capture; nobody fine-tunes inside a band.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Band {
    const char* id;       ///< what the clients send ("433")
    const char* name;     ///< the frequency readout's text
    double centreHz;      ///< the band's middle — the "frequency" the clients show
    double dcOffsetHz;    ///< the RADIO is tuned this far above the middle, so its DC remnant misses the busy channel
    double needHz;        ///< the span the band's devices occupy around centreHz (for reachability)
};
inline const std::vector<Band>& bands() {
    static const std::vector<Band> b = {
        {"315", "315 MHz ISM",    315.00e6, 250e3, 200e3},   // US/Japan TPMS, fobs, remotes
        {"433", "433.92 MHz ISM", 433.92e6, 250e3, 870e3},   // 433.05–434.79: EU/UK sensors, weather, doorbells, TPMS
        {"868", "868 MHz SRD",    868.60e6, 0,     350e3},   // 868.3 + 868.95 (EU): LaCrosse IT+, Bresser, Fine Offset
        {"915", "915 MHz ISM",    915.00e6, 250e3, 200e3},   // US: LaCrosse, Ambient, Fine Offset 915 versions
    };
    return b;
}
inline const Band* bandById(const std::string& id) { for (const auto& b : bands()) if (id == b.id) return &b; return nullptr; }
inline const Band* bandNearest(double hz) {
    const Band* best = nullptr;
    for (const auto& b : bands()) if (!best || std::fabs(b.centreHz - hz) < std::fabs(best->centreHz - hz)) best = &b;
    return best;
}
/** ★ The landing band by the server's location: the Americas (ITU Region 2) → 315 MHz (TPMS and fobs there; Stuart's
 *  rule "US → 315/915 by what's most common"), everywhere else → 433.92. NAN position → 433.92. `can(id)` = reachable. */
template <class Can> inline const Band* landingBand(double lat, double lon, Can can) {
    const bool americas = lon == lon && lat == lat && lon < -25.0 && lon > -170.0;
    const char* order[4] = {"433", "868", "315", "915"};
    const char* amer[4] = {"315", "915", "433", "868"};
    for (int i = 0; i < 4; ++i) { const Band* b = bandById(americas ? amer[i] : order[i]); if (b && can(*b)) return b; }
    return nullptr;
}
static constexpr double kMinRateHz = 0.9e6;   ///< below this a band's devices do not fit (and the HF+ cannot reach one)

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  SipHash-2-4 (Aumasson & Bernstein, 2012 — public algorithm, reference description) for the nickname keys.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline uint64_t sipHash24(uint64_t k0, uint64_t k1, uint64_t m) {
    auto rotl = [](uint64_t x, int b) { return (x << b) | (x >> (64 - b)); };
    uint64_t v0 = 0x736f6d6570736575ULL ^ k0, v1 = 0x646f72616e646f6dULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0, v3 = 0x7465646279746573ULL ^ k1;
    auto round = [&] {
        v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32);
        v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;
        v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;
        v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32);
    };
    v3 ^= m; round(); round(); v0 ^= m;
    const uint64_t last = uint64_t(8) << 56;
    v3 ^= last; round(); round(); v0 ^= last;
    v2 ^= 0xff; round(); round(); round(); round();
    return v0 ^ v1 ^ v2 ^ v3;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  READABLE TEXT — "Oregon Scientific THGR810 · 18.4 °C · 62 % RH · ch 2"
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Units { bool mph = false; int pressure = 0; /* 0 PSI, 1 kPa, 2 bar */ };

inline std::string fmt1(double v, int dp) { char b[32]; std::snprintf(b, sizeof b, "%.*f", dp, v); return b; }
inline const char* compass(double deg) {
    static const char* p[16] = {"N","NNE","NE","ENE","E","ESE","SE","SSE","S","SSW","SW","WSW","W","WNW","NW","NNW"};
    int i = int(std::floor(std::fmod(deg + 11.25 + 3600.0, 360.0) / 22.5)) & 15;
    return p[i];
}
inline const char* kindLabel(const std::string& k) {
    if (k == "tpms") return "TPMS";
    if (k == "keyfob") return "Key fob";
    if (k == "doorbell") return "Doorbell / remote";
    return "";
}
/** The device's name: "Oregon Scientific THGR810", or for a secret kind "TPMS #2" / "Key fob #1". */
inline std::string deviceName(const Reading& r, int nick) {
    if (r.secret) {
        std::string n = kindLabel(r.kind);
        return n + " #" + std::to_string(nick);
    }
    return r.model.empty() ? r.make : r.make + " " + r.model;
}
inline std::string readable(const Reading& r, int nick, const Units& u = Units()) {
    std::vector<std::string> p;
    p.push_back(deviceName(r, nick));
    if (r.secret && r.kind == "tpms" && !r.model.empty()) p.push_back(r.model);   // "TPMS #2 · type B · …"
    if (r.secret && r.kind == "keyfob" && !r.make.empty()) p.push_back(r.make);    // "Key fob #1 · KeeLoq · …"
    if (r.kind == "tpms" && r.kpa == r.kpa) {
        if (u.pressure == 1) p.push_back(fmt1(r.kpa, 0) + " kPa");
        else if (u.pressure == 2) p.push_back(fmt1(r.kpa / 100.0, 2) + " bar");
        else p.push_back(fmt1(r.kpa * 0.1450377, 0) + " PSI");
    }
    if (r.tempC == r.tempC) p.push_back(fmt1(r.tempC, r.kind == "tpms" ? 0 : 1) + " \xc2\xb0" "C");
    if (r.hum == r.hum) p.push_back(fmt1(r.hum, 0) + " % RH");
    auto speed = [&](double kmh) { return u.mph ? fmt1(kmh / 1.609344, 0) + " mph" : fmt1(kmh, 0) + " km/h"; };
    if (r.windKmh == r.windKmh) {
        std::string w = "wind " + speed(r.windKmh);
        if (r.windDir == r.windDir) w += std::string(" ") + compass(r.windDir);
        p.push_back(w);
    }
    if (r.gustKmh == r.gustKmh) p.push_back("gust " + speed(r.gustKmh));
    if (r.rainMm == r.rainMm) p.push_back("rain " + fmt1(r.rainMm, 1) + " mm");
    if (r.rainRateMmH == r.rainRateMmH && r.rainRateMmH > 0) p.push_back(fmt1(r.rainRateMmH, 1) + " mm/h");
    if (r.uv == r.uv) p.push_back("UV " + fmt1(r.uv, 0));
    if (r.baroHpa == r.baroHpa) p.push_back(fmt1(r.baroHpa, 0) + " hPa");
    if (r.watts == r.watts) p.push_back(r.watts >= 1000 ? fmt1(r.watts / 1000.0, 2) + " kW" : fmt1(r.watts, 0) + " W");
    if (r.kind == "keyfob" || r.kind == "doorbell") {
        if (!r.button.empty()) p.push_back("button " + r.button);
        else if (r.note == "learn") p.push_back("learn");
        else p.push_back("button press");
    }
    if (!r.extra.empty()) p.push_back(r.extra);
    if (r.battOk == 1) p.push_back("battery OK"); else if (r.battOk == 0) p.push_back("battery low");
    if (!r.secret && r.channel >= 0) p.push_back("ch " + std::to_string(r.channel));
    std::string s;
    for (size_t i = 0; i < p.size(); ++i) { if (i) s += " \xc2\xb7 "; s += p[i]; }
    return s;
}

inline std::string jsonEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (uint8_t(c) < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE DEVICE LIST — newest first, one line per device, repeats folded, last-heard time.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
class Tracker {
public:
    static constexpr size_t kMaxDevices = 300;
    Tracker() {
        std::random_device rd;
        k0_ = (uint64_t(rd()) << 32) ^ rd();
        k1_ = (uint64_t(rd()) << 32) ^ rd();
    }
    /** Take a reading heard at `t` (seconds). Returns true when the list changed. ★ Wipes r.secretId. */
    bool accept(Reading r, double t) {
        std::string key;
        int nick = 0;
        if (r.secret) {
            const uint64_t h = sipHash24(k0_, k1_, r.secretId);
            r.secretId = 0;                                         // ★ gone — only the hash remains, in memory only
            const std::string fam = r.kind;
            auto it = nickOf_.find(h);
            if (it == nickOf_.end()) {
                if (nickOf_.size() > 4096) nickOf_.clear();           // bounded; numbering restarts, which is harmless
                it = nickOf_.emplace(h, ++nextNick_[fam]).first;
            }
            nick = it->second;
            char kb[40]; std::snprintf(kb, sizeof kb, "s%016llx", (unsigned long long)h);
            key = kb;                                                 // ★ internal map key; never sent (devices get "d<n>")
        } else {
            key = r.make + "|" + r.model + "|" + std::to_string(r.channel) + "|" + r.pubId;
        }
        if (r.confirm) {
            // ★ a weak check: the same values from the same device twice within 90 s, or nothing
            const std::string sig = key + "|" + readable(r, nick);
            auto pit = pending_.find(sig);
            if (pit == pending_.end() || t - pit->second > 90) {
                pending_[sig] = t;
                if (pending_.size() > 512) pending_.clear();
                return false;
            }
        }
        Dev& d = devs_[key];
        const std::string line = readable(r, nick);
        const bool fresh = d.count == 0;
        if (fresh) { d.id = ++nextId_; d.first = t; }
        const bool repeat = !fresh && line == d.line && t - d.last < 3.0;
        if (!repeat) ++d.count;
        d.r = r; d.nick = nick; d.line = line; d.last = t;
        ++decoded_;
        prune(t);
        return !repeat;
    }
    void prune(double t) {
        for (auto it = devs_.begin(); it != devs_.end();) {
            const bool brief = it->second.r.secret;                       // cars and fobs pass by: 15 min; sensors 2 h
            if (t - it->second.last > (brief ? 900 : 7200)) it = devs_.erase(it); else ++it;
        }
        while (devs_.size() > kMaxDevices) {
            auto old = devs_.begin();
            for (auto it = devs_.begin(); it != devs_.end(); ++it) if (it->second.last < old->second.last) old = it;
            devs_.erase(old);
        }
    }
    void clear() { devs_.clear(); pending_.clear(); }
    size_t size() const { return devs_.size(); }
    uint64_t decoded() const { return decoded_; }

    /** The device list, newest first: [{…}, …] */
    std::string json(double t) const {
        std::vector<const Dev*> v;
        for (const auto& kv : devs_) v.push_back(&kv.second);
        std::sort(v.begin(), v.end(), [](const Dev* a, const Dev* b) { return a->last > b->last; });
        std::string o = "[";
        bool first = true;
        for (const Dev* d : v) {
            const Reading& r = d->r;
            if (!first) o += ',';
            first = false;
            char b[96];
            std::snprintf(b, sizeof b, "{\"key\":\"d%llu\"", (unsigned long long)d->id);
            o += b;
            o += ",\"kind\":\"" + jsonEsc(r.kind) + "\",\"make\":\"" + jsonEsc(r.make) + "\",\"model\":\"" + jsonEsc(r.model) + "\"";
            o += ",\"name\":\"" + jsonEsc(deviceName(r, d->nick)) + "\"";
            if (r.secret) o += ",\"nick\":" + std::to_string(d->nick);
            else {
                if (!r.pubId.empty()) o += ",\"id\":\"" + jsonEsc(r.pubId) + "\"";
                if (r.channel >= 0) o += ",\"ch\":" + std::to_string(r.channel);
            }
            o += ",\"line\":\"" + jsonEsc(d->line) + "\"";
            auto num = [&](const char* k, double x, int dp) { if (x == x) { o += ",\""; o += k; o += "\":" + fmt1(x, dp); } };
            num("temp", r.tempC, 1); num("hum", r.hum, 0); num("wind", r.windKmh, 1); num("gust", r.gustKmh, 1);
            num("dir", r.windDir, 0); num("rain", r.rainMm, 1); num("rainRate", r.rainRateMmH, 1); num("kpa", r.kpa, 1);
            num("uv", r.uv, 0); num("watts", r.watts, 0); num("baro", r.baroHpa, 1);
            if (r.battOk >= 0) o += ",\"batt\":" + std::to_string(r.battOk);
            if (!r.button.empty()) o += ",\"button\":\"" + jsonEsc(r.button) + "\"";
            if (!r.note.empty()) o += ",\"note\":\"" + jsonEsc(r.note) + "\"";
            if (!r.extra.empty()) o += ",\"extra\":\"" + jsonEsc(r.extra) + "\"";
            std::snprintf(b, sizeof b, ",\"freq\":%.0f,\"level\":%.1f,\"snr\":%.1f,\"seen\":%.0f,\"age\":%.0f,\"count\":%d}",
                          r.freqHz, r.levelDb, r.snrDb, std::max(0.0, t - d->last), std::max(0.0, t - d->first), d->count);
            o += b;
        }
        return o + "]";
    }

private:
    struct Dev { uint64_t id = 0; Reading r; int nick = 0; std::string line; double first = 0, last = 0; int count = 0; };
    uint64_t k0_, k1_;
    std::map<std::string, Dev> devs_;
    std::unordered_map<uint64_t, int> nickOf_;
    std::map<std::string, int> nextNick_;
    std::map<std::string, double> pending_;
    uint64_t nextId_ = 0, decoded_ = 0;
};

/** One second's measurements for the status line and the gain rule. */
struct Health { double noiseDb = -99; double clipFrac = 0; uint64_t bursts = 0, decoded = 0; bool valid = false; };

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE SERVICE
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
class Service {
public:
    ~Service() { stop(); }

    /** Start on `band` with the radio tuned to `hwCentreHz` at `rateHz`. */
    void start(const Band& band, double hwCentreHz, double rateHz) {
        stop();
        std::lock_guard<std::mutex> lk(mtx_);
        band_ = &band; hwCentre_ = hwCentreHz; rate_ = rateHz; customHz_ = 0;
        fe_.reset(new FrontEnd(rateHz));
        fe_->setDcGuardHz(15e3);
        queue_.clear(); queued_ = 0; dropped_ = 0;
        t0_ = std::chrono::steady_clock::now();
        cap_.reset(rateHz, 0.0);
        tracker_.clear();
        bursts_ = 0; unknown_ = 0;
        health_ = Health{};
        run_ = true;
        worker_ = std::thread([this] { loop(); });
    }
    /** A typed exact frequency inside ISM mode (an odd device): the capture follows, the list is kept. */
    void setCustom(double hz) { std::lock_guard<std::mutex> lk(mtx_); customHz_ = hz; }
    void stop() {
        { std::lock_guard<std::mutex> lk(mtx_); if (!run_) return; run_ = false; }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    bool running() const { return run_.load(std::memory_order_relaxed); }
    const Band* band() const { return band_; }

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

    /** {"type":"ism", "band":"433", "bandName":…, "centre":Hz, "rate":…, "devices":[…], …} */
    std::string report() {
        std::lock_guard<std::mutex> lk(mtx_);
        const double t = now();
        std::string o = "{\"type\":\"ism\"";
        if (band_) o += std::string(",\"band\":\"") + band_->id + "\",\"bandName\":\"" + jsonEsc(band_->name) + "\"";
        char b[320];
        std::snprintf(b, sizeof b, ",\"centre\":%.0f,\"custom\":%d,\"rate\":%.0f,\"now\":%.1f,\"bursts\":%llu,\"decoded\":%llu,"
                      "\"unknown\":%llu,\"noise\":%.1f,\"dropped\":%llu,\"capture\":%d,\"devices\":",
                      customHz_ > 0 ? customHz_ : (band_ ? band_->centreHz : 0), customHz_ > 0 ? 1 : 0, rate_, t,
                      (unsigned long long)bursts_, (unsigned long long)tracker_.decoded(), (unsigned long long)unknown_,
                      health_.noiseDb, (unsigned long long)dropped_, cap_.percent(t));
        o += b;
        o += tracker_.json(t);
        return o + "}";
    }
    Health health() { std::lock_guard<std::mutex> lk(mtx_); return health_; }
    size_t deviceCount() { std::lock_guard<std::mutex> lk(mtx_); return tracker_.size(); }

    /** ★ For tests: run the decoder chain on one burst exactly as the worker does (no thread, no queue). */
    static void decodeBurst(const Burst& b, double hwCentreHz, std::vector<Reading>& out) {
        decodeAll(b, out);
        for (auto& r : out) { r.freqHz = hwCentreHz + b.offHz + b.carrierHz; r.levelDb = b.levelDb; r.snrDb = b.levelDb - b.noiseDb; }
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
                FrontEnd* fe = fe_.get();
                const double hc = hwCentre_;
                lk.unlock();
                std::vector<Reading> got;
                std::vector<Burst> bs;
                fe->feed(blk.data(), blk.size(), [&](const Burst& b) { bs.push_back(b); });
                for (const Burst& b : bs) { size_t before = got.size(); decodeBurst(b, hc, got); if (got.size() == before) ++unknownLocal_; }
                lk.lock();
                bursts_ += bs.size();
                unknown_ += unknownLocal_; unknownLocal_ = 0;
                const double t = now();
                for (auto& r : got) tracker_.accept(r, t);
            }
            const double t = now();
            if (t - winStart >= 1.0 && fe_) {
                winStart = t;
                health_.noiseDb = fe_->noiseDb();
                health_.clipFrac = fe_->clipFrac();
                health_.bursts = bursts_;
                health_.decoded = tracker_.decoded();
                health_.valid = true;
                tracker_.prune(t);
            }
        }
    }

    std::mutex mtx_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> run_{false};
    const Band* band_ = nullptr;
    double hwCentre_ = 0, rate_ = 0, customHz_ = 0;
    std::unique_ptr<FrontEnd> fe_;
    std::deque<std::vector<cf>> queue_;
    size_t queued_ = 0;
    uint64_t dropped_ = 0, bursts_ = 0, unknown_ = 0, unknownLocal_ = 0;
    vibeadsb::CaptureMeter cap_;
    std::chrono::steady_clock::time_point t0_;
    Tracker tracker_;
    Health health_;
};

/** ★ ISM's gain rule. Sensors are short strong bursts over a quiet band, like ADS-B, so it is not VibeAGC: start
 *  60 % up the radio's OWN gain list (any radio — the RTL's table, the RSP's 0–49 dB, the Airspy's list) and come
 *  down only when bursts clip; climb back one rung after two quiet minutes, never past the start. */
struct GainRule {
    double clipMax = 0.02;     ///< share of detector frames near full scale before we step down
    int decide(const Health& h, int idx, int startIdx, double quietFor) const {
        if (!h.valid) return 0;
        if (h.clipFrac > clipMax && idx > 0) return -1;
        if (h.clipFrac == 0 && idx < startIdx && quietFor > 120) return +1;
        return 0;
    }
};

} // namespace vibeism
