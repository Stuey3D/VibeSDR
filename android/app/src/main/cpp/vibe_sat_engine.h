// vibe_sat_engine.h — V12 SATELLITE ENGINE (docs/v12/SATELLITE-ENGINE.md): the curated satellite list, element sets
// from CelesTrak's OMM JSON, the server's own sky (passes, look angles), and the DOPPLER LOCK — a listener's (or the
// shared dial's) channel following the predicted Doppler, trimmed by a slow closed loop on FM repeaters.
//
// ★★★ COMPLEXITY IN THE ENGINE, NOT THE SETTINGS (brief §1): one button. The dial shows the NOMINAL (published)
//     frequency; the channel sits at nominal + shift. The shift is applied by the caller inside the channel's own NCO
//     (RxPipeline::setTune on the same mode and width = a phase-continuous NCO move, no rebuild, no AGC reset) — never by
//     retuning the radio, never through the tune pacer, never as a "user tune" (the SHARED-DIAL CONTRACT: the server's
//     own moves are not tunes; brief §4.2 TRAP).
// ★★★ SMOOTH BY CONSTRUCTION: the model is a continuous function of time evaluated every tick (25 Hz → at most ~9 Hz a
//     step at 70 cm overhead, under the FM channel's own noise); the loop's trim is slew-limited (kTrimSlewHzS) and
//     clamped (kTrimClampHz — the capture-range TRAP: without it the loop locks to the steady carrier beside ISS).
// ★★ THE LOOP ONLY WHERE IT HELPS: `fll` (FM repeaters — every voice shares the satellite's one Doppler curve), never on
//    a linear transponder (`none` — each uplinker brings their own error; the loop would chase whoever talks), never on
//    `costas` (a digital demodulator does its own fine tracking; the engine is its feed-forward).
// ★★ HOOK FOR DECODERS (LRPT, AX.25 9600): Engine::selected(key) / Engine::dopplerFor(norad, hz, t) — see RESUME HERE.
// ★ Bounded: the curated list is ~20 birds; element sets are kept only for listed birds (plus none other); passes are
//   cached per bird for kPassCacheSec, ≤ 8 a bird.
#pragma once

#include "vibe_sat_orbit.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if __has_include("vibedsp/simd_internal.h")
#include "vibedsp/simd_internal.h"
#endif
#ifndef VIBE_NEON
#define VIBE_NEON 0
#endif
#ifndef VIBE_SSE
#define VIBE_SSE 0
#endif

