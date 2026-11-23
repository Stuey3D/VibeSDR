// vibe_wefax_finish.h — a finished WEFAX chart as the listener SAW it: auto-aligned (margin / border / strip, shift +
// slant) and rendered on clean paper, before the server saves it to the gallery.
//
// ★★★ WHY (Stuart, 2026-11-23: "background wefax is not applying our auto align and margin or contrast tweaks like they do
//     for users"). The live decoder view in the app and the web page runs two TypeScript modules on every line it draws —
//     src/utils/wefaxAlign.ts (where the line starts, and its slant) and src/utils/wefaxCrisp.ts (the chart's own paper
//     and ink levels, a 5×5 smoothing, the 35 %/90 % ramp) — but the gallery shows the PNG the SERVER saved, which was the
//     raw lines: grey speckle (paper ~138 of 255) and rolled. Background charts AND listeners' charts alike.
//
// ★★ THIS IS A PORT OF OUR OWN TYPESCRIPT, line for line — same constants, same order of arithmetic, same rounding (JS
//    Math.round, fmod for % on fractions) — so a chart is decided and drawn exactly as a listener's client would have
//    drawn it on a complete chart (scripts/wefax-finish-golden.ts + vibeserver/test-wefax-finish.cpp hold the two to
//    each other, pixel for pixel). A change to either .ts file must be made here too, and the parity test says so.
//    The rules and their measurements are documented in the .ts files; only what differs is said here.
//
// ★ THE WHOLE CHART, REPLAYED: the client decides as lines arrive (chartAlignStep: a look at 300 lines with content, a
//   second at 600, a retry; an early look when a chart ends flat). Its final state depends on WHEN it looked, so the
//   chart is replayed row by row through the same state machine — each look sees exactly the rows the client had then.
//   It stops at the second look (nothing after it can change the answer). Then, as the client's final redraw
//   (web main.ts redrawDecAlign / the app's DecoderPanel): shift = this chart's, slant = the slant MEASURED on it, else
//   the station's (nothing is saved on a server); lost lines filled from the line above (≤ 8); crisp over the lot.
//
// ★ MEMORY (256 MB boards): the chart is finished IN PLACE in the caller's buffer, plus ONE extra W×H buffer (the
//   rendered rows the strip search judges, then the output) and O(W + H) scratch — no per-row or per-pixel allocation.
// ★ FMA: the TS runs in IEEE doubles with no fused multiply-add. Clang may contract a*b+c on arm64, which moves results by
//   an ulp and can flip a tie — so every function here starts with VWF_EXACT (contraction off, scoped to that body).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#if defined(__clang__)
#define VWF_EXACT _Pragma("clang fp contract(off)")
#else
#define VWF_EXACT
#endif

