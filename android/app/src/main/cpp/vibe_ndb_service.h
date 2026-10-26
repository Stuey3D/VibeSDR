// vibe_ndb_service.h — V12 NDB mode's decoder service: the LF slice cut off the DSP thread (vibe_wide_tap.h — the CW
// skimmer's slice path), the skimmer on it (decoders/ndb_skimmer.h), every beacon matched against the published list
// (vibe_ndb_db.h), harmonics flagged, the heard log, the stats, and the JSON the clients draw. docs/v12/NDB-SKIMMER.md.
//
// ★★★ OFF THE DSP THREAD, AND IT DROPS RATHER THAN WAITS (Network > Audio > Spectrum > Decoders). feed() is WideTap's
//     copy into a half-second queue that loses the OLDEST block and counts it (the health pill's "TOO HEAVY" reads
//     droppedBlocks()). Everything else runs on the tap's own decoder-priority thread.
// ★★ THE NETWORK NEVER WAITS ON THE DECODER: the report is built on the decoder's thread once a second and handed over
//    as a finished string under a lock held for a swap — report() is a copy.
// ★ Decoded text comes off the air: idents are A–Z/0–9 by construction (the engine emits nothing else), and every
//   string from the published list is JSON-escaped.
#pragma once
#include "decoders/ndb_skimmer.h"
#include "vibe_ndb_db.h"
#include "vibe_wide_tap.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace vibendb {

/** ★ The NDB band (ICAO Annex 10: 190–1750 kHz; in practice 190–535 kHz, Europe mostly 255–526.5). */
constexpr double kBandLoHz = 190e3, kBandHiHz = 535e3;
/** ★ Below this much of the band in the capture, the mode is not worth offering. */
constexpr double kMinSpanHz = 100e3;

/** Where to put the capture, given what the radio reaches ([reachLo, reachHi]) and its rate: the band (or as much of it
 *  as fits, centred on Europe's busy 255–526.5 kHz), the hardware centre a little off the 0.5 kHz raster so the DC
 *  remnant lands between beacons. Returns false when under kMinSpanHz of the band can be had. */
struct Plan { double hwCentre = 0, lo = 0, hi = 0; };
inline bool plan(double reachLo, double reachHi, double rate, Plan& p) {
    double lo = std::max(kBandLoHz, reachLo), hi = std::min(kBandHiHz, reachHi);
    if (hi - lo < kMinSpanHz) return false;
    const double usable = rate * 0.90;   // ★ the slice's (and the radio's) outer 5 % are filter roll-off
    if (hi - lo > usable) {
        // ★ centre on the core (255–526.5), clamped into what the radio reaches
        const double core = 0.5 * (255e3 + 526.5e3);
        double c = std::max(lo + usable / 2, std::min(hi - usable / 2, core));
        lo = c - usable / 2; hi = c + usable / 2;
    }
    if (hi - lo < kMinSpanHz) return false;
    p.lo = lo; p.hi = hi;
    p.hwCentre = std::floor((lo + hi) / 2 / 500.0) * 500.0 + 250.0;   // ★ x.25 / x.75 kHz: between raster beacons
    return true;
}

/** ★ The gallery's copy (vibe_ndb_gallery.h): called on the decoder's thread. */
struct HeardBeacon {
    std::string ident, name, cc;
    double hz = 0, lat = NAN, lon = NAN, km = -1, deg = 0;
    float snrDb = 0, toneHz = 0, wpm = 0;
    bool listed = false;
};
class Sink {
public:
    virtual ~Sink() = default;
    virtual void heard(const HeardBeacon& b, int64_t utcSec) = 0;   ///< a confirmed beacon, at most once a minute each
    virtual void tick() {}
    virtual void finish() {}
};

inline std::string jsonEsc(const std::string& s) {
    std::string o; o.reserve(s.size() + 4);
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c >= 0x20) o += (char)c;
    }
    return o;
}
/** Idents and copy are A–Z, 0–9, '?' and spaces by construction — this keeps it so whatever arrives. */
inline std::string cleanText(const std::string& s, size_t cap) {
    std::string o;
    for (char c : s) if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '?' || c == ' ') o += c;
    if (o.size() > cap) o.erase(0, o.size() - cap);
    return o;
}

/** {"type":"ndb_found",…}: published beacons nearest the receiver first, optionally filtered — the "Find NDB" box and
 *  the map's unheard beacons. `heard` holds "IDENT@<kHz×2>" keys of beacons identified (marked heard). */
