// test-varatag.cpp — the VARA HF IDENTIFICATION TAG (android/app/src/main/cpp/vibe_varatag.h; docs/v12/VARA-TAG.md).
//
// ★★★ IDENTIFY ONLY. Nothing here decodes VARA or anything like it. The "VARA-shaped" signals below are GENERIC OFDM
//     built from VARA HF's PUBLISHED outward figures only (occupied width, carrier count, symbol rate — sigidwiki) and
//     the spacing measured on real signals: random QPSK on each carrier, no framing, no coding, nothing of VARA's own.
//
// The capture is simulated the way a VibeServer makes it: a complex capture → the shared Channelizer (fftSizeForRate's
// bins, UNWINDOWED, as emitWideFromBins) → rows averaged FFT_AVG at 10 fps → Tagger::row; the comb's slice is cut from
// the SAME forward FFT by Channelizer::extract (sliceBins), exactly as the shim does.
//
// Cases (each must hold its verdict):
//   POSITIVE — VARA-shaped 2300 / 2750 / 500, in an ARQ burst pattern and continuous, at 47 Hz and 75 Hz wide-FFT bins,
//              with and without a two-path fade: TAGGED, on the right centre, within the test's time.
//   GUARDS (0 labels each) — noise; PACTOR III stand-in (18 tones 120 Hz apart, 100 Bd, ARQ); single carriers: PACTOR IV
//              stand-in 1800 Bd, STANAG 4285 / MIL-STD-188-110 stand-in 2400 Bd; FreeDV 700D / 700E stand-ins; RADE V1
//              stand-in (30 carriers 50 Hz); a 2.3 kHz-wide 50 Hz comb (the spacing guard alone); DRM mode B (our own
//              transmitter, drm_tx.h — its carriers are ALSO 46.875 Hz apart, only the width keeps it out); Olivia 8/500,
//              16/500, 32/1000, 64/2000, MFSK16, Contestia (our transmitter, textmodes.h); MT63 500 / 1000 / 2000 (mt63.h);
//              SSB speech (a recording, if ~/VibeSDR-test-audio/lang is on this machine — else that case is skipped, said so).
//   COMB — measureComb on known ripples (the period it names).
//   COST — stage 1 per row and stage 2 per second of slice, printed.
//
// Silent (no audio device). Bounded: every case is ≤ 150 s of simulated capture at 192 kS/s.
//   g++ -std=c++17 -O2 -I android/app/src/main/cpp vibeserver/test-varatag.cpp <vibedsp channelizer/fft, textmodes, mt63> <pffft, kiss>
#include "vibe_varatag.h"
#include "vibedsp/vibedsp.h"
#include "decoders/textmodes.h"
#include "decoders/mt63.h"
#include "decoders/drm/drm_tx.h"

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

using cf = std::complex<float>;
using vibe::varatag::Tagger;

static int g_fail = 0;
static std::string g_only;   // VIBE_VARA_ONLY=<substring>: run only the cases whose name holds it (debugging)
#define CHECK(c, ...) do { if (!(c)) { std::printf("   FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); g_fail++; } } while (0)

// ── the server's own sizes ───────────────────────────────────────────────────────────────────────────────────────
static int fftSizeForRate(double rate) { double want = rate / 75.0; int s = 4096; while (s < (int)want && s < 32768) s *= 2; return s; }
static constexpr int FFT_AVG = 4;

