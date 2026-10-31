// decoders/feld_hell.cpp — FELD HELL receiver (drawn, not read) and its test transmitter. See feld_hell.h. V12, 2026-10-31.
#include "feld_hell.h"
#include "digimodes/q65_simd.h"
#include "../vibedsp/third_party/pffft/pffft.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace vibe {
namespace {
constexpr double kElemRate = 122.5;          // elements a second (the published baud)
constexpr double kHalfRate = 2 * kElemRate;  // the receiver samples half-elements
constexpr int kFft = 1024, kHop = 512;
constexpr float kLo = 300.f, kHi = 2700.f;
}

struct FeldHellDecoder::Impl {
    double inRate = 48000, fs = 8000; int q = 1, qn = 0; float acc = 0, qinv = 1;
    // finder
    PFFFT_Setup* setup = nullptr; float* fin = nullptr; float* fout = nullptr; float* work = nullptr;
    std::vector<float> win, ring, P; int wp = 0, sinceHop = 0, filled = 0;
    int bin = -1; float hzNow = 0; bool sig = false; int quietHops = 0, hops = 0;
    // mixer: four phasors a lane, rotated by w^4 each step
    alignas(16) float pr[4] = {1, 0, 0, 0}, pi[4] = {0, 0, 0, 0}; float w4r = 1, w4i = 0; double omega = 0; int renorm = 0;
    std::vector<float> xb, ib, qb;   // a block in, its mixed I and Q
    // the box filter over one half-element
    int L = 33; std::vector<float> ri, rq; int rp = 0; double si = 0, sq = 0;
    // sampling
    double t = 0, next = 0, step = 0; float col[kRows] = {}; int nh = 0; bool partial = true;
    float peak = 1e-6f, floorv = 0; bool levels = false;
    // the strip
    std::vector<uint8_t> strip; int x = 0; uint32_t stripNo = 0; bool dirty = false;
    std::string lastStatus;
    FeldHellDecoder* self = nullptr;

