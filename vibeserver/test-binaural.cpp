// ★★ BINAURAL TUNING (V12) — vibedsp/binaural.h: the spatialiser, the controller, the WFM hand-off and the SSB
//    offset source, on synthetic signals with KNOWN offsets.
//
//   1. The pan law: 0 inside the dead zone, + for a signal ABOVE the dial (lean right = tune up), full scale.
//   2. The effect: the far ear is quieter (≤ 10 dB, never silent), later (~0.6 ms) and duller (head shadow);
//      on tune both ears are the input exactly; a jumping target makes no click; nothing turns into NaN.
//   3. The controller on a given offset (the spectrum sources: AM carrier, CW peak, FM centroid).
//   4. WFM: a locked pilot fades the aid out; a FLICKERING pilot does not make it alternate.
//   5. SSB: a synthetic voice (harmonics of a gliding pitch, as test-ssb-offset) at known offsets, through the
//      FAST estimator + running median — the sign per sideband, the dead zone at 0 Hz, noise never believed,
//      the dial move carried at once.
//   6. Cost per second of audio.
//   7. The COARSE SSB cue (the waterfall's carrier): drives the lean where the estimator has no answer or none in
//      reach, never fights a valid in-reach answer, keeps the knob's exact figure when it agrees, hands over to
//      the estimator, follows the dial, lets go when it goes invalid.
//   8. THE SPREAD MAP at full amount (BinauralSpread alone): tones placed by audio frequency (USB: 500 Hz left,
//      2200 Hz right; LSB mirrored), clamped past the default passband's edges, one voice centred overall, never
//      silent, the CW note measured from the noise; latency printed.
//   9. ONE BEHAVIOUR (Stuart on 476: "when on signal it presents in mono as it would before and then pan left and
//      right based on signal and audio response when off tune"): on tune the output IS the input (delayed by the
//      bank's constant frame on SSB/CW, not at all elsewhere); off tune the lean AND the spread, both from the same
//      weight w, growing together; LSB mirrored; none of the spread on AM/FM; the SSB estimator still fed and
//      publishing with the spread map set; no click or NaN through toggles, w changes and a sideband change; cost.
//
// ★ SILENT: every signal is generated and measured in memory, never played.
#include "vibedsp/binaural.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace vibedsp;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("   %s   %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) fails++;
}

static double rms(const std::vector<float>& x, size_t from, size_t to) {
    double s = 0;
    for (size_t i = from; i < to; ++i) s += (double)x[i] * x[i];
    return std::sqrt(s / (double)(to - from));
}
static double db(double a, double b) { return 20 * std::log10((a + 1e-30) / (b + 1e-30)); }
static bool finite(const std::vector<float>& x) { for (float v : x) if (!std::isfinite(v)) return false; return true; }

static std::vector<float> tone(double rate, double secs, double hz, double amp) {
    std::vector<float> x((size_t)(rate * secs));
    for (size_t i = 0; i < x.size(); ++i) x[i] = (float)(amp * std::sin(2 * M_PI * hz * i / rate));
    return x;
}

// Run a mono signal through the spatialiser, both ears fed the same, with a fixed target.
static void spatialise(const std::vector<float>& x, double rate, float target, std::vector<float>& L, std::vector<float>& R) {
    BinauralSpatialiser fx;
    L = x; R = x;
    const int blk = 480;
    for (size_t i = 0; i < x.size(); i += blk) {
        const int n = (int)std::min<size_t>(blk, x.size() - i);
        fx.process(&L[i], &R[i], n, rate, target);
    }
}

// Lag (samples) at which b best matches a, searched 0..maxLag (b later than a).
static int lagOf(const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to, int maxLag) {
    int best = 0; double bestV = -1e30;
    for (int k = 0; k <= maxLag; ++k) {
        double s = 0;
        for (size_t i = from; i < to; ++i) s += (double)a[i] * b[i + k];
        if (s > bestV) { bestV = s; best = k; }
    }
    return best;
}

/* The synthetic SSB voice from test-ssb-offset.cpp: harmonics of a pitch that glides (95-180 Hz), every one
 * moved by `offsetHz` (that IS the mistuning — +d = the audio d Hz HIGH), in a 250-2800 Hz passband, spoken
 * in syllables with a breath every 5 s, over band noise `snrDb` below the speech. */
static std::vector<float> voice(double rate, double secs, double offsetHz, double snrDb, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.0f, 1.0f);
    std::uniform_real_distribution<float> u(0.0f, 6.2831853f);
    const size_t n = (size_t)(rate * secs);
    std::vector<float> x(n);
    float th[40];
    for (auto& t : th) t = u(rng);
    double ph = 0;
    const double nAmp = 0.25 * std::pow(10.0, -snrDb / 20.0);
    for (size_t i = 0; i < n; ++i) {
        const double t = i / rate;
        const double f0 = 135 + 35 * std::sin(2 * M_PI * 0.23 * t) + 12 * std::sin(2 * M_PI * 1.7 * t + 1.0);
        ph += 2 * M_PI * f0 / rate;
        const double syl = std::max(0.0, std::sin(2 * M_PI * 3.1 * t));
        const double pause = std::fmod(t, 5.0) > 4.0 ? 0.0 : 1.0;
        double s = 0;
        for (int k = 1; k < 40; ++k) {
            const double fk = k * f0 + offsetHz;
            if (fk < 250 || fk > 2800) continue;
            s += std::cos(k * ph + 2 * M_PI * offsetHz * t + th[k]) / std::sqrt((double)k);
        }
        x[i] = (float)(0.08 * s * syl * pause + nAmp * g(rng));
    }
    return x;
}

// Feed mono audio through a controller in 20 ms packets, pumping the SSB job like the worker would.
static void feed(BinauralTuning& bt, const std::vector<float>& x, int rate, std::vector<float>* L = nullptr,
                 std::vector<float>* R = nullptr) {
    const int pkt = rate / 50;
    std::vector<float> l(pkt), r(pkt);
    if (L) { L->clear(); R->clear(); }
    for (size_t i = 0; i + pkt <= x.size(); i += pkt) {
        for (int k = 0; k < pkt; ++k) l[k] = r[k] = x[i + k];
        bt.processPlanar(l.data(), r.data(), pkt, rate);
        if (L) { L->insert(L->end(), l.begin(), l.end()); R->insert(R->end(), r.begin(), r.end()); }
        if ((i / pkt) % 2 == 1) bt.pumpSsbForTest();   // the worker wakes every 25 ms
    }
    bt.pumpSsbForTest();
}

static constexpr float kReckonTol = 60.0f;   // = BinauralTuning::kReckonTrustHz: refinement may move it this far

// A windowed-sinc band-pass on white noise: the receiver's noise as its filter shapes it (CW, §8).
static std::vector<float> bandNoise(double rate, double secs, double f1, double f2, double amp, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.0f, 1.0f);
    const int taps = 511;
    std::vector<double> hk(taps);
    for (int i = 0; i < taps; ++i) {
        const double m = i - (taps - 1) / 2.0;
        auto sinc = [&](double fc) { return m == 0 ? 2 * fc / rate : std::sin(2 * M_PI * fc / rate * m) / (M_PI * m); };
        const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / (taps - 1));
        hk[i] = (sinc(f2) - sinc(f1)) * w;
    }
    const size_t n = (size_t)(rate * secs);
    std::vector<float> wn(n + taps), x(n);
    for (auto& v : wn) v = g(rng);
    for (size_t i = 0; i < n; ++i) { double a = 0; for (int k = 0; k < taps; ++k) a += hk[k] * wn[i + k]; x[i] = (float)(amp * a); }
    return x;
}

