// ★★★ V12 SYNCHRONOUS AM — vibedsp SyncAm (amsync.cpp): SAM, SAM-U/L, ECSS-U/L, AM stereo (C-QUAM compatible)
//     and AM auto bandwidth, on synthetic signals with KNOWN programme, so every figure is measured against truth.
//
//   1. ACQUIRE: a carrier at 0 … ±900 Hz off is found and locked inside ~0.7 s; the offset readout is right.
//   2. SELECTIVE FADING (the case SAM exists for): a two-path channel whose carrier notch sweeps through every 4 s.
//      Programme = 18 tones on exact FFT bins, so SINAD (tone power / everything else in 150-4500 Hz) is a pure
//      distortion+noise figure, blind to the filters' phase. AM (the shipping envelope detector) vs SAM, in the
//      worst second and in the median, raw detector and through the production AGC. SAM must WIN, never lose.
//   3. NO WORSE ON A CLEAN SIGNAL: same programme, no fading — SAM within 1 dB of AM or better.
//   4. SIDEBAND REJECTION: SAM-U / ECSS-U on a USB tone and an LSB tone — the wrong one ≥ 35 dB down.
//   5. AM STEREO (C-QUAM compatible): our own modulator (envelope 1+L+R, phase atan((L−R+pilot)/(1+L+R))):
//      separation ≥ 25 dB both ways, pilot found in ≤ 1.5 s, NO pilot = mono (never stereo), the envelope
//      detector hears L+R undistorted (mono compatibility), and the pilot itself is removed from the audio.
//   6. AUTO BANDWIDTH: a 5 kHz-spaced neighbour on the upper side (SW); SINAD with auto off/on for AM and SAM —
//      auto must help, and with NO neighbour it must not hurt (and must leave the width alone on a clean signal).
//   7. A RETUNE CLEARS every verdict; lock loss on a carrier that vanishes eases to the envelope with no burst.
//   8. COST per listener (% of one core in real time) — the build's SIMD path; build with -DVIBE_FORCE_SCALAR for
//      the scalar figure (the runner prints both).
//
// ★ SILENT: every signal is generated and measured in memory, never played.
#include "vibedsp/vibedsp.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <vector>

