// ★★★ SSB AUTO-TUNE FOR THE RECOGNISER — the arithmetic, on signals whose truth is known (2026-10-16).
//
// vibedsp/ssb_offset.h: the offset estimator (pitch-harmonic alignment), the single-sideband shifter that
// corrects it, SsbAutoTune's decision rule and RecogniserFeed (any rate → 16 kHz, anti-aliased). The
// real-recording proof is test-ssb-offset-clips (needs the research clips); this one always runs:
//
//   1. THE SHIFT IS A SHIFT. A tone moved ±60 Hz lands where it should and leaves no mirror image
//      (≥ 40 dB down from 300 Hz to 3.4 kHz) — a plain mix would put half its power on the wrong side.
//   2. THE ESTIMATOR FINDS A KNOWN OFFSET in a synthetic voice (harmonics of a gliding pitch, syllables,
//      pauses, band noise, an SSB passband) from −180 to +150 Hz, within 3 Hz, and believes it. Pure
//      noise is never believed.
//   3. THE DECISION: two agreeing SLOW windows → the audio is shifted back; a retune/mode change resets
//      it; non-SSB leaves the estimator idle. The C API's dial correction has the right SIGN per sideband.
//   4. THE FEED IS ANTI-ALIASED: a 12 kHz tone at 48 kHz (which the old linear interpolator folded to
//      4 kHz, inside the speech band) comes out ≥ 60 dB down; 1 kHz passes; 48 000 in → 16 000 out.
//   5. FAST (the binaural aid's profile) answers every 1-2 s of speech, roughly right; and both profiles'
//      cost is reported — SLOW must be well under 1 % of a core.
// ★ SILENT: audio is generated and measured, never played.
#include "vibedsp/ssb_offset.h"

#include <algorithm>
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

// Power of the component at `hz` (Goertzel over the whole buffer, Hann-windowed).
static double power(const std::vector<float>& x, size_t from, double rate, double hz) {
    const size_t n = x.size() - from;
    double re = 0, im = 0;
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / (n - 1));
        const double a = 2 * M_PI * hz * i / rate;
        re += x[from + i] * w * std::cos(a); im -= x[from + i] * w * std::sin(a);
    }
    return re * re + im * im;
}
static double db(double p, double ref) { return 10 * std::log10((p + 1e-30) / (ref + 1e-30)); }

/* A synthetic SSB voice: harmonics of a pitch that glides (95-180 Hz) the way intonation does, every one
 * moved by `offsetHz` (that IS the mistuning), inside a 250-2800 Hz passband with a falling tilt, spoken
 * in syllables with a pause every few seconds, over band noise `snrDb` below the speech. */
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
        const double syl = std::max(0.0, std::sin(2 * M_PI * 3.1 * t));            // ~6 syllables a second
        const double pause = std::fmod(t, 5.0) > 4.0 ? 0.0 : 1.0;                 // breath every 5 s
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

