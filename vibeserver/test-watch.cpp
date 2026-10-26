// test-watch.cpp — V12 VFO A/B (vibe_watch.h): the watch channel, its squelch, and the pan mixer that puts the LOWER
// VFO in the LEFT ear and the HIGHER in the RIGHT (Stuart, 2026-10-27), with every move a smooth equal-power ramp.
//
//   1. a watch channel demodulates ITS carrier out of a stream holding two (AM, 600 Hz vs 1100 Hz tones);
//   2. routing by frequency: lower left, higher right — whichever is A or B;
//   3. adding B pans A from centre to its ear while B fades in; constant loudness through the pan; no click;
//   4. tuning A across B swaps the ears with a cross-fade, kSwapHz of hysteresis, no flutter, no discontinuity;
//   5. equal frequencies (within kSameHz) put both in both ears;
//   6. STEREO OFF = both centre; the live toggle ramps; a MONO output centres everything;
//   7. removing B fades it out and pans A back to centre — then the mixer is exactly today's mono (hand-back, no step);
//   8. the squelch: hysteresis, tail, open/closed with the time it changed; a closed gate silences ITS channel only;
//   9. the FIFO primes, bounds its latency and re-primes after an underrun.
// SILENT: nothing is played.
#include "../android/app/src/main/cpp/vibe_watch.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace vibewatch;
static int pass = 0, fail = 0;
static void ok(bool c, const char* what) { (c ? pass : fail)++; std::printf("   %s %s\n", c ? "ok  " : "FAIL", what); }

static double goertzel(const float* x, int n, int stride, double f, double fs) {
    const double w = 2 * M_PI * f / fs, cw = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) { const double s = x[i * stride] + cw * s1 - s2; s2 = s1; s1 = s; }
    return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - cw * s1 * s2)) / n;
}
static double rms(const float* x, int n, int stride) { double a = 0; for (int i = 0; i < n; i++) a += x[i * stride] * x[i * stride]; return std::sqrt(a / std::max(1, n)); }

/** Run the mixer for `frames` with sources given by a callback (so frequencies can move mid-run). */
struct Bench {
    PanMixer mx;
    std::vector<float> out;      // interleaved L/R, appended
    double t = 0;
    void run(std::vector<PanMixer::Src> s, int frames, const std::vector<double>& toneHz, const std::vector<float>& amp) {
        std::vector<std::vector<float>> pcm(s.size(), std::vector<float>((size_t)frames));
        for (size_t k = 0; k < s.size(); k++)
            for (int i = 0; i < frames; i++)
                pcm[k][i] = toneHz[k] > 0 ? amp[k] * (float)std::sin(2 * M_PI * toneHz[k] * (t + (double)i / kOutRate)) : amp[k];
        for (size_t k = 0; k < s.size(); k++) s[k].pcm = pcm[k].data();
        std::vector<float> o((size_t)frames * 2);
        mx.mix(s, frames, o.data());
        out.insert(out.end(), o.begin(), o.end());
        t += (double)frames / kOutRate;
    }
    /** largest sample-to-sample step in either ear over the last `frames` */
    double maxStep(int frames) const {
        double m = 0; const size_t n = out.size() / 2;
        for (size_t i = n - frames + 1; i < n; i++)
            for (int c = 0; c < 2; c++) m = std::max(m, (double)std::fabs(out[2 * i + c] - out[2 * (i - 1) + c]));
        return m;
    }
    const float* tail(int frames) const { return out.data() + out.size() - (size_t)frames * 2; }
};
static PanMixer::Src S(int key, double hz, bool present = true, bool open = true) { PanMixer::Src s; s.key = key; s.hz = hz; s.present = present; s.gateOpen = open; return s; }

