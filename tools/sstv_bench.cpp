// sstv_bench — the SSTV decoder's PICTURE QUALITY on synthetic transmissions with known content,
// noise, sender clock error and fading (2026-10-05).
//
//   build: see tools/sstv_harness.cpp (same objects), then
//   sstv_bench [--img truth.ppm] [--modes M1,M2,S1,S2,R36,R72,PD50,PD120] [--snr 20,10,5,3,0]
//              [--ppm 0] [--fade 0|1] [--seeds 1] [--out dir]      score a matrix of pictures
//   sstv_bench --noise SECONDS [--seeds N]                          false starts on band noise
//
// ★★★ WHY A SECOND TOOL. vibeserver/test-sstv-quality asserts GEOMETRY (slant, offset, bar colour)
//     at 20 dB, where every demodulator looks the same. What a listener on 20 m actually sees is a
//     picture at 0–10 dB, and there the decoder's choices — how long a window, how often an
//     estimate — decide between a picture you can read and a box of streaks (Stuart's PD50 at
//     15:41 UTC 2026-10-05: UberSDR's speckle still read "HA7BJ", ours had smeared it out). That is
//     a number too, given a known picture: this scores
//       • RMSE (levels, RGB) and SSIM (luma, 8×8) against the truth after the best integer
//         alignment (±16 px, ±2 lines) — so it measures the picture, not the offset (that is
//         test-sstv-quality's job); the alignment found is reported as dx;
//       • STREAK: the error image's horizontal correlation at 3 px. Speckle (independent errors) is
//         ~0; a window held across many pixels is near 1. The eye reads through speckle, not streaks.
// ★★ THE CHANNEL IS 20 M'S: 12 kHz audio (what UberSDR records and what the host hands the
//    decoder), noise band-limited to an SSB passband (300–2700 Hz), SNR in 2500 Hz. The sender's
//    sound card is modelled as test-sstv-quality does: every duration AND every tone scaled by
//    (1+ppm). --fade 1 adds two-path Watterson fading (0 and 1 ms, 0.5 Hz spread each), the
//    ITU-R F.1487 "moderate" channel's shape.
#include "decoders/sstv_decoder.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace vibe;

struct Img { int W = 0, H = 0; std::vector<uint8_t> rgb; const uint8_t* px(int x, int y) const { return &rgb[((size_t)y*W + x)*3]; } };

static bool readPpm(const char* p, Img& im) {
    FILE* f = fopen(p, "rb"); if (!f) return false;
    int maxv; char m[3] = {0};
    if (fscanf(f, "%2s %d %d %d", m, &im.W, &im.H, &maxv) != 4 || strcmp(m, "P6")) { fclose(f); return false; }
    fgetc(f);
    im.rgb.resize((size_t)im.W*im.H*3);
    const size_t n = fread(im.rgb.data(), 1, im.rgb.size(), f); fclose(f);
    return n == im.rgb.size();
}
static void writePpm(const std::string& p, const Img& im) {
    FILE* f = fopen(p.c_str(), "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", im.W, im.H); fwrite(im.rgb.data(), 1, im.rgb.size(), f); fclose(f);
}
/** Nearest-neighbour resample — the truth for a mode whose frame differs from the source picture. */
static Img resized(const Img& s, int W, int H) {
    Img d; d.W = W; d.H = H; d.rgb.resize((size_t)W*H*3);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
        std::memcpy(&d.rgb[((size_t)y*W + x)*3], s.px(x * s.W / W, y * s.H / H), 3);
    return d;
}
/** Default content: colour bars over a checker, and white "text-sized" blocks — 2–6 px strokes. */
static Img card(int W, int H) {
    static const uint8_t bars[8][3] = {{255,255,255},{255,255,0},{0,255,255},{0,255,0},{255,0,255},{255,0,0},{0,0,255},{0,0,0}};
    Img c; c.W = W; c.H = H; c.rgb.resize((size_t)W*H*3);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t* p = &c.rgb[((size_t)y*W + x)*3];
        if (y < H/2) { const uint8_t* b = bars[x*8/W]; p[0]=b[0]; p[1]=b[1]; p[2]=b[2]; }
        else { const int s = 2 + (x * 5 / W); const uint8_t v = (((x / s) + (y / s)) & 1) ? 230 : 20; p[0]=p[1]=p[2]=v; }
    }
    return c;
}