// ── signal helpers (all at 12 kHz complex unless said) ─────────────────────────────────────────────────────────────
static constexpr double kBb = 12000.0;
/** ×R windowed-sinc polyphase interpolation (Blackman, 16 taps a phase), input Nyquist kept. */
static std::vector<cf> upsample(const std::vector<cf>& in, int R) {
    const int T = 16, N = R * T;
    std::vector<float> h((size_t)N);
    for (int i = 0; i < N; i++) {
        const double t = (i - (N - 1) / 2.0) / R;
        const double s = t == 0 ? 1.0 : std::sin(M_PI * t * 0.9) / (M_PI * t);
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (N - 1)) + 0.08 * std::cos(4 * M_PI * i / (N - 1));
        h[(size_t)i] = (float)(s * w);
    }
    std::vector<cf> up(in.size() * (size_t)R);
    for (size_t n = 0; n < in.size(); n++)
        for (int p = 0; p < R; p++) {
            cf acc(0, 0);
            for (int k = 0; k < T && (long)n - k >= 0; k++) acc += in[n - (size_t)k] * h[(size_t)(k * R + p)];
            up[n * (size_t)R + (size_t)p] = acc;
        }
    return up;
}
/** Real audio (USB at `fs`) → its analytic signal at 12 kHz (a 255-tap Hilbert pair), resampled linearly if fs ≠ 12 k. */
static std::vector<cf> analytic(const std::vector<float>& a, double fs, bool lsb = false) {
    std::vector<float> x;
    if (std::fabs(fs - kBb) < 1) x = a;
    else { const double r = fs / kBb; for (double t = 0; t + 1 < a.size(); t += r) { const size_t i = (size_t)t; const double f = t - i; x.push_back((float)(a[i] * (1 - f) + a[i + 1] * f)); } }
    const int L = 255, c = L / 2;
    std::vector<float> h((size_t)L, 0.0f);
    for (int k = 0; k < L; k++) { const int m = k - c; if (m % 2) h[(size_t)k] = (float)(2.0 / (M_PI * m) * (0.54 - 0.46 * std::cos(2 * M_PI * k / (L - 1)))); }
    std::vector<cf> y(x.size());
    for (size_t n = 0; n < x.size(); n++) {
        double q = 0;
        for (int k = 0; k < L; k++) { const long j = (long)n - k; if (j >= 0) q += h[(size_t)k] * x[(size_t)j]; }
        const float re = n >= (size_t)c ? x[n - c] : 0.0f;
        y[n] = lsb ? cf(re, -(float)q) : cf(re, (float)q);
    }
    return y;
}
/** Mean power of the non-silent part. */
static double power(const std::vector<cf>& s) { double p = 0; size_t n = 0; for (auto& v : s) { const double e = std::norm(v); if (e > 1e-12) { p += e; n++; } } return n ? p / n : 0; }

/** Generic CP-OFDM (random QPSK per carrier per symbol): `nc` carriers `df` apart, `baud` symbols/s, centred on 0 Hz. */
static std::vector<cf> ofdm(int nc, double df, double baud, double secs, uint32_t seed) {
    std::mt19937 rng(seed);
    const size_t n = (size_t)(secs * kBb);
    std::vector<cf> y(n);
    const double tsym = 1.0 / baud, tu = 1.0 / df;
    std::vector<cf> a((size_t)nc);
    double symStart = 0; long sym = -1;
    for (size_t i = 0; i < n; i++) {
        const double t = i / kBb;
        const long s = (long)std::floor(t / tsym);
        if (s != sym) { sym = s; symStart = s * tsym; for (auto& v : a) { const int q = rng() & 3; v = std::polar(1.0f, (float)(M_PI / 4 + q * M_PI / 2)); } }
        const double tl = t - symStart - (tsym - tu);   // the cyclic prefix comes first: tl ∈ [−Tcp, Tu)
        cf acc(0, 0);
        for (int k = 0; k < nc; k++) { const double f = (k - (nc - 1) / 2.0) * df; acc += a[(size_t)k] * std::polar(1.0f, (float)(2 * M_PI * f * tl)); }
        y[i] = acc;
    }
    return y;
}
/** Tones `df` apart with DBPSK-like random phase steps at `baud` and a raised-cosine envelope per symbol (PACTOR III-like). */
static std::vector<cf> toneComb(int nc, double df, double baud, double secs, uint32_t seed) {
    std::mt19937 rng(seed);
    const size_t n = (size_t)(secs * kBb);
    std::vector<cf> y(n);
    std::vector<double> ph((size_t)nc, 0.0);
    long sym = -1; const double tsym = 1.0 / baud;
    for (size_t i = 0; i < n; i++) {
        const double t = i / kBb; const long s = (long)(t / tsym);
        if (s != sym) { sym = s; for (auto& p : ph) if (rng() & 1) p += M_PI; }
        const double u = (t - s * tsym) / tsym, env = 0.5 - 0.5 * std::cos(2 * M_PI * u);
        cf acc(0, 0);
        for (int k = 0; k < nc; k++) { const double f = (k - (nc - 1) / 2.0) * df; acc += std::polar(1.0f, (float)(2 * M_PI * f * t + ph[(size_t)k])); }
        y[i] = acc * (float)std::sqrt(env + 0.05);
    }
    return y;
}
/** One carrier: random 8PSK at `baud`, root-raised-cosine (α) pulses, centred on 0 Hz. */
static std::vector<cf> singleCarrier(double baud, double alpha, double secs, uint32_t seed) {
    std::mt19937 rng(seed);
    const size_t n = (size_t)(secs * kBb);
    const double sps = kBb / baud;
    const int span = 8; const int L = (int)(span * sps) | 1;
    std::vector<double> h((size_t)L);
    for (int k = 0; k < L; k++) {
        const double t = (k - L / 2) / sps;
        double v;
        if (std::fabs(t) < 1e-9) v = 1 - alpha + 4 * alpha / M_PI;
        else if (std::fabs(std::fabs(t) - 1 / (4 * alpha)) < 1e-6) v = alpha / std::sqrt(2) * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * alpha)) + (1 - 2 / M_PI) * std::cos(M_PI / (4 * alpha)));
        else v = (std::sin(M_PI * t * (1 - alpha)) + 4 * alpha * t * std::cos(M_PI * t * (1 + alpha))) / (M_PI * t * (1 - 16 * alpha * alpha * t * t));
        h[(size_t)k] = v;
    }
    std::vector<cf> y(n, cf(0, 0));
    for (double ts = 0; ts < n; ts += sps) {
        const cf a = std::polar(1.0f, (float)((rng() & 7) * M_PI / 4));
        const long c = (long)std::lround(ts);
        for (int k = 0; k < L; k++) { const long j = c + k - L / 2; if (j >= 0 && j < (long)n) y[(size_t)j] += a * (float)h[(size_t)k]; }
    }
    return y;
}
/** ARQ gating: `on` s of A, `gap`, `ack` s of B at `bGain`, `gap`, repeated; ack = 0 → A only, continuous when gap = 0. */
static void arq(std::vector<cf>& a, const std::vector<cf>* b, double on, double ack, double gap, float bGain) {
    const double cyc = on + gap + ack + gap;
    for (size_t i = 0; i < a.size(); i++) {
        const double t = std::fmod(i / kBb, cyc);
        if (t < on) continue;
        if (b && t >= on + gap && t < on + gap + ack) a[i] = (*b)[i % b->size()] * bGain;
        else a[i] = cf(0, 0);
    }
}
/** A slow two-path fade (the second path 1.1 ms late, −3 dB, its phase drifting) — frequency-selective notches. */
static void fade(std::vector<cf>& s) {
    const size_t d = (size_t)(0.0011 * kBb);
    std::vector<cf> o(s.size());
    for (size_t i = 0; i < s.size(); i++) { const cf late = i >= d ? s[i - d] : cf(0, 0); o[i] = s[i] + late * std::polar(0.7f, (float)(2 * M_PI * 0.07 * i / kBb)); }
    s.swap(o);
}

