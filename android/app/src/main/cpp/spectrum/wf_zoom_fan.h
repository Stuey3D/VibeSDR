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
    explicit FanState(int rows = 0) : xfA(rows, 1.0), xfB(rows, 0.0) {}
};

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

/** BEFORE a row placed at (lo, span) Hz goes in: a change of span fans the history. */
inline bool fanNoteRow(FanState& st, const uint8_t* ring, size_t ringLen, int n, int rows, double frames, int visible,
                       double lo, double span) {
    if (!(span > 0)) return false;
    if (!std::isfinite(lo)) {
        if (!(st.lastSpan > 0)) return false;
        lo = st.lastLo + (st.lastSpan - span) / 2;
    }
    bool fanned = false;
    if (frames > 0 && st.lastSpan > 0 && (spanChanged(st.lastSpan, span) || std::fabs(lo - st.lastLo) > st.lastSpan * MOTION_MAX_FRAC)) {
        const double k = span / st.lastSpan, c = (lo - st.lastLo) / st.lastSpan;
        st.stepC += st.stepK * c; st.stepK *= k;   // x0 = c1 + k1·(c2 + k2·x2)
        const bool zoomed = applyViewChange(st.xfA.data(), st.xfB.data(), (int)st.xfA.size(), st.lastLo, st.lastSpan, lo, span);
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
    st.xfA[slot] = 1; st.xfB[slot] = 0;
    if (st.disp.empty()) return;
    const size_t at = (size_t)slot * n;
    std::memcpy(st.disp.data() + at, ring + at, n);
    if (++st.sinceZoom >= rows) { st.disp.clear(); st.disp.shrink_to_fit(); }
}

/** The screen move the fans made since the last call (x_old = c + k·x_new), reset to none. */
inline void takeFanStep(FanState& st, double& k, double& c) { k = st.stepK; c = st.stepC; st.stepK = 1; st.stepC = 0; }

/** The bytes to make the waterfall image from: the displayed ring with the motion rows in its tail (the buffers are
 *  n × (rows + MOTION_ROWS); the image is made that tall). */
inline const uint8_t* wfImageBytes(FanState& st, const MotionState& ms, uint8_t* ring, int n, int rows) {
    uint8_t* b = st.disp.empty() ? ring : st.disp.data();
    std::memcpy(b + (size_t)rows * n, ms.bytes.data(), (size_t)MOTION_ROWS * n);
    return b;
}

/** pushRow's whole placement (WaterfallView's placeRow): slide onto the view, fan on a span change or a jump, keep the
 *  display in step, note the placement and the move from the row before (MOTION). `ring` is n × (rows + MOTION_ROWS).
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
        && fanNoteRow(fan, ring, (size_t)(rows + MOTION_ROWS) * n, n, rows, frames, visible, start, rowHzPerBin * n);
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