inline std::string findJson(const std::string& q, size_t limit, bool have, double lat, double lon, const std::set<std::string>& heard) {
    std::string o = "{\"type\":\"ndb_found\",\"q\":\"" + jsonEsc(q.substr(0, 40)) + "\",\"pos\":" + (have ? "true" : "false") + ",\"list\":[";
    if (have) {
        const auto v = nearest(lat, lon, std::min<size_t>(limit, 400), q);
        bool first = true;
        for (const auto& f : v) {
            const std::string key = f.n.ident() + "@" + std::to_string((long long)std::lround(f.n.hz() / 500.0));
            char b[360];
            std::snprintf(b, sizeof b, "%s{\"ident\":\"%s\",\"hz\":%.0f,\"name\":\"%s\",\"cc\":\"%s\",\"lat\":%.4f,\"lon\":%.4f,\"km\":%.0f,\"brg\":%.0f,\"heard\":%s}",
                          first ? "" : ",", cleanText(f.n.ident(), 6).c_str(), f.n.hz(), jsonEsc(f.n.name()).c_str(), jsonEsc(f.n.cc()).c_str(),
                          f.n.lat(), f.n.lon(), f.km, f.deg, heard.count(key) ? "true" : "false");
            o += b; first = false;
        }
    }
    return o + "]}";
}

class Service {
public:
    using cf32 = std::complex<float>;
    ~Service() { stop(); }

    /** Start on a capture of `rate` centred (hardware) on `hwCentreHz`, searching [loHz, hiHz] RF. `havePos`: the
     *  receiver's position is known (lat, lon) — distances and bearings. `sink`: the gallery's daily log (or null). */
    void start(double rate, double hwCentreHz, double loHz, double hiHz, bool havePos, double lat, double lon,
               std::shared_ptr<Sink> sink = nullptr) {
        stop();
        {
            std::lock_guard<std::mutex> lk(m_);
            rate_ = rate; hw_ = hwCentreHz; lo_ = loHz; hi_ = hiHz; havePos_ = havePos; lat_ = lat; lon_ = lon;
            sink_ = std::move(sink);
            sk_.reset(); sliceRate_ = 0; sliceCentre_ = 0;
            report_.clear(); copy_.clear(); copySeq_ = 0;
            started_ = nowUtc();
        }
        vibe::WideTap::Geometry g;
        g.sampleRate = rate; g.hwCentreHz = hwCentreHz; g.wantCentreHz = 0.5 * (loHz + hiHz); g.widthHz = hiHz - loHz + 2600.0;
        g.fftSize = 16384;   // ★ the CW skimmer's tap size: at 2.4 MS/s, 195 transforms a second (vibe_wide_tap.h)
        tap_.aim(g);
        run_.store(true);
        tap_.start([this](const cf32* x, int n, double r, double c) { onSlice(x, n, r, c); });
    }
    void stop() {
        if (!run_.exchange(false)) return;
        tap_.stop();
        std::shared_ptr<Sink> s;
        { std::lock_guard<std::mutex> lk(m_); s.swap(sink_); sk_.reset(); }
        if (s) s->finish();
    }
    bool running() const { return run_.load(std::memory_order_relaxed); }
    /** From the DSP thread: copy and return (WideTap's bounded queue). */
    void feed(const cf32* iq, int n) { if (run_.load(std::memory_order_relaxed)) tap_.feed(iq, n); }
    uint64_t droppedBlocks() const { return tap_.droppedBlocks(); }
    /** The last report — {"type":"ndb",…} — or "" before the first. A copy: never waits on the decoder. */
    std::string report() const { std::lock_guard<std::mutex> lk(m_); return report_; }
    /** For the admin line: "12 beacons identified of 41 carriers". */
    std::string brief() const { std::lock_guard<std::mutex> lk(m_); return brief_; }
    int identifiedNow() const { return identified_.load(); }
    double loHz() const { return lo_; }
    double hiHz() const { return hi_; }
    double hwCentre() const { return hw_; }

