// vibe_ism_unknown.h — beta94: what ISM says about a burst it could NOT identify, the steady carriers it should not
// count as bursts at all, and a small ring of unidentified bursts' samples the OWNER can download.
//
// ★★ WHY (Stuart, 2026-11-24 22:35, the Sony on 868 MHz): the box said "0 devices · 65 bursts (65 unidentified)". The
//    detector heard plenty; nothing matched; and "868 is usually very active". A count says nothing anybody can act on.
//    Each unidentified burst is now DESCRIBED — how it is modulated (OOK / FSK), how it is coded (PWM / PPM / NRZ or
//    Manchester), its rate, length, frequency and level — and the last few are KEPT as IQ, so a protocol can be identified
//    (rtl_433 -r reads the file as it is named) and then added here.
// ★★ A STEADY CARRIER IS NOT A BURST. Two drifting carriers near 868.10 and 868.27 MHz on the Sony were local
//    interference, not devices. A burst that is one long unbroken mark with no FSK data in it is labelled a carrier and
//    listed as one (frequency, level, how far it has drifted); it is not counted as unidentified. The detector's held
//    frequencies (vibe_ism.h kHoldQuietS — what it already ignores) go on the same list.
// ★★★ PRIVACY: the descriptions carry no bits — only shape. The IQ COULD hold a fixed-code remote's code, which is why
//     the clips are the OWNER's alone (the admin API, local_sdr_shim.cpp "ism-clip"); listeners see the descriptions.
// ★★★ BOUNDED (the 256 MB rule): at most kMaxClips clips, each at most kMaxClipBytes, all of them at most kMaxBytes, as
//     8-bit IQ (cu8, 2 bytes a sample) — 1.5 MB in the worst case, the oldest dropped first. Descriptions: the last 16.
// ★ Runs on vibe-ism3 (the slicer's thread), never the DSP or audio thread. Per unidentified burst: a sort of its run
//   lengths and, when kept, one pass converting its baseband to bytes. Nothing here is per-capture-sample.
#pragma once

#include "vibe_ism.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vibeism {

/** What the run lengths say about a burst's coding. `rate` is bits a second for PWM / PPM (one bit a mark) and SYMBOLS
 *  a second for NRZ / Manchester (the shortest run's rate — NRZ's bit rate, twice a Manchester signal's). */
struct RunStats {
    std::string coding;   ///< "PWM" | "PPM" | "NRZ/Manchester" | "" (too few runs to say)
    double rate = 0;      ///< see above; 0 = not known
    double unitUs = 0;    ///< the shortest run, refined over every run that is a whole number of it
    int pulses = 0;       ///< marks (OOK) / high-tone runs (FSK)
    double spanMs = 0;    ///< from the first mark's start to the last mark's end
};

/** One unidentified burst, described. */
struct BurstDesc {
    uint32_t id = 0;
    double t = 0;            ///< the service's clock when it was heard (for "ago")
    double freqHz = 0;
    float levelDb = -99, snrDb = 0;
    double durMs = 0;        ///< the transmission's own length (spanMs), else the channel's
    std::string mod;         ///< "OOK" | "FSK" | "carrier"
    RunStats rs;
    double devHz = 0;        ///< FSK: half the tone spacing
    bool clipped = false;
};

namespace unk {
inline double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    const size_t k = std::min(v.size() - 1, size_t(p * double(v.size() - 1) + 0.5));
    std::nth_element(v.begin(), v.begin() + long(k), v.end());
    return v[k];
}
inline double spread(const std::vector<double>& v) { if (v.size() < 3) return 1; const double lo = pct(v, 0.1); return lo > 0 ? pct(v, 0.9) / lo : 1; }
}  // namespace unk

