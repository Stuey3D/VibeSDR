// wf_wide.h — THE FULL-CAPTURE LAYER under the native engine's waterfall history (V12, docs/v12/WATERFALL-FULL-CAPTURE.md).
// A LINE-FOR-LINE port of src/utils/wfWide.ts (the app's JS path — the full design note and the cost trade-off are there);
// test_spectrum_engine.cpp runs the same cases as scripts/test_wf_wide_app.ts and the layer section of
// scripts/test_wf_row_place.ts. Change one, change both. The web's reference is web/client/src/wfWide.ts.
//
// ★★★ Stuart, 2026-10-28: "the waterfall should be buffering outside the zoomed area so that the black doesn't show at all"
//     — and it must look like "the same waterfall, just less sharp". A VibeServer that advertises "wide":2, asked with
//     `&wide=1`, follows every 4th spectrum frame with an SPWB v2 row of the WHOLE capture (vibe_wide_row.h): its floor
//     (medians + σ) and ≤ 110 peaks (cell, level, place to 1/16, width). Every ring row gets a LAYER ROW at the same slot in
//     CAPTURE space (k = rowSpan/capSpan, m = (rowLo − capLo)/capSpan); the zoom fan's per-row map moves both together,
//     and fanRegen fills the texels outside the detailed extent from it (wf_zoom_fan.h).
//  ★ RAM: (F + 4·P + 16) bytes a slot — ~0.8 MB for 1024 rows — allocated at the FIRST SPWB only; an older server
//    allocates nothing. CPU: per row written one blend (F + C) + F × 5 + ≤ P index mappings; per row the fan regenerates,
//    one pass over the texels outside the detailed extent. GPU: nothing — the shader is unchanged (the XCover 4s).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <vector>