// ── Modes and the transmitter ────────────────────────────────────────────────────────────────
struct Seg { double f, dur; };
static double lumHz(double v) { return 1500.0 + v * 3.1372549; }
static void yuv(const uint8_t* p, double& Y, double& U, double& V) {
    const double R = p[0], G = p[1], B = p[2];
    Y = 16.0  + ( 65.738*R + 129.057*G +  25.064*B) / 256.0;
    U = 128.0 + (-37.945*R -  74.494*G + 112.439*B) / 256.0;
    V = 128.0 + (112.439*R -  94.154*G -  18.285*B) / 256.0;
}
struct ModeDef { const char* tag; int vis, W, H; };
static const ModeDef kDefs[] = {
    {"M1",44,320,256},{"M2",40,320,256},{"S1",60,320,256},{"S2",56,320,256},
    {"R36",8,320,240},{"R72",12,320,240},{"PD50",93,320,256},{"PD120",95,640,496},
};
static const ModeDef* defOf(const std::string& t) { for (auto& d : kDefs) if (t == d.tag) return &d; return nullptr; }

static std::vector<Seg> schedule(const std::string& m, const Img& c) {
    std::vector<Seg> s; auto add = [&](double f, double d) { s.push_back({f, d}); };
    const ModeDef* d = defOf(m);
    add(1900, 0.300); add(1200, 0.010); add(1900, 0.300); add(1200, 0.030);
    int par = 0;
    for (int b = 0; b < 7; b++) { const int bit = (d->vis >> b) & 1; par ^= bit; add(bit ? 1100 : 1300, 0.030); }
    add(par ? 1100 : 1300, 0.030); add(1200, 0.030);
    const int W = c.W, H = c.H;
    auto chan = [&](int y, int ch, double pxT) { for (int x = 0; x < W; x++) add(lumHz(c.px(x, y)[ch]), pxT); };
    if (m == "M1" || m == "M2") {
        const double pt = m == "M1" ? 0.4576e-3 : 0.2288e-3;
        for (int y = 0; y < H; y++) {
            add(1200, 4.862e-3); add(1500, 0.572e-3);
            chan(y, 1, pt); add(1500, 0.572e-3); chan(y, 2, pt); add(1500, 0.572e-3); chan(y, 0, pt); add(1500, 0.572e-3);
        }
    } else if (m == "S1" || m == "S2") {
        const double pt = m == "S1" ? 0.432e-3 : 0.2752e-3;
        add(1200, 9e-3);
        for (int y = 0; y < H; y++) {
            add(1500, 1.5e-3); chan(y, 1, pt); add(1500, 1.5e-3); chan(y, 2, pt);
            add(1200, 9e-3); add(1500, 1.5e-3); chan(y, 0, pt);
        }
    } else if (m == "R36") {
        for (int y = 0; y < H; y++) {
            add(1200, 9e-3); add(1500, 3e-3);
            for (int x = 0; x < W; x++) { double Y, U, V; yuv(c.px(x, y), Y, U, V); add(lumHz(Y), 0.275e-3); }
            add((y & 1) ? 2300 : 1500, 4.5e-3); add(1900, 1.5e-3);
            for (int x = 0; x < W; x += 2) {
                double Y, U0, V0, U1, V1; yuv(c.px(x, y), Y, U0, V0); yuv(c.px(x+1, y), Y, U1, V1);
                add(lumHz((y & 1) ? (U0+U1)/2 : (V0+V1)/2), 0.275e-3);
            }
        }
    } else if (m == "R72") {
        // The Dayton spec (and MMSSTV): 9 sync, 3 porch, Y 138 ms, 4.5 sep + 1.5 porch, R-Y 69 ms,
        // 4.5 sep + 1.5 porch, B-Y 69 ms = 300 ms. Chroma at half horizontal resolution, every line.
        for (int y = 0; y < H; y++) {
            add(1200, 9e-3); add(1500, 3e-3);
            for (int x = 0; x < W; x++) { double Y, U, V; yuv(c.px(x, y), Y, U, V); add(lumHz(Y), 138e-3 / W); }
            add(1500, 4.5e-3); add(1900, 1.5e-3);
            for (int x = 0; x < W; x += 2) { double Y, U0, V0, U1, V1; yuv(c.px(x, y), Y, U0, V0); yuv(c.px(x+1, y), Y, U1, V1); add(lumHz((V0+V1)/2), 138e-3 / W); }
            add(2300, 4.5e-3); add(1500, 1.5e-3);
            for (int x = 0; x < W; x += 2) { double Y, U0, V0, U1, V1; yuv(c.px(x, y), Y, U0, V0); yuv(c.px(x+1, y), Y, U1, V1); add(lumHz((U0+U1)/2), 138e-3 / W); }
        }
    } else {   // PD
        const double pt = m == "PD50" ? 0.286e-3 : 0.19e-3;
        for (int y = 0; y < H; y += 2) {
            add(1200, 20e-3); add(1500, 2.08e-3);
            double Y, U, V, Y2, U2, V2;
            for (int x = 0; x < W; x++) { yuv(c.px(x, y), Y, U, V); add(lumHz(Y), pt); }
            for (int x = 0; x < W; x++) { yuv(c.px(x, y), Y, U, V); yuv(c.px(x, y+1), Y2, U2, V2); add(lumHz((V+V2)/2), pt); }
            for (int x = 0; x < W; x++) { yuv(c.px(x, y), Y, U, V); yuv(c.px(x, y+1), Y2, U2, V2); add(lumHz((U+U2)/2), pt); }
            for (int x = 0; x < W; x++) { yuv(c.px(x, y+1), Y, U, V); add(lumHz(Y), pt); }
        }
    }
    return s;
}

