// wf_zoom_fan.h — THE ZOOM FAN for the native engine's waterfall ring: a zoom re-maps the whole history to the new view.
// A LINE-FOR-LINE port of the zoom-fan section of src/utils/wfRowPlace.ts (the app's JS path — the full design note and
// the cost trade-off are there); test_wf_motion.cpp runs the same cases as that section of scripts/test_wf_row_place.ts.
// Change one, change both.
//
// ★★★ Stuart, 2026-10-27/28: zooming left the history in horizontal SLICES — every old row kept the scale it was drawn
//     at. The hard rule (memory feedback_waterfall_core_rules): "when zooming in and out you had a cool fanning in and out
//     of the signals". The STORED ring is never rewritten; each slot carries a map u = a·x + b (screen x → the row's own
//     u), composed only when the SPAN changes (a tune leaves it alone — the bend, #57), and the DISPLAYED ring the image
//     is made from is regenerated through the maps: lossless (zoom in then out restores the edges byte for byte) and a
//     zoom out keeps one-bin carriers (max under a pixel).
//  ★★ A JUMP (a placement more than MOTION_MAX_FRAC of the width from the row before — a bookmark, a typed frequency, a
//    spin faster than the radio) is re-mapped like a zoom: the history slides into place, black only at the newly exposed
//    edge (Stuart 2026-10-28: "NO SLICING when zooming or tuning"). stepK/stepC compose each fan's screen move for the UI's
//    ease (the shader eases from the old geometry over one row interval at display rate).
//  ★ RAM: the displayed ring (RING_W × RING_ROWS = 1 MiB) is allocated at the FIRST zoom and freed once every row is back
//    at the identity (zoomed back, or RING_ROWS rows since). Maps: 16 KiB, always. CPU: one regeneration of the ring per
//    zoom step, on the engine thread; a 1 KiB copy per row while a zoomed history is on screen. GPU: nothing.
#pragma once
#include "wf_motion.h"
#include "wf_wide.h"   // ★ V12 the full-capture layer the fan fills from
#include <algorithm>
#include <memory>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vibewf {

// spanChanged() and placedStartHz() are in wf_motion.h.

/** Every map after the view moved (lo0, span0) → (lo1, span1) Hz; one back at the identity snaps to it exactly. Returns
 *  true if any map is not the identity afterwards. */
inline bool applyViewChange(double* xfA, double* xfB, int count, double lo0, double span0, double lo1, double span1) {
    if (!(span0 > 0 && span1 > 0)) return false;
    const double k = span1 / span0, c = (lo1 - lo0) / span0;
    bool any = false;
    for (int i = 0; i < count; i++) {
        const double a = xfA[i];
        double na = a * k, nb = a * c + xfB[i];
        if (std::fabs(na - 1) < 1e-9 && std::fabs(nb) < 1e-9) { na = 1; nb = 0; } else any = true;
        xfA[i] = na; xfB[i] = nb;
    }
    return any;
}

struct FanState {
    double lastLo = 0, lastSpan = 0;
    std::vector<double> xfA, xfB;
    std::vector<uint8_t> disp;     // empty = the stored ring IS the display
    int sinceZoom = 0;
    double validFrom = 0;          // oldest frame whose displayed row is current
    std::vector<int32_t> lo, hi;
    double stepK = 1, stepC = 0;   // the screen move of every fan since the last takeFanStep: x_old = stepC + stepK·x_new
    std::unique_ptr<LayerRing> layer;   // ★ V12 the full-capture layer (wf_wide.h) — null until the first SPWB
    // ★★ THE ZOOM EASE (wfRowPlace.ts fanAfterRow): the geometry the newest rows are drawn at while a zoom eases in, its
    //    start, the rows done / the rows it takes, and each slot's own span (a jump moves rows by Hz in their scale).
    double dLo = NAN, dSpan = NAN, eLo0 = NAN, eSpan0 = NAN;
    int easeLeft = 0, eJ = 0, eT = 0;
    std::vector<double> span;
    double jumpHz = 0;     // ★ the Hz the LAST row's jump moved the history by (0 = not a jump) — the scale stamps follow (wf_stamps.h)
    double rowRate = 30;   // ★ rows/s the waterfall runs at — the ease is by TIME (zoomEaseRows / tuneEaseRows); the engine keeps it current
    explicit FanState(int rows = 0) : xfA(rows, 1.0), xfB(rows, 0.0), span(rows, 0.0) {}
};

