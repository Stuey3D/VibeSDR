// dec_pic_align.h — the WEFAX chart's rendering and auto-align, PORTED LINE FOR LINE from the app's own files:
//   src/utils/wefaxCrisp.ts  (the "gold standard" rendering: 5×5 smoothing, the chart's own paper/ink, lost lines)
//   src/utils/wefaxAlign.ts  (SHIFT / SLANT, the station presets and formats, the per-chart margin / border / strip /
//                             header-bar search, chartAlignStep)
// Every rule, threshold and comment's reasoning lives in those files — read them there; this is the same code in C++
// so the handheld draws exactly what the web page and the app draw. Golden-tested against the TS on the world corpus
// and the unit test's synthetic charts (handheld-ui/test/run_wefax_golden.sh).
//
// ★★ JS SEMANTICS KEPT: Math.round is floor(x + 0.5) (jsRound — std::round differs on negative halves), `%` on doubles
//    is fmod, the vertical smoothing is stored as float32 (crispLine's Float32Array), every other number a double.
// ★★ THE LOOK IS SPLIT FROM THE STEP (the one change of shape): chartAlignStep's counting runs on the UI thread as each
//    line arrives (alignStepPre), the heavy look (alignLook — up to ~100 passes over 600 × 1809 px) runs on a worker,
//    and its answer is applied when it lands (alignStepApply). Called back to back they ARE chartAlignStep (the golden
//    test does exactly that). PHASED ≠ ALIGNED (memory, Stuart 2026-10-06): the look judges content; nothing here
//    gates on the decoder's phasing state.
// ★ Pure: no GL, no App — rows are `const uint8_t*` (nullptr = a line that has not arrived), every row `width` long.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace hh {
namespace wfx {

inline double jsRound(double x) { return std::floor(x + 0.5); }
inline double jsMod(double a, double b) { return std::fmod(a, b); }
/** ((v % W) + W) % W for an integer v. */
inline int wrapI(long long v, int W) { long long m = v % W; if (m < 0) m += W; return (int)m; }
/** ((v % W) + W) % W for a double v (JS % on doubles). */
inline double wrapD(double v, double W) { return jsMod(jsMod(v, W) + W, W); }

typedef std::vector<const uint8_t*> Rows;

// ── wefaxCrisp ────────────────────────────────────────────────────────────────────────────────────────────────────────
constexpr double CRISP_WHITE = 0.90, CRISP_BLACK = 0.35;
struct Hist { uint32_t h[256] = {0}; };
inline void addToHist(Hist& h, const uint8_t* line, int n) { for (int i = 0; i < n; i++) h.h[line[i]]++; }
struct Levels { bool ok = false; double lo = 0, hi = 0; };
inline Levels crispLevels(const Hist& h) {
    double n = 0;
    for (int v = 0; v < 256; v++) n += h.h[v];
    Levels L;
    if (n < 2000) return L;
    auto at = [&](double p) { double acc = 0; for (int v = 0; v < 256; v++) { acc += h.h[v]; if (acc >= n * p) return v; } return 255; };
    const int ink = at(0.02), paper = at(0.70);
    if (paper - ink < 8) return L;
    L.ok = true; L.lo = ink + CRISP_BLACK * (paper - ink); L.hi = ink + CRISP_WHITE * (paper - ink);
    return L;
}
/** crispLine: `row(j)` → line j as DRAWN, or nullptr. `out` gets `width` greys. `v` is scratch of ≥ width floats. */
template <class RowF>
inline void crispLine(RowF row, int y, int W, const Levels& lv, uint8_t* out, std::vector<float>& v) {
    const uint8_t* self = row(y);
    if (!self) return;
    const uint8_t* rows[5];
    for (int d = -2; d <= 2; d++) {
        const uint8_t* r = row(y + d);
        if (!r) { r = d < 0 ? row(y + d + 1) : row(y + d - 1); if (!r) r = self; }
        if (!r) r = self;
        rows[d + 2] = r;
    }
    if ((int)v.size() < W) v.resize((size_t)W);
    for (int x = 0; x < W; x++)
        v[(size_t)x] = (float)(((double)rows[0][x] * 1 + (double)rows[1][x] * 4 + (double)rows[2][x] * 6 + (double)rows[3][x] * 4 + (double)rows[4][x] * 1) / 16);
    const double lo = lv.lo, span = std::max(1.0, lv.hi - lv.lo);
    for (int x = 0; x < W; x++) {
        const double a = v[(size_t)(x < 2 ? x : x - 2)], b = v[(size_t)(x < 1 ? x : x - 1)], c = v[(size_t)x],
                     d = v[(size_t)(x > W - 2 ? x : x + 1)], e = v[(size_t)(x > W - 3 ? x : x + 2)];
        const double g = (a * 1 + b * 4 + c * 6 + d * 4 + e * 1) / 16;
        const double t = (g - lo) / span;
        out[x] = t <= 0 ? 0 : t >= 1 ? 255 : (uint8_t)jsRound(t * 255);
    }
}
constexpr int WEFAX_FILL_MAX = 8;

// ── wefaxAlign: presets, formats, the offset ──────────────────────────────────────────────────────────────────────────
struct Align { double shift = 0, slant = 0; };
struct Format { bool margin = false, border = false; };
inline bool nearStation(double dialHz, const double* list, int n) {
    const double khz = dialHz / 1000;
    for (int i = 0; i < n; i++) if (std::fabs(khz + 1.9 - list[i]) <= 0.5 || std::fabs(khz - list[i]) <= 0.5) return true;
    return false;
}
inline const double kNorthwood[] = {2618.5, 4610, 8040, 11086.5};
inline const double kHll2[] = {3585, 5857.5, 7433.5, 9165, 13570};
inline const double kBorderKhz[] = {3855, 7880, 13882.5, 4482.9, 8106.9};
inline Align preset(double dialHz) {
    const double khz = dialHz / 1000;
    if (nearStation(khz * 1000, kNorthwood, 4)) return {0, -0.06};
    if (nearStation(khz * 1000, kHll2, 5)) return {0, 0.032};
    return {0, 0};
}
inline Format format(double dialHz) { return {nearStation(dialHz, kNorthwood, 4), nearStation(dialHz, kBorderKhz, 5)}; }
inline int alignKey(double dialHz) { return (int)jsRound(dialHz / 1000); }
inline int offset(const Align& a, int y, int width) {
    if (!width) return 0;
    return wrapI((long long)jsRound(a.shift + a.slant * y), width);
}
inline Align drawnAlign(const Align& a, bool raw) { return raw ? Align{} : a; }
constexpr double SLANT_STEP = 0.001;
constexpr int MARGIN_AFTER_LINES = 150;

// ── the searches ──────────────────────────────────────────────────────────────────────────────────────────────────────
constexpr double BLANK_INK = 6, MARGIN_INK = 140;
struct Peak { bool have = false; int col = 0; double strength = 0; bool ok = false; };
inline Peak marginPeak(const Rows& rows, int W, double slant, int fromRow, double count, double minRows) {
    Peak P;
    if (!W) return P;
    std::vector<double> acc((size_t)W, 0.0);
    int n = 0;
    for (int y = fromRow; y < fromRow + count && y < (int)rows.size(); y++) {
        const uint8_t* r = rows[(size_t)y];
        if (!r) continue;
        const int off = wrapI((long long)jsRound(slant * y), W);
        for (int x = 0; x < W; x++) acc[(size_t)x] += 255 - r[(x + off) % W];
        n++;
    }
    if (n < minRows) return P;
    std::vector<double> sm((size_t)W), narrow((size_t)W);
    for (int x = 0; x < W; x++) {
        double a = 0;
        for (int k = -2; k <= 2; k++) a += acc[(size_t)wrapI(x + k, W)];
        sm[(size_t)x] = a / 5 / n;
    }
    for (int x = 0; x < W; x++) {
        double l = 0, r = 0;
        for (int k = 25; k <= 40; k++) { r += sm[(size_t)((x + k) % W)]; l += sm[(size_t)wrapI(x - k, W)]; }
        narrow[(size_t)x] = sm[(size_t)x] - std::max(l, r) / 16;
    }
    int best = 0;
    for (int x = 1; x < W; x++) if (narrow[(size_t)x] > narrow[(size_t)best]) best = x;
    std::vector<double> sorted = narrow;
    std::sort(sorted.begin(), sorted.end(), [](double a, double b) { return a > b; });
    const double ref = sorted[(size_t)std::min(29, (int)sorted.size() - 1)];
    auto side = [&](int dir) { double a = 0; for (int k = 8; k <= 40; k++) a += sm[(size_t)wrapI(best + dir * k, W)]; return a / 33; };
    const bool framed = std::min(side(-1), side(1)) < BLANK_INK;
    double ink = 0;
    for (int k = -2; k <= 2; k++) ink = std::max(ink, acc[(size_t)wrapI(best + k, W)] / n);
    P.have = true; P.col = best; P.strength = narrow[(size_t)best];
    P.ok = !framed && ink >= MARGIN_INK && narrow[(size_t)best] >= 40 && narrow[(size_t)best] >= 2 * std::max(ref, 1.0);
    return P;
}
inline bool findMargin(const Rows& rows, int W, double slant, int fromRow, double count, int& col) {
    const Peak m = marginPeak(rows, W, slant, fromRow, count, count / 2);
    if (m.have && m.ok) { col = m.col; return true; }
    return false;
}
constexpr double SLANT_SEARCH = 0.05, MARGIN_SLANT_SEARCH = 0.04;
inline bool findMarginSlant(const Rows& rows, int W, double centre, int fromRow, double count, double minRows, int& col, double& slant) {
    bool have = false; int bc = 0; double bs = 0, bk = 0;
    auto tryAt = [&](double k) {
        const Peak m = marginPeak(rows, W, k, fromRow, count, minRows);
        if (m.have && m.ok && (!have || m.strength > bs)) { have = true; bc = m.col; bs = m.strength; bk = k; }
    };
    for (int i = -8; i <= 8; i++) tryAt(centre + i * MARGIN_SLANT_SEARCH / 8);
    if (!have) return false;
    const double c = bk;
    for (int i = -4; i <= 4; i++) if (i) tryAt(jsRound((c + i * 0.001) * 1000) / 1000);
    col = bc; slant = jsRound(bk * 1000) / 1000;
    return true;
}
constexpr double NOISY_CONTRAST = 0.05, GUTTER_SPECKS = 0.02, GUTTER_EDGE = 0.09;
constexpr int GUTTER_MIN = 24, GUTTER_GROW = 120, GUTTER_NOISY_MIN = 48, BORDER_MIN = 90, CENTRE_DEADBAND = 8;
struct Band { bool have = false; int col = 0, len = 0, edge = 0; double slant = 0, sharp = 0; int line = 0; };
struct Profile { std::vector<uint32_t> dark; std::vector<double> ink; int n = 0; };
inline Profile columnProfile(const Rows& rows, int W, double slant, int fromRow, double count, int rowStep) {
    Profile p; p.dark.assign((size_t)W, 0); p.ink.assign((size_t)W, 0.0);
    for (int y = fromRow; y < fromRow + count && y < (int)rows.size(); y += rowStep) {
        const uint8_t* r = rows[(size_t)y];
        if (!r) continue;
        const int off = wrapI((long long)jsRound(slant * y), W);
        int prev = r[(W - 1 + off) % W], last = -1;
        for (int x = 0; x < W; x++) {
            const int v = r[(x + off) % W];
            p.ink[(size_t)x] += 255 - v;
            if (prev + v < 320) {
                if (last != x - 1) p.dark[(size_t)((x - 1 + W) % W)]++;
                p.dark[(size_t)x]++; last = x;
            }
            prev = v;
        }
        p.n++;
    }
    return p;
}
inline Band bandOf(const Profile& p, int W, double slant) {
    Band B;
    const std::vector<uint32_t>& dark = p.dark;
    const std::vector<double>& ink = p.ink;
    const int n = p.n;
    std::vector<uint8_t> inked((size_t)W);
    struct Run { bool have; int len, at; };
    auto longest = [&](double tol) -> Run {
        for (int x = 0; x < W; x++) inked[(size_t)x] = dark[(size_t)x] > tol ? 1 : 0;
        int start = -1;
        for (int x = 0; x < W; x++) if (inked[(size_t)x]) { start = x; break; }
        if (start < 0) return {false, 0, 0};
        int bLen = 0, bAt = 0, len = 0;
        for (int i = 1; i <= W; i++) {
            const int x = (start + i) % W;
            if (!inked[(size_t)x]) { len++; if (len > bLen) { bLen = len; bAt = (x - len + 1 + W) % W; } }
            else len = 0;
        }
        return {true, bLen, bAt};
    };
    Run run = longest(GUTTER_SPECKS * n);
    if (!run.have) return B;
    double growTol = GUTTER_EDGE;
    if (run.len < GUTTER_MIN) {
        double floor = std::numeric_limits<double>::infinity();
        for (int x = 0; x < W; x++) {
            double a = 0;
            for (int k = -7; k <= 7; k++) a += dark[(size_t)((x + k + W) % W)];
            floor = std::min(floor, a / 15);
        }
        if (floor <= GUTTER_SPECKS * n / 2) return B;
        std::vector<uint32_t> sorted = dark;
        std::sort(sorted.begin(), sorted.end());
        if ((double)sorted[(size_t)(W >> 1)] - floor < NOISY_CONTRAST * n) return B;
        run = longest(floor + GUTTER_SPECKS * n);
        if (!run.have || run.len < GUTTER_NOISY_MIN) return B;
        growTol = std::max(GUTTER_EDGE, floor / n + 0.05);
    }
    int bestLen = run.len, bestAt = run.at;
    for (int k = 0; k < GUTTER_GROW && dark[(size_t)((bestAt - 1 + W) % W)] <= growTol * n && bestLen < W; k++) { bestAt = (bestAt - 1 + W) % W; bestLen++; }
    for (int k = 0; k < GUTTER_GROW && dark[(size_t)((bestAt + bestLen) % W)] <= growTol * n && bestLen < W; k++) bestLen++;
    if (bestLen < BORDER_MIN) return B;
    for (int x = 0; x < W; x++) inked[(size_t)x] = 1;
    for (int i = 0; i < bestLen; i++) inked[(size_t)((bestAt + i) % W)] = 0;
    int edge = 0;
    for (int x = 0; x < W && !inked[(size_t)x]; x++) edge++;
    for (int x = W - 1; x >= 0 && !inked[(size_t)x]; x--) edge++;
    double pl = 0, pr = 0; int cl = 0, cr = 0;
    for (int k = 1; k <= 12; k++) {
        const int xl = wrapI(bestAt - k, W), xr = (bestAt + bestLen - 1 + k) % W;
        if (ink[(size_t)xl] > pl) { pl = ink[(size_t)xl]; cl = xl; }
        if (ink[(size_t)xr] > pr) { pr = ink[(size_t)xr]; cr = xr; }
    }
    B.have = true; B.col = (bestAt + bestLen / 2) % W; B.len = bestLen; B.edge = edge; B.slant = slant;
    B.sharp = std::max(pl, pr) / n; B.line = pl >= pr ? cl : cr;
    return B;
}
inline Band findGutter(const Rows& rows, int W, double slant, int fromRow, double count, double minRows, int rowStep = 1) {
    if (!W) return Band{};
    const Profile p = columnProfile(rows, W, slant, fromRow, count, rowStep);
    if (p.n < minRows / rowStep) return Band{};
    return bandOf(p, W, slant);
}
constexpr double NEAR_WIN = 1.15, BAND_KEEP = 0.6, BORDER_SLANT_SEARCH = 0.15, WIDE_SLANT_WIN = 1.2, BORDER_LINE_INK = 80;
constexpr double SLANT_TRUST = 0.015, LINE_COVER = 0.75, LINE_SEG_DARK = 0.15;
constexpr int LINE_SEGMENTS = 8, LINE_PAIR_DARK = 256;
inline bool lineStretches(const Rows& rows, int W, double slant, int col, int fromRow, double count, int pair, bool* solid) {
    std::vector<int> ys;
    for (int y = fromRow; y < fromRow + count && y < (int)rows.size(); y++) if (rows[(size_t)y]) ys.push_back(y);
    const int per = (int)std::floor((double)ys.size() / LINE_SEGMENTS);
    if (!W || per < 4) return false;
    auto darkAt = [&](const uint8_t* r, int c, int off) {
        for (int k = -2; k <= 1; k++) {
            const int x = wrapI((long long)c + k + off, W);
            if (r[x] + r[(x + 1) % W] < pair) return 1;
        }
        return 0;
    };
    for (int s = 0; s < LINE_SEGMENTS; s++) {
        int dark = 0, left = 0, right = 0;
        for (int i = s * per; i < (s + 1) * per; i++) {
            const int y = ys[(size_t)i];
            const uint8_t* r = rows[(size_t)y];
            const int off = wrapI((long long)jsRound(slant * y), W);
            dark += darkAt(r, col, off); left += darkAt(r, col - 7, off); right += darkAt(r, col + 7, off);
        }
        solid[s] = dark >= per * LINE_SEG_DARK && dark >= std::max(left, right) + per * LINE_SEG_DARK;
    }
    return true;
}
inline double lineCover(const Rows& rows, int W, double slant, int col, int fromRow, double count, int pair = LINE_PAIR_DARK) {
    bool solid[LINE_SEGMENTS];
    if (!lineStretches(rows, W, slant, col, fromRow, count, pair, solid)) return 0;
    int k = 0; for (bool b : solid) if (b) k++;
    return (double)k / LINE_SEGMENTS;
}
inline bool slantJustified(const Rows& rows, int W, double slant, double centre, int col, int fromRow, double count) {
    if (std::fabs(slant - centre) <= SLANT_TRUST + 1e-9) return true;
    return lineCover(rows, W, slant, col, fromRow, count) >= LINE_COVER;
}
inline Band findBorder(const Rows& rows, int W, double centre, int fromRow, double count, double minRows) {
    Band best;
    auto tryAt = [&](double k, int step) {
        const Band g = findGutter(rows, W, jsRound(k * 1000) / 1000, fromRow, count, minRows, step);
        if (g.have && (!best.have || g.sharp > best.sharp + 0.5
                       || (g.sharp > best.sharp - 0.5 && std::fabs(g.slant - centre) < std::fabs(best.slant - centre)))) best = g;
    };
    for (int i = -10; i <= 10; i++) tryAt(centre + i * SLANT_SEARCH / 10, 2);
    const Band nearB = best;
    for (double k = SLANT_SEARCH + 0.01; k <= BORDER_SLANT_SEARCH + 1e-9; k += 0.01) { tryAt(centre + k, 2); tryAt(centre - k, 2); }
    if (nearB.have && best.have && best.sharp < WIDE_SLANT_WIN * nearB.sharp) best = nearB;
    if (!best.have) return Band{};
    const double c = best.slant;
    std::vector<Band> fine;
    for (int i = -6; i <= 6; i++) {
        const Band g = findGutter(rows, W, jsRound((c + i * 0.001) * 1000) / 1000, fromRow, count, minRows);
        if (g.have) fine.push_back(g);
    }
    best = Band{};
    if (!fine.empty()) {
        double top = -std::numeric_limits<double>::infinity();
        for (auto& g : fine) top = std::max(top, g.sharp);
        std::vector<Band> flat;
        for (auto& g : fine) if (g.sharp >= top * 0.99) flat.push_back(g);
        best = flat[(size_t)((flat.size() - 1) / 2)];
    }
    const Band b = best;
    Band close;
    for (int i = -3; i <= 3; i++) {
        const Band g = findGutter(rows, W, jsRound((centre + i * SLANT_TRUST / 3) * 1000) / 1000, fromRow, count, minRows);
        if (g.have && (!close.have || g.sharp > close.sharp + 0.5)) close = g;
    }
    if (b.have && b.sharp >= BORDER_LINE_INK
        && (std::fabs(b.slant - centre) <= SLANT_TRUST + 1e-9
            || ((!close.have || (b.sharp > NEAR_WIN * close.sharp && b.len >= BAND_KEEP * close.len)) && slantJustified(rows, W, b.slant, centre, b.line, fromRow, count)))) return b;
    if (close.have && close.sharp >= BORDER_LINE_INK) return close;
    return findGutter(rows, W, centre, fromRow, count, minRows);
}
constexpr int BAR_LOOK_LINES = 400, BAR_MIN_ROWS = 6, BAR_EDGE = 16, BAR_GAP_LO = 60, BAR_GAP_HI = 130;
constexpr double BAR_DARK = 0.85;
inline bool findHeaderBar(const Rows& rows, int W, double slant, double& out, int upto = BAR_LOOK_LINES) {
    if (W < 200) return false;
    std::vector<double> centres;
    std::vector<double> m((size_t)W);
    for (int y = 0; y < (int)rows.size() && y < upto; y++) {
        const uint8_t* r = rows[(size_t)y];
        if (!r) continue;
        double a = 0;
        for (int k = -2; k <= 2; k++) a += r[(k + W) % W];
        for (int x = 0; x < W; x++) { m[(size_t)x] = a / 5; a += (double)r[(x + 3) % W] - (double)r[(x - 2 + W) % W]; }
        int dark = 0;
        for (int x = 0; x < W; x++) if (m[(size_t)x] < 128) dark++;
        if (dark < BAR_DARK * W) { if ((int)centres.size() >= BAR_MIN_ROWS) break; centres.clear(); continue; }
        int start = -1;
        for (int x = 0; x < W; x++) if (m[(size_t)x] < 128) { start = x; break; }
        if (start < 0) continue;
        int bLen = 0, bAt = 0, len = 0, light = 0;
        for (int i = 1; i <= W; i++) {
            const int x = (start + i) % W;
            if (m[(size_t)x] >= 128) { len++; light++; if (len > bLen) { bLen = len; bAt = (x - len + 1 + W) % W; } } else len = 0;
        }
        if (bLen < BAR_GAP_LO || bLen > BAR_GAP_HI || light > bLen + 20) continue;
        const double off = jsRound(slant * y);
        centres.push_back(wrapD(bAt + bLen / 2.0 - off, W));
    }
    if ((int)centres.size() < BAR_MIN_ROWS) return false;
    const double ref = centres[0];
    std::vector<double> d;
    for (double c : centres) d.push_back(jsMod(c - ref + W * 1.5, W) - W / 2.0);
    std::sort(d.begin(), d.end());
    if (d.back() - d.front() > 8) return false;
    out = jsRound(wrapD(ref + d[d.size() >> 1], W));
    return true;
}
constexpr int ALIGN_CHECK_0 = 300, ALIGN_CHECK_1 = 600;

enum Via { V_NONE, V_BORDER, V_STRIP, V_MARGIN };
struct LookOut { bool atEdge = false; Via via = V_NONE; bool hasMeasured = false; double measured = 0; bool fixedSlant = false; };

constexpr int FAINT_PAIR = 384, FRAME_REACH = 12;
inline bool bandConfirmed(const Rows& rows, int W, const Band& g, double count) {
    return g.sharp >= BORDER_LINE_INK && lineCover(rows, W, g.slant, g.line, 0, count) >= LINE_COVER;
}
inline Band framedBand(const Rows& rows, int W, const Band& g, double count) {
    const int start = g.col - g.len / 2;
    std::vector<int> lines;
    for (int i = -FRAME_REACH; i < g.len + FRAME_REACH; i++) {
        const int c = wrapI(start + i, W);
        if (lineCover(rows, W, g.slant, c, 0, count, FAINT_PAIR) >= LINE_COVER) lines.push_back(i);
    }
    std::vector<std::pair<int, int>> groups;
    for (int i : lines) {
        if (!groups.empty() && i - groups.back().second <= 3) groups.back().second = i; else groups.push_back({i, i});
    }
    bool have = false; int ba = 0, bb = 0;
    for (size_t j = 0; j + 1 < groups.size(); j++) {
        const int a = groups[j].second, b = groups[j + 1].first;
        if (b - a - 1 >= BORDER_MIN && (!have || b - a > bb - ba)) { have = true; ba = a; bb = b; }
    }
    if (!have) return Band{};
    const int col = wrapI((long long)jsRound(start + (ba + bb) / 2.0), W);
    if (std::min(col, W - col) < CENTRE_DEADBAND) return Band{};
    Band o = g; o.col = col; o.len = bb - ba - 1; o.line = wrapI(start + bb, W);
    return o;
}
constexpr int STRIP_MIN = 40, STRIP_MAX = 200, STRIP_EDGE = 24, STRIP_GAIN = 8;
constexpr double STRIP_SOLID = 0.9, STRIP_SIDE_INK = 128, STRIP_INK = 225;
struct Strip { bool have = false; int at = 0, len = 0; };
inline Strip findStrip(const Rows& rows, int W, double slant, int fromRow, double count, double minRows) {
    if (!W) return Strip{};
    const Profile p = columnProfile(rows, W, slant, fromRow, count, 1);
    if (p.n < minRows || p.n < 20) return Strip{};
    std::vector<uint8_t> solid((size_t)W);
    int any = -1;
    for (int x = 0; x < W; x++) { solid[(size_t)x] = p.dark[(size_t)x] >= STRIP_SOLID * p.n ? 1 : 0; if (!solid[(size_t)x]) any = x; }
    if (any < 0) return Strip{};
    Strip best; int len = 0;
    for (int i = 1; i <= W; i++) {
        const int x = (any + i) % W;
        if (solid[(size_t)x]) { len++; continue; }
        if (len >= STRIP_MIN && len <= STRIP_MAX && (!best.have || len > best.len)) { best.have = true; best.at = (x - len + W) % W; best.len = len; }
        len = 0;
    }
    if (!best.have) return Strip{};
    auto side = [&](int from, int dir) { double a = 0; for (int k = 4; k < 44; k++) a += p.ink[(size_t)wrapI(from + dir * k, W)]; return a / 40 / p.n; };
    if (side(best.at, -1) > STRIP_SIDE_INK || side(best.at + best.len - 1, 1) > STRIP_SIDE_INK) return Strip{};
    double ink = 0;
    for (int i = 0; i < best.len; i++) ink += p.ink[(size_t)((best.at + i) % W)];
    if (ink / best.len / p.n < STRIP_INK) return Strip{};
    return best;
}
inline bool findStripSlant(const Rows& rows, int W, double centre, bool fixed, int fromRow, double count, double minRows, Strip& outS, double& outK) {
    auto at = [&](double k) { return findStrip(rows, W, jsRound(k * 1000) / 1000, fromRow, count, minRows); };
    const Strip s0 = at(centre);
    if (fixed) { if (!s0.have) return false; outS = s0; outK = centre; return true; }
    bool have = s0.have; Strip bs = s0; double bk = centre;
    auto tryAt = [&](double k) {
        const Strip s = at(k);
        if (s.have && (!have || s.len > bs.len || (s.len == bs.len && std::fabs(k - centre) < std::fabs(bk - centre)))) {
            have = true; bs = s; bk = jsRound(k * 1000) / 1000;
        }
    };
    for (int i = 1; i <= 17; i++) { tryAt(centre + i * 0.01); tryAt(centre - i * 0.01); }
    if (!have) return false;
    const double c = bk;
    for (int i = -4; i <= 4; i++) if (i) tryAt(c + i * 0.002);
    if (std::fabs(bk - centre) > SLANT_TRUST + 1e-9 && (!s0.have ? bs.len < STRIP_MIN + STRIP_GAIN : bs.len < s0.len + STRIP_GAIN)) {
        if (!s0.have) return false;
        outS = s0; outK = centre; return true;
    }
    outS = bs; outK = bk;
    return true;
}
inline Format defaultFormat(double stationSlant) { return {stationSlant != 0, false}; }

/** findChartAlign. Returns false for "nothing to move" (the TS null). */
inline bool findChartAlign(const Rows& rows, int W, double stationSlant, double count, double minRows, LookOut* out, const Format& fmt,
                           const Rows& stripRows, Align& res) {
    const bool fixed = out && out->fixedSlant;
    const Via kind = out ? out->via : V_NONE;
    auto phased = [&](double k) -> bool {
        if (out) out->atEdge = true;
        if (std::fabs(k - stationSlant) <= SLANT_TRUST + 1e-9) return false;
        if (out) { out->hasMeasured = true; out->measured = k; }
        res = {0, k};
        return true;
    };
    Band g0;
    if (!(kind == V_MARGIN || kind == V_STRIP))
        g0 = fixed ? findGutter(rows, W, stationSlant, 0, count, minRows) : findBorder(rows, W, stationSlant, 0, count, minRows);
    double bar = 0;
    const bool haveBar = g0.have && findHeaderBar(rows, W, stationSlant, bar);
    if (g0.have && haveBar && std::fabs(jsMod(bar - g0.col + W * 1.5, W) - W / 2.0) <= g0.len / 2.0 + 10) {
        const double k = !fixed && bandConfirmed(rows, W, g0, count) ? g0.slant : stationSlant;
        if (out) out->via = V_BORDER;
        if (std::min(bar, W - bar) < BAR_EDGE) return phased(k);
        if (out && k != stationSlant) { out->hasMeasured = true; out->measured = k; }
        res = {bar, k};
        return true;
    }
    Band g;
    if (g0.have && fmt.border) g = bandConfirmed(rows, W, g0, count) ? g0 : framedBand(rows, W, g0, count);
    if (g.have) {
        if (out) out->via = V_BORDER;
        const int fromEdge = std::min(g.col, W - g.col);
        if (fromEdge < CENTRE_DEADBAND) return phased(fixed ? stationSlant : g.slant);
        if (out && !fixed) { out->hasMeasured = true; out->measured = g.slant; }
        res = {(double)g.col, g.slant};
        return true;
    }
    if (kind != V_MARGIN) {
        Strip sp; double k = 0;
        if (findStripSlant(stripRows, W, stationSlant, fixed, 0, count, minRows, sp, k)) {
            if (out) out->via = V_STRIP;
            const int seam = (sp.at + sp.len / 2) % W;
            if (std::min(seam, W - seam) <= STRIP_EDGE || ((W - sp.at) % W) < sp.len) return phased(k);
            if (out && k != stationSlant) { out->hasMeasured = true; out->measured = k; }
            res = {(double)seam, k};
            return true;
        }
    }
    if (out) out->via = V_MARGIN;
    if (!fmt.margin) return false;
    if (fixed) {
        int col;
        if (!findMargin(rows, W, stationSlant, 0, count, col)) return false;
        res = {(double)(col - 2), stationSlant};
        return true;
    }
    int mc = 0; double mk = 0;
    const bool m = findMarginSlant(rows, W, stationSlant, 0, count, minRows, mc, mk);
    if (m && !slantJustified(rows, W, mk, stationSlant, mc, 0, count)) {
        int col;
        if (!findMargin(rows, W, stationSlant, 0, count, col)) return false;
        res = {(double)(col - 2), stationSlant};
        return true;
    }
    if (m && out) { out->hasMeasured = true; out->measured = mk; }
    if (!m) return false;
    res = {(double)(mc - 2), mk};
    return true;
}

constexpr int CHART_END_FLAT = 40, FLAT_ROW = 16;
inline bool rowIsFlat(const uint8_t* r, int width) {
    const int B = width / 8;
    if (B < 2) return true;
    double s = 0, ss = 0;
    for (int j = 0; j < B; j++) {
        double a = 0;
        for (int k = 0; k < 8; k++) a += r[j * 8 + k];
        a /= 8; s += a; ss += a * a;
    }
    const double m = s / B;
    return std::sqrt(std::max(0.0, ss / B - m * m)) < FLAT_ROW;
}
constexpr double RENDER_MAX_GREY = 0.18;
/** renderedRows: fills `store` (one row per content row) and `out`; false = not a two-tone chart (use the rows). */
inline bool renderedRows(const Rows& rows, int width, std::vector<std::vector<uint8_t>>& store, Rows& out) {
    Hist h;
    for (auto r : rows) if (r) addToHist(h, r, width);
    const Levels lv = crispLevels(h);
    if (!lv.ok) return false;
    long grey = 0, n = 0;
    store.assign(rows.size(), std::vector<uint8_t>());
    out.assign(rows.size(), nullptr);
    std::vector<float> v;
    auto rowF = [&](int j) -> const uint8_t* { return j >= 0 && j < (int)rows.size() ? rows[(size_t)j] : nullptr; };
    for (size_t y = 0; y < rows.size(); y++) {
        if (!rows[y]) continue;
        store[y].assign((size_t)width, 0);
        crispLine(rowF, (int)y, width, lv, store[y].data(), v);
        for (int x = 0; x < width; x += 3) { n++; if (store[y][(size_t)x] > 40 && store[y][(size_t)x] < 215) grey++; }
        out[y] = store[y].data();
    }
    return n && (double)grey / n <= RENDER_MAX_GREY;
}

/** ChartAlignState (wefaxAlign.ts) — `al`: lookedAt=false → undefined; lookedAt && !hasAl → null. */
struct AlignState {
    bool lookedAt = false, hasAl = false; Align al;
    bool refined = false; int n = 0;
    bool atEdge = false; Via via = V_NONE;
    bool hasSlant = false; double slant = 0;
    int flat = 0; bool retried = false;
};
/** The look chartAlignStep asks for (the heavy part — a worker's). */
enum LookKind { LK_NONE, LK_FIRST, LK_RETRY, LK_SECOND };
struct LookReq { LookKind kind = LK_NONE; Via via = V_NONE; bool fixedSlant = false; int n = 0; };
struct LookRes { bool have = false; Align a; bool atEdge = false; Via via = V_NONE; bool hasMeasured = false; double measured = 0; };

/** The step's counting (UI thread, every line): the row just received is counted; returns the look wanted, if any.
 *  ★ `pending`: a look is out on the worker — then none is asked for (the state it would read has not moved yet). */
inline LookReq alignStepPre(AlignState& st, const uint8_t* row, int width, bool pending = false) {
    if (!rowIsFlat(row, width)) { st.n++; st.flat = 0; }
    else st.flat++;
    LookReq q;
    q.n = st.n;
    if (pending) return q;
    const int n = st.n;
    if (!st.lookedAt) {
        const bool ended = n >= MARGIN_AFTER_LINES && st.flat == CHART_END_FLAT;
        if (n < ALIGN_CHECK_0 && !ended) return q;
        q.kind = LK_FIRST; q.via = V_NONE; q.fixedSlant = n < ALIGN_CHECK_0;
        return q;
    }
    if (st.refined || n < ALIGN_CHECK_1) return q;
    st.refined = true;
    if (!st.hasAl) {
        if (st.atEdge || st.retried) return q;
        st.retried = true;
        q.kind = LK_RETRY; q.via = V_NONE; q.fixedSlant = false;
        return q;
    }
    q.kind = LK_SECOND; q.via = st.via == V_BORDER ? V_NONE : st.via; q.fixedSlant = false;
    return q;
}
/** The look itself (pure, any thread): `rows` = every row so far, as received. */
inline LookRes alignLook(const Rows& rows, int width, double stationSlant, const Format& fmt, const LookReq& q) {
    LookOut out; out.via = q.via; out.fixedSlant = q.fixedSlant;
    Rows content(rows.size(), nullptr);
    for (size_t y = 0; y < rows.size(); y++) if (rows[y] && !rowIsFlat(rows[y], width)) content[y] = rows[y];
    std::vector<std::vector<uint8_t>> store;
    Rows rendered;
    const bool useR = renderedRows(content, width, store, rendered);
    LookRes r;
    r.have = findChartAlign(content, width, stationSlant, (double)rows.size(), q.n / 2.0, &out, fmt, useR ? rendered : content, r.a);
    r.atEdge = out.atEdge; r.via = out.via; r.hasMeasured = out.hasMeasured; r.measured = out.measured;
    return r;
}
/** Apply a look's answer (UI thread): returns true when the chart must be redrawn (chartAlignStep's return). */
inline bool alignStepApply(AlignState& st, const LookReq& q, const LookRes& lk, int width, double stationSlant) {
    if (q.kind == LK_FIRST) {
        st.lookedAt = true; st.hasAl = lk.have; st.al = lk.a; st.atEdge = lk.atEdge; st.via = lk.via;
        st.hasSlant = lk.hasMeasured; st.slant = lk.measured;
        return st.hasAl;
    }
    if (q.kind == LK_RETRY) {
        if (lk.hasMeasured) { st.hasSlant = true; st.slant = lk.measured; }
        if (!lk.have) return false;
        st.hasAl = true; st.al = lk.a; st.atEdge = lk.atEdge; st.via = lk.via;
        return true;
    }
    if (q.kind != LK_SECOND || !st.hasAl) return false;
    const Align cur = st.al;
    const bool again = lk.via == st.via && (st.atEdge ? lk.atEdge && (!lk.have || lk.a.shift == 0) : lk.have);
    if (!again) return false;
    const bool slantMoved = st.hasSlant != lk.hasMeasured || (st.hasSlant && st.slant != lk.measured);
    st.hasSlant = lk.hasMeasured; st.slant = lk.measured;
    const Align next = lk.have ? lk.a : Align{0, stationSlant};
    const int d = wrapI((long long)(next.shift - cur.shift), width);
    if (std::min(d, width - d) <= 2 && std::fabs(next.slant - cur.slant) < 0.002) return slantMoved;
    if (lk.have) { st.hasAl = true; st.al = next; }
    else if (st.atEdge) st.hasAl = false;
    else { st.hasAl = true; st.al = next; }
    return true;
}
/** chartAlignStep, whole (the golden test, and anything that wants it synchronously). */
inline bool chartAlignStep(AlignState& st, const Rows& rows, int width, double stationSlant, const uint8_t* row, const Format& fmt) {
    const LookReq q = alignStepPre(st, row, width);
    if (q.kind == LK_NONE) return false;
    return alignStepApply(st, q, alignLook(rows, width, stationSlant, fmt, q), width, stationSlant);
}

}  // namespace wfx
}  // namespace hh