/** The coding and rate from signed run lengths (µs, + mark / − space). `fsk`: the runs are tones, not carrier on/off. */
inline RunStats analyseRuns(const Runs& r, bool fsk) {
    RunStats o;
    if (r.empty()) return o;
    // the span: first mark's start to the last mark's end (trailing / leading spaces are the quiet around the burst)
    size_t a = 0, e = r.size();
    while (a < e && r[a] <= 0) ++a;
    while (e > a && r[e - 1] <= 0) --e;
    double span = 0;
    for (size_t i = a; i < e; ++i) span += std::abs(r[i]);
    o.spanMs = span / 1000.0;
    for (size_t i = a; i < e; ++i) o.pulses += r[i] > 0;
    // the inner runs (the first and last may be cut), without the gaps between repeated frames
    std::vector<double> all;
    for (size_t i = a + 1; i + 1 < e; ++i) all.push_back(std::abs(r[i]));
    if (all.size() < 6) return o;
    const double med = unk::pct(all, 0.5);
    std::vector<double> v, marks, spaces, periods;
    for (size_t i = a + 1; i + 1 < e; ++i) {
        const double d = std::abs(r[i]);
        if (d > 8 * med) continue;   // a gap between frames
        v.push_back(d);
        if (r[i] > 0) {
            marks.push_back(d);
            if (std::abs(r[i + 1]) <= 8 * med && r[i + 1] < 0) { spaces.push_back(std::abs(r[i + 1])); periods.push_back(d + std::abs(r[i + 1])); }
        }
    }
    if (v.size() < 6) return o;
    // the unit: the shortest cluster, then refined over every run that is a whole number (1–8) of it
    const double u0 = unk::pct(v, 0.1);
    std::vector<double> c;
    for (double d : v) if (d <= 1.5 * u0) c.push_back(d);
    const double u1 = c.empty() ? u0 : unk::pct(c, 0.5);
    double sx = 0, sk = 0;
    for (double d : v) {
        const double k = std::round(d / u1);
        if (k >= 1 && k <= 8 && std::fabs(d - k * u1) <= 0.3 * u1) { sx += d; sk += k; }
    }
    o.unitUs = sk > 0 ? sx / sk : u1;
    if (!(o.unitUs > 0)) return o;
    if (!fsk && marks.size() >= 4 && spaces.size() >= 3) {
        const double ms = unk::spread(marks), ss = unk::spread(spaces), ps = unk::spread(periods);
        // PWM: the MARK carries the bit (two widths), the period steady or the space steady
        if (ms >= 1.6 && (ps < 1.25 || ss < 1.6)) { o.coding = "PWM"; o.rate = 1e6 / unk::pct(periods, 0.5); return o; }
        // PPM: the marks all alike, the SPACE carries the bit
        if (ms < 1.6 && ss >= 1.6) { o.coding = "PPM"; o.rate = 1e6 / unk::pct(periods, 0.5); return o; }
    }
    o.coding = "NRZ/Manchester";
    o.rate = 1e6 / o.unitUs;
    return o;
}

/** Among a burst's slices (finest first), the first whose run count the next coarser one agrees with (±15 %) — the
 *  smoothing that neither splits a run on noise nor merges two real ones. None agree: the finest. */
inline const Runs* stableSlice(const std::vector<Runs>& all) {
    if (all.empty()) return nullptr;
    for (size_t i = 0; i + 1 < all.size(); ++i) {
        const double x = double(all[i].size()), y = double(all[i + 1].size());
        if (x >= 4 && std::fabs(x - y) <= 0.15 * std::max(x, y)) return &all[i];
    }
    return &all.front();
}

/** ★ A steady carrier, not a burst: one unbroken mark of at least kCarrierMinS (the coarsest slice's longest mark, and
 *  most of all its on-time), and no FSK data in it (fewer than 20 tone changes a second — a drifting carrier's
 *  discriminator wanders; a data burst's flips at its bit rate, ≥ 300 a second).
 *  ★★ WHY SO SHORT A MARK COUNTS: the detector's floor learns a carrier that stays put in ~0.15 s and the channel then
 *     closes as "quiet" — and a DRIFTING one (the Sony's 868.10 / 868.27) moves into fresh bins and is "heard" again and
 *     again, each time one long unbroken mark. That is what filled "65 bursts (65 unidentified)". No sensor sends a tenth
 *     of a second of plain carrier: every one tested (test-ism) has a preamble of edges, or FSK under a flat envelope. */
constexpr double kCarrierMinS = 0.1;
inline bool isCarrier(const Burst& b) {
    if (b.ookAll.empty()) return false;
    const Runs& r = b.ookAll.back();
    int longest = 0, onUs = 0;
    for (int d : r) if (d > 0) { longest = std::max(longest, d); onUs += d; }
    if (longest < kCarrierMinS * 1e6 || longest < 0.8 * onUs) return false;
    if (!b.fskAll.empty()) {
        const Runs& f = b.fskAll.front();
        double s = 0; for (int d : f) s += std::abs(d);
        if (s > 0 && double(f.size()) / (s * 1e-6) >= 20) return false;
    }
    return true;
}

/** Describe an unidentified burst. `hwCentreHz`: where the radio is tuned (the burst's offset is from there). */
inline BurstDesc describeBurst(const Burst& b, double hwCentreHz) {
    BurstDesc d;
    d.freqHz = hwCentreHz + b.offHz + b.carrierHz;
    d.levelDb = b.levelDb; d.snrDb = b.levelDb - b.noiseDb; d.clipped = b.clipped;
    if (isCarrier(b)) { d.mod = "carrier"; d.durMs = b.durS * 1000; return d; }
    const bool fsk = b.fskDevHz > 0 && !b.fskAll.empty() && b.fskAll.front().size() >= 8;
    d.mod = fsk ? "FSK" : "OOK";
    if (fsk) d.devHz = b.fskDevHz;
    const Runs* r = stableSlice(fsk ? b.fskAll : b.ookAll);
    if (r) d.rs = analyseRuns(*r, fsk);
    d.durMs = d.rs.spanMs > 0 ? d.rs.spanMs : b.durS * 1000;
    return d;
}