namespace vibe { namespace sat {

// ── The curated list ──────────────────────────────────────────────────────────────────────────────────────────────
/** One downlink. `hiHz` > 0 = a passband (a linear transponder), else a single channel at `hz`. */
struct Transmitter {
    std::string label;
    double hz = 0, hiHz = 0;
    std::string mode;        // the demodulator the dial should be on: "nfm", "usb", "cw" — or a decoder's name
    std::string loop;        // "fll" | "none" | "costas" (brief §6.2)
    std::string consumer;    // "" = listening; "lrpt", "ax25-9600", "ax25-1200", "bpsk-telemetry" = a decoder's
    double baud = 0;
    double upHz = 0, ctcss = 0;
    std::string note;
    std::string status;      // sats.json: "active" | "inactive" (SatNOGS DB's word) | "" (nobody said)
    bool passband() const { return hiHz > hz; }
    double centre() const { return passband() ? 0.5 * (hz + hiHz) : hz; }
};
struct SatDef {
    std::string id, name;
    long long norad = 0;
    std::string group;       // CelesTrak GP group its elements come from
    std::string note;        // what the listener should know (schedule, sunlight only …)
    std::vector<Transmitter> tx;
};

/** ★★ THE DEFAULT SET — checked 2026-11-19 against AMSAT's live FM / linear lists (FM page; "Live Linear Satellites",
 *  updated 2026-02-21), CelesTrak's current amateur + weather groups (every NORAD number below has a current element
 *  set there) and SatNOGS. Left OUT on purpose: NOAA-15/18/19 (decommissioned 6 June, 13 and 19 August 2025 — NOAA's
 *  POES archive; their 137 MHz APT is off), AO-91 (end of mission), SO-125 (status disputed: SatNOGS says re-entered),
 *  TEVEL-2 (downlinks unconfirmed), CAS-4A / XW-2 (re-entered), QO-100 (geostationary, no Doppler to correct).
 *  ★ Meteor frequencies and modes CHANGE ON ORBIT (brief §6.2 TRAP) — these are today's; a vibesdr.net sats.json
 *  (phase 2) replaces this list. */
inline std::vector<SatDef> curatedCatalogue() {
    auto fm = [](const char* label, double hz, const char* note = "", double up = 0, double ctcss = 0) {
        Transmitter t; t.label = label; t.hz = hz; t.mode = "nfm"; t.loop = "fll"; t.upHz = up; t.ctcss = ctcss; t.note = note; return t; };
    auto lin = [](const char* label, double lo, double hi, double upLo = 0) {
        Transmitter t; t.label = label; t.hz = lo; t.hiHz = hi; t.mode = "usb"; t.loop = "none"; t.upHz = upLo; return t; };
    auto dig = [](const char* label, double hz, const char* mode, const char* consumer, double baud, const char* loop) {
        Transmitter t; t.label = label; t.hz = hz; t.mode = mode; t.consumer = consumer; t.baud = baud; t.loop = loop; return t; };
    auto cw = [](const char* label, double hz) {
        Transmitter t; t.label = label; t.hz = hz; t.mode = "cw"; t.loop = "none"; return t; };
    std::vector<SatDef> v;
    v.push_back({"ISS", "ISS", 25544, "amateur", "Crew contacts, events and the repeater when ARISS has it on",
                 {fm("FM repeater", 437800000, "Cross-band repeater, uplink 145.990 MHz (67 Hz)", 145990000, 67.0),
                  fm("Voice / SSTV", 145800000, "School contacts and SSTV events"),
                  dig("APRS", 145825000, "nfm", "ax25-1200", 1200, "fll")}});
    v.push_back({"SO-50", "SO-50 (SaudiSat-1C)", 27607, "amateur", "FM repeater; armed by a 74.4 Hz tone, 10 minute timer",
                 {fm("FM repeater", 436795000, "Uplink 145.850 MHz (67 Hz)", 145850000, 67.0)}});
    v.push_back({"AO-123", "AO-123 (ASRTU-1)", 61781, "amateur", "FM repeater",
                 {fm("FM repeater", 435400000, "Uplink 145.850 MHz (67 Hz)", 145850000, 67.0)}});
    v.push_back({"PO-101", "PO-101 (Diwata-2)", 43678, "amateur", "FM repeater on a schedule",
                 {fm("FM repeater", 145900000, "Uplink 437.500 MHz (141.3 Hz)", 437500000, 141.3)}});
    v.push_back({"RS-44", "RS-44", 44909, "amateur", "Linear transponder, inverting",
                 {lin("Transponder", 435610000, 435670000, 145935000), cw("CW beacon", 435605000)}});
    v.push_back({"FO-29", "FO-29 (JAS-2)", 24278, "amateur", "Linear transponder on a schedule (power permitting)",
                 {lin("Transponder", 435800000, 435900000, 145900000), cw("CW beacon", 435795000)}});
    v.push_back({"AO-7", "AO-7", 7530, "amateur", "Sunlight only; mode A or B by its own timer",
                 {lin("Mode B transponder", 145925000, 145975000, 432125000), lin("Mode A transponder", 29400000, 29500000, 145850000),
                  cw("Beacon", 29502000)}});
    v.push_back({"AO-73", "AO-73 (FUNcube-1)", 39444, "amateur", "Transponder in eclipse; telemetry in sunlight",
                 {lin("Transponder", 145950000, 145970000, 435130000), dig("Telemetry", 145935000, "usb", "bpsk-telemetry", 1200, "none")}});
    v.push_back({"JO-97", "JO-97 (JY1Sat)", 43803, "amateur", "Linear transponder",
                 {lin("Transponder", 145855000, 145875000, 435100000)}});
    v.push_back({"MO-122", "MO-122 (MESAT1)", 60209, "amateur", "Linear transponder",
                 {lin("Transponder", 435810000, 435840000, 145910000), dig("Telemetry", 435800000, "usb", "bpsk-telemetry", 1200, "none")}});
    v.push_back({"SONATE-2", "SONATE-2", 59112, "amateur", "Telemetry and APRS",
                 {dig("Telemetry", 437025000, "nfm", "ax25-9600", 9600, "none"), dig("APRS", 145825000, "nfm", "ax25-1200", 1200, "fll")}});
    v.push_back({"METEOR-M2-3", "Meteor-M2-3", 57166, "weather", "Weather images (LRPT); mode and frequency can change",
                 {dig("LRPT", 137900000, "lrpt", "lrpt", 72000, "costas")}});
    v.push_back({"METEOR-M2-4", "Meteor-M2-4", 59051, "weather", "Weather images (LRPT); mode and frequency can change",
                 {dig("LRPT", 137900000, "lrpt", "lrpt", 72000, "costas")}});
    return v;
}

/** What sats.json says about itself — the picker's credit line ("Orbits: CelesTrak · 5 h old"). */
struct SatsMeta {
    double generated = 0;     // the mirror built it (Unix)
    double orbitsAt = 0;      // the OLDEST group's CelesTrak fetch (Unix) — the data's age
    std::string orbitsCredit = "Orbits: CelesTrak";
    std::string txCredit;
    bool fromMirror = false;
};
inline double isoToUnix(const std::string& s) { double jd = 0; return parseIsoUtc(s, jd) ? unixFromJd(jd) : 0.0; }

/** ★★ sats.json (vibesdr.net's mirror, docs/v12/SATELLITE-ENGINE.md §6.2) → the list, the element sets, the credit.
 *  Strict: an unknown format, no satellites, a bad id / catalogue number / frequency / mode = refused (false) and the
 *  caller keeps what it has. A transmitter with an unknown mode or loop is skipped, not the whole bird.
 *  ★ Catalogue numbers are integers 1 … 999 999 999 (six digits are normal now) — never a 5-digit TLE field. */
inline bool parseSats(const std::string& text, std::vector<SatDef>& defs, std::vector<Elements>& els, std::vector<bool>& haveEl,
                      SatsMeta& meta, std::string& err) {
    json::Value doc;
    if (text.size() > 4u * 1024 * 1024 || !json::parseValue(text, doc) || doc.t != json::Value::Obj) { err = "not JSON"; return false; }
    if (doc.num("format") != 1) { err = "an unknown sats.json format"; return false; }
    const json::Value* sats = doc.get("satellites");
    if (!sats || sats->t != json::Value::Arr || sats->a.empty() || sats->a.size() > 200) { err = "no satellites"; return false; }
    defs.clear(); els.clear(); haveEl.clear();
    meta = SatsMeta{};
    meta.fromMirror = true;
    meta.generated = isoToUnix(doc.str("generated"));
    if (const json::Value* at = doc.get("attribution")) {
        if (!at->str("orbits").empty()) meta.orbitsCredit = at->str("orbits").substr(0, 120);
        meta.txCredit = at->str("transmitters").substr(0, 160);
    }
    if (const json::Value* og = doc.get("orbits"); og && og->t == json::Value::Obj)
        for (auto& kv : og->o) {
            const double t = isoToUnix(kv.second.str("fetched"));
            if (t > 0 && (meta.orbitsAt <= 0 || t < meta.orbitsAt)) meta.orbitsAt = t;
        }
    auto okId = [](const std::string& id) {
        if (id.empty() || id.size() > 32) return false;
        for (char c : id) if (!std::isalnum((unsigned char)c) && c != '-' && c != '_') return false;
        return true;
    };
    for (const auto& x : sats->a) {
        if (x.t != json::Value::Obj) { err = "a satellite is not an object"; return false; }
        SatDef d;
        d.id = x.str("id"); d.name = x.str("name").substr(0, 64); d.group = x.str("group").substr(0, 32); d.note = x.str("note").substr(0, 240);
        const double nd = x.num("norad");
        if (!okId(d.id) || !(nd >= 1 && nd <= 999999999.0) || nd != std::floor(nd)) { err = "a bad satellite id or catalogue number"; return false; }
        d.norad = (long long)nd;
        if (d.name.empty()) d.name = d.id;
        const json::Value* txs = x.get("transmitters");
        if (txs && txs->t == json::Value::Arr)
            for (const auto& t : txs->a) {
                if (t.t != json::Value::Obj || d.tx.size() >= 16) continue;
                Transmitter tr;
                tr.label = t.str("label").substr(0, 48); tr.hz = t.num("downlink"); tr.hiHz = t.num("downlinkHigh");
                tr.mode = t.str("mode"); tr.loop = t.str("loop"); tr.consumer = t.str("consumer"); tr.baud = t.num("baud");
                tr.upHz = t.num("uplink"); tr.ctcss = t.num("ctcss"); tr.note = t.str("note").substr(0, 160); tr.status = t.str("status");
                static const char* kModes[] = {"nfm", "usb", "lsb", "cw", "am", "lrpt"};
                bool modeOk = false; for (auto m : kModes) if (tr.mode == m) modeOk = true;
                const bool loopOk = tr.loop == "fll" || tr.loop == "none" || tr.loop == "costas";
                if (!modeOk || !loopOk || !(tr.hz >= 1e6 && tr.hz <= 1e10) || (tr.hiHz != 0 && !(tr.hiHz > tr.hz && tr.hiHz - tr.hz <= 2e6))) continue;
                if (tr.status != "active" && tr.status != "inactive") tr.status.clear();
                if (tr.consumer.size() > 24) tr.consumer.clear();
                if (tr.label.empty()) tr.label = tr.mode;
                d.tx.push_back(tr);
            }
        if (d.tx.empty()) continue;   // nothing to listen to: not offered
        Elements e; bool have = false;
        if (const json::Value* omm = x.get("omm"); omm && omm->t == json::Value::Obj) {
            std::string e2;
            have = fromOmm(omm->flat(), e, e2) && e.norad == d.norad;
        }
        defs.push_back(std::move(d)); els.push_back(e); haveEl.push_back(have);
    }
    if (defs.empty()) { err = "no satellite with a usable downlink"; return false; }
    return true;
}

/** The CelesTrak GP groups the list needs. */
inline std::vector<std::string> catalogueGroups() { return {"amateur", "weather"}; }

// ── Small helpers ─────────────────────────────────────────────────────────────────────────────────────────────────
inline std::string jsonEsc(const std::string& s) {
    std::string o; o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}
inline std::string fmtNum(double v, int dp) { char b[48]; std::snprintf(b, sizeof b, "%.*f", dp, std::isfinite(v) ? v : 0.0); return b; }

/** Element-set age → what the brief's §6.3 table says to do. */
enum class Age { Fresh, Stale, Old, Expired, None };
inline Age ageOf(double days) {
    if (!(days >= -2)) return Age::None;     // NaN, or an epoch far in the future = a wrong clock or a bad set
    if (days < 3) return Age::Fresh;
    if (days < 7) return Age::Stale;
    if (days < 14) return Age::Old;
    return Age::Expired;
}
inline const char* ageName(Age a) {
    switch (a) { case Age::Fresh: return "fresh"; case Age::Stale: return "stale"; case Age::Old: return "old";
                 case Age::Expired: return "expired"; default: return "none"; }
}

// ── The closed loop's measurement (SIMD) ──────────────────────────────────────────────────────────────────────────
/** Σ x[i]·conj(x[i−1]), Σ|x[i]|² and Σ|x[i]|⁴ over a block of interleaved float IQ, x[−1] = `prev` (updated to the
 *  last sample). The angle of the first sum over a window is the power-weighted mean frequency of the channel — a
 *  carrier offset estimator with no arctangent per sample; the second and fourth moments give the window's SNR (M2M4,
 *  below). ★ NEON / SSE2 / scalar give the SAME sums: four lanes in the same order (lane k takes i ≡ k+1 mod 4 from
 *  i = 1), the tail and the first product sequentially, lanes added ((0+1)+(2+3)) into double; the scalar products go
 *  through sumOfProducts() so no compiler fuses them into an FMA the vector lanes do not use. */
struct LagSums { double re = 0, im = 0, pw = 0, p4 = 0; long long n = 0; };
inline float sumOfProducts(float a, float b, float c, float d) { volatile float x = a * b, y = c * d; return x + y; }
inline float diffOfProducts(float a, float b, float c, float d) { volatile float x = a * b, y = c * d; return x - y; }
inline float squared(float a) { volatile float x = a * a; return x; }
inline void lagAccumulate(const float* iq, int n, float prev[2], LagSums& acc) {
    if (n <= 0) return;
    float hr = sumOfProducts(iq[0], prev[0], iq[1], prev[1]);
    float hi = diffOfProducts(iq[1], prev[0], iq[0], prev[1]);
    float hp = sumOfProducts(iq[0], iq[0], iq[1], iq[1]);
    float h4 = squared(hp);
    float lr[4] = {0, 0, 0, 0}, li[4] = {0, 0, 0, 0}, lp[4] = {0, 0, 0, 0}, l4[4] = {0, 0, 0, 0};
    int i = 1;
    const int full = 1 + ((n - 1) / 4) * 4;
#if VIBE_NEON
    float32x4_t ar = vdupq_n_f32(0), ai = vdupq_n_f32(0), ap = vdupq_n_f32(0), a4 = vdupq_n_f32(0);
    for (; i < full; i += 4) {
        const float32x4x2_t c = vld2q_f32(iq + 2 * i), p = vld2q_f32(iq + 2 * (i - 1));
        ar = vaddq_f32(ar, vaddq_f32(vmulq_f32(c.val[0], p.val[0]), vmulq_f32(c.val[1], p.val[1])));
        ai = vaddq_f32(ai, vsubq_f32(vmulq_f32(c.val[1], p.val[0]), vmulq_f32(c.val[0], p.val[1])));
        const float32x4_t q = vaddq_f32(vmulq_f32(c.val[0], c.val[0]), vmulq_f32(c.val[1], c.val[1]));
        ap = vaddq_f32(ap, q);
        a4 = vaddq_f32(a4, vmulq_f32(q, q));
    }
    vst1q_f32(lr, ar); vst1q_f32(li, ai); vst1q_f32(lp, ap); vst1q_f32(l4, a4);
#elif VIBE_SSE
    __m128 ar = _mm_setzero_ps(), ai = _mm_setzero_ps(), ap = _mm_setzero_ps(), a4 = _mm_setzero_ps();
    for (; i < full; i += 4) {
        const __m128 c0 = _mm_loadu_ps(iq + 2 * i), c1 = _mm_loadu_ps(iq + 2 * i + 4);
        const __m128 p0 = _mm_loadu_ps(iq + 2 * (i - 1)), p1 = _mm_loadu_ps(iq + 2 * (i - 1) + 4);
        const __m128 cr = _mm_shuffle_ps(c0, c1, _MM_SHUFFLE(2, 0, 2, 0)), ci = _mm_shuffle_ps(c0, c1, _MM_SHUFFLE(3, 1, 3, 1));
        const __m128 pr = _mm_shuffle_ps(p0, p1, _MM_SHUFFLE(2, 0, 2, 0)), pi = _mm_shuffle_ps(p0, p1, _MM_SHUFFLE(3, 1, 3, 1));
        ar = _mm_add_ps(ar, _mm_add_ps(_mm_mul_ps(cr, pr), _mm_mul_ps(ci, pi)));
        ai = _mm_add_ps(ai, _mm_sub_ps(_mm_mul_ps(ci, pr), _mm_mul_ps(cr, pi)));
        const __m128 q = _mm_add_ps(_mm_mul_ps(cr, cr), _mm_mul_ps(ci, ci));
        ap = _mm_add_ps(ap, q);
        a4 = _mm_add_ps(a4, _mm_mul_ps(q, q));
    }
    _mm_storeu_ps(lr, ar); _mm_storeu_ps(li, ai); _mm_storeu_ps(lp, ap); _mm_storeu_ps(l4, a4);
#else
    for (; i < full; i += 4)
        for (int k = 0; k < 4; ++k) {
            const float* c = iq + 2 * (i + k); const float* p = c - 2;
            lr[k] = lr[k] + sumOfProducts(c[0], p[0], c[1], p[1]);
            li[k] = li[k] + diffOfProducts(c[1], p[0], c[0], p[1]);
            const float q = sumOfProducts(c[0], c[0], c[1], c[1]);
            lp[k] = lp[k] + q;
            l4[k] = l4[k] + squared(q);
        }
#endif
    for (; i < n; ++i) {
        const float* c = iq + 2 * i; const float* p = c - 2;
        hr = hr + sumOfProducts(c[0], p[0], c[1], p[1]);
        hi = hi + diffOfProducts(c[1], p[0], c[0], p[1]);
        const float q = sumOfProducts(c[0], c[0], c[1], c[1]);
        hp = hp + q;
        h4 = h4 + squared(q);
    }
    acc.re += (double)((lr[0] + lr[1]) + (lr[2] + lr[3])) + (double)hr;
    acc.im += (double)((li[0] + li[1]) + (li[2] + li[3])) + (double)hi;
    acc.pw += (double)((lp[0] + lp[1]) + (lp[2] + lp[3])) + (double)hp;
    acc.p4 += (double)((l4[0] + l4[1]) + (l4[2] + l4[3])) + (double)h4;
    acc.n += n;
    prev[0] = iq[2 * (n - 1)]; prev[1] = iq[2 * (n - 1) + 1];
}

/** The FLL's front end: windows of ~0.4 s → a residual (Hz) when the signal is clearly up, else nothing (COAST).
 *  ★ "Update only when squelch open AND SNR above threshold" (brief §4.2). The gate is the window's own SNR by the
 *    M2M4 moments estimator — for a constant-envelope signal (FM) in Gaussian noise, S = √(2·M2² − M4), N = M2 − S —
 *    so it needs no noise reference, no learning period and no squelch setting: noise alone gives S ≈ 0. A window
 *    under kGateDb never reaches the loop (its lag-sum angle is meaningless). */
class ResidualMeter {
public:
    static constexpr double kWindowSec = 0.4, kGateDb = 3.0;
    /** Feed channel IQ (the dial at DC). Returns true with `hz` when a window closed with the gate open. */
    bool feed(const float* iq, int n, double rateHz, double& hz) {
        if (rateHz <= 0 || n <= 0) return false;
        if (rateHz != rate_) { rate_ = rateHz; acc_ = LagSums{}; prev_[0] = prev_[1] = 0; }
        lagAccumulate(iq, n, prev_, acc_);
        if ((double)acc_.n < kWindowSec * rateHz) return false;
        const double cnt = (double)std::max<long long>(1, acc_.n);
        const double m2 = acc_.pw / cnt, m4 = acc_.p4 / cnt, re = acc_.re, im = acc_.im;
        acc_ = LagSums{};
        if (!(m2 > 0)) { lastSnrDb_ = -99; return false; }
        const double s2 = 2.0 * m2 * m2 - m4;
        const double S = s2 > 0 ? std::sqrt(s2) : 0.0, N = std::max(m2 - S, m2 * 1e-6);
        lastSnrDb_ = S > 0 ? 10.0 * std::log10(S / N) : -99.0;
        if (lastSnrDb_ < kGateDb) return false;
        hz = std::atan2(im, re) * rate_ / kTwoPi;
        return std::isfinite(hz);
    }
    double lastSnrDb() const { return lastSnrDb_; }
    void reset() { acc_ = LagSums{}; rate_ = 0; }
private:
    LagSums acc_; double rate_ = 0, lastSnrDb_ = 0; float prev_[2] = {0, 0};
};

// ── The engine ────────────────────────────────────────────────────────────────────────────────────────────────────
enum class LockState { Off, Armed, Track, Coast, Ended };
inline const char* lockName(LockState s) {
    switch (s) { case LockState::Armed: return "ARMED"; case LockState::Track: return "TRACK"; case LockState::Coast: return "COAST";
                 case LockState::Ended: return "ENDED"; default: return "OFF"; }
}

/** The clock the engine reads. ★ The shim passes vibeUtcNow() and clockMeasured() (brief §5: the same corrected time
 *  FT8 uses, never the raw system clock). */
struct Clock { double unix = 0; bool measured = false; };

/** What a lock is doing right now — what the caller applies (shiftHz) and what the clients draw. */
struct Track {
    LockState state = LockState::Off;
    std::string why;               // a refusal or an ending, in words
    std::string satId, satName, txLabel, mode, loop, consumer;
    long long norad = 0;
    double nominalHz = 0;          // the dial
    double shiftHz = 0;            // applied: model + trim (0 unless TRACK/COAST)
    double modelHz = 0, trimHz = 0, rateHzS = 0;
    double azDeg = 0, elDeg = -90, rangeKm = 0;
    double aos = 0, los = 0, maxElDeg = 0;   // the pass being tracked / armed for
    double ageDays = -1;
    std::string ageState = "none";
    std::string time = "ok";       // "ok" | "system" (clock unmeasured but plausible) | "none"
    bool approaching() const { return shiftHz > 0; }   // ▲ approaching = received high, ▼ receding
};

class Engine {
public:
    static constexpr double kMatchHz = 12000.0;      // brief §8: a downlink within ±12 kHz of the dial
    static constexpr double kTrimClampHz = 2500.0;   // brief §4.2: ±2–3 kHz around the model
    static constexpr double kTrimSlewHzS = 100.0;    // the applied trim moves at most this fast: 4 Hz a 25 Hz tick
    static constexpr double kEndTuneHz = 15000.0;    // brief §9: a tune further than this ends the lock
    static constexpr double kPassCacheSec = 300.0;
    static constexpr double kLosHoldSec = 3.0;       // below the horizon this long = LOS (no flicker at the edge)
    static constexpr double kOrphanArmSec = 1800.0;  // brief §9 TRAP: an unattended arm more than 30 min from AOS goes