    void init(double r) {
        inRate = r;
        q = std::max(1, (int)std::floor(r / 8000.0 + 1e-9)); qinv = 1.f / q; fs = r / q;
        setup = pffft_new_setup(kFft, PFFFT_REAL);
        fin = (float*)pffft_aligned_malloc(kFft * sizeof(float));
        fout = (float*)pffft_aligned_malloc(kFft * sizeof(float));
        work = (float*)pffft_aligned_malloc(kFft * sizeof(float));
        win.resize(kFft); ring.assign(kFft, 0.f); P.assign(kFft / 2, 0.f);
        for (int i = 0; i < kFft; ++i) win[i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / kFft));
        // one HALF-element: a whole element (matched to the font, +3 dB) blurred and shifted every edge by half a
        // half-element — 0 dB drew 87.6 % right against 90.2 % (measured, test-feld-hell)
        L = std::max(1, (int)std::lround(fs / kHalfRate));
        ri.assign(L, 0.f); rq.assign(L, 0.f);
        step = fs / kHalfRate; next = step;
        strip.assign((size_t)2 * kRows * kStripCols, 255);
    }
    ~Impl() { if (setup) pffft_destroy_setup(setup); pffft_aligned_free(fin); pffft_aligned_free(fout); pffft_aligned_free(work); }

    void setTone(float hz) {
        hzNow = hz; omega = 2 * M_PI * hz / fs;
        // phasors e^{-jωk}, k = 0…3, and the step e^{-j4ω}
        for (int k = 0; k < 4; ++k) { pr[k] = (float)std::cos(omega * k); pi[k] = (float)-std::sin(omega * k); }
        w4r = (float)std::cos(4 * omega); w4i = (float)-std::sin(4 * omega);
    }
    /** every 64 ms: the strongest steady tone in 300–2700 Hz (EMA of the power, one bin), held unless another is twice it */
    void hop() {
        const int a = kFft - wp;
        std::memcpy(fin, ring.data() + wp, a * sizeof(float));
        std::memcpy(fin + a, ring.data(), wp * sizeof(float));
        for (int k = 0; k < kFft; k += 4) q65v::st(fin + k, q65v::mul(q65v::ld(fin + k), q65v::ld(win.data() + k)));
        pffft_transform_ordered(setup, fin, fout, work, PFFFT_FORWARD);
        const q65v::V A = q65v::set1(0.25f);
        for (int k = 0; k < kFft / 2; k += 4) {
            q65v::V re, im; q65v::ld2(fout + 2 * k, re, im);
            const q65v::V p = q65v::add(q65v::mul(re, re), q65v::mul(im, im)), c = q65v::ld(P.data() + k);
            q65v::st(P.data() + k, q65v::add(c, q65v::mul(A, q65v::sub(p, c))));
        }
        const double bhz = fs / kFft;
        const int lo = (int)std::ceil(kLo / bhz), hi = std::min(kFft / 2 - 2, (int)std::floor(kHi / bhz));
        int bb = lo; double sum = 0;
        for (int k = lo; k <= hi; ++k) { sum += P[k]; if (P[k] > P[bb]) bb = k; }
        const double mean = sum / (hi - lo + 1);
        // ★ a keyed tone is FOUND at ≥ 8× the band's mean in its bin and HELD while ≥ 4× (noise: an EMA of ≈ 7 hops over
        //   ≈ 300 bins). Dead end: 6× / 3× — 2884 lines of noise drawn in 60 min, and a noise place found in the second
        //   before a +10 dB signal held the finder there (68 % of its pixels right).
        // ★ …and not before the average has settled (8 hops, 0.5 s): the first hops are single periodograms, where one of
        //   ≈ 300 noise bins passes 8× one hop in ten — measured: noise drawn before the signal on 2 seeds in 20
        const bool here = ++hops > 8 && P[bb] > 8 * mean;
        if (here && (bin < 0 || P[bb] > 2 * P[bin] || P[bin] < 4 * mean)) {
            if (bin < 0 || std::abs(bb - bin) > 1) {
                const float y0 = P[bb - 1], y1 = P[bb], y2 = P[bb + 1], den = y0 - 2 * y1 + y2;
                const float d = std::fabs(den) > 1e-20f ? 0.5f * (y0 - y2) / den : 0.f;
                setTone((float)((bb + std::max(-0.5f, std::min(0.5f, d))) * bhz));
            }
            bin = bb;
        }
        const bool s = bin >= 0 && P[bin] > 4 * mean;
        quietHops = s ? 0 : quietHops + 1;
        sig = s || (bin >= 0 && quietHops < 32);   // 2 s of grace: a pause between words is not the end
        status();
    }
    void status() {
        char js[64]; std::snprintf(js, sizeof js, "{\"hz\":%.0f,\"signal\":%d}", std::round(hzNow / 10) * 10, sig ? 1 : 0);
        if (js != lastStatus) { lastStatus = js; if (self->onStatus) self->onStatus(js); }
    }
    void column() {
        // ★ the first column after the tone is found is part-empty: skipped, so it neither draws nor sets the levels
        //   (it set them from uninitialised samples once — NaN on armhf, every pixel black on 4 seeds in 20; measured)
        if (partial) { partial = false; return; }
        // ★ LEVELS: the two levels of on-off keying, each the running mean of the samples on its side of the half-way
        //   mark (a two-level slicer, ≈ 1 s), ink = how far from "off" to "on". (A peak-and-floor tracker first: on a
        //   weak signal the peak rode the noise peaks and the floor its dips, and −6 dB drew as noise — measured.)
        if (!levels) {   // the first column sets the two levels (from nothing, the slicer drew the first seconds black)
            levels = true; floorv = *std::min_element(col, col + kRows); peak = std::max(floorv * 2, *std::max_element(col, col + kRows));
        }
        for (float e : col) { const float mid = 0.5f * (floorv + peak); if (e < mid) floorv += (1.f / 128) * (e - floorv); else peak += (1.f / 128) * (e - peak); }
        if (peak < floorv * 1.05f) peak = floorv * 1.05f + 1e-9f;
        const float g = 255.f / std::max(1e-9f, peak - floorv);   // one divide a column
        if (!sig) return;   // nothing drawn while no tone is keyed: the strip does not advance through silence
        // ★ NO COLUMN-PHASE CORRECTION, on purpose: the receiver's columns start wherever its clock started, and when that
        //   is half a column from the sender's each copy shows a letter's bottom above its top — the two copies, one
        //   below the other, are what makes it whole (the published reason for printing every column twice). Dead ends
        //   (2026-10-31): moving the column edge to the least-inked position, a half-element a column (letters wobbled
        //   on both public recordings, which were in phase to start with), and once after 2 s in the same place (it
        //   still moved mid-word on both, and did nothing for a font that inks every row).
        for (int h = 0; h < kRows; ++h) {
            const float v = std::max(0.f, std::min(255.f, (col[h] - floorv) * g));
            const uint8_t ink = (uint8_t)(255.f - v);
            const int row = kRows - 1 - h;   // ★ bottom first (settled on the reference recordings)
            strip[(size_t)row * kStripCols + x] = ink;              // upper copy
            strip[(size_t)(row + kRows) * kStripCols + x] = ink;    // lower copy (the same column again)
        }
        ++x; dirty = true;
        if (x % 7 == 0 || x == kStripCols) send();
        if (x == kStripCols) { x = 0; ++stripNo; std::fill(strip.begin(), strip.end(), 255); dirty = false; }
    }
    void send() {
        if (!self->onLine) return;
        for (int r = 0; r < 2 * kRows; ++r) self->onLine(stripNo * 2 * kRows + r, kStripCols, strip.data() + (size_t)r * kStripCols);
        dirty = false;
    }
    void block(const float* x8, int n) {
        // finder ring
        for (int i = 0; i < n; ++i) {
            ring[wp] = x8[i]; if (++wp == kFft) wp = 0;
            if (filled < kFft) ++filled;
            if (++sinceHop == kHop) { sinceHop = 0; if (filled == kFft) hop(); }
        }
        // ★ the column clock runs from the first sample whether or not a tone has been found: started at the finding
        //   instead, the columns began wherever that hop fell (measured: every letter a row out, 72 % of the pixels)
        if (bin < 0) {
            for (int i = 0; i < n; ++i) { t += 1.0; if (t >= next) { next += step; if (++nh == kRows) nh = 0; } }
            partial = true;   // the column in progress when the tone is found holds nothing yet: not drawn
            return;
        }
        // mixer, four samples a lane
        const int n4 = n & ~3;
        ib.resize(n); qb.resize(n);
        q65v::V Pr = q65v::ld(pr), Pi = q65v::ld(pi); const q65v::V Wr = q65v::set1(w4r), Wi = q65v::set1(w4i);
        for (int i = 0; i < n4; i += 4) {
            const q65v::V xv = q65v::ld(x8 + i);
            q65v::st(ib.data() + i, q65v::mul(xv, Pr)); q65v::st(qb.data() + i, q65v::mul(xv, Pi));
            const q65v::V nr = q65v::sub(q65v::mul(Pr, Wr), q65v::mul(Pi, Wi)), ni = q65v::add(q65v::mul(Pr, Wi), q65v::mul(Pi, Wr));
            Pr = nr; Pi = ni;
        }
        q65v::st(pr, Pr); q65v::st(pi, Pi);
        const float c = (float)std::cos(omega), s = (float)-std::sin(omega);   // (once a block, for the tail)
        for (int i = n4; i < n; ++i) {   // the tail one sample at a time: lane 0, then every lane on by one sample
            ib[i] = x8[i] * pr[0]; qb[i] = x8[i] * pi[0];
            for (int k = 0; k < 4; ++k) { const float r = pr[k] * c - pi[k] * s, m = pr[k] * s + pi[k] * c; pr[k] = r; pi[k] = m; }
        }
        if (++renorm >= 32) {   // keep the phasors on the unit circle
            renorm = 0;
            for (int k = 0; k < 4; ++k) { const float m = 1.f / std::sqrt(pr[k] * pr[k] + pi[k] * pi[k]); pr[k] *= m; pi[k] *= m; }
        }
        // the box filter and the half-element sampler (sequential)
        for (int i = 0; i < n; ++i) {
            si += ib[i] - ri[rp]; sq += qb[i] - rq[rp]; ri[rp] = ib[i]; rq[rp] = qb[i]; if (++rp == L) rp = 0;
            t += 1.0;
            if (t >= next) {
                next += step;
                const float e = (float)std::sqrt(si * si + sq * sq);
                col[nh] = e;
                if (++nh == kRows) { nh = 0; column(); }
            }
        }
        if (t > 1e9) { t -= 1e9; next -= 1e9; }
    }
    std::vector<float> d8;
    void feed(const float* in, int n) {
        d8.clear();
        for (int i = 0; i < n; ++i) { acc += in[i]; if (++qn == q) { d8.push_back(acc * qinv); acc = 0; qn = 0; } }
        if (!d8.empty()) block(d8.data(), (int)d8.size());
    }
};