// ★★★ THE EASE IS BY TIME at the running row rate — the twin of src/utils/wfRowPlace.ts zoomEaseSec / tuneEaseSec (the
//     note there: a ×√2 key was TWO rows on an ease-out — a stair-step with a hairline join; Stuart 2026-10-29 "the
//     lines are simply too sharp"). 0.33 s per ×2, 0.25 s minimum for a key or detent, 0.5 s cap; a pinch's < 0.2 octave
//     steps proportional. A tune 0.25 s, up to 0.35 s for most of the view. Change one, change both.
constexpr double ZOOM_EASE_S_PER_OCTAVE = 0.33, ZOOM_EASE_MIN_S = 0.25, ZOOM_EASE_MAX_S = 0.5;
constexpr double TUNE_EASE_S = 0.25, TUNE_EASE_MAX_S = 0.35, EASE_ROW_RATE = 30;
inline double zoomEaseSec(double k) {
    const double oct = std::fabs(std::log2(k));
    if (!(oct > 0)) return 0;
    const double t = oct * ZOOM_EASE_S_PER_OCTAVE;
    return oct < 0.2 ? t : std::max(ZOOM_EASE_MIN_S, std::min(ZOOM_EASE_MAX_S, t));
}
inline double tuneEaseSec(double frac) { return TUNE_EASE_S + (TUNE_EASE_MAX_S - TUNE_EASE_S) * std::min(1.0, std::max(0.0, frac)); }
inline int zoomEaseRows(double k, double rowRate = EASE_ROW_RATE) {
    const double r = rowRate > 0 ? rowRate : EASE_ROW_RATE;
    return std::max(1, (int)std::floor(zoomEaseSec(k) * r + 0.5));
}
inline int tuneEaseRows(double frac, double rowRate = EASE_ROW_RATE) {
    const double r = rowRate > 0 ? rowRate : EASE_ROW_RATE;
    return std::max(2, (int)std::floor(tuneEaseSec(frac) * r + 0.5));
}
inline const int ZOOM_EASE_ROWS = zoomEaseRows(2);   // a ×2 step at the default rate (the tests' reference)
constexpr double TUNE_EASE_MIN_FRAC = 2.0 / 1024;   // every step past two columns eases (Stuart, build 511: "still slicing")

/** THE EASE's geometry at u (wfRowPlace.ts easeView — steady with a soft landing, the span geometric about the fixed point). */
inline void easeView(double lo0, double span0, double lo1, double span1, double u, double& L, double& S) {
    const double x = u < 0 ? 0 : u > 1 ? 1 : u, e = x * (1.5 - 0.5 * x);
    S = span0 * std::pow(span1 / span0, e);
    const double ds = span0 - span1;
    if (std::fabs(ds) <= span1 * 1e-9) { L = lo0 + (lo1 - lo0) * e; return; }
    const double xs = (lo1 - lo0) / ds;
    L = lo0 + span0 * xs - S * xs;
}
// ★★★ THE SLOPE CAP — the twin of wfRowPlace.ts cappedEaseRows (Stuart 2026-10-29: "higher step rates turn into sharp
//     lines with no detail" / "same with zooming"): an ease is at least as long as keeps every on-screen move under
//     SLOPE_CAP_FRAC of the view a row, up to EASE_CEIL_S. A step the cap does not bind is exactly what it was.
constexpr double SLOPE_CAP_FRAC = 1.0 / 256, EASE_CEIL_S = 1.0;
inline double viewMoveFrac(double L0, double S0, double L1, double S1) {
    if (!(S0 > 0 && S1 > 0)) return 0;
    return std::max(std::fabs((L0 - L1) / S1), std::fabs((L0 + S0 - L1) / S1 - 1));
}
inline int cappedEaseRows(double lo0, double span0, double lo1, double span1, int base, double rowRate = EASE_ROW_RATE) {
    const double r = rowRate > 0 ? rowRate : EASE_ROW_RATE;
    const int ceilR = std::max(base, (int)std::floor(EASE_CEIL_S * r + 0.5));
    if (!(span0 > 0 && span1 > 0) || !std::isfinite(lo0) || !std::isfinite(lo1)) return base;
    int N = std::max(1, base);
    for (int i = 0; i < 6 && N < ceilR; i++) {
        double L, S; easeView(lo0, span0, lo1, span1, 1.0 / N, L, S);
        const double d = viewMoveFrac(lo0, span0, L, S);
        if (!(d > SLOPE_CAP_FRAC * 1.0001)) break;
        N = std::min(ceilR, (int)std::ceil(N * d / SLOPE_CAP_FRAC));
    }
    return std::max(base, N);
}

