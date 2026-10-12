// VibeSDR — SpectrumProcessor: line-by-line port of src/assets/signalProcessor.ts. See spectrum_processor.h.
// Step numbers and comments follow the TypeScript so the two can be read side by side.
#include "spectrum_processor.h"
#include <cmath>
#include <cstring>

namespace vibedsp {

namespace {

constexpr double CLIP_THRESHOLD      = 14.97;
constexpr double PEAK_DECAY_DB_PER_S = 10;
constexpr double BAND_FLUSH_FRAC     = 0.4;
constexpr double RANGE_MARGIN        = 5;
constexpr double NOISE_PERCENTILE    = 0.10;
constexpr double EDGE_EXCLUDE_FRAC   = 0.09;
constexpr double NO_DATA_DBFS        = -200;
constexpr double PEAK_EXCLUDE_FRAC   = 0.004;
constexpr double MIN_HISTORY_MS      = 2000;
constexpr double MAX_HISTORY_MS      = 5000;
constexpr double FLOOR_FREEZE_SPAN_HZ = 25000;
constexpr double FLOOR_CLIMB_MAX     = 12;
constexpr double MIN_RANGE_DB        = 30;
constexpr double MAX_SLEW_DB_PER_SEC = 40;

// JS semantics, so the port matches bit for bit.
inline double jmax(double a, double b) { return (std::isnan(a) || std::isnan(b)) ? NAN : (a > b ? a : b); }
inline double jmin(double a, double b) { return (std::isnan(a) || std::isnan(b)) ? NAN : (a < b ? a : b); }
inline double jsign(double x) { return x > 0 ? 1 : (x < 0 ? -1 : x); }    // Math.sign (0 → 0, NaN → NaN)
inline double jround(double x) { return std::floor(x + 0.5); }             // Math.round
inline int32_t toInt32(double x) {                                          // `x | 0`
    if (!std::isfinite(x)) return 0;
    double t = std::trunc(x);
    double m = std::fmod(t, 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return (int32_t)(uint32_t)m;
}
inline uint8_t toUint8Clamped(double x) {                                   // a store into Uint8Array of 0…255 or NaN
    if (std::isnan(x)) return 0;
    return (uint8_t)(int)x;
}

}  // namespace

void SpectrumProcessor::applySettings(const SpectrumProcessorSettings& p) {
    const bool rangeChanged =
        p.autoContrast != s_.autoContrast || p.manual != s_.manual ||
        (p.manual && (p.manualMinDb != s_.manualMinDb || p.manualMaxDb != s_.manualMaxDb)) ||
        p.wfBrightness != s_.wfBrightness || p.wfContrast != s_.wfContrast ||
        p.wfSharpness != s_.wfSharpness || p.minRangeDb != s_.minRangeDb;
    s_ = p;
    if (!p.peakHold) { peakLine_.clear(); hasPeak_ = false; }
    if (rangeChanged) settingsVer_++;
}

double SpectrumProcessor::slewFloor(double want, double now) {
    if (!lastMinAt_ || !std::isfinite(actualMinDb_) || adoptRange_) {
        lastMinAt_ = now; return want;
    }
    const double dt = jmin(0.25, (now - lastMinAt_) / 1000);
    lastMinAt_ = now;
    const double step = MAX_SLEW_DB_PER_SEC * dt;
    const double d = want - actualMinDb_;
    return actualMinDb_ + (std::fabs(d) <= step ? d : jsign(d) * step);
}

void SpectrumProcessor::reset() {
    haveBuffers_ = false;
    peakLine_.clear(); hasPeak_ = false;
    minHistory_.clear(); maxHistory_.clear();
    lastFrameMs_ = 0;
    prevCenterHz_ = 0;
}

SpectrumFrameOut SpectrumProcessor::process(const float* bins, int n, double centerHz, double bwHz,
                                            bool interacting, double now) {
    const SpectrumProcessorSettings& s = s_;
    const double dtSec = lastFrameMs_ ? jmin(0.5, jmax(0.01, (now - lastFrameMs_) / 1000)) : 0.1;
    lastFrameMs_ = now;

    // ── 0. View change: drop stale ceiling samples ──
    if (centerHz > 0 && bwHz > 0) {
        const bool moved = std::fabs(centerHz - lastCenterHz_) > bwHz * 0.25
                        || std::fabs(bwHz - lastBwHz_) > lastBwHz_ * 0.05;
        if (moved && lastBwHz_ > 0) {
            maxHistory_.clear(); minHistory_.clear();
            adoptRange_ = true;
        }
        lastCenterHz_ = centerHz;
        lastBwHz_ = bwHz;
    }

    // ── 0a. Resize buffers if bin count changed ──
    if (!haveBuffers_ || (int)dbAvg_.size() != n) {
        dbAvg_.assign(bins, bins + n);
        specSmooth_.assign(bins, bins + n);
        peakLine_.clear(); hasPeak_ = false;
        tmp_.assign(n, 0.f);
        normRow_.assign(n, 0.f);
        outRow_.assign(n, 0);
        outSpec_.assign(n, 0.f);
        outPeak_.assign(n, 0.f);
        minHistory_.clear(); maxHistory_.clear();
        haveFrozenFloor_ = false;
        haveBuffers_ = true;
    }

    // ── 0b. Flush on settings change ──
    if (settingsVerApp_ != settingsVer_) {
        settingsVerApp_ = settingsVer_;
        std::memcpy(dbAvg_.data(), bins, sizeof(float) * n);
        minHistory_.clear(); maxHistory_.clear();
        haveFrozenFloor_ = false;
    }

    // ── 0c. Flush on band change (> 40% of visible bandwidth) ── (JS truthiness: 0 and NaN are false)
    const bool centerTruthy = centerHz != 0 && !std::isnan(centerHz);
    const bool prevTruthy = prevCenterHz_ != 0 && !std::isnan(prevCenterHz_);
    if (centerTruthy && prevTruthy && bwHz > 0 && std::fabs(centerHz - prevCenterHz_) > bwHz * BAND_FLUSH_FRAC) {
        std::memcpy(dbAvg_.data(), bins, sizeof(float) * n);
        std::memcpy(specSmooth_.data(), bins, sizeof(float) * n);
        peakLine_.clear(); hasPeak_ = false;
        minHistory_.clear(); maxHistory_.clear();
        haveFrozenFloor_ = false;
    }
    if (centerTruthy) prevCenterHz_ = centerHz;

    // ── 1a. Weak-signal processing → dbAvg ──
    const double w = (interacting || s.avgFrames <= 0) ? 0 : jmin(0.9, s.avgFrames);
    if (w > 0) {
        float* da = dbAvg_.data();
        const double nw = 1 - w;
        for (int i = 0; i < n; i++) da[i] = (float)((double)da[i] * w + (double)bins[i] * nw);
    } else {
        std::memcpy(dbAvg_.data(), bins, sizeof(float) * n);
    }

    // ── 2. Auto-range ──
    if (s.manual) {
        actualMinDb_ = s.manualMinDb;
        actualMaxDb_ = s.manualMaxDb;
    } else {
        uint32_t* hist = dbHist_;
        std::memset(hist, 0, sizeof(dbHist_));
        double absoluteMax = -INFINITY;
        int count = 0;
        const float* src = dbAvg_.data();
        int edge = (int)std::floor(n * EDGE_EXCLUDE_FRAC);
        if (n - 2 * edge < 16) edge = 0;
        for (int i = edge; i < n - edge; i++) {
            const double db = src[i];
            if (!std::isfinite(db)) continue;
            if ((double)bins[i] <= NO_DATA_DBFS) continue;
            count++;
            if (db > absoluteMax) absoluteMax = db;
            int32_t b = toInt32(db + 280);
            if (b < 0) b = 0; else if (b > 299) b = 299;
            hist[b]++;
        }
        if (count > 0) {
            const double target = std::floor(count * NOISE_PERCENTILE);
            double acc = 0, floorDb = -120;
            for (int b = 0; b < 300; b++) {
                acc += hist[b];
                if (acc > target) { floorDb = b - 280; break; }
            }
            const double targetMin = std::floor(floorDb - RANGE_MARGIN);
            const double drop = std::floor(count * PEAK_EXCLUDE_FRAC);
            double peakDb = absoluteMax;
            if (drop >= 1) {
                double accTop = 0;
                for (int b = 299; b >= 0; b--) {
                    accTop += hist[b];
                    if (accTop > drop) { peakDb = b - 280; break; }
                }
            }
            const double targetMax = std::ceil(peakDb + RANGE_MARGIN);

            maxHistory_.push_back({targetMax, now});
            while (!maxHistory_.empty() && now - maxHistory_.front().ts > MAX_HISTORY_MS) maxHistory_.pop_front();
            double sumMax = 0; for (const auto& e : maxHistory_) sumMax += e.value;
            const double wantMaxDb = sumMax / (double)maxHistory_.size() - s.autoContrast;

            if (!lastMaxAt_ || !std::isfinite(actualMaxDb_) || adoptRange_) {
                actualMaxDb_ = wantMaxDb;
            } else {
                const double dt = lastMaxAt_ ? jmin(0.25, (now - lastMaxAt_) / 1000) : 0.05;
                const double maxStep = MAX_SLEW_DB_PER_SEC * dt;
                const double delta = wantMaxDb - actualMaxDb_;
                actualMaxDb_ += std::fabs(delta) <= maxStep ? delta : jsign(delta) * maxStep;
            }
            lastMaxAt_ = now;

            const bool floorTrust = !(bwHz > 0 && bwHz < FLOOR_FREEZE_SPAN_HZ);
            if (floorTrust || !haveFrozenFloor_) {
                minHistory_.push_back({targetMin, now});
                while (!minHistory_.empty() && now - minHistory_.front().ts > MIN_HISTORY_MS) minHistory_.pop_front();
                double sumMin = 0; for (const auto& e : minHistory_) sumMin += e.value;
                const double avgMin = sumMin / (double)minHistory_.size();
                actualMinDb_ = slewFloor(avgMin + s.autoContrast, now);
                if (floorTrust) { frozenFloorDb_ = avgMin; haveFrozenFloor_ = true; }
            } else {
                const double cappedFloor = jmin(targetMin, frozenFloorDb_ + FLOOR_CLIMB_MAX);
                actualMinDb_ = slewFloor(cappedFloor + s.autoContrast, now);
            }
            adoptRange_ = false;
        }
    }
    const double minRange = (std::isfinite(s.minRangeDb) && s.minRangeDb > 0) ? s.minRangeDb : MIN_RANGE_DB;
    if (actualMaxDb_ - actualMinDb_ < minRange) actualMaxDb_ = actualMinDb_ + minRange;
    const double dbRange = actualMaxDb_ - actualMinDb_;

    // ── 4. Spatial 5-tap smooth [1,2,3,2,1]/9 ──
    float* tmp = tmp_.data();
    const float* a = dbAvg_.data();
    if (s.spatialSmooth && n >= 5) {
        tmp[0]     = (float)(((double)a[0] * 3 + (double)a[1] * 2) / 5);
        tmp[1]     = (float)(((double)a[0] + (double)a[1] * 2 + (double)a[2] * 2) / 5);
        tmp[n - 1] = (float)(((double)a[n - 2] * 2 + (double)a[n - 1] * 3) / 5);
        tmp[n - 2] = (float)(((double)a[n - 3] + (double)a[n - 2] * 2 + (double)a[n - 1] * 2) / 5);
        for (int k = 2; k < n - 2; k++) {
            tmp[k] = (float)(((double)a[k - 2] + (double)a[k - 1] * 2 + (double)a[k] * 3 +
                              (double)a[k + 1] * 2 + (double)a[k + 2]) / 9);
        }
    } else {
        std::memcpy(tmp, a, sizeof(float) * n);
    }

    // ── 5. Spectrum EMA — time-normalised, rise 4× faster than fall ──
    float* spec = specSmooth_.data();
    const bool noSmooth = s.smoothingFrames <= 1;
    const double tcSec = noSmooth ? 0 : (s.smoothingFrames - 1) / 20;
    const double fallAlpha = noSmooth ? 1.0 : jmin(0.95, 1.0 - std::exp(-dtSec / jmax(0.01, tcSec)));
    const double riseAlpha = noSmooth ? 1.0 : jmin(0.95, fallAlpha * 4);

    // ── 6. Peak hold seed (only once spec has signal) ──
    if (s.peakHold && !hasPeak_) {
        bool hasSignal = false;
        for (int i = 0; i < n; i++) if (spec[i] != 0) { hasSignal = true; break; }
        if (hasSignal) { peakLine_.assign(spec, spec + n); hasPeak_ = true; }
    }
    float* pk = hasPeak_ ? peakLine_.data() : nullptr;

    // ── 5/6/7 combined per-bin loop ──
    float* norm = normRow_.data();
    const double brightDb = s.wfBrightness;
    for (int j = 0; j < n; j++) {
        const double ta = (double)tmp[j] > (double)spec[j] ? riseAlpha : fallAlpha;
        spec[j] = (float)((double)spec[j] + ta * ((double)tmp[j] - (double)spec[j]));
        if (s.peakHold && pk) {
            const double cur = spec[j];
            pk[j] = cur > (double)pk[j] ? (float)cur : (float)((double)pk[j] - PEAK_DECAY_DB_PER_S * dtSec);
        }
        const double nrm = jmax(0, jmin(1, ((double)tmp[j] + brightDb - actualMinDb_) / dbRange));
        double mag = nrm * 255;
        mag = mag < CLIP_THRESHOLD ? 0 : ((mag - CLIP_THRESHOLD) / (255 - CLIP_THRESHOLD)) * 255;
        norm[j] = (float)(mag / 255);
    }

    // ── 8. Row output = normalised intensity bytes ──
    uint8_t* out = outRow_.data();
    for (int j = 0; j < n; j++) out[j] = toUint8Clamped(jmax(0, jmin(255, jround((double)norm[j] * 255))));

    // ── Normalised spectrum / peak outputs ──
    float* oSpec = outSpec_.data();
    float* oPeak = outPeak_.data();
    const double sf = dbRange > 0 ? s.specFloor / dbRange : 0;
    const double sp = s.specPeakScale / 10;
    for (int j = 0; j < n; j++) {
        const double ns = jmax(0, jmin(1, ((double)spec[j] - actualMinDb_) / dbRange));
        oSpec[j] = (float)jmax(0, jmin(1, (ns + sf) * sp));
        if (s.peakHold && pk) {
            const double np = jmax(0, jmin(1, ((double)pk[j] - actualMinDb_) / dbRange));
            oPeak[j] = (float)jmax(0, jmin(1, (np + sf) * sp));
        } else {
            oPeak[j] = 0;
        }
    }

    SpectrumFrameOut r;
    r.row = out; r.spec = oSpec; r.peak = oPeak; r.n = n;
    r.dbMin = actualMinDb_; r.dbMax = actualMaxDb_;
    return r;
}

}  // namespace vibedsp
