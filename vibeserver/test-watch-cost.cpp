// test-watch-cost.cpp — what a VFO B COSTS (Stuart: "VibeSDR and VibeServer is heavily optimised to run on super low end
// hardware, so I want to preserve that"). Measured, per mode, at 2.4 MS/s (an RTL's usual rate) on THIS machine:
//   · ONE-PIPELINE radio: VFO A = the shared RxPipeline at the full rate; VFO B = one more full-rate tune-and-decimate
//     (vibewatch::WatchChannel fed the whole capture, as FullRateFeeder does).
//   · RANGE-LOCKED radio: VFO A = a listener's slice of the shared forward FFT + a channel-rate pipeline; VFO B = one more
//     slice + one more channel-rate pipeline. The forward FFT is shared and paid once, however many VFOs.
//   · The mixer itself (two sources → L/R, per output sample).
// Reported as % of one core in real time. The checks are the SHAPE, not a machine's numbers: a VFO B costs about one more
// listener (≤ 1.6× VFO A's own chain in each architecture) and the mixer is noise next to either.
// SILENT: no audio device; the audio is discarded.
#include "../android/app/src/main/cpp/vibe_watch.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <random>
#include <vector>

using namespace vibewatch;
static int pass = 0, fail = 0;
static void ok(bool c, const char* what) { (c ? pass : fail)++; std::printf("   %s %s\n", c ? "ok  " : "FAIL", what); }
static double cpuNow() { timespec t{}; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void sink(void*, const float*, int, int, int) {}

int main() {
    const double fs = 2.4e6; const int fft = 16384; const double secs = 4.0;
    const int n = (int)(fs * secs), blk = 120000;
    std::vector<cf32> iq((size_t)n);
    std::mt19937 rng(3); std::normal_distribution<float> nd(0.0f, 0.02f);
    for (int i = 0; i < n; i++) {
        const double t = i / fs;
        iq[i] = cf32(0.2f * (float)std::cos(2 * M_PI * 50000 * t) + nd(rng), 0.2f * (float)std::sin(2 * M_PI * 50000 * t) + nd(rng));
    }
    struct M { const char* name; Mode m; double bw; };
    const M modes[] = { {"AM", Mode::AM, 10000}, {"USB", Mode::SSB_USB, 2700}, {"NFM", Mode::NFM, 12500}, {"WFM", Mode::WFM, 200000} };
    std::printf("── cost at %.1f MS/s, %% of one core in real time ──\n", fs / 1e6);
    std::printf("   %-4s  %-28s %-28s\n", "", "one-pipeline radio", "range-locked radio (per listener)");
    std::printf("   %-4s  %8s %8s %8s   %8s %8s %8s\n", "mode", "A", "+B", "B/A", "A", "+B", "B/A");
    bool shapeOk = true;
    for (const auto& md : modes) {
        // one pipeline: the shared chain at full rate (no spectrum callback — the wide FFT is VFO A's regardless)
        double a1, b1;
        {
            vibedsp::RxPipeline rx; vibedsp::RxPipeline::Callbacks cb{}; cb.audio = sink;
            rx.start(fs, 1024, 1.0, kOutRate, cb); rx.setTune(50000, md.m, md.bw);
            const double c0 = cpuNow();
            for (int i = 0; i + blk <= n; i += blk) rx.feed(iq.data() + i, blk);
            a1 = (cpuNow() - c0) / secs * 100.0;
            WatchChannel w(1); Wanted wd; wd.hz = 50000; wd.rxMode = md.m; wd.bwHz = md.bw; w.want(wd);
            std::vector<float> o(blk / 50);
            const double c1 = cpuNow();
            for (int i = 0; i + blk <= n; i += blk) { w.feed(iq.data() + i, blk, fs, 50000); w.fifo().pop(o.data(), o.size()); }
            b1 = (cpuNow() - c1) / secs * 100.0;
        }
        // range-locked: the shared forward FFT (paid once, not counted per VFO), then per VFO an extract + channel pipeline
        double a2, b2;
        {
            vibedsp::Channelizer ch(fft);
            const double binHz = fs / fft;
            const double need = std::max(md.bw * 2.5, 24000.0);
            int bins = 64; while (bins < fft && fs * bins / fft < need) bins <<= 1;
            const double rate = fs * bins / fft;
            const int cb = (int)std::lround(50000 / binHz); const double resid = 50000 - cb * binHz;
            vibedsp::RxPipeline rx; vibedsp::RxPipeline::Callbacks c{}; c.audio = sink;
            rx.start(rate, 1024, 1.0, kOutRate, c); rx.setTune(resid, md.m, md.bw);
            WatchChannel w(1); Wanted wd; wd.hz = 50000; wd.rxMode = md.m; wd.bwHz = md.bw; w.want(wd);
            vibedsp::Channelizer::ExtractCtx ea, eb;
            std::vector<cf32> sa((size_t)bins), sb((size_t)bins);
            std::vector<float> o(4096);
            double ta = 0, tb = 0;
            for (int i = 0; i + blk <= n; i += blk)
                ch.feed(iq.data() + i, blk, [&](const cf32* b, int) {
                    double t0 = cpuNow();
                    int g = ch.extract(b, cb, bins, sa.data(), ea, ch.blockIndex());
                    if (g > 0) rx.feed(sa.data(), g);
                    double t1 = cpuNow();
                    g = ch.extract(b, cb, bins, sb.data(), eb, ch.blockIndex());
                    if (g > 0) { w.feed(sb.data(), g, rate, resid); w.fifo().pop(o.data(), std::min<size_t>(o.size(), (size_t)g * kOutRate / (size_t)rate + 1)); }
                    tb += cpuNow() - t1; ta += t1 - t0;
                });
            a2 = ta / secs * 100.0; b2 = tb / secs * 100.0;
        }
        std::printf("   %-4s  %7.2f%% %7.2f%% %7.2fx   %7.3f%% %7.3f%% %7.2fx\n", md.name, a1, b1, b1 / a1, a2, b2, b2 / a2);
        if (b1 > 1.6 * a1 || b2 > 1.6 * a2) shapeOk = false;
    }
    ok(shapeOk, "a VFO B costs about one more listener: ≤ 1.6× VFO A's own chain, every mode, both architectures");
    // the mixer
    {
        PanMixer mx; std::vector<float> a(480, 0.1f), b(480, 0.1f), out(960);
        const int blocks = 20000;   // 200 s of audio
        const double c0 = cpuNow();
        for (int i = 0; i < blocks; i++) {
            PanMixer::Src s0; s0.key = 0; s0.hz = 7.0e6 + (i % 400 < 200 ? 0 : 20000); s0.pcm = a.data();
            PanMixer::Src s1; s1.key = 1; s1.hz = 7.01e6; s1.pcm = b.data();
            mx.mix({s0, s1}, 480, out.data());
        }
        const double pct = (cpuNow() - c0) / (blocks * 0.01) * 100.0;
        std::printf("   mixer (two sources, swapping every 2 s): %.3f%% of a core\n", pct);
        ok(pct < 1.0, "the mixer is noise next to either chain (< 1% of a core here)");
    }
    std::printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