/** Per output pixel, the stored bins [lo, hi] it shows under (a, b); lo = -1 = blank. */
inline void fanTable(double a, double b, int n, int32_t* lo, int32_t* hi) {
    const double bn = b * n;
    if (a <= 1) {
        for (int x = 0; x < n; x++) {
            const double c = std::floor(a * (x + 0.5) + bn);
            if (c >= 0 && c < n) { lo[x] = hi[x] = (int32_t)c; } else { lo[x] = hi[x] = -1; }
        }
        return;
    }
    for (int x = 0; x < n; x++) {
        const double l = std::max(0.0, std::floor(a * x + bn)), h = std::min((double)(n - 1), std::ceil(a * (x + 1) + bn) - 1);
        if (l <= h) { lo[x] = (int32_t)l; hi[x] = (int32_t)h; } else { lo[x] = hi[x] = -1; }
    }
}

/** Regenerate the displayed rows of the newest `count` frames from the stored ring through their maps. */
inline void fanRegen(FanState& st, const uint8_t* ring, int n, int rows, double frames, int count) {
    uint8_t* disp = st.disp.data();
    if ((int)st.lo.size() < n) { st.lo.resize(n); st.hi.resize(n); }
    int32_t* lo = st.lo.data(); int32_t* hi = st.hi.data();
    const int k = (int)std::min({(double)count, frames, (double)rows});
    st.validFrom = frames - k;
    double ta = NAN, tb = NAN;
    for (int j = 1; j <= k; j++) {
        const int r = (int)std::fmod(frames - j, (double)rows);
        const double a = st.xfA[r], b = st.xfB[r];
        const size_t at = (size_t)r * n;
        if (a == 1 && b == 0) { std::memcpy(disp + at, ring + at, n); continue; }
        if (!(a == ta && b == tb)) { fanTable(a, b, n, lo, hi); ta = a; tb = b; }
        const uint8_t* s = ring + at; uint8_t* d = disp + at;
        if (a <= 1) { for (int x = 0; x < n; x++) { const int32_t l = lo[x]; d[x] = l < 0 ? 0 : s[l]; } }
        else {
            for (int x = 0; x < n; x++) {
                const int32_t l = lo[x];
                if (l < 0) { d[x] = 0; continue; }
                uint8_t m = s[l];
                for (int32_t q = l + 1; q <= hi[x]; q++) if (s[q] > m) m = s[q];
                d[x] = m;
            }
        }
        // ★★ V12: outside the detailed extent, the row's own full-capture layer through the same map (wf_wide.h)
        if (st.layer) layerFillRow(*st.layer, r, a, b, n, d, lo);
    }
}

/** ONE displayed row from the stored ring through its map; where an eased row has no data it continues the row below
 *  as drawn, then the layer (if any) draws the capture there. No-op for an identity row. */
inline void fanRow(FanState& st, const uint8_t* ring, int n, int r, int rows = 0) {
    const double a = st.xfA[r], b = st.xfB[r];
    if (st.disp.empty() || (a == 1 && b == 0)) return;
    if ((int)st.lo.size() < n) { st.lo.resize(n); st.hi.resize(n); }
    int32_t* lo = st.lo.data(); int32_t* hi = st.hi.data();
    fanTable(a, b, n, lo, hi);
    uint8_t* disp = st.disp.data();
    const size_t at = (size_t)r * n;
    const uint8_t* s = ring + at; uint8_t* d = disp + at;
    const uint8_t* pv = rows > 0 ? disp + (size_t)((r + rows - 1) % rows) * n : nullptr;
    for (int x = 0; x < n; x++) {
        const int32_t l = lo[x];
        if (l < 0) { d[x] = pv ? pv[x] : 0; continue; }
        uint8_t m = s[l];
        for (int32_t q = l + 1; q <= hi[x]; q++) if (s[q] > m) m = s[q];
        d[x] = m;
    }
    if (st.layer) layerFillRow(*st.layer, r, a, b, n, d, lo);
}

/** ★★★ A JUMP KEEPS THE HISTORY AS DRAWN (Stuart, 2026-10-30, #57: "can we preserve the history on longer tunes and zooms so
 *  it is not lost") — the twin of wfRowPlace.ts JUMP_KEEPS_HISTORY: no re-map; a scale stamp marks the boundary, and a tap
 *  tunes by the row's own scale (SpectrumEngine::rowGeom). The re-map is kept behind the switch. */