int main() {
    const int blk = 480, settle = (int)(kRampSec * kOutRate) + 12 * blk;   // the ramp, then 120 ms at rest to measure
    std::printf("── 1. a watch channel demodulates its own carrier ──\n");
    {
        // 240 kHz of IQ: carrier A at -30 kHz (600 Hz tone), carrier B at +40 kHz (1100 Hz tone), a little noise.
        const double fs = 240000; const int n = (int)fs * 3;
        std::vector<cf32> iq((size_t)n);
        std::mt19937 rng(7); std::normal_distribution<float> nd(0.0f, 0.002f);
        for (int i = 0; i < n; i++) {
            const double t = i / fs;
            const double a = 0.2 * (1 + 0.6 * std::sin(2 * M_PI * 600 * t)), b = 0.2 * (1 + 0.6 * std::sin(2 * M_PI * 1100 * t));
            iq[i] = cf32((float)(a * std::cos(2 * M_PI * -30000 * t) + b * std::cos(2 * M_PI * 40000 * t)) + nd(rng),
                         (float)(a * std::sin(2 * M_PI * -30000 * t) + b * std::sin(2 * M_PI * 40000 * t)) + nd(rng));
        }
        WatchChannel w(1);
        Wanted wa; wa.hz = 40000; wa.mode = "am"; wa.rxMode = Mode::AM; wa.bwHz = 8000;
        w.want(wa);
        // ★ Fed and drained a block at a time, as the shim does: the FIFO holds 200 ms at most by design.
        auto run = [&](double off) {
            std::vector<float> all, o(960);
            for (int i = 0; i + 4800 <= n; i += 4800) { w.feed(iq.data() + i, 4800, fs, off); w.fifo().pop(o.data(), 960); all.insert(all.end(), o.begin(), o.end()); }
            return std::vector<float>(all.end() - 48000, all.end());   // the last second (the first: AGC settling)
        };
        std::vector<float> a = run(40000.0);
        size_t got = 0; for (float v : a) got += v != 0.0f;
        const double g1100 = goertzel(a.data(), 24000, 1, 1100, 48000), g600 = goertzel(a.data(), 24000, 1, 600, 48000);
        std::printf("      1100 Hz %.4f, 600 Hz %.5f (%zu non-zero frames)\n", g1100, g600, got);
        ok(got > 40000, "the channel produced a second of audio");
        ok(g1100 > 20 * g600, "it hears ITS carrier (1100 Hz) and not the other (600 Hz) — > 26 dB apart");
        // re-aim at A: the same object now hears A
        wa.hz = -30000; w.want(wa);
        w.flush();
        a = run(-30000.0);
        ok(goertzel(a.data(), 24000, 1, 600, 48000) > 20 * goertzel(a.data(), 24000, 1, 1100, 48000), "re-aimed (channel stepping), it hears the other carrier");
    }

    std::printf("── 2+3. lower left, higher right; adding B pans A from centre with B fading in ──\n");
    {
        Bench b;
        b.run({S(0, 7.000e6)}, settle, {600}, {0.25f});            // A alone: centre, unity
        const float* t0 = b.tail(blk * 4);
        ok(std::fabs(rms(t0, blk * 4, 2) - rms(t0 + 1, blk * 4, 2)) < 1e-4 && std::fabs(rms(t0, blk * 4, 2) - 0.25 / std::sqrt(2.0)) < 2e-3,
           "A alone: both ears at unity (today's mono)");
        const size_t before = b.out.size();
        for (int i = 0; i < settle / blk; i++) b.run({S(0, 7.000e6), S(1, 7.010e6)}, blk, {600, 1100}, {0.25f, 0.25f});
        const float* t = b.tail(blk * 8);
        const double lA = goertzel(t, blk * 8, 2, 600, kOutRate), lB = goertzel(t, blk * 8, 2, 1100, kOutRate);
        const double rA = goertzel(t + 1, blk * 8, 2, 600, kOutRate), rB = goertzel(t + 1, blk * 8, 2, 1100, kOutRate);
        std::printf("      L: A %.4f B %.5f · R: A %.5f B %.4f\n", lA, lB, rA, rB);
        ok(lA > 1000 * (lB + 1e-9) && rB > 1000 * (rA + 1e-9), "A (lower) wholly LEFT, B (higher) wholly RIGHT");
        ok(std::fabs(lA - 0.125 * std::sqrt(2.0)) < 0.01, "a hard-panned source is +3 dB in its ear (equal-power, √2)");
        // the same with B BELOW A: the ears follow frequency, not the letter
        Bench c;
        c.run({S(0, 7.000e6)}, settle, {600}, {0.25f});
        for (int i = 0; i < settle / blk; i++) c.run({S(0, 7.000e6), S(1, 6.990e6)}, blk, {600, 1100}, {0.25f, 0.25f});
        const float* u = c.tail(blk * 8);
        ok(goertzel(u, blk * 8, 2, 1100, kOutRate) > 1000 * goertzel(u, blk * 8, 2, 600, kOutRate) &&
           goertzel(u + 1, blk * 8, 2, 600, kOutRate) > 1000 * goertzel(u + 1, blk * 8, 2, 1100, kOutRate),
           "B below A: B left, A right — the ears follow frequency, whichever is A or B");
        // constant loudness of A through its pan: DC source, watch the L²+R² of A alone (B silent, amp 0)
        Bench d;
        d.run({S(0, 7.0e6)}, settle, {0}, {0.25f});
        for (int i = 0; i < settle / blk; i++) d.run({S(0, 7.0e6), S(1, 7.01e6)}, blk, {0, 0}, {0.25f, 0.0f});
        double pmin = 1e9, pmax = 0;
        const size_t n0 = d.out.size() / 2 - settle;
        for (size_t i = n0; i < d.out.size() / 2; i++) { const double p = d.out[2 * i] * d.out[2 * i] + d.out[2 * i + 1] * d.out[2 * i + 1]; pmin = std::min(pmin, p); pmax = std::max(pmax, p); }
        std::printf("      A's power through the pan: %.6f … %.6f (centre %.6f)\n", pmin, pmax, 2 * 0.25 * 0.25);
        ok(pmax / pmin < 1.0001 && std::fabs(pmin - 0.125) < 1e-4, "equal power: A's loudness is constant all the way through the pan");
        ok(d.maxStep(settle) < 0.25 * 3.0 / (kRampSec * kOutRate), "the pan has no step (largest sample-to-sample change ≤ 3× the ramp's mean slope)");
        ok(std::fabs(d.tail(1)[0] - 0.25f * 1.41421356f) < 1e-4 && std::fabs(d.tail(1)[1]) < 1e-6, "A ends hard left at √2");
        (void)before;
    }

    std::printf("── 4. tuning A across B swaps the ears, with hysteresis ──\n");
    {
        Bench b;
        const double B = 7.010e6;
        for (int i = 0; i < settle / blk; i++) b.run({S(0, 7.000e6), S(1, B)}, blk, {0, 0}, {0.25f, 0.0f});
        auto leftness = [&] { const float* t = b.tail(1); return t[0] - t[1]; };
        for (int i = 0; i < settle / blk; i++) b.run({S(0, B + 50), S(1, B)}, blk, {0, 0}, {0.25f, 0.0f});
        ok(leftness() > 0.3, "A 50 Hz above B (inside the 100 Hz hysteresis): no swap — A still left");
        for (int i = 0; i < settle / blk; i++) b.run({S(0, B + 150), S(1, B)}, blk, {0, 0}, {0.25f, 0.0f});
        ok(leftness() < -0.3, "A 150 Hz above B: swapped — A now right");
        ok(b.maxStep(settle) < 0.25 * 3.0 / (kRampSec * kOutRate) * 2.0, "the swap is a cross-fade: no discontinuity");
        // wobble back and forth across B by ±60 Hz: no flutter
        int flips = 0; double prev = leftness();
        for (int k = 0; k < 20; k++) {
            b.run({S(0, B + ((k & 1) ? 60 : -60)), S(1, B)}, blk, {0, 0}, {0.25f, 0.0f});
            const double now = leftness(); if ((now > 0) != (prev > 0)) flips++; prev = now;
        }
        ok(flips == 0, "a needle wobbling ±60 Hz over the other never flutters the ears");
        for (int i = 0; i < settle / blk; i++) b.run({S(0, B - 150), S(1, B)}, blk, {0, 0}, {0.25f, 0.0f});
        ok(leftness() > 0.3, "back 150 Hz below B: A left again");
    }

    std::printf("── 5. equal frequencies: both in both ears ──\n");
    {
        Bench b;
        for (int i = 0; i < settle / blk; i++) b.run({S(0, 7.0e6), S(1, 7.0e6 + 3)}, blk, {600, 1100}, {0.25f, 0.25f});
        const float* t = b.tail(blk * 8);
        const double lA = goertzel(t, blk * 8, 2, 600, kOutRate), rA = goertzel(t + 1, blk * 8, 2, 600, kOutRate);
        const double lB = goertzel(t, blk * 8, 2, 1100, kOutRate), rB = goertzel(t + 1, blk * 8, 2, 1100, kOutRate);
        ok(std::fabs(lA - rA) < 1e-4 && std::fabs(lB - rB) < 1e-4 && std::fabs(lA - 0.125) < 0.005, "3 Hz apart: each at unity in BOTH ears");
    }

    std::printf("── 6. stereo off, the live toggle, a mono output ──\n");
    {
        Bench b;
        for (int i = 0; i < settle / blk; i++) b.run({S(0, 7.0e6), S(1, 7.01e6)}, blk, {0, 0}, {0.25f, 0.0f});
        b.mx.setStereo(false);
        for (int i = 0; i < settle / blk; i++) b.run({S(0, 7.0e6), S(1, 7.01e6)}, blk, {0, 0}, {0.25f, 0.0f});
        ok(std::fabs(b.tail(1)[0] - 0.25f) < 1e-5 && std::fabs(b.tail(1)[1] - 0.25f) < 1e-5, "STEREO OFF: A centre, unity in both ears");
        ok(b.maxStep(settle) < 0.25 * 3.0 / (kRampSec * kOutRate), "switched off while listening: a ramp, no step");
        b.mx.setStereo(true);
        for (int i = 0; i < settle / blk; i++) b.run({S(0, 7.0e6), S(1, 7.01e6)}, blk, {0, 0}, {0.25f, 0.0f});
        ok(b.tail(1)[0] > 0.35f && std::fabs(b.tail(1)[1]) < 1e-5, "switched back on: A hard left again");
        ok(b.maxStep(settle) < 0.25 * 3.0 / (kRampSec * kOutRate), "…and that ramp has no step either");
        b.mx.setMonoOut(true);
        for (int i = 0; i < settle / blk; i++) b.run({S(0, 7.0e6), S(1, 7.01e6)}, blk, {0, 0}, {0.25f, 0.25f});
        ok(std::fabs(b.tail(1)[0] - 0.5f) < 1e-4 && std::fabs(b.tail(1)[1] - 0.5f) < 1e-4, "MONO output: both centre at unity, so the fold hears A + B at full level");
    }

    std::printf("── 7. removing B: it fades out, A pans back, the hand-back is seamless ──\n");
    {
        Bench b;
        b.run({S(0, 7.0e6)}, settle, {0}, {0.25f});
        for (int i = 0; i < settle / blk; i++) b.run({S(0, 7.0e6), S(1, 7.01e6)}, blk, {0, 600}, {0.25f, 0.2f});
        ok(!b.mx.restingMono(), "while B is on, the mixer is not today's mono");
        int frames = 0;
        while (!b.mx.faded(1) && frames < 4 * settle) { b.run({S(0, 7.0e6), S(1, 7.01e6, false)}, blk, {0, 600}, {0.25f, 0.2f}); frames += blk; }
        ok(b.mx.faded(1), "B has faded out");
        std::printf("      fade-out took %.0f ms\n", 1000.0 * frames / kOutRate);
        ok(frames <= (int)(kRampSec * kOutRate) + blk, "within kRampSec");
        b.run({S(0, 7.0e6)}, blk, {0}, {0.25f});
        ok(b.mx.restingMono(), "then the mixer is resting at today's mono");
        ok(std::fabs(b.tail(1)[0] - 0.25f) < 1e-5 && std::fabs(b.tail(1)[1] - 0.25f) < 1e-5, "A back at unity in both ears — the ordinary path takes over with no step");
        double m = 0; const size_t n = b.out.size() / 2;
        for (size_t i = n - frames - blk + 1; i < n; i++) {  // smoothness of A's DC through the pan back (B's tone removed by differencing is not needed: B is a sine fading — check L+R of A via the DC component only)
            m = std::max(m, (double)std::fabs((b.out[2 * i] + b.out[2 * i + 1]) - (b.out[2 * (i - 1)] + b.out[2 * (i - 1) + 1])));
        }
        ok(m < 0.2 * 2 * M_PI * 600 / kOutRate * 1.5 + 0.25 * 6.0 / (kRampSec * kOutRate), "no click on removal (the step is only the fading tone's own slope)");
    }

    std::printf("── 8. the squelch ──\n");
    {
        SquelchGate g; g.on = true; g.db = -80; g.open = false;
        ok(!g.update(-90, 0) && !g.open, "below threshold: shut");
        ok(g.update(-79, 100) && g.open && g.changedAt == 100, "above: opens, stamped with when");
        ok(!g.update(-81, 200) && g.open, "1 dB under (inside the 2 dB hysteresis): stays open");
        ok(!g.update(-83, 300) && g.open, "3 dB under: still open during the tail");
        ok(g.update(-83, 560) && !g.open && g.changedAt == 560, "after the 250 ms tail: shuts, stamped");
        SquelchGate off; ok(!off.update(-200, 0) && off.open, "squelch off: always open");
        // a shut gate silences ITS channel only, through the mixer's 10 ms fade
        Bench b;
        for (int i = 0; i < settle / blk; i++) b.run({S(0, 7.0e6), S(1, 7.01e6, true, false)}, blk, {600, 1100}, {0.25f, 0.25f});
        const float* t = b.tail(blk * 8);
        ok(goertzel(t + 1, blk * 8, 2, 1100, kOutRate) < 1e-6 && goertzel(t, blk * 8, 2, 600, kOutRate) > 0.1, "B shut: the right ear is silent, A plays on in the left");
        for (int i = 0; i < 4; i++) b.run({S(0, 7.0e6), S(1, 7.01e6, true, true)}, blk, {600, 1100}, {0.25f, 0.25f});
        ok(goertzel(b.tail(blk) + 1, blk, 2, 1100, kOutRate) > 0.1, "B opens: heard within a block");
    }

    std::printf("── 9. the FIFO ──\n");
    {
        PcmFifo f(480, 2000);
        std::vector<float> x(300, 1.0f), o(300);
        f.push(x.data(), 300);
        ok(f.pop(o.data(), 300) == 0 && o[0] == 0.0f, "unprimed: silence");
        f.push(x.data(), 300);
        ok(f.pop(o.data(), 300) == 300 && o[0] == 1.0f, "primed at 480: audio");
        for (int i = 0; i < 20; i++) f.push(x.data(), 300);
        ok(f.depth() == 2000 && f.droppedFrames() > 0, "never more than its bound (oldest dropped)");
        std::vector<float> big(3000);
        ok(f.pop(big.data(), 3000) == 2000 && f.underruns() == 1, "an underrun is counted and zero-filled");
        f.push(x.data(), 300);
        ok(f.pop(o.data(), 300) == 0, "…and it re-primes before playing again");
    }
    std::printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