// ── the simulated server ───────────────────────────────────────────────────────────────────────────────────────────
struct Placed { std::vector<cf> bb; double offHz; double snr2500; };   // a 12 kHz signal, where in the capture, its SNR (2.5 kHz)
struct Result { int tags = 0; std::vector<vibe::varatag::Tag> last; double firstTagS = -1; double rowUs = 0, sliceUs = 0; long rows = 0;
                double sliceSecs = 0; Tagger::Stats st; vibe::varatag::CombResult comb; double width = 0; };

/** Run `secs` of a capture at `fs` (complex), noise of unit power a sample, through the server's path. `extra` = a
 *  ready-made capture-rate signal (a recording) added in. */
static Result runCapture(double fs, double secs, const std::vector<Placed>& sigs, uint32_t seed,
                         const std::vector<cf>* extra = nullptr, bool printTrace = false, bool noise = true,
                         std::vector<std::string>* tagLog = nullptr) {
    Result res;
    const int fftSize = fftSizeForRate(fs);
    vibedsp::Channelizer chan(fftSize);
    vibedsp::Channelizer::ExtractCtx ectx;
    Tagger tg;
    const double binHz = fs / fftSize;
    const int R = (int)std::lround(fs / kBb);
    // each signal upsampled once, scaled to its SNR, with its own NCO
    std::vector<std::vector<cf>> up;
    for (auto& s : sigs) {
        std::vector<cf> u = upsample(s.bb, R);
        const double p = power(u);
        const double want = std::pow(10.0, s.snr2500 / 10.0) * 2500.0 / fs;
        const float g = p > 0 ? (float)std::sqrt(want / p) : 0.0f;
        const double w = 2 * M_PI * s.offHz / fs;
        for (size_t i = 0; i < u.size(); i++) u[i] *= g * std::polar(1.0f, (float)std::fmod(w * (double)i, 2 * M_PI));
        up.push_back(std::move(u));
    }
    std::mt19937 rng(seed); std::normal_distribution<float> nd(0.0f, (float)std::sqrt(0.5));
    const size_t n = (size_t)(secs * fs);
    const int chunk = 8192;
    std::vector<cf> buf((size_t)chunk);
    std::vector<float> acc((size_t)fftSize, 0.0f), row((size_t)fftSize);
    int accN = 0, decimCnt = 0;
    const double blockRate = fs / (double)chan.hop();
    const int decim = std::max(1, (int)std::lround(blockRate / (10.0 * FFT_AVG)));
    std::vector<cf> slice;
    double now = 0;
    for (size_t o = 0; o < n; o += chunk) {
        const int k = (int)std::min<size_t>(chunk, n - o);
        for (int i = 0; i < k; i++) {
            cf v = noise ? cf(nd(rng), nd(rng)) : cf(0, 0);
            for (auto& u : up) if (o + i < u.size()) v += u[o + i];
            if (extra && o + i < extra->size()) v += (*extra)[o + i];
            buf[(size_t)i] = v;
        }
        chan.feed(buf.data(), k, [&](const cf* bins, int nb) {
            const long long bi = chan.blockIndex();
            now = (double)(bi * chan.hop()) / fs;
            // ★ the comb's slice, as the shim cuts it
            double c, w;
            if (tg.wantSlice(c, w)) {
                const int sb = vibe::varatag::sliceBins(fs, fftSize, w);
                const int cb = (int)std::lround(c / binHz);   // capture centre = 0 Hz here
                if ((int)slice.size() != sb) slice.assign((size_t)sb, cf(0, 0));
                const int got = chan.extract(bins, cb, sb, slice.data(), ectx, bi);
                const auto t0 = std::chrono::steady_clock::now();
                if (got > 0) { tg.slice(slice.data(), got, fs * sb / fftSize, cb * binHz, now); res.sliceSecs += got / (fs * sb / fftSize); }
                res.sliceUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
            }
            // ★ the wide row, as emitWideFromBins + onSpectrum: unwindowed power, fftshifted, FFT_AVG averaged
            if (++decimCnt < decim) return;
            decimCnt = 0;
            const float sc = 1.0f / ((float)nb * (float)nb);
            for (int i = 0; i < nb; i++) { const cf& z = bins[(i + nb / 2) % nb]; acc[(size_t)i] += 10.0f * std::log10((z.real() * z.real() + z.imag() * z.imag()) * sc + 1e-30f); }
            if (++accN < FFT_AVG) return;
            const auto t0 = std::chrono::steady_clock::now();
            tg.row(acc.data(), 1.0f / accN, nb, 0.0, binHz, now);
            res.rowUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
            res.rows++;
            std::fill(acc.begin(), acc.end(), 0.0f); accN = 0;
            const auto t = tg.tags();
            if (!t.empty() && res.firstTagS < 0) res.firstTagS = now;
            if ((int)t.size() > res.tags) res.tags = (int)t.size();
            if (!t.empty()) res.last = t;
            if (tagLog) {   // every change of the tag set, with the time (real replays)
                static std::string prev; std::string cur;
                for (auto& x : t) { char b[96]; std::snprintf(b, sizeof b, "%.0f/%.0f(%.2f,%.1fHz) ", x.hz, x.bw, x.conf, x.spacing); cur += b; }
                if (cur != prev) { char b[64]; std::snprintf(b, sizeof b, "t=%6.1f s: ", now); tagLog->push_back(std::string(b) + (cur.empty() ? "(none)" : cur)); prev = cur; }
            }
        });
    }
    res.st = tg.stats(); res.comb = tg.lastComb(); res.width = tg.lastWidth();
    if (printTrace) std::printf("   rows %ld, comb evals %lld (pass %lld), last comb best %.2f Hz P %.3f / vara %.2f Hz P %.3f\n",
                                res.rows, res.st.evals, res.st.evalsPass, res.comb.bestHz, res.comb.bestP, res.comb.varaHz, res.comb.varaP);
    return res;
}

