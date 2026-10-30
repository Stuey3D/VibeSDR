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
//   9. the FIFO primes, bounds its latency and re-primes after an underrun;
//  10. MUTE (round 3): a muted VFO fades out over the same 400 ms ramp and the other pans home to both ears; unmute reverses;
//  10b. THE A ⟷ B BALANCE (Stuart, 2026-10-30): balanceGains at −1/−0.5/0/0.5/1 (+ the dead zone and end snap) is
//      dualMix.ts's rule; in the mixer it only LOWERS the VFO leaned away from, in STEREO and in MONO; a full lean is
//      that VFO alone, centred; a VFO alone has no claim on it; every change a ramp with no step;
//  11. the RF centre for two VFOs on a personal receiver (fitCentre): stays put, moves to fit both, refuses too far apart;
//  12. DECODE ON EITHER VFO (run inside 1, on its channel): the audio tap a decoder on VFO B is fed — every frame, the
//      same samples the mix gets, still while MUTED, nothing while PARKED or once removed.
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

        std::printf("── 12. decode on either VFO: the channel's audio tap (2026-10-27) ──\n");
        // ★ What a decoder on VFO B is fed: the SAME demodulated audio the FIFO gets, mono, frame for frame — and it is not
        //   the mix, so a MUTED channel still feeds it; a PARKED one (outside the window) feeds nothing; no tap, no cost.
        std::vector<float> tapped, popped;
        w.setAudioTap([&](const float* p, int k) { tapped.insert(tapped.end(), p, p + k); });
        w.flush();
        w.setMuted(true);
        std::vector<float> o(960);
        for (int i = 0; i + 4800 <= n; i += 4800) {
            w.feed(iq.data() + i, 4800, fs, -30000.0);
            const size_t got = w.fifo().pop(o.data(), 960);
            popped.insert(popped.end(), o.begin(), o.begin() + (long)got);
        }
        std::printf("      tapped %zu frames, the FIFO gave %zu\n", tapped.size(), popped.size());
        ok(tapped.size() > 140000, "the tap is handed every frame the channel makes (3 s of 48 kHz)");
        // ★ nothing dropped (fed and drained in step), so what the mix popped is the tap's stream from its start
        ok(popped.size() > 100000 && popped.size() <= tapped.size() && std::equal(popped.begin(), popped.end(), tapped.begin()),
           "…the same samples the mix is given (the FIFO's, frame for frame)");
        const std::vector<float> last(tapped.end() - 24000, tapped.end());
        ok(goertzel(last.data(), 24000, 1, 600, 48000) > 20 * goertzel(last.data(), 24000, 1, 1100, 48000),
           "the tap hears the channel's own carrier — MUTED in the mix, and still decoding");
        const size_t before = tapped.size();
        w.setParked(true);
        for (int i = 0; i + 4800 <= 48000; i += 4800) w.feed(iq.data() + i, 4800, fs, -30000.0);
        ok(tapped.size() == before, "a PARKED channel (outside the window) feeds the decoder nothing");
        w.setParked(false);
        w.setAudioTap(nullptr);
        for (int i = 0; i + 4800 <= 48000; i += 4800) w.feed(iq.data() + i, 4800, fs, -30000.0);
        ok(tapped.size() == before, "the tap removed: nothing more handed on");
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
        /* ★★ V12 beta24 — THE CUSHION. The mix asks for ~491 frames a block and the channel delivers ~491, each varying by a
         *  frame, and on a multi-core server the channel's demodulator is a worker thread, a hand-off behind. Primed at one
         *  block (the old 480) every short block underran; primed at kWatchPrimeFrames it never does, even with the
         *  producer a whole 2-block burst late now and then. */
        auto run = [](size_t prime) {
            PcmFifo q(prime, 9600);
            std::vector<float> in(1000, 0.5f), out(1000);
            int under = 0;
            for (int k = 0; k < 2000; k++) {
                const size_t give = 491 + (k % 3 == 0), take = 491 + (k % 2 == 0);
                const bool late = (k % 97) == 50;                       // ★ the worker late: nothing this block…
                if (!late) q.push(in.data(), (k % 97) == 51 ? 0 : give);
                if ((k % 97) == 51) { q.push(in.data(), give); q.push(in.data(), give); }   // …then two at once
                if (q.pop(out.data(), take) < take && k > 10) under++;
            }
            return under;
        };
        const int u480 = run(480), uNow = run(kWatchPrimeFrames);
        ok(u480 > 20, std::string("primed at one block (480), a mix block of 491-492 underruns again and again (" + std::to_string(u480) + " of 2000)").c_str());
        ok(uNow == 0, std::string("primed at kWatchPrimeFrames (" + std::to_string(kWatchPrimeFrames) + "): never (" + std::to_string(uNow) + ")").c_str());
    }
    std::printf("── 10. MUTE (round 3): the muted VFO fades out and the other pans home to both ears; unmuting reverses it ──\n");
    {
        // DC sources, B silent while measuring A's place: A (0.25) left, B right — then B muted.
        Bench b;
        auto SM = [](int key, double hz, bool muted) { PanMixer::Src s = S(key, hz); s.muted = muted; return s; };
        for (int i = 0; i < settle / blk; i++) b.run({S(0, 7.0e6), S(1, 7.01e6)}, blk, {0, 600}, {0.25f, 0.25f});
        ok(std::fabs(b.tail(1)[0] - 0.25f * 1.41421356f) < 2e-3, "before: A hard left (√2 in the left ear; B's tone alone in the right)");
        const size_t mark = b.out.size() / 2;
        for (int i = 0; i < settle / blk; i++) b.run({S(0, 7.0e6), SM(1, 7.01e6, true)}, blk, {0, 600}, {0.25f, 0.25f});
        const float* t = b.tail(blk * 8);
        ok(goertzel(t + 1, blk * 8, 2, 600, kOutRate) < 1e-6 && goertzel(t, blk * 8, 2, 600, kOutRate) < 1e-6, "B muted: B is gone from both ears");
        // A's own DC: back in the middle at unity, both ears — "pans back to mono so both ears are in play"
        double lA = 0, rA = 0; for (int i = 0; i < blk * 8; i++) { lA += t[2 * i]; rA += t[2 * i + 1]; }
        lA /= blk * 8; rA /= blk * 8;
        std::printf("      A after B muted: L %.4f R %.4f\n", lA, rA);
        ok(std::fabs(lA - 0.25) < 2e-3 && std::fabs(rA - 0.25) < 2e-3, "A is centre at unity — today's mono, both ears");
        // A's power constant through the move home (equal-power): DC only, B's tone muted so it adds nothing once faded —
        // measured on A alone by re-running with B silent
        Bench d;
        for (int i = 0; i < settle / blk; i++) d.run({S(0, 7.0e6), S(1, 7.01e6)}, blk, {0, 0}, {0.25f, 0.0f});
        const size_t n0 = d.out.size() / 2;
        for (int i = 0; i < settle / blk; i++) d.run({S(0, 7.0e6), SM(1, 7.01e6, true)}, blk, {0, 0}, {0.25f, 0.0f});
        double pmin = 1e9, pmax = 0;
        for (size_t i = n0; i < d.out.size() / 2; i++) { const double p = d.out[2 * i] * d.out[2 * i] + d.out[2 * i + 1] * d.out[2 * i + 1]; pmin = std::min(pmin, p); pmax = std::max(pmax, p); }
        ok(pmax / pmin < 1.0001, "equal power: A's loudness is constant all the way home");
        ok(d.maxStep(settle) < 0.25 * 3.0 / (kRampSec * kOutRate), "the move home has no step");
        // the fade itself takes kRampSec: half way through, B is still partly there
        Bench h;
        for (int i = 0; i < settle / blk; i++) h.run({S(0, 7.0e6), S(1, 7.01e6)}, blk, {0, 0}, {0.0f, 0.25f});
        const int half = (int)(kRampSec * kOutRate / 2);
        h.run({S(0, 7.0e6), SM(1, 7.01e6, true)}, half, {0, 0}, {0.0f, 0.25f});
        const float rHalf = h.tail(1)[1];
        ok(rHalf > 0.05f && rHalf < 0.25f * 1.41421356f - 0.05f, "B's fade is a ramp (half way: part way down), not a cut");
        ok(h.maxStep(half) < 0.25 * 1.41421356 * 3.0 / (kRampSec * kOutRate), "…with no click");
        // unmute: back out — A left, B right again
        for (int i = 0; i < settle / blk; i++) b.run({S(0, 7.0e6), S(1, 7.01e6)}, blk, {0, 600}, {0.25f, 0.25f});
        ok(b.tail(1)[0] > 0.3f && goertzel(b.tail(blk * 8) + 1, blk * 8, 2, 600, kOutRate) > 0.1, "unmuted: A back left, B back right");
        (void)mark;
        // mute A instead: B (the higher) goes to the middle
        Bench a;
        for (int i = 0; i < settle / blk; i++) a.run({S(0, 7.0e6), S(1, 7.01e6)}, blk, {0, 0}, {0.25f, 0.25f});
        for (int i = 0; i < settle / blk; i++) a.run({SM(0, 7.0e6, true), S(1, 7.01e6)}, blk, {0, 0}, {0.25f, 0.25f});
        ok(std::fabs(a.tail(1)[0] - 0.25f) < 2e-3 && std::fabs(a.tail(1)[1] - 0.25f) < 2e-3, "A muted: B alone, centre, both ears at unity");
        ok(!a.mx.restingMono(), "…and that is NOT a hand-back (A is silent, B still mixed)");
        // B removed while A was muted: the shim unmutes A as B goes; the hand-back waits for A to be home AND at unity
        for (int i = 0; i < 3; i++) a.run({S(0, 7.0e6), S(1, 7.01e6, false)}, blk, {0, 0}, {0.25f, 0.0f});
        ok(!a.mx.restingMono(), "B removed, A unmuted: not resting while A fades back in");
        for (int i = 0; i < settle / blk; i++) a.run({S(0, 7.0e6), S(1, 7.01e6, false)}, blk, {0, 0}, {0.25f, 0.0f});
        ok(a.mx.restingMono() && std::fabs(a.tail(1)[0] - 0.25f) < 1e-4, "…then resting: A centre at unity, the ordinary path takes over with no step");
    }

    std::printf("── 10b. THE A ⟷ B BALANCE: the two-server rule, a level before placement, stereo and mono, no click ──\n");
    {
        const float h = 0.70710678f;   // cos(π/4)
        auto near = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };
        BalanceGains g;
        g = balanceGains(-1.0f);  ok(near(g.a, 1) && near(g.b, 0), "−1: all A (B silent)");
        g = balanceGains(-0.5f);  ok(near(g.a, 1) && near(g.b, h), "−0.5: A as it is, B down to cos(π/4)");
        g = balanceGains(0.0f);   ok(near(g.a, 1) && near(g.b, 1), "0: both as they are — the balance never raises");
        g = balanceGains(0.5f);   ok(near(g.a, h) && near(g.b, 1), "+0.5: A down to cos(π/4), B as it is");
        g = balanceGains(1.0f);   ok(near(g.a, 0) && near(g.b, 1), "+1: all B");
        g = balanceGains(0.03f);  ok(near(g.a, 1) && near(g.b, 1), "dead zone: 0.03 IS even");
        g = balanceGains(-0.97f); ok(near(g.a, 1) && near(g.b, 0), "end snap: −0.97 IS all A");
        g = balanceGains(NAN);    ok(near(g.a, 1) && near(g.b, 1), "NaN → even");
        g = balanceGains(5.0f);   ok(near(g.a, 0) && near(g.b, 1), "out of range → clamped");
        // STEREO: A (DC 0.25) at 7.0 MHz hard left, B (DC 0.25) at 7.01 hard right — each ear is one VFO.
        Bench s;
        for (int i = 0; i < settle / blk; i++) s.run({S(0, 7.0e6), S(1, 7.01e6)}, blk, {0, 0}, {0.25f, 0.25f});
        const float side = 0.25f * 1.41421356f;
        ok(std::fabs(s.tail(1)[0] - side) < 2e-3 && std::fabs(s.tail(1)[1] - side) < 2e-3, "stereo, even: A left, B right, both at their own level");
        s.mx.setBalance(0.5f);
        for (int i = 0; i < settle / blk; i++) s.run({S(0, 7.0e6), S(1, 7.01e6)}, blk, {0, 0}, {0.25f, 0.25f});
        ok(std::fabs(s.tail(1)[0] - side * h) < 2e-3 && std::fabs(s.tail(1)[1] - side) < 2e-3, "stereo, +0.5: A's ear down by cos(π/4), B's untouched");
        s.mx.setBalance(-0.5f);
        for (int i = 0; i < settle / blk; i++) s.run({S(0, 7.0e6), S(1, 7.01e6)}, blk, {0, 0}, {0.25f, 0.25f});
        ok(std::fabs(s.tail(1)[0] - side) < 2e-3 && std::fabs(s.tail(1)[1] - side * h) < 2e-3, "stereo, −0.5: B's ear down, A's untouched");
        // full lean to A: B fades out AND gives up its place — A alone, centred, both ears at unity
        s.mx.setBalance(-1.0f);
        for (int i = 0; i < settle / blk; i++) s.run({S(0, 7.0e6), S(1, 7.01e6)}, blk, {0, 600}, {0.25f, 0.25f});
        const float* t = s.tail(blk * 8);
        ok(goertzel(t, blk * 8, 2, 600, kOutRate) < 1e-6 && goertzel(t + 1, blk * 8, 2, 600, kOutRate) < 1e-6, "stereo, −1: B gone from both ears");
        double lA = 0, rA = 0; for (int i = 0; i < blk * 8; i++) { lA += t[2 * i]; rA += t[2 * i + 1]; }
        lA /= blk * 8; rA /= blk * 8;
        ok(std::fabs(lA - 0.25) < 2e-3 && std::fabs(rA - 0.25) < 2e-3, "…and A alone, centred, both ears at unity (as the two-server full lean)");
        // ★ It IS exactly the mono a hand-back would give (as with B muted) — the shim keeps mixing while B exists
        //   (watchMix: activeLocked), so B comes straight back when the balance moves off the end.
        ok(s.mx.restingMono(), "…exactly today's mono (B muted by the balance — the shim still mixes while B exists)");
        s.mx.setBalance(1.0f);
        for (int i = 0; i < settle / blk; i++) s.run({S(0, 7.0e6), S(1, 7.01e6)}, blk, {0, 0}, {0.25f, 0.25f});
        ok(std::fabs(s.tail(1)[0] - 0.25f) < 2e-3 && std::fabs(s.tail(1)[1] - 0.25f) < 2e-3, "stereo, +1: B alone, centred, both ears at unity");
        // MONO OUTPUT (Stuart: "the balance also applies if the user is in Mono mode too"): both centre; A tone 600, B DC
        Bench m;
        m.mx.setMonoOut(true);
        for (int i = 0; i < settle / blk; i++) m.run({S(0, 7.0e6), S(1, 7.01e6)}, blk, {0, 0}, {0.25f, 0.0f});
        const float aEven = m.tail(1)[0];
        m.mx.setBalance(0.5f);
        for (int i = 0; i < settle / blk; i++) m.run({S(0, 7.0e6), S(1, 7.01e6)}, blk, {0, 0}, {0.25f, 0.0f});
        ok(std::fabs(aEven - 0.25f) < 2e-3 && std::fabs(m.tail(1)[0] - 0.25f * h) < 2e-3 && std::fabs(m.tail(1)[1] - 0.25f * h) < 2e-3,
           "mono, +0.5: A (both ears) turned down by cos(π/4)");
        Bench mb; mb.mx.setMonoOut(true); mb.mx.setBalance(0.5f);
        for (int i = 0; i < settle / blk; i++) mb.run({S(0, 7.0e6), S(1, 7.01e6)}, blk, {0, 0}, {0.0f, 0.25f});
        ok(std::fabs(mb.tail(1)[0] - 0.25f) < 2e-3 && std::fabs(mb.tail(1)[1] - 0.25f) < 2e-3, "mono, +0.5: B (both ears) untouched");
        mb.mx.setBalance(-1.0f);
        for (int i = 0; i < settle / blk; i++) mb.run({S(0, 7.0e6), S(1, 7.01e6)}, blk, {0, 0}, {0.0f, 0.25f});
        ok(std::fabs(mb.tail(1)[0]) < 1e-4 && std::fabs(mb.tail(1)[1]) < 1e-4, "mono, −1: B silent — A alone");
        // STEREO OFF (both in both ears) behaves as mono
        Bench so; so.mx.setStereo(false); so.mx.setBalance(-0.5f);
        for (int i = 0; i < settle / blk; i++) so.run({S(0, 7.0e6), S(1, 7.01e6)}, blk, {0, 0}, {0.0f, 0.25f});
        ok(std::fabs(so.tail(1)[0] - 0.25f * h) < 2e-3 && std::fabs(so.tail(1)[1] - 0.25f * h) < 2e-3, "stereo off, −0.5: B in both ears, turned down");
        // THE RAMP: a balance change is a 400 ms raised cosine — half way it is part way, and no step; re-aimed mid-glide too
        Bench r;
        for (int i = 0; i < settle / blk; i++) r.run({S(0, 7.0e6), S(1, 7.01e6)}, blk, {0, 0}, {0.25f, 0.0f});
        r.mx.setBalance(1.0f);
        const int half = (int)(kRampSec * kOutRate / 2);
        r.run({S(0, 7.0e6), S(1, 7.01e6)}, half, {0, 0}, {0.25f, 0.0f});
        const float lHalf = r.tail(1)[0];
        ok(lHalf > 0.05f && lHalf < side - 0.05f, "a full lean glides (half way: part way down), not a cut");
        r.mx.setBalance(-0.5f);   // re-aimed mid-glide: from where it is
        for (int i = 0; i < settle / blk; i++) r.run({S(0, 7.0e6), S(1, 7.01e6)}, blk, {0, 0}, {0.25f, 0.0f});
        ok(r.maxStep(settle + half) < 0.25 * 1.41421356 * 3.0 / (kRampSec * kOutRate), "…no step anywhere, re-aimed mid-glide included");
        ok(std::fabs(r.tail(1)[0] - side) < 2e-3, "…and lands: A back at its own level in its own ear");
        // A VFO ALONE has no claim on the balance: B muted, balance leaning to B — A stays at unity, centred
        Bench al; al.mx.setBalance(0.9f);
        auto SM = [](int key, double hz, bool muted) { PanMixer::Src x = S(key, hz); x.muted = muted; return x; };
        for (int i = 0; i < settle / blk; i++) al.run({S(0, 7.0e6), SM(1, 7.01e6, true)}, blk, {0, 0}, {0.25f, 0.25f});
        ok(std::fabs(al.tail(1)[0] - 0.25f) < 2e-3 && std::fabs(al.tail(1)[1] - 0.25f) < 2e-3, "B muted, balance +0.9: A alone at unity (not turned down)");
        // B removed with A leaned away: A comes home to unity and the hand-back happens
        Bench rm; rm.mx.setBalance(0.5f);
        for (int i = 0; i < settle / blk; i++) rm.run({S(0, 7.0e6), S(1, 7.01e6)}, blk, {0, 0}, {0.25f, 0.0f});
        for (int i = 0; i < settle / blk; i++) rm.run({S(0, 7.0e6), S(1, 7.01e6, false)}, blk, {0, 0}, {0.25f, 0.0f});
        ok(rm.mx.restingMono() && std::fabs(rm.tail(1)[0] - 0.25f) < 1e-4, "B removed while leaned away from A: A home at unity, hand-back with no step");
    }

    std::printf("── 11. the RF centre for two VFOs (fitCentre) ──\n");
    {
        const double lim = 955e3, clr = 20e3;
        CentreFit f = fitCentre(7.0e6, 7.0e6, lim, 7.5e6, lim, clr);
        ok(f.ok && !f.moved && f.centre == 7.0e6, "both inside: the radio stays put");
        f = fitCentre(7.0e6, 7.0e6, lim, 8.2e6, lim, clr);
        ok(f.ok && f.moved && std::fabs(f.centre - 7.6e6) < 1, "B 1.2 MHz away: the centre moves to the middle of the room both allow");
        ok(std::fabs(f.centre - 7.0e6) <= lim && std::fabs(f.centre - 8.2e6) <= lim, "…where both are inside their limits");
        f = fitCentre(7.0e6, 7.0e6, lim, 9.0e6, lim, clr);
        ok(!f.ok && f.centre == 7.0e6, "2 MHz apart (> 2 × lim): refused, nothing moves");
        // close together, centre far away: the middle would put DC between two narrow passbands 10 kHz apart
        f = fitCentre(8.0e6, 7.000e6, lim, 7.010e6, lim, clr);
        ok(f.ok && f.moved && std::fabs(f.centre - 7.000e6) >= clr && std::fabs(f.centre - 7.010e6) >= clr,
           "close VFOs: the centre lands clear of both passbands (no DC spike in either)");
        ok(std::fabs(f.centre - 7.000e6) <= lim && std::fabs(f.centre - 7.010e6) <= lim, "…still inside both limits");
        f = fitCentre(0, 7.0e6, lim, 7.0e6 + 2 * lim, lim, clr);
        ok(f.ok && std::fabs(f.centre - (7.0e6 + lim)) < 1, "exactly 2 × lim apart: the one centre that holds both");
    }

    std::printf("── 11b. …and the DC is PHYSICAL: logical centre + the tuner's offset, out of BOTH passbands (V12 beta24) ──\n");
    {
        // ★ A dongle: the spike 15 kHz above the logical centre. A on AM 7.000 MHz (±5 kHz), B on USB.
        const double lim = 955e3, clr = 20e3, off = 15000;
        const vibe::VfoDcZone za = vibe::vfoDcZone(7.000e6, -5000, 5000, off);
        auto zb = [&](double bHz) { return vibe::vfoDcZone(bHz, 0, 2700, off); };
        auto clearOf = [&](double c, const vibe::VfoDcZone& z) { const double d = c + off; return d <= z.lo || d >= z.hi; };
        // BEFORE (no offset given): B tuned to 7.015 MHz with the centre on A — both fit, nothing moves, and the
        // spike (7.015) sits on B's carrier. The fault the brief names.
        CentreFit f = fitCentre(7.000e6, 7.000e6, lim, 7.015e6, lim, clr);
        ok(f.ok && !f.moved && !clearOf(f.centre, zb(7.015e6)), "before: B tuned onto the spike (centre on A + 15 kHz) — fitCentre left it there");
        // AFTER: the same tune moves the radio, the smallest move, the DC out of B's zone AND A's
        f = fitCentre(7.000e6, 7.000e6, lim, 7.015e6, lim, clr, off, za, zb(7.015e6));
        ok(f.ok && f.moved && clearOf(f.centre, zb(7.015e6)) && clearOf(f.centre, za),
           ("after: the DC leaves B's passband and stays off A's (centre " + std::to_string(f.centre) + ", DC " + std::to_string(f.centre + off) + ")").c_str());
        ok(std::fabs(f.centre - 7.000e6) < 20e3, "…by a few kHz, not a recentre (the view barely moves)");
        // the hold: the DC goes kVfoDcHoldMinHz beyond the zone's edge, so B nudged 100 Hz does not ask again
        const CentreFit g = fitCentre(f.centre, 7.000e6, lim, 7.0151e6, lim, clr, off, za, zb(7.0151e6));
        ok(g.ok && !g.moved, "B nudged 100 Hz: no second move (the hysteresis)");
        // A's own zone: B tuned so that the CURRENT centre's DC is on A (the centre had been moved off A for some reason)
        f = fitCentre(6.985e6, 7.000e6, lim, 7.300e6, lim, clr, off, za, zb(7.300e6));
        ok(f.ok && f.moved && clearOf(f.centre, za) && clearOf(f.centre, zb(7.300e6)), "a centre whose DC is on A is moved off it too");
        // a decisive move (B far): clear of both by `clr`, measured from the PHYSICAL DC
        // B on 7.010 USB: its zone 7.0075–7.0152 MHz holds the DC at 7.015 — moved, though both VFOs fit
        f = fitCentre(7.0e6, 7.000e6, lim, 7.010e6, lim, clr, off, za, zb(7.010e6));
        ok(f.ok && f.moved && clearOf(f.centre, za) && clearOf(f.centre, zb(7.010e6)), "B's USB passband + guard reaching the DC: moved, though both fit");
        // B on 7.020 USB: the DC (7.015) is 2.5 kHz below B's carrier — outside its zone (7.0175–7.0252): nothing moves
        f = fitCentre(7.0e6, 7.000e6, lim, 7.020e6, lim, clr, off, za, zb(7.020e6));
        ok(f.ok && !f.moved, "B on 7.020 USB, the DC below its zone: the radio stays put");
        f = fitCentre(8.0e6, 7.000e6, lim, 7.010e6, lim, clr, off, za, zb(7.010e6));
        ok(f.ok && f.moved && std::fabs(f.centre + off - 7.000e6) >= clr && std::fabs(f.centre + off - 7.010e6) >= clr
           && clearOf(f.centre, za) && clearOf(f.centre, zb(7.010e6)), "a decisive move keeps the PHYSICAL DC clr from both and out of both zones");
        // WFM-like width on A (the zone shrinks to ±off, never stricter than the locked VFO): A's own lock-step offset is fine
        const vibe::VfoDcZone wide = vibe::vfoDcZone(7.000e6, -100e3, 100e3, off);
        f = fitCentre(7.000e6, 7.000e6, lim, 7.300e6, lim, clr, off, wide, zb(7.300e6));
        ok(f.ok && !f.moved, "a wide A: the spike at the locked VFO's own 15 kHz is not chased (never stricter than locked)");
        // no offset (an HF+, a SpyServer): exactly the old answers
        const CentreFit o1 = fitCentre(8.0e6, 7.000e6, lim, 7.010e6, lim, clr), o2 = fitCentre(8.0e6, 7.000e6, lim, 7.010e6, lim, clr, 0.0, za, zb(7.010e6));
        ok(o1.centre == o2.centre && o1.moved == o2.moved, "no offset: the old placement, unchanged");
    }

    std::printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
