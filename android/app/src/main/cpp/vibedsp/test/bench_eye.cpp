// VibeSDR — the composite eye and the deviation monitor, driven through the REAL pipeline.
//
// Two jobs, because both were guessed at before and both guesses were wrong:
//   1. CPU: the eye's per-sample work (high-pass, six resonators, deviation cascade, guard band,
//      splat) as % of a core, with the Advanced RDS panel open and closed. A 2026-09-13 benchmark
//      timed only the fold and licensed a 25x regression; this times the whole feed().
//   2. CALIBRATION: a processed composite of KNOWN peak deviation, FM-modulated, with white IQ
//      noise added at a range of carrier-to-noise ratios. The deviation readout must stay at the
//      truth as the noise rises — that is the failure tgcfabian's MPXtool dataset showed (+24 kHz on a
//      compliant station at −66 dBFS) and the one the guard-band correction exists to remove.
//
//   cmake --build build && ./build/vibedsp_bench_eye [cnr_db ...]
#include "vibedsp.h"
#include <cstdio>
#include <cmath>
#include <chrono>
#include <vector>
#include <string>
#include <cstdlib>
#include <random>
#include <algorithm>

using namespace vibedsp;

static void onAud(void*, const float*, int, int, int) {}
static void onSpec(void*, const float*, int) {}
struct Seen {
    float dev = 0, hold = 0, noise = 0, eyeDev = 0, pilot = 0; int eyeW = 0, eyeH = 0, n = 0;
    std::vector<unsigned char> eye[3];
    std::vector<float> devs;
};
static void onExt(void* ctx, const RxPipeline::Callbacks::RdsExt& x) {
    Seen* s = (Seen*)ctx;
    s->dev = x.mpxDevKHz; s->hold = x.mpxDevHoldKHz; s->noise = x.mpxDevNoiseKHz; s->eyeDev = x.eyeDevKHz; s->pilot = x.pilotDevKHz;
    s->eyeW = x.eyeW; s->eyeH = x.eyeH; s->n++;
    s->devs.push_back(x.mpxDevKHz);
    if (x.eyeBand[0] && x.eyeW > 0)
        for (int b = 0; b < 3; ++b) s->eye[b].assign(x.eyeBand[b], x.eyeBand[b] + (size_t)x.eyeW * x.eyeH);
}