    Engine() : defs_(curatedCatalogue()) { sats_.resize(defs_.size()); }

    // ── inputs ──
    void setObserver(double lat, double lon, double altM = 0) {
        std::lock_guard<std::mutex> lk(m_);
        obs_.set(lat, lon, altM);
        for (auto& s : sats_) s.passesAt = 0;
    }
    bool haveObserver() const { std::lock_guard<std::mutex> lk(m_); return obs_.valid; }
    /** ★★ sats.json (vibesdr.net's mirror) → the LIST and the element sets, replacing the built-in list. Returns how
     *  many listed birds have elements, -1 = refused (the current list and elements stay). A bird kept from the old
     *  list keeps its elements if the new file has none or older ones; a running lock follows its bird and transmitter
     *  (by catalogue number and label) or ends when the new list no longer has them. */
    int loadSats(const std::string& text, std::string* errOut = nullptr) {
        std::vector<SatDef> defs; std::vector<Elements> els; std::vector<bool> have; SatsMeta meta; std::string err;
        if (!parseSats(text, defs, els, have, meta, err)) { if (errOut) *errOut = err; return -1; }
        std::lock_guard<std::mutex> lk(m_);
        std::vector<SatRow> rows(defs.size());
        for (size_t i = 0; i < defs.size(); ++i) {
            const SatRow* old = nullptr;
            for (size_t k = 0; k < defs_.size(); ++k) if (defs_[k].norad == defs[i].norad && sats_[k].have) old = &sats_[k];
            if (have[i]) {
                Sgp4 sg;
                if (sg.init(els[i]) && !(old && old->el.epochJd > els[i].epochJd)) { rows[i].have = true; rows[i].el = els[i]; rows[i].sgp = sg; }
            }
            if (!rows[i].have && old) { rows[i].have = true; rows[i].el = old->el; rows[i].sgp = old->sgp; }
        }
        for (auto it = locks_.begin(); it != locks_.end();) {
            Lock& L = it->second;
            const long long nd = defs_[L.s].norad; const std::string lab = defs_[L.s].tx[L.t].label;
            int ns = -1, nt = -1;
            for (size_t i = 0; i < defs.size() && ns < 0; ++i)
                if (defs[i].norad == nd)
                    for (size_t k = 0; k < defs[i].tx.size(); ++k) if (defs[i].tx[k].label == lab) { ns = (int)i; nt = (int)k; break; }
            if (ns < 0 || !rows[ns].have) { it = locks_.erase(it); continue; }
            L.s = ns; L.t = nt; ++it;
        }
        defs_ = std::move(defs); sats_ = std::move(rows); meta_ = meta;
        int n = 0; for (auto& r : sats_) if (r.have) ++n;
        return n;
    }
    SatsMeta meta() const { std::lock_guard<std::mutex> lk(m_); return meta_; }
    /** CelesTrak OMM JSON (one group) → element sets for the listed birds. Returns how many listed birds it updated;
     *  a newer epoch replaces an older one, an older never replaces a newer. */
    int loadOmm(const std::string& jsonText) {
        std::vector<json::Object> objs;
        if (!json::parseArray(jsonText, objs)) return -1;
        std::lock_guard<std::mutex> lk(m_);
        int n = 0;
        for (auto& o : objs) {
            double id = 0;
            if (!json::num(o, "NORAD_CAT_ID", id)) continue;
            for (size_t i = 0; i < defs_.size(); ++i) {
                if (defs_[i].norad != (long long)id) continue;
                Elements e; std::string err;
                if (!fromOmm(o, e, err)) continue;
                if (sats_[i].have && sats_[i].el.epochJd >= e.epochJd) continue;
                Sgp4 s;
                if (!s.init(e)) continue;
                sats_[i].have = true; sats_[i].el = e; sats_[i].sgp = s; sats_[i].passesAt = 0;
                ++n;
            }
        }
        return n;
    }
    /** How many listed birds have elements, and the newest epoch (Unix). */
    int elementCount(double* newestEpoch = nullptr) const {
        std::lock_guard<std::mutex> lk(m_);
        int n = 0; double best = 0;
        for (auto& s : sats_) if (s.have) { ++n; best = std::max(best, s.el.epochUnix()); }
        if (newestEpoch) *newestEpoch = best;
        return n;
    }
    /** A copy: sats.json can replace the list at any time (loadSats). */
    std::vector<SatDef> defs() const { std::lock_guard<std::mutex> lk(m_); return defs_; }