    /** {"type":"ndb_stats",…}: per hour (beacons heard, furthest), by hour of day, and each beacon's SNR history —
     *  since the mode started on this server (in memory; the gallery keeps the days). */
    std::string stats() const {
        std::lock_guard<std::mutex> lk(m_);
        std::string o = "{\"type\":\"ndb_stats\",\"since\":" + std::to_string((long long)started_) + ",\"hours\":[";
        bool first = true;
        for (const auto& kv : hours_) {
            char b[96];
            std::snprintf(b, sizeof b, "%s{\"t\":%lld,\"heard\":%zu,\"far\":%.0f}", first ? "" : ",", (long long)kv.first * 3600,
                          kv.second.idents.size(), kv.second.farKm);
            o += b; first = false;
        }
        o += "],\"beacons\":[";
        first = true;
        for (const auto& kv : hist_) {
            const Hist& h = kv.second;
            double mean = 0; for (const auto& s : h.snr) mean += s.second; if (!h.snr.empty()) mean /= h.snr.size();
            char b[400];
            std::snprintf(b, sizeof b, "%s{\"ident\":\"%s\",\"hz\":%.0f,\"name\":\"%s\",\"cc\":\"%s\",\"km\":%.0f,\"minutes\":%d,\"mean\":%.1f,\"best\":%.1f,\"last\":%lld,\"snr\":[",
                          first ? "" : ",", h.ident.c_str(), h.hz, jsonEsc(h.name).c_str(), jsonEsc(h.cc).c_str(), h.km, h.minutes, mean, h.best,
                          (long long)h.last);
            o += b; first = false;
            bool f2 = true;
            for (const auto& s : h.snr) { char e[48]; std::snprintf(e, sizeof e, "%s[%lld,%.1f]", f2 ? "" : ",", (long long)s.first, s.second); o += e; f2 = false; }
            o += "]}";
        }
        return o + "]}";
    }

    /** {"type":"ndb_found",…}: published beacons nearest the receiver first, optionally filtered — the "Find NDB" box
     *  and the map's unheard beacons. `heard` marks those identified this session. */
    std::string find(const std::string& q, size_t limit) const {
        double lat, lon; bool have;
        std::set<std::string> heard;
        { std::lock_guard<std::mutex> lk(m_); lat = lat_; lon = lon_; have = havePos_; for (const auto& kv : log_) heard.insert(kv.first); }
        return findJson(q, limit, have, lat, lon, heard);
    }

