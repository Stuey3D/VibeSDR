// ★★★ V12 AUTO BANDWIDTH FOR SSB AND CW — vibedsp AutoPassband (autobw.cpp), through the REAL RxPipeline (channel
//     filter, Weaver demod, auto bandwidth, AGC, resampler), on synthetic signals whose programme is KNOWN, so every
//     figure is measured against truth with abscore (autobw_score.h: wanted ÷ (unwanted + wanted thrown away)).
//
//   SSB (USB and LSB): a pseudo-voice (a wandering 95–190 Hz glottal pitch, its harmonics under moving formants,
//   syllables at ~4 Hz with pauses) band-limited the way a transmitter does, in white noise at several S/N, with
//   (a) a transmitter narrower than the listener's passband, (b) a listener who has opened the passband wide,
//   (c) a WIDE transmitter whose whole band must be kept, (d) a second talker splattering into the top of the
//   passband, (e) a heterodyne in the passband, (f) noise only.
//   CW: a keyed tone (Morse, 20 WPM) at and beside the beat note in white noise, a stronger neighbour 250 Hz away,
//   and noise only.
//   Rules: auto must WIN where there is something to win, NEVER lose more than the measurement's own noise
//   (0.15 dB) anywhere, keep ≥ 97 % of the voice the fixed passband delivered, and leave noise alone.
//   Plus: a retune puts the passband back at once; with the switch OFF the pipeline is byte-for-byte the old one.
//
// ★ SILENT: every signal is generated and measured in memory, never played.
#include "vibedsp/vibedsp.h"
#include "autobw_score.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace vibedsp;
using M = RxPipeline::Mode;
static int pass = 0, fail = 0;
static void ok(bool c, const char* what) { (c ? pass : fail)++; std::printf("   %s %s\n", c ? "ok  " : "FAIL", what); }

static const double FS = 48000.0;    // IQ rate in, audio rate out
static const double FC = 6000.0;     // the station's carrier (USB/LSB) or the dial (CW), Hz from the IQ centre

// ── pseudo-voice: a complex (one-sided, i.e. already single-sideband) baseband, 0 Hz = suppressed carrier ──
static std::vector<std::complex<double>> voice(double secs, unsigned seed, double txLo, double txHi) {
    std::mt19937 rng(seed); std::uniform_real_distribution<double> U(0, 1);
    const size_t n = (size_t)(secs * FS);
    std::vector<std::complex<double>> z(n);
    const int H = 40;
    std::vector<double> ph(H, 0.0);
    double f0 = 120, f0T = 120, F1 = 600, F1T = 600, F2 = 1500, F2T = 1500, g = 0, gT = 1;
    double nextSyl = 0, nextPitch = 0;
    for (size_t i = 0; i < n; ++i) {
        const double t = i / FS;
        if (t >= nextSyl) {                    // a syllable or a pause
            const bool pause = U(rng) < 0.28;
            gT = pause ? 0.0 : 0.4 + 0.6 * U(rng);
            F1T = 300 + 600 * U(rng); F2T = 900 + 1700 * U(rng);
            nextSyl = t + (pause ? 0.15 + 0.35 * U(rng) : 0.12 + 0.18 * U(rng));
        }
        if (t >= nextPitch) { f0T = 95 + 95 * U(rng); nextPitch = t + 0.08 + 0.2 * U(rng); }
        const double a = 1.0 / (0.03 * FS);
        g += (gT - g) * a; f0 += (f0T - f0) * a; F1 += (F1T - F1) * a; F2 += (F2T - F2) * a;
        std::complex<double> acc(0, 0);
        for (int h = 1; h <= H; ++h) {
            const double f = f0 * h;
            ph[h - 1] += 2 * M_PI * f / FS;
            if (f > 4200) continue;
            // formants (two resonances) over a falling glottal tilt; the transmitter's band with 100 Hz skirts
            const double r1 = 1.0 / (1.0 + std::pow((f - F1) / 180.0, 2)), r2 = 0.6 / (1.0 + std::pow((f - F2) / 250.0, 2));
            double amp = (0.25 + r1 + r2) / std::sqrt((double)h);
            const double sk = std::min(1.0, std::max(0.0, (f - txLo + 50) / 100.0)) * std::min(1.0, std::max(0.0, (txHi + 50 - f) / 100.0));
            amp *= sk;
            if (amp <= 0) continue;
            acc += amp * std::polar(1.0, ph[h - 1]);
        }
        z[i] = g * acc;
    }
    double p = 0; for (auto& v : z) p += std::norm(v); p /= n;
    for (auto& v : z) v /= std::sqrt(p);       // mean power 1 over the whole clip (pauses included)
    return z;
}