/** The burst's channel baseband as unsigned 8-bit IQ (rtl_433's cu8), scaled so its peak sits at ±100 of 127.5 — the
 *  most resolution 8 bits can give a burst whatever its level (the level is in the description). At most `maxSamples`
 *  from the start (the pre-roll and the burst's beginning, where a protocol's preamble and sync are). */
inline std::vector<uint8_t> toCu8(const std::vector<cf>& x, size_t maxSamples) {
    const size_t n = std::min(x.size(), maxSamples);
    float pk = absPeakRun(x.data(), n, 0.0f);   // ★ NEON / SSE2 (vibe_ism.h)
    const float k = pk > 0 ? 100.0f / pk : 1.0f;
    std::vector<uint8_t> o(2 * n);
    for (size_t i = 0; i < n; ++i) {
        o[2 * i]     = uint8_t(std::max(0.0f, std::min(255.0f, std::round(127.5f + x[i].real() * k))));
        o[2 * i + 1] = uint8_t(std::max(0.0f, std::min(255.0f, std::round(127.5f + x[i].imag() * k))));
    }
    return o;
}

/** "ism_868.2715M_500k_20261124T223512Z.cu8" — the frequency, the sample rate, when (UTC) and the format, in the shape
 *  rtl_433 reads from a file's name (-r: "_<freq>M_", "_<rate>k_", the extension), so it plays as it is. */