constexpr bool JUMP_KEEPS_HISTORY = true;

/** BEFORE a row placed at (lo, span) Hz goes in: a zoom starts the ease; a same-span JUMP moves the history. */
inline bool fanNoteRow(FanState& st, const uint8_t* ring, size_t ringLen, int n, int rows, double frames, int visible,
                       double lo, double span) {
    st.jumpHz = 0;
    if (!(span > 0)) return false;
    if (!std::isfinite(lo)) {
        if (!(st.lastSpan > 0)) return false;
        lo = st.lastLo + (st.lastSpan - span) / 2;
    }
    bool fanned = false;
    if (frames > 0 && st.lastSpan > 0 && spanChanged(st.lastSpan, span)) {
        if (!(st.easeLeft > 0)) { st.dLo = st.lastLo; st.dSpan = st.lastSpan; }
        st.eLo0 = st.dLo; st.eSpan0 = st.dSpan; st.eJ = 0;
        st.eT = st.easeLeft = std::max(cappedEaseRows(st.dLo, st.dSpan, lo, span, zoomEaseRows(span / st.dSpan, st.rowRate), st.rowRate),
                                        st.easeLeft > 0 ? st.easeLeft : 0);   // ★★★ + the slope cap
    }
    // ★★ A tune past MOTION_MAX_FRAC but within the view EASES like a zoom (the JS twin, wfRowPlace.ts — Stuart 2026-10-28:
    //    "the tuning still looks a little sliced up … blended like the zoom"); only a move of a whole view or more is a JUMP.
    const double moved = std::fabs(lo - st.lastLo);
    if (frames > 0 && st.lastSpan > 0 && !spanChanged(st.lastSpan, span) && moved > st.lastSpan * TUNE_EASE_MIN_FRAC && moved < st.lastSpan) {
        if (!(st.easeLeft > 0)) { st.dLo = st.lastLo; st.dSpan = st.lastSpan; }
        st.eLo0 = st.dLo; st.eSpan0 = st.dSpan; st.eJ = 0;
        st.eT = st.easeLeft = std::max(cappedEaseRows(st.dLo, st.dSpan, lo, span, tuneEaseRows(moved / st.lastSpan, st.rowRate), st.rowRate),
                                        st.easeLeft > 0 ? st.easeLeft : 0);   // ★★★ + the slope cap
    }
    if (!JUMP_KEEPS_HISTORY && frames > 0 && st.lastSpan > 0 && !spanChanged(st.lastSpan, span) && moved >= st.lastSpan) {
        const double k = span / st.lastSpan, c = (lo - st.lastLo) / st.lastSpan;
        st.stepC += st.stepK * c; st.stepK *= k;   // x0 = c1 + k1·(c2 + k2·x2)
        st.easeLeft = 0;
        // every row moves by the jump's Hz in ITS own scale (rows keep the zoom they were drawn at)
        const double dHz = lo - st.lastLo;
        st.jumpHz = dHz;
        bool zoomed = false;
        for (size_t r = 0; r < st.xfA.size(); r++) {
            const double sp = st.span[r] > 0 ? st.span[r] : span;
            double nb = st.xfB[r] + dHz / sp;
            if (st.xfA[r] == 1 && std::fabs(nb) < 1e-9) nb = 0; else zoomed = true;
            st.xfB[r] = nb;
        }
        if (zoomed) {
            if (st.disp.empty()) st.disp.assign(ringLen, 0);
            fanRegen(st, ring, n, rows, frames, visible);
            st.sinceZoom = 0;
        } else { st.disp.clear(); st.disp.shrink_to_fit(); }
        fanned = true;
    }
    st.lastLo = lo; st.lastSpan = span;
    return fanned;
}