// ── The channel ──────────────────────────────────────────────────────────────────────────────
static const double FS = 12000.0;
/** Band-limited noise: white through a 300–2700 Hz windowed-sinc band-pass, scaled so its power
 *  IN 2500 Hz is `pn`. */
static std::vector<double> bandNoise(size_t n, double pn, std::mt19937& rng) {
    const int T = 129; std::vector<double> h(T);
    double f1 = 300 / FS, f2 = 2700 / FS;
    for (int i = 0; i < T; i++) {
        const double k = i - (T - 1) / 2.0;
        const double w = 0.42 - 0.5*std::cos(2*M_PI*i/(T-1)) + 0.08*std::cos(4*M_PI*i/(T-1));
        h[i] = (k == 0 ? 2*(f2 - f1) : (std::sin(2*M_PI*f2*k) - std::sin(2*M_PI*f1*k)) / (M_PI*k)) * w;
    }
    // white variance so the band-pass output carries pn/2500 per Hz over its 2400 Hz
    const double sig = std::sqrt(pn / 2500.0 * (FS / 2.0));
    std::normal_distribution<double> g(0.0, sig);
    std::vector<double> w(n + T), o(n);
    for (auto& v : w) v = g(rng);
    for (size_t i = 0; i < n; i++) { double a = 0; for (int k = 0; k < T; k++) a += h[k] * w[i + k]; o[i] = a; }
    return o;
}
/** One Watterson path gain: complex Gaussian, Gaussian Doppler spectrum ~0.5 Hz, unit power. */
static std::vector<std::complex<double>> fadeGain(size_t n, double spreadHz, std::mt19937& rng) {
    // two-pole low-pass on complex white noise at a 100 Hz control rate, interpolated
    const int dec = 120; const size_t m = n / dec + 4;
    std::normal_distribution<double> g(0.0, 1.0);
    const double a = std::exp(-2*M_PI*spreadHz / (FS / dec));
    std::vector<std::complex<double>> c(m); std::complex<double> s1 = 0, s2 = 0;
    for (int i = 0; i < 2000; i++) { s1 = a*s1 + (1-a)*std::complex<double>(g(rng), g(rng)); s2 = a*s2 + (1-a)*s1; }
    double p = 0;
    for (size_t i = 0; i < m; i++) { s1 = a*s1 + (1-a)*std::complex<double>(g(rng), g(rng)); s2 = a*s2 + (1-a)*s1; c[i] = s2; p += std::norm(s2); }
    p /= m; for (auto& v : c) v /= std::sqrt(p);
    std::vector<std::complex<double>> o(n);
    for (size_t i = 0; i < n; i++) { const size_t k = i / dec; const double f = (double)(i % dec) / dec; o[i] = c[k]*(1-f) + c[k+1]*f; }
    return o;
}
static std::vector<int16_t> transmit(const std::vector<Seg>& sc, double ppm, double snrDb, bool fade, unsigned seed,
                                     double pre = 1.0, double post = 3.0) {
    std::mt19937 rng(seed);
    const double k = 1.0 + ppm * 1e-6, A = 1.0;
    double total = 0; for (auto& g : sc) total += g.dur;
    const size_t N0 = (size_t)(pre * FS), N = (size_t)(total / k * FS), N1 = (size_t)(post * FS);
    std::vector<double> ph(N0 + N + N1, 0.0); std::vector<uint8_t> on(ph.size(), 0);
    double p = 0, segEnd = sc[0].dur; size_t si = 0;
    for (size_t i = 0; i < N; i++) {
        const double tau = (double)i / FS * k;
        while (si + 1 < sc.size() && tau >= segEnd) { si++; segEnd += sc[si].dur; }
        p += 2.0 * M_PI * sc[si].f * k / FS; if (p > 2*M_PI) p -= 2*M_PI;
        ph[N0 + i] = p; on[N0 + i] = sc[si].f > 0;
    }
    std::vector<double> x(ph.size(), 0.0);
    if (!fade) { for (size_t i = 0; i < x.size(); i++) if (on[i]) x[i] = A * std::sin(ph[i]); }
    else {
        const auto g1 = fadeGain(x.size(), 0.5, rng), g2 = fadeGain(x.size(), 0.5, rng);
        const size_t D = (size_t)(1e-3 * FS);
        for (size_t i = 0; i < x.size(); i++) {
            std::complex<double> s = 0;
            if (on[i]) s += g1[i] * std::polar(1.0, ph[i]);
            if (i >= D && on[i - D]) s += g2[i] * std::polar(1.0, ph[i - D]);
            x[i] = A * std::imag(s) / std::sqrt(2.0);
        }
    }
    const double ps = A * A / 2.0;
    const auto nz = bandNoise(x.size(), ps / std::pow(10.0, snrDb / 10.0), rng);
    std::vector<int16_t> o(x.size());
    for (size_t i = 0; i < x.size(); i++) { const double v = (x[i] + nz[i]) * 6000.0; o[i] = (int16_t)std::max(-32767.0, std::min(32767.0, v)); }
    return o;
}