namespace vibe {
namespace wefaxfinish {

/** JavaScript's Math.round: the nearest integer, halves upward (−2.5 → −2). */
inline double jsRound(double x) { VWF_EXACT const double f = std::floor(x); return (x - f >= 0.5) ? f + 1.0 : f; }
inline int wrapi(long long a, int W) { return (int)(((a % W) + W) % W); }

// ── Station presets (wefaxAlign.ts wefaxPreset / wefaxFormat) ─────────────────────────────────────────────────────────
inline bool nearStation(double dialHz, const double* list, int n) {
    VWF_EXACT
    const double khz = dialHz / 1000;
    for (int i = 0; i < n; i++) if (std::fabs(khz + 1.9 - list[i]) <= 0.5 || std::fabs(khz - list[i]) <= 0.5) return true;
    return false;
}
static const double kNorthwoodKhz[] = {2618.5, 4610, 8040, 11086.5};
static const double kHll2Khz[] = {3585, 5857.5, 7433.5, 9165, 13570};
static const double kBorderKhz[] = {3855, 7880, 13882.5, 4482.9, 8106.9};
/** The station's own slant for a DIAL frequency: Northwood −0.06, HLL2 +0.032, else 0. */
inline double stationSlant(double dialHz) {
    VWF_EXACT
    const double khz = dialHz / 1000;
    if (nearStation(khz * 1000, kNorthwoodKhz, 4)) return -0.06;
    if (nearStation(khz * 1000, kHll2Khz, 5)) return 0.032;
    return 0;
}
struct Format { bool margin = false, border = false; };
inline Format stationFormat(double dialHz) { return {nearStation(dialHz, kNorthwoodKhz, 4), nearStation(dialHz, kBorderKhz, 5)}; }

// ── Constants (the same names as the TS) ──────────────────────────────────────────────────────────────────────────────
constexpr double CRISP_WHITE = 0.90, CRISP_BLACK = 0.35;
constexpr int WEFAX_FILL_MAX = 8;
constexpr int MARGIN_AFTER_LINES = 150;
constexpr double BLANK_INK = 6, MARGIN_INK = 140;
constexpr double SLANT_SEARCH = 0.05, MARGIN_SLANT_SEARCH = 0.04;
constexpr double NOISY_CONTRAST = 0.05;
constexpr int GUTTER_MIN = 24;
constexpr double GUTTER_SPECKS = 0.02;
constexpr int GUTTER_GROW = 120;
constexpr double GUTTER_EDGE = 0.09;
constexpr int GUTTER_NOISY_MIN = 48, BORDER_MIN = 90, CENTRE_DEADBAND = 8;
constexpr double NEAR_WIN = 1.15, BAND_KEEP = 0.6, BORDER_SLANT_SEARCH = 0.15, WIDE_SLANT_WIN = 1.2, BORDER_LINE_INK = 80;
constexpr double SLANT_TRUST = 0.015;
constexpr int LINE_SEGMENTS = 8;
constexpr double LINE_COVER = 0.75, LINE_SEG_DARK = 0.15;
constexpr int LINE_PAIR_DARK = 256;
constexpr int BAR_LOOK_LINES = 400, BAR_MIN_ROWS = 6;
constexpr double BAR_DARK = 0.85;
constexpr int BAR_GAP_LO = 60, BAR_GAP_HI = 130, BAR_EDGE = 16;
constexpr int ALIGN_CHECK_0 = 300, ALIGN_CHECK_1 = 600, CHART_END_FLAT = 40;
constexpr double FLAT_ROW = 16;
constexpr double RENDER_MAX_GREY = 0.18;
constexpr int FAINT_PAIR = 384, FRAME_REACH = 12;
constexpr int STRIP_MIN = 40, STRIP_MAX = 200;
constexpr double STRIP_SOLID = 0.9, STRIP_SIDE_INK = 128;
constexpr int STRIP_EDGE = 24, STRIP_GAIN = 8;
constexpr double STRIP_INK = 225;

enum Via : uint8_t { VIA_NONE = 0, VIA_BORDER = 1, VIA_STRIP = 2, VIA_MARGIN = 3 };

/** A view of rows 0..len-1; a null row was not received (or is skipped). Each row is W bytes. */
struct Rows {
    const uint8_t* const* r = nullptr;
    int len = 0;
    const uint8_t* at(int y) const { return y >= 0 && y < len ? r[y] : nullptr; }
};

// ── wefaxCrisp.ts ─────────────────────────────────────────────────────────────────────────────────────────────────────
/** crispLevels: [lo, hi], false when there is not enough to judge or the page is flat. */
inline bool crispLevels(const uint32_t* h, double& lo, double& hi) {
    VWF_EXACT
    double n = 0;
    for (int v = 0; v < 256; v++) n += h[v];
    if (n < 2000) return false;
    auto at = [&](double p) -> int {
        VWF_EXACT
        double acc = 0;
        for (int v = 0; v < 256; v++) { acc += h[v]; if (acc >= n * p) return v; }
        return 255;
    };
    const int ink = at(0.02), paper = at(0.70);
    if (paper - ink < 8) return false;
    lo = ink + CRISP_BLACK * (paper - ink);
    hi = ink + CRISP_WHITE * (paper - ink);
    return true;
}

/** fillLostLines: `received(j)`, `copyAbove(y)` as the TS. */
template <class R, class C>
inline void fillLostLines(R received, C copyAbove, int y0, int y1, int maxRun = WEFAX_FILL_MAX) {
    for (int y = std::max(1, y0); y <= y1; y++) {
        if (received(y)) continue;
        int k = y - 1;
        while (k >= 0 && !received(k)) k--;
        if (k < 0) continue;
        int e = y + 1;
        while (e <= y1 && !received(e)) e++;
        if (e - k - 1 > maxRun) { y = e - 1; continue; }
        copyAbove(y);
    }
}

// ── The finisher: wefaxAlign.ts on one chart's width, with its scratch ───────────────────────────────────────────────
struct Band { int col = 0, len = 0, edge = 0; double slant = 0, sharp = 0; int line = 0; };
struct Strip { int at = 0, len = 0; };
struct Align { int shift = 0; double slant = 0; };
struct LookOut { bool atEdge = false; Via via = VIA_NONE; bool hasMeasured = false; double measured = 0; bool fixedSlant = false; };
/** chartAlignStep's ChartAlignState. `looked`: al defined; `hasAl`: al non-null. */
struct State {
    bool looked = false, hasAl = false; Align al;
    bool refined = false; int n = 0; bool atEdge = false; Via via = VIA_NONE;
    bool hasSlant = false; double slant = 0; int flat = 0; bool retried = false;
};

/** What finishing did — for the tests and the log. */
struct Result {
    bool looked = false, hasAl = false; int shift = 0; double slant = 0;   // the chart's state (st.al, st.slant drawn)
    Via via = VIA_NONE; bool atEdge = false, refined = false;
    bool crisp = false; double lo = 0, hi = 0;                              // levels used (crisp false = left as aligned)
};

class Finisher {
public:
    explicit Finisher(int W) : W(W) {
        acc.resize(W); sm.resize(W); narrow.resize(W); sorted.resize(W); ink.resize(W); dark.resize(W);
        darkSorted.resize(W); inked.resize(W); vsum.resize(W); vf.resize(W);
    }