/** AFTER the row at `slot` is written: the identity, copied to the display. */
inline void fanAfterRow(FanState& st, const uint8_t* ring, int slot, int n, int rows) {
    st.span[slot] = st.lastSpan;
    if (st.easeLeft > 0 && st.lastSpan > 0 && st.eSpan0 > 0 && std::isfinite(st.eLo0)) {
        const double tl = st.lastLo, ts = st.lastSpan;
        st.eJ++; st.easeLeft = st.eT - st.eJ;
        const double u = std::min(1.0, (double)st.eJ / st.eT), e = u * (1.5 - 0.5 * u);   // steady, a soft landing (wfRowPlace.ts easeView)
        const double ns = st.eSpan0 * std::pow(ts / st.eSpan0, e);
        const double ds = st.eSpan0 - ts;
        double nl;
        if (std::fabs(ds) > ts * 1e-9) { const double xs = (tl - st.eLo0) / ds; nl = st.eLo0 + st.eSpan0 * xs - ns * xs; }
        else nl = st.eLo0 + (tl - st.eLo0) * e;
        st.dLo = nl; st.dSpan = ns;
        if (st.easeLeft > 0) {
            st.xfA[slot] = ns / ts; st.xfB[slot] = (nl - tl) / ts;
            if (st.disp.empty()) st.disp.assign(ring, ring + (size_t)(rows + motionImageRows(rows, n)) * n);
            fanRow(st, ring, n, slot, rows);
            st.sinceZoom = 0;
            return;
        }
    } else st.easeLeft = 0;
    st.xfA[slot] = 1; st.xfB[slot] = 0;
    if (st.disp.empty()) return;
    const size_t at = (size_t)slot * n;
    std::memcpy(st.disp.data() + at, ring + at, n);
    if (++st.sinceZoom >= rows) { st.disp.clear(); st.disp.shrink_to_fit(); }
}

/** The screen move the fans made since the last call (x_old = c + k·x_new), reset to none. */
inline void takeFanStep(FanState& st, double& k, double& c) { k = st.stepK; c = st.stepC; st.stepK = 1; st.stepC = 0; }

/** The bytes to make the waterfall image from: the displayed ring with the motion rows in its tail (the buffers are
 *  n × (rows + motionImageRows(rows, n)); the image is made that tall). */
inline const uint8_t* wfImageBytes(FanState& st, const MotionState& ms, uint8_t* ring, int n, int rows) {
    uint8_t* b = st.disp.empty() ? ring : st.disp.data();
    std::memcpy(b + (size_t)rows * n, ms.bytes.data(), ms.bytes.size());
    return b;
}

/** pushRow's whole placement (WaterfallView's placeRow): slide onto the view, fan on a span change or a jump, keep the
 *  display in step, note the placement and the move from the row before (MOTION). `ring` is n × (rows + motionImageRows).
 *  `frames` = rows written before this one; `startOut` receives column 0's Hz (the layer's placement). Returns the slot. */
inline int placeRow(uint8_t* ring, int rows, int n, double frames, const uint8_t* row, double rowCenterHz, double rowHzPerBin,
                    double viewCenterHz, double viewBwHz, MotionState& motion, FanState& fan, int visible = -1,
                    double* startOut = nullptr) {
    if (visible < 0) visible = rows;
    const int slot = (int)std::fmod(frames, (double)rows);
    double off = 0;
    if (rowHzPerBin > 0 && rowCenterHz > 0 && viewCenterHz > 0 && viewBwHz > 0 && n > 0) {   // rowViewOffset
        const double viewHzPerBin = viewBwHz / n;
        if (std::fabs(rowHzPerBin - viewHzPerBin) <= viewHzPerBin * 1e-6) {
            off = std::floor((rowCenterHz - viewCenterHz) / rowHzPerBin + 0.5);   // Math.round
            if (std::fabs(off) >= n) off = 0;
        }
    }
    const double start = placedStartHz(rowCenterHz, rowHzPerBin, off, n);
    if (startOut) *startOut = start;   // ★ V12: where the layer row goes (its k and m — wf_wide.h)
    const bool fanned = rowHzPerBin > 0
        && fanNoteRow(fan, ring, (size_t)(rows + motionImageRows(rows, n)) * n, n, rows, frames, visible, start, rowHzPerBin * n);
    uint8_t* dst = ring + (size_t)slot * n;
    if (off == 0) {
        std::memcpy(dst, row, n);
    } else {                                                                  // writeRow: the vacated edge = floor
        std::memset(dst, 0, n);
        const int o = (int)off;
        if (o > 0) std::memcpy(dst + o, row, n - o);
        else       std::memcpy(dst, row - o, n + o);
    }
    fanAfterRow(fan, ring, slot, n, rows);
    motionAfterRow(motion, fan.xfA.data(), fan.xfB.data(), n, rows, frames, start, rowHzPerBin, fanned);
    return slot;
}

}  // namespace vibewf
