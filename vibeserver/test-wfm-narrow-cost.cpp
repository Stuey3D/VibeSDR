// ★★★ NARROWING THE WFM PASSBAND MUST NOT MAKE THE DSP DEARER.
//
// The field report (Stuart, Sony Android TV, VibeServer Lite 11.0.0~b4, 32-bit Cortex-A53, RTL at
// 2.048 MS/s, WFM on 99.7 with Advanced RDS open, 2026-09-29): dragging the passband to ±28 kHz gave
// "bursts of fast noisy audio with about a 1 second gap". The server's own log said why:
//
//     dsp load: wide 34-40% of real time (backlog 0 ms)       ← ±100 kHz
//     dsp load: wide 161% ... 276% of real time (backlog 256 ms) ← ±28 kHz, IQ overruns
//
// The cause was WFM's width ladder (chainBand in pipeline.cpp): its bottom band started at 1 kHz,
// and the selectivity filter for a whole band is designed from the band's LOW edge — a 250 Hz
// transition, 5501 taps at a 256 kS/s stage where the band above uses 113. Every WFM width under
// 100 kHz paid it, however it got there (a fresh build or a drag).
//
// ★ RELATIVE, SO IT HOLDS ON ANY MACHINE: CPU time per second of signal at each narrow width, over
//   the same at ±100 kHz, must stay under 1.3. Before the fix it was ~4.7 on a Mac (and ~5-8 on the
//   A53, where the absolute figure is what hurt).
// ★ Both routes to a narrow width are measured: a chain BUILT narrow (a listener arriving with a
//   remembered width) and one DRAGGED narrow from wide (the smooth in-band retune).
// ★ SILENT: the audio callback discards everything; no device is touched.
#include "vibedsp.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

using vibedsp::RxPipeline;
using vibedsp::cf32;

static int failures = 0;
static void ok(bool cond, const std::string& what) {
    std::printf("   %s %s\n", cond ? "ok  " : "FAIL", what.c_str());
    if (!cond) failures++;
}

namespace {
void onAudio(void*, const float*, int, int, int) {}   // ★ silent — nothing leaves
void onPs(void*, uint16_t, const char*) {}
void onExt(void*, const RxPipeline::Callbacks::RdsExt&) {}

/** One second of a broadcast-like FM signal 200 kHz off centre: programme, pilot, and enough
 *  deviation (±75 kHz peak) that the channel filter is genuinely working. */
std::vector<cf32> makeIq(double fs) {
    std::vector<cf32> iq((size_t)fs);
    double ph = 0.0;
    for (size_t i = 0; i < iq.size(); ++i) {
        const double t = (double)i / fs;
        const double mpx = 0.45 * std::sin(2 * M_PI * 1000.0 * t) + 0.3 * std::sin(2 * M_PI * 7000.0 * t)
                         + 0.09 * std::cos(2 * M_PI * 19000.0 * t);
        ph = std::fmod(ph + 2 * M_PI * (200000.0 + 75000.0 * mpx) / fs, 2 * M_PI);
        iq[i] = cf32((float)(0.5 * std::cos(ph)), (float)(0.5 * std::sin(ph)));
    }
    return iq;
}

double cpuNow() { return (double)std::clock() / CLOCKS_PER_SEC; }

/** CPU seconds per second of signal at `bw`, after `from` (0 = a fresh chain built at bw). The
 *  best of `reps` passes, so a scheduler hiccup cannot fail the test. */
double costAt(double fs, const std::vector<cf32>& iq, double from, double bw, bool rdsx, int reps = 3) {
    static std::atomic<bool> wanted{false};
    wanted.store(rdsx);
    RxPipeline rx;
    RxPipeline::Callbacks cb{};
    cb.audio = &onAudio; cb.rdsPs = &onPs; cb.rdsExt = &onExt;
    rx.setRdsExtWantedFlag(&wanted);
    rx.start(fs, 1024, 10.0, 48000, cb);
    const int blk = (int)(fs / 100.0);
    auto second = [&]() { for (int o = 0; o + blk <= (int)iq.size(); o += blk) rx.feed(iq.data() + o, blk); };
    if (from > 0.0) { rx.setTune(200000.0, RxPipeline::Mode::WFM, from); second(); }
    rx.setTune(200000.0, RxPipeline::Mode::WFM, bw);
    second();                                             // settle: rebuild, pilot lock, warm caches
    double best = 1e9;
    for (int r = 0; r < reps; ++r) {
        const double c0 = cpuNow();
        second();
        best = std::min(best, cpuNow() - c0);
    }
    rx.stop();
    return best;
}
}  // namespace

int main() {
    std::printf("WFM DSP cost vs passband (CPU per second of signal, relative to ±100 kHz)\n");
    for (double fs : { 2048000.0, 2400000.0 }) {
        const auto iq = makeIq(fs);
        for (bool rdsx : { false, true }) {
            const double wide = costAt(fs, iq, 0.0, 200000.0, rdsx);
            std::printf("\n  %.3f MS/s, Advanced RDS %s — ±100 kHz costs %.1f%% of real time here\n",
                        fs / 1e6, rdsx ? "open" : "closed", wide * 100.0);
            for (double bw : { 150000.0, 90000.0, 56000.0, 40000.0, 20000.0 }) {
                for (double from : { 0.0, 200000.0, 90000.0 }) {
                    if (from == bw || (from == 90000.0 && bw > 90000.0)) continue;
                    const double c = costAt(fs, iq, from, bw, rdsx);
                    const double ratio = c / std::max(wide, 1e-6);
                    char what[200];
                    std::snprintf(what, sizeof what, "±%.0f kHz %-14s %5.1f%% = %.2fx wide (<= 1.3)", bw / 2000.0,
                                  from == 0.0 ? "(built)" : from == 200000.0 ? "(dragged)" : "(dragged 90k)",
                                  c * 100.0, ratio);
                    ok(ratio <= 1.3, what);
                }
            }
        }
    }
    std::printf("\n%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