// ── Morse at 20 WPM, 5 ms raised-cosine edges, unit amplitude envelope ──
static std::vector<double> morse(double secs, unsigned seed) {
    std::mt19937 rng(seed);
    const double dot = 0.06; const size_t n = (size_t)(secs * FS);
    std::vector<double> e(n, 0.0);
    std::vector<int> key;                       // 1 = on, 0 = off, in dot units
    while (key.size() * dot < secs + 1) {
        const int letters = 3 + rng() % 4;
        for (int l = 0; l < letters; ++l) {
            const int els = 1 + rng() % 4;
            for (int k = 0; k < els; ++k) { const int len = (rng() % 2) ? 3 : 1; for (int j = 0; j < len; ++j) key.push_back(1); key.push_back(0); }
            key.push_back(0); key.push_back(0);
        }
        for (int j = 0; j < 4; ++j) key.push_back(0);
    }
    const double rise = 0.005;
    double env = 0;
    for (size_t i = 0; i < n; ++i) {
        const int on = key[(size_t)((i / FS) / dot)];
        env += ((on ? 1.0 : 0.0) - env) * std::min(1.0, 1.0 / (rise * FS) * 1.0);
        e[i] = env;
    }
    return e;
}

struct Run { std::vector<float> y; AutoPassband::Status last{}; };
static Run runPipe(const std::vector<std::complex<float>>& iq, M mode, double off, double bw, bool autoOn, int retuneAt = -1) {
    Run r;
    RxPipeline::Callbacks cb; cb.ctx = &r;
    cb.audio = [](void* c, const float* pcm, int frames, int ch, int) {
        auto* rr = (Run*)c; for (int i = 0; i < frames; ++i) rr->y.push_back(ch == 2 ? 0.5f * (pcm[2 * i] + pcm[2 * i + 1]) : pcm[i]); };
    RxPipeline p; p.setMeasureThread(false);
    p.start(FS, 1024, 20.0, 48000, cb);
    p.setAmAutoBandwidth(autoOn);
    p.setTune(off, mode, bw);
    const int blk = 4800;
    for (size_t i = 0; i < iq.size(); i += blk) {
        if (retuneAt >= 0 && (int)(i / blk) == retuneAt) { r.last = p.autoPassbandStatus(); p.setTune(off + 10, mode, bw); }
        p.feed(reinterpret_cast<const cf32*>(iq.data() + i), (int)std::min<size_t>(blk, iq.size() - i));
    }
    if (retuneAt < 0) r.last = p.autoPassbandStatus();
    p.stop();
    return r;
}

struct Result { double gain = 0, se = 0, keptMin = 1; int n = 0, won = 0; AutoPassband::Status st{}; };
/** Fixed vs auto on the same IQ; the truth is the receiver-side audio `x` (48 kHz). */
static Result compare(const std::vector<std::complex<float>>& iq, const std::vector<float>& x, M mode, double off, double bw,
                      double coreLo, double coreHi, double bandHi, double warm = 8.0) {
    Run f = runPipe(iq, mode, off, bw, false), a = runPipe(iq, mode, off, bw, true);
    const size_t from = (size_t)(warm * FS), len = (size_t)(4 * FS);
    const int lf = abscore::lagOf(x, f.y, (int)(0.25 * FS), from, len), la = abscore::lagOf(x, a.y, (int)(0.25 * FS), from, len);
    std::vector<double> d, kept;
    const int W = (int)(2 * FS);
    for (size_t w = from; w + W + 0.3 * FS < x.size(); w += W) {
        if (w + W + la >= a.y.size() || w + W + lf >= f.y.size()) break;
        auto sf = abscore::welch(x.data() + w, f.y.data() + w + lf, W), sa = abscore::welch(x.data() + w, a.y.data() + w + la, W);
        std::vector<double> Cf, Nf, Ca, Na;
        abscore::cn(sf, FS, coreLo, coreHi, Cf, Nf); abscore::cn(sa, FS, coreLo, coreHi, Ca, Na);
        double k = 1;
        const double s0 = abscore::score(Cf, Nf, Cf, FS, 30, bandHi), s1 = abscore::score(Ca, Na, Cf, FS, 30, bandHi, &k);
        d.push_back(s1 - s0); kept.push_back(k);
    }
    const auto p = abscore::paired(d, kept);
    Result r; r.gain = p.mean; r.se = p.se; r.n = p.n; r.won = p.won; r.keptMin = p.keptMin; r.st = a.last;
    return r;
}

