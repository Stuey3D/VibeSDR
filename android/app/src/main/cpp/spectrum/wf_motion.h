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
//  ★ A zoom is never motion: the fan re-maps every row to the current scale, so h across a zoom step is 0.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vibewf {

constexpr int MOTION_ROWS = 2;          // image rows below the ring: high byte, low byte of each slot's h
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
    std::vector<float> h;                    // per slot: display columns to the next newer row
    std::vector<uint8_t> bytes;              // MOTION_ROWS × n — the image's tail rows
    double lo = 1, hi = 0;                   // absolute frames whose pair moves; lo > hi = none
    MotionState(int rows = 0, int n = 0) : startHz(rows, NAN), hzPerCol(rows, 0.0), h(rows, 0.f), bytes((size_t)MOTION_ROWS * n) {
        for (int r = 0; r < n; r++) { bytes[r] = MOTION_ZERO >> 8; bytes[n + r] = MOTION_ZERO & 255; }
    }
};

inline int encodeMotion(double h) {
    const double v = std::floor(h * MOTION_SCALE + 0.5) + MOTION_ZERO;   // Math.round
    return (int)std::max(0.0, std::min(65535.0, v));
}
inline double decodeMotion(int hi, int lo) { return ((hi * 256 + lo) - MOTION_ZERO) / (double)MOTION_SCALE; }

/** Slot r's display map: screen column x shows P + Q·x Hz. False when its placement is unknown. */
inline bool displayMap(const MotionState& ms, const double* xfA, const double* xfB, int r, int n, double& P, double& Q) {
    const double s = ms.startHz[r], hz = ms.hzPerCol[r];
    if (!std::isfinite(s) || !(hz > 0)) return false;
    P = s + hz * xfB[r] * n; Q = hz * xfA[r];
    return true;
}

/** Display columns a trail moves from slot a (older) to slot b (the next newer); 0 = steady/unknown/scale/jump. */
inline double pairMotion(const MotionState& ms, const double* xfA, const double* xfB, int a, int b, int n) {
    double Pa, Qa, Pb, Qb;
    if (!displayMap(ms, xfA, xfB, a, n, Pa, Qa) || !displayMap(ms, xfA, xfB, b, n, Pb, Qb)) return 0;
    if (std::fabs(Qa / Qb - 1) > 1e-3) return 0;
    const double h = (Pa - Pb) / Qb;
    if (!std::isfinite(h) || std::fabs(h) < 1.0 / MOTION_SCALE || std::fabs(h) > n * MOTION_MAX_FRAC) return 0;
    return h;
}

inline void setMotion(MotionState& ms, int slot, double h, int n) {
    ms.h[slot] = (float)h;
    const int v = encodeMotion(h);
    ms.bytes[slot] = (uint8_t)(v >> 8); ms.bytes[n + slot] = (uint8_t)(v & 255);
}

/** Every pair among the newest `rows` frames, and the moving range (after a fan re-mapped them all). */
inline void motionRecompute(MotionState& ms, const double* xfA, const double* xfB, int n, int rows, double frames) {
    ms.lo = 1; ms.hi = 0;
    for (double f = std::max(0.0, frames - rows); f < frames; f++) {
        const int r = (int)std::fmod(f, (double)rows);
        const double h = f + 1 < frames ? pairMotion(ms, xfA, xfB, r, (int)std::fmod(f + 1, (double)rows), n) : 0;
        setMotion(ms, r, h, n);
        if (h != 0) { if (ms.lo > ms.hi) ms.lo = f; ms.hi = f; }
    }
}

/** AFTER frame f is written into its slot at (startHz, hzPerCol): the pair (f−1 → f), this slot cleared, the range. */
inline void motionAfterRow(MotionState& ms, const double* xfA, const double* xfB, int n, int rows, double f,
                           double startHz, double hzPerCol, bool refanned) {
    const int slot = (int)std::fmod(f, (double)rows);
    ms.startHz[slot] = std::isfinite(startHz) && hzPerCol > 0 ? startHz : NAN;
    ms.hzPerCol[slot] = hzPerCol > 0 ? hzPerCol : 0;
    if (refanned) { motionRecompute(ms, xfA, xfB, n, rows, f + 1); return; }
    setMotion(ms, slot, 0, n);
    if (f > 0) {
        const int p = (int)std::fmod(f - 1, (double)rows);
        const double h = pairMotion(ms, xfA, xfB, p, slot, n);
        setMotion(ms, p, h, n);
        if (h != 0) { if (ms.lo > ms.hi) ms.lo = f - 1; ms.hi = f - 1; }
    }
    const double oldest = f + 1 - rows;
    if (ms.hi < oldest) { ms.lo = 1; ms.hi = 0; } else if (ms.lo < oldest) ms.lo = oldest;
}

}  // namespace vibewf