static void report(const char* name, const Result& r) {
    std::printf("   %-46s tags %d", name, r.tags);
    if (!r.last.empty()) std::printf(" (at %.0f Hz, %.0f Hz wide, conf %.2f, spacing %.2f, first after %.0f s)", r.last[0].hz, r.last[0].bw, r.last[0].conf, r.last[0].spacing, r.firstTagS);
    std::printf(" | evals %lld pass %lld, comb best %.1f Hz P %.2f (in VARA window %.1f Hz P %.2f)", r.st.evals, r.st.evalsPass, r.comb.bestHz, r.comb.bestP, r.comb.varaHz, r.comb.varaP);
    if (std::getenv("VIBE_VARA_WHY")) std::printf(" | stage-1 rejects: width %lld sides %lld flat %lld fill %lld level %lld (last width %.0f)",
                                                 r.st.rejWidth, r.st.rejSteep, r.st.rejFlat, r.st.rejFill, r.st.rejSnr, r.width);
    std::printf("\n");
}


// ── REAL RECORDINGS (VIBE_VARA_REAL=1): every IQ capture in ~/VibeSDR-test-audio/vara (UberSDR iq192 / iq48, stereo
//    int16 I/Q), replayed through the same path with no noise added; the FreeDV RADE recording as a real look-alike.
static std::vector<cf> readIqWav(const std::string& f, double& rate, double startS, double durS, double& fileS) {
    std::vector<cf> v; FILE* w = std::fopen(f.c_str(), "rb"); if (!w) return v;
    uint8_t h[44]; if (std::fread(h, 1, 44, w) != 44) { std::fclose(w); return v; }
    const int ch = h[22] | (h[23] << 8); rate = h[24] | (h[25] << 8) | (h[26] << 16) | (h[27] << 24);
    if (ch != 2 || rate <= 0) { std::fclose(w); return v; }
    std::fseek(w, 0, SEEK_END); fileS = (std::ftell(w) - 44) / 4.0 / rate;
    std::fseek(w, 44 + 4L * (long)(startS * rate), SEEK_SET);
    const size_t want = (size_t)(durS * rate);
    std::vector<int16_t> b(1 << 16); size_t got;
    while (v.size() < want && (got = std::fread(b.data(), 2, b.size(), w)) > 1)
        for (size_t i = 0; i + 1 < got && v.size() < want; i += 2) v.emplace_back(b[i] / 32768.0f, b[i + 1] / 32768.0f);
    std::fclose(w); return v;
}
static int realReplays() {
    const char* home = std::getenv("HOME");
    const std::string dir = std::string(home ? home : "") + "/VibeSDR-test-audio/vara";
    std::vector<std::string> files;
    if (FILE* p = popen(("ls " + dir + "/*.wav 2>/dev/null").c_str(), "r")) {
        char line[1024]; while (std::fgets(line, sizeof line, p)) { std::string s(line); while (!s.empty() && s.back() == '\n') s.pop_back(); files.push_back(s); }
        pclose(p);
    }
    std::printf("\n[real] %zu recording(s) in %s\n", files.size(), dir.c_str());
    int total = 0;
    for (const auto& f : files) {
        // ★ in 150 s pieces (bounded memory: ≤ ~250 MB at 192 kS/s), each with a fresh tagger
        double fileS = 0;
        for (double st = 0; ; st += 150) {
            double rate = 0; std::vector<cf> x = readIqWav(f, rate, st, 150, fileS);
            if (x.size() < 20 * rate || rate < 40000) { if (st == 0) std::printf("   %s: not a stereo IQ capture — skipped\n", f.c_str()); break; }
            if (rate < 150000) { x = upsample(x, (int)std::lround(192000 / rate)); rate = 192000; }   // ★ a server's coarse bins
            std::vector<float> e(std::min<size_t>(x.size(), 2000000)); for (size_t i = 0; i < e.size(); i++) e[i] = std::norm(x[i * (x.size() / e.size())]);
            std::nth_element(e.begin(), e.begin() + e.size() / 2, e.end()); const float g = e[e.size() / 2] > 0 ? 1.0f / std::sqrt(e[e.size() / 2]) : 1.0f;
            for (auto& v : x) v *= g;
            std::vector<std::string> log;
            const Result r = runCapture(rate, x.size() / rate, {}, 9, &x, false, false, &log);
            const size_t u = f.find("uber_"); const size_t c1 = f.find('_', f.find('_', u + 5) + 1);
            const double fc = std::atof(f.substr(f.find('_', u + 5) + 1, c1).c_str());
            std::printf("   %s [%.0f–%.0f s]: tags at once max %d, comb evals %lld (pass %lld)\n", f.substr(f.find_last_of('/') + 1).c_str(), st, st + x.size() / rate, r.tags, r.st.evals, r.st.evalsPass);
            for (auto& l : log) std::printf("      %s\n", l.c_str());
            for (auto& t : r.last) std::printf("      last tag: %.3f kHz RF, %.0f Hz class, spacing %.2f Hz\n", (fc + t.hz) / 1e3, t.bw, t.spacing);
            total += r.tags;
            if (st + 150 >= fileS) break;
        }
    }
    return total;
}