// ── Decode, paced like the harness (pendingSamples) ──────────────────────────────────────────
struct Pic { int W = 0, H = 0; std::vector<uint8_t> img; bool redrew = false; std::string status; };
// ★ every decode the decoder began, and every one it dropped as "no picture followed" (a false VIS)
static int gIgnored = 0, gDecodes = 0; static size_t gFed = 0;
static std::vector<Pic> decodeAll(const std::vector<int16_t>& pcm) {
    if (const char* wp = getenv("SSTV_BENCH_WAV")) { FILE* f = fopen(wp, "wb"); uint32_t n = (uint32_t)pcm.size() * 2, r = 12000, br = 24000, c16 = 16, sz = 36 + n; uint16_t one = 1, ba = 2, bits = 16; fwrite("RIFF", 1, 4, f); fwrite(&sz, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&c16, 4, 1, f); fwrite(&one, 2, 1, f); fwrite(&one, 2, 1, f); fwrite(&r, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&n, 4, 1, f); fwrite(pcm.data(), 2, pcm.size(), f); fclose(f); }
    std::vector<Pic> pics; std::mutex mu;
    SstvDecoder dec(FS, true, true);
    dec.onImageStart = [&](int w, int h) { std::lock_guard<std::mutex> l(mu); Pic p; p.W = w; p.H = h; p.img.assign((size_t)w*h*3, 0); pics.push_back(p); };
    dec.onLine = [&](int y, int w, const uint8_t* rgb) { std::lock_guard<std::mutex> l(mu); if (pics.empty()) return; auto& p = pics.back(); if (y >= 0 && y < p.H && w == p.W) std::memcpy(&p.img[(size_t)y*w*3], rgb, (size_t)w*3); };
    dec.onRedrawStart = [&]() { std::lock_guard<std::mutex> l(mu); if (!pics.empty()) pics.back().redrew = true; };
    dec.onStatus = [&](const std::string& s) { std::lock_guard<std::mutex> l(mu); if (getenv("SSTV_DBG")) fprintf(stderr, "STATUS @fed %zu %s\n", gFed, s.c_str()); if (s.rfind("ignored", 0) == 0) gIgnored++; if (s.rfind("Decoding", 0) == 0) gDecodes++; if (!pics.empty() && s.rfind("Decoding", 0) != 0) { auto& st = pics.back().status; if (!st.empty()) st += " | "; st += s; } };
    const int B = 120;
    for (size_t i = 0; i < pcm.size(); i += B) {
        while (dec.pendingSamples() > 1500) std::this_thread::yield();
        dec.process(&pcm[i], (int)std::min<size_t>(B, pcm.size() - i)); gFed = i + B;
    }
    std::vector<int16_t> z(B, 0);
    for (int k = 0; k < 300; k++) { while (dec.pendingSamples() > 1500) std::this_thread::yield(); dec.process(z.data(), B); }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::lock_guard<std::mutex> l(mu);
    return pics;
}

