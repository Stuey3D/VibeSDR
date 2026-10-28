// wf_zoom_fan.h — THE ZOOM FAN for the native engine's waterfall ring: a zoom re-maps the whole history to the new view.
// A LINE-FOR-LINE port of the zoom-fan section of src/utils/wfRowPlace.ts (the app's JS path — the full design note and
// the cost trade-off are there); test_wf_glide.cpp runs the same cases as that section of scripts/test_wf_row_place.ts.
// Change one, change both.
//
// ★★★ Stuart, 2026-10-27/28: zooming left the history in horizontal SLICES — every old row kept the scale it was drawn
//     at. The hard rule (memory feedback_waterfall_core_rules): "when zooming in and out you had a cool fanning in and out
//     of the signals". The STORED ring is never rewritten; each slot carries a map u = a·x + b (screen x → the row's own
//     u), composed only when the SPAN changes (a tune leaves it alone — the bend, #57), and the DISPLAYED ring the image
//     is made from is regenerated through the maps: lossless (zoom in then out restores the edges byte for byte) and a
//     zoom out keeps one-bin carriers (max under a pixel).
//  ★ RAM: the displayed ring (RING_W × RING_ROWS = 1 MiB) is allocated at the FIRST zoom and freed once every row is back
//    at the identity (zoomed back, or RING_ROWS rows since). Maps: 16 KiB, always. CPU: one regeneration of the ring per
//    zoom step, on the engine thread; a 1 KiB copy per row while a zoomed history is on screen. GPU: nothing.
#pragma once
#include "wf_glide.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vibewf {

// spanChanged() is in wf_glide.h (the glide needs it too).

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
        if (a <= 1) { for (int x = 0; x < n; x++) { const int32_t l = lo[x]; d[x] = l < 0 ? 0 : s[l]; } continue; }
        for (int x = 0; x < n; x++) {
            const int32_t l = lo[x];
            if (l < 0) { d[x] = 0; continue; }
            uint8_t m = s[l];
            for (int32_t q = l + 1; q <= hi[x]; q++) if (s[q] > m) m = s[q];
            d[x] = m;
        }
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
    if (spanChanged(st.lastSpan, span) && frames > 0) {
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

/** AFTER the row at `slot` is written (and glided): the identity, copied to the display. */
inline void fanAfterRow(FanState& st, const uint8_t* ring, int slot, int n, int rows) {
    st.xfA[slot] = 1; st.xfB[slot] = 0;
    if (st.disp.empty()) return;
    const size_t at = (size_t)slot * n;
    std::memcpy(st.disp.data() + at, ring + at, n);
    if (++st.sinceZoom >= rows) { st.disp.clear(); st.disp.shrink_to_fit(); }
}

/** The bytes to make the waterfall image from. */
inline const uint8_t* fanImage(const FanState& st, const uint8_t* ring) { return st.disp.empty() ? ring : st.disp.data(); }

/** pushRow's whole placement (WaterfallView's placeRow): slide onto the view, fan on a span change, glide a moved row,
 *  keep the display in step. `frames` = rows written before this one. Returns the slot written. */
inline int placeRow(uint8_t* ring, int rows, int n, double frames, const uint8_t* row, double rowCenterHz, double rowHzPerBin,
                    double viewCenterHz, double viewBwHz, GlideState& glide, FanState& fan, int visible = -1) {
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
    if (rowHzPerBin > 0)
        fanNoteRow(fan, ring, (size_t)rows * n, n, rows, frames, visible, placedStartHz(rowCenterHz, rowHzPerBin, off, n), rowHzPerBin * n);
    uint8_t* dst = ring + (size_t)slot * n;
    if (off == 0) {
        std::memcpy(dst, row, n);
    } else {                                                                  // writeRow: the vacated edge = floor
        std::memset(dst, 0, n);
        const int o = (int)off;
        if (o > 0) std::memcpy(dst + o, row, n - o);
        else       std::memcpy(dst, row - o, n + o);
    }
    glideRow(dst, n, rowCenterHz, rowHzPerBin, off, glide);
    fanAfterRow(fan, ring, slot, n, rows);
    return slot;
}

}  // namespace vibewf