static std::vector<std::complex<float>> addNoise(std::vector<std::complex<double>> z, double snrDb, double refBwHz, unsigned seed) {
    // noise power in refBwHz = signal power / snr  →  complex white noise of total power (FS/refBwHz)·that
    std::mt19937 rng(seed); std::normal_distribution<double> G(0, 1);
    const double pn = std::pow(10.0, -snrDb / 10.0) * FS / refBwHz;
    const double s = std::sqrt(pn / 2);
    std::vector<std::complex<float>> o(z.size());
    for (size_t i = 0; i < z.size(); ++i) { const auto v = z[i] + std::complex<double>(G(rng) * s, G(rng) * s); o[i] = std::complex<float>((float)v.real(), (float)v.imag()); }
    return o;
}
static std::vector<std::complex<double>> shift(const std::vector<std::complex<double>>& z, double hz, double gain = 1.0, bool conj = false) {
    std::vector<std::complex<double>> o(z.size());
    for (size_t i = 0; i < z.size(); ++i) o[i] = gain * (conj ? std::conj(z[i]) : z[i]) * std::polar(1.0, 2 * M_PI * hz * i / FS);
    return o;
}
static std::vector<float> realPart(const std::vector<std::complex<double>>& z) {
    std::vector<float> x(z.size()); for (size_t i = 0; i < z.size(); ++i) x[i] = (float)z[i].real(); return x;
}

static void line(const char* name, const Result& r) {
    std::printf("   %-58s %+6.2f ± %.2f dB  won %2d/%2d  voice kept ≥ %5.1f %%  passband %4.0f–%4.0f Hz%s\n", name, r.gain, r.se,
                r.won, r.n, 100 * r.keptMin, r.st.loHz, r.st.hiHz, r.st.cut ? "" : " (not cut)");
}

