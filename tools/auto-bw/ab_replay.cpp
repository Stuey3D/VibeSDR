// ab_replay — REAL recordings through the real RxPipeline, the fixed passband against AUTO bandwidth (V12, SSB/CW).
//
//   Two transmitters, one programme (VOLMET on two frequencies, recorded at the same moment):
//     ab_replay pair <iqA.wav> <offA Hz> <usb|lsb> <bw Hz> <iqB.wav> <offB Hz> [seconds] [outdir]
//   Station A is judged; station B, demodulated with the fixed passband, is the REFERENCE — an independent path with
//   its own fading and noise. Scored with abscore (vibeserver/autobw_score.h): wanted ÷ (unwanted + wanted thrown
//   away), paired per 2 s window against the fixed passband on A. The reference's own noise makes C(f) an
//   UNDER-estimate of A's programme by the same factor in both runs, so the comparison is fair; the absolute
//   figure is not the station's S/N.
//
//   One CW signal (no second path exists for CW):
//     ab_replay cw <iq.wav> <dial offset Hz> [seconds] [outdir]
//   Scored on the output spectrum: the power within ±40 Hz of the tone AUTO centred on, against the power in the
//   rest of 0–1400 Hz, per 2 s window, fixed vs auto — a narrow keyed tone is the one signal whose "wanted part" is
//   known from the spectrum alone.
//
//   Optional outdir: 48 kHz WAVs of each run, RMS-matched (−20 dBFS) for listening. ★ SILENT: nothing is played.
#include "vibedsp.h"
#include "autobw_score.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace vibedsp;
using M = RxPipeline::Mode;