int main() {
    std::printf("SSB auto-tune — synthetic\n");

    std::printf(" ── 1. the single-sideband shift ──\n");
    for (double rate : {16000.0, 12000.0}) {
        for (double f : {300.0, 1000.0, 3000.0}) {
            for (double sh : {60.0, -60.0}) {
                const size_t n = (size_t)rate * 2;
                std::vector<float> x(n);
                for (size_t i = 0; i < n; ++i) x[i] = (float)std::sin(2 * M_PI * f * i / rate);
                SsbShifter s(rate);
                s.setShift(sh);
                s.process(x.data(), 1000);                       // in two pieces: the boundary must not matter
                s.process(x.data() + 1000, (int)n - 1000);
                const double want = power(x, 2000, rate, f + sh), image = power(x, 2000, rate, f - sh);
                const double orig = power(x, 2000, rate, f);
                char what[160];
                std::snprintf(what, sizeof what, "%5.0f Hz %+3.0f at %5.0f Hz: image %.0f dB, the original %.0f dB (want ≤ -40)",
                              f, sh, rate, db(image, want), db(orig, want));
                check(db(image, want) <= -40 && db(orig, want) <= -40, what);
            }
        }
    }
    {
        // zero shift = the input, delayed by the filter's group delay (so engaging it never jumps in time)
        std::vector<float> x(2000), y;
        for (size_t i = 0; i < x.size(); ++i) x[i] = (float)std::sin(0.01 * i * i);
        y = x;
        SsbShifter s(16000);
        s.process(y.data(), (int)y.size());
        bool same = true;
        for (size_t i = 95; i < y.size(); ++i) same &= y[i] == x[i - 95];
        check(same, "no shift: the input exactly, 95 samples (5.9 ms at 16 kHz) late");
    }

    std::printf(" ── 2. known offsets in a synthetic voice (40 s, 16 kHz) ──\n");
    for (double d : {-180.0, -100.0, -37.0, 0.0, 64.0, 150.0}) {
        const auto x = voice(16000, 40, d, 30, 7 + (unsigned)(d + 500));
        const auto e = SsbOffsetEstimator::estimate(x.data(), (int)x.size(), 16000);
        char what[200];
        std::snprintf(what, sizeof what, "offset %+4.0f: estimated %+6.1f Hz, confidence %.2f (gate %.2f), %.0f dB, %.0f s voiced",
                      d, e.offsetHz, e.confidence, SsbOffsetEstimator::gateFor(e.offsetHz), e.snrDb, e.voicedSeconds);
        check(std::fabs(e.offsetHz - d) <= 3 && e.passes(), what);
    }
    {
        std::mt19937 rng(3);
        std::normal_distribution<float> g(0.0f, 0.05f);
        std::vector<float> x(16000 * 40);
        for (auto& v : x) v = g(rng);
        const auto e = SsbOffsetEstimator::estimate(x.data(), (int)x.size(), 16000);
        char what[160];
        std::snprintf(what, sizeof what, "band noise alone: %+.0f Hz at confidence %.2f — not believed", e.offsetHz, e.confidence);
        check(!e.passes(), what);
        std::vector<float> z(16000 * 40, 0.0f);
        const auto ez = SsbOffsetEstimator::estimate(z.data(), (int)z.size(), 16000);
        check(!ez.valid, "digital silence (squelch shut): no estimate at all");
    }

    std::printf(" ── 3. the decision ──\n");
    {
        const auto x = voice(16000, 120, 90, 30, 11);
        SsbAutoTune t(16000);
        t.setSideband(+1);
        std::vector<float> y(x);
        double firstAt = -1;
        for (size_t i = 0; i + 320 <= y.size(); i += 320) {
            t.process(&y[i], 320);
            if (firstAt < 0 && t.appliedHz() != 0) firstAt = i / 16000.0;
        }
        char what[200];
        std::snprintf(what, sizeof what, "+90 Hz voice: shifted %+.1f Hz (want -90 ± 3) after %.0f s, %d estimates, %d correction(s)",
                      t.appliedHz(), firstAt, t.estimates(), t.corrections());
        check(std::fabs(t.appliedHz() + 90) <= 3 && t.corrections() == 1, what);
        // the corrected audio's comb is back on multiples of the pitch
        const auto e = SsbOffsetEstimator::estimate(y.data() + 16000 * 80, 16000 * 40, 16000);
        std::snprintf(what, sizeof what, "…and the corrected audio now reads %+.1f Hz", e.offsetHz);
        check(std::fabs(e.offsetHz) <= 3, what);
        t.reset();
        check(t.appliedHz() == 0, "a retune resets the shift");
        t.setSideband(-1);
        check(t.appliedHz() == 0 && t.estimates() == 0, "a mode change resets it too");
    }
    {
        const auto x = voice(16000, 100, 90, 20, 12);
        SsbAutoTune t(16000);   // sideband 0: not SSB
        std::vector<float> y(x);
        for (size_t i = 0; i + 320 <= y.size(); i += 320) t.process(&y[i], 320);
        check(t.estimates() == 0 && t.appliedHz() == 0, "not SSB: the estimator never runs, nothing is shifted");
    }
    {
        // ★ An offset beyond ±250 Hz is never corrected (and ±300 is the edge of the search).
        const auto x = voice(16000, 120, 280, 30, 13);
        SsbAutoTune t(16000);
        t.setSideband(+1);
        std::vector<float> y(x);
        for (size_t i = 0; i + 320 <= y.size(); i += 320) t.process(&y[i], 320);
        char what[160];
        std::snprintf(what, sizeof what, "+280 Hz voice (beyond ±250): left alone (shift %+.1f, last estimate %+.0f)",
                      t.appliedHz(), t.lastEstimate().offsetHz);
        check(t.appliedHz() == 0, what);
    }
    {
        // C API: the dial correction's sign per sideband. Audio HIGH by 90 Hz means: USB dial 90 Hz LOW
        // (move it +90), LSB dial 90 Hz HIGH (move it −90).
        const auto x16 = voice(16000, 120, 90, 30, 14);
        for (int sb : {+1, -1}) {
            void* h = vibe_rxfeed_create();
            vibe_rxfeed_set_tuning(h, sb, 7.1e6);
            std::vector<float> out(1000);
            for (size_t i = 0; i + 320 <= x16.size(); i += 320)
                vibe_rxfeed_process(h, &x16[i], 320, 16000, out.data(), (int)out.size());
            const float dial = vibe_rxfeed_dial_correction(h);
            float st[7]; vibe_rxfeed_stats(h, st);
            char what[160];
            std::snprintf(what, sizeof what, "%s: dial correction %+.1f Hz (want %+d ± 3)", sb > 0 ? "USB" : "LSB", dial, sb > 0 ? 90 : -90);
            check(std::fabs(dial - (sb > 0 ? 90 : -90)) <= 3, what);
            vibe_rxfeed_set_tuning(h, sb, 7.1e6 + 50);   // retune by 50 Hz → reset on the next block
            vibe_rxfeed_process(h, x16.data(), 320, 16000, out.data(), (int)out.size());
            check(vibe_rxfeed_dial_correction(h) == 0, "  …a retune (from another thread's call) resets it on the next block");
            vibe_rxfeed_destroy(h);
        }
    }

    std::printf(" ── 4. the feed: any rate → 16 kHz, anti-aliased ──\n");
    {
        RecogniserFeed f;
        const int rate = 48000;
        std::vector<float> in(rate), out(RecogniserFeed::maxOut(960, rate));
        std::vector<float> got1k, got12k;
        for (int tone : {1000, 12000}) {
            RecogniserFeed g;
            std::vector<float> acc;
            for (int i = 0; i < rate; ++i) in[i] = (float)std::sin(2 * M_PI * tone * i / rate);
            size_t total = 0;
            for (int i = 0; i < rate; i += 960) {
                const int m = g.process(&in[i], 960, rate, out.data());
                acc.insert(acc.end(), out.begin(), out.begin() + m);
                total += m;
            }
            if (tone == 1000) { got1k = acc; } else { got12k = acc; }
            if (tone == 1000) {
                char what[120];
                std::snprintf(what, sizeof what, "48 000 samples in → %zu out (want 16 000 ± 64, the filter's delay)", total);
                check(total >= 16000 - 64 && total <= 16000, what);
            }
        }
        const double p1 = power(got1k, 400, 16000, 1000), p12 = power(got12k, 400, 16000, 4000);
        // ★ What the Android tap's old continuous-phase LINEAR interpolator made of the same 12 kHz tone.
        std::vector<float> lin;
        { double p = 0; const double step = 3.0;
          for (int i = 0; i < rate; ++i) in[i] = (float)std::sin(2 * M_PI * 12000 * i / rate);
          for (; p < rate - 1; p += step) { const int i = (int)p; const double fr = p - i; lin.push_back((float)(in[i] + (in[i + 1] - in[i]) * fr)); } }
        const double pl = power(lin, 400, 16000, 4000);
        char what[200];
        std::snprintf(what, sizeof what, "a 12 kHz tone does not fold to 4 kHz: %.0f dB against the 1 kHz tone (want ≤ -50; the old linear tap: %.0f dB)",
                      db(p12, p1), db(pl, p1));
        check(db(p12, p1) <= -50, what);
        double rms = 0;
        for (size_t i = 400; i < got1k.size(); ++i) rms += got1k[i] * got1k[i];
        rms = std::sqrt(rms / (got1k.size() - 400));
        std::snprintf(what, sizeof what, "1 kHz passes at %.2f dB", 20 * std::log10(rms * std::sqrt(2.0)));
        check(std::fabs(20 * std::log10(rms * std::sqrt(2.0))) < 0.2, what);
        // 44.1 kHz too (Bluetooth / some players)
        RecogniserFeed h;
        std::vector<float> a(44100), o(RecogniserFeed::maxOut(441, 44100));
        size_t total = 0;
        for (int i = 0; i < 44100; i += 441) total += h.process(&a[i], 441, 44100, o.data());
        std::snprintf(what, sizeof what, "44 100 in → %zu out", total);
        check(total >= 16000 - 64 && total <= 16000, what);
    }

    std::printf(" ── 5. FAST (the binaural aid) and the cost ──\n");
    {
        const auto x = voice(16000, 60, -70, 30, 21);
        SsbOffsetEstimator fast(16000, SsbOffsetEstimator::Profile::Fast);
        std::vector<float> got;
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i + 320 <= x.size(); i += 320)
            if (fast.push(&x[i], 320) && fast.last().valid) got.push_back(fast.last().offsetHz);
        const double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::vector<float> s(got);
        std::sort(s.begin(), s.end());
        const float med = s.empty() ? 999 : s[s.size() / 2];
        int near = 0;
        for (float v : got) near += std::fabs(v + 70) <= 15;
        char what[200];
        std::snprintf(what, sizeof what, "FAST on a -70 Hz voice: %zu estimates in 60 s, median %+.0f Hz, %d within 15 Hz; %.2f %% of a core",
                      got.size(), med, near, 100 * cpu / 60);
        check(got.size() >= 25 && std::fabs(med + 70) <= 8 && near >= (int)got.size() * 3 / 4, what);
    }
    {
        const auto x = voice(16000, 120, 40, 20, 22);
        SsbAutoTune t(16000);
        t.setSideband(-1);
        std::vector<float> y(x);
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i + 320 <= y.size(); i += 320) t.process(&y[i], 320);
        const double all = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        char what[200];
        std::snprintf(what, sizeof what, "SLOW auto-tune, everything (estimator + shift) at 16 kHz: %.3f %% of a core; analysis alone %.3f %%",
                      100 * all / 120, 100 * t.cpuSeconds() / 120);
        check(100 * all / 120 < 0.5, what);
    }

    std::printf("%s\n", fails ? "SSB AUTO-TUNE: FAILURES" : "SSB AUTO-TUNE: all passed");
    return fails ? 1 : 0;
}
