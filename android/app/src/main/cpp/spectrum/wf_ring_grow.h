// wf_ring_grow.h — GROW THE ENGINE'S WATERFALL RING WITHOUT LOSING A ROW. A line-for-line twin of src/utils/wfRingGrow.ts
// (the app's JS path — the full design note is there; how big: src/utils/wfRingSize.ts, decided in JS and sent in the
// engine's config as ringRows). test_spectrum_engine.cpp runs the same "grown ≡ born big" case as scripts/test_wf_ring_grow.ts.
// Change one, change both.
//
// ★★★ Stuart, 2026-10-30: "the history only goes when it genuinely goes off screen". GROW-ONLY, never shrinks, and a growth
//     is a REMAP, never a restart (the hard rule — a resize/rotation keeps the waterfall with NO redraw): every frame the
//     old ring holds is copied from slot f % oldRows to f % newRows — the stored row, the displayed row, the motion
//     (placement, h, g; the image planes re-encoded at the new layout — wf_motion.h motionAt), the fan's maps and spans,
//     the full-capture layer. Absolute state (frames, the motion range, validFrom, the stamps) is untouched.
//  ★ On the ENGINE THREAD, between rows (SpectrumEngine::runDue), so no row is written mid-copy; the next image carries the
//    new depth to the UI (UiUpdate::ringRows), the shader's uRows switching with it.
//  ★ RAM (the trade-off, stated): 1 MiB per 1024 rows for the ring, ×2 while a zoom's displayed ring exists, plus the
//    image (the same size again, until the UI retires it) — 4 MiB / 8 MiB / +4 MiB at 4096. The 1 GB tier never grows.
#pragma once
#include "wf_motion.h"
#include "wf_zoom_fan.h"
#include "wf_wide.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace vibewf {

/** Copy the newest min(frames, oldRows) frames' `stride`-element slots of `src` (oldRows slots) into `dst` (newRows). */
template <class T>
inline void remapSlots(const std::vector<T>& src, std::vector<T>& dst, int oldRows, int newRows, double frames, size_t stride = 1) {
    for (double f = std::max(0.0, frames - oldRows); f < frames; f++) {
        const size_t o = (size_t)std::fmod(f, (double)oldRows) * stride, s = (size_t)std::fmod(f, (double)newRows) * stride;
        std::copy(src.begin() + o, src.begin() + o + stride, dst.begin() + s);
    }
}

/** Grow from oldRows to newRows slots (n bytes a row) after `frames` rows. `ring` = the stored ring buffer
 *  (n × (oldRows + motionImageRows)) or empty (nothing written yet). Everything is replaced in place. No-op unless it grows. */
inline void growRing(std::vector<uint8_t>& ring, MotionState& ms, FanState& fan, int n, int oldRows, int newRows, double frames) {
    if (!(newRows > oldRows)) return;
    const size_t len = (size_t)n * (newRows + motionImageRows(newRows, n));
    if (!ring.empty()) {
        std::vector<uint8_t> out(len, 0);
        remapSlots(ring, out, oldRows, newRows, frames, (size_t)n);
        ring.swap(out);
    }
    if (!fan.disp.empty()) {
        std::vector<uint8_t> d(len, 0);
        remapSlots(fan.disp, d, oldRows, newRows, frames, (size_t)n);
        fan.disp.swap(d);
    }
    MotionState m2(newRows, n);
    remapSlots(ms.startHz, m2.startHz, oldRows, newRows, frames);
    remapSlots(ms.hzPerCol, m2.hzPerCol, oldRows, newRows, frames);
    remapSlots(ms.h, m2.h, oldRows, newRows, frames);
    remapSlots(ms.g, m2.g, oldRows, newRows, frames);
    for (double f = std::max(0.0, frames - oldRows); f < frames; f++) {
        const int s = (int)std::fmod(f, (double)newRows);
        setMotion(m2, s, m2.g[s], m2.h[s], n);
    }
    m2.lo = ms.lo; m2.hi = ms.hi;   // absolute frames — unchanged
    ms = std::move(m2);
    std::vector<double> xfA(newRows, 1.0), xfB(newRows, 0.0), span(newRows, 0.0);
    remapSlots(fan.xfA, xfA, oldRows, newRows, frames);
    remapSlots(fan.xfB, xfB, oldRows, newRows, frames);
    remapSlots(fan.span, span, oldRows, newRows, frames);
    fan.xfA.swap(xfA); fan.xfB.swap(xfB); fan.span.swap(span);
    if (fan.layer) {
        const LayerRing& L = *fan.layer;
        auto G = std::make_unique<LayerRing>(newRows, L.F, L.C, L.P);
        G->fadeTx = L.fadeTx;
        remapSlots(L.k, G->k, oldRows, newRows, frames);
        remapSlots(L.m, G->m, oldRows, newRows, frames);
        remapSlots(L.floor, G->floor, oldRows, newRows, frames, (size_t)L.F);
        remapSlots(L.pkN, G->pkN, oldRows, newRows, frames);
        remapSlots(L.pkCell, G->pkCell, oldRows, newRows, frames, (size_t)L.P);
        remapSlots(L.pkIdx, G->pkIdx, oldRows, newRows, frames, (size_t)L.P);
        remapSlots(L.pkShape, G->pkShape, oldRows, newRows, frames, (size_t)L.P);
        fan.layer = std::move(G);
    }
}

}  // namespace vibewf