// ── Scoring ──────────────────────────────────────────────────────────────────────────────────
static double luma(const uint8_t* p) { return 0.299*p[0] + 0.587*p[1] + 0.114*p[2]; }
struct Score { double rmse = NAN, ssim = NAN, streak = NAN; int dx = 0, dy = 0; };
static Score score(const Img& t, const std::vector<uint8_t>& d) {
    Score s; const int W = t.W, H = t.H;
    std::vector<double> lt(W*H), ld(W*H);
    for (int i = 0; i < W*H; i++) { lt[i] = luma(&t.rgb[i*3]); ld[i] = luma(&d[i*3]); }
    // best integer alignment on luma (decoded pixel (x+dx, y+dy) shows truth (x, y))
    double best = 1e300; const int M = 16;
    for (int dy = -2; dy <= 2; dy++) for (int dx = -M; dx <= M; dx++) {
        double e = 0; int n = 0;
        for (int y = 4; y < H - 4; y += 2) for (int x = M; x < W - M; x += 2) { const double v = ld[(y+dy)*W + x+dx] - lt[y*W + x]; e += v*v; n++; }
        if (e / n < best) { best = e / n; s.dx = dx; s.dy = dy; }
    }
    double se = 0; int n = 0;
    std::vector<double> err(W*H, 0.0); std::vector<uint8_t> in(W*H, 0);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        const int xx = x + s.dx, yy = y + s.dy; if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
        for (int c = 0; c < 3; c++) { const double v = (double)d[((size_t)yy*W + xx)*3 + c] - t.rgb[((size_t)y*W + x)*3 + c]; se += v*v; n++; }
        err[y*W + x] = ld[yy*W + xx] - lt[y*W + x]; in[y*W + x] = 1;
    }
    s.rmse = std::sqrt(se / std::max(1, n));
    // SSIM, 8×8 windows on a 4-px grid
    const double C1 = 6.5025, C2 = 58.5225; double ss = 0; int nw = 0;
    for (int y = 0; y + 8 <= H; y += 4) for (int x = 0; x + 8 <= W; x += 4) {
        double ma = 0, mb = 0, va = 0, vb = 0, cv = 0; int k = 0; bool okw = true;
        for (int j = 0; j < 8 && okw; j++) for (int i = 0; i < 8; i++) {
            const int xx = x + i + s.dx, yy = y + j + s.dy; if (xx < 0 || yy < 0 || xx >= W || yy >= H) { okw = false; break; }
            const double a = lt[(y+j)*W + x+i], b = ld[yy*W + xx]; ma += a; mb += b; va += a*a; vb += b*b; cv += a*b; k++;
        }
        if (!okw) continue;
        ma /= k; mb /= k; va = va/k - ma*ma; vb = vb/k - mb*mb; cv = cv/k - ma*mb;
        ss += ((2*ma*mb + C1)*(2*cv + C2)) / ((ma*ma + mb*mb + C1)*(va + vb + C2)); nw++;
    }
    s.ssim = nw ? ss / nw : NAN;
    // streak: horizontal correlation of the error at a 3-px lag, mean removed per row
    double sxy = 0, sxx = 0;
    for (int y = 0; y < H; y++) {
        double m = 0; int c = 0; for (int x = 0; x < W; x++) if (in[y*W + x]) { m += err[y*W + x]; c++; }
        if (c < 8) continue; m /= c;
        for (int x = 0; x + 3 < W; x++) if (in[y*W + x] && in[y*W + x + 3]) {
            const double a = err[y*W + x] - m, b = err[y*W + x + 3] - m; sxy += a*b; sxx += a*a;
        }
    }
    s.streak = sxx > 0 ? sxy / sxx : NAN;
    return s;
}

