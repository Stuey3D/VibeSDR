// dab_quality.h — how well a DAB multiplex is RECEIVED, as three bars and a sentence: src/utils/dabQuality.ts, ported
// line for line (the app and the web client both import that file; this is its third reader).
//
// ★★★ WHY (Stuart, 2026-10-06): in DAB the ordinary signal bar measures power in the passband — S9 for an hour on a
//     multiplex that listed its stations and never played a sound. So in DAB the pill's gradient goes and the meter
//     says what the listener will HEAR: "Multiplex weak · No or heavily broken audio · 14 % frames lost".
// ★★ The counters are cumulative; the meter keeps ~5 s of reports and judges the DIFFERENCE. A counter that goes
//    backwards (a service change) starts the window again, as does a new multiplex. Hysteresis: 2 reports down, 3 up.
// ★ Keep the thresholds in step with the TS — nothing here is invented, and nothing may drift from it.
#pragma once
#include "json.h"

#include <cmath>
#include <cstdio>
#include <deque>
#include <string>

namespace hh {
namespace dabq {

constexpr double SF_OK_STRONG = 0.98, SF_OK_WEAK = 0.90;
constexpr double MP2_BAD_STRONG = 0.01, MP2_BAD_WEAK = 0.10;
constexpr double BER_MODERATE = 0.015, BER_WEAK = 0.085;
constexpr double MER_STRONG = 16, MER_WEAK = 10;
constexpr double FIB_STRONG = 0.99,  FIB_WEAK = 0.90, FIB_NONE = 0.05;
constexpr double ERASED_MODERATE = 0.02, ERASED_WEAK = 0.10;
constexpr double MIN_SF = 4, MIN_MP2 = 20, MIN_AU = 10;
constexpr double AU_BAD_STRONG = 0.01, AU_BAD_WEAK = 0.08;
constexpr double SCF_STRONG = 0.02, SCF_WEAK = 0.15;
constexpr int HOLD_DOWN = 2, HOLD_UP = 3;
constexpr double WINDOW_MS = 5000;

inline const char* label(int l) { static const char* t[] = {"No signal", "Multiplex weak", "Multiplex moderate", "Multiplex strong"}; return t[l & 3]; }
inline const char* shortLabel(int l) { static const char* t[] = {"No signal", "Weak", "Moderate", "Strong"}; return t[l & 3]; }
inline const char* advice(int l) {
    static const char* t[] = {"Searching for the multiplex", "No or heavily broken audio", "Expect occasional audio break-ups", "Clear audio"};
    return t[l & 3];
}
/** DAB_STARTING_ADVICE / DAB_CHOOSE_ADVICE / DAB_NO_AUDIO_ADVICE, and DAB_NO_AUDIO_MS. */
constexpr const char* STARTING_ADVICE = "Starting audio…";
constexpr const char* CHOOSE_ADVICE = "Choose a station";
constexpr const char* NO_AUDIO_ADVICE = "No audio reaching you";
constexpr double NO_AUDIO_MS = 4000;
/** dabQuality.ts pct(): "14", "4.1", "0.3" */
inline std::string pct(double f) {
    const double p = f * 100;
    char b[24];
    if (p >= 10) std::snprintf(b, sizeof b, "%.0f", std::round(p));
    else if (p >= 1) { std::snprintf(b, sizeof b, "%.1f", p); std::string s = b; if (s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0) s.resize(s.size() - 2); return s; }
    else std::snprintf(b, sizeof b, "%.1f", p);
    return b;
}

}  // namespace dabq

struct DabQuality {
    int level = 0;
    std::string label = dabq::label(0), shortL = dabq::shortLabel(0), advice = dabq::advice(0), detail;
    bool hasMer = false; double merDb = 0;
    /** A service is selected and has not produced its first good audio unit (dabQuality.ts `starting`). */
    bool starting = false;
};

/** One `dab` report's counters (absent = NaN, the TS's undefined). */
struct DabQSample {
    double t = 0;
    double frames = NAN, erased = NAN, mp2In = NAN, mp2Bad = NAN, sfTried = NAN, sfOk = NAN;
    double scfConcealed = NAN, auIn = NAN, auBad = NAN, rsLost = NAN;
    double fibRate = NAN, mer = NAN, mscBer = NAN;
};

struct DabWindow {
    bool locked = false;
    double fibRate = NAN, fibNow = NAN, mer = NAN, mscBer = NAN;
    double sfTried = 0, sfOk = 0, mp2In = 0, mp2Bad = 0, frames = 0, erased = 0;
    double scfConcealed = NAN, auIn = NAN, auBad = NAN, rsLost = NAN;
    /** dabQuality.ts audioStarted: -1 undefined, 0 = no audio evidence may be used (no service, or none decoded yet), 1. */
    int audioStarted = -1;
};

/** classifyDabWindow — the verdict for one window, no memory. */
inline void dabClassify(const DabWindow& w, int& level, std::string& detail) {
    using namespace dabq;
    auto has = [](double v) { return !std::isnan(v); };
    detail.clear();
    if (!w.locked) { level = 0; detail = "searching"; return; }
    if (has(w.fibNow) && w.fibNow < FIB_NONE) { level = 0; detail = "no station list"; return; }
    bool haveA = false; int aL = 3; std::string aD;
    auto worseA = [&](int l, const std::string& d) { if (!haveA || l < aL) { haveA = true; aL = l; aD = d; } };
    if (w.audioStarted == 0) { /* start-up / no service: the prediction below decides */ }
    else if (has(w.auIn) && w.auIn >= MIN_AU) {
        const double lost = std::min(has(w.auBad) ? w.auBad : 0.0, w.auIn) / w.auIn;
        worseA(lost >= AU_BAD_WEAK ? 1 : lost >= AU_BAD_STRONG ? 2 : 3, lost > 0 ? pct(lost) + " % audio lost" : "");
    } else if (w.sfTried >= MIN_SF) {
        const double lost = 1 - std::min(w.sfOk, w.sfTried) / w.sfTried;
        worseA(1 - lost <= SF_OK_WEAK ? 1 : 1 - lost <= SF_OK_STRONG ? 2 : 3, lost > 0 ? pct(lost) + " % frames lost" : "");
        if (has(w.rsLost) && w.rsLost > 0) worseA(2, std::to_string((long)w.rsLost) + " RS failures");
    }
    if (w.audioStarted != 0 && w.mp2In >= MIN_MP2) {
        const double bad = std::min(w.mp2Bad, w.mp2In) / w.mp2In;
        worseA(bad >= MP2_BAD_WEAK ? 1 : bad >= MP2_BAD_STRONG ? 2 : 3, bad > 0 ? pct(bad) + " % frames bad" : "");
        if (has(w.scfConcealed)) {
            const double mud = w.scfConcealed / w.mp2In;
            worseA(mud >= SCF_WEAK ? 1 : mud >= SCF_STRONG ? 2 : 3, mud > 0 ? pct(std::min(1.0, mud)) + " % mud" : "");
        }
    }
    if (haveA) {
        int l = aL; std::string d = aD;
        if (has(w.mscBer)) {
            if (w.mscBer >= BER_WEAK && l > 1) { l = 1; d = "BER " + pct(w.mscBer) + " %"; }
            else if (w.mscBer >= BER_MODERATE && l > 2) { l = 2; d = "BER " + pct(w.mscBer) + " %"; }
        }
        const bool seesMud = (has(w.auIn) && w.auIn >= MIN_AU) || (w.mp2In >= MIN_MP2 && has(w.scfConcealed));
        if (!seesMud && l == 3 && has(w.mer) && w.mer > 0 && w.mer < MER_WEAK) {
            char b[40]; std::snprintf(b, sizeof b, "MER %.1f dB", w.mer); l = 2; d = b;
        }
        level = l; detail = d; return;
    }
    level = 3;
    auto worse = [&](int l, const std::string& d) { if (l < level) { level = l; detail = d; } };
    if (has(w.fibRate)) {
        if (w.fibRate < FIB_WEAK) worse(1, "FIC " + pct(1 - w.fibRate) + " % errors");
        else if (w.fibRate < FIB_STRONG) worse(2, "FIC " + pct(1 - w.fibRate) + " % errors");
    }
    if (has(w.mer) && w.mer > 0) {
        char b[40]; std::snprintf(b, sizeof b, "MER %.1f dB", w.mer);
        if (w.mer < MER_WEAK) worse(1, b); else if (w.mer < MER_STRONG) worse(2, b);
    }
    if (has(w.mscBer)) {
        if (w.mscBer >= BER_WEAK) worse(1, "BER " + pct(w.mscBer) + " %");
        else if (w.mscBer >= BER_MODERATE) worse(2, "BER " + pct(w.mscBer) + " %");
    }
    if (w.frames >= 10) {
        const double e = std::min(w.erased, w.frames) / w.frames;
        if (e >= ERASED_WEAK) worse(1, pct(e) + " % frames erased");
        else if (e >= ERASED_MODERATE) worse(2, pct(e) + " % frames erased");
    }
}

/** DabQualityMeter — feed it every `dab` report; reset() on leaving DAB (no verdict may outlive the mode). */
class DabQualityMeter {
public:
    void reset() { samples_.clear(); channel_.clear(); haveChannel_ = false; shown_ = -1; pendL_ = -1; pendN_ = 0; have_ = false; sid_ = 0; startingSince_ = NAN; }
    bool has() const { return have_; }
    const DabQuality& current() const { return last_; }