    /** ★ TIME (brief §5 TRAPs): measured by SNTP = "ok"; unmeasured but within a plausible window of the element sets
     *  (a desktop whose own NTP is fine but whose SNTP to us is blocked) = "system", tracked with the word shown; a
     *  clock before the newest epoch − 1 day or more than 60 days after it (a Pi with no RTC booted offline) = "none":
     *  NO TIME, nothing tracks. */
    std::string timeState(const Clock& c) const {
        if (c.measured) return "ok";
        double newest = 0; elementCount(&newest);
        if (newest <= 0) return "none";
        if (c.unix < newest - 86400.0 || c.unix > newest + 60 * 86400.0) return "none";
        return "system";
    }

    /** "Can this radio receive [lo, hi] Hz?" — the radio's tuning ranges (and a locked window), from the caller. */
    using ReachFn = std::function<bool(double lo, double hi)>;
    static ReachFn rangeReach(double lo, double hi) { return [lo, hi](double a, double b) { return a >= lo && b <= hi; }; }
    /** Is any listed downlink receivable? — the reach gate (no dead controls). */
    bool reaches(const ReachFn& r) const {
        std::lock_guard<std::mutex> lk(m_);
        for (auto& d : defs_) for (auto& t : d.tx)
            if (r(t.hz, t.passband() ? t.hiHz : t.hz)) return true;
        return false;
    }
    bool reaches(double lo, double hi) const { return reaches(rangeReach(lo, hi)); }

