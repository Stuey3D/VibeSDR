// wf_row_combine.h — when the waterfall thins a feed, COMBINE the frames into the row instead of dropping them. A
// LINE-FOR-LINE port of src/assets/wfCombine.ts (the app's JS path and the web use that one); test_spectrum_engine.cpp
// proves it alongside scripts/test_wf_combine.ts. Change one, change both.
//
// ★★★ Stuart (2026-10-30): "with the frame rates that cut the rate where the server cannot send a lower rate rather than
//     simply dropping the excess frames can we average them". Per bin:
//         out = max( mean of the frames' LINEAR power, the strongest frame's power − 6 dB )
//     Linear power, never dB (0 and 10 dB → 7.4, not 5); the −6 dB floor under the strongest frame keeps a one-frame
//     burst visible; on noise the mean wins, so the floor is smoother than one frame. A row of ONE frame is that frame,
//     byte for byte (NATIVE, a feed at or below the rate: untouched). Groups restart across a tune/zoom (geometry).
//     Bytes are linear in dB at (dbMax − dbMin) × (255 − 14.97) / 255² a unit (spectrum_processor.cpp).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vibewf {

constexpr double COMBINE_PEAK_DB = 6;
constexpr int COMBINE_MAX_FRAMES = 24;   // the most frames the engine KEEPS for one row (clockRows_)

inline double dbPerUnit(double dbMin, double dbMax) {
    return (dbMax > dbMin ? dbMax - dbMin : 60.0) * (255.0 - 14.97) / (255.0 * 255.0);
}

struct RowCombiner {
    std::vector<float> sum, peak;
    int n = 0; size_t len = 0; double lo = 0, span = 0, k = 0;
    float lut[256] = {}; double lutK = -1;
    std::vector<uint8_t> one;   // the group's only frame so far, exact
};

inline void combineReset(RowCombiner& c) { c.n = 0; }

inline void combineFold(RowCombiner& c, const uint8_t* row, size_t len) {
    float* s = c.sum.data(); float* p = c.peak.data();
    for (size_t i = 0; i < len; i++) { const float v = c.lut[row[i]]; s[i] += v; if (v > p[i]) p[i] = v; }
}

/** Add a frame; a different geometry restarts the group from it (returns false then). */
inline bool combineAdd(RowCombiner& c, const uint8_t* row, size_t len, double lo, double span, double k) {
    bool kept = true;
    if (c.n > 0 && (len != c.len || lo != c.lo || span != c.span)) { c.n = 0; kept = false; }
    if (c.sum.size() < len) { c.sum.resize(len); c.peak.resize(len); }
    if (c.one.size() < len) c.one.resize(len);
    if (c.n == 0) {
        c.len = len; c.lo = lo; c.span = span; c.k = k > 0 ? k : dbPerUnit(0, 0);
        std::copy(row, row + len, c.one.begin());
        c.n = 1;
        return kept;
    }
    if (c.lutK != c.k) { for (int b = 0; b < 256; b++) c.lut[b] = (float)std::pow(10.0, b * c.k / 10.0); c.lutK = c.k; }
    if (c.n == 1) {
        std::fill(c.sum.begin(), c.sum.begin() + len, 0.f); std::fill(c.peak.begin(), c.peak.begin() + len, 0.f);
        combineFold(c, c.one.data(), len);
    }
    combineFold(c, row, len);
    c.n++;
    return true;
}

/** The combined row into `out` (its length must be the group's); the group empties. n = 1 → the frame exactly. Returns
 *  the frames combined (0 = nothing written). */
inline int combineTake(RowCombiner& c, uint8_t* out, size_t outLen) {
    const int n = c.n; const size_t len = c.len;
    c.n = 0;
    if (n == 0 || outLen != len) return 0;
    if (n == 1) { std::copy(c.one.begin(), c.one.begin() + len, out); return 1; }
    const double inv = 1.0 / n, scale = 10.0 / c.k, pf = std::pow(10.0, -COMBINE_PEAK_DB / 10.0);
    for (size_t i = 0; i < len; i++) {
        const double m = c.sum[i] * inv, pk = c.peak[i] * pf;
        const long v = std::lround(std::log10(m > pk ? m : pk) * scale);
        out[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    return n;
}

}  // namespace vibewf
