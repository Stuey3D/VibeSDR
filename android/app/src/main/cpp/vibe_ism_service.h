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
#include "vibe_ism_bands.h"    // the four bands, reachability, the landing band
#include "vibe_ism_sink.h"     // ★ V12 2026-10-25: the gallery's copy of every new reading (vibe_ism_gallery.h)
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
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vibeism {

// ★ THE BANDS live in vibe_ism_bands.h (2026-10-25) — the background-decoding config validates against them too.


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
    if (k == "suspectfob") return "Suspected key fob";
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
    if (r.kind == "keyfob" || r.kind == "doorbell" || r.kind == "suspectfob") {
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
//  SUSPECTED KEY FOBS — a car key we cannot name still shows up (Stuart, 2026-10-25: "add them to a list that maybe
//  says suspected keyfob"). His key had lit the waterfall and appeared nowhere in the list.
//
//  ★★ WHAT A PRESS LOOKS LIKE, measured on his key (raw IQ, the Pi 500's V4L, 16 presses): FSK at 434.134 MHz,
//     ±42.7 kHz, every run 200 or 400 µs (Manchester, 2.5 kbit/s) after an alternating preamble, 68 ms frames, the
//     SAME frame 2-4 times 161 ms apart (more while the button is held). Car keys are rolling-code and proprietary, so
//     the name stays "suspected" — the shape, not a protocol, is the evidence. So a press is:
//       · an UNDECODED burst, FSK (deviation 10-120 kHz), at least 10 dB over the noise, 15-200 ms long;
//       · its runs Manchester-shaped: ≥ 90 % within a quarter of T or 2T, T = its shortest common run, 50-1000 µs;
//       · REPEATED: an earlier such frame within 30 kHz, of the same length (±15 %), 60-600 ms before it.
//  ★★ WHAT IS NOT, from the same recording: a neighbour's FSK sensor at 434.283 (one 47 ms frame every 1-2 s — never
//     repeated inside 600 ms), a carrier blip at 434.139 every second (OOK, not FSK), and the DONGLE'S OWN IMAGES of the
//     key ±120 kHz away when it overloads (0 dBFS) — one press is counted ONCE: after a press, nothing within 250 kHz
//     counts until its frames have stopped for 0.5 s (a held button is one press). And anything that comes back on a
//     TIMER (three intervals alike within 10 %, ≥ 8 s apart) is a sensor, not a hand on a button: that frequency stops
//     counting and its row goes.
//  ★★★ PRIVACY: no bits are read, kept or sent. The row is "Suspected key fob #n" by frequency (50 kHz) and symbol
//     time — two fobs of one make on one frequency share a row, which is the price of never reading an ID.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
class FobWatch {
public:
    /** Offer an UNDECODED burst. True = it completes a press: `out` is the reading to show. */
    bool offer(const Burst& b, double hwCentreHz, Reading& out) {
        const double t = b.tSec, f = hwCentreHz + b.offHz + b.carrierHz;
        while (!frames_.empty() && t - frames_.front().t > 1.5) frames_.pop_front();
        double T = 0, lenMs = 0;
        if (!candidate(b, T, lenMs)) return false;
        // ★ the same press (or its images): it lasts while its frames keep coming — a held button is ONE press — and the
        //   next press needs half a second of silence (his presses came 1.3 s apart; a fixed 1.5 s hold-off lost them)
        if (t < quietUntil_ && std::fabs(f - quietHz_) < 250e3) { quietUntil_ = t + 0.5; return false; }
        const long bucket = std::lround(f / 50e3);
        if (periodic_.count(bucket)) return false;
        bool repeat = false;
        for (const Frame& p : frames_) {
            const double dt = t - p.t;
            if (dt >= 0.060 && dt <= 0.600 && std::fabs(f - p.f) < 30e3 && std::fabs(lenMs - p.lenMs) <= 0.15 * p.lenMs) { repeat = true; break; }
        }
        frames_.push_back({t, f, lenMs});
        if (frames_.size() > 64) frames_.pop_front();
        if (!repeat) return false;
        quietUntil_ = t + 0.5; quietHz_ = f;
        // ★ a timer, not a hand: three intervals alike, ≥ 8 s apart
        auto& ts = presses_[bucket];
        ts.push_back(t);
        if (ts.size() > 4) ts.erase(ts.begin());
        if (ts.size() == 4) {
            const double a = ts[1] - ts[0], c = ts[2] - ts[1], d = ts[3] - ts[2];
            const double lo = std::min(a, std::min(c, d)), hi = std::max(a, std::max(c, d));
            if (lo >= 8.0 && hi <= lo * 1.1) { periodic_.insert(bucket); periodicHz_.push_back(f); return false; }
        }
        out = Reading();
        out.kind = "suspectfob"; out.make = "FSK"; out.secret = true;
        out.secretId = (uint64_t(bucket) << 12) ^ uint64_t(std::lround(T / 25.0));
        out.freqHz = f; out.levelDb = b.levelDb; out.snrDb = b.levelDb - b.noiseDb;
        ++presses_total_;
        return true;
    }
    /** Frequencies found to be on a timer since the last clear — their rows are removed by the caller. */
    std::vector<double> takePeriodic() { std::vector<double> v; v.swap(periodicHz_); return v; }
    uint64_t presses() const { return presses_total_; }
    void clear() { frames_.clear(); presses_.clear(); periodic_.clear(); periodicHz_.clear(); quietUntil_ = -1; }

    /** The burst-level test, public for the tests. */
    static bool candidate(const Burst& b, double& T, double& lenMs) {
        if (b.fsk.size() < 24 || b.fskDevHz < 10e3 || b.fskDevHz > 120e3 || b.levelDb - b.noiseDb < 10) return false;
        double all = 0; for (int d : b.fsk) all += std::abs(d);
        lenMs = all / 1000.0;
        if (lenMs < 15 || lenMs > 200) return false;
        // T: the shortest run that at least a fifth of the runs sit at (one stray glitch must not set it)
        std::vector<int> a; a.reserve(b.fsk.size()); for (int d : b.fsk) a.push_back(std::abs(d));
        std::sort(a.begin(), a.end());
        T = a[a.size() / 5];
        if (T < 50 || T > 1000) return false;
        size_t fit = 0;
        for (int d : a) if (std::fabs(d - T) <= 0.25 * T || std::fabs(d - 2 * T) <= 0.25 * T) ++fit;
        return double(fit) >= 0.9 * double(a.size());
    }

private:
    struct Frame { double t, f, lenMs; };
    std::deque<Frame> frames_;
    std::map<long, std::vector<double>> presses_;
    std::set<long> periodic_;
    std::vector<double> periodicHz_;
    double quietUntil_ = -1, quietHz_ = 0;
    uint64_t presses_total_ = 0;
};

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
    /** Take a reading heard at `t` (seconds). Returns true when the list changed. ★ Wipes r.secretId.
     *  `out` (when not null, and only when this returns true): the reading as the LIST counts it — its kind, its name and
     *  its readable line, nothing else (vibe_ism_sink.h) — for the gallery's daily log. */
    bool accept(Reading r, double t, Heard* out = nullptr) {
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
        if (!repeat && out) { out->kind = r.kind; out->name = deviceName(r, nick); out->line = line; out->freqHz = r.freqHz; out->secret = r.secret; }
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
    /** Remove the rows of `kind` within `hz` of `freq` (a suspected fob found to be a sensor on a timer). */
    void forget(const std::string& kind, double freq, double hz) {
        for (auto it = devs_.begin(); it != devs_.end();)
            if (it->second.r.kind == kind && std::fabs(it->second.r.freqHz - freq) < hz) it = devs_.erase(it); else ++it;
    }
    size_t size() const { return devs_.size(); }
    uint64_t decoded() const { return decoded_; }
    /** ★ V12 2026-10-25: the next nickname of `kind` will be above `floor` — today's gallery summary already names
     *  "TPMS #1"…"#floor" from before a restart, and a fresh salt cannot tell whether a car is one of those, so a new
     *  one must not take a name the day has already used. */
    void reserveNicks(const std::string& kind, int floor) { int& n = nextNick_[kind]; if (floor > n) n = floor; }

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

    /** Start on `band` with the radio tuned to `hwCentreHz` at `rateHz`. `customHz` > 0: a frequency typed in ISM mode
     *  (an odd device) — the whole capture is searched and the report names that frequency. `keepList`: a band step
     *  keeps the devices already heard (and the time base, so their "last heard" stays true); entering the mode clears. */
    /** ★ V12 2026-10-25 — `sink`: who keeps the readings (the gallery's daily log and summary, vibe_ism_gallery.h). It
     *  lives exactly as long as this run: stop() joins the worker, then calls its finish() (the RAM buffer written). */
    void start(const Band& band, double hwCentreHz, double rateHz, double customHz = 0, bool keepList = false,
               std::shared_ptr<Sink> sink = nullptr) {
        stop();
        std::lock_guard<std::mutex> lk(mtx_);
        sink_ = std::move(sink);
        band_ = &band; hwCentre_ = hwCentreHz; rate_ = rateHz; customHz_ = customHz;
        fe_.reset(new FrontEnd(rateHz));
        fe_->setDcGuardHz(15e3);
        // ★ detect inside the band's own span (+ a margin for drifting sensors), not on the tuner's skirts
        if (customHz <= 0) {
            const double mid = band.centreHz - hwCentreHz, half = band.needHz + 40e3;
            fe_->setUsableHz(mid - half, mid + half);
        }
        queue_.clear(); queued_ = 0; dropped_ = 0;
        if (!keepList) { t0_ = std::chrono::steady_clock::now(); tracker_.clear(); bursts_ = 0; unknown_ = 0; }
        fobs_.clear();   // ★ a new FrontEnd restarts burst time at 0
        cap_.reset(rateHz, now());
        health_ = Health{};
        run_ = true;
        worker_ = std::thread([this] { loop(); });
    }
    void stop() {
        { std::lock_guard<std::mutex> lk(mtx_); if (!run_) return; run_ = false; }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        // ★ The worker is gone, so nothing else touches the sink: its buffer is written here, once.
        std::shared_ptr<Sink> s;
        { std::lock_guard<std::mutex> lk(mtx_); s.swap(sink_); }
        if (s) s->finish();
    }
    /** ★ See Tracker::reserveNicks. Before start(), from the day's summary card. */
    void reserveNicks(const std::string& kind, int floor) { std::lock_guard<std::mutex> lk(mtx_); tracker_.reserveNicks(kind, floor); }
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
                for (const Burst& b : bs) {
                    size_t before = got.size(); decodeBurst(b, hc, got);
                    if (got.size() != before) continue;
                    ++unknownLocal_;
                    Reading sf;
                    if (fobs_.offer(b, hc, sf)) got.push_back(sf);       // ★ an undecoded press: "Suspected key fob"
                }
                lk.lock();
                for (double hz : fobs_.takePeriodic()) tracker_.forget("suspectfob", hz, 50e3);
                bursts_ += bs.size();
                unknown_ += unknownLocal_; unknownLocal_ = 0;
                const double t = now();
                std::vector<Heard> heard;
                for (auto& r : got) { Heard h; if (tracker_.accept(r, t, sink_ ? &h : nullptr) && sink_) heard.push_back(std::move(h)); }
                // ★ The gallery's copy OUTSIDE the lock: a report() asked for meanwhile never waits on a file.
                if (!heard.empty()) { auto s = sink_; lk.unlock(); for (const auto& h : heard) s->heard(h); lk.lock(); }
            }
            const double t = now();
            if (t - winStart >= 1.0 && sink_) { auto s = sink_; lk.unlock(); s->tick(); lk.lock(); }
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
    FobWatch fobs_;
    Health health_;
    std::shared_ptr<Sink> sink_;   // ★ only the worker reads it while running; start()/stop() swap it with the worker stopped
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