    const DabQuality& push(const Json& r, double nowMs) {
        const std::string ch = r["channel"].str();
        if (!haveChannel_ || ch != channel_) { reset(); channel_ = ch; haveChannel_ = true; }
        auto n = [&](const char* k) { const Json& v = r[k]; return v.kind == Json::Num && std::isfinite(v.n) ? v.n : NAN; };
        const double sid = std::isnan(n("sid")) ? 0 : n("sid");
        DabQSample s;
        s.t = nowMs;
        s.frames = n("frames"); s.erased = n("erased"); s.mp2In = n("mp2In"); s.mp2Bad = n("mp2Bad");
        s.sfTried = n("sfTried"); s.sfOk = n("sfOk"); s.scfConcealed = n("scfConcealed"); s.auIn = n("auIn"); s.auBad = n("auBad");
        s.rsLost = n("rsLost"); s.fibRate = n("fibRate"); s.mer = n("mer");
        s.mscBer = sid ? n("mscBer") : NAN;
        if (!samples_.empty()) {
            const DabQSample& p = samples_.back();
            const double DabQSample::*ks[] = {&DabQSample::frames, &DabQSample::erased, &DabQSample::mp2In, &DabQSample::mp2Bad, &DabQSample::sfTried,
                                              &DabQSample::sfOk, &DabQSample::scfConcealed, &DabQSample::auIn, &DabQSample::auBad, &DabQSample::rsLost};
            for (auto k : ks) if (!std::isnan(p.*k) && !std::isnan(s.*k) && s.*k < p.*k) {
                samples_.clear();
                if (!std::isnan(startingSince_)) startingSince_ = nowMs;   // a new service starts its own wait
                break;
            }
        }
        // ★ Has this service played yet? (dabQuality.ts) — the server's per-service totals answer it.
        auto z = [](double v) { return std::isnan(v) ? 0.0 : v; };
        const bool started = sid && (z(s.sfOk) > 0 || z(s.auIn) - z(s.auBad) > 0 || z(s.mp2In) - z(s.mp2Bad) > 0);
        if (sid != sid_) { sid_ = sid; startingSince_ = (sid && !started) ? nowMs : NAN; }
        if (!sid) startingSince_ = NAN;
        else if (!started) { if (std::isnan(startingSince_)) startingSince_ = nowMs; }
        else if (!std::isnan(startingSince_)) {
            startingSince_ = NAN;
            samples_.clear(); samples_.push_back(s);   // the first good unit: judge from HERE (a base copy)
        }
        samples_.push_back(s);
        while (samples_.size() > 2 && nowMs - samples_[1].t >= dabq::WINDOW_MS) samples_.pop_front();
        const DabQSample& a = samples_.front();
        const bool one = samples_.size() == 1;
        auto d = [&](double DabQSample::*k) {
            const double x = a.*k, y = s.*k;
            if (std::isnan(y)) return 0.0;
            if (one || std::isnan(x)) return y;
            return std::max(0.0, y - x);
        };
        auto mean = [&](double DabQSample::*k) {
            double sum = 0; int c = 0;
            for (auto& q : samples_) if (!std::isnan(q.*k)) { sum += q.*k; c++; }
            return c ? sum / c : NAN;
        };
        DabWindow w;
        w.locked = r["locked"].boolean();
        w.fibRate = mean(&DabQSample::fibRate); w.fibNow = s.fibRate;
        w.mer = mean(&DabQSample::mer); w.mscBer = mean(&DabQSample::mscBer);
        w.sfTried = d(&DabQSample::sfTried); w.sfOk = d(&DabQSample::sfOk); w.mp2In = d(&DabQSample::mp2In); w.mp2Bad = d(&DabQSample::mp2Bad);
        w.scfConcealed = std::isnan(s.scfConcealed) ? NAN : d(&DabQSample::scfConcealed);
        w.auIn = std::isnan(s.auIn) ? NAN : d(&DabQSample::auIn);
        w.auBad = std::isnan(s.auBad) ? NAN : d(&DabQSample::auBad);
        w.rsLost = std::isnan(s.rsLost) ? NAN : d(&DabQSample::rsLost);
        w.frames = samples_.size() > 1 ? d(&DabQSample::frames) : 0;
        w.erased = samples_.size() > 1 ? d(&DabQSample::erased) : 0;
        w.audioStarted = started ? 1 : 0;
        int vl; std::string vd;
        dabClassify(w, vl, vd);
        if (shown_ < 0) shown_ = vl;
        else if (vl != shown_) {
            if (pendL_ == vl) pendN_++; else { pendL_ = vl; pendN_ = 1; }
            if (pendN_ >= (vl < shown_ ? dabq::HOLD_DOWN : dabq::HOLD_UP)) { shown_ = vl; pendL_ = -1; pendN_ = 0; }
        } else { pendL_ = -1; pendN_ = 0; }
        const int lvl = shown_;
        DabQuality q;
        q.level = lvl; q.label = dabq::label(lvl); q.shortL = dabq::shortLabel(lvl);
        q.hasMer = w.locked && !std::isnan(w.mer) && w.mer > 0; q.merDb = q.hasMer ? w.mer : 0;
        q.advice = (lvl == 0 && vl == 0 && vd == "no station list") ? "Locked, but nothing decodes" : dabq::advice(lvl);
        if (lvl == vl && lvl != 0) q.detail = vd;
        if (lvl != 0 && !sid) q.advice = dabq::CHOOSE_ADVICE;
        else if (lvl != 0 && !started && !std::isnan(startingSince_)) {
            q.starting = true;
            q.advice = nowMs - startingSince_ >= dabq::NO_AUDIO_MS ? dabq::NO_AUDIO_ADVICE : dabq::STARTING_ADVICE;
        }
        last_ = q; have_ = true;
        return last_;
    }

private:
    std::deque<DabQSample> samples_;
    std::string channel_; bool haveChannel_ = false;
    int shown_ = -1, pendL_ = -1, pendN_ = 0;
    DabQuality last_; bool have_ = false;
    double sid_ = 0, startingSince_ = NAN;
};

}  // namespace hh