FeldHellDecoder::FeldHellDecoder(double inRate) : d_(new Impl) { d_->self = this; d_->init(inRate); }
FeldHellDecoder::~FeldHellDecoder() = default;
void FeldHellDecoder::feed(const float* x, int n) { d_->feed(x, n); }
float FeldHellDecoder::hz() const { return d_->hzNow; }
bool FeldHellDecoder::signal() const { return d_->sig; }

// ── the test transmitter ─────────────────────────────────────────────────────────────────────────────────────────────
namespace {
// this test's own 5 × 7 drawing (rows top to bottom, '#' = ink), set in a 7-column cell; not anyone's font
struct Glyph { char c; const char* rows[7]; };
const Glyph kFont[] = {
    {'A', {" ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"}}, {'B', {"#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### "}},
    {'C', {" ####", "#    ", "#    ", "#    ", "#    ", "#    ", " ####"}}, {'D', {"#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### "}},
    {'E', {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"}}, {'F', {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "}},
    {'G', {" ####", "#    ", "#    ", "#  ##", "#   #", "#   #", " ####"}}, {'H', {"#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"}},
    {'I', {"#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "#####"}}, {'J', {"  ###", "    #", "    #", "    #", "    #", "#   #", " ### "}},
    {'K', {"#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #"}}, {'L', {"#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"}},
    {'M', {"#   #", "## ##", "# # #", "#   #", "#   #", "#   #", "#   #"}}, {'N', {"#   #", "##  #", "# # #", "#  ##", "#   #", "#   #", "#   #"}},
    {'O', {" ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}}, {'P', {"#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "}},
    {'Q', {" ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #"}}, {'R', {"#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"}},
    {'S', {" ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "}}, {'T', {"#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "}},
    {'U', {"#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}}, {'V', {"#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  "}},
    {'W', {"#   #", "#   #", "#   #", "# # #", "# # #", "## ##", "#   #"}}, {'X', {"#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #"}},
    {'Y', {"#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  "}}, {'Z', {"#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####"}},
    {'0', {" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "}}, {'1', {"  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}},
    {'2', {" ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####"}}, {'3', {"#### ", "    #", "    #", " ### ", "    #", "    #", "#### "}},
    {'4', {"   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "}}, {'5', {"#####", "#    ", "#### ", "    #", "    #", "#   #", " ### "}},
    {'6', {" ### ", "#    ", "#    ", "#### ", "#   #", "#   #", " ### "}}, {'7', {"#####", "    #", "   # ", "  #  ", "  #  ", "  #  ", "  #  "}},
    {'8', {" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "}}, {'9', {" ### ", "#   #", "#   #", " ####", "    #", "    #", " ### "}},
    {'/', {"    #", "    #", "   # ", "  #  ", " #   ", "#    ", "#    "}}, {'?', {" ### ", "#   #", "    #", "   # ", "  #  ", "     ", "  #  "}},
    {'.', {"     ", "     ", "     ", "     ", "     ", "     ", "  #  "}}, {',', {"     ", "     ", "     ", "     ", "     ", "  #  ", " #   "}},
    {' ', {"     ", "     ", "     ", "     ", "     ", "     ", "     "}},
};
const Glyph* glyphOf(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (const Glyph& g : kFont) if (g.c == c) return &g;
    return nullptr;
}
}

std::vector<float> feldHellSynth(const std::string& text, double fs, double audioHz, double clockPpm) {
    // elements: per character 7 columns (1 blank, 5 glyph, 1 blank), each column 7 elements sent BOTTOM FIRST
    std::vector<uint8_t> el;
    for (char ch : text) {
        const Glyph* g = glyphOf(ch); if (!g) continue;
        for (int c = 0; c < 7; ++c)
            for (int r = 6; r >= 0; --r) el.push_back(c >= 1 && c <= 5 && g->rows[r][c - 1] == '#');
    }
    const double perEl = fs / (kElemRate * (1.0 + clockPpm * 1e-6));
    const size_t n = (size_t)std::ceil(el.size() * perEl) + (size_t)fs;
    std::vector<float> out(n, 0.f);
    // keyed with a short raised-cosine edge (≈ 1 ms) so the spectrum stays inside the published ≈ 350 Hz
    const int edge = std::max(1, (int)(fs * 0.001));
    std::vector<float> env(n, 0.f);
    for (size_t i = 0; i < el.size(); ++i) {
        const size_t a = (size_t)(i * perEl), b = (size_t)((i + 1) * perEl);
        for (size_t k = a; k < b && k < n; ++k) env[k] = el[i] ? 1.f : 0.f;
    }
    std::vector<float> sm(n, 0.f); double s = 0;
    for (size_t k = 0; k < n; ++k) { s += env[k]; if (k >= (size_t)edge) s -= env[k - edge]; sm[k] = (float)(s / edge); }
    for (size_t k = 0; k < n; ++k) out[k] = sm[k] * (float)std::sin(2 * M_PI * audioHz * k / fs);
    return out;
}

} // namespace vibe
