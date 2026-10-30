// VibeSDR — SCALE STAMPS on the waterfall history: the twin of src/utils/wfStamps.ts (read the notes there; change one,
// change both). Stuart, 2026-10-30 (#57): "zoom in and a stamp of the previous frequency scale as it was is added onto the
// old waterfall position and it gently drops off the bottom of the screen, same with tuning too if needed".
// ★ The engine only DETECTS (each row's displayed geometry as it is written, wf_motion.h displayMap) and publishes the
//   list (UiUpdate.stamps); WaterfallView draws them (WfStampLayer) from the same uniforms that scroll the picture.
// ★ One stamp per move, when it LANDS (a rapid run = one, the pre-run scale); a long tune/pan sweep one now and then
//   (sweepRows; never a zoom run); never crowded (3 at most; one within minGapRows of the last REPLACES it).
#pragma once
#include <cmath>
#include <vector>
#include <algorithm>

namespace vibewf {

constexpr int    STAMP_MAX = 3;
constexpr double STAMP_SETTLE_S = 0.5;
constexpr int    STAMP_MIN_ROWS = 3;
constexpr double STAMP_ZOOM_FRAC = 0.02;
constexpr double STAMP_SHIFT_FRAC = 1.0 / 20;
constexpr double STAMP_SAME_FRAC = 2e-4;

struct WfStamp { long id = 0; double frame = 0, lo = 0, span = 0, born = 0; };

struct StampState {
    double refLo = NAN, refSpan = NAN, refFrame = -1;
    bool moving = false; double candLo = NAN, candSpan = NAN, candFrom = -1;
    std::vector<WfStamp> stamps;   // newest first
    long nextId = 1;
    unsigned gen = 0;
    int minGapRows = 45;
    int sweepRows = 0;
    double moveFrom = -1, runStampFrame = -1; bool preDone = false, runZoomed = false;
};

inline int stampSettleRows(double rowRate) {
    return std::max(STAMP_MIN_ROWS, (int)std::ceil((rowRate > 0 ? rowRate : 30) * STAMP_SETTLE_S));
}

inline bool stampSame(double lo0, double span0, double lo1, double span1) {
    const double tol = std::max(span0, span1) * STAMP_SAME_FRAC;
    return std::fabs(lo0 - lo1) <= tol && std::fabs((lo0 + span0) - (lo1 + span1)) <= tol;
}

inline bool stampMeaningful(double lo0, double span0, double lo1, double span1) {
    if (!(span0 > 0 && span1 > 0)) return false;
    if (std::fabs(span0 - span1) > span1 * STAMP_ZOOM_FRAC) return true;
    const double edge = std::max(std::fabs(lo0 - lo1), std::fabs((lo0 + span0) - (lo1 + span1)));
    return edge > span1 * STAMP_SHIFT_FRAC;
}

inline void stampAdd(StampState& st, double frame, double lo, double span, double born) {
    if (!st.stamps.empty() && frame - st.stamps.front().frame < st.minGapRows) st.stamps.erase(st.stamps.begin());
    st.stamps.insert(st.stamps.begin(), WfStamp{ st.nextId++, frame, lo, span, born });
    if ((int)st.stamps.size() > STAMP_MAX) st.stamps.resize(STAMP_MAX);
    st.gen++;
}

inline void stampReset(StampState& st) {
    const bool had = !st.stamps.empty();
    st.refLo = st.refSpan = NAN; st.refFrame = -1; st.moving = false; st.candLo = st.candSpan = NAN; st.candFrom = -1;
    st.stamps.clear();
    if (had) st.gen++;
}

/** AFTER row `frame` is written, displayed at (lo, span). Returns true when the stamps changed. */
inline bool stampNoteRow(StampState& st, double frame, double lo, double span, int settleRows) {
    if (!(span > 0) || !std::isfinite(lo)) return false;
    if (!(st.refSpan > 0)) { st.refLo = lo; st.refSpan = span; st.refFrame = frame; st.moving = false; return false; }
    if (!st.moving) {
        if (stampSame(lo, span, st.refLo, st.refSpan)) { st.refFrame = frame; return false; }
        st.moving = true; st.candLo = lo; st.candSpan = span; st.candFrom = frame;
        st.moveFrom = frame; st.runStampFrame = -1; st.preDone = false;
        st.runZoomed = std::fabs(span - st.refSpan) > st.refSpan * STAMP_ZOOM_FRAC;
        return false;
    }
    if (std::fabs(span - st.refSpan) > st.refSpan * STAMP_ZOOM_FRAC) st.runZoomed = true;
    if (st.sweepRows > 0 && !st.runZoomed && frame - std::max(st.moveFrom, st.runStampFrame) >= st.sweepRows
        && stampMeaningful(st.refLo, st.refSpan, lo, span)) {
        if (!st.preDone && st.refFrame >= 0) stampAdd(st, st.refFrame, st.refLo, st.refSpan, frame);
        stampAdd(st, frame, lo, span, frame);
        st.preDone = true; st.runStampFrame = frame;
        if (!stampSame(lo, span, st.candLo, st.candSpan)) { st.candLo = lo; st.candSpan = span; st.candFrom = frame; }
        return true;
    }
    if (!stampSame(lo, span, st.candLo, st.candSpan)) { st.candLo = lo; st.candSpan = span; st.candFrom = frame; return false; }
    if (frame - st.candFrom + 1 < settleRows) return false;
    st.moving = false;
    bool changed = false;
    if (!st.preDone && stampMeaningful(st.refLo, st.refSpan, lo, span) && st.refFrame >= 0) {
        stampAdd(st, st.refFrame, st.refLo, st.refSpan, frame);
        changed = true;
    }
    st.preDone = false; st.runZoomed = false; st.runStampFrame = -1;
    st.refLo = lo; st.refSpan = span; st.refFrame = frame;
    return changed;
}

/** A jump moved every row by dHz: the stamps describe what is drawn, so they move with it. */
inline bool stampJump(StampState& st, double dHz) {
    if (!(dHz != 0) || !std::isfinite(dHz)) return false;
    if (st.refSpan > 0) st.refLo += dHz;
    if (st.moving && st.candSpan > 0) st.candLo += dHz;
    if (st.stamps.empty()) return false;
    for (auto& s : st.stamps) s.lo += dHz;
    st.gen++;
    return true;
}

inline bool stampPrune(StampState& st, double newest, int keepRows) {
    size_t n = st.stamps.size();
    while (n > 0 && newest - st.stamps[n - 1].frame > keepRows) n--;
    if (n == st.stamps.size()) return false;
    st.stamps.resize(n);
    st.gen++;
    return true;
}

}  // namespace vibewf