inline std::string clipName(double freqHz, double rateHz, double unixSec) {
    const time_t tt = time_t(unixSec);
    struct tm g{};
#if defined(_WIN32)
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char b[96];
    std::snprintf(b, sizeof b, "ism_%.4fM_%.0fk_%04d%02d%02dT%02d%02d%02dZ.cu8", freqHz / 1e6, rateHz / 1e3,
                  g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

inline std::string unkFmt(double v, int dp) { char b[32]; std::snprintf(b, sizeof b, "%.*f", dp, v); return b; }

/** The steady carriers: merged by frequency (a drifting one stays one line, its drift range kept), forgotten after two
 *  quiet minutes, at most kMax. */
class CarrierList {
public:
    static constexpr int kMax = 8;
    static constexpr double kMergeHz = 30e3, kForgetS = 120;
    struct C { double hz = 0, lo = 0, hi = 0; float lvl = -99; double first = 0, last = 0; uint32_t n = 0; };
    void offer(double hz, float lvl, double t) {
        C* best = nullptr; double bd = 1e18;
        for (auto& c : list_) {
            const double d = hz < c.lo ? c.lo - hz : hz > c.hi ? hz - c.hi : 0;
            if (d <= kMergeHz && d < bd) { bd = d; best = &c; }
        }
        if (!best) {
            if (int(list_.size()) >= kMax) {   // full: the stalest goes
                auto it = std::min_element(list_.begin(), list_.end(), [](const C& a, const C& b) { return a.last < b.last; });
                list_.erase(it);
            }
            C c; c.hz = c.lo = c.hi = hz; c.lvl = lvl; c.first = c.last = t; c.n = 1;
            list_.push_back(c);
            return;
        }
        best->hz = hz; best->lo = std::min(best->lo, hz); best->hi = std::max(best->hi, hz);
        best->lvl = lvl; best->last = t; ++best->n;
    }
    void prune(double t) { list_.erase(std::remove_if(list_.begin(), list_.end(), [&](const C& c) { return t - c.last > kForgetS; }), list_.end()); }
    void clear() { list_.clear(); }
    size_t size() const { return list_.size(); }
    const std::vector<C>& list() const { return list_; }
    /** [{"hz":868100000,"lvl":-31.2,"lo":…,"hi":…,"age":3.1,"for":42.0}] — strongest first */
    std::string json(double t) const {
        std::vector<const C*> v; for (const auto& c : list_) v.push_back(&c);
        std::sort(v.begin(), v.end(), [](const C* a, const C* b) { return a->lvl > b->lvl; });
        std::string o = "[";
        for (const C* c : v) {
            if (o.size() > 1) o += ",";
            o += "{\"hz\":" + unkFmt(c->hz, 0) + ",\"lvl\":" + unkFmt(c->lvl, 1) + ",\"lo\":" + unkFmt(c->lo, 0) + ",\"hi\":" + unkFmt(c->hi, 0)
               + ",\"age\":" + unkFmt(std::max(0.0, t - c->last), 1) + ",\"for\":" + unkFmt(std::max(0.0, c->last - c->first), 1) + "}";
        }
        return o + "]";
    }
private:
    std::vector<C> list_;
};

/** The last unidentified bursts, described, and the samples of the last few, for the owner. Its own lock: the HTTP
 *  thread reads a clip while vibe-ism3 adds. */
class UnknownBook {
public:
    struct Limits { size_t descs = 16, clips = 12, clipBytes = 256 * 1024, bytes = 1536 * 1024; };
    UnknownBook() = default;
    explicit UnknownBook(Limits l) : lim_(l) {}
    struct Clip { uint32_t id = 0; std::string name; std::vector<uint8_t> data; bool cut = false; };

    /** Keep `d` (its id is set here) and, when `bb` is given, a clip of it. Returns the id. */
    uint32_t add(BurstDesc d, const std::vector<cf>* bb, double rateHz, double unixSec) {
        std::vector<uint8_t> bytes; bool cut = false;
        if (bb && !bb->empty() && rateHz > 0 && lim_.clips > 0) {
            const size_t maxS = lim_.clipBytes / 2;
            cut = bb->size() > maxS;
            bytes = toCu8(*bb, maxS);   // ★ outside the lock
        }
        std::lock_guard<std::mutex> lk(m_);
        d.id = ++next_;
        descs_.push_front(d);
        while (descs_.size() > lim_.descs) descs_.pop_back();
        if (!bytes.empty()) {
            Clip c; c.id = d.id; c.name = clipName(d.freqHz, rateHz, unixSec); c.data = std::move(bytes); c.cut = cut;
            bytes_ += c.data.size();
            clips_.push_front(std::move(c));
            while (!clips_.empty() && (clips_.size() > lim_.clips || bytes_ > lim_.bytes)) { bytes_ -= clips_.back().data.size(); clips_.pop_back(); }
        }
        return d.id;
    }
    bool clip(uint32_t id, std::string& name, std::string& data) const {
        std::lock_guard<std::mutex> lk(m_);
        for (const auto& c : clips_) if (c.id == id) { name = c.name; data.assign(c.data.begin(), c.data.end()); return true; }
        return false;
    }
    size_t clipCount() const { std::lock_guard<std::mutex> lk(m_); return clips_.size(); }
    size_t clipBytes() const { std::lock_guard<std::mutex> lk(m_); return bytes_; }
    size_t descCount() const { std::lock_guard<std::mutex> lk(m_); return descs_.size(); }
    void clear() { std::lock_guard<std::mutex> lk(m_); descs_.clear(); clips_.clear(); bytes_ = 0; }
    /** [{"id":7,"ago":2.1,"hz":…,"mod":"OOK","code":"PWM","rate":1000,"unit":250,"n":64,"ms":64.0,"lvl":-22.0,"snr":24.1,
     *    "dev":…,"clipped":1,"iq":1,"kb":64,"cut":1}] — newest first; `iq` 1 = the owner can download it (ism-clip?id=7) */
    std::string json(double t) const {
        std::lock_guard<std::mutex> lk(m_);
        std::string o = "[";
        for (const auto& d : descs_) {
            if (o.size() > 1) o += ",";
            o += "{\"id\":" + std::to_string(d.id) + ",\"ago\":" + unkFmt(std::max(0.0, t - d.t), 1) + ",\"hz\":" + unkFmt(d.freqHz, 0)
               + ",\"mod\":\"" + d.mod + "\"";
            if (!d.rs.coding.empty()) o += ",\"code\":\"" + d.rs.coding + "\"";
            if (d.rs.rate > 0) o += ",\"rate\":" + unkFmt(d.rs.rate, 0);
            if (d.rs.unitUs > 0) o += ",\"unit\":" + unkFmt(d.rs.unitUs, 0);
            if (d.rs.pulses > 0) o += ",\"n\":" + std::to_string(d.rs.pulses);
            o += ",\"ms\":" + unkFmt(d.durMs, 1) + ",\"lvl\":" + unkFmt(d.levelDb, 1) + ",\"snr\":" + unkFmt(d.snrDb, 1);
            if (d.devHz > 0) o += ",\"dev\":" + unkFmt(d.devHz, 0);
            if (d.clipped) o += ",\"clipped\":1";
            for (const auto& c : clips_) if (c.id == d.id) {
                o += ",\"iq\":1,\"kb\":" + std::to_string((c.data.size() + 1023) / 1024);
                if (c.cut) o += ",\"cut\":1";
                break;
            }
            o += "}";
        }
        return o + "]";
    }
private:
    Limits lim_;
    mutable std::mutex m_;
    std::deque<BurstDesc> descs_;
    std::deque<Clip> clips_;
    size_t bytes_ = 0;
    uint32_t next_ = 0;
};

}  // namespace vibeism