int main(int argc, char** argv) {
    const double fs = 2400000.0, fc = 300000.0;
    const double secs = 14.0;   // 1.5 s retune blank + ~6 time constants, then a settled read
    const int Ni = (int)(fs * secs);
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.0f, 1.0f);

    // ── A processed composite with a KNOWN peak ─────────────────────────────────────────────
    // Audio: a handful of tones in a musical-ish spread, hard-clipped like a broadcast processor,
    // then band-limited; pilot at 9 % and an RDS-like 57 kHz tone at 2.5 %. Peak set to 75 kHz.
    std::vector<float> mpx(Ni);
    {
        // ★ CLIP, THEN LOW-PASS AT 15 kHz — the order a broadcast processor uses. A bare clipper
        //   puts harmonics to 80 kHz and above: the first cut of this bench skipped the low-pass
        //   and read 12 kHz LOW on a clean signal (the receiver's filters rounded the spiky peaks
        //   the truth was measured on) while the guard band saw the clipper's harmonics as noise.
        struct Lp { float b0, b1, b2, a1, a2, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
            Lp(double fs_, double f0, double q) { const double w = 2 * M_PI * f0 / fs_, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
                b0 = (float)((1 - c) / 2 / a0); b1 = (float)((1 - c) / a0); b2 = b0; a1 = (float)(-2 * c / a0); a2 = (float)((1 - al) / a0); }
            float step(float x) { const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2; x2 = x1; x1 = x; y2 = y1; y1 = y; return y; } };
        Lp lpA(fs, 15000.0, 0.5412), lpB(fs, 15000.0, 1.3066), lmA(fs, 15000.0, 0.5412), lmB(fs, 15000.0, 1.3066);
        const double fL[5] = { 220, 440, 1100, 2600, 6200 }, fR[5] = { 330, 550, 1500, 3300, 8800 };
        for (int i = 0; i < Ni; ++i) {
            const double t = i / fs;
            double L = 0, R = 0;
            for (int k = 0; k < 5; ++k) { L += std::sin(2 * M_PI * fL[k] * t + k) / (k + 1); R += std::sin(2 * M_PI * fR[k] * t + 2 * k) / (k + 1); }
            // slow programme dynamics so the meter sees quiet and loud passages
            const double env = getenv("FLAT") ? 1.0 : 0.55 + 0.45 * std::sin(2 * M_PI * 0.37 * t);
            static const double kAud = getenv("AUDIO") ? atof(getenv("AUDIO")) : 1.0;
            L *= env * kAud; R *= env * 0.8 * kAud;
            double lpr = 0.5 * (L + R), lmr = 0.5 * (L - R);
            // ★ Stereo width: real programme has L−R some 6–12 dB below L+R; the first cut used
            //   nearly independent channels, which fills the 23–53 kHz band and widens the FM
            //   spectrum well past what a 200 kHz channel passes. LMR scales it (default 0.35).
            static const double kLmr = getenv("LMR") ? atof(getenv("LMR")) : 0.35;
            lmr *= kLmr;
            // hard clipper on the audio (the processor's job), drive 1.6, then the 15 kHz low-pass
            lpr = std::max(-1.0, std::min(1.0, 1.6 * lpr));
            lmr = std::max(-1.0, std::min(1.0, 1.6 * lmr));
            lpr = lpB.step(lpA.step((float)lpr));
            lmr = lmB.step(lmA.step((float)lmr));
            mpx[i] = (float)(0.90 * (lpr + lmr * std::cos(2 * M_PI * 38000.0 * t))
                             + 0.09 * std::sin(2 * M_PI * 19000.0 * t)
                             + 0.025 * std::sin(2 * M_PI * 57000.0 * t));
        }
        float pk = 0; for (float v : mpx) pk = std::max(pk, std::fabs(v));
        if (!getenv("NONORM")) for (float& v : mpx) v /= pk;   // peak exactly 1.0 = 75 kHz
        // ★ COMPOSITE CLIPPER: what a modern processor does last — flat tops at exactly the limit.
        if (getenv("MPXCLIP")) { const float c = (float)atof(getenv("MPXCLIP")); for (float& v : mpx) v = std::max(-c, std::min(c, v / c)); }
    }
    // Truth for the STATISTIC the meter reports: the mean of 50 ms maxima over the run.
    double truthBar = 0;
    {
        const int win = (int)(fs * 0.05); int nw = 0; double truthMax = 0;
        std::vector<float> tmp(win);
        for (int o = 0; o + win <= Ni; o += win) {
            float m = 0; for (int i = o; i < o + win; ++i) { tmp[i - o] = std::fabs(mpx[i]); m = std::max(m, tmp[i - o]); }
            static const double skipPm = getenv("VIBE_DEV_SKIP") ? atof(getenv("VIBE_DEV_SKIP")) : 0.3;
            const int sk = (int)(win * skipPm / 1000.0);
            std::nth_element(tmp.begin(), tmp.begin() + (win - sk - 1), tmp.end());
            truthBar += tmp[win - sk - 1]; truthMax += m; ++nw; }
        truthBar = 75.0 * truthBar / nw; truthMax = 75.0 * truthMax / nw;
        std::printf("truth: mean of 50 ms maxima %.1f kHz, mean of 50 ms 99.97th percentiles %.1f kHz (the meter's statistic)\n", truthMax, truthBar);
        float wmin = 1e9, wmax = 0;
        for (int o = 0; o + win <= Ni; o += win) { float m = 0; for (int i = o; i < o + win; ++i) m = std::max(m, std::fabs(mpx[i])); wmin = std::min(wmin, m); wmax = std::max(wmax, m); }
        std::printf("truth windows: min %.1f max %.1f kHz\n", 75 * wmin, 75 * wmax);
    }
    std::vector<cf32> clean(Ni);
    { double ph = 0; for (int i = 0; i < Ni; ++i) { ph += 2 * M_PI * (fc + 75000.0 * mpx[i]) / fs; if (ph > 2 * M_PI) ph -= 2 * M_PI; clean[i] = cf32((float)std::cos(ph), (float)std::sin(ph)); } }
    std::vector<float> nz(2 * Ni); for (float& v : nz) v = nd(rng);

    std::vector<double> cnrs;
    for (int a = 1; a < argc; ++a) cnrs.push_back(std::atof(argv[a]));
    if (cnrs.empty()) cnrs = { 70, 40, 30, 25, 20, 16, 13, 10 };

    std::printf("processed composite, true peak 75.0 kHz; truth for the bar (mean of 50 ms maxima) = %.1f kHz\n", truthBar);
    std::printf("CNR is carrier-to-noise in the full %.1f MHz IQ band\n", fs / 1e6);
    std::printf("  CNR dB   bar kHz   err   hold   noise-est kHz   eyeDev   pilot  frames\n");
    std::vector<cf32> iq(Ni);
    for (double cnr : cnrs) {
        const float a = (float)std::pow(10.0, -cnr / 20.0) / std::sqrt(2.0f);   // per-component sigma for unit carrier
        for (int i = 0; i < Ni; ++i) iq[i] = cf32(clean[i].real() + a * nz[2 * i], clean[i].imag() + a * nz[2 * i + 1]);
        RxPipeline pipe; RxPipeline::Callbacks cb; Seen seen;
        cb.audio = onAud; cb.spectrum = onSpec; cb.rdsExt = onExt; cb.ctx = &seen;
        pipe.start(fs, 1024, 20.0, 48000, cb);
        pipe.setRdsEnabled(true);
        pipe.setTune(fc, RxPipeline::Mode::WFM, getenv("BW") ? atof(getenv("BW")) : 200000.0);
        for (int o = 0; o < Ni; o += 65536) pipe.feed(iq.data() + o, std::min(65536, Ni - o));
        // settle-skip: the first 1.5 s are blanked by the meter and the 1.5 s smoother then ramps; read the last quarter
        double bar = 0; int nb = 0;
        for (size_t k = seen.devs.size() * 3 / 4; k < seen.devs.size(); ++k) { bar += seen.devs[k]; ++nb; }
        bar = nb ? bar / nb : 0;
        std::printf("  %5.0f    %6.1f   %+5.1f  %5.1f   %6.2f          %5.1f   %5.2f  %d\n", cnr, bar, bar - truthBar, seen.hold, seen.noise, seen.eyeDev, seen.pilot, seen.n);
        if (getenv("SERIES")) { std::printf("    bar series:"); for (size_t k = 0; k < seen.devs.size(); k += 8) std::printf(" %.0f", seen.devs[k]); std::printf("\n"); }
        if (cnr == cnrs.front()) {
            // eye grid stats at the cleanest CNR: fill and how the wire would look
            for (int b = 0; b < 3; ++b) {
                int lit = 0, zeroRuns = 0; bool inRun = false;
                for (unsigned char v : seen.eye[b]) { if (v) { lit++; inRun = false; } else if (!inRun) { zeroRuns++; inRun = true; } }
                std::printf("    eye band %d: %dx%d, %d lit cells, ~%d bytes RLE\n", b, seen.eyeW, seen.eyeH, lit, lit + 2 * zeroRuns);
            }
        }
    }

    // ── CPU: panel open vs closed, clean signal ─────────────────────────────────────────────
    for (int open = 0; open < 2; ++open) {
        RxPipeline pipe; RxPipeline::Callbacks cb; Seen seen;
        cb.audio = onAud; cb.spectrum = onSpec; cb.ctx = &seen;
        if (open) cb.rdsExt = onExt;
        pipe.start(fs, 1024, 20.0, 48000, cb);
        pipe.setRdsEnabled(true);
        pipe.setTune(fc, RxPipeline::Mode::WFM, getenv("BW") ? atof(getenv("BW")) : 200000.0);
        for (int o = 0; o < Ni; o += 65536) pipe.feed(clean.data() + o, std::min(65536, Ni - o));
        double best = 1e9;
        for (int r = 0; r < 3; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            for (int o = 0; o < Ni; o += 65536) pipe.feed(clean.data() + o, std::min(65536, Ni - o));
            best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        }
        std::printf("CPU, WFM stereo + RDS, advanced panel %s: %.1f ms per %.1f s = %.2f%% of a core\n",
                    open ? "OPEN  " : "closed", best * 1e3, secs, 100.0 * best / secs);
    }
    return 0;
}
