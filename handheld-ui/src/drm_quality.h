// drm_quality.h — how well a DRM station is RECEIVED: src/utils/drmQuality.ts, ported line for line (the app and the web
// client import that file; this is its third reader, as dab_quality.h is dabQuality.ts's).
//
// ★★★ WHY (Stuart, 2026-11-12, Music4Joy in xHE-AAC): "on the R36S the main signal meter was reading 42db SNR but it was
//     dropping out more than the server … This is the same issue we had with the DAB MER not matching the standard signal
//     meter". The pill's SNR is the carrier over the noise beside it — strength. The decoder's SNR is the FAC cells' MER
//     after equalisation — what is LEFT once the echoes and fading are undone. Both are right; the second decides whether
//     the audio plays. So in DRM, as in DAB, the pill's gradient goes and its reading is the DECODER's SNR, with the
//     decoder's verdict in three bars beside it (app_impl.h drawDeck).
// ★★ Keep the thresholds and words in step with the TS — test/run_drm_quality_test.sh replays the TS meter's own outputs
//    (test/gen_drm_quality_golden.ts) through this one and fails on any difference.
#pragma once
#include "json.h"

#include <cmath>
#include <cstdio>
#include <deque>
#include <string>

namespace hh {
namespace drmq {

constexpr double FAC_BAD_MODERATE = 0.02, FAC_BAD_WEAK = 0.10;
constexpr double AUDIO_BAD_STRONG = 0.01, AUDIO_BAD_WEAK = 0.08;
constexpr double MIN_FAC = 3, MIN_AUDIO = 5;
constexpr int HOLD_DOWN = 2, HOLD_UP = 3;
constexpr double WINDOW_MS = 6000;

inline const char* label(int l) { static const char* t[] = {"No DRM signal", "DRM weak", "DRM moderate", "DRM strong"}; return t[l & 3]; }
inline const char* shortLabel(int l) { static const char* t[] = {"No signal", "Weak", "Moderate", "Strong"}; return t[l & 3]; }
inline const char* advice(int l) {
    static const char* t[] = {"Searching for a DRM signal", "No or heavily broken audio", "Expect occasional audio break-ups", "Clear audio"};
    return t[l & 3];
}
constexpr const char* FOUND_LABEL = "DRM found";
constexpr const char* FOUND_ADVICE = "Reading its parameters";
constexpr const char* UNPLAYABLE_ADVICE = "This server cannot play its audio";
/** ★ The R36S has no tooltip: pressing the reading shows this (a shorter DRM_SNR_EXPLAIN, the same meaning). */
constexpr const char* EXPLAIN_SHORT =
    "Decoded SNR: measured by the DRM decoder after it undoes echoes and fading. Lower than the signal meter's SNR "
    "(strength over the noise beside it) - both are right, and this one decides whether the audio plays.";
/** The Signal pane's merRow: about this MSC MER (dB) or better decodes cleanly (16-QAM 8, 64-QAM 14, 4-QAM 4; else 12). */
inline double merNeed(double qam) { return qam == 4 ? 4 : qam == 16 ? 8 : qam == 64 ? 14 : (std::isnan(qam) ? 8 : 12); }

/** JS Math.round (halves up), for the whole-dB figures the TS prints. */
inline long jsRound(double v) { return (long)std::floor(v + 0.5); }
/** ★ JS toFixed(1): an exact half rounds UP ("11.25" → "11.3"), where printf's %.1f rounds it to even ("11.2") — the
 *  golden test caught exactly that. Positive values only (MER, percentages). */
inline std::string fixed1(double v) {
    char b[32]; const double r = std::floor(v * 10 + 0.5) / 10; std::snprintf(b, sizeof b, "%.1f", r); return b;
}
/** drmQuality.ts pct(): "14", "4.1", "0.3" */
inline std::string pct(double f) {
    const double p = f * 100;
    if (p >= 10) return std::to_string(jsRound(p));
    std::string s = fixed1(p);
    if (p >= 1 && s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0) s.resize(s.size() - 2);
    return s;
}

}  // namespace drmq

struct DrmQuality {
    int level = 0;
    std::string label = drmq::label(0), shortL = drmq::shortLabel(0), advice = drmq::advice(0), detail;
    bool hasSnr = false; double snrDb = 0;   // ★ the DECODER's SNR (the FAC's MER), absent until locked
    /** drmQuality.ts drmBoxReading: "SNR 18 dB", or the verdict's one word before there is one. */
    std::string boxReading() const {
        if (!hasSnr) return shortL;
        char b[24]; std::snprintf(b, sizeof b, "SNR %ld dB", drmq::jsRound(snrDb)); return b;
    }
    /** drmQualityLine: "DRM strong · Clear audio · decoded SNR 18 dB". */
    std::string line() const { return label + " \xc2\xb7 " + advice + (detail.empty() ? "" : " \xc2\xb7 " + detail); }
};

struct DrmQSample { double t = 0, facOk = NAN, facBad = NAN, audioUnits = NAN, audioBad = NAN, snr = NAN, mscMer = NAN; };

struct DrmWindow {
    bool sync = false, locked = false;
    double snr = NAN, mscMer = NAN, mscQam = NAN;
    double facIn = 0, facBad = 0;
    double audioIn = NAN, audioBad = NAN;
    bool unplayable = false;
};

/** classifyDrmWindow — the pure core. */
inline void drmClassify(const DrmWindow& w, int& level, std::string& detail) {
    detail.clear();
    if (!w.locked) { level = 0; detail = w.sync ? "found" : "searching"; return; }
    const double need = drmq::merNeed(w.mscQam);
    auto merDetail = [&](double m) {
        char b[48];
        if (!std::isnan(w.mscQam)) std::snprintf(b, sizeof b, " (%g-QAM needs %g)", w.mscQam, need); else b[0] = 0;
        return "MSC MER " + drmq::fixed1(m) + " dB" + b;
    };
    if (!std::isnan(w.audioIn) && w.audioIn >= drmq::MIN_AUDIO) {
        const double lost = std::min(std::isnan(w.audioBad) ? 0 : w.audioBad, w.audioIn) / w.audioIn;
        level = lost >= drmq::AUDIO_BAD_WEAK ? 1 : lost >= drmq::AUDIO_BAD_STRONG ? 2 : 3;
        if (lost > 0) detail = drmq::pct(lost) + " % audio lost";
        if (level == 3 && !std::isnan(w.mscMer) && w.mscMer > 0 && w.mscMer < need) { level = 2; detail = merDetail(w.mscMer); }
        return;
    }
    level = 3;
    auto worse = [&](int l, const std::string& d) { if (l < level) { level = l; detail = d; } };
    if (w.facIn >= drmq::MIN_FAC) {
        const double e = std::min(w.facBad, w.facIn) / w.facIn;
        if (e >= drmq::FAC_BAD_WEAK) worse(1, "FAC " + drmq::pct(e) + " % errors");
        else if (e >= drmq::FAC_BAD_MODERATE) worse(2, "FAC " + drmq::pct(e) + " % errors");
    }
    if (!std::isnan(w.mscMer) && w.mscMer > 0) {
        if (w.mscMer < need - 3) worse(1, merDetail(w.mscMer));
        else if (w.mscMer < need) worse(2, merDetail(w.mscMer));
    } else if (!std::isnan(w.snr)) {
        char b[48]; std::snprintf(b, sizeof b, "decoded SNR %ld dB", drmq::jsRound(w.snr));
        if (w.snr < 9) worse(1, b); else if (w.snr < 15) worse(2, b);
    }
}

/** DrmQualityMeter — feed it every `drm` report; reset() on leaving DRM (no verdict may outlive the station). */
class DrmQualityMeter {
public:
    void reset() { samples_.clear(); shown_ = -1; pendL_ = -1; pendN_ = 0; have_ = false; last_ = DrmQuality{}; }
    bool has() const { return have_; }
    const DrmQuality& current() const { return last_; }