static std::vector<std::string> split(const std::string& s) { std::vector<std::string> o; size_t a = 0; for (;;) { size_t b = s.find(',', a); o.push_back(s.substr(a, b - a)); if (b == std::string::npos) break; a = b + 1; } return o; }

int main(int argc, char** argv) {
    std::string imgPath, outDir; std::vector<std::string> modes = {"M1","M2","S1","S2","R36","PD50","PD120"};
    std::vector<double> snrs = {20, 10, 6, 3, 0}, ppms = {0}; int fade = 0, seeds = 1, abandon = 0, visTrials = 0, noVis = 0; double noiseSec = 0, gapSec = 2.0;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i]; auto nx = [&]() { return std::string(i + 1 < argc ? argv[++i] : ""); };
        if (a == "--img") imgPath = nx();
        else if (a == "--modes") modes = split(nx());
        else if (a == "--snr") { snrs.clear(); for (auto& v : split(nx())) snrs.push_back(atof(v.c_str())); }
        else if (a == "--ppm") { ppms.clear(); for (auto& v : split(nx())) ppms.push_back(atof(v.c_str())); }
        else if (a == "--fade") fade = atoi(nx().c_str());
        else if (a == "--seeds") seeds = atoi(nx().c_str());
        else if (a == "--out") outDir = nx();
        else if (a == "--vis") visTrials = atoi(nx().c_str());
        else if (a == "--novis") noVis = 1;
        else if (a == "--noise") noiseSec = atof(nx().c_str());
        else if (a == "--abandon") { abandon = 1; gapSec = atof(nx().c_str()); }
    }
    if (noiseSec > 0) {
        // ★ False starts: band noise alone, at a level where a real picture would be at ~3 dB.
        int starts = 0; double total = 0;
        for (int sd = 0; sd < seeds; sd++) {
            std::mt19937 rng(777 + sd);
            const auto nz = bandNoise((size_t)(noiseSec * FS), 0.5 / 2.0, rng);
            std::vector<int16_t> pcm(nz.size()); for (size_t i = 0; i < nz.size(); i++) pcm[i] = (int16_t)std::max(-32767.0, std::min(32767.0, nz[i] * 6000.0));
            const auto pics = decodeAll(pcm); starts += (int)pics.size(); total += noiseSec;
            for (auto& p : pics) printf("  noise start: %dx%d  %s\n", p.W, p.H, p.status.c_str());
        }
        printf("noise: %d picture starts in %.0f s (%.2f / hour); %d decodes begun, %d dropped as no picture\n", starts, total, starts * 3600.0 / total, gDecodes, gIgnored);
        return 0;
    }
    Img src; const bool haveImg = !imgPath.empty() && readPpm(imgPath.c_str(), src);
    if (visTrials > 0) {
        // ★ How often is a picture STARTED at all? VIS + the first 60 lines of each mode, `visTrials`
        //   different noise/fading draws per SNR. (The VIS-by-energy measurement, 2026-10-05.)
        printf("mode\tsnr\tfade\tstarted\n");
        for (auto& m : modes) for (double snr : snrs) {
            const ModeDef* d = defOf(m);
            const Img truth = haveImg ? resized(src, d->W, d->H) : card(d->W, d->H);
            auto sc = schedule(m, truth);
            int ok = 0;
            for (int t = 0; t < visTrials; t++) {
                auto part = sc; part.resize(15 + (sc.size() - 15) / 6);   // VIS + the first sixth of the picture
                const auto pcm = transmit(part, 0, snr, fade != 0, 99991 + t * 131 + (unsigned)(snr * 7));
                ok += decodeAll(pcm).empty() ? 0 : 1;
            }
            printf("%s\t%.0f\t%d\t%d/%d\n", m.c_str(), snr, fade, ok, visTrials);
            fflush(stdout);
        }
        return 0;
    }
    if (abandon) {
        // ★ An abandoned picture, then the real one: the sender stops mode A a third of the way in,
        //   waits `gap` s and sends mode B. Was B decoded, and how well? (VIS-during-decode, 2026-10-05)
        printf("first\tsecond\tsnr\tgap\tpics\tsecond_found\trmse\tssim\tstatus\n");
        for (size_t a = 0; a < modes.size(); a++) for (size_t b = 0; b < modes.size(); b++) for (double snr : snrs) {
            const ModeDef* da = defOf(modes[a]); const ModeDef* db = defOf(modes[b]);
            const Img ta = haveImg ? resized(src, da->W, da->H) : card(da->W, da->H);
            const Img tb = haveImg ? resized(src, db->W, db->H) : card(db->W, db->H);
            auto sa = schedule(modes[a], ta); sa.resize(sa.size() / 3);
            sa.push_back({0.0, gapSec});   // silence (0 Hz) — the noise carries on over it
            auto sb = schedule(modes[b], tb);
            sa.insert(sa.end(), sb.begin(), sb.end());
            const auto pcm = transmit(sa, 0, snr, fade != 0, 4321 + (unsigned)(a * 31 + b * 7));
            const auto pics = decodeAll(pcm);
            int found = -1;
            for (size_t k = 0; k < pics.size(); k++) if (pics[k].W == db->W && pics[k].H == db->H && (k > 0 || modes[a] != modes[b])) found = (int)k;
            if (modes[a] == modes[b] && pics.size() >= 2) found = (int)pics.size() - 1;
            Score s; if (found >= 0) s = score(tb, pics[found].img);
            printf("%s\t%s\t%.0f\t%.1f\t%zu\t%s\t%.1f\t%.3f\t%s\n", modes[a].c_str(), modes[b].c_str(), snr, gapSec, pics.size(),
                   found >= 0 ? "yes" : "NO", s.rmse, s.ssim, found >= 0 ? pics[found].status.c_str() : "");
            fflush(stdout);
        }
        return 0;
    }
    printf("mode\tsnr\tppm\tfade\tseed\tpics\trmse\tssim\tstreak\tdx\tdy\tstatus\n");
    for (auto& m : modes) {
        const ModeDef* d = defOf(m); if (!d) { fprintf(stderr, "unknown mode %s\n", m.c_str()); continue; }
        const Img truth = haveImg ? resized(src, d->W, d->H) : card(d->W, d->H);
        for (double ppm : ppms) for (double snr : snrs) for (int sd = 0; sd < seeds; sd++) {
            auto sched = schedule(m, truth);
            if (noVis) sched.erase(sched.begin(), sched.begin() + 13);   // ★ the VIS lost (13 segments)
            const auto pcm = transmit(sched, ppm, snr, fade != 0, 1234 + sd * 7919 + (unsigned)(snr * 10));
            const auto pics = decodeAll(pcm);
            Score s; std::string st = "NO PICTURE";
            if (!pics.empty() && pics[0].W == d->W && pics[0].H == d->H) { s = score(truth, pics[0].img); st = pics[0].status; }
            printf("%s\t%.0f\t%.0f\t%d\t%d\t%zu\t%.1f\t%.3f\t%.3f\t%d\t%d\t%s\n", m.c_str(), snr, ppm, fade, sd, pics.size(),
                   s.rmse, s.ssim, s.streak, s.dx, s.dy, st.c_str());
            fflush(stdout);
            if (!outDir.empty() && !pics.empty()) {
                char nm[256]; snprintf(nm, sizeof nm, "%s/%s_snr%.0f_ppm%.0f_f%d_s%d.ppm", outDir.c_str(), m.c_str(), snr, ppm, fade, sd);
                Img o; o.W = pics[0].W; o.H = pics[0].H; o.rgb = pics[0].img; writePpm(nm, o);
            }
        }
    }
    return 0;
}
