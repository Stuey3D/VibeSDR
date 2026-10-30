// wf_motion.h — the waterfall's MOTION metadata: each ring slot's placement and how far its trail moves to the next newer
// row, for the shader's motion-compensated interpolation. A LINE-FOR-LINE port of the MOTION section of
// src/utils/wfRowPlace.ts (the app's JS path — the full design note is there); test_wf_motion.cpp runs the same cases as
// the motion section of scripts/test_wf_row_place.ts. Change one, change both.
//
// ★★ Stuart, 2026-10-28: "I just wanted the chunky broken diagonal lines joined up so they looked solid" (the first native
//    build's MW sweep is the reference). A ring row is one display line, and a tune moves a station several columns
//    between rows, so the trail was dashes. Each slot now carries h (DISPLAY columns its trail moves to the next row, from
//    the two rows' own placement maps — a view pan over a fixed capture and a tuner that moves alike), stored as 16 bits
//    in two extra IMAGE rows below the ring, and the shader samples frame f at x − t·h and f+1 at x + (1−t)·h between them.
//    Nothing stored is altered; steady rows (h = 0) draw exactly as before. This REPLACES the glide streak (a moved row's
//    signals max-smeared back to the previous row's place — "smeared", rejected).
//  ★ A move past MOTION_MAX_FRAC of the width is a JUMP: not interpolated — the fan re-maps the history (wf_zoom_fan.h).
//  ★★ A ZOOM IS MOTION TOO (2026-10-28 — Stuart: "a thin line signal should expand out to the width and the history
//    remains the thin line"): the history keeps the zoom it was drawn at, so each slot also carries g, the change of scale
//    to the next row (two more image rows), and the shader draws the fan between them. h is the move at the CENTRE.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vibewf {

constexpr int MOTION_ROWS = 4;          // motion PLANES below the ring: h high/low byte, then g (log2) high/low byte
// ★★ A PLANE IS ceil(rows / n) IMAGE ROWS (2026-10-30, the growing ring — wf_ring_grow.h; the twin of wfRowPlace.ts
//    motionImageRows / motionAt): a ring of up to n (1024) rows keeps the old layout (plane p = image row rows + p,
//    column = slot); 2048 / 4096 rows wrap each plane onto 2 / 4 image rows (slot r at column r % n, row r / n).
inline int motionImageRows(int rows, int n) { return MOTION_ROWS * std::max(1, (rows + n - 1) / n); }
inline size_t motionAt(int rows, int n, int p, int r) {
    const int k = std::max(1, (rows + n - 1) / n);
    return (size_t)(p * k + r / n) * n + (size_t)(r % n);
}
constexpr int ZOOM_SCALE = 4096;        // log2(g) in 1/4096 of an octave
constexpr int MOTION_SCALE = 64;        // 1/64 column
constexpr int MOTION_ZERO = 32768;
constexpr double MOTION_MAX_FRAC = 1.0 / 8;

/** True when two rows' bin scales differ enough to count as a zoom (not float noise). */
inline bool spanChanged(double span0, double span1) {
    return span0 > 0 && span1 > 0 && std::fabs(span1 - span0) > span0 * 1e-6;
}

/** Column c of the WRITTEN row is startHz + c × hzPerCol; NaN when the row's centre/scale is unknown. */
inline double placedStartHz(double rowCenterHz, double rowHzPerBin, double off, int n) {
    if (!(rowCenterHz > 0 && rowHzPerBin > 0 && n > 0)) return NAN;
    return rowCenterHz - (off + n / 2.0) * rowHzPerBin;
}

struct MotionState {
    std::vector<double> startHz, hzPerCol;   // per slot (NaN start = unknown)
    std::vector<float> h;                    // per slot: display columns to the next newer row, at the centre
    std::vector<float> g;                    // per slot: the zoom to the next newer row (1 = none)
    std::vector<uint8_t> bytes;              // motionImageRows(rows, n) × n — the image's tail rows (motionAt)
    double lo = 1, hi = 0;                   // absolute frames whose pair moves; lo > hi = none
    int rows = 0, n = 0;                     // the ring's slots and width the planes are laid out for
    MotionState(int rows_ = 0, int n_ = 0) : startHz(rows_, NAN), hzPerCol(rows_, 0.0), h(rows_, 0.f), g(rows_, 1.f),
        bytes(n_ > 0 ? (size_t)motionImageRows(rows_, n_) * n_ : 0), rows(rows_), n(n_) {
        const size_t pl = bytes.size() / MOTION_ROWS;   // one plane
        std::fill(bytes.begin(), bytes.begin() + pl, (uint8_t)(MOTION_ZERO >> 8));
        std::fill(bytes.begin() + pl, bytes.begin() + 2 * pl, (uint8_t)(MOTION_ZERO & 255));
        std::fill(bytes.begin() + 2 * pl, bytes.begin() + 3 * pl, (uint8_t)(MOTION_ZERO >> 8));
        std::fill(bytes.begin() + 3 * pl, bytes.end(), (uint8_t)(MOTION_ZERO & 255));
    }
};