// Through a controller (mono in, both ears), no SSB pumping; returns L and R.
static void run(BinauralTuning& bt, const std::vector<float>& x, int rate, std::vector<float>& L, std::vector<float>& R) {
    const int pkt = rate / 50;
    L.assign(x.size(), 0.0f); R.assign(x.size(), 0.0f);
    for (size_t i = 0; i + pkt <= x.size(); i += pkt) {
        std::copy(x.begin() + i, x.begin() + i + pkt, L.begin() + i);
        std::copy(x.begin() + i, x.begin() + i + pkt, R.begin() + i);
        bt.processPlanar(&L[i], &R[i], pkt, rate);
    }
}
static std::vector<float> mix(const std::vector<float>& a, const std::vector<float>& b) {
    std::vector<float> y(std::min(a.size(), b.size()));
    for (size_t i = 0; i < y.size(); ++i) y[i] = a[i] + b[i];
    return y;
}

int main() {
    std::printf("Binaural tuning — synthetic\n");
    const double fs = 48000;

    std::printf(" ── 1. the pan law ──\n");
    check(binauralPanFor(0, 15, 300) == 0 && binauralPanFor(14.9f, 15, 300) == 0 && binauralPanFor(-15, 15, 300) == 0,
          "inside ±15 Hz: dead centre");
    check(binauralPanFor(100, 15, 300) > 0 && binauralPanFor(-100, 15, 300) < 0,
          "signal ABOVE the dial leans RIGHT (+), below leans LEFT (−)");
    check(binauralPanFor(300, 15, 300) == 1 && binauralPanFor(-900, 15, 300) == -1, "full scale at ±300 Hz and beyond");
    {
        bool mono = true; float prev = 0;
        for (int h = 0; h <= 400; ++h) { const float p = binauralPanFor((float)h, 15, 300); if (p < prev) mono = false; prev = p; }
        check(mono, "monotonic from the dead zone to full scale");
        const float e = binauralPanFor(16, 15, 300);
        check(e > 0 && e < 0.001f, "eased out of the dead zone (16 Hz → a whisker, not a step)");
    }
    check(binauralPanFor(NAN, 15, 300) == 0 && binauralPanFor(INFINITY, 15, 300) == 0, "NaN / inf offset → centre");

    std::printf(" ── 2. the effect ──\n");
    {
        const auto x = tone(fs, 2.0, 500, 0.5);
        std::vector<float> L, R;
        spatialise(x, fs, 0.0f, L, R);
        double md = 0;
        for (size_t i = 0; i < x.size(); ++i) md = std::max(md, (double)std::max(std::fabs(L[i] - x[i]), std::fabs(R[i] - x[i])));
        std::printf("      on tune: max |out − in| = %.2g\n", md);
        check(md < 1e-5, "on tune both ears ARE the input (no delay, no shelf, no level)");

        spatialise(x, fs, 1.0f, L, R);
        const size_t a = (size_t)(1.0 * fs), b = x.size() - 200;
        const double near = db(rms(R, a, b), rms(x, a, b)), far = db(rms(L, a, b), rms(x, a, b));
        std::printf("      full right, 500 Hz: near ear %+.2f dB, far ear %+.2f dB\n", near, far);
        check(std::fabs(near) < 0.05, "the NEAR (right) ear is untouched");
        check(far < -9.0 && far > -13.0, "the FAR (left) ear is ~10 dB down (+ head shadow) — quieter, never silent");
        const int lag = lagOf(R, L, a, a + 4800, 60);
        std::printf("      far-ear delay: %d samples = %.3f ms\n", lag, lag / fs * 1000);
        // ★ ±3 samples: the head-shadow shelf adds a little phase of its own on top of the pure delay.
        check(std::abs(lag - (int)std::lround(BinauralSpatialiser::kMaxItdSec * fs)) <= 3, "the FAR ear is ~0.6 ms LATER");

        spatialise(x, fs, -1.0f, L, R);
        check(db(rms(L, a, b), rms(R, a, b)) > 9.0, "full LEFT mirrors it: the left ear is the near one");

        // Head shadow: the far ear loses more at 5 kHz than at 200 Hz.
        std::vector<float> L2, R2;
        const auto lo = tone(fs, 2.0, 200, 0.5), hi = tone(fs, 2.0, 5000, 0.5);
        spatialise(lo, fs, 1.0f, L, R);
        spatialise(hi, fs, 1.0f, L2, R2);
        const double dLo = db(rms(L, a, b), rms(lo, a, b)), dHi = db(rms(L2, a, b), rms(hi, a, b));
        std::printf("      far ear: 200 Hz %+.2f dB, 5 kHz %+.2f dB\n", dLo, dHi);
        check(dHi < dLo - 3.0, "head shadow: the far ear is DULLER (treble cut ≥ 3 dB more than the bass)");
        check(dHi > -20.0, "…and still audible at full lean (≥ −20 dB at 5 kHz)");

        // Half way off: a few dB, not the full 10.
        spatialise(x, fs, 0.3f, L, R);
        const double part = db(rms(L, a, b), rms(x, a, b));
        std::printf("      pan 0.3: far ear %+.2f dB\n", part);
        check(part < -2.0 && part > -6.0, "a small lean is a few dB, not the full drop");
    }
    {
        // ★ No click: the target jumps 0 → +1 → −1 → 0 on a low tone; the output's steepest step stays
        //   within the tone's own steepest step (plus a margin), so no discontinuity reached the ear.
        const auto x = tone(fs, 3.0, 200, 0.5);
        std::vector<float> L = x, R = x;
        BinauralSpatialiser fx;
        const int blk = 256;
        for (size_t i = 0; i < x.size(); i += blk) {
            const double t = i / fs;
            const float tgt = t < 0.5 ? 0.0f : t < 1.2 ? 1.0f : t < 2.0 ? -1.0f : 0.0f;
            fx.process(&L[i], &R[i], (int)std::min<size_t>(blk, x.size() - i), fs, tgt);
        }
        double mx = 0, mIn = 0;
        for (size_t i = 1; i < x.size(); ++i) {
            mIn = std::max(mIn, (double)std::fabs(x[i] - x[i - 1]));
            mx = std::max(mx, (double)std::max(std::fabs(L[i] - L[i - 1]), std::fabs(R[i] - R[i - 1])));
        }
        std::printf("      jumping target: steepest output step %.4f (the tone's own %.4f)\n", mx, mIn);
        check(mx < mIn * 1.05, "a jumping target makes NO click (no step steeper than the signal's own)");
    }
    {
        std::vector<float> L(4800, 0.0f), R(4800, 0.0f);
        BinauralSpatialiser fx;
        fx.process(L.data(), R.data(), 4800, fs, NAN);
        fx.process(L.data(), R.data(), 4800, fs, 1e9f);
        fx.process(L.data(), R.data(), 4800, 12000, -1.0f);   // a rate change mid-stream
        check(finite(L) && finite(R) && std::isfinite(fx.pan()), "NaN / huge targets and a rate change: no NaN out");
    }

    std::printf(" ── 3. the controller on a given offset (AM carrier / CW peak / FM centroid) ──\n");
    {
        const auto x = tone(fs, 1.5, 700, 0.4);
        auto run = [&](float hz, bool valid, double& lr) {
            BinauralTuning bt(false);
            bt.setEnabled(true);
            bt.setSource(BinauralTuning::Given, 15, 300);
            bt.setOffset(hz, valid);
            std::vector<float> L, R;
            feed(bt, x, (int)fs, &L, &R);
            lr = db(rms(L, L.size() / 2, L.size()), rms(R, R.size() / 2, R.size()));   // + = left louder
            return finite(L) && finite(R);
        };
        double lr;
        bool ok = run(+200, true, lr);
        std::printf("      +200 Hz: L−R %+.2f dB\n", lr);
        check(ok && lr < -3.0, "+200 Hz (signal above: tune UP) → the RIGHT ear leads");
        ok = run(-200, true, lr);
        std::printf("      −200 Hz: L−R %+.2f dB\n", lr);
        check(ok && lr > 3.0, "−200 Hz (signal below: tune DOWN) → the LEFT ear leads");
        ok = run(+10, true, lr);
        check(ok && std::fabs(lr) < 0.01, "+10 Hz is inside the dead zone → centre");
        ok = run(+200, false, lr);
        check(ok && std::fabs(lr) < 0.01, "an invalid offset → centre");
        // int16 path: mono in → stereo out, same lean.
        BinauralTuning bt(false);
        bt.setEnabled(true);
        bt.setSource(BinauralTuning::Given, 15, 300);
        bt.setOffset(-250, true);
        std::vector<int16_t> in(960), out(1920);
        double el = 0, er = 0;
        for (int p = 0; p < 100; ++p) {
            for (int i = 0; i < 960; ++i) in[i] = (int16_t)(12000 * std::sin(2 * M_PI * 700 * (p * 960 + i) / fs));
            const int f = bt.processInt16(in.data(), 1, 960, (int)fs, out.data());
            if (p >= 50) for (int i = 0; i < f; ++i) { el += (double)out[2 * i] * out[2 * i]; er += (double)out[2 * i + 1] * out[2 * i + 1]; }
        }
        check(el > er * 4, "int16 mono → stereo: −250 Hz leans LEFT (Android's path)");
    }

    std::printf(" ── 4. WFM: stereo lock fades the aid; a flickering pilot does not ──\n");
    {
        const auto x = tone(fs, 8.0, 700, 0.4);
        BinauralTuning bt(false);
        bt.setEnabled(true);
        bt.setSource(BinauralTuning::Given, 2000, 40000);
        bt.setOffset(30000, true);
        bt.setStereoHint(BinauralTuning::Mono);
        std::vector<float> L, R;
        feed(bt, std::vector<float>(x.begin(), x.begin() + (size_t)(2 * fs)), (int)fs, &L, &R);
        float st[9];
        bt.stats(st, 9);
        check(st[5] == 1.0f && st[4] > 0.5f, "mono: the aid is at full weight and leaning");
        // A pilot that flickers every 0.3 s — never holding 0.8 s — must leave the weight alone.
        float minW = 1;
        for (int k = 0; k < 10; ++k) {
            bt.setStereoHint(k % 2 ? BinauralTuning::Mono : BinauralTuning::Locked);
            feed(bt, std::vector<float>(x.begin(), x.begin() + (size_t)(0.3 * fs)), (int)fs);
            bt.stats(st, 9); minW = std::min(minW, st[5]);
        }
        std::printf("      flicker every 0.3 s for 3 s: lowest weight %.2f\n", minW);
        check(minW == 1.0f, "a FLICKERING pilot does not make it alternate (weight never moved)");
        // A real lock: held → after the hold + the fade the aid is gone, with no step on the way.
        bt.setStereoHint(BinauralTuning::Locked);
        std::vector<float> ws;
        for (int k = 0; k < 40; ++k) {
            feed(bt, std::vector<float>(x.begin(), x.begin() + (size_t)(0.1 * fs)), (int)fs);
            bt.stats(st, 9); ws.push_back(st[5]);
        }
        float maxStep = 0;
        for (size_t i = 1; i < ws.size(); ++i) maxStep = std::max(maxStep, std::fabs(ws[i] - ws[i - 1]));
        std::printf("      locked: weight after 4 s %.2f, largest 0.1 s step %.3f\n", ws.back(), maxStep);
        check(ws.back() == 0.0f, "held stereo: the aid fades out completely");
        check(ws[5] == 1.0f && maxStep < 0.1f, "…after the hold, and as a slow crossfade (no hard switch)");
        // Unknown hint: measure the audio. Distinct L/R (real stereo) → fades; identical (mono) → stays.
        BinauralTuning bu(false);
        bu.setEnabled(true);
        bu.setSource(BinauralTuning::Given, 2000, 40000);
        bu.setOffset(30000, true);
        bu.setStereoHint(BinauralTuning::Unknown);
        std::vector<float> l(960), r(960);
        for (int p = 0; p < 300; ++p) {
            for (int i = 0; i < 960; ++i) {
                const double t = (p * 960 + i) / fs;
                l[i] = (float)(0.3 * std::sin(2 * M_PI * 440 * t));
                r[i] = (float)(0.3 * std::sin(2 * M_PI * 660 * t));
            }
            bu.processPlanar(l.data(), r.data(), 960, (int)fs);
        }
        bu.stats(st, 9);
        check(st[5] == 0.0f, "unknown pilot, audio measured as STEREO (L≠R) → the aid fades out");
    }

    std::printf(" ── 5. SSB: the FAST estimator + running median, on a synthetic voice ──\n");
    {
        struct Case { double off; int src; float want; const char* what; };
        const Case cases[] = {
            { +100, BinauralTuning::SsbUsb, +100, "USB, audio 100 Hz HIGH → signal ABOVE the dial: +100 (lean right)" },
            { -120, BinauralTuning::SsbUsb, -120, "USB, audio 120 Hz LOW → −120 (lean left)" },
            { +100, BinauralTuning::SsbLsb, -100, "LSB, audio 100 Hz HIGH → signal BELOW the dial: −100 (lean left)" },
            { -80,  BinauralTuning::SsbLsb, +80,  "LSB, audio 80 Hz LOW → +80 (lean right)" },
        };
        for (const Case& c : cases) {
            BinauralTuning bt(false);
            bt.setEnabled(true);
            bt.setSource(c.src, 15, 300);
            bt.setDial(7100000);
            std::vector<float> L, R;
            feed(bt, voice(fs, 12, c.off, 25, 7), (int)fs, &L, &R);
            float hz = 0; const bool ok = bt.ssbOffset(hz);
            float st[9]; bt.stats(st, 9);
            std::printf("      %+5.0f Hz audio: published %s %+.1f Hz (%d estimates, %d accepted)\n",
                        c.off, ok ? "" : "(none)", hz, (int)st[6], (int)st[7]);
            const double lr = db(rms(L, L.size() - (size_t)fs, L.size()), rms(R, R.size() - (size_t)fs, R.size()));
            check(ok && std::fabs(hz - c.want) <= 20 && (c.want > 0 ? lr < -0.5 : lr > 0.5) && finite(L) && finite(R), c.what);
        }
        {
            // How long from the first word to the first lean, and a weaker signal (informational + a sign check).
            for (double snr : { 25.0, 12.0 }) {
                BinauralTuning bt(false);
                bt.setEnabled(true);
                bt.setSource(BinauralTuning::SsbUsb, 15, 300);
                const auto v = voice(fs, 12, 150, snr, 21);
                double first = -1;
                for (size_t i = 0; i + (size_t)(fs / 10) <= v.size(); i += (size_t)(fs / 10)) {
                    feed(bt, std::vector<float>(v.begin() + i, v.begin() + i + (size_t)(fs / 10)), (int)fs);
                    float hz; if (first < 0 && bt.ssbOffset(hz)) first = (i + fs / 10) / fs;
                }
                float hz = 0; const bool ok = bt.ssbOffset(hz);
                std::printf("      +150 Hz at %2.0f dB: first lean after %.1f s, now %s %+.1f Hz\n", snr, first, ok ? "" : "(none)", hz);
                if (snr > 20) check(ok && first > 0 && first < 6 && std::fabs(hz - 150) <= 20, "a strong voice leans within a few seconds of speech");
                else check(!ok || hz > 0, "a weaker voice: no answer, or the right SIGN — never the wrong ear");
            }
        }
        {
            BinauralTuning bt(false);
            bt.setEnabled(true);
            bt.setSource(BinauralTuning::SsbUsb, 15, 300);
            bt.setDial(3700000);
            std::vector<float> L, R;
            feed(bt, voice(fs, 12, 0, 25, 11), (int)fs, &L, &R);
            float hz = 0; const bool ok = bt.ssbOffset(hz);
            const double lr = db(rms(L, L.size() - (size_t)fs, L.size()), rms(R, R.size() - (size_t)fs, R.size()));
            std::printf("      on tune: published %s %+.1f Hz, L−R %+.2f dB\n", ok ? "" : "(none)", hz, lr);
            check(ok && std::fabs(hz) < 15 && std::fabs(lr) < 0.5, "an ON-TUNE voice sits centre-head (inside the dead zone)");
            // ±1 Hz of frequency-stamp jitter is not a move: the held answer stays exactly where it was.
            bt.setDial(3700001); bt.pumpSsbForTest(); bt.setDial(3699999); bt.pumpSsbForTest(); bt.setDial(3700000);
            float hzj = 0; bt.pumpSsbForTest(); bt.ssbOffset(hzj);
            check(hzj == hz, "±1 Hz of dial jitter does not move (or restart) the SSB answer");
            // ★ The dial moves +50 Hz: the held answer follows at once (signal − dial: 0 → −50 Hz, lean left).
            bt.setDial(3700050);
            bt.pumpSsbForTest();
            float hz2 = 0; const bool ok2 = bt.ssbOffset(hz2);
            std::printf("      dial +50 Hz: held answer now %+.1f Hz\n", hz2);
            check(ok2 && std::fabs(hz2 - (hz - 50)) < 0.01, "the dial moved +50 Hz → the held answer moved −50 Hz at once");
        }
        {
            // ★★★ STUART'S 500 Hz STEP (V12 474): lock on tune, step the dial +500 Hz — beyond the estimator's
            //     ±300 Hz — keep listening to the now-mistuned voice for 20 s, and the lean must HOLD, not centre.
            //     Then step back: centre again; step −500: lean the other way. ("it pans but then I wait just off
            //     and it centres again" / "actually I cant trigger it again")
            BinauralTuning bt(false);
            bt.setEnabled(true);
            bt.setSource(BinauralTuning::SsbUsb, 15, 300);
            bt.setDial(7100000);
            feed(bt, voice(fs, 10, 0, 25, 13), (int)fs);
            float h0 = 0; const bool l0 = bt.ssbOffset(h0);
            bt.setDial(7100500);                       // one step up: the signal is now 500 Hz BELOW the dial
            std::vector<float> L, R;
            feed(bt, voice(fs, 20, -500, 25, 14), (int)fs, &L, &R);   // the voice as heard 500 Hz off
            float h1 = 0; const bool l1 = bt.ssbOffset(h1);
            const double lr = db(rms(L, L.size() - (size_t)fs, L.size()), rms(R, R.size() - (size_t)fs, R.size()));
            std::printf("      500 Hz step: locked %s %+.1f → after 20 s off-tune %s %+.1f Hz, L−R %+.2f dB\n",
                        l0 ? "" : "(none)", h0, l1 ? "" : "(none)", h1, lr);
            check(l0 && l1 && std::fabs(h1 - (h0 - 500)) < 1.0 && lr > 0.5,
                  "a 500 Hz step off a locked voice HOLDS its lean (left) through 20 s of mistuned speech");
            bt.setDial(7100000);
            feed(bt, voice(fs, 4, 0, 25, 15), (int)fs);
            float h2 = 0; bt.ssbOffset(h2);
            bt.setDial(7099500);                       // one step down: signal 500 Hz ABOVE → lean right
            feed(bt, voice(fs, 6, 500, 25, 16), (int)fs, &L, &R);
            float h3 = 0; const bool l3 = bt.ssbOffset(h3);
            const double lr3 = db(rms(L, L.size() - (size_t)fs, L.size()), rms(R, R.size() - (size_t)fs, R.size()));
            std::printf("      back on tune %+.1f Hz; one step down → %s %+.1f Hz, L−R %+.2f dB\n", h2, l3 ? "" : "(none)", h3, lr3);
            check(std::fabs(h2) < 15 && l3 && h3 > 300 && lr3 < -0.5, "back on tune centres; a step the other way leans the other ear — it triggers again");
        }
        {
            // ★★ 100 Hz STEPS — inside the estimator's reach, where FAST's pull toward small offsets used to win.
            BinauralTuning bt(false);
            bt.setEnabled(true);
            bt.setSource(BinauralTuning::SsbUsb, 15, 300);
            bt.setDial(7100000);
            feed(bt, voice(fs, 10, 0, 25, 17), (int)fs);
            float h0 = 0; bt.ssbOffset(h0);
            bt.setDial(7100100);
            std::vector<float> L, R;
            feed(bt, voice(fs, 20, -100, 25, 18), (int)fs, &L, &R);
            float h1 = 0; const bool l1 = bt.ssbOffset(h1);
            const double lr = db(rms(L, L.size() - (size_t)fs, L.size()), rms(R, R.size() - (size_t)fs, R.size()));
            std::printf("      100 Hz step: %+.1f → after 20 s off-tune %s %+.1f Hz, L−R %+.2f dB\n", h0, l1 ? "" : "(none)", h1, lr);
            check(l1 && std::fabs(h1 - (h0 - 100)) <= kReckonTol && lr > 0.5, "a 100 Hz step HOLDS its lean through 20 s of mistuned speech");
        }
        {
            BinauralTuning bt(false);
            bt.setEnabled(true);
            bt.setSource(BinauralTuning::SsbUsb, 15, 300);
            std::mt19937 rng(5);
            std::normal_distribution<float> g(0.0f, 0.05f);
            std::vector<float> n((size_t)(fs * 15));
            for (auto& v : n) v = g(rng);
            std::vector<float> L, R;
            feed(bt, n, (int)fs, &L, &R);
            float hz = 0; const bool ok = bt.ssbOffset(hz);
            float st[9]; bt.stats(st, 9);
            std::printf("      noise only: %d estimates, %d accepted, published %s\n", (int)st[6], (int)st[7], ok ? "YES" : "nothing");
            const double lr = db(rms(L, L.size() / 2, L.size()), rms(R, R.size() / 2, R.size()));
            check(!ok && std::fabs(lr) < 0.01, "noise is never believed: no lean on an empty channel");
        }
    }

    std::printf(" ── 6. cost ──\n");
    {
        const auto x = voice(fs, 20, 60, 25, 3);
        BinauralTuning bt(false);
        bt.setEnabled(true);
        bt.setSource(BinauralTuning::Given, 15, 300);
        bt.setOffset(150, true);
        std::vector<float> l(960), r(960);
        auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i + 960 <= x.size(); i += 960) {
            std::copy(x.begin() + i, x.begin() + i + 960, l.begin()); std::copy(l.begin(), l.end(), r.begin());
            bt.processPlanar(l.data(), r.data(), 960, (int)fs);
        }
        const double effect = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 20.0;
        BinauralTuning bs(false);
        bs.setEnabled(true);
        bs.setSource(BinauralTuning::SsbUsb, 15, 300);
        t0 = std::chrono::steady_clock::now();
        feed(bs, x, (int)fs);
        const double ssb = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 20.0;
        std::printf("      effect (audio thread): %.3f %% of one core; + SSB estimator (worker): %.3f %%\n",
                    effect * 100, ssb * 100);
        check(effect < 0.01, "the audio-thread effect costs < 1 % of a core at 48 kHz stereo");
    }

    std::printf(" ── 7. the coarse SSB cue (the waterfall) ──\n");
    {
        const double dial = 7100000;
        {
            // ★ ARRIVING 500 Hz BELOW A VOICE never locked: the estimator cannot reach it; the cue can.
            BinauralTuning bt(false);
            bt.setEnabled(true);
            bt.setSource(BinauralTuning::SsbUsb, 15, 300);
            bt.setDial(dial);
            bt.setSsbHint(dial + 500, true);
            std::vector<float> L, R;
            feed(bt, voice(fs, 4, 500, 25, 31), (int)fs, &L, &R);
            float h = 0; const bool v = bt.ssbOffset(h);
            float st[12]; bt.stats(st, 12);
            const double lr = db(rms(L, L.size() - (size_t)fs, L.size()), rms(R, R.size() - (size_t)fs, R.size()));
            std::printf("      no fine answer, cue +500 → %s %+.1f Hz (coarse %d), L−R %+.2f dB\n", v ? "" : "(none)", h, (int)st[10], lr);
            check(v && std::fabs(h - 500) < 1 && st[10] == 1 && lr < -3, "no fine answer: the cue drives the lean (right, tune up)");
            // The dial moves 200 Hz up: the cue is ABSOLUTE, so the answer is now 300 Hz, at once.
            bt.setDial(dial + 200);
            bt.pumpSsbForTest();
            bt.ssbOffset(h);
            std::printf("      dial +200 → %+.1f Hz\n", h);
            check(std::fabs(h - 300) < 1, "the dial moves a coarse-held answer like any other");
            bt.setSsbHint(0, false);
            bt.pumpSsbForTest();
            check(!bt.ssbOffset(h), "the cue going invalid lets a coarse-held answer go");
        }
        {
            // ★ A VALID IN-REACH FINE ANSWER IS NEVER FOUGHT.
            BinauralTuning bt(false);
            bt.setEnabled(true);
            bt.setSource(BinauralTuning::SsbUsb, 15, 300);
            bt.setDial(dial);
            feed(bt, voice(fs, 10, 0, 25, 32), (int)fs);
            float h0 = 0; const bool v0 = bt.ssbOffset(h0);
            bt.setSsbHint(dial + 800, true);           // a wrong cue (another voice, a misread edge)
            std::vector<float> L, R;
            feed(bt, voice(fs, 4, 0, 25, 33), (int)fs, &L, &R);
            float h1 = 0; const bool v1 = bt.ssbOffset(h1);
            float st[12]; bt.stats(st, 12);
            const double lr = db(rms(L, L.size() - (size_t)fs, L.size()), rms(R, R.size() - (size_t)fs, R.size()));
            std::printf("      locked %s%+.1f Hz; cue +800 → %+.1f Hz (coarse %d), L−R %+.2f dB\n", v0 ? "" : "(none) ", h0, h1, (int)st[10], lr);
            check(v0 && v1 && std::fabs(h1) < 15 && st[10] == 0 && std::fabs(lr) < 0.1, "a valid in-reach fine answer ignores the cue: still centred");
        }
        {
            // ★ BEYOND REACH: the knob's exact figure stays when the cue agrees; a disagreeing cue replaces it.
            BinauralTuning bt(false);
            bt.setEnabled(true);
            bt.setSource(BinauralTuning::SsbUsb, 15, 300);
            bt.setDial(dial);
            feed(bt, voice(fs, 10, 0, 25, 34), (int)fs);
            float h0 = 0; bt.ssbOffset(h0);
            bt.setDial(dial - 500);                    // a 500 Hz step down: reckoned +500
            bt.setSsbHint(dial + 80, true);            // the cue reads +580: the same voice, within its error
            bt.pumpSsbForTest();
            float h1 = 0; bt.ssbOffset(h1);
            check(std::fabs(h1 - (h0 + 500)) < 1, "beyond reach, an agreeing cue leaves the reckoned figure exact");
            bt.setSsbHint(dial - 500 + 1500, true);     // now a voice 1.5 kHz up fills the passband
            bt.pumpSsbForTest();
            float h2 = 0; bt.ssbOffset(h2);
            float st[12]; bt.stats(st, 12);
            std::printf("      reckoned %+.1f → agreeing cue keeps %+.1f; disagreeing cue → %+.1f (coarse %d)\n", h0 + 500, h1, h2, (int)st[10]);
            check(std::fabs(h2 - 1500) < 1 && st[10] == 1, "beyond reach, a disagreeing cue replaces the reckoned figure");
        }
        {
            // ★ HANDED BACK: a coarse-held answer inside the estimator's reach is replaced by the estimator.
            BinauralTuning bt(false);
            bt.setEnabled(true);
            bt.setSource(BinauralTuning::SsbUsb, 15, 300);
            bt.setDial(dial);
            bt.setSsbHint(dial + 260, true);           // the cue says 260; the voice is really at +120
            feed(bt, voice(fs, 20, 120, 25, 35), (int)fs);
            float h = 0; const bool v = bt.ssbOffset(h);
            float st[12]; bt.stats(st, 12);
            std::printf("      cue +260, voice +120 → %s%+.1f Hz (coarse %d, %d accepted)\n", v ? "" : "(none) ", h, (int)st[10], (int)st[7]);
            check(v && st[10] == 0 && std::fabs(h - 120) < 60, "the estimator replaces a coarse answer within its reach");
        }
        {
            BinauralTuning bt(false);
            bt.setEnabled(true);
            bt.setSource(BinauralTuning::SsbUsb, 15, 300);
            bt.setDial(dial);
            bt.setSsbHint(dial + 90, true);            // within the cue's own error
            bt.pumpSsbForTest();
            float h = 0;
            check(!bt.ssbOffset(h), "a cue under ±150 Hz (its own error) never leans on its own");
            bt.setSsbHint(dial - 700, true);
            bt.pumpSsbForTest();
            const bool v = bt.ssbOffset(h);
            bt.setSource(BinauralTuning::SsbLsb, 15, 300);
            bt.pumpSsbForTest();
            float h2 = 0; const bool v2 = bt.ssbOffset(h2);
            check(v && std::fabs(h + 700) < 1 && !v2, "a cue below the dial reads −; a change of sideband forgets it");
        }
    }

    std::printf(" ── 8. the SPREAD map, at full amount ──\n");
    const float lo = 50, hi = 2700;   // = MODE_BANDWIDTHS.usb (sdrProtocol.ts)
    {
        check(BinauralSpread::panAt(50, BinauralSpread::Usb, 1375, 1325) == -1 && BinauralSpread::panAt(2700, BinauralSpread::Usb, 1375, 1325) == 1
              && BinauralSpread::panAt(1375, BinauralSpread::Usb, 1375, 1325) == 0, "the scale: 50 Hz full left, 2700 Hz full right, 1375 Hz centre");
        check(BinauralSpread::panAt(4000, BinauralSpread::Usb, 1375, 1325) == 1 && BinauralSpread::panAt(10, BinauralSpread::Usb, 1375, 1325) == -1,
              "beyond the default edges: clamped at full");
        check(BinauralSpread::panAt(500, BinauralSpread::Lsb, 1375, 1325) == -BinauralSpread::panAt(500, BinauralSpread::Usb, 1375, 1325),
              "LSB is the mirror (higher audio = lower RF)");
        /* ★ CHANGED with the combined behaviour: these ran through the CONTROLLER under the old SPREAD style, which
         *   spread at full whatever the tuning. Now the controller spreads only as far as you are off tune, so the
         *   MAP is measured on the bank alone at amount 1 — what the controller gives at full off-tune weight, minus
         *   the lean (§9 measures the two together). */
        auto lrOf = [&](const std::vector<float>& x, int kind, std::vector<float>* Lo = nullptr, std::vector<float>* Ro = nullptr) {
            BinauralSpread sp;
            std::vector<float> L = x, R = x;
            for (size_t i = 0; i + 960 <= x.size(); i += 960) sp.process(&L[i], &R[i], 960, fs, kind, 1.0f, lo, hi);
            if (Lo) { *Lo = L; *Ro = R; }
            const size_t a = (size_t)(1.0 * fs), b = x.size() - (size_t)(0.1 * fs);
            return db(rms(L, a, b), rms(R, a, b));
        };
        const auto t500 = tone(fs, 2.0, 500, 0.3), t2200 = tone(fs, 2.0, 2200, 0.3);
        const double u5 = lrOf(t500, BinauralSpread::Usb), u22 = lrOf(t2200, BinauralSpread::Usb);
        const double l5 = lrOf(t500, BinauralSpread::Lsb), l22 = lrOf(t2200, BinauralSpread::Lsb);
        std::printf("      USB: 500 Hz L−R %+.2f dB, 2200 Hz %+.2f dB · LSB: 500 Hz %+.2f dB, 2200 Hz %+.2f dB\n", u5, u22, l5, l22);
        check(u5 > 3 && u22 < -3, "USB: a 500 Hz signal sits LEFT, a 2200 Hz one RIGHT (higher RF = right)");
        check(l5 < -3 && l22 > 3, "LSB: mirrored — 500 Hz audio (the HIGHER RF) sits right, 2200 Hz left");
        {
            std::vector<float> L, R;
            lrOf(mix(t500, t2200), BinauralSpread::Usb, &L, &R);
            auto bandRms = [&](const std::vector<float>& y, double f) {   // a single-bin DFT over the steady half second
                double re = 0, im = 0; const size_t a = (size_t)fs, b = a + (size_t)fs / 2;
                for (size_t i = a; i < b; ++i) { re += y[i] * std::cos(2 * M_PI * f * i / fs); im += y[i] * std::sin(2 * M_PI * f * i / fs); }
                return std::sqrt(re * re + im * im);
            };
            const double s5 = db(bandRms(L, 500), bandRms(R, 500)), s22 = db(bandRms(L, 2200), bandRms(R, 2200));
            std::printf("      together: 500 Hz L−R %+.2f dB, 2200 Hz %+.2f dB\n", s5, s22);
            check(s5 > 3 && s22 < -3, "two signals at once: one in each ear");
        }
        const double c1 = lrOf(tone(fs, 2.0, 3000, 0.3), BinauralSpread::Usb), c2 = lrOf(tone(fs, 2.0, 3600, 0.3), BinauralSpread::Usb);
        std::printf("      past the edge: 3000 Hz %+.2f dB, 3600 Hz %+.2f dB\n", c1, c2);
        check(std::fabs(c1 - c2) < 0.6, "past the default edge the placement is clamped (the same lean)");
        {
            std::vector<float> L, R;
            const auto v = voice(fs, 8, 0, 25, 41);
            const double lr = lrOf(v, BinauralSpread::Usb, &L, &R);
            const double lrL = lrOf(v, BinauralSpread::Lsb);
            std::printf("      one voice through the full map: L−R %+.2f dB (USB), %+.2f dB (LSB)\n", lr, lrL);
            check(std::fabs(lr) < 1.5 && std::fabs(lrL) < 1.5 && finite(L) && finite(R), "the pan law keeps one voice's weight central (|L−R| < 1.5 dB)");
        }
        {
            std::vector<float> L, R;
            const auto x = tone(fs, 2.0, 2700, 0.3);
            lrOf(x, BinauralSpread::Usb, &L, &R);
            const double far = db(rms(L, (size_t)fs, x.size() - 4800), rms(x, (size_t)fs, x.size() - 4800));
            std::printf("      far ear at the edge: %+.2f dB re the input\n", far);
            check(far > -17.5 && far < -8, "never silent: the far ear ≈ 10 dB down (+ the head shadow), not off");
        }
        {
            // ★ CW: the note is measured from the noise. A Kiwi-style 300-700 Hz passband (note 500) with a CW signal
            //   ON the note → centred; one 200 Hz higher → a little to the right.
            auto cw = [&](double toneHz, float& centre) {
                BinauralSpread sp;
                const auto nz = bandNoise(fs, 6.0, 300, 700, 0.05, 7);
                auto t = tone(fs, 6.0, toneHz, 0.2);
                for (size_t i = 0; i < t.size(); ++i) if (std::fmod(i / fs, 0.24) > 0.12) t[i] = 0;   // keyed dits
                std::vector<float> L = mix(nz, t), R = L;
                for (size_t i = 0; i + 960 <= L.size(); i += 960) sp.process(&L[i], &R[i], 960, fs, BinauralSpread::CwUpper, 1.0f, lo, hi);
                centre = sp.cwCentreHz();
                const size_t a = (size_t)(3 * fs), b = L.size() - 4800;
                return db(rms(L, a, b), rms(R, a, b));
            };
            float c0 = 0, c1b = 0;
            const double on = cw(500, c0), up = cw(700, c1b);
            std::printf("      CW 300-700 Hz passband: measured note %.0f Hz; on-note signal L−R %+.2f dB, 200 Hz up %+.2f dB\n", c0, on, up);
            check(std::fabs(c0 - 500) < 70 && std::fabs(on) < 1.0 && up < on - 0.2, "CW: the note found from the noise, the note centred in the map, a higher one to the right");
        }
        {
            BinauralSpread sp;
            std::vector<float> l(480, 0.0f), r(480, 0.0f);
            sp.process(l.data(), r.data(), 480, fs, BinauralSpread::Usb, 0.0f, lo, hi);
            std::printf("      latency while the bank runs (SSB/CW, on tune or off): %d samples = %.1f ms at 48 kHz (AM/FM: 0)\n",
                        sp.latencySamples(), 1000.0 * sp.latencySamples() / fs);
            check(sp.latencySamples() <= 512, "latency one frame (≤ 512 samples at 48 kHz)");
        }
    }

    std::printf(" ── 9. one behaviour: untouched on tune, lean + spread together off tune ──\n");
    {
        auto toneAt = [&](const std::vector<float>& y, double f, size_t a, size_t b) {   // single-bin DFT magnitude
            double re = 0, im = 0;
            for (size_t i = a; i < b; ++i) { re += y[i] * std::cos(2 * M_PI * f * i / fs); im += y[i] * std::sin(2 * M_PI * f * i / fs); }
            return std::sqrt(re * re + im * im);
        };
        const int D = 512;   // the bank's frame at 48 kHz — the constant latency on SSB/CW
        // (a) ON TUNE = UNTOUCHED: an SSB voice right on tune, locked by the estimator, with the spread map set.
        for (int kind : { (int)BinauralSpread::Usb, (int)BinauralSpread::Lsb }) {
            BinauralTuning bt(false);
            bt.setEnabled(true);
            bt.setSource(kind == BinauralSpread::Lsb ? BinauralTuning::SsbLsb : BinauralTuning::SsbUsb, 15, 300);
            bt.setDial(7100000);
            bt.setSpread(kind, lo, hi);
            const auto x = voice(fs, 12, 0, 25, 7);
            std::vector<float> L, R;
            feed(bt, x, (int)fs, &L, &R);
            float hz = 0; const bool ok = bt.ssbOffset(hz);
            float st[12]; bt.stats(st, 12);
            double worst = 0;
            for (size_t i = (size_t)(0.5 * fs); i < L.size(); ++i)
                worst = std::max({worst, (double)std::fabs(L[i] - x[i - D]), (double)std::fabs(R[i] - x[i - D])});
            std::printf("      %s on-tune voice: locked %s %+.1f Hz, max |out − in delayed %d| = %.2e (pan %.3f, spread %.3f)\n",
                        kind == BinauralSpread::Usb ? "USB" : "LSB", ok ? "at" : "(not)", hz, D, worst, st[4], st[9]);
            check(ok && std::fabs(hz) < 15 && worst <= 1e-6 && st[4] == 0 && st[9] == 0,
                  kind == BinauralSpread::Usb ? "USB on tune: the output IS the input (one constant frame later), no lean, no spread"
                                              : "LSB on tune: the output IS the input (one constant frame later), no lean, no spread");
        }
        // The same on AM (no spread map): untouched with NO delay at all.
        {
            BinauralTuning bt(false);
            bt.setEnabled(true);
            bt.setSource(BinauralTuning::Given, 15, 300);
            bt.setOffset(5, true);
            bt.setSpread(BinauralSpread::Off, lo, hi);
            const auto x = tone(fs, 2.0, 700, 0.3);
            std::vector<float> L, R;
            run(bt, x, (int)fs, L, R);
            double worst = 0;
            for (size_t i = 0; i < x.size() - 960; ++i) worst = std::max({worst, (double)std::fabs(L[i] - x[i]), (double)std::fabs(R[i] - x[i])});
            std::printf("      AM on tune (5 Hz, inside the dead zone): max |out − in| = %.2e, no delay\n", worst);
            check(worst <= 1e-6, "AM on tune: the output IS the input, not even delayed");
        }
        // (b)/(c) OFF TUNE: three tones — 500 Hz, 1375 Hz (the map's centre: the lean alone), 2200 Hz.
        auto offTune = [&](int kind, float offHz, double& d5, double& d13, double& d22, float& pan, float& spr) {
            BinauralTuning bt(false);
            bt.setEnabled(true);
            bt.setSource(BinauralTuning::Given, 15, 300);   // the controller takes w from whichever source: Given is exact
            bt.setOffset(offHz, true);
            bt.setSpread(kind, lo, hi);
            const auto x = mix(mix(tone(fs, 3.0, 500, 0.2), tone(fs, 3.0, 1375, 0.2)), tone(fs, 3.0, 2200, 0.2));
            std::vector<float> L, R;
            run(bt, x, (int)fs, L, R);
            const size_t a = (size_t)(2.0 * fs), b = a + (size_t)(0.5 * fs);
            d5 = db(toneAt(L, 500, a, b), toneAt(R, 500, a, b));
            d13 = db(toneAt(L, 1375, a, b), toneAt(R, 1375, a, b));
            d22 = db(toneAt(L, 2200, a, b), toneAt(R, 2200, a, b));
            float st[12]; bt.stats(st, 12); pan = st[4]; spr = st[9];
        };
        {
            double d5, d13, d22; float pan, spr;
            offTune(BinauralSpread::Usb, 500, d5, d13, d22, pan, spr);
            std::printf("      USB +500 Hz: pan %.2f spread %.2f · L−R 500 Hz %+.2f, 1375 Hz %+.2f, 2200 Hz %+.2f dB\n", pan, spr, d5, d13, d22);
            /* ★ RELATIVE, NOT ABSOLUTE, on purpose. The lean (full, 10 dB) moves the WHOLE picture to the right — it
             *   says "tune up" — and the spread then places what is in the passband around it, lower RF left of
             *   higher. So at full weight a 500 Hz tone sits LEFT OF the 2200 Hz one but may still be right of
             *   centre; "louder left" is measured against the map's centre tone (1375 Hz), which only the lean moves. */
            check(pan > 0.99 && spr > 0.99 && d13 < -3, "USB +500 Hz: the lean is full and to the RIGHT (tune up), the spread is full");
            check(d5 - d13 > 3 && d13 - d22 > 3, "USB +500 Hz: the spread is there — 500 Hz to the left of the centre, 2200 Hz to its right");
        }
        {
            double d5, d13, d22; float pan, spr;
            offTune(BinauralSpread::Lsb, 500, d5, d13, d22, pan, spr);
            std::printf("      LSB +500 Hz: pan %.2f spread %.2f · L−R 500 Hz %+.2f, 1375 Hz %+.2f, 2200 Hz %+.2f dB\n", pan, spr, d5, d13, d22);
            check(pan > 0.99 && spr > 0.99 && d13 < -3 && d22 - d13 > 3 && d13 - d5 > 3,
                  "LSB +500 Hz: the same lean right, the spread mirrored (500 Hz audio = the higher RF, to the right)");
        }
        // (d) THE WEIGHT: as the offset grows, lean and spread grow together, monotonically, from nothing.
        {
            BinauralTuning bt(false);
            bt.setEnabled(true);
            bt.setSource(BinauralTuning::Given, 15, 300);
            bt.setSpread(BinauralSpread::Usb, lo, hi);
            const auto x = tone(fs, 1.2, 900, 0.2);
            const float offs[] = { 0, 10, 15, 30, 60, 100, 150, 200, 250, 300, 400 };
            float prevP = -1, prevS = -1, worstGap = 0; bool mono = true, zero = true;
            std::printf("      offset → pan / spread:");
            for (float o : offs) {
                bt.setOffset(o, true);
                std::vector<float> L, R;
                run(bt, x, (int)fs, L, R);
                float st[12]; bt.stats(st, 12);
                std::printf(" %.0f→%.2f/%.2f", o, st[4], st[9]);
                if (st[4] < prevP - 1e-4f || st[9] < prevS - 1e-4f) mono = false;
                if (o <= 15 && (st[4] != 0 || st[9] != 0)) zero = false;
                worstGap = std::max(worstGap, std::fabs(st[4] - st[9]));
                prevP = st[4]; prevS = st[9];
            }
            std::printf("\n      largest |pan − spread| %.3f\n", worstGap);
            check(zero && mono && worstGap < 0.02 && prevP > 0.99 && prevS > 0.99,
                  "w: nothing in the dead zone, then lean and spread grow together (monotonic, within 0.02) to full");
        }
        // (e) AM / FM: no spread map → never any spread, the lean as before.
        {
            BinauralTuning bt(false);
            bt.setEnabled(true);
            bt.setSource(BinauralTuning::Given, 15, 300);
            bt.setOffset(500, true);
            bt.setSpread(BinauralSpread::Off, lo, hi);
            float maxS = 0;
            const auto x = tone(fs, 2.0, 900, 0.2);
            std::vector<float> l(960), r(960);
            for (size_t i = 0; i + 960 <= x.size(); i += 960) {
                std::copy(x.begin() + i, x.begin() + i + 960, l.begin()); std::copy(l.begin(), l.end(), r.begin());
                bt.processPlanar(l.data(), r.data(), 960, (int)fs);
                float st[12]; bt.stats(st, 12); maxS = std::max(maxS, st[9]);
            }
            float st[12]; bt.stats(st, 12);
            std::printf("      AM/FM +500 Hz: pan %.2f, spread never above %.2f\n", st[4], maxS);
            check(maxS == 0 && st[4] > 0.99, "AM/FM (no map): the lean alone — no spread, ever");
        }
        // (f) THE SSB ESTIMATOR IS STILL FED with the spread map set — it is what drives w on SSB.
        for (int kind : { (int)BinauralSpread::Usb, (int)BinauralSpread::Lsb }) {
            BinauralTuning bt(false);
            bt.setEnabled(true);
            bt.setSource(kind == BinauralSpread::Lsb ? BinauralTuning::SsbLsb : BinauralTuning::SsbUsb, 15, 300);
            bt.setDial(7100000);
            bt.setSpread(kind, lo, hi);
            feed(bt, voice(fs, 12, 150, 25, 7), (int)fs);
            float hz = 0; const bool ok = bt.ssbOffset(hz);
            float st[12]; bt.stats(st, 12);
            const float want = kind == BinauralSpread::Usb ? 150.0f : -150.0f;
            std::printf("      %s voice 150 Hz high, spread map set: %d estimates, published %+.1f Hz → pan %+.2f, spread %.2f\n",
                        kind == BinauralSpread::Usb ? "USB" : "LSB", (int)st[6], hz, st[4], st[9]);
            check(st[6] > 0 && ok && std::fabs(hz - want) <= 20 && st[4] * want > 0 && st[9] > 0.3,
                  kind == BinauralSpread::Usb ? "USB: the estimator is fed and publishes with the map set; its answer drives lean AND spread"
                                              : "LSB: the estimator is fed and publishes with the map set (sign per sideband); lean AND spread follow");
        }
        {
            // …and the waterfall's coarse cue, beyond the estimator's reach.
            BinauralTuning bt(false);
            bt.setEnabled(true);
            bt.setSource(BinauralTuning::SsbUsb, 15, 300);
            bt.setDial(7100000);
            bt.setSpread(BinauralSpread::Usb, lo, hi);
            bt.setSsbHint(7100000 + 800, true);
            feed(bt, bandNoise(fs, 1.5, 300, 2700, 0.02, 3), (int)fs);
            float hz = 0; const bool ok = bt.ssbOffset(hz);
            float st[12]; bt.stats(st, 12);
            std::printf("      coarse cue 800 Hz up, map set: held %+.0f Hz → pan %.2f, spread %.2f\n", hz, st[4], st[9]);
            check(ok && std::fabs(hz - 800) < 1 && st[4] > 0.99 && st[9] > 0.99, "the coarse cue drives lean and spread too");
        }
        // (g) NO CLICK, NO NaN: on tune → off → a sideband change mid-lean → the lean flips → on tune → to AM while
        //     leaning → back to USB → switched OFF mid-lean (called only while active(), as iOS does) → back on.
        {
            BinauralTuning bt(false);
            bt.setEnabled(true);
            bt.setSource(BinauralTuning::Given, 15, 300);
            const auto x = tone(fs, 10.0, 500, 0.3);
            const double maxIn = 2 * M_PI * 500 / fs * 0.3;
            std::vector<float> L(x.size()), R(x.size());
            const int pkt = 480;
            float maxSpread = 0; bool glided = false;
            for (size_t i = 0; i + pkt <= x.size(); i += pkt) {
                const double t = i / fs;
                bt.setOffset(t < 0.5 ? 0 : t < 3 ? 400 : t < 4 ? -400 : t < 5 ? 0 : t < 7.3 ? 200 : 300, true);
                bt.setSpread(t < 2 ? BinauralSpread::Usb : t < 5 ? BinauralSpread::Lsb : t < 6 ? BinauralSpread::Off : BinauralSpread::Usb, lo, hi);
                if (std::fabs(t - 7.0) < 1e-6) bt.setEnabled(false);
                if (std::fabs(t - 8.5) < 1e-6) bt.setEnabled(true);
                std::copy(x.begin() + i, x.begin() + i + pkt, L.begin() + i);
                std::copy(x.begin() + i, x.begin() + i + pkt, R.begin() + i);
                if (bt.active()) { bt.processPlanar(&L[i], &R[i], pkt, (int)fs); if (!bt.enabled()) glided = true; }
                float st[12]; bt.stats(st, 12); maxSpread = std::max(maxSpread, st[9]);
            }
            double worst = 0;
            for (size_t i = 1; i < L.size(); ++i) worst = std::max({worst, (double)std::fabs(L[i] - L[i - 1]), (double)std::fabs(R[i] - R[i - 1])});
            std::printf("      toggles, w changes, sideband change: worst step %.4f (the sine's own %.4f), reached spread %.2f\n", worst, maxIn, maxSpread);
            check(finite(L) && finite(R) && worst < 1.6 * maxIn && maxSpread > 0.99 && glided,
                  "no click and no NaN — w moving, a sideband change, to AM and back, switched off (a glide) and on");
        }
        // (h) Cost, audio thread, through the whole controller.
        for (float off : { 0.0f, 500.0f }) {
            const auto x = voice(fs, 20, 0, 25, 42);
            BinauralTuning bt(false);
            bt.setEnabled(true);
            bt.setSource(BinauralTuning::Given, 15, 300);
            bt.setOffset(off, true);
            bt.setSpread(BinauralSpread::Usb, lo, hi);
            std::vector<float> l(960), r(960);
            auto t0 = std::chrono::steady_clock::now();
            for (size_t i = 0; i + 960 <= x.size(); i += 960) {
                std::copy(x.begin() + i, x.begin() + i + 960, l.begin()); std::copy(l.begin(), l.end(), r.begin());
                bt.processPlanar(l.data(), r.data(), 960, (int)fs);
            }
            const double c = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 20.0;
            std::printf("      SSB %s (audio thread): %.3f %% of one core at 48 kHz\n", off == 0 ? "on tune (no FFT)" : "off tune, lean + spread", c * 100);
            check(c < 0.02, off == 0 ? "on tune costs < 2 % of a core at 48 kHz (this Mac)" : "off tune costs < 2 % of a core at 48 kHz (this Mac)");
        }
    }

    std::printf(fails ? "\nFAILED: %d\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
