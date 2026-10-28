// wf_glide.h — the waterfall GLIDE: a tune's trail drawn as one continuous line. A LINE-FOR-LINE port of glideRow and
// friends in src/utils/wfRowPlace.ts (the app's JS path); test_wf_glide.cpp runs the same cases as the glide section of
// scripts/test_wf_row_place.ts. Change one, change both.
//
// ★★ Stuart, 2026-10-28 (build 501, 20 fps): "the signal glide into place looks broken". A row is one device-pixel row
//    and a fast tune moves a station tens of bins between rows, so the diagonal was dots. When a row's placement differs
//    from the previous row's, its SIGNAL columns (> median + GLIDE_MARGIN) are streaked back to where they sat in that
//    row, so consecutive rows join. Only the new row, once; steady rows untouched; the floor never raised; a jump past
//    GLIDE_MAX_FRAC of the width is left as it was (a gap). A ZOOM (any change of span) is never streaked: the zoom fan
//    (wf_zoom_fan.h) re-maps the history to the new view, and a streak on top would draw the fan twice.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vibewf {

constexpr int GLIDE_MARGIN = 24;
constexpr double GLIDE_MAX_FRAC = 1.0 / 8;

/** True when two rows' bin scales differ enough to count as a zoom (not float noise). */
inline bool spanChanged(double span0, double span1) {
    return span0 > 0 && span1 > 0 && std::fabs(span1 - span0) > span0 * 1e-6;
}

struct GlideState {
    bool has = false; double startHz = 0, hzPerCol = 0;
    std::vector<uint8_t> mask; std::vector<int32_t> dq; uint32_t hist[256] = {};
    explicit GlideState(int n = 0) : mask(n), dq(n) {}
};

/** Column c of the WRITTEN row is startHz + c × hzPerCol; NaN when the row's centre/scale is unknown. */
inline double placedStartHz(double rowCenterHz, double rowHzPerBin, double off, int n) {
    if (!(rowCenterHz > 0 && rowHzPerBin > 0 && n > 0)) return NAN;
    return rowCenterHz - (off + n / 2.0) * rowHzPerBin;
}

/** p(c) = c + b (new column → previous row's column; a is always 1); false = steady, a zoom (the fan's job), or a jump. */
inline bool glideMap(double prevStartHz, double prevHzPerCol, double startHz, double hzPerCol, int n, double& a, double& b) {
    if (!(prevHzPerCol > 0 && hzPerCol > 0 && n > 0) || !std::isfinite(prevStartHz) || !std::isfinite(startHz)) return false;
    if (spanChanged(prevHzPerCol, hzPerCol)) return false;   // ★ a zoom: the fan placed the history — never streak it too
    a = 1;
    b = (startHz - prevStartHz) / prevHzPerCol;
    const double span = std::fabs(b);
    if (span < 0.5 || span > n * GLIDE_MAX_FRAC) return false;
    return true;
}

inline int rowMedian(const uint8_t* row, int n, uint32_t* hist) {
    std::fill(hist, hist + 256, 0u);
    for (int i = 0; i < n; i++) hist[row[i]]++;
    const uint32_t half = (uint32_t)(n >> 1);
    uint32_t acc = 0;
    for (int v = 0; v < 256; v++) { acc += hist[v]; if (acc > half) return v; }
    return 255;
}

/** Streak the signal in `row` (n columns, in place) along p(c) = a·c + b. Returns the columns raised. */
inline int glideStreak(uint8_t* row, int n, double a, double b, GlideState& st) {
    if ((int)st.mask.size() < n) { st.mask.resize(n); st.dq.resize(n); }
    const int thr = rowMedian(row, n, st.hist) + GLIDE_MARGIN;
    uint8_t* mask = st.mask.data();
    bool any = false;
    for (int i = 0; i < n; i++) { const uint8_t v = row[i]; if (v > thr) { mask[i] = v; any = true; } else mask[i] = 0; }
    if (!any) return 0;
    int raised = 0;
    if (a == 1) {
        // A pure tune: a sliding-window max of the mask (monotonic deque), O(n) whatever the shift.
        const int s = (int)std::floor(b + 0.5);   // Math.round
        if (s == 0) return 0;
        int32_t* dq = st.dq.data();
        int h = 0, t = 0;
        if (s > 0) {
            for (int x = 0; x < n; x++) {          // window c ∈ [x − s, x]
                while (t > h && mask[dq[t - 1]] <= mask[x]) t--;
                dq[t++] = x;
                while (dq[h] < x - s) h++;
                const uint8_t m = mask[dq[h]];
                if (m > row[x]) { row[x] = m; raised++; }
            }
        } else {
            const int k = -s;
            for (int x = n - 1; x >= 0; x--) {     // window c ∈ [x, x + k]
                while (t > h && mask[dq[t - 1]] <= mask[x]) t--;
                dq[t++] = x;
                while (dq[h] > x + k) h++;
                const uint8_t m = mask[dq[h]];
                if (m > row[x]) { row[x] = m; raised++; }
            }
        }
        return raised;
    }
    // A scale change is a zoom — the fan's job (glideMap never asks for one). Nothing to streak.
    return raised;
}

/** After writeRow: streak the row just written if its placement moved; remember this row's placement. Returns the span. */
inline double glideRow(uint8_t* row, int n, double rowCenterHz, double rowHzPerBin, double off, GlideState& st) {
    const double start = placedStartHz(rowCenterHz, rowHzPerBin, off, n);
    if (!std::isfinite(start)) { st.has = false; return 0; }
    double span = 0, a = 1, b = 0;
    if (st.has && glideMap(st.startHz, st.hzPerCol, start, rowHzPerBin, n, a, b)) {
        glideStreak(row, n, a, b, st);
        span = std::fabs(b);
    }
    st.has = true; st.startHz = start; st.hzPerCol = rowHzPerBin;
    return span;
}

}  // namespace vibewf