static std::vector<cf32> readIq(const char* path, double secs, int& rate) {
    FILE* f = std::fopen(path, "rb"); if (!f) { std::perror(path); std::exit(1); }
    unsigned char h[44]; if (std::fread(h, 1, 44, f) != 44) std::exit(1);
    rate = h[24] | h[25] << 8 | h[26] << 16 | h[27] << 24;
    const size_t n = (size_t)(secs * rate);
    std::vector<short> b(2 * n);
    const size_t got = std::fread(b.data(), 4, n, f);
    std::fclose(f);
    std::vector<cf32> z(got);
    for (size_t i = 0; i < got; ++i) z[i] = cf32(b[2 * i] / 32768.f, b[2 * i + 1] / 32768.f);
    return z;
}
struct Run { std::vector<float> y; std::vector<AutoPassband::Status> st; };
static Run run(const std::vector<cf32>& z, int rate, double off, M mode, double bw, bool autoOn) {
    Run r; RxPipeline::Callbacks cb; cb.ctx = &r;
    cb.audio = [](void* c, const float* pcm, int frames, int ch, int) {
        auto* rr = (Run*)c; for (int i = 0; i < frames; ++i) rr->y.push_back(ch == 2 ? 0.5f * (pcm[2 * i] + pcm[2 * i + 1]) : pcm[i]); };
    RxPipeline p; p.setMeasureThread(false);
    p.start(rate, 1024, 20.0, 48000, cb);
    p.setAmAutoBandwidth(autoOn);
    p.setTune(off, mode, bw);
    const size_t blk = (size_t)rate / 10;
    for (size_t i = 0; i < z.size(); i += blk) {
        p.feed(z.data() + i, (int)std::min(blk, z.size() - i));
        if ((i / blk) % 20 == 19) r.st.push_back(p.autoPassbandStatus());   // every 2 s
    }
    p.stop();
    return r;
}
static void wav(const std::string& path, const std::vector<float>& y) {
    double p = 0; for (float v : y) p += (double)v * v; p = std::sqrt(p / std::max<size_t>(1, y.size()));
    const float g = p > 0 ? (float)(0.1 / p) : 1.0f;
    FILE* f = std::fopen(path.c_str(), "wb"); if (!f) return;
    const unsigned n = (unsigned)y.size(), sr = 48000, br = sr * 2, ds = n * 2, rs = 36 + ds;
    unsigned char h[44] = {'R','I','F','F'}; std::memcpy(h + 4, &rs, 4); std::memcpy(h + 8, "WAVEfmt ", 8);
    const unsigned fl = 16; const unsigned short fmt = 1, ch = 1, ba = 2, bps = 16;
    std::memcpy(h + 16, &fl, 4); std::memcpy(h + 20, &fmt, 2); std::memcpy(h + 22, &ch, 2); std::memcpy(h + 24, &sr, 4);
    std::memcpy(h + 28, &br, 4); std::memcpy(h + 32, &ba, 2); std::memcpy(h + 34, &bps, 2); std::memcpy(h + 36, "data", 4); std::memcpy(h + 40, &ds, 4);
    std::fwrite(h, 1, 44, f);
    for (float v : y) { const float s = std::max(-1.f, std::min(1.f, v * g)); const short q = (short)std::lrint(s * 32767); std::fwrite(&q, 2, 1, f); }
    std::fclose(f);
}
static void passbands(const Run& a) {
    std::printf("   AUTO passband every 2 s:");
    for (const auto& s : a.st) std::printf(" %s%.0f-%.0f", s.cut ? "" : "=", s.loHz, s.hiHz);
    std::printf("\n");
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "see the header\n"); return 2; }
    const std::string kind = argv[1];
    const double FS = 48000.0;
    if (kind == "pair" && argc >= 8) {
        const double secs = argc > 8 ? std::atof(argv[8]) : 240.0;
        const M mode = std::string(argv[4]) == "lsb" ? M::SSB_LSB : M::SSB_USB;
        const double bw = std::atof(argv[5]);
        int ra = 0, rb = 0;
        const auto za = readIq(argv[2], secs, ra), zb = readIq(argv[6], secs, rb);
        const Run fix = run(za, ra, std::atof(argv[3]), mode, bw, false), aut = run(za, ra, std::atof(argv[3]), mode, bw, true);
        const Run ref = run(zb, rb, std::atof(argv[7]), mode, 2700, false);
        // the reference's lag against A, either direction: try B later than A, then A later than B
        const size_t from = (size_t)(20 * FS), len = (size_t)(8 * FS);
        const int lagAB = abscore::lagOf(ref.y, fix.y, (int)(0.4 * FS), from, len);   // fix[i+lag] ~ ref[i]
        const int lagBA = abscore::lagOf(fix.y, ref.y, (int)(0.4 * FS), from, len);
        // pick the direction with the higher correlation
        auto cc = [&](const std::vector<float>& x, const std::vector<float>& y, int L) {
            double a = 0, xx = 0, yy = 0; for (size_t i = from; i < from + len; ++i) { a += (double)x[i] * y[i + L]; xx += (double)x[i] * x[i]; yy += (double)y[i + L] * y[i + L]; }
            return std::fabs(a) / std::sqrt(xx * yy + 1e-30); };
        const double cAB = cc(ref.y, fix.y, lagAB), cBA = cc(fix.y, ref.y, lagBA);
        const int laA = abscore::lagOf(fix.y, aut.y, (int)(0.05 * FS), from, len);   // auto's extra delay over fixed
        std::printf("   reference correlation %.3f (lag %+d samples)\n", std::max(cAB, cBA), cAB >= cBA ? lagAB : -lagBA);
        const int W = (int)(4 * FS);
        std::vector<double> d, kept, sF, sA;
        for (size_t w = (size_t)(10 * FS); w + W + FS < std::min({fix.y.size(), aut.y.size(), ref.y.size()}); w += W) {
            const float* x; const float* yf; const float* ya;
            if (cAB >= cBA) { x = ref.y.data() + w; yf = fix.y.data() + w + lagAB; ya = aut.y.data() + w + lagAB + laA; }
            else            { x = ref.y.data() + w + lagBA; yf = fix.y.data() + w; ya = aut.y.data() + w + laA; }
            std::vector<double> Cf, Nf, Ca, Na;
            abscore::cnPower(x, yf, W, FS, 500, 1500, 100, Cf, Nf); abscore::cnPower(x, ya, W, FS, 500, 1500, 100, Ca, Na);
            // score() reads C/N per index with a bin width of fs/(2(nb−1)); pass the band rate that makes index = band
            const double fsB = 2.0 * 100.0 * (Cf.size() - 1);
            double k = 1;
            const double s0 = abscore::score(Cf, Nf, Cf, fsB, 0, bw + 300), s1 = abscore::score(Ca, Na, Cf, fsB, 0, bw + 300, &k);
            d.push_back(s1 - s0); kept.push_back(k); sF.push_back(s0); sA.push_back(s1);
        }
        const auto p = abscore::paired(d, kept);
        double mf = 0; for (double v : sF) mf += v; mf /= std::max<size_t>(1, sF.size());
        std::printf("   fixed score %.2f dB · AUTO %+.2f ± %.2f dB (won %d/%d) · voice kept ≥ %.1f %% (median window kept shown below)\n",
                    mf, p.mean, p.se, p.won, p.n, 100 * p.keptMin);
        std::vector<double> ks = kept; std::sort(ks.begin(), ks.end());
        if (!ks.empty()) std::printf("   kept: median %.1f %%, 10th percentile %.1f %%\n", 100 * ks[ks.size() / 2], 100 * ks[ks.size() / 10]);
        passbands(aut);
        if (argc > 9) { wav(std::string(argv[9]) + "/fixed.wav", fix.y); wav(std::string(argv[9]) + "/auto.wav", aut.y); }
        return 0;
    }
    if (kind == "cw" && argc >= 4) {
        const double secs = argc > 4 ? std::atof(argv[4]) : 120.0;
        int r = 0; const auto z = readIq(argv[2], secs, r);
        const double off = std::atof(argv[3]);
        const Run fix = run(z, r, off - 600, M::CW, 1200, false), aut = run(z, r, off - 600, M::CW, 1200, true);
        const int la = abscore::lagOf(fix.y, aut.y, (int)(0.05 * FS), (size_t)(10 * FS), (size_t)(4 * FS));
        const int W = (int)(2 * FS);
        std::vector<double> d, kept;
        size_t wi = 0;
        for (size_t w = (size_t)(6 * FS); w + W + FS < std::min(fix.y.size(), aut.y.size()); w += W, ++wi) {
            const size_t si = std::min(aut.st.size() - 1, (size_t)((w / FS) / 2));
            const double tone = aut.st.empty() ? 0 : aut.st[si].toneHz;
            if (tone <= 0) continue;
            auto pf = abscore::welch(fix.y.data() + w, fix.y.data() + w, W), pa = abscore::welch(aut.y.data() + w + la, aut.y.data() + w + la, W);
            auto snr = [&](const abscore::Spec& s, double& t) { double rest = 0; t = 0; const double bh = FS / 1024;
                for (size_t b = 0; b < s.pxx.size(); ++b) { const double f = b * bh; if (f > 1400) break;
                    if (std::fabs(f - tone) <= 40) t += s.pxx[b]; else rest += s.pxx[b]; } return 10 * std::log10(t / std::max(rest, 1e-30)); };
            double tf, ta; const double s0 = snr(pf, tf), s1 = snr(pa, ta);
            d.push_back(s1 - s0); kept.push_back(1.0);
        }
        const auto p = abscore::paired(d, kept);
        std::printf("   CW tone ÷ rest: AUTO %+.2f ± %.2f dB over the fixed CW passband (won %d/%d windows with a tone)\n", p.mean, p.se, p.won, p.n);
        passbands(aut);
        if (argc > 5) { wav(std::string(argv[5]) + "/cw_fixed.wav", fix.y); wav(std::string(argv[5]) + "/cw_auto.wav", aut.y); }
        return 0;
    }
    std::fprintf(stderr, "see the header\n"); return 2;
}