    // ── the lock ──
    /** Press Doppler at `dialHz` (brief §8). Matches the listed downlinks within ±12 kHz; one up → TRACK; several →
     *  the highest; none up but one will rise → ARMED for its AOS; else refused (Track.state Off, why set).
     *  ★ `wantNorad` > 0: the listener chose a bird in the picker — only that bird's downlinks match. */
    Track start(const std::string& key, const std::string& owner, double dialHz, const std::string& mode, const Clock& clk,
                long long wantNorad = 0) {
        std::lock_guard<std::mutex> lk(m_);
        Track out;
        out.time = timeStateLocked(clk);
        if (!obs_.valid) { out.why = "This server has no location set, so it cannot work out where satellites are"; return out; }
        if (out.time == "none") { out.why = "NO TIME — this server's clock has not been checked yet"; return out; }
        struct Cand { int s, t; double el, aos; Pass pass; };
        std::vector<Cand> cands;
        bool anyElements = false, anyExpired = false;
        for (size_t i = 0; i < defs_.size(); ++i) {
            if (wantNorad > 0 && defs_[i].norad != wantNorad) continue;
            for (size_t k = 0; k < defs_[i].tx.size(); ++k) {
                const Transmitter& t = defs_[i].tx[k];
                const bool near = t.passband() ? (dialHz >= t.hz - kMatchHz && dialHz <= t.hiHz + kMatchHz)
                                               : std::fabs(dialHz - t.hz) <= kMatchHz;
                if (!near) continue;
                SatRow& r = sats_[i];
                if (!r.have) continue;
                anyElements = true;
                const double age = (clk.unix - r.el.epochUnix()) / 86400.0;
                if (ageOf(age) == Age::Expired || ageOf(age) == Age::None) { anyExpired = true; continue; }
                Look L = look(r.sgp, obs_, clk.unix);
                Cand c{(int)i, (int)k, L.ok ? L.elDeg : -90, 0, Pass{}};
                auto ps = passesLocked(i, clk.unix);
                for (auto& p : ps) if (p.los > clk.unix) { c.pass = p; c.aos = p.aos; break; }
                cands.push_back(c);
            }
        }
        if (cands.empty()) {
            out.why = anyExpired ? "The satellite's orbit data is too old to track (over 14 days)"
                    : anyElements || wantNorad > 0 ? "No listed satellite transmits within 12 kHz of this frequency"
                    : "NO SAT — no listed satellite downlink near this frequency";
            if (!anyElements && wantNorad <= 0) {
                bool listed = false;
                for (auto& d : defs_) for (auto& t : d.tx)
                    if (t.passband() ? (dialHz >= t.hz - kMatchHz && dialHz <= t.hiHz + kMatchHz) : std::fabs(dialHz - t.hz) <= kMatchHz) listed = true;
                if (listed) out.why = "NO SAT DATA — the orbit data has not been downloaded yet";
            }
            return out;
        }
        // highest elevation first; among those below the horizon, the soonest AOS
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
            const bool ua = a.el >= 0, ub = b.el >= 0;
            if (ua != ub) return ua;
            if (ua) return a.el > b.el;
            const double xa = a.aos > 0 ? a.aos : 1e300, xb = b.aos > 0 ? b.aos : 1e300;
            return xa < xb;
        });
        const Cand& c = cands.front();
        if (c.el < 0 && c.aos <= 0) { out.why = "That satellite does not rise here in the next day"; return out; }
        Lock L;
        L.s = c.s; L.t = c.t; L.owner = owner; L.mode = mode; L.pass = c.pass; L.startedAt = clk.unix;
        L.lastMeasure = clk.unix;   // ★ TRACK, not COAST, for the first moment: nothing has been heard yet either way
        const Transmitter& t = defs_[c.s].tx[c.t];
        L.nominal = t.hz;
        if (t.passband()) {
            // ★ A linear transponder: the dial the listener is on stays where the signal is — the nominal is the
            //   satellite-frame frequency that puts the channel exactly there now (no jump on engagement).
            double nom = dialHz;
            if (c.el >= 0) { Look lk2 = look(sats_[c.s].sgp, obs_, clk.unix); if (lk2.ok) nom = dialHz - lk2.dopplerHz(dialHz); }
            L.nominal = std::max(t.hz, std::min(t.hiHz, nom));
        }
        L.state = c.el >= 0 ? LockState::Track : LockState::Armed;
        locks_[key] = L;
        return trackLocked(key, clk);
    }
    /** End it (the button again, a deliberate tune, LOS). The caller freezes the channel at nominal + shift. */
    void stop(const std::string& key) { std::lock_guard<std::mutex> lk(m_); locks_.erase(key); }
    bool active(const std::string& key) const { std::lock_guard<std::mutex> lk(m_); return locks_.count(key) != 0; }
    std::vector<std::string> keys() const {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<std::string> v; for (auto& kv : locks_) v.push_back(kv.first); return v;
    }
    /** ★ A small tune while locked (brief §9: "small fine-tune nudges adjust trim rather than cancel") — the listener
     *  moved the dial by less than 15 kHz: the nominal follows their dial, the lock goes on. Returns false when the
     *  move is a deliberate tune away (the caller ends the lock). */
    bool nudge(const std::string& key, double newDialHz) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = locks_.find(key);
        if (it == locks_.end()) return false;
        if (std::fabs(newDialHz - it->second.nominal) > kEndTuneHz) return false;
        it->second.nominal = newDialHz;
        return true;
    }
    /** The closed loop's input: a residual (Hz, signal above channel centre = positive) measured with the gate open. */
    void noteResidual(const std::string& key, double hz, double now) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = locks_.find(key);
        if (it == locks_.end()) return;
        Lock& L = it->second;
        if (defs_[L.s].tx[L.t].loop != "fll" || (L.state != LockState::Track && L.state != LockState::Coast)) return;
        if (!std::isfinite(hz) || std::fabs(hz) > kTrimClampHz * 2) return;   // ★ a far-off figure is another carrier
        // ★ type-2 on 0.4 s windows. The target is set from the trim APPLIED NOW plus a share of the error — never by
        //   accumulating errors the slew has not caught up with yet (that winds up and rings: the first version did,
        //   ±500 Hz around the answer). The rate term follows a residual ramp (a clock error grows with the Doppler
        //   rate near TCA) with no steady-state lag.
        L.trimTarget = std::max(-kTrimClampHz, std::min(kTrimClampHz, L.trim + 0.5 * hz));
        L.trimRate = std::max(-50.0, std::min(50.0, L.trimRate + 0.08 * hz));
        L.lastMeasure = now;
    }
    /** This tick's state for `key` — what to apply and what to show. ★ Call at the tracker's rate (25 Hz); it advances
     *  the slew-limited trim, the ARMED → TRACK step at AOS and LOS. */
    Track tick(const std::string& key, const Clock& clk) {
        std::lock_guard<std::mutex> lk(m_);
        return trackLocked(key, clk);
    }
    /** ★ HOOK FOR DECODERS: the bird `key`'s lock is on (or nothing), without advancing anything. */
    Track selected(const std::string& key) const {
        std::lock_guard<std::mutex> lk(m_);
        auto it = locks_.find(key);
        return it == locks_.end() ? Track{} : it->second.last;
    }
    /** ★ HOOK FOR DECODERS: Doppler (Hz, received − transmitted) and its rate (Hz/s) for `norad` at `hz` and time `t`,
     *  from the model alone. false when the bird is not listed / has no elements / no location. */
    bool dopplerFor(long long norad, double hz, double t, double& dopplerHz, double& rateHzS, double* elDeg = nullptr) {
        std::lock_guard<std::mutex> lk(m_);
        if (!obs_.valid) return false;
        for (size_t i = 0; i < defs_.size(); ++i) {
            if (defs_[i].norad != norad || !sats_[i].have) continue;
            Look a = look(sats_[i].sgp, obs_, t), b = look(sats_[i].sgp, obs_, t + 0.5);
            if (!a.ok || !b.ok) return false;
            dopplerHz = a.dopplerHz(hz);
            rateHzS = (b.dopplerHz(hz) - dopplerHz) / 0.5;
            if (elDeg) *elDeg = a.elDeg;
            return true;
        }
        return false;
    }
    /** ★ The brief's orphan rule: with nobody connected, an ARMED lock more than 30 minutes from its AOS goes. Returns
     *  the keys removed. Active locks run to LOS unattended. */
    std::vector<std::string> dropOrphanArms(const Clock& clk) {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<std::string> gone;
        for (auto it = locks_.begin(); it != locks_.end();) {
            if (it->second.state == LockState::Armed && it->second.pass.aos - clk.unix > kOrphanArmSec) { gone.push_back(it->first); it = locks_.erase(it); }
            else ++it;
        }
        return gone;
    }

    /** The picker's list: every listed bird, its elements' age, where it is now and its next passes; `lo`/`hi` = the
     *  radio's tuning range (a downlink outside it is marked "reach":false — the client does not offer it).
     *  ★ Bounded: ≤ 4 passes a bird in the next 24 h. */
    std::string listJson(const Clock& clk, double lo, double hi) { return listJson(clk, rangeReach(lo, hi)); }
    std::string listJson(const Clock& clk, const ReachFn& reachFn) {
        std::lock_guard<std::mutex> lk(m_);
        std::string j = "{\"type\":\"sat_list\",\"time\":\"" + timeStateLocked(clk) + "\",\"located\":" + (obs_.valid ? "true" : "false");
        j += ",\"now\":" + fmtNum(clk.unix, 1);
        {
            // ★ the credit (CelesTrak's data, SatNOGS's where used) and the data's age — the picker shows both
            double newest = 0; for (auto& r : sats_) if (r.have) newest = std::max(newest, r.el.epochUnix());
            j += ",\"source\":{\"orbits\":\"" + jsonEsc(meta_.orbitsCredit) + "\"";
            if (!meta_.txCredit.empty()) j += ",\"transmitters\":\"" + jsonEsc(meta_.txCredit) + "\"";
            j += ",\"via\":\"" + std::string(meta_.fromMirror ? "vibesdr.net" : "built-in") + "\"";
            if (meta_.orbitsAt > 0) j += ",\"fetched\":" + fmtNum(meta_.orbitsAt, 0);
            if (newest > 0) j += ",\"newestEpoch\":" + fmtNum(newest, 0);
            j += "}";
        }
        j += ",\"sats\":[";
        bool first = true;
        for (size_t i = 0; i < defs_.size(); ++i) {
            const SatDef& d = defs_[i];
            SatRow& r = sats_[i];
            if (!first) j += ",";
            first = false;
            j += "{\"id\":\"" + jsonEsc(d.id) + "\",\"name\":\"" + jsonEsc(d.name) + "\",\"norad\":" + std::to_string(d.norad);
            j += ",\"note\":\"" + jsonEsc(d.note) + "\"";
            const double age = r.have ? (clk.unix - r.el.epochUnix()) / 86400.0 : -1;
            j += ",\"age\":" + fmtNum(age, 2) + ",\"ageState\":\"" + std::string(r.have ? ageName(ageOf(age)) : "none") + "\"";
            j += ",\"tx\":[";
            for (size_t k = 0; k < d.tx.size(); ++k) {
                const Transmitter& t = d.tx[k];
                if (k) j += ",";
                const bool reach = reachFn(t.hz, t.passband() ? t.hiHz : t.hz);
                j += "{\"label\":\"" + jsonEsc(t.label) + "\",\"hz\":" + fmtNum(t.hz, 0);
                if (t.passband()) j += ",\"hiHz\":" + fmtNum(t.hiHz, 0);
                j += ",\"mode\":\"" + t.mode + "\",\"loop\":\"" + t.loop + "\"";
                if (!t.consumer.empty()) j += ",\"consumer\":\"" + t.consumer + "\"";
                if (t.upHz > 0) j += ",\"upHz\":" + fmtNum(t.upHz, 0);
                if (t.ctcss > 0) j += ",\"ctcss\":" + fmtNum(t.ctcss, 1);
                if (!t.note.empty()) j += ",\"note\":\"" + jsonEsc(t.note) + "\"";
                if (!t.status.empty()) j += ",\"status\":\"" + t.status + "\"";
                j += std::string(",\"reach\":") + (reach ? "true" : "false") + "}";
            }
            j += "]";
            if (r.have && obs_.valid) {
                Look L = look(r.sgp, obs_, clk.unix);
                if (L.ok) j += ",\"az\":" + fmtNum(L.azDeg, 1) + ",\"el\":" + fmtNum(L.elDeg, 1) + ",\"rangeKm\":" + fmtNum(L.rangeKm, 0)
                            + ",\"lat\":" + fmtNum(L.subLatDeg, 2) + ",\"lon\":" + fmtNum(L.subLonDeg, 2);
                auto ps = passesLocked(i, clk.unix);
                j += ",\"passes\":[";
                int np = 0;
                for (auto& p : ps) {
                    if (p.los < clk.unix || np >= 4) continue;
                    if (np++) j += ",";
                    j += "{\"aos\":" + fmtNum(p.aos, 0) + ",\"tca\":" + fmtNum(p.tca, 0) + ",\"los\":" + fmtNum(p.los, 0)
                       + ",\"maxEl\":" + fmtNum(p.maxElDeg, 1) + ",\"aosAz\":" + fmtNum(p.aosAzDeg, 0) + ",\"losAz\":" + fmtNum(p.losAzDeg, 0) + "}";
                }
                j += "]";
            }
            j += "}";
        }
        j += "]}";
        return j;
    }

    /** The sky track for one pass of `norad` (the picker's polar plot): az/el every `stepSec` from AOS to LOS, ≤ 120
     *  points. Empty when unknown. */
    std::string trackJson(long long norad, double aos, double los, double stepSec = 15) {
        std::lock_guard<std::mutex> lk(m_);
        std::string j = "[";
        if (!obs_.valid || los <= aos) return "[]";
        stepSec = std::max(stepSec, (los - aos) / 120.0);
        for (size_t i = 0; i < defs_.size(); ++i) {
            if (defs_[i].norad != norad || !sats_[i].have) continue;
            bool first = true;
            for (double t = aos; t <= los + 0.01; t += stepSec) {
                Look L = look(sats_[i].sgp, obs_, std::min(t, los));
                if (!L.ok) continue;
                if (!first) j += ",";
                first = false;
                j += "[" + fmtNum(L.azDeg, 1) + "," + fmtNum(std::max(0.0, L.elDeg), 1) + "]";
            }
        }
        return j + "]";
    }

    /** The per-listener readout (`{"type":"sat", …}`), from a Track. */
    static std::string trackJsonMsg(const Track& t) {
        std::string j = "{\"type\":\"sat\",\"state\":\"" + std::string(lockName(t.state)) + "\"";
        if (!t.why.empty()) j += ",\"why\":\"" + jsonEsc(t.why) + "\"";
        if (t.state != LockState::Off) {
            j += ",\"id\":\"" + jsonEsc(t.satId) + "\",\"name\":\"" + jsonEsc(t.satName) + "\",\"norad\":" + std::to_string(t.norad);
            j += ",\"tx\":\"" + jsonEsc(t.txLabel) + "\",\"loop\":\"" + t.loop + "\"";
            if (!t.consumer.empty()) j += ",\"consumer\":\"" + t.consumer + "\"";
            j += ",\"nominal\":" + fmtNum(t.nominalHz, 0) + ",\"offset\":" + fmtNum(t.shiftHz, 1) + ",\"rate\":" + fmtNum(t.rateHzS, 1);
            j += ",\"trim\":" + fmtNum(t.trimHz, 1) + ",\"az\":" + fmtNum(t.azDeg, 1) + ",\"el\":" + fmtNum(t.elDeg, 1);
            j += ",\"aos\":" + fmtNum(t.aos, 0) + ",\"los\":" + fmtNum(t.los, 0) + ",\"maxEl\":" + fmtNum(t.maxElDeg, 1);
            j += ",\"age\":" + fmtNum(t.ageDays, 2) + ",\"ageState\":\"" + t.ageState + "\"";
        }
        j += ",\"time\":\"" + t.time + "\"}";
        return j;
    }

