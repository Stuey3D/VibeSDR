// nfm_probe — would ANY fixed narrower IF help NFM? (V12 auto bandwidth, docs/v12/AM-SYNC.md §11)
// If no fixed width beats the listener's 12.5 kHz on a measured signal, an automatic one cannot either, so this is
// the gate on building one. A pseudo-voice (vibeserver/test-auto-bw.cpp's generator, 300-3000 Hz) frequency-modulated
// at ±2.5 kHz peak (12.5 kHz channel) or ±5 kHz (25 kHz channel), in white noise at several CNRs (in 12.5 kHz), with
// and without a neighbour one channel away; demodulated through the real RxPipeline in NFM at 12.5 kHz and at
// narrower widths; scored against the known audio with abscore's coherence score over 300-3000 Hz (FM's detector is
// not linear, so here the per-frequency S/N CAN move and the coherence score is the right one).
// ★ SILENT. Build: like vibeserver/test-auto-bw.cpp.
#include "vibedsp.h"
#include "autobw_score.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace vibedsp;
static const double FS = 48000.0, FC = 0.0;
static std::vector<double> voiceAudio(double secs, unsigned seed) {
    std::mt19937 rng(seed); std::uniform_real_distribution<double> U(0, 1);
    const size_t n = (size_t)(secs * FS); std::vector<double> x(n);
    double ph[40] = {0}, f0 = 120, f0T = 120, F1 = 600, F1T = 600, F2 = 1500, F2T = 1500, g = 0, gT = 1, nextSyl = 0, nextPitch = 0;
    for (size_t i = 0; i < n; ++i) {
        const double t = i / FS;
        if (t >= nextSyl) { const bool p = U(rng) < 0.28; gT = p ? 0 : 0.4 + 0.6 * U(rng); F1T = 300 + 600 * U(rng); F2T = 900 + 1700 * U(rng); nextSyl = t + (p ? 0.15 + 0.35 * U(rng) : 0.12 + 0.18 * U(rng)); }
        if (t >= nextPitch) { f0T = 95 + 95 * U(rng); nextPitch = t + 0.08 + 0.2 * U(rng); }
        const double a = 1.0 / (0.03 * FS); g += (gT - g) * a; f0 += (f0T - f0) * a; F1 += (F1T - F1) * a; F2 += (F2T - F2) * a;
        double acc = 0;
        for (int h = 1; h <= 40; ++h) { const double f = f0 * h; ph[h - 1] += 2 * M_PI * f / FS; if (f < 300 || f > 3000) continue;
            acc += (0.25 + 1 / (1 + std::pow((f - F1) / 180, 2)) + 0.6 / (1 + std::pow((f - F2) / 250, 2))) / std::sqrt((double)h) * std::sin(ph[h - 1]); }
        x[i] = g * acc;
    }
    double pk = 0; for (double v : x) pk = std::max(pk, std::fabs(v)); for (auto& v : x) v /= pk;   // peak 1
    return x;
}
static std::vector<cf32> fm(const std::vector<double>& x, double dev, double fc, double amp) {
    std::vector<cf32> z(x.size()); double ph = 0;
    for (size_t i = 0; i < x.size(); ++i) { ph += 2 * M_PI * (fc + dev * x[i]) / FS; z[i] = cf32((float)(amp * std::cos(ph)), (float)(amp * std::sin(ph))); }
    return z;
}
static std::vector<float> demod(const std::vector<cf32>& iq, double bw) {
    std::vector<float> y; RxPipeline::Callbacks cb; cb.ctx = &y;
    cb.audio = [](void* c, const float* p, int n, int ch, int) { auto* v = (std::vector<float>*)c; for (int i = 0; i < n; ++i) v->push_back(ch == 2 ? 0.5f * (p[2*i] + p[2*i+1]) : p[i]); };
    RxPipeline P; P.setMeasureThread(false); P.start(FS, 1024, 20, 48000, cb); P.setTune(FC, RxPipeline::Mode::NFM, bw);
    for (size_t i = 0; i < iq.size(); i += 4800) P.feed(iq.data() + i, (int)std::min<size_t>(4800, iq.size() - i));
    P.stop(); return y;
}
int main() {
    const double secs = 24;
    struct C { const char* name; double dev, cnr, nbDb; };
    const C cs[] = { {"±2.5 kHz, CNR 20 dB", 2500, 20, -99}, {"±2.5 kHz, CNR 10 dB", 2500, 10, -99}, {"±2.5 kHz, CNR 6 dB", 2500, 6, -99},
                     {"±2.5 kHz, CNR 3 dB", 2500, 3, -99}, {"±2.5 kHz, CNR 15, neighbour 12.5 kHz up +20 dB", 2500, 15, 20},
                     {"±5 kHz, CNR 10 dB", 5000, 10, -99} };
    const double widths[] = { 12500, 11000, 9000, 7000 };
    std::printf("score vs truth (dB, coherence S/N over 300-3000 Hz, mean of 2 s windows after 4 s); NFM width →\n%-48s", "");
    for (double w : widths) std::printf("%9.1fk", w / 1000); std::printf("\n");
    unsigned seed = 1;
    for (const auto& c : cs) {
        const auto x = voiceAudio(secs, seed++);
        auto iq = fm(x, c.dev, FC, 1.0);
        std::mt19937 rng(seed * 7); std::normal_distribution<double> G(0, 1);
        const double sn = std::sqrt(std::pow(10, -c.cnr / 10) * FS / 12500 / 2);
        if (c.nbDb > -90) { const auto x2 = voiceAudio(secs, 100 + seed); const auto z2 = fm(x2, 2500, 12500, std::pow(10, c.nbDb / 20));
                            for (size_t i = 0; i < iq.size(); ++i) iq[i] += z2[i]; }
        for (auto& v : iq) v += cf32((float)(G(rng) * sn), (float)(G(rng) * sn));
        std::vector<float> xf(x.begin(), x.end());
        std::printf("%-48s", c.name);
        for (double w : widths) {
            const auto y = demod(iq, w);
            const int lag = abscore::lagOf(xf, y, (int)(0.1 * FS), (size_t)(4 * FS), (size_t)(4 * FS));
            double tot = 0; int n = 0;
            for (size_t s = (size_t)(4 * FS); s + 2 * FS + lag < y.size(); s += (size_t)(2 * FS)) {
                const auto sp = abscore::welch(xf.data() + s, y.data() + s + lag, (int)(2 * FS));
                double g2 = 0; int k = 0;
                for (size_t b = 0; b < sp.pxx.size(); ++b) { const double f = b * FS / 1024; if (f < 300 || f > 3000) continue;
                    const double coh = std::norm(sp.pxy[b]) / (sp.pxx[b] * sp.pyy[b] + 1e-30); g2 += coh; ++k; }
                g2 /= k; tot += 10 * std::log10(g2 / std::max(1e-9, 1 - g2)); ++n;
            }
            std::printf("%10.2f", tot / n);
        }
        std::printf("\n");
    }
}