    const DrmQuality& push(const Json& r, double nowMs) {
        auto n = [&](const char* k) { const Json& v = r[k]; return v.kind == Json::Num && std::isfinite(v.n) ? v.n : NAN; };
        const bool locked = r["locked"].boolean(), sync = r["sync"].boolean();
        DrmQSample s;
        s.t = nowMs; s.facOk = n("facOk"); s.facBad = n("facBad"); s.audioUnits = n("audioUnits"); s.audioBad = n("audioBad");
        s.snr = locked ? n("snr") : NAN; s.mscMer = locked ? n("mscMer") : NAN;
        if (!samples_.empty()) {
            const DrmQSample& p = samples_.back();
            const double DrmQSample::*ks[] = {&DrmQSample::facOk, &DrmQSample::facBad, &DrmQSample::audioUnits, &DrmQSample::audioBad};
            for (auto k : ks) if (!std::isnan(p.*k) && !std::isnan(s.*k) && s.*k < p.*k) { samples_.clear(); break; }
        }
        if (!locked) samples_.clear();
        samples_.push_back(s);
        while (samples_.size() > 2 && nowMs - samples_[1].t >= drmq::WINDOW_MS) samples_.pop_front();
        const DrmQSample& a = samples_.front();
        const bool one = samples_.size() == 1;
        auto d = [&](double DrmQSample::*k) {
            const double x = a.*k, y = s.*k;
            if (std::isnan(y) || one || std::isnan(x)) return 0.0;
            return std::max(0.0, y - x);
        };
        auto mean = [&](double DrmQSample::*k) {
            double sum = 0; int c = 0;
            for (auto& q : samples_) if (!std::isnan(q.*k) && q.*k != 0) { sum += q.*k; c++; }
            return c ? sum / c : NAN;
        };
        DrmWindow w;
        w.sync = sync; w.locked = locked;
        w.snr = mean(&DrmQSample::snr); w.mscMer = mean(&DrmQSample::mscMer); w.mscQam = n("mscQam");
        w.facIn = d(&DrmQSample::facOk) + d(&DrmQSample::facBad); w.facBad = d(&DrmQSample::facBad);
        if (!std::isnan(s.audioUnits) && samples_.size() > 1) { w.audioIn = d(&DrmQSample::audioUnits) + d(&DrmQSample::audioBad); w.audioBad = d(&DrmQSample::audioBad); }
        w.unplayable = locked && !r["codec"].str().empty() && !(r["playable"].kind == Json::Bool && r["playable"].boolean());
        int vl; std::string vd;
        drmClassify(w, vl, vd);
        if (shown_ < 0) shown_ = vl;
        else if (vl != shown_) {
            if (pendL_ == vl) pendN_++; else { pendL_ = vl; pendN_ = 1; }
            if (pendN_ >= (vl < shown_ ? drmq::HOLD_DOWN : drmq::HOLD_UP)) { shown_ = vl; pendL_ = -1; pendN_ = 0; }
        } else { pendL_ = -1; pendN_ = 0; }
        if (!locked) { shown_ = 0; pendL_ = -1; pendN_ = 0; }
        const int lvl = shown_;
        const bool found = lvl == 0 && sync;
        DrmQuality q;
        q.level = lvl;
        q.label = found ? drmq::FOUND_LABEL : drmq::label(lvl);
        q.shortL = found ? "Locking" : drmq::shortLabel(lvl);
        q.advice = found ? drmq::FOUND_ADVICE : (lvl != 0 && w.unplayable) ? drmq::UNPLAYABLE_ADVICE : drmq::advice(lvl);
        if (lvl == vl && lvl != 0 && !vd.empty()) q.detail = vd;
        else if (lvl != 0 && !std::isnan(w.snr)) { char b[48]; std::snprintf(b, sizeof b, "decoded SNR %ld dB", drmq::jsRound(w.snr)); q.detail = b; }
        q.hasSnr = locked && !std::isnan(w.snr); q.snrDb = q.hasSnr ? w.snr : 0;
        last_ = q; have_ = true;
        return last_;
    }

private:
    std::deque<DrmQSample> samples_;
    int shown_ = -1, pendL_ = -1, pendN_ = 0;
    DrmQuality last_; bool have_ = false;
};

}  // namespace hh