    // ── test seams ──
    /** Feed a slice directly (no tap thread) — what the tests and the bench drive. */
    void feedSliceForTest(const cf32* x, int n, double rate, double centre) { onSlice(x, n, rate, centre); }
    void setClockForTest(std::function<int64_t()> c) { clock_ = std::move(c); }
    void buildReportForTest() { std::lock_guard<std::mutex> lk(work_); buildReport(); }

private:
    int64_t nowUtc() const {
        if (clock_) return clock_();
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    static int64_t wallMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // ── the decoder's thread ──
    void onSlice(const cf32* x, int n, double rate, double centre) {
        std::lock_guard<std::mutex> wk(work_);
        if (!sk_ || std::fabs(rate - sliceRate_) > 0.5 || std::fabs(centre - sliceCentre_) > 0.5) {
            vibe::ndb::Skimmer::Config c;
            c.rate = rate; c.loHz = lo_ - centre; c.hiHz = hi_ - centre;
            c.avoidHz = hw_ - centre; c.avoidWidthHz = 120;   // ★ the hardware centre's DC remnant is not a beacon
            auto s = std::make_unique<vibe::ndb::Skimmer>(c);
            s->setStartUtcMs(clock_ ? clock_() * 1000 : wallMs());
            s->onCopy = [this](const vibe::ndb::Copy& cp) { onCopy(cp); };
            std::lock_guard<std::mutex> lk(m_);
            sk_ = std::move(s); sliceRate_ = rate; sliceCentre_ = centre;
        }
        sk_->feedIq(x, n);
        const int64_t t = sk_->nowMs();
        if (t - lastReportMs_ >= 1000) { lastReportMs_ = t; buildReport(); }
    }
    void onCopy(const vibe::ndb::Copy& c) {
        std::lock_guard<std::mutex> lk(m_);
        copy_.push_back({ ++copySeq_, c.utcMs, sliceCentre_ + c.offsetHz, c.id, cleanText(c.text, 8), c.q });
        while (copy_.size() > 160) copy_.pop_front();   // ★ the list's live readings: every beacon's last few words
    }

    struct Row {
        vibe::ndb::BeaconView v;
        double hz = 0;
        Match m;
        int harmOf = 0; std::string harmIdent; double harmHz = 0;
    };
    struct LogEntry { std::string ident, name, cc; double hz = 0, km = -1, deg = 0, lat = NAN, lon = NAN; float best = -99; int64_t first = 0, last = 0; bool listed = false; int64_t sinkAt = 0; };
    struct Hour { std::set<std::string> idents; double farKm = 0; };
    struct Hist { std::string ident, name, cc; double hz = 0, km = -1; float best = -99; int minutes = 0; int64_t last = 0, lastMin = 0; std::deque<std::pair<int64_t, float>> snr; };

    /** ★ Once a second, on the decoder's thread: match, flag harmonics, update the log and stats, build the JSON. */
    void buildReport() {
        if (!sk_) return;
        const int64_t nowS = nowUtc();
        const auto views = sk_->beacons(true);
        std::vector<Row> rows;
        rows.reserve(views.size());
        for (const auto& v : views) {
            Row r; r.v = v; r.hz = sliceCentre_ + v.offsetHz;
            r.m = match(r.hz, v.ident, havePos_, lat_, lon_);
            rows.push_back(std::move(r));
        }
        // ★★ HARMONICS: a carrier at twice another's frequency (within a bin's worth either way, plus the clock's
        //    error doubled) is that one's second harmonic — flagged, and named after it.
        for (auto& r : rows) {
            if (!r.v.active) continue;
            for (const auto& f : rows) {
                if (&f == &r || !f.v.active || f.v.snrDb < r.v.snrDb) continue;
                if (std::fabs(r.hz - 2 * f.hz) <= 30.0) { r.harmOf = f.v.id; r.harmIdent = f.v.ident; r.harmHz = f.hz; break; }
            }
        }
        // ── the log and the stats (★ under m_: stats() and find() read them from the network thread) ──
        std::unique_lock<std::mutex> lk(m_);
        std::vector<HeardBeacon> toSink;
        const int64_t hour = nowS / 3600, minute = nowS / 60;
        int carriers = 0, ident = 0; double far = -1; std::string farIdent;
        for (const auto& r : rows) {
            if (!r.v.active) continue;
            ++carriers;
            if (r.v.ident.empty() || r.harmOf) continue;
            ++ident;
            // ★ "heard" = a confirmed ident read in the last 2 minutes (a fading beacon stops counting)
            const bool fresh = r.v.lastIdentMs > 0 && sk_->nowMs() - r.v.lastIdentMs < 120000;
            if (!fresh) continue;
            const std::string key = r.v.ident + "@" + std::to_string((long long)std::lround(r.hz / 500.0));
            LogEntry& e = log_[key];
            const bool listed = r.m.kind == Match::Exact;
            if (e.first == 0) { e.first = nowS; e.ident = r.v.ident; }
            e.last = nowS; e.hz = r.hz; e.best = std::max(e.best, r.v.snrDb); e.listed = listed;
            if (listed) { e.name = r.m.n.name(); e.cc = r.m.n.cc(); e.km = r.m.km; e.deg = r.m.deg; e.lat = r.m.n.lat(); e.lon = r.m.n.lon(); }
            if (listed && e.km > far) { far = e.km; farIdent = r.v.ident; }
            Hour& h = hours_[hour];
            h.idents.insert(key); if (listed) h.farKm = std::max(h.farKm, e.km);
            Hist& hs = hist_[key];
            hs.ident = r.v.ident; hs.hz = r.hz; hs.name = e.name; hs.cc = e.cc; hs.km = e.km; hs.best = std::max(hs.best, r.v.snrDb); hs.last = nowS;
            if (hs.lastMin != minute) {
                hs.lastMin = minute; ++hs.minutes;
                if (minute % 5 == 0 || hs.snr.empty()) { hs.snr.push_back({ nowS, r.v.snrDb }); while (hs.snr.size() > 288) hs.snr.pop_front(); }
            }
            if (sink_ && nowS - e.sinkAt >= 60) {
                e.sinkAt = nowS;
                HeardBeacon hb; hb.ident = r.v.ident; hb.hz = r.hz; hb.snrDb = r.v.snrDb; hb.toneHz = r.v.toneHz; hb.wpm = r.v.wpm; hb.listed = listed;
                if (listed) { hb.name = e.name; hb.cc = e.cc; hb.lat = e.lat; hb.lon = e.lon; hb.km = e.km; hb.deg = e.deg; }
                toSink.push_back(hb);
            }
        }
        while (hours_.size() > 24 * 7) hours_.erase(hours_.begin());
        while (hist_.size() > 400) { auto old = hist_.begin(); for (auto it = hist_.begin(); it != hist_.end(); ++it) if (it->second.last < old->second.last) old = it; hist_.erase(old); }
        identified_.store(ident);
        // ── coverage: of the published beacons on these frequencies within the furthest one identified, how many ──
        int published = 0, heardIn = 0;
        if (havePos_ && far > 0) {
            std::set<std::string> keys; for (const auto& kv : log_) if (kv.second.listed && nowS - kv.second.last < 600) keys.insert(kv.first);
            for (size_t i = 0; i < kNavaidCount; ++i) {
                const NavaidRec& n = kNavaids[i];
                if (n.hz < lo_ || n.hz > hi_) continue;
                double km, deg; distBearing(lat_, lon_, n.lat, n.lon, km, deg);
                if (km > far) continue;
                ++published;
                if (keys.count(std::string(n.ident) + "@" + std::to_string((long long)std::lround(n.hz / 500.0)))) ++heardIn;
            }
        }
        lk.unlock();
        std::string o;
        o.reserve(16384);
        char b[512];
        std::snprintf(b, sizeof b, "{\"type\":\"ndb\",\"centre\":%.0f,\"lo\":%.0f,\"hi\":%.0f,\"rate\":%.0f,\"now\":%lld,\"carriers\":%d,\"identified\":%d,"
                      "\"published\":%d,\"heardIn\":%d,\"pos\":%s,\"dropped\":%llu,\"bin\":%.2f,\"source\":\"%s\"",
                      hw_, lo_, hi_, rate_, (long long)nowS, carriers, ident, published, heardIn, havePos_ ? "true" : "false",
                      (unsigned long long)tap_.droppedBlocks(), sk_->binHzActual(), jsonEsc(kNavaidSource).c_str());
        o += b;
        if (far > 0) { std::snprintf(b, sizeof b, ",\"far\":{\"ident\":\"%s\",\"km\":%.0f}", cleanText(farIdent, 6).c_str(), far); o += b; }
        o += ",\"beacons\":[";
        int n = 0;
        for (const auto& r : rows) {
            if (n >= 160) break;
            if (!r.v.active && r.v.ident.empty()) continue;   // ★ a carrier that went without an ident: not worth a row
            if (n++) o += ',';
            const char* kind = r.m.kind == Match::Exact ? "match" : r.m.kind == Match::Near ? "near" : r.m.kind == Match::Candidate ? "cand" : "";
            std::snprintf(b, sizeof b, "{\"id\":%d,\"hz\":%.1f,\"snr\":%.1f,\"lvl\":%.1f,\"tone\":%.0f,\"tsnr\":%.1f,\"a1a\":%s,\"wpm\":%.1f,"
                          "\"ident\":\"%s\",\"votes\":%d,\"ballots\":%d,\"read\":\"%s\",\"active\":%s,\"first\":%lld,\"last\":%lld,\"idAt\":%lld,\"lastId\":%lld",
                          r.v.id, r.hz, r.v.snrDb, r.v.levelDb, r.v.toneHz, r.v.toneSnrDb, r.v.keyedCarrier ? "true" : "false", r.v.wpm,
                          cleanText(r.v.ident, 6).c_str(), r.v.votes, r.v.ballots, cleanText(r.v.reading, 6).c_str(), r.v.active ? "true" : "false",
                          (long long)r.v.firstMs, (long long)r.v.lastMs, (long long)r.v.identMs, (long long)r.v.lastIdentMs);
            o += b;
            if (*kind) {
                std::snprintf(b, sizeof b, ",\"db\":{\"k\":\"%s\",\"ident\":\"%s\",\"name\":\"%s\",\"cc\":\"%s\",\"hz\":%.0f,\"lat\":%.4f,\"lon\":%.4f,\"km\":%.0f,\"brg\":%.0f}",
                              kind, cleanText(r.m.n.ident(), 6).c_str(), jsonEsc(r.m.n.name()).c_str(), jsonEsc(r.m.n.cc()).c_str(), r.m.n.hz(),
                              r.m.n.lat(), r.m.n.lon(), r.m.km, r.m.deg);
                o += b;
            }
            if (!r.m.others.empty()) {
                o += ",\"alt\":[";
                for (size_t i = 0; i < r.m.others.size() && i < 4; ++i) {
                    double km = -1, deg = 0;
                    if (havePos_) distBearing(lat_, lon_, r.m.others[i].lat(), r.m.others[i].lon(), km, deg);
                    std::snprintf(b, sizeof b, "%s{\"ident\":\"%s\",\"name\":\"%s\",\"cc\":\"%s\",\"km\":%.0f}", i ? "," : "",
                                  cleanText(r.m.others[i].ident(), 6).c_str(), jsonEsc(r.m.others[i].name()).c_str(), jsonEsc(r.m.others[i].cc()).c_str(), km);
                    o += b;
                }
                o += "]";
            }
            if (!r.v.others.empty()) {
                o += ",\"also\":[";
                for (size_t i = 0; i < r.v.others.size(); ++i) o += (i ? ",\"" : "\"") + cleanText(r.v.others[i], 6) + "\"";
                o += "]";
            }
            if (r.harmOf) { std::snprintf(b, sizeof b, ",\"harm\":{\"id\":%d,\"ident\":\"%s\",\"hz\":%.0f}", r.harmOf, cleanText(r.harmIdent, 6).c_str(), r.harmHz); o += b; }
            o += ",\"copy\":\"" + cleanText(r.v.copy, 60) + "\"}";
        }
        o += "],\"log\":[";
        lk.lock();
        {
            std::vector<const LogEntry*> lv;
            for (const auto& kv : log_) lv.push_back(&kv.second);
            std::sort(lv.begin(), lv.end(), [](const LogEntry* a, const LogEntry* b) { return a->last > b->last; });
            for (size_t i = 0; i < lv.size() && i < 200; ++i) {
                const LogEntry& e = *lv[i];
                std::snprintf(b, sizeof b, "%s{\"ident\":\"%s\",\"hz\":%.0f,\"name\":\"%s\",\"cc\":\"%s\",\"km\":%.0f,\"brg\":%.0f,\"lat\":%.4f,\"lon\":%.4f,\"best\":%.1f,\"first\":%lld,\"last\":%lld}",
                              i ? "," : "", cleanText(e.ident, 6).c_str(), e.hz, jsonEsc(e.name).c_str(), jsonEsc(e.cc).c_str(), e.km, e.deg,
                              std::isfinite(e.lat) ? e.lat : 0.0, std::isfinite(e.lon) ? e.lon : 0.0, e.best, (long long)e.first, (long long)e.last);
                o += b;
            }
        }
        o += "],\"copy\":[";
        {
            bool first = true;
            for (const auto& c : copy_) {
                std::snprintf(b, sizeof b, "%s{\"s\":%llu,\"t\":%lld,\"hz\":%.0f,\"id\":%d,\"text\":\"%s\",\"q\":\"%s\"}", first ? "" : ",",
                              (unsigned long long)c.seq, (long long)c.t, c.hz, c.id, c.text.c_str(), c.q);
                o += b; first = false;
            }
        }
        o += "]}";
        char br[160];
        std::snprintf(br, sizeof br, "%d beacon%s identified of %d carrier%s", ident, ident == 1 ? "" : "s", carriers, carriers == 1 ? "" : "s");
        report_ = std::move(o);
        brief_ = br;
        lk.unlock();
        // ★ the gallery's copy OUTSIDE the report lock: a report() asked for meanwhile never waits on a file
        if (sink_) { for (const auto& h : toSink) sink_->heard(h, nowS); sink_->tick(); }
    }

    vibe::WideTap tap_;
    std::atomic<bool> run_{false};
    mutable std::mutex m_;        // ★ guards what the network thread reads (report_, brief_, copy_, the log for find())
    std::mutex work_;             // ★ the decoder's own state (one tap thread; the tests' direct feed)
    double rate_ = 0, hw_ = 0, lo_ = 0, hi_ = 0;
    bool havePos_ = false; double lat_ = 0, lon_ = 0;
    std::shared_ptr<Sink> sink_;
    std::unique_ptr<vibe::ndb::Skimmer> sk_;
    double sliceRate_ = 0, sliceCentre_ = 0;
    int64_t lastReportMs_ = 0, started_ = 0;
    std::string report_, brief_;
    struct CopyLine { unsigned long long seq; int64_t t; double hz; int id; std::string text; const char* q; };
    std::deque<CopyLine> copy_;
    unsigned long long copySeq_ = 0;
    std::map<std::string, LogEntry> log_;
    std::map<int64_t, Hour> hours_;
    std::map<std::string, Hist> hist_;
    std::atomic<int> identified_{0};
    std::function<int64_t()> clock_;
};

} // namespace vibendb