private:
    struct SatRow {
        bool have = false; Elements el; Sgp4 sgp;
        std::vector<Pass> passes; double passesAt = 0;
    };
    struct Lock {
        int s = 0, t = 0;
        std::string owner, mode;
        double nominal = 0, startedAt = 0;
        LockState state = LockState::Track;
        Pass pass;
        double trimTarget = 0, trim = 0, trimRate = 0, lastMeasure = 0, lastTick = 0, belowSince = 0;
        Track last;
    };

    std::string timeStateLocked(const Clock& c) const {
        if (c.measured) return "ok";
        double newest = 0;
        for (auto& s : sats_) if (s.have) newest = std::max(newest, s.el.epochUnix());
        if (newest <= 0) return "none";
        if (c.unix < newest - 86400.0 || c.unix > newest + 60 * 86400.0) return "none";
        return "system";
    }

    const std::vector<Pass>& passesLocked(size_t i, double now) {
        SatRow& r = sats_[i];
        if (r.passesAt <= 0 || std::fabs(now - r.passesAt) > kPassCacheSec || (!r.passes.empty() && r.passes.front().los < now - 1)) {
            r.passes = r.have && obs_.valid ? predictPasses(r.sgp, obs_, now - 900, 86400 + 900, 0.0, 8) : std::vector<Pass>{};
            // ★ the search started 15 min back so a pass already in progress has its true AOS
            r.passes.erase(std::remove_if(r.passes.begin(), r.passes.end(), [&](const Pass& p) { return p.los < now; }), r.passes.end());
            r.passesAt = now;
        }
        return r.passes;
    }

    Track trackLocked(const std::string& key, const Clock& clk) {
        Track out;
        out.time = timeStateLocked(clk);
        auto it = locks_.find(key);
        if (it == locks_.end()) return out;
        Lock& L = it->second;
        const SatDef& d = defs_[L.s];
        const Transmitter& tx = d.tx[L.t];
        SatRow& r = sats_[L.s];
        out.satId = d.id; out.satName = d.name; out.norad = d.norad; out.txLabel = tx.label;
        out.mode = L.mode; out.loop = tx.loop; out.consumer = tx.consumer; out.nominalHz = L.nominal;
        out.ageDays = (clk.unix - r.el.epochUnix()) / 86400.0;
        out.ageState = ageName(ageOf(out.ageDays));
        const double dt = L.lastTick > 0 ? std::max(0.0, std::min(1.0, clk.unix - L.lastTick)) : 0.0;
        L.lastTick = clk.unix;
        Look now = look(r.sgp, obs_, clk.unix);
        out.azDeg = now.azDeg; out.elDeg = now.elDeg; out.rangeKm = now.rangeKm;
        // the pass: refresh once it has ended (an ARMED lock waits for the next one)
        if (L.pass.los < clk.unix) {
            auto ps = passesLocked(L.s, clk.unix);
            for (auto& p : ps) if (p.los > clk.unix) { L.pass = p; break; }
        }
        out.aos = L.pass.aos; out.los = L.pass.los; out.maxElDeg = L.pass.maxElDeg;
        if (L.state == LockState::Armed) {
            if (now.ok && now.elDeg >= 0) { L.state = LockState::Track; L.trim = L.trimTarget = L.trimRate = 0; L.lastMeasure = clk.unix; }
        } else if (L.state == LockState::Track || L.state == LockState::Coast) {
            if (!now.ok || now.elDeg < 0.0) {
                if (L.belowSince <= 0) L.belowSince = clk.unix;
                if (clk.unix - L.belowSince >= kLosHoldSec) { L.state = LockState::Ended; }
            } else L.belowSince = 0;
        }
        if (L.state == LockState::Track || L.state == LockState::Coast || L.state == LockState::Ended) {
            // model at the actual listening frequency (nominal); the loop's trim, slew-limited
            const double m = now.ok ? now.dopplerHz(L.nominal) : L.last.modelHz;
            Look ahead = look(r.sgp, obs_, clk.unix + 0.5);
            out.rateHzS = (now.ok && ahead.ok) ? (ahead.dopplerHz(L.nominal) - m) / 0.5 : 0;
            if (L.state != LockState::Ended) {
                // COAST: the loop has heard nothing for over a second — the model carries on alone, the trim FROZEN
                //   (brief §3: "on signal loss the loop freezes trim and the model keeps moving")
                const bool fll = tx.loop == "fll";
                L.state = (fll && (L.lastMeasure <= 0 || clk.unix - L.lastMeasure > 1.2)) ? LockState::Coast : LockState::Track;
                if (L.state == LockState::Track && fll) {
                    L.trimTarget += L.trimRate * dt;
                    L.trimTarget = std::max(-kTrimClampHz, std::min(kTrimClampHz, L.trimTarget));
                    const double step = kTrimSlewHzS * dt;
                    L.trim += std::max(-step, std::min(step, L.trimTarget - L.trim));
                }
                out.modelHz = m; out.trimHz = L.trim; out.shiftHz = m + L.trim;
            } else {
                // ★ ENDED (LOS): frozen where it was — the caller moves the dial there (brief §9: never snap back)
                out.modelHz = L.last.modelHz; out.trimHz = L.last.trimHz; out.shiftHz = L.last.shiftHz; out.rateHzS = 0;
                out.why = "Below the horizon — Doppler correction ended";
            }
        }
        out.state = L.state;
        L.last = out;
        return out;
    }

    mutable std::mutex m_;
    std::vector<SatDef> defs_;
    std::vector<SatRow> sats_;
    SatsMeta meta_;
    Observer obs_;
    std::map<std::string, Lock> locks_;
};