using namespace vibedsp;
using Det = SyncAm::Det;
static int pass = 0, fail = 0;
static void ok(bool c, const char* what) { (c ? pass : fail)++; std::printf("   %s %s\n", c ? "ok  " : "FAIL", what); }
static double cpuNow() { timespec t{}; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static const double FS = 36000.0;           // a typical AM channel rate (2.4 MS/s / 66)
static const int    WIN = 32768;            // analysis window (0.91 s)

// ── programme: tones on exact bins of the analysis window ────────────────────
struct Prog {
    std::vector<int> bins; std::vector<double> amp, ph;
    explicit Prog(double maxHz = 4200.0, int nTones = 18, unsigned seed = 7, double total = 0.6) {
        std::mt19937 r(seed); std::uniform_real_distribution<double> u(0, 2 * M_PI);
        const double binHz = FS / WIN;
        for (int k = 0; k < nTones; ++k) {
            const double f = 220.0 * std::pow(maxHz / 220.0, k / (double)(nTones - 1));
            int b = (int)std::lround(f / binHz);
            if (!bins.empty() && b <= bins.back() + 12) b = bins.back() + 13;   // well apart (IMD lands between)
            bins.push_back(b); amp.push_back(total / nTones * 1.6); ph.push_back(u(r));
        }
    }
    double at(long n) const {
        double s = 0;
        for (size_t k = 0; k < bins.size(); ++k) s += amp[k] * std::cos(2 * M_PI * bins[k] * (double)n / WIN + ph[k]);
        return s;
    }
};

// SINAD over one window: tone bins (±3) vs all other bins in 150-4500 Hz. Hann window.
static double sinadDb(const float* x, const Prog& p, double hiHz = 4500.0) {
    static ComplexFFT fft(WIN);
    static std::vector<cf32> in(WIN), out(WIN);
    double mean = 0; for (int i = 0; i < WIN; ++i) mean += x[i]; mean /= WIN;
    for (int i = 0; i < WIN; ++i) in[i] = cf32((float)((x[i] - mean) * (0.5 - 0.5 * std::cos(2 * M_PI * i / WIN))), 0.0f);
    fft.forward(in.data(), out.data());
    const double binHz = FS / WIN;
    std::vector<char> tone(WIN / 2, 0);
    for (int b : p.bins) for (int d = -3; d <= 3; ++d) if (b + d > 0 && b + d < WIN / 2) tone[b + d] = 1;
    double sp = 0, np = 0;
    for (int b = (int)(150 / binHz); b < (int)(hiHz / binHz) && b < WIN / 2; ++b) (tone[b] ? sp : np) += std::norm(out[b]);
    return 10 * std::log10((sp + 1e-30) / (np + 1e-30));
}
/** Top end kept: tone power above 2.5 kHz over tone power below 1.5 kHz, dB (compare against a clean reference). */
static double topEndDb(const float* x, const Prog& p) {
    static ComplexFFT fft(WIN);
    static std::vector<cf32> in(WIN), out(WIN);
    for (int i = 0; i < WIN; ++i) in[i] = cf32((float)(x[i] * (0.5 - 0.5 * std::cos(2 * M_PI * i / WIN))), 0.0f);
    fft.forward(in.data(), out.data());
    const double binHz = FS / WIN; double hi = 0, lo = 0;
    for (int b : p.bins) { double e = 0; for (int d = -2; d <= 2; ++d) e += std::norm(out[b + d]);
                           if (b * binHz > 2500) hi += e; else if (b * binHz < 1500) lo += e; }
    return 10 * std::log10((hi + 1e-30) / (lo + 1e-30));
}
static double toneDb(const float* x, int bin) {
    static ComplexFFT fft(WIN);
    static std::vector<cf32> in(WIN), out(WIN);
    for (int i = 0; i < WIN; ++i) in[i] = cf32((float)(x[i] * (0.5 - 0.5 * std::cos(2 * M_PI * i / WIN))), 0.0f);
    fft.forward(in.data(), out.data());
    double p = 0; for (int d = -2; d <= 2; ++d) p += std::norm(out[bin + d]);
    return 10 * std::log10(p + 1e-30);
}

// ── a demodulator under test: SyncAm in some mode, or the shipping AmDemod ──
struct Rx {
    bool env; SyncAm s; AmDemod a; Agc agc; bool useAgc = false;
    Rx(Det d, double edge, bool autoBw, bool agcOn = false) : env(false) {
        s.configure(d, FS, edge); s.setAutoBw(autoBw); useAgc = agcOn;
        agc.configure(FS); agc.reset(); if (d != Det::ENV || autoBw) agc.setMaxGain(d == Det::ENV ? 256.0f : 4.0f);
    }
    explicit Rx(bool agcOn = false) : env(true) { useAgc = agcOn; agc.configure(FS); agc.reset(); }
    void run(const cf32* z, float* out, int n, float* outR = nullptr) {
        if (env) a.process(z, out, n); else s.process(z, out, outR, n);
        if (useAgc) agc.process(out, n);
    }
};

// ── signals ──────────────────────────────────────────────────────────────────
static std::vector<cf32> amSignal(const Prog& p, double secs, double offHz, double cnrDb, unsigned seed,
                                  double fadeRho = 0.0, double fadeTauMs = 0.0, double fadeHz = 0.0,
                                  double neighOffHz = 0.0, double neighDb = 0.0) {
    const long n = (long)(secs * FS);
    std::vector<cf32> z((size_t)n);
    std::mt19937 r(seed); std::normal_distribution<double> g(0, 1);
    // neighbour programme: band-limited noise (speech-like), 4.5 kHz, independent
    std::vector<double> nb((size_t)n, 0.0);
    if (neighOffHz != 0.0) {
        double s1 = 0, s2 = 0, s3 = 0, s4 = 0; const double a = 1 - std::exp(-2 * M_PI * 3000.0 / FS);
        for (long i = 0; i < n; ++i) { s1 += a * (g(r) - s1); s2 += a * (s1 - s2); s3 += a * (s2 - s3); s4 += a * (s3 - s4); nb[i] = s4 * 2.2; }
    }
    const double nAmp = std::pow(10.0, -cnrDb / 20.0) * std::sqrt(0.5) * std::sqrt(FS / 10000.0);   // CNR in 10 kHz
    const int dly = (int)std::lround(fadeTauMs * 1e-3 * FS);
    std::vector<double> m((size_t)n);
    for (long i = 0; i < n; ++i) m[i] = p.at(i);
    const double nA = std::pow(10.0, neighDb / 20.0);
    for (long i = 0; i < n; ++i) {
        const double t = i / FS;
        cf32 a((float)(1.0 + m[i]), 0.0f);
        if (fadeRho > 0.0) {
            const long j = i - dly;
            const double mj = j >= 0 ? m[j] : 0.0;
            const double th = 2 * M_PI * fadeHz * t + M_PI * 0.37;
            a += cf32((float)(fadeRho * (1.0 + mj) * std::cos(th)), (float)(fadeRho * (1.0 + mj) * std::sin(th)));
        }
        const double ph = 2 * M_PI * offHz * t;
        cf32 v = a * cf32((float)std::cos(ph), (float)std::sin(ph));
        if (neighOffHz != 0.0) {
            const double pn = 2 * M_PI * (offHz + neighOffHz) * t;
            v += cf32((float)(nA * (1.0 + 0.8 * nb[i]) * std::cos(pn)), (float)(nA * (1.0 + 0.8 * nb[i]) * std::sin(pn)));
        }
        v += cf32((float)(nAmp * g(r)), (float)(nAmp * g(r)));
        z[i] = v * 0.01f;          // arbitrary scale: nothing may depend on it
    }
    return z;
}

static std::vector<float> demod(Rx& rx, const std::vector<cf32>& z, std::vector<float>* R = nullptr) {
    std::vector<float> out(z.size()), r(z.size());
    const int blk = 1200;           // a channel block
    for (size_t i = 0; i < z.size(); i += blk) {
        const int k = (int)std::min<size_t>(blk, z.size() - i);
        rx.run(z.data() + i, out.data() + i, k, R ? r.data() + i : nullptr);
    }
    if (R) *R = r;
    return out;
}
struct Stat { double worst, median, mean; };
static Stat sinadStats(const std::vector<float>& y, const Prog& p, double skipSecs = 3.0, double hiHz = 4500.0) {
    std::vector<double> v;
    for (size_t i = (size_t)(skipSecs * FS); i + WIN <= y.size(); i += WIN / 4) v.push_back(sinadDb(y.data() + i, p, hiHz));
    std::vector<double> s = v; std::sort(s.begin(), s.end());
    double m = 0; for (double d : v) m += d;
    return { s.empty() ? 0 : s.front(), s.empty() ? 0 : s[s.size() / 2], v.empty() ? 0 : m / v.size() };
}

int main(int argc, char** argv) {
    const bool quick = argc > 1 && std::string(argv[1]) == "--quick";
    std::printf("── 1. acquisition ──\n");
    {
        const Prog p;
        for (double off : { 0.0, 7.0, 42.0, -310.0, 880.0 }) {
            auto z = amSignal(p, 3.0, off, 35.0, 11);
            Rx rx(Det::SAM, 5000.0, false);
            std::vector<float> out(z.size());
            double lockAt = -1;
            for (size_t i = 0; i < z.size(); i += 360) {
                rx.run(z.data() + i, out.data() + i, 360);
                if (lockAt < 0 && rx.s.status().locked) lockAt = (i + 360) / FS;
            }
            const auto st = rx.s.status();
            char b[160]; std::snprintf(b, sizeof b, "offset %+6.0f Hz: locked at %.2f s, reads %+8.2f Hz", off, lockAt, st.offsetHz);
            ok(lockAt > 0 && lockAt < 0.9 && std::fabs(st.offsetHz - off) < 0.5, b);
        }
    }

    std::printf("── 2. selective fading (two-path, carrier notch sweeps through every 4 s) ──\n");
    const Prog p;
    struct Case { const char* name; double rho, tau, cnr; };
    const Case cases[] = { { "rho 0.90 tau 0.3 ms, CNR 40 dB", 0.90, 0.3, 40.0 },
                           { "rho 0.97 tau 0.3 ms, CNR 40 dB", 0.97, 0.3, 40.0 },
                           { "rho 0.90 tau 1.0 ms, CNR 30 dB", 0.90, 1.0, 30.0 } };
    for (const Case& c : cases) {
        if (quick && &c != &cases[0]) break;
        auto z = amSignal(p, 24.0, 12.0, c.cnr, 21, c.rho, c.tau, 0.25);
        Rx am(false), sam(Det::SAM, 5000.0, false), amA(true), samA(Det::SAM, 5000.0, false, true);
        const auto ya = demod(am, z), ys = demod(sam, z), yaa = demod(amA, z), ysa = demod(samA, z);
        const Stat a = sinadStats(ya, p), s = sinadStats(ys, p), aa = sinadStats(yaa, p), sa = sinadStats(ysa, p);
        std::printf("   %s\n      raw   AM worst %5.1f median %5.1f | SAM worst %5.1f median %5.1f dB\n"
                    "      +AGC  AM worst %5.1f median %5.1f | SAM worst %5.1f median %5.1f dB\n",
                    c.name, a.worst, a.median, s.worst, s.median, aa.worst, aa.median, sa.worst, sa.median);
        ok(s.worst > a.worst + 3.0 && s.median >= a.median - 0.5, "SAM beats AM in the fades and is no worse between them");
        ok(sa.worst > aa.worst + 3.0 && sa.median >= aa.median - 0.5, "…and still does through the production AGC");
    }

    std::printf("── 3. clean signal: SAM no worse than AM ──\n");
    {
        for (double cnr : { 45.0, 25.0, 15.0 }) {
            auto z = amSignal(p, 10.0, 3.0, cnr, 31);
            Rx am(false), sam(Det::SAM, 5000.0, false);
            const Stat a = sinadStats(demod(am, z), p), s = sinadStats(demod(sam, z), p);
            char b[160]; std::snprintf(b, sizeof b, "CNR %2.0f dB: AM %5.1f dB, SAM %5.1f dB (median SINAD)", cnr, a.median, s.median);
            ok(s.median >= a.median - 1.0, b);
        }
    }

    std::printf("── 4. sideband rejection (SAM-U / ECSS-U / SAM-L / ECSS-L) ──\n");
    {
        const double binHz = FS / WIN;
        for (double fu : { 400.0, 1500.0, 3200.0 }) {
            const int bu = (int)std::lround(fu / binHz), bl = (int)std::lround((fu * 0.77) / binHz);
            const long n = (long)(6.0 * FS);
            std::vector<cf32> z((size_t)n);
            for (long i = 0; i < n; ++i) {
                const double ph = 2 * M_PI * 5.0 * i / FS;    // 5 Hz off
                const cf32 c((float)std::cos(ph), (float)std::sin(ph));
                const double au = 2 * M_PI * bu * (double)i / WIN, al = -2 * M_PI * bl * (double)i / WIN;
                z[i] = c * (cf32(1.0f, 0.0f) + 0.2f * cf32((float)std::cos(au), (float)std::sin(au))
                                             + 0.2f * cf32((float)std::cos(al), (float)std::sin(al)));
            }
            for (Det d : { Det::SAM_U, Det::ECSS_U, Det::SAM_L, Det::ECSS_L }) {
                Rx rx(d, 5000.0, false);
                const auto y = demod(rx, z);
                const float* w = y.data() + (size_t)(3.0 * FS);
                const bool up = d == Det::SAM_U || d == Det::ECSS_U;
                const double want = toneDb(w, up ? bu : bl), unwanted = toneDb(w, up ? bl : bu);
                const char* nm = d == Det::SAM_U ? "SAM-U" : d == Det::ECSS_U ? "ECSS-U" : d == Det::SAM_L ? "SAM-L" : "ECSS-L";
                char b[160]; std::snprintf(b, sizeof b, "%-6s wanted %4.0f Hz vs other side %4.0f Hz: rejection %5.1f dB", nm,
                                           up ? bu * binHz : bl * binHz, up ? bl * binHz : bu * binHz, want - unwanted);
                ok(want - unwanted >= 35.0, b);
            }
        }
    }

    std::printf("── 5. AM stereo (C-QUAM compatible) ──\n");
    {
        const double binHz = FS / WIN;
        const int bL = (int)std::lround(410.0 / binHz), bR = (int)std::lround(1130.0 / binHz);
        auto cquam = [&](bool pilot, double secs, double cnr, double fadeRho, double pilotLvl = 0.05) {
            const long n = (long)(secs * FS);
            std::vector<cf32> z((size_t)n);
            std::mt19937 r(5); std::normal_distribution<double> g(0, 1);
            const double nAmp = std::pow(10.0, -cnr / 20.0) * std::sqrt(0.5) * std::sqrt(FS / 10000.0);
            for (long i = 0; i < n; ++i) {
                const double t = i / FS;
                const double L = 0.30 * std::cos(2 * M_PI * bL * (double)i / WIN), R = 0.30 * std::cos(2 * M_PI * bR * (double)i / WIN + 1.0);
                const double I = 1.0 + L + R, Q = (L - R) + (pilot ? pilotLvl * std::sin(2 * M_PI * 25.0 * t) : 0.0);
                const double phi = std::atan2(Q, I), env = I;
                cf32 v((float)(env * std::cos(phi)), (float)(env * std::sin(phi)));
                if (fadeRho > 0.0) v *= (float)(1.0 - fadeRho * 0.5 * (1.0 + std::cos(2 * M_PI * 0.3 * t)));   // flat fading
                const double off = 2 * M_PI * 9.0 * t;
                v *= cf32((float)std::cos(off), (float)std::sin(off));
                v += cf32((float)(nAmp * g(r)), (float)(nAmp * g(r)));
                z[i] = v * 0.02f;
            }
            return z;
        };
        {
            auto z = cquam(true, 8.0, 45.0, 0.0);
            Rx rx(Det::STEREO, 5000.0, false);
            std::vector<float> L(z.size()), R(z.size());
            double pilotAt = -1;
            for (size_t i = 0; i < z.size(); i += 1200) {
                const int k = (int)std::min<size_t>(1200, z.size() - i);
                rx.run(z.data() + i, L.data() + i, k, R.data() + i);
                if (pilotAt < 0 && rx.s.status().pilot) pilotAt = (i + k) / FS;
            }
            const float* l = L.data() + (size_t)(5.0 * FS); const float* r = R.data() + (size_t)(5.0 * FS);
            const double sepL = toneDb(l, bL) - toneDb(l, bR), sepR = toneDb(r, bR) - toneDb(r, bL);
            char b[200]; std::snprintf(b, sizeof b, "pilot found at %.2f s (level %.3f); separation L %.1f dB, R %.1f dB",
                                       pilotAt, rx.s.status().pilotLevel, sepL, sepR);
            ok(pilotAt > 0 && pilotAt <= 1.5 && sepL >= 25.0 && sepR >= 25.0, b);
            const int b25 = (int)std::lround(25.0 / binHz);
            const double pil = toneDb(l, b25) - toneDb(l, bL);
            std::snprintf(b, sizeof b, "the 25 Hz pilot is removed from the audio: %.1f dB under the programme", -pil);
            ok(pil < -40.0, b);
            // mono compatibility: the envelope detector hears L+R
            Rx env(false);
            auto y = demod(env, z);
            const float* m = y.data() + (size_t)(5.0 * FS);
            const double lv = toneDb(m, bL), rv = toneDb(m, bR);
            // distortion: everything but the two tones and DC/pilot region
            std::snprintf(b, sizeof b, "mono compatibility: envelope AM hears L %.1f and R %.1f dB (equal, both present)", lv, rv);
            ok(std::fabs(lv - rv) < 0.5, b);
        }
        {
            auto z = cquam(false, 6.0, 40.0, 0.0);
            Rx rx(Det::STEREO, 5000.0, false);
            std::vector<float> L(z.size()), R(z.size());
            bool ever = false;
            for (size_t i = 0; i < z.size(); i += 1200) {
                const int k = (int)std::min<size_t>(1200, z.size() - i);
                rx.run(z.data() + i, L.data() + i, k, R.data() + i);
                ever |= rx.s.status().pilot;
            }
            const float* l = L.data() + (size_t)(4.0 * FS); const float* r = R.data() + (size_t)(4.0 * FS);
            double diff = 0, sum = 0; for (int i = 0; i < WIN; ++i) { diff += (l[i] - r[i]) * (l[i] - r[i]); sum += l[i] * l[i]; }
            char b[160]; std::snprintf(b, sizeof b, "no pilot: never stereo (%s), L = R (difference %.1f dB)", ever ? "WAS" : "never",
                                       10 * std::log10(diff / (sum + 1e-30) + 1e-30));
            ok(!ever && diff < sum * 1e-6, b);
        }
        {
            auto z = cquam(true, 12.0, 25.0, 0.6);
            Rx rx(Det::STEREO, 5000.0, false);
            std::vector<float> L(z.size()), R(z.size());
            int on = 0, cnt = 0;
            for (size_t i = 0; i < z.size(); i += 1200) {
                const int k = (int)std::min<size_t>(1200, z.size() - i);
                rx.run(z.data() + i, L.data() + i, k, R.data() + i);
                if (i > 3 * FS) { cnt++; on += rx.s.status().pilot; }
            }
            const float* l = L.data() + (size_t)(8.0 * FS);
            const double sepL = toneDb(l, bL) - toneDb(l, bR);
            char b[160]; std::snprintf(b, sizeof b, "25 dB CNR + 60%% flat fading: stereo held %d%% of the time, separation L %.1f dB", 100 * on / std::max(1, cnt), sepL);
            ok(on > cnt * 9 / 10 && sepL > 20.0, b);
        }
    }

    std::printf("── 6. auto bandwidth ──\n");
    {
        const Prog p45(4200.0, 48);          // ★ dense: every 250 Hz band carries programme, as speech/music does
        struct AC { const char* name; double neigh, ndb, cnr; };
        const AC acs[] = { { "neighbour +5 kHz at -6 dB (SW spacing), CNR 40", 5000.0, -6.0, 40.0 },
                           { "neighbour -9 kHz at +6 dB, width 16 kHz, CNR 40", -9000.0, 6.0, 40.0 },
                           { "no neighbour, CNR 20 (noise only)", 0.0, 0.0, 20.0 },
                           { "no neighbour, CNR 45 (clean)", 0.0, 0.0, 45.0 } };
        for (const AC& c : acs) {
            const double edge = c.neigh == -9000.0 ? 8000.0 : 5000.0;
            auto z = amSignal(p45, 16.0, 4.0, c.cnr, 41, 0, 0, 0, c.neigh, c.ndb);
            Rx am(false), amA(Det::ENV, edge, true), sam(Det::SAM, edge, false), samA(Det::SAM, edge, true);
            const Stat a = sinadStats(demod(am, z), p45, 6.0), aa = sinadStats(demod(amA, z), p45, 6.0);
            const Stat s = sinadStats(demod(sam, z), p45, 6.0), sa = sinadStats(demod(samA, z), p45, 6.0);
            const auto st = samA.s.status(), ste = amA.s.status();
            Rx samL(Det::SAM_L, edge, false), samU(Det::SAM_U, edge, false);
            const Stat sl = sinadStats(demod(samL, z), p45, 6.0), su = sinadStats(demod(samU, z), p45, 6.0);
            // top end kept, against SAM on the same programme with nothing else on air
            auto zc = amSignal(p45, 8.0, 4.0, 80.0, 41);
            Rx ref(Det::SAM, edge, false);
            const auto yr = demod(ref, zc);
            const double topRef = topEndDb(yr.data() + (size_t)(6.0 * FS), p45);
            const auto ysa = demod(samA, z); const auto yaa = demod(amA, z);
            std::printf("   %s\n      AM %5.1f -> auto %5.1f dB (edge %.0f, top end %+.1f dB) | SAM %5.1f -> auto %5.1f dB "
                        "(L %.0f / U %.0f, mrc %.2f, notch %.0f, top end %+.1f dB) | SAM-L %5.1f SAM-U %5.1f\n",
                        c.name, a.median, aa.median, ste.edgeHiHz, topEndDb(yaa.data() + (size_t)(12.0 * FS), p45) - topRef,
                        s.median, sa.median, st.edgeLoHz, st.edgeHiHz, st.mrcUpper, st.notchHz,
                        topEndDb(ysa.data() + (size_t)(12.0 * FS), p45) - topRef, sl.median, su.median);
            if (c.neigh != 0.0) {
                ok(sa.median > s.median + 3.0, "auto bandwidth helps SAM against the neighbour (> 3 dB)");
                ok(aa.median > a.median + 1.0, "…and envelope AM (> 1 dB)");
            } else {
                ok(sa.median >= s.median - 0.5 && aa.median >= a.median - 0.5, "no neighbour: auto never hurts (within 0.5 dB)");
            }
        }
    }

    std::printf("── 7. retune clears; carrier loss eases to the envelope ──\n");
    {
        const Prog p45(4200.0);
        auto z = amSignal(p45, 12.0, 4.0, 40.0, 41, 0, 0, 0, 5000.0, -6.0);
        Rx samA(Det::SAM, 5000.0, true);
        demod(samA, z);
        const auto before = samA.s.status();
        samA.s.retune();
        const auto after = samA.s.status();
        char b[160]; std::snprintf(b, sizeof b, "upper edge %.0f before the retune, %.0f after (the listener's 5000)", before.edgeHiHz, after.edgeHiHz);
        ok(before.edgeHiHz < 4900.0 && after.edgeHiHz == 5000.0 && !after.locked, b);
        // carrier switched off half way: no burst — the output peak after the loss stays below the programme peak
        auto z2 = amSignal(p45, 8.0, 4.0, 40.0, 43);
        { std::mt19937 r(9); std::normal_distribution<float> g(0, 1);
          const float na = 0.01f * (float)(std::pow(10.0, -40.0 / 20.0) * std::sqrt(0.5) * std::sqrt(FS / 10000.0));
          for (size_t i = (size_t)(4.0 * FS); i < z2.size(); ++i) z2[i] = cf32(na * g(r), na * g(r)); }   // station gone, noise remains
        Rx sam(Det::SAM, 5000.0, false, true);
        const auto y = demod(sam, z2);
        float pk1 = 0, pk2 = 0;
        for (size_t i = (size_t)(2.0 * FS); i < (size_t)(4.0 * FS); ++i) pk1 = std::max(pk1, std::fabs(y[i]));
        for (size_t i = (size_t)(4.0 * FS); i < y.size(); ++i) pk2 = std::max(pk2, std::fabs(y[i]));
        std::snprintf(b, sizeof b, "station vanishes: peak after %.2f vs programme %.2f; blend now %.2f", pk2, pk1, sam.s.status().blend);
        ok(pk2 < pk1 * 1.2f && std::isfinite(pk2) && sam.s.status().blend < 0.2f, b);
    }

    std::printf("── 8. cost per listener at %.0f kHz (%% of one core) ──\n", FS / 1e3);
    {
        const Prog p45(4200.0);
        auto z = amSignal(p45, 10.0, 4.0, 35.0, 51);
        struct M { const char* nm; Det d; bool env, autoBw; };
        const M ms[] = { { "AM (shipping)", Det::ENV, true, false }, { "AM + auto BW", Det::ENV, false, true },
                         { "SAM", Det::SAM, false, false }, { "SAM + auto BW", Det::SAM, false, true },
                         { "SAM-U", Det::SAM_U, false, false }, { "ECSS-L + auto", Det::ECSS_L, false, true },
                         { "AM stereo", Det::STEREO, false, false }, { "AM stereo + auto", Det::STEREO, false, true } };
#ifdef VIBE_FORCE_SCALAR
        std::printf("   (SCALAR build)\n");
#else
        std::printf("   (SIMD build)\n");
#endif
        for (const M& m : ms) {
            std::unique_ptr<Rx> rxp(m.env ? new Rx(false) : new Rx(m.d, 5000.0, m.autoBw)); Rx& rx = *rxp;
            std::vector<float> o(z.size()), r(z.size());
            const double t0 = cpuNow();
            for (size_t i = 0; i < z.size(); i += 1200) rx.run(z.data() + i, o.data() + i, (int)std::min<size_t>(1200, z.size() - i), r.data() + i);
            const double dt = cpuNow() - t0;
            std::printf("   %-18s %6.3f %%\n", m.nm, 100.0 * dt / 10.0);
        }
    }
    std::printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