    // ── wefaxAlign.ts ──
    bool rowIsFlat(const uint8_t* r) const {
        VWF_EXACT
        const int B = W / 8;
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

    struct Peak { bool has = false; int col = 0; double strength = 0; bool ok = false; };
    Peak marginPeak(const Rows& rows, double slant, int fromRow, int count, double minRows) {
        VWF_EXACT
        Peak out;
        if (!W) return out;
        std::fill(acc.begin(), acc.end(), 0.0);
        int n = 0;
        for (int y = fromRow; y < fromRow + count && y < rows.len; y++) {
            const uint8_t* r = rows.r[y];
            if (!r) continue;
            const int off = wrapi((long long)jsRound(slant * y), W);
            for (int x = 0; x < W; x++) acc[x] += 255 - r[(x + off) % W];
            n++;
        }
        if (n < minRows) return out;
        for (int x = 0; x < W; x++) {
            double a = 0;
            for (int k = -2; k <= 2; k++) a += acc[wrapi(x + k, W)];
            sm[x] = a / 5 / n;
        }
        for (int x = 0; x < W; x++) {
            double l = 0, r = 0;
            for (int k = 25; k <= 40; k++) { r += sm[(x + k) % W]; l += sm[wrapi(x - k, W)]; }
            narrow[x] = sm[x] - std::max(l, r) / 16;
        }
        int best = 0;
        for (int x = 1; x < W; x++) if (narrow[x] > narrow[best]) best = x;
        std::copy(narrow.begin(), narrow.end(), sorted.begin());
        const int ri = std::min(29, W - 1);
        std::nth_element(sorted.begin(), sorted.begin() + ri, sorted.end(), std::greater<double>());
        const double ref = sorted[ri];
        double sl = 0, sr = 0;
        for (int k = 8; k <= 40; k++) sl += sm[wrapi(best - k, W)];
        for (int k = 8; k <= 40; k++) sr += sm[wrapi(best + k, W)];
        const bool framed = std::min(sl / 33, sr / 33) < BLANK_INK;
        double inkMax = 0;
        for (int k = -2; k <= 2; k++) inkMax = std::max(inkMax, acc[wrapi(best + k, W)] / n);
        out.has = true; out.col = best; out.strength = narrow[best];
        out.ok = !framed && inkMax >= MARGIN_INK && narrow[best] >= 40 && narrow[best] >= 2 * std::max(ref, 1.0);
        return out;
    }
    bool findMargin(const Rows& rows, double slant, int fromRow, int count, int& col) {
        const Peak m = marginPeak(rows, slant, fromRow, count, count / 2.0);
        if (m.has && m.ok) { col = m.col; return true; }
        return false;
    }
    bool findMarginSlant(const Rows& rows, double centre, int fromRow, int count, double minRows, int& col, double& slant) {
        VWF_EXACT
        bool has = false; int bCol = 0; double bStr = 0, bSlant = 0;
        auto tryAt = [&](double k) {
            const Peak m = marginPeak(rows, k, fromRow, count, minRows);
            if (m.has && m.ok && (!has || m.strength > bStr)) { has = true; bCol = m.col; bStr = m.strength; bSlant = k; }
        };
        for (int i = -8; i <= 8; i++) tryAt(centre + (double)i * MARGIN_SLANT_SEARCH / 8);
        if (!has) return false;
        const double c = bSlant;
        for (int i = -4; i <= 4; i++) if (i) tryAt(jsRound((c + (double)i * 0.001) * 1000) / 1000);
        col = bCol; slant = jsRound(bSlant * 1000) / 1000;
        return true;
    }

    /** columnProfile → dark / ink (members), returns n. */
    int columnProfile(const Rows& rows, double slant, int fromRow, int count, int rowStep) {
        VWF_EXACT
        std::fill(dark.begin(), dark.end(), 0u);
        std::fill(ink.begin(), ink.end(), 0.0);
        int n = 0;
        for (int y = fromRow; y < fromRow + count && y < rows.len; y += rowStep) {
            const uint8_t* r = rows.r[y];
            if (!r) continue;
            const int off = wrapi((long long)jsRound(slant * y), W);
            int prev = r[(W - 1 + off) % W], last = -1;
            for (int x = 0; x < W; x++) {
                const int v = r[(x + off) % W];
                ink[x] += 255 - v;
                if (prev + v < 320) {
                    if (last != x - 1) dark[(x - 1 + W) % W]++;
                    dark[x]++; last = x;
                }
                prev = v;
            }
            n++;
        }
        return n;
    }

    /** bandOf on the profile in dark / ink. */
    bool bandOf(int n, double slant, Band& out) {
        VWF_EXACT
        auto longest = [&](double tol, int& len_, int& at_) -> bool {
            VWF_EXACT
            for (int x = 0; x < W; x++) inked[x] = dark[x] > tol ? 1 : 0;
            int start = -1;
            for (int x = 0; x < W; x++) if (inked[x]) { start = x; break; }
            if (start < 0) return false;
            int bLen = 0, bAt = 0, len = 0;
            for (int i = 1; i <= W; i++) {
                const int x = (start + i) % W;
                if (!inked[x]) { len++; if (len > bLen) { bLen = len; bAt = (x - len + 1 + W) % W; } }
                else len = 0;
            }
            len_ = bLen; at_ = bAt;
            return true;
        };
        int runLen = 0, runAt = 0;
        if (!longest(GUTTER_SPECKS * n, runLen, runAt)) return false;
        double growTol = GUTTER_EDGE;
        if (runLen < GUTTER_MIN) {
            double floorv = std::numeric_limits<double>::infinity();
            for (int x = 0; x < W; x++) {
                double a = 0;
                for (int k = -7; k <= 7; k++) a += dark[(x + k + W) % W];
                floorv = std::min(floorv, a / 15);
            }
            if (floorv <= GUTTER_SPECKS * n / 2) return false;
            std::copy(dark.begin(), dark.end(), darkSorted.begin());
            std::nth_element(darkSorted.begin(), darkSorted.begin() + (W >> 1), darkSorted.end());
            if (darkSorted[W >> 1] - floorv < NOISY_CONTRAST * n) return false;
            if (!longest(floorv + GUTTER_SPECKS * n, runLen, runAt) || runLen < GUTTER_NOISY_MIN) return false;
            growTol = std::max(GUTTER_EDGE, floorv / n + 0.05);
        }
        int bestLen = runLen, bestAt = runAt;
        for (int k = 0; k < GUTTER_GROW && dark[(bestAt - 1 + W) % W] <= growTol * n && bestLen < W; k++) {
            bestAt = (bestAt - 1 + W) % W; bestLen++;
        }
        for (int k = 0; k < GUTTER_GROW && dark[(bestAt + bestLen) % W] <= growTol * n && bestLen < W; k++) bestLen++;
        if (bestLen < BORDER_MIN) return false;
        for (int x = 0; x < W; x++) inked[x] = 1;
        for (int i = 0; i < bestLen; i++) inked[(bestAt + i) % W] = 0;
        int edge = 0;
        for (int x = 0; x < W && !inked[x]; x++) edge++;
        for (int x = W - 1; x >= 0 && !inked[x]; x--) edge++;
        double pl = 0, pr = 0; int cl = 0, cr = 0;
        for (int k = 1; k <= 12; k++) {
            const int xl = wrapi(bestAt - k, W), xr = (bestAt + bestLen - 1 + k) % W;
            if (ink[xl] > pl) { pl = ink[xl]; cl = xl; }
            if (ink[xr] > pr) { pr = ink[xr]; cr = xr; }
        }
        out.col = (bestAt + bestLen / 2) % W; out.len = bestLen; out.edge = edge; out.slant = slant;
        out.sharp = std::max(pl, pr) / n; out.line = pl >= pr ? cl : cr;
        return true;
    }
    bool findGutter(const Rows& rows, double slant, int fromRow, int count, double minRows, int rowStep, Band& out) {
        VWF_EXACT
        if (!W) return false;
        const int n = columnProfile(rows, slant, fromRow, count, rowStep);
        if (n < minRows / rowStep) return false;
        return bandOf(n, slant, out);
    }

    bool findBorder(const Rows& rows, double centre, int fromRow, int count, double minRows, Band& out) {
        VWF_EXACT
        bool has = false; Band best;
        auto tryAt = [&](double k, int step) {
            VWF_EXACT
            Band g;
            if (findGutter(rows, jsRound(k * 1000) / 1000, fromRow, count, minRows, step, g)
                && (!has || g.sharp > best.sharp + 0.5
                    || (g.sharp > best.sharp - 0.5 && std::fabs(g.slant - centre) < std::fabs(best.slant - centre)))) { best = g; has = true; }
        };
        for (int i = -10; i <= 10; i++) tryAt(centre + (double)i * SLANT_SEARCH / 10, 2);
        const bool hasNear = has; const Band nearB = best;
        for (double k = SLANT_SEARCH + 0.01; k <= BORDER_SLANT_SEARCH + 1e-9; k += 0.01) { tryAt(centre + k, 2); tryAt(centre - k, 2); }
        if (hasNear && has && best.sharp < WIDE_SLANT_WIN * nearB.sharp) best = nearB;
        if (!has) return false;
        const double c = best.slant;
        fine.clear();
        for (int i = -6; i <= 6; i++) {
            Band g;
            if (findGutter(rows, jsRound((c + (double)i * 0.001) * 1000) / 1000, fromRow, count, minRows, 1, g)) fine.push_back(g);
        }
        bool hasB = false; Band b;
        if (!fine.empty()) {
            double top = -std::numeric_limits<double>::infinity();
            for (const Band& g : fine) top = std::max(top, g.sharp);
            flat.clear();
            for (const Band& g : fine) if (g.sharp >= top * 0.99) flat.push_back(g);
            b = flat[(flat.size() - 1) / 2]; hasB = true;
        }
        bool hasClose = false; Band close;
        for (int i = -3; i <= 3; i++) {
            Band g;
            if (findGutter(rows, jsRound((centre + (double)i * SLANT_TRUST / 3) * 1000) / 1000, fromRow, count, minRows, 1, g)
                && (!hasClose || g.sharp > close.sharp + 0.5)) { close = g; hasClose = true; }
        }
        if (hasB && b.sharp >= BORDER_LINE_INK
            && (std::fabs(b.slant - centre) <= SLANT_TRUST + 1e-9
                || ((!hasClose || (b.sharp > NEAR_WIN * close.sharp && b.len >= BAND_KEEP * close.len))
                    && slantJustified(rows, b.slant, centre, b.line, fromRow, count)))) { out = b; return true; }
        if (hasClose && close.sharp >= BORDER_LINE_INK) { out = close; return true; }
        return findGutter(rows, centre, fromRow, count, minRows, 1, out);
    }

    bool slantJustified(const Rows& rows, double slant, double centre, int col, int fromRow, int count) {
        VWF_EXACT
        if (std::fabs(slant - centre) <= SLANT_TRUST + 1e-9) return true;
        return lineCover(rows, slant, col, fromRow, count, LINE_PAIR_DARK) >= LINE_COVER;
    }
    double lineCover(const Rows& rows, double slant, int col, int fromRow, int count, int pair) {
        VWF_EXACT
        ys.clear();
        for (int y = fromRow; y < fromRow + count && y < rows.len; y++) if (rows.r[y]) ys.push_back(y);
        const int per = (int)(ys.size() / LINE_SEGMENTS);
        if (!W || per < 4) return 0;
        auto darkAt = [&](const uint8_t* r, int c, int off) -> int {
            for (int k = -2; k <= 1; k++) {
                const int x = wrapi((long long)c + k + off, W);
                if (r[x] + r[(x + 1) % W] < pair) return 1;
            }
            return 0;
        };
        int solid = 0;
        for (int s = 0; s < LINE_SEGMENTS; s++) {
            int dk = 0, left = 0, right = 0;
            for (int i = s * per; i < (s + 1) * per; i++) {
                const int y = ys[i];
                const uint8_t* r = rows.r[y];
                const int off = wrapi((long long)jsRound(slant * y), W);
                dk += darkAt(r, col, off); left += darkAt(r, col - 7, off); right += darkAt(r, col + 7, off);
            }
            if (dk >= per * LINE_SEG_DARK && dk >= std::max(left, right) + per * LINE_SEG_DARK) solid++;
        }
        return (double)solid / LINE_SEGMENTS;
    }

    bool findHeaderBar(const Rows& rows, double slant, int& bar, int upto = BAR_LOOK_LINES) {
        VWF_EXACT
        if (W < 200) return false;
        centres.clear();
        std::vector<double>& m = sm;   // scratch, W doubles (free here)
        for (int y = 0; y < rows.len && y < upto; y++) {
            const uint8_t* r = rows.r[y];
            if (!r) continue;
            double a = 0;
            for (int k = -2; k <= 2; k++) a += r[(k + W) % W];
            for (int x = 0; x < W; x++) { m[x] = a / 5; a += (double)r[(x + 3) % W] - (double)r[(x - 2 + W) % W]; }
            int dk = 0;
            for (int x = 0; x < W; x++) if (m[x] < 128) dk++;
            if (dk < BAR_DARK * W) { if ((int)centres.size() >= BAR_MIN_ROWS) break; centres.clear(); continue; }
            int start = -1;
            for (int x = 0; x < W; x++) if (m[x] < 128) { start = x; break; }
            if (start < 0) continue;
            int bLen = 0, bAt = 0, len = 0, light = 0;
            for (int i = 1; i <= W; i++) {
                const int x = (start + i) % W;
                if (m[x] >= 128) { len++; light++; if (len > bLen) { bLen = len; bAt = (x - len + 1 + W) % W; } } else len = 0;
            }
            if (bLen < BAR_GAP_LO || bLen > BAR_GAP_HI || light > bLen + 20) continue;
            const double off = jsRound(slant * y);
            centres.push_back(std::fmod(std::fmod(bAt + bLen / 2.0 - off, (double)W) + W, (double)W));
        }
        if ((int)centres.size() < BAR_MIN_ROWS) return false;
        const double ref = centres[0];
        for (double& c : centres) c = std::fmod(c - ref + W * 1.5, (double)W) - W / 2.0;
        std::sort(centres.begin(), centres.end());
        if (centres.back() - centres.front() > 8) return false;
        bar = (int)jsRound(std::fmod(std::fmod(ref + centres[centres.size() >> 1], (double)W) + W, (double)W));
        return true;
    }

    bool bandConfirmed(const Rows& rows, const Band& g, int count) {
        return g.sharp >= BORDER_LINE_INK && lineCover(rows, g.slant, g.line, 0, count, LINE_PAIR_DARK) >= LINE_COVER;
    }
    bool framedBand(const Rows& rows, const Band& g, int count, Band& out) {
        VWF_EXACT
        const int start = g.col - g.len / 2;
        lines.clear();
        for (int i = -FRAME_REACH; i < g.len + FRAME_REACH; i++) {
            const int c = wrapi((long long)start + i, W);
            if (lineCover(rows, g.slant, c, 0, count, FAINT_PAIR) >= LINE_COVER) lines.push_back(i);
        }
        groups.clear();
        for (int i : lines) {
            if (!groups.empty() && i - groups.back().second <= 3) groups.back().second = i; else groups.push_back({i, i});
        }
        bool has = false; int ba = 0, bb = 0;
        for (size_t j = 0; j + 1 < groups.size(); j++) {
            const int a = groups[j].second, b = groups[j + 1].first;
            if (b - a - 1 >= BORDER_MIN && (!has || b - a > bb - ba)) { has = true; ba = a; bb = b; }
        }
        if (!has) return false;
        const int col = wrapi((long long)jsRound(start + (ba + bb) / 2.0), W);
        if (std::min(col, W - col) < CENTRE_DEADBAND) return false;
        out = g; out.col = col; out.len = bb - ba - 1; out.line = wrapi((long long)start + bb, W);
        return true;
    }

    bool findStrip(const Rows& rows, double slant, int fromRow, int count, double minRows, Strip& out) {
        VWF_EXACT
        if (!W) return false;
        const int n = columnProfile(rows, slant, fromRow, count, 1);
        if (n < minRows || n < 20) return false;
        std::vector<uint8_t>& solid = inked;
        int any = -1;
        for (int x = 0; x < W; x++) { solid[x] = dark[x] >= STRIP_SOLID * n ? 1 : 0; if (!solid[x]) any = x; }
        if (any < 0) return false;
        bool has = false; Strip best; int len = 0;
        for (int i = 1; i <= W; i++) {
            const int x = (any + i) % W;
            if (solid[x]) { len++; continue; }
            if (len >= STRIP_MIN && len <= STRIP_MAX && (!has || len > best.len)) { has = true; best.at = (x - len + W) % W; best.len = len; }
            len = 0;
        }
        if (!has) return false;
        auto side = [&](int from, int dir) -> double {
            VWF_EXACT
            double a = 0;
            for (int k = 4; k < 44; k++) a += ink[wrapi((long long)from + dir * k, W)];
            return a / 40 / n;
        };
        if (side(best.at, -1) > STRIP_SIDE_INK || side(best.at + best.len - 1, 1) > STRIP_SIDE_INK) return false;
        double s = 0;
        for (int i = 0; i < best.len; i++) s += ink[(best.at + i) % W];
        if (s / best.len / n < STRIP_INK) return false;
        out = best;
        return true;
    }
    bool findStripSlant(const Rows& rows, double centre, bool fixed, int fromRow, int count, double minRows, Strip& strip, double& slant) {
        VWF_EXACT
        Strip s0; const bool has0 = findStrip(rows, jsRound(centre * 1000) / 1000, fromRow, count, minRows, s0);
        if (fixed) { if (has0) { strip = s0; slant = centre; } return has0; }
        bool has = has0; Strip bS = s0; double bK = centre;
        auto tryAt = [&](double k) {
            VWF_EXACT
            Strip s;
            if (findStrip(rows, jsRound(k * 1000) / 1000, fromRow, count, minRows, s)
                && (!has || s.len > bS.len || (s.len == bS.len && std::fabs(k - centre) < std::fabs(bK - centre)))) {
                has = true; bS = s; bK = jsRound(k * 1000) / 1000;
            }
        };
        for (int i = 1; i <= 17; i++) { tryAt(centre + (double)i * 0.01); tryAt(centre - (double)i * 0.01); }
        if (!has) return false;
        const double c = bK;
        for (int i = -4; i <= 4; i++) if (i) tryAt(c + (double)i * 0.002);
        if (std::fabs(bK - centre) > SLANT_TRUST + 1e-9 && (!has0 ? bS.len < STRIP_MIN + STRIP_GAIN : bS.len < s0.len + STRIP_GAIN)) {
            if (has0) { strip = s0; slant = centre; }
            return has0;
        }
        strip = bS; slant = bK;
        return true;
    }

    /** renderedRows: the chart levelled + smoothed (crisp) into `rendBuf`, pointers in `rendPtr`; false = use content. */
    bool renderedRows(const Rows& rows) {
        VWF_EXACT
        uint32_t h[256] = {0};
        for (int y = 0; y < rows.len; y++) if (const uint8_t* r = rows.r[y]) for (int x = 0; x < W; x++) h[r[x]]++;
        double lo, hi;
        if (!crispLevels(h, lo, hi)) return false;
        ensureBuf(rows.len);
        rendPtr.assign(rows.len, nullptr);
        double grey = 0, n = 0;
        for (int y = 0; y < rows.len; y++) {
            if (!rows.r[y]) continue;
            uint8_t* o = buf.data() + (size_t)y * W;
            crispLine(rows, y, lo, hi, o);
            for (int x = 0; x < W; x += 3) { n++; if (o[x] > 40 && o[x] < 215) grey++; }
            rendPtr[y] = o;
        }
        return n != 0 && grey / n <= RENDER_MAX_GREY;
    }

    /** crispLine: `out` receives W greys of row y as drawn. Integer sums: every value the TS forms is a multiple of
     *  1/256 below 2^20, exact in a double, so g = T / 256 is bit-for-bit the TS's. */
    void crispLine(const Rows& rows, int y, double lo, double hi, uint8_t* out) {
        VWF_EXACT
        const uint8_t* self = rows.at(y);
        if (!self) return;
        const uint8_t* r5[5];
        for (int d = -2; d <= 2; d++) {
            const uint8_t* r = rows.at(y + d);
            if (!r) { const uint8_t* q = d < 0 ? rows.at(y + d + 1) : rows.at(y + d - 1); r = q ? q : self; }
            r5[d + 2] = r;
        }
        for (int x = 0; x < W; x++) vsum[x] = r5[0][x] + 4 * r5[1][x] + 6 * r5[2][x] + 4 * r5[3][x] + r5[4][x];
        const double span = std::max(1.0, hi - lo);
        for (int x = 0; x < W; x++) {
            const int a = vsum[x < 2 ? x : x - 2], b = vsum[x < 1 ? x : x - 1], c = vsum[x],
                      d = vsum[x > W - 2 ? x : x + 1], e = vsum[x > W - 3 ? x : x + 2];
            const double g = (double)(a + 4 * b + 6 * c + 4 * d + e) / 256.0;
            const double t = (g - lo) / span;
            out[x] = t <= 0 ? 0 : t >= 1 ? 255 : (uint8_t)jsRound(t * 255);
        }
    }

    /** findChartAlign. `stripRows` is built on first use (renderedRows on `rows`, else `rows`). */
    bool findChartAlign(const Rows& rows, double stationSlant, int count, double minRows, LookOut& out, Format fmt, Align& res) {
        VWF_EXACT
        const bool fixed = out.fixedSlant; const Via kind = out.via;
        auto phased = [&](double k) -> bool {
            VWF_EXACT
            out.atEdge = true;
            if (std::fabs(k - stationSlant) <= SLANT_TRUST + 1e-9) return false;
            out.hasMeasured = true; out.measured = k;
            res.shift = 0; res.slant = k;
            return true;
        };
        Band g0; bool hasG0 = false;
        if (!(kind == VIA_MARGIN || kind == VIA_STRIP))
            hasG0 = fixed ? findGutter(rows, stationSlant, 0, count, minRows, 1, g0) : findBorder(rows, stationSlant, 0, count, minRows, g0);
        int bar = 0; const bool hasBar = hasG0 && findHeaderBar(rows, stationSlant, bar);
        if (hasG0 && hasBar && std::fabs(std::fmod(bar - g0.col + W * 1.5, (double)W) - W / 2.0) <= g0.len / 2.0 + 10) {
            const double k = !fixed && bandConfirmed(rows, g0, count) ? g0.slant : stationSlant;
            out.via = VIA_BORDER;
            if (std::min(bar, W - bar) < BAR_EDGE) return phased(k);
            if (k != stationSlant) { out.hasMeasured = true; out.measured = k; }
            res.shift = bar; res.slant = k;
            return true;
        }
        Band g; bool hasG = false;
        if (hasG0 && fmt.border) {
            if (bandConfirmed(rows, g0, count)) { g = g0; hasG = true; }
            else hasG = framedBand(rows, g0, count, g);
        }
        if (hasG) {
            out.via = VIA_BORDER;
            const int fromEdge = std::min(g.col, W - g.col);
            if (fromEdge < CENTRE_DEADBAND) return phased(fixed ? stationSlant : g.slant);
            if (!fixed) { out.hasMeasured = true; out.measured = g.slant; }
            res.shift = g.col; res.slant = g.slant;
            return true;
        }
        if (kind != VIA_MARGIN) {
            const Rows stripRows = renderedRows(rows) ? Rows{rendPtr.data(), rows.len} : rows;
            Strip sp; double k = 0;
            if (findStripSlant(stripRows, stationSlant, fixed, 0, count, minRows, sp, k)) {
                out.via = VIA_STRIP;
                const int seam = (sp.at + sp.len / 2) % W;
                if (std::min(seam, W - seam) <= STRIP_EDGE || ((W - sp.at) % W) < sp.len) return phased(k);
                if (k != stationSlant) { out.hasMeasured = true; out.measured = k; }
                res.shift = seam; res.slant = k;
                return true;
            }
        }
        out.via = VIA_MARGIN;
        if (!fmt.margin) return false;
        if (fixed) {
            int col;
            if (!findMargin(rows, stationSlant, 0, count, col)) return false;
            res.shift = col - 2; res.slant = stationSlant; return true;
        }
        int mCol = 0; double mSlant = 0;
        const bool hasM = findMarginSlant(rows, stationSlant, 0, count, minRows, mCol, mSlant);
        if (hasM && !slantJustified(rows, mSlant, stationSlant, mCol, 0, count)) {
            int col;
            if (!findMargin(rows, stationSlant, 0, count, col)) return false;
            res.shift = col - 2; res.slant = stationSlant; return true;
        }
        if (hasM) { out.hasMeasured = true; out.measured = mSlant; res.shift = mCol - 2; res.slant = mSlant; return true; }
        return false;
    }

    /** chartAlignStep for the row just received at `y` (raw[y], content flags precomputed). True = would redraw. */
    bool chartAlignStep(State& st, const std::vector<const uint8_t*>& content, int y, double stationSlant, Format fmt, bool rowFlat) {
        VWF_EXACT
        if (!rowFlat) { st.n++; st.flat = 0; } else st.flat++;
        const int n = st.n;
        const Rows rows{content.data(), y + 1};
        struct Lk { bool hasA; Align a; bool atEdge; Via via; bool hasMeasured; double measured; };
        auto look = [&](Via via, bool fixedSlant) -> Lk {
            VWF_EXACT
            LookOut out; out.via = via; out.fixedSlant = fixedSlant;
            Align a; const bool hasA = findChartAlign(rows, stationSlant, rows.len, n / 2.0, out, fmt, a);
            return {hasA, a, out.atEdge, out.via, out.hasMeasured, out.measured};
        };
        if (!st.looked) {
            const bool ended = n >= MARGIN_AFTER_LINES && st.flat == CHART_END_FLAT;
            if (n < ALIGN_CHECK_0 && !ended) return false;
            const Lk lk = look(VIA_NONE, n < ALIGN_CHECK_0);
            st.looked = true; st.hasAl = lk.hasA; st.al = lk.a; st.atEdge = lk.atEdge; st.via = lk.via;
            st.hasSlant = lk.hasMeasured; st.slant = lk.measured;
            return st.hasAl;
        }
        if (st.refined || n < ALIGN_CHECK_1) return false;
        st.refined = true;
        if (!st.hasAl) {
            if (st.atEdge || st.retried) return false;
            st.retried = true;
            const Lk lk = look(VIA_NONE, false);
            if (lk.hasMeasured) { st.hasSlant = true; st.slant = lk.measured; }
            if (!lk.hasA) return false;
            st.hasAl = true; st.al = lk.a; st.atEdge = lk.atEdge; st.via = lk.via;
            return true;
        }
        const Align cur = st.al;
        const Lk lk = look(st.via == VIA_BORDER ? VIA_NONE : st.via, false);
        const bool again = lk.via == st.via && (st.atEdge ? lk.atEdge && (!lk.hasA || lk.a.shift == 0) : lk.hasA);
        if (!again) return false;
        const bool slantMoved = st.hasSlant != lk.hasMeasured || (st.hasSlant && st.slant != lk.measured);
        st.hasSlant = lk.hasMeasured; st.slant = lk.measured;
        const Align next = lk.hasA ? lk.a : Align{0, stationSlant};
        const int d = wrapi((long long)next.shift - cur.shift, W);
        if (std::min(d, W - d) <= 2 && std::fabs(next.slant - cur.slant) < 0.002) return slantMoved;
        if (lk.hasA) { st.hasAl = true; st.al = next; }
        else if (st.atEdge) { st.hasAl = false; }
        else { st.hasAl = true; st.al = next; }
        return true;
    }

    /**
     * ★ FINISH A CHART IN PLACE: `px` = H rows of W greys; `got[y]` = row y was received (null = all were). Rows never
     * received and not filled are left as they are in `px`.
     */
    Result finish(uint8_t* px, int H, const uint8_t* got, double dialHz) {
        VWF_EXACT
        Result res;
        if (W <= 0 || H <= 0) return res;
        auto rec = [&](int y) { return !got || got[y] != 0; };
        const double station = stationSlant(dialHz);
        const Format fmt = stationFormat(dialHz);
        // 1. the alignment, replayed as the client decided it
        std::vector<const uint8_t*> content((size_t)H, nullptr);
        State st;
        for (int y = 0; y < H; y++) {
            if (!rec(y)) continue;
            const uint8_t* r = px + (size_t)y * W;
            const bool fl = rowIsFlat(r);
            content[y] = fl ? nullptr : r;
            chartAlignStep(st, content, y, station, fmt, fl);
            if (st.refined) break;   // nothing after the second look changes the answer
        }
        std::vector<const uint8_t*>().swap(content);
        std::vector<const uint8_t*>().swap(rendPtr);
        res.looked = st.looked; res.hasAl = st.hasAl; res.via = st.via; res.atEdge = st.atEdge; res.refined = st.refined;
        res.shift = st.hasAl ? st.al.shift : 0;
        res.slant = st.hasSlant ? st.slant : station;
        // 2. the levels: every received row as received
        uint32_t h[256] = {0};
        for (int y = 0; y < H; y++) if (rec(y)) { const uint8_t* r = px + (size_t)y * W; for (int x = 0; x < W; x++) h[r[x]]++; }
        double lo = 0, hi = 0;
        res.crisp = crispLevels(h, lo, hi); res.lo = lo; res.hi = hi;
        // 3. shift + slant, in place (wefaxOffset, decAlignRow)
        std::vector<uint8_t>& tmp = rowTmp; tmp.resize(W);
        for (int y = 0; y < H; y++) {
            if (!rec(y)) continue;
            const int off = wrapi((long long)jsRound((double)res.shift + res.slant * y), W);
            if (!off) continue;
            uint8_t* r = px + (size_t)y * W;
            std::memcpy(tmp.data(), r, W);
            for (int x = 0; x < W; x++) r[x] = tmp[(x + off) % W];
        }
        // 4. lost lines drawn as the line above
        std::vector<uint8_t> drawn((size_t)H);
        for (int y = 0; y < H; y++) drawn[y] = rec(y) ? 1 : 0;
        fillLostLines([&](int j) { return rec(j); },
                      [&](int y) { std::memcpy(px + (size_t)y * W, px + (size_t)(y - 1) * W, W); drawn[y] = 1; }, 0, H - 1);
        if (!res.crisp) return res;
        // 5. the crisp rendering of every drawn row, into the one extra buffer, then back
        std::vector<const uint8_t*> al((size_t)H, nullptr);
        for (int y = 0; y < H; y++) if (drawn[y]) al[y] = px + (size_t)y * W;
        ensureBuf(H);
        const Rows rows{al.data(), H};
        for (int y = 0; y < H; y++) if (drawn[y]) crispLine(rows, y, lo, hi, buf.data() + (size_t)y * W);
        for (int y = 0; y < H; y++) if (drawn[y]) std::memcpy(px + (size_t)y * W, buf.data() + (size_t)y * W, W);
        return res;
    }

    /** Free the big buffer (the finisher may be kept). */
    void release() { std::vector<uint8_t>().swap(buf); }

private:
    void ensureBuf(int rowsNeeded) { const size_t need = (size_t)rowsNeeded * W; if (buf.size() < need) buf.resize(need); }

    int W;
    std::vector<double> acc, sm, narrow, sorted, ink;
    std::vector<uint32_t> dark, darkSorted;
    std::vector<uint8_t> inked, rowTmp, buf;
    std::vector<int> vsum, ys, lines;
    std::vector<float> vf;
    std::vector<double> centres;
    std::vector<Band> fine, flat;
    std::vector<std::pair<int, int>> groups;
    std::vector<const uint8_t*> rendPtr;
};

/** ★ One call: finish a WEFAX chart in place for the gallery. */
inline Result finishChart(uint8_t* px, uint32_t W, uint32_t H, const uint8_t* got, double dialHz) {
    if (!px || W < 16 || H == 0) return Result{};
    Finisher f((int)W);
    return f.finish(px, (int)H, got, dialHz);
}

}  // namespace wefaxfinish
}  // namespace vibe