// VARA HF's PUBLISHED outward shape (sigidwiki): 2300 Hz / up to 49 carriers, 2750 / 59, 500 / 11; ~42 Bd at ~46.9 Hz
static constexpr double kDf = 46.875, kBaud = 42.0;

int main() {
    std::printf("── test-varatag: the VARA HF identification tag (identify only — nothing is decoded) ──\n");
    if (std::getenv("VIBE_VARA_REAL")) { realReplays(); return 0; }
    if (const char* only = std::getenv("VIBE_VARA_ONLY")) g_only = only;   // ★ report only: what is on air is not known in advance
    const double fs = 192000;   // fftSizeForRate → 4096 points, 46.9 Hz bins (an Airspy HF+ / a narrow capture)
    const double fs75 = 307200; // → 4096 points, 75 Hz bins (the server's coarsest, a 2.4 MS/s RTL's)

    // ── COMB: the measurement alone ───────────────────────────────────────────────────────────────────────────
    std::printf("\n[comb] measureComb on known ripples\n");
    {
        const double fb = 2.0; std::vector<float> d(1100);
        for (double sp : { 31.25, 46.875, 50.0, 62.5, 120.0 }) {
            for (size_t i = 0; i < d.size(); i++) d[i] = (float)(1.0 * std::cos(2 * M_PI * i * fb / sp) + 0.3 * std::sin(i * 0.37));
            const auto r = vibe::varatag::measureComb(d.data(), (int)d.size(), fb);
            std::printf("   ripple %6.2f Hz → best %6.2f Hz P %.3f\n", sp, r.bestHz, r.bestP);
            CHECK(std::fabs(r.bestHz - sp) < sp * 0.015, "comb named %.2f for a %.2f Hz ripple", r.bestHz, sp);
        }
        std::mt19937 rng(7); std::normal_distribution<float> nd(0, 1);
        for (auto& v : d) v = nd(rng);
        const auto r = vibe::varatag::measureComb(d.data(), (int)d.size(), fb);
        std::printf("   noise → best %.2f Hz P %.3f (must be < %.2f)\n", r.bestHz, r.bestP, vibe::varatag::kCombMin);
        CHECK(r.bestP < vibe::varatag::kCombMin, "noise comb P %.3f", r.bestP);
    }

    // ── POSITIVES ─────────────────────────────────────────────────────────────────────────────────────────────
    std::printf("\n[positive] VARA-shaped generic OFDM (published width / carriers / baud; random QPSK — nothing of VARA's own)\n");
    struct Pos { const char* name; int nc; double fsr; bool arqOn; bool fadeOn; double snr; double off; };
    const Pos pos[] = {
        { "2300 Hz (49 carriers), ARQ, 47 Hz bins",      49, fs,   true,  false, 15, 21000 },
        { "2750 Hz (59 carriers), ARQ, 47 Hz bins",      59, fs,   true,  false, 15, -33000 },
        { "500 Hz (11 carriers), ARQ, 47 Hz bins",       11, fs,   true,  false, 12, 8000 },
        { "2300 Hz, continuous, 75 Hz bins",             49, fs75, false, false, 15, 40000 },
        { "2300 Hz, ARQ, two-path fade, 75 Hz bins",     49, fs75, true,  true,  18, -52000 },
        { "2750 Hz, ARQ, two-path fade, 47 Hz bins",     59, fs,   true,  true,  18, 61000 },
    };
    for (const auto& p : pos) {
        if (!g_only.empty() && std::string(p.name).find(g_only) == std::string::npos) continue;
        const double secs = 150;
        auto a = ofdm(p.nc, kDf, kBaud, secs, 11);
        auto b = ofdm(p.nc, kDf, kBaud, secs, 12);
        if (p.arqOn) arq(a, &b, 3.6, 0.7, 0.4, 0.5f);
        if (p.fadeOn) fade(a);
        const Result r = runCapture(p.fsr, secs, { { a, p.off, p.snr } }, 1);
        report(p.name, r);
        CHECK(r.tags == 1, "%s: %d tags, want 1", p.name, r.tags);
        if (!r.last.empty()) CHECK(std::fabs(r.last[0].hz - p.off) < 150, "%s: tag at %.0f, signal at %.0f", p.name, r.last[0].hz, p.off);
    }
    // two at once (two ARQ sessions on one capture) and a weak one
    {
        const double secs = 150;
        auto a = ofdm(49, kDf, kBaud, secs, 21), b = ofdm(49, kDf, kBaud, secs, 22);
        auto c = ofdm(59, kDf, kBaud, secs, 23), d = ofdm(59, kDf, kBaud, secs, 24);
        arq(a, &b, 3.6, 0.7, 0.4, 0.5f); arq(c, &d, 2.9, 0.6, 0.5, 0.6f);
        const Result r = runCapture(fs, secs, { { a, -12000, 16 }, { c, 30000, 14 } }, 2);
        report("two sessions at once", r);
        CHECK(r.tags == 2, "two sessions: %d tags", r.tags);
    }

    // ── GUARDS: 0 labels each ─────────────────────────────────────────────────────────────────────────────────
    std::printf("\n[guards] look-alikes and noise — 0 labels each\n");
    int guardLabels = 0;
    auto guard = [&](const char* name, const std::vector<Placed>& s, double fsr = 192000, const std::vector<cf>* extra = nullptr) {
        const Result r = runCapture(fsr, 150, s, 3, extra);
        report(name, r);
        guardLabels += r.tags;
        CHECK(r.tags == 0, "%s: %d false label(s)", name, r.tags);
    };
    guard("noise only", {});
    guard("noise only, 75 Hz bins", {}, fs75);
    {
        auto a = toneComb(18, 120.0, 100.0, 150, 31), b = toneComb(18, 120.0, 100.0, 150, 32);
        arq(a, &b, 0.96, 0.12, 0.085, 0.6f);
        guard("PACTOR III stand-in (18 tones, 120 Hz, ARQ)", { { a, 15000, 18 } });
        auto c = toneComb(18, 120.0, 100.0, 150, 33);
        guard("PACTOR III stand-in, continuous, 75 Hz bins", { { c, 15000, 18 } }, fs75);
    }
    {
        auto a = singleCarrier(1800, 0.25, 150, 41), b = singleCarrier(1800, 0.25, 150, 42);
        arq(a, &b, 0.96, 0.12, 0.085, 0.6f);
        guard("PACTOR IV stand-in (1800 Bd, one carrier, ARQ)", { { a, -20000, 18 } });
        guard("STANAG 4285 / MIL-110 stand-in (2400 Bd)", { { singleCarrier(2400, 0.2, 150, 43), 25000, 18 } });
        guard("1800 Bd one carrier, 75 Hz bins", { { singleCarrier(1800, 0.25, 150, 44), 25000, 18 } }, fs75);
    }
    {
        auto a = ofdm(17, 1 / 0.018, 1 / 0.020, 150, 51); arq(a, nullptr, 6, 0, 2, 0);
        guard("FreeDV 700D stand-in (17 carriers ~56 Hz)", { { a, 9000, 16 } });
        guard("FreeDV 700E stand-in (21 carriers ~72 Hz)", { { ofdm(21, 72.0, 1 / 0.016, 150, 52), 9000, 16 } });
        auto r = ofdm(30, 50.0, 1 / 0.024, 150, 53); arq(r, nullptr, 8, 0, 3, 0);
        guard("RADE V1 stand-in (30 carriers, 50 Hz, 1.5 kHz)", { { r, -9000, 16 } });
        auto w = ofdm(46, 50.0, 1 / 0.024, 150, 54), w2 = ofdm(46, 50.0, 1 / 0.024, 150, 55); arq(w, &w2, 3.6, 0.7, 0.4, 0.5f);
        guard("2.3 kHz-wide 50 Hz comb, ARQ (spacing guard)", { { w, -9000, 16 } });
        auto x = ofdm(73, 31.25, 1 / 0.036, 150, 56), x2 = ofdm(73, 31.25, 1 / 0.036, 150, 57); arq(x, &x2, 3.6, 0.7, 0.4, 0.5f);
        guard("2.3 kHz-wide 31.25 Hz comb, ARQ (spacing guard)", { { x, -9000, 16 } });
    }
    {   // DRM mode B, 10 kHz — our own transmitter (drm_tx.h); its carriers are 46.875 Hz apart, like VARA's
        using namespace vibedrm;
        TxConfig c; c.mode = Mode::B; c.mscLevels = 2; c.sdcLevels = 1;
        Transmitter tx(c);
        std::vector<cf> bb24;
        while (bb24.size() < 150.0 * kFs) { const auto y = tx.superFrame(); bb24.insert(bb24.end(), y.begin(), y.end()); }
        std::vector<cf> bb12;   // 24 k → 12 k would cut a 10 kHz DRM: keep 24 k and place it with its own upsampler
        std::vector<cf> up24 = upsample(bb24, 8);
        const double p = power(up24), want = std::pow(10.0, 20 / 10.0) * 2500.0 / fs; const float g = (float)std::sqrt(want / p);
        const double w0 = 2 * M_PI * 30000.0 / fs;
        for (size_t i = 0; i < up24.size(); i++) up24[i] *= g * std::polar(1.0f, (float)std::fmod(w0 * (double)i, 2 * M_PI));
        guard("DRM mode B 10 kHz (our transmitter)", {}, fs, &up24);
    }
    {
        struct TM { const char* name; vibe::TextModeDecoder::Family f; const char* id; };
        const TM tms[] = {
            { "Olivia 8/500 (our transmitter)",   vibe::TextModeDecoder::Family::Olivia, "olivia-8-500" },
            { "Olivia 16/500",                    vibe::TextModeDecoder::Family::Olivia, "olivia-16-500" },
            { "Olivia 32/1000",                   vibe::TextModeDecoder::Family::Olivia, "olivia-32-1000" },
            { "Olivia 64/2000",                   vibe::TextModeDecoder::Family::Olivia, "olivia-64-2000" },
            { "MFSK16",                           vibe::TextModeDecoder::Family::Mfsk,   "mfsk16" },
        };
        const std::string text(400, 'E');
        for (const auto& t : tms) {
            std::vector<float> au = vibe::textModeSynth(t.f, t.id, text + " CQ CQ TEST DE VIBE " + text, 12000, 1500, 9);
            while (au.size() < 150 * 12000) au.insert(au.end(), au.begin(), au.begin() + std::min(au.size(), (size_t)150 * 12000 - au.size()));
            guard(t.name, { { analytic(au, 12000), 18000, 16 } });
        }
        for (int bw : { 500, 1000, 2000 }) {
            std::vector<float> au = vibe::mt63Synth(bw, false, text + " CQ CQ TEST DE VIBE " + text, 12000, bw == 2000 ? 500 : 1000, 3);
            while (au.size() < 150 * 12000) au.insert(au.end(), au.begin(), au.begin() + std::min(au.size(), (size_t)150 * 12000 - au.size()));
            char nm[64]; std::snprintf(nm, sizeof nm, "MT63-%d (our transmitter)", bw);
            guard(nm, { { analytic(au, 12000), -27000, 16 } });
        }
    }
    {   // ★ REAL: Stuart's UberSDR, 40 m LSB 2026-11-16 — RADE V1 (CT1DHR's over, 470–522 s) among SSB speech (FREEDV.md)
        const char* home = std::getenv("HOME");
        const std::string f = std::string(home ? home : "") + "/VibeSDR-test-audio/freedv/20261116T1655_7065000_lsb.wav";
        if (FILE* w = std::fopen(f.c_str(), "rb")) {
            std::fseek(w, 44 + 2L * 12000 * 440, SEEK_SET);
            std::vector<int16_t> s16((size_t)12000 * 150); const size_t got = std::fread(s16.data(), 2, s16.size(), w); std::fclose(w);
            std::vector<float> au(got); for (size_t i = 0; i < got; i++) au[i] = s16[i] / 32768.0f;
            guard("REAL RADE V1 + SSB speech (40 m LSB, 440–590 s)", { { analytic(au, 12000, true), 5000, 20 } });
        } else std::printf("   REAL RADE V1 + SSB speech: SKIPPED (%s not on this machine)\n", f.c_str());
    }
    std::printf("   guard labels in total: %d\n", guardLabels);

    // ── COST ─────────────────────────────────────────────────────────────────────────────────────────────────
    std::printf("\n[cost] stage 1 per row, stage 2 per second of slice (this machine)\n");
    {
        auto a = ofdm(49, kDf, kBaud, 120, 61), b = ofdm(49, kDf, kBaud, 120, 62); arq(a, &b, 3.6, 0.7, 0.4, 0.5f);
        const Result r = runCapture(fs75, 120, { { a, 20000, 15 } }, 4);
        const Result q = runCapture(1228800, 30, {}, 5);   // 16384-point rows, nothing to find
        std::printf("   rows at 4096 bins: %.1f us/row; at 16384 bins (quiet): %.1f us/row; stage 2: %.1f us per second of slice (%.0f s sliced)\n",
                    r.rowUs / std::max(1L, r.rows), q.rowUs / std::max(1L, q.rows), r.sliceSecs > 0 ? r.sliceUs / r.sliceSecs : 0.0, r.sliceSecs);
        std::printf("   ★ stage 1 runs at ≤ 4 rows/s: %.3f %% of this core at 16384 bins\n", 4 * q.rowUs / std::max(1L, q.rows) / 1e4);
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