inline int encodeMotion(double h) {
    const double v = std::floor(h * MOTION_SCALE + 0.5) + MOTION_ZERO;   // Math.round
    return (int)std::max(0.0, std::min(65535.0, v));
}
inline double decodeMotion(int hi, int lo) { return ((hi * 256 + lo) - MOTION_ZERO) / (double)MOTION_SCALE; }
inline int encodeZoom(double g) {
    const double v = std::floor(std::log2(g) * ZOOM_SCALE + 0.5) + MOTION_ZERO;
    return (int)std::max(0.0, std::min(65535.0, v));
}
inline double decodeZoom(int hi, int lo) { return std::exp2(((hi * 256 + lo) - MOTION_ZERO) / (double)ZOOM_SCALE); }

/** Slot r's display map: screen column x shows P + Q·x Hz. False when its placement is unknown. */
inline bool displayMap(const MotionState& ms, const double* xfA, const double* xfB, int r, int n, double& P, double& Q) {
    const double s = ms.startHz[r], hz = ms.hzPerCol[r];
    if (!std::isfinite(s) || !(hz > 0)) return false;
    P = s + hz * xfB[r] * n; Q = hz * xfA[r];
    return true;
}

/** How a trail moves from slot a (older) to slot b (the next newer): a station at display column x in a is at
 *  m + g·(x − m) + h in b (m = n/2). g = 1, h = 0: steady / unknown / a jump. A tune is h alone (≤ MOTION_MAX_FRAC); a
 *  zoom is g (h = the centre's move, ≤ ±511 columns or it is drawn as a step). */
inline void pairMotion(const MotionState& ms, const double* xfA, const double* xfB, int a, int b, int n, double& g, double& h) {
    g = 1; h = 0;
    double Pa, Qa, Pb, Qb;
    if (!displayMap(ms, xfA, xfB, a, n, Pa, Qa) || !displayMap(ms, xfA, xfB, b, n, Pb, Qb)) return;
    const double m = n / 2.0, gg = Qa / Qb, c = (Pa - Pb) / Qb;
    if (!std::isfinite(gg) || !std::isfinite(c)) return;
    if (std::fabs(gg - 1) <= 1e-3) {
        if (std::fabs(c) < 1.0 / MOTION_SCALE || std::fabs(c) > n * MOTION_MAX_FRAC) return;
        h = c; return;
    }
    if (gg < 1.0 / 256 || gg > 256) return;
    const double hc = c + (gg - 1) * m;
    if (std::fabs(hc) > 511) return;
    g = gg; h = std::fabs(hc) < 1.0 / MOTION_SCALE ? 0 : hc;
}

inline void setMotion(MotionState& ms, int slot, double g, double h, int n) {
    ms.h[slot] = (float)h; ms.g[slot] = (float)g;
    const int v = encodeMotion(h), z = encodeZoom(g);
    uint8_t* b = ms.bytes.data();
    if (ms.rows <= n) {   // ≤ 1024 rows: the original layout
        b[slot] = (uint8_t)(v >> 8); b[n + slot] = (uint8_t)(v & 255);
        b[2 * n + slot] = (uint8_t)(z >> 8); b[3 * n + slot] = (uint8_t)(z & 255);
        return;
    }
    const int R = ms.rows;
    b[motionAt(R, n, 0, slot)] = (uint8_t)(v >> 8); b[motionAt(R, n, 1, slot)] = (uint8_t)(v & 255);
    b[motionAt(R, n, 2, slot)] = (uint8_t)(z >> 8); b[motionAt(R, n, 3, slot)] = (uint8_t)(z & 255);
}
inline bool moves(double g, double h) { return g != 1 || h != 0; }

/** Every pair among the newest `rows` frames, and the moving range (after a fan re-mapped them all). */
inline void motionRecompute(MotionState& ms, const double* xfA, const double* xfB, int n, int rows, double frames) {
    ms.lo = 1; ms.hi = 0;
    for (double f = std::max(0.0, frames - rows); f < frames; f++) {
        const int r = (int)std::fmod(f, (double)rows);
        double g = 1, h = 0;
        if (f + 1 < frames) pairMotion(ms, xfA, xfB, r, (int)std::fmod(f + 1, (double)rows), n, g, h);
        setMotion(ms, r, g, h, n);
        if (moves(g, h)) { if (ms.lo > ms.hi) ms.lo = f; ms.hi = f; }
    }
}

/** AFTER frame f is written into its slot at (startHz, hzPerCol): the pair (f−1 → f), this slot cleared, the range. */
inline void motionAfterRow(MotionState& ms, const double* xfA, const double* xfB, int n, int rows, double f,
                           double startHz, double hzPerCol, bool refanned) {
    const int slot = (int)std::fmod(f, (double)rows);
    ms.startHz[slot] = std::isfinite(startHz) && hzPerCol > 0 ? startHz : NAN;
    ms.hzPerCol[slot] = hzPerCol > 0 ? hzPerCol : 0;
    if (refanned) { motionRecompute(ms, xfA, xfB, n, rows, f + 1); return; }
    setMotion(ms, slot, 1, 0, n);
    if (f > 0) {
        const int p = (int)std::fmod(f - 1, (double)rows);
        double g, h;
        pairMotion(ms, xfA, xfB, p, slot, n, g, h);
        setMotion(ms, p, g, h, n);
        if (moves(g, h)) { if (ms.lo > ms.hi) ms.lo = f - 1; ms.hi = f - 1; }
    }
    const double oldest = f + 1 - rows;
    if (ms.hi < oldest) { ms.lo = 1; ms.hi = 0; } else if (ms.lo < oldest) ms.lo = oldest;
}

}  // namespace vibewf