namespace vibewf {

constexpr uint32_t WIDE_MAGIC = 0x42575053;   // 'SPWB' little-endian
constexpr size_t WIDE_HDR = 32;
constexpr int WIDE_VERSION = 2;
constexpr double WIDE_STALE_MS = 4000;
constexpr int LAYER_PEAKS = 160;
constexpr double LAYER_FULL = 0.98;

struct WideRow {
    double centreHz = 0, spanHz = 0;
    std::vector<float> floor, spread;          // dB (NaN = no data), σ dB
    int cells = 0;
    std::vector<int32_t> pkCell; std::vector<float> pkDb; std::vector<uint8_t> pkShape;
    int nPk = 0;
};

inline bool isWideFrame(const uint8_t* p, size_t len) {
    return len >= 4 && p[0] == 0x53 && p[1] == 0x50 && p[2] == 0x57 && p[3] == 0x42;
}

/** Parse an SPWB v2 frame into `w` (dB = byte + offsetDb; a 0 byte = no data). False if it is not one. */
inline bool parseWide(const uint8_t* u8, size_t len, WideRow& w, double offsetDb = -256) {
    if (len < WIDE_HDR || !isWideFrame(u8, len) || u8[4] != WIDE_VERSION) return false;
    auto r16 = [&](size_t o) { return (int)(u8[o] | (u8[o + 1] << 8)); };
    auto r32 = [&](size_t o) { return (uint32_t)u8[o] | ((uint32_t)u8[o + 1] << 8) | ((uint32_t)u8[o + 2] << 16) | ((uint32_t)u8[o + 3] << 24); };
    const int F = r16(26), C = r16(28), P = r16(30);
    if (!F || !C || len < WIDE_HDR + 2 * (size_t)F + (size_t)P * 3) return false;
    w.floor.resize(F); w.spread.resize(F);
    for (int i = 0; i < F; i++) { const int b = u8[WIDE_HDR + i]; w.floor[i] = b == 0 ? NAN : (float)(b + offsetDb); }
    for (int i = 0; i < F; i++) w.spread[i] = (float)(u8[WIDE_HDR + F + i] / 10.0);
    w.pkCell.resize(P); w.pkDb.resize(P); w.pkShape.resize(P);
    int n = 0, cell = -1; size_t o = WIDE_HDR + 2 * (size_t)F;
    for (int i = 0; i < P; i++, o += 3) {
        const int lvl = u8[o + 1];
        cell += u8[o];
        if (lvl == 0) continue;   // an escape: advance only
        if (cell >= 0 && cell < C) { w.pkCell[n] = cell; w.pkDb[n] = (float)(lvl + offsetDb); w.pkShape[n] = u8[o + 2]; n++; }
    }
    w.nPk = n; w.cells = C;
    w.centreHz = (double)r32(14) + (double)r32(18) * 4294967296.0;
    w.spanHz = (double)r32(22);
    return true;
}

/** The floor at capture fraction x — linear between floor-bin centres, NaN where there is none. */
inline double floorAt(const float* floor, int F, double x) {
    if (F < 2) return floor[0];
    const double p = x * F - 0.5;
    int i = (int)std::floor(p); double t = p - i;
    if (i < 0) { i = 0; t = 0; } if (i >= F - 1) { i = F - 2; t = 1; }
    const double a = floor[i], b = floor[i + 1];
    return a == a ? (b == b ? a + (b - a) * t : a) : b;
}

/** The detailed rows' own dB → index mapping (spectrum_processor.cpp step 7) with the layer's floor offset. */
struct ToIdx {
    double dbMin = -120, range = 100, bright = 0, off = 0;
    ToIdx(double mn, double mx, double br, double of) : dbMin(mn), range(mx - mn > 0 ? mx - mn : 1), bright(br), off(of) {}
    int operator()(double db) const {
        const double nrm = std::max(0.0, std::min(1.0, (db + off + bright - dbMin) / range));
        const double mag = nrm * 255, CLIP = 14.97;
        return (int)std::floor((mag < CLIP ? 0 : (mag - CLIP) / (255 - CLIP) * 255) + 0.5);
    }
};

/** ★★ The floor as the detailed rows LOOK: E[idx(m + σZ)] by 5-point Gauss–Hermite. */
template <class F>
inline int floorIdx(double m, double sigma, const F& toIdx) {
    if (!(sigma > 0.05)) return toIdx(m);
    static const double Z[5] = {0, 1.3556262, -1.3556262, 2.8569700, -2.8569700};
    static const double W[5] = {0.5333333, 0.2220759, 0.2220759, 0.0112574, 0.0112574};
    double s = 0;
    for (int k = 0; k < 5; k++) s += W[k] * toIdx(m + sigma * Z[k]);
    return (int)std::floor(s + 0.5);
}

/** The median of the finite values of a[0..n) (scratch reused); NaN if fewer than 8. */
inline double medianOf(const float* a, int n, std::vector<float>& scratch) {
    scratch.clear();
    for (int i = 0; i < n; i++) if (a[i] == a[i]) scratch.push_back(a[i]);
    if (scratch.size() < 8) return NAN;
    auto mid = scratch.begin() + (long)(scratch.size() / 2);
    std::nth_element(scratch.begin(), mid, scratch.end());
    return *mid;
}

/* ── THE NEWEST PICTURE OF THE CAPTURE, and its time blend ─────────────────────────────────────────────────────────── */

struct WideState {
    bool has = false;
    double centreHz = 0, spanHz = 0;
    double at = 0, gap = 400;
    int F = 0, C = 0;
    std::vector<float> wFloor, wSpread, wCell, pFloor, pSpread, pCell, bFloor, bSpread, bCell;
    std::vector<uint8_t> wShape, pShape, bShape;
    bool hasPrev = false;
    double off = 0; bool offSet = false;
};

inline void wideArrive(WideState& st, const WideRow& row, double now, double viewLo, double viewSpan, double detP50) {
    const int F = (int)row.floor.size(), C = row.cells;
    const bool same = st.has && st.centreHz == row.centreHz && st.spanHz == row.spanHz && st.F == F && st.C == C;
    if (st.F != F) {
        st.F = F;
        for (auto* v : {&st.wFloor, &st.wSpread, &st.pFloor, &st.pSpread, &st.bFloor, &st.bSpread}) v->assign(F, 0.f);
    }
    if (st.C != C) {
        st.C = C;
        for (auto* v : {&st.wCell, &st.pCell, &st.bCell}) v->assign(C, 0.f);
        for (auto* v : {&st.wShape, &st.pShape, &st.bShape}) v->assign(C, 0);
    }
    if (same) {   // ★ from what is drawn NOW (the blend part-way)
        const bool mid = st.hasPrev;
        st.pFloor = mid ? st.bFloor : st.wFloor; st.pSpread = mid ? st.bSpread : st.wSpread;
        st.pCell = mid ? st.bCell : st.wCell; st.pShape = mid ? st.bShape : st.wShape;
        st.hasPrev = true;
        const double gap = now - st.at;
        if (gap > 0 && gap < 5000) st.gap += (gap - st.gap) * 0.3;
    } else st.hasPrev = false;
    std::copy(row.floor.begin(), row.floor.end(), st.wFloor.begin());
    std::copy(row.spread.begin(), row.spread.end(), st.wSpread.begin());
    std::fill(st.wCell.begin(), st.wCell.end(), NAN);
    for (int i = 0; i < row.nPk; i++) { st.wCell[row.pkCell[i]] = row.pkDb[i]; st.wShape[row.pkCell[i]] = row.pkShape[i]; }
    st.has = true; st.centreHz = row.centreHz; st.spanHz = row.spanHz; st.at = now;
    if (viewSpan > 0 && detP50 == detP50) {   // ★★ THE FLOOR MATCH (EMA 0.3, ±30 dB)
        const double wLo = row.centreHz - row.spanHz / 2;
        double sum = 0; int n = 0;
        for (int i = 0; i < 16; i++) {
            const double f = floorAt(row.floor.data(), F, (viewLo + (i + 0.5) / 16 * viewSpan - wLo) / row.spanHz);
            if (f == f) { sum += f; n++; }
        }
        if (n >= 8) {
            const double off = std::max(-30.0, std::min(30.0, detP50 - sum / n));
            st.off = st.offSet ? st.off + (off - st.off) * 0.3 : off;
            st.offSet = true;
        }
    }
}

inline bool wideBlend(WideState& st, double now) {
    if (!st.has || now - st.at > WIDE_STALE_MS) return false;
    const int F = st.F, C = st.C;
    const double t = st.hasPrev ? std::max(0.0, std::min(1.0, (now - st.at) / std::max(50.0, st.gap))) : 1;
    if (t >= 1) { st.bFloor = st.wFloor; st.bSpread = st.wSpread; st.bCell = st.wCell; st.bShape = st.wShape; return true; }
    for (int i = 0; i < F; i++) {
        const double a = st.pFloor[i], b = st.wFloor[i];
        st.bFloor[i] = (float)(a == a && b == b ? a + (b - a) * t : b);
        st.bSpread[i] = (float)(st.pSpread[i] + (st.wSpread[i] - st.pSpread[i]) * t);
    }
    for (int c = 0; c < C; c++) {
        const double p = st.pCell[c], q = st.wCell[c];
        if (p != p && q != q) { st.bCell[c] = NAN; continue; }
        const double fl = floorAt(st.bFloor.data(), F, (c + 0.5) / C);
        if (p == p && q == q) { st.bCell[c] = (float)(p + (q - p) * t); st.bShape[c] = st.wShape[c]; }
        else if (q == q) { st.bCell[c] = (float)(fl + std::max(0.0, q - fl) * t); st.bShape[c] = st.wShape[c]; }
        else { st.bCell[c] = (float)(fl + std::max(0.0, p - fl) * (1 - t)); st.bShape[c] = st.pShape[c]; }
    }
    return true;
}

/* ── THE LAYER RING ────────────────────────────────────────────────────────────────────────────────────────────────── */

struct LayerRing {
    int rows = 0, F = 0, C = 0, P = LAYER_PEAKS;
    std::vector<double> k, m;
    std::vector<uint8_t> floor;
    std::vector<uint16_t> pkN, pkCell;
    std::vector<uint8_t> pkIdx, pkShape;
    double fadeTx = 6;
    std::vector<int16_t> lay;
    std::vector<int32_t> tmpC; std::vector<uint8_t> tmpI; std::vector<int> order;
    LayerRing(int r, int f, int c, int p = LAYER_PEAKS)
        : rows(r), F(f), C(c), P(p), k(r, 0.0), m(r, 0.0), floor((size_t)r * f, 0), pkN(r, 0), pkCell((size_t)r * p, 0),
          pkIdx((size_t)r * p, 0), pkShape((size_t)r * p, 0), tmpC(c), tmpI(c) {}
};

/** The layer row for the ring row just written at `slot`, placed at (rowLo, rowSpan) Hz. Returns k (0 = none). */
template <class F>
inline double layerWriteRow(LayerRing& L, int slot, WideState* st, double rowLo, double rowSpan, double now, const F& toIdx) {
    L.k[slot] = 0; L.pkN[slot] = 0;
    if (!st || !st->has || !(rowSpan > 0) || !std::isfinite(rowLo) || rowSpan >= st->spanHz * LAYER_FULL || st->F != L.F
        || st->C != L.C || !wideBlend(*st, now)) return 0;
    const int Fn = L.F, C = L.C, P = L.P;
    const size_t fo = (size_t)slot * Fn;
    for (int i = 0; i < Fn; i++) {
        const double v = st->bFloor[i];
        L.floor[fo + i] = v == v ? (uint8_t)std::max(1, std::min(255, floorIdx(v, st->bSpread[i], toIdx))) : 0;
    }
    int n = 0;
    for (int c = 0; c < C; c++) {
        const double v = st->bCell[c];
        if (v != v) continue;
        const int ix = std::min(255, toIdx(v));
        if (ix < 1) continue;
        L.tmpC[n] = c; L.tmpI[n] = (uint8_t)ix; n++;
    }
    if (n > P) {   // keep the strongest P
        L.order.resize(n); std::iota(L.order.begin(), L.order.end(), 0);
        std::stable_sort(L.order.begin(), L.order.end(), [&](int a, int b) { return L.tmpI[a] > L.tmpI[b]; });
        L.order.resize(P); std::sort(L.order.begin(), L.order.end());
        for (int i = 0; i < P; i++) { const int j = L.order[i]; L.tmpC[i] = L.tmpC[j]; L.tmpI[i] = L.tmpI[j]; }   // j ≥ i: in place is safe
        n = P;
    }
    const size_t po = (size_t)slot * P;
    for (int i = 0; i < n; i++) { L.pkCell[po + i] = (uint16_t)L.tmpC[i]; L.pkIdx[po + i] = L.tmpI[i]; L.pkShape[po + i] = st->bShape[L.tmpC[i]]; }
    L.pkN[slot] = (uint16_t)n;
    L.k[slot] = rowSpan / st->spanHz;
    L.m[slot] = (rowLo - (st->centreHz - st->spanHz / 2)) / st->spanHz;
    return L.k[slot];
}

inline int layerFloorAt(const LayerRing& L, size_t fo, double uc) {
    if (uc < 0 || uc > 1) return -1;
    const int F = L.F; const uint8_t* fl = L.floor.data();
    if (F < 2) return fl[fo] ? fl[fo] : -1;
    const double p = uc * F - 0.5;
    int i = (int)std::floor(p); double t = p - i;
    if (i < 0) { i = 0; t = 0; } if (i >= F - 1) { i = F - 2; t = 1; }
    const int a = fl[fo + i], b = fl[fo + i + 1];
    if (!a && !b) return -1;
    if (!a) return b; if (!b) return a;
    return (int)std::floor(a + (b - a) * t + 0.5);
}

/** ★★ FILL a displayed row the fan just made (see wfWide.ts layerFillRow): lo[x] < 0 = no detailed data there. */
inline void layerFillRow(LayerRing& L, int slot, double a, double b, int n, uint8_t* disp, const int32_t* lo) {
    const double k = L.k[slot];
    if (!(k > 0) || !(a > 0)) return;
    if ((int)L.lay.size() < n) L.lay.resize(n);
    int16_t* lay = L.lay.data();
    const double m = L.m[slot];
    const size_t fo = (size_t)slot * L.F;
    const double A = k * a / n, B = k * (a * 0.5 / n + b) + m;
    const double XL = -b / a * n, XR = (1 - b) / a * n, fade = L.fadeTx;
    const bool fadeL = XL > 0 && fade > 0, fadeR = XR < n && fade > 0;
    bool any = false;
    for (int x = 0; x < n; x++) {
        bool need = lo[x] < 0;
        if (!need && (fadeL || fadeR)) { const double c = x + 0.5; need = (fadeL && c - XL < fade) || (fadeR && XR - c < fade); }
        if (!need) { lay[x] = -2; continue; }
        lay[x] = (int16_t)layerFloorAt(L, fo, A * x + B);
        any = true;
    }
    if (!any) return;
    const int C = L.C; const size_t po = (size_t)slot * L.P; const int np = L.pkN[slot]; const double half = 0.5 * A;
    for (int i = 0; i < np; i++) {
        const int cell = L.pkCell[po + i], sh = L.pkShape[po + i], ix = L.pkIdx[po + i];
        const double c0 = (cell + ((sh & 15) + 0.5) / 16) / C, hw = std::max(half, ((sh >> 4) + 1) / 32.0 / C);
        const int x0 = std::max(0, (int)std::ceil((c0 - hw - B) / A - 1e-9)), x1 = std::min(n - 1, (int)std::floor((c0 + hw - B) / A + 1e-9));
        for (int x = x0; x <= x1; x++) if (lay[x] >= 0 && ix > lay[x]) lay[x] = (int16_t)ix;
    }
    for (int x = 0; x < n; x++) {
        const int v = lay[x];
        if (v < 0) continue;
        if (lo[x] < 0) { disp[x] = (uint8_t)v; continue; }
        const double c = x + 0.5;
        double e = 1;
        if (fadeL) e = std::min(e, (c - XL) / fade);
        if (fadeR) e = std::min(e, (XR - c) / fade);
        if (e < 0) e = 0;
        if (e >= 1) continue;
        const int d = disp[x];
        disp[x] = (uint8_t)std::floor(v + (d - v) * e + 0.5);
    }
}

}  // namespace vibewf