// ── ★★ THE HOOK FOR DECODERS (docs/v12/SATELLITE-ENGINE.md "RESUME HERE — the decoder hook") ─────────────────────────
/** The process's one engine (the shim's lock, the server's sky). Any translation unit may call it. */
inline Engine& engine() { static Engine e; return e; }
/** The shared dial's lock key (a session id never starts with \x01). Per-listener locks are keyed by the session id. */
constexpr const char* kSharedKey = "\x01shared";

/** What a decoder needs to know about the satellite its listener is on.
 *  ★★ TWO DIFFERENT DOPPLERS — read the right one:
 *   · `inChannelHz` is ALREADY APPLIED in the listener's channel NCO (the lock moves it 25 times a second). A decoder fed
 *     from that listener's CHANNEL IQ (clientIqCb — "the dial at DC") sees the signal at DC ± the residual only: it must
 *     NOT correct again. (On `costas` downlinks the engine's trim is 0, so inChannelHz == modelHz: the demodulator's own
 *     carrier loop takes the residual.)
 *   · `modelHz` (+ `rateHzS`) is the prediction for `downlinkHz` now. A decoder cutting its OWN slice of the capture at
 *     the nominal (feedSkimFromBins-style, off the shared forward FFT) must apply it itself — feed-forward into its NCO,
 *     with the rate for its timing loop. */