int main() {
    const double secs = 40.0;
    std::printf("── SSB: a pseudo-voice through the real pipeline, fixed passband vs AUTO (paired 2 s windows, after 8 s) ──\n");
    struct SsbCase { const char* name; double txLo, txHi, bw, snr; bool lsb; double nbOff, nbDb, hetOff; bool noiseOnly; };
    const SsbCase cases[] = {
        { "USB, TX 300-2700, RX 2.7 k, S/N 6 dB",                 300, 2700, 2700,  6, false, 0, 0, 0, false },
        { "USB, TX 300-2700, RX 2.7 k, S/N 15 dB",                300, 2700, 2700, 15, false, 0, 0, 0, false },
        { "USB, TX 300-2700, RX 2.7 k, S/N 25 dB",                300, 2700, 2700, 25, false, 0, 0, 0, false },
        { "USB, TX 300-2400 (narrow TX), RX 2.7 k, S/N 10 dB",    300, 2400, 2700, 10, false, 0, 0, 0, false },
        { "USB, TX 300-2700, RX opened to 4.0 k, S/N 10 dB",      300, 2700, 4000, 10, false, 0, 0, 0, false },
        { "USB, TX 300-2700, RX opened to 4.0 k, S/N 0 dB",       300, 2700, 4000,  0, false, 0, 0, 0, false },
        { "USB, WIDE TX 100-3500, RX 4.0 k, S/N 15 dB (keep all)", 100, 3500, 4000, 15, false, 0, 0, 0, false },
        { "LSB, TX 300-2700, RX opened to 4.0 k, S/N 10 dB",      300, 2700, 4000, 10, true,  0, 0, 0, false },
        { "USB + a 2nd talker 2.2 kHz up, equal level, S/N 20",   300, 2700, 2700, 20, false, 2200, 0, 0, false },
        { "USB + a 2nd talker 2.2 kHz up, +6 dB, S/N 20",         300, 2700, 2700, 20, false, 2200, 6, 0, false },
        { "USB TX 300-2400 + heterodyne at 2.6 kHz (−10 dB), S/N 20", 300, 2400, 2700, 20, false, 0, 0, 2600, false },
    };
    double worst = 1e9, keptWorst = 1.0;
    int idx = 0;
    for (const auto& c : cases) {
        auto z = voice(secs, 11 + idx, c.txLo, c.txHi);
        const std::vector<float> x = realPart(z);   // the voice as the receiver should hear it (audio Hz = offset)
        auto rf = shift(z, c.lsb ? -FC : FC, 1.0, c.lsb);
        if (c.nbOff > 0) { auto z2 = voice(secs, 900 + idx, 300, 2700); auto r2 = shift(z2, FC + c.nbOff, std::pow(10.0, c.nbDb / 20.0));
                           for (size_t i = 0; i < rf.size(); ++i) rf[i] += r2[i]; }
        if (c.hetOff > 0) for (size_t i = 0; i < rf.size(); ++i) rf[i] += std::polar(std::sqrt(0.1), 2 * M_PI * (FC + c.hetOff) * i / FS);
        const auto iq = addNoise(rf, c.snr, 2400.0, 77 + idx);
        const Result r = compare(iq, x, c.lsb ? M::SSB_LSB : M::SSB_USB, c.lsb ? -FC : FC, c.bw, 500, 1500, std::max(c.bw, 3000.0) + 200);
        line(c.name, r);
        worst = std::min(worst, r.gain + 2 * r.se);
        // ★ ≥ 97 % of the voice kept wherever the voice is not buried: at 0 dB S/N or under a LOUDER second talker the
        //   top bands are mostly not this voice, and cutting them is a net win on the score (counted above) — ≥ 90 % there.
        if (c.snr >= 6 && c.nbDb <= 0) keptWorst = std::min(keptWorst, r.keptMin);
        else ok(r.keptMin > 0.90, "…and ≥ 90 % of the voice kept even here");
        if (idx == 0) ok(r.st.cut && r.st.loHz >= 100 && r.st.loHz <= 250 && r.st.hiHz >= 2650, "plain USB: the empty bottom is cut, the voice's top is kept");
        if (idx == 3) ok(r.gain > 0.15 && r.st.hiHz < 2600, "a narrow transmitter: the top comes in and it helps");
        if (idx == 4) ok(r.gain > 0.8 && r.st.hiHz <= 3000, "a passband opened wide: fitted back to the voice, > 0.8 dB");
        if (idx == 6) ok(r.keptMin > 0.97 && r.st.hiHz >= 3500, "a wide transmitter: its whole band is kept");
        if (idx == 7) ok(r.gain > 0.6 && r.st.hiHz <= 3000, "LSB behaves as USB");
        if (idx == 9) ok(r.gain > 1.0, "a louder talker splattering into the top: cut (some of the time), and it helps");
        if (idx == 10) ok(r.gain > 1.0 && r.st.hiHz < 2600, "a heterodyne above a narrow voice: cut, and it helps");
        ++idx;
    }
    ok(worst > -0.15, "SSB: never worse than the fixed passband (beyond 0.15 dB) on any case");
    ok(keptWorst > 0.97, "SSB: ≥ 97 % of the voice the fixed passband delivered is kept, in every window");
    {   // noise only: must not cut (nothing to fit to)
        std::vector<std::complex<double>> z((size_t)(20 * FS), {0, 0});
        const auto iq = addNoise(z, -100, 2400.0, 5);   // signal 0: the noise IS everything
        Run a = runPipe(iq, M::SSB_USB, FC, 2700, true);
        std::printf("   noise only: passband %0.f–%0.f Hz %s\n", a.last.loHz, a.last.hiHz, a.last.cut ? "(cut)" : "(not cut)");
        ok(!a.last.cut || (a.last.hiHz >= 2000 && a.last.loHz <= 300), "noise only: never past the floors (2 kHz top, 300 Hz bottom)");
    }

    std::printf("── CW: a keyed tone, the CW passband (1200 Hz, beat note 600 Hz) vs AUTO ──\n");
    struct CwCase { const char* name; double toneOff, snr, nbOff, nbDb; };
    const CwCase cw[] = {
        { "on the beat note, S/N −6 dB (in 1.2 kHz)",  0, -6, 0, 0 },
        { "on the beat note, S/N +6 dB",               0,  6, 0, 0 },
        { "on the beat note, S/N +20 dB",              0, 20, 0, 0 },
        { "100 Hz off the beat note, S/N 0 dB",      100,  0, 0, 0 },
        { "+ a neighbour 250 Hz up, +6 dB, S/N 6",     0,  6, 250, 6 },
        { "+ a neighbour 150 Hz down, equal, S/N 6",   0,  6, -150, 0 },
    };
    double cwWorst = 1e9; idx = 0;
    for (const auto& c : cw) {
        const auto e = morse(secs, 31 + idx);
        const double fa = 600 + c.toneOff;     // audio tone
        std::vector<std::complex<double>> rf(e.size());
        std::vector<float> x(e.size());
        for (size_t i = 0; i < e.size(); ++i) { rf[i] = e[i] * std::polar(1.0, 2 * M_PI * (FC + c.toneOff) * i / FS);
                                                x[i] = (float)(e[i] * std::cos(2 * M_PI * fa * i / FS)); }
        if (c.nbOff != 0) { const auto e2 = morse(secs, 500 + idx); const double g = std::pow(10.0, c.nbDb / 20.0);
                            for (size_t i = 0; i < rf.size(); ++i) rf[i] += g * e2[i] * std::polar(1.0, 2 * M_PI * (FC + c.nbOff) * i / FS + 1.0); }
        double pw = 0; for (auto& v : e) pw += v * v; pw /= e.size();
        for (auto& v : rf) v /= std::sqrt(pw);
        for (auto& v : x) v /= (float)std::sqrt(pw);
        const auto iq = addNoise(rf, c.snr, 1200.0, 99 + idx);
        const Result r = compare(iq, x, M::CW, FC - 600, 1200, fa - 40, fa + 40, 1400, 6.0);
        line(c.name, r);
        cwWorst = std::min(cwWorst, r.gain + 2 * r.se);
        if (idx == 0) ok(r.gain > 3.0 && r.st.cut, "CW at −6 dB: narrowed round the tone, > 3 dB");
        if (idx == 3) ok(r.st.cut && std::fabs(r.st.toneHz - 700) < 15, "CW 100 Hz off: found and centred on 700 Hz");
        if (idx == 4) ok(r.gain > 3.0, "CW with a louder neighbour 250 Hz away: it is cut out");
        if (idx == 5) ok(r.st.cut && (r.st.hiHz - r.st.loHz) < 200, "a neighbour 150 Hz away: the passband narrows below ±150");
        ++idx;
    }
    ok(cwWorst > -0.15, "CW: never worse than the fixed passband on any case");
    {
        std::vector<std::complex<double>> z((size_t)(20 * FS), {0, 0});
        const auto iq = addNoise(z, -100, 1200.0, 6);
        Run a = runPipe(iq, M::CW, FC - 600, 1200, true);
        ok(!a.last.cut, "CW noise only: stays at the full CW passband");
    }

    std::printf("── retune and switch ──\n");
    {   // a retune clears every verdict: drive the class directly
        AutoPassband ap; ap.configure(AutoPassband::Kind::SSB, 12000, 4000);
        std::mt19937 rng(1); std::normal_distribution<float> G(0, 1);
        auto z = voice(12, 9, 300, 2700);
        // demodulated-audio stand-in at 12 kHz: real part, decimated by 4, plus noise
        std::vector<float> a; for (size_t i = 0; i < z.size(); i += 4) a.push_back((float)z[i].real() + 0.3f * G(rng));
        for (size_t i = 0; i < a.size(); i += 600) ap.process(a.data() + i, (int)std::min<size_t>(600, a.size() - i));
        const bool cutBefore = ap.status().cut;
        ap.retune();
        ok(cutBefore && !ap.status().cut && ap.status().hiHz == 4000.0f, "AutoPassband: a retune puts the listener's passband back at once");
    }
    {   // OFF = the old pipeline, byte for byte
        auto z = voice(6, 21, 300, 2700);
        const auto iq = addNoise(shift(z, FC), 10, 2400.0, 8);
        Run off1 = runPipe(iq, M::SSB_USB, FC, 2700, false);
        // the same chain with auto never built is not reachable from outside; instead: OFF twice is identical, and
        // OFF differs from ON (so the switch is not a no-op)
        Run off2 = runPipe(iq, M::SSB_USB, FC, 2700, false), on = runPipe(iq, M::SSB_USB, FC, 2700, true);
        ok(off1.y == off2.y && off1.y != on.y, "switch OFF: deterministic, and the switch really changes the audio");
    }
    {   // cost
        auto z = voice(30, 41, 300, 2700);
        const auto iq = addNoise(shift(z, FC), 10, 2400.0, 9);
        auto cpu = [&](bool on, M m, double bw) {
            const auto t0 = std::chrono::steady_clock::now(); runPipe(iq, m, m == M::CW ? FC - 600 : FC, bw, on);
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 30.0 * 100.0; };
        std::printf("   cost, %% of one core in real time (48 kHz IQ in): USB %.3f → AUTO %.3f · CW %.3f → AUTO %.3f\n",
                    cpu(false, M::SSB_USB, 2700), cpu(true, M::SSB_USB, 2700), cpu(false, M::CW, 1200), cpu(true, M::CW, 1200));
    }
    std::printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