struct DecoderView {
    bool on = false;                 // a lock (TRACK / COAST) is running for this listener or the shared dial
    std::string state;               // "TRACK" | "COAST" | "ARMED" | "ENDED" | "" (none)
    long long norad = 0;
    std::string satId, satName, txLabel, consumer, loop;   // consumer: "lrpt", "ax25-9600", "ax25-1200", "bpsk-telemetry", ""
    double downlinkHz = 0;           // the nominal (the dial)
    double inChannelHz = 0;          // the shift the channel already carries (see above)
    double modelHz = 0, rateHzS = 0; // the model's Doppler at downlinkHz now, and its rate
    double elDeg = -90, azDeg = 0, rangeKm = 0;
    double aos = 0, los = 0;         // the pass (UTC Unix seconds)
    double ageDays = -1;             // the element set's age (the model is refused past 14 days)
};
/** The listener `session`'s lock, else the shared dial's, else nothing (`on` false). ★ Cheap (a map look-up under the
 *  engine's mutex, no SGP4): call it per block if you like. The values are the tracker's last tick (≤ 40 ms old). */
inline DecoderView decoderView(const std::string& session) {
    Track t = engine().selected(session);
    if (t.state == LockState::Off) t = engine().selected(kSharedKey);
    DecoderView v;
    if (t.state == LockState::Off) return v;
    v.state = lockName(t.state);
    v.on = t.state == LockState::Track || t.state == LockState::Coast;
    v.norad = t.norad; v.satId = t.satId; v.satName = t.satName; v.txLabel = t.txLabel; v.consumer = t.consumer; v.loop = t.loop;
    v.downlinkHz = t.nominalHz; v.inChannelHz = v.on ? t.shiftHz : 0.0; v.modelHz = t.modelHz; v.rateHzS = t.rateHzS;
    v.elDeg = t.elDeg; v.azDeg = t.azDeg; v.rangeKm = t.rangeKm; v.aos = t.aos; v.los = t.los; v.ageDays = t.ageDays;
    return v;
}
/** The model alone, for any listed bird at any frequency and time — a decoder that runs WITHOUT a listener's lock (a
 *  gallery capture of a Meteor pass, a background decoder). false = not listed / no elements / no location. */
inline bool dopplerNow(long long norad, double hz, double unixUtc, double& dopplerHz, double& rateHzS, double* elDeg = nullptr) {
    return engine().dopplerFor(norad, hz, unixUtc, dopplerHz, rateHzS, elDeg);
}

}}  // namespace vibe::sat
