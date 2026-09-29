// ★★★ NO PASSBAND, IN ANY MODE, MAY BUILD A CHAIN DEARER THAN THE WIDTHS AROUND IT.
//
// test-wfm-narrow-cost guards the one cliff that reached the air (WFM narrower than ±50 kHz built a
// 5501-tap filter: 34% -> 161-276% of real time on Stuart's Sony, 2026-09-29). Stuart's question
// the same day: "I assume it was just WFM — check ALL modes." This is that check, for every mode the
// server builds (AM/SAM, USB/LSB, CW, NFM/FM, WFM), from below what any client allows to above it.
//
// What it found (2026-09-29): no other NARROWING cliff — every narrow width is at or under its mode's
// default. But nothing clamped the TOP, and past the sliders the cost is not linear: USB at 500 kHz
// rebuilt its Weaver pair at the full capture rate, 55x its default at 2.4 MS/s; CW 45x; WFM at
// 1 MHz 6x; AM and NFM 6-8x. The per-listener path clamped nothing at all. Hence
// RxPipeline::maxBwHz / clampBwHz, which setTune applies and the shim applies before sizing a
// listener's channel.
//
// ★ THREE RULES, each one a shape of the fault rather than a number for one machine:
//   1. NARROWER IS NOT DEARER. Any width at or under the mode's default costs <= 1.35x the default
//      on the shared pipeline (the WFM cliff was ~4.7x there on a Mac, and the Sony's 5-8x).
//      ★★ KNOWN, MEASURED, NOT FIXED — <= 3x on a PER-LISTENER pipeline. That chain is small
//         (0.07-0.1 % of a Mac core at the default), so the ladder's bottom bands show through:
//         AM 1-6 kHz 1.8x (the [1k, 6k] band's 250 Hz transition, 705 taps) and USB/LSB/CW
//         50-1500 Hz 1.8-2.8x (the [200, 1500] band's 100 Hz transition, 1761 taps, plus a Weaver
//         pair the decimation planner does not cost). CW's own default lives in that band. Fixing
//         it changes the filter SKIRTS a listener hears, so it is Stuart's call, not a quiet edit.
//         The cap here still fails anything shaped like the WFM cliff.
//   2. PAST THE CEILING IS THE CEILING. A width above maxBwHz — or NaN, inf, 0, negative, as a
//      hostile client can send through strtod — builds EXACTLY the ceiling's chain (same channel
//      rate, same tap counts). Deterministic: compared as lengths, not timings.
//   3. WIDER COSTS ROUGHLY IN PROPORTION. Between the default and the ceiling, cost <= 2.5x the
//      default x (width / default). A wider channel needs a faster channel rate, so growth is
//      physics; the 2.5 is the rounding on top of it — the chain is built for the TOP of its
//      ladder band and a listener's channel is a power-of-two slice, each up to 2x.
//   4. THE CHAIN CARRIES THE WIDTH, HOWEVER IT GOT THERE. The selectivity filter's cutoff is capped
//      at 0.45 of the channel rate, so the channel must be at least the passband's edge / 0.45 —
//      built fresh AND dragged. WFM broke this: its channel rate was sized from the width it was
//      BUILT at, not the band top, so ±55 kHz dragged back out to ±100 kHz kept a 146 kHz channel —
//      ±66 kHz of passband while the listener was shown ±100 kHz, until something rebuilt it.
//      (Not "dragged == built": inside a band the planner's early stages may differ with the width
//      it was built at, which is harmless. Carrying the width is what matters.)
//   The "(>2x)" flag is Stuart's rule of thumb, reported, not enforced: the widest slider settings
//   of AM, NFM, SSB and WFM cost 2-4x their defaults (NFM per-listener 10x at ±30 kHz, where its
//   default is nearly free), in proportion to the width they carry.
// ★ Both routes: a chain BUILT at the width (a listener arriving with it remembered) and one
//   DRAGGED there from the default (the smooth in-band retune). Both host paths: the shared
//   pipeline at the capture rate, and a per-listener pipeline at the channel rate the shim would
//   cut for that width (chanBinsFor: a power-of-two slice >= max(2.5 x bw, 24 kHz)).
// ★ SILENT: the audio callback discards everything; no device is touched.
#include "vibedsp.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <random>
#include <string>
#include <vector>

using vibedsp::RxPipeline;
using vibedsp::cf32;
using M = RxPipeline::Mode;

static int failures = 0;
static void ok(bool cond, const std::string& what) {
    if (!cond) { std::printf("   FAIL %s\n", what.c_str()); failures++; }
}

namespace {
void onAudio(void*, const float*, int, int, int) {}   // ★ silent — nothing leaves
void onPs(void*, uint16_t, const char*) {}

/** One second of signal at `off` Hz: a broadcast-like FM carrier for WFM (±75 kHz deviation, so the
 *  channel filter is genuinely working), a carrier plus noise for everything else. */
std::vector<cf32> makeIq(double fs, double off, bool wfm) {
    std::vector<cf32> iq((size_t)fs);
    std::mt19937 rng(7);
    std::normal_distribution<float> n(0.0f, 0.03f);
    double ph = 0.0;
    for (size_t i = 0; i < iq.size(); ++i) {
        const double t = (double)i / fs;
        double f = off + 1000.0;
        if (wfm) f = off + 75000.0 * (0.45 * std::sin(2 * M_PI * 1000.0 * t) + 0.3 * std::sin(2 * M_PI * 7000.0 * t)
                                     + 0.09 * std::cos(2 * M_PI * 19000.0 * t));
        ph = std::fmod(ph + 2 * M_PI * f / fs, 2 * M_PI);
        iq[i] = cf32((float)(0.4 * std::cos(ph)) + n(rng), (float)(0.4 * std::sin(ph)) + n(rng));
    }
    return iq;
}

double cpuNow() { return (double)std::clock() / CLOCKS_PER_SEC; }

struct Result { double cpu; RxPipeline::ChainShape shape; };

/** CPU seconds per second of signal at `bw` (best of three), and the chain it built. `from` > 0
 *  builds at `from` first and drags. */
Result costAt(double fs, const std::vector<cf32>& iq, double off, M mode, double from, double bw) {
    static std::atomic<bool> rdsx{false};
    RxPipeline rx;
    RxPipeline::Callbacks cb{};
    cb.audio = &onAudio; cb.rdsPs = &onPs;
    rx.setRdsExtWantedFlag(&rdsx);
    rx.start(fs, 1024, 10.0, 48000, cb);
    const int blk = std::max(64, (int)(fs / 100.0));
    auto second = [&]() { for (int o = 0; o + blk <= (int)iq.size(); o += blk) rx.feed(iq.data() + o, blk); };
    if (from > 0.0) { rx.setTune(off, mode, from); second(); }
    rx.setTune(off, mode, bw);
    second();                                             // settle: rebuild, locks, warm caches
    double best = 1e9;
    for (int r = 0; r < 3; ++r) { const double c0 = cpuNow(); second(); best = std::min(best, cpuNow() - c0); }
    Result res{best, rx.chainShape()};
    rx.stop();
    return res;
}

/** The chain a drag from `from` to `bw` leaves behind — shape only, no timing, so it is cheap. */
RxPipeline::ChainShape shapeAfterDrag(double fs, const std::vector<cf32>& iq, double off, M mode, double from, double bw) {
    static std::atomic<bool> rdsx{false};
    RxPipeline rx;
    RxPipeline::Callbacks cb{};
    cb.audio = &onAudio; cb.rdsPs = &onPs;
    rx.setRdsExtWantedFlag(&rdsx);
    rx.start(fs, 1024, 10.0, 48000, cb);
    const int blk = std::max(64, (int)(fs / 100.0));
    auto tenth = [&]() { for (int o = 0; o + blk <= (int)iq.size() / 10; o += blk) rx.feed(iq.data() + o, blk); };
    rx.setTune(off, mode, from); tenth();
    rx.setTune(off, mode, bw);   tenth();
    const auto s = rx.chainShape();
    rx.stop();
    return s;
}

/** The per-listener channel rate the shim cuts for this width (chanBinsFor, after clampBwHz). */
double listenerRate(double fs, M mode, double bw) {
    const double need = std::max(RxPipeline::clampBwHz(mode, bw) * 2.5, 24000.0);
    double r = fs;
    while (r / 2.0 >= need && r > 16000.0) r /= 2.0;
    return r;
}

bool sameShape(const RxPipeline::ChainShape& a, const RxPipeline::ChainShape& b) {
    return a.chFs == b.chFs && a.stages == b.stages && a.chanTaps == b.chanTaps &&
           a.lastTaps == b.lastTaps && a.weaverTaps == b.weaverTaps;
}

std::string wstr(double w) {
    char b[32];
    if (std::isnan(w))       std::snprintf(b, sizeof b, "NaN");
    else if (std::isinf(w))  std::snprintf(b, sizeof b, "inf");
    else                     std::snprintf(b, sizeof b, "%.0f", w);
    return b;
}
}  // namespace

int main() {
    const double NaN = std::numeric_limits<double>::quiet_NaN();
    const double Inf = std::numeric_limits<double>::infinity();
    struct Md { const char* name; M mode; double def; std::vector<double> widths; };
    // ★ Defaults are the server's (paramsFor in local_sdr_shim.cpp). Widths: hostile-narrow, each
    //   ladder edge either side, the client sliders' maxima (web BW_EDGE_MAX x2), the ceiling, and
    //   hostile-wide including what strtod will hand over for "nan"/"inf".
    const std::vector<Md> modes = {
        { "AM",  M::AM,      10000,  { 1, 100, 999, 1000, 3000, 6000, 12000, 24000, 40000, 48000,
                                       60000, 500000, 1600000, NaN, Inf, 0, -5000 } },
        { "USB", M::SSB_USB, 2700,   { 1, 50, 199, 200, 500, 1500, 3000, 6000, 12000,
                                       24000, 100000, 500000, 1600000, NaN, Inf, 0 } },
        { "CW",  M::CW,      1200,   { 1, 50, 200, 400, 1500, 4000, 12000, 100000, 500000, NaN } },
        { "NFM", M::NFM,     12500,  { 1, 1000, 4000, 8000, 16000, 32000, 60000, 64000,
                                       100000, 500000, 1600000, NaN, Inf, 0 } },
        { "WFM", M::WFM,     200000, { 1, 20000, 40000, 56000, 100000, 150000, 200000, 250000, 400000,
                                       500000, 800000, 1000000, 1600000, NaN, Inf, 0 } },
    };
    struct Host { const char* name; double fs; bool perListener; };
    const std::vector<Host> hosts = { { "shared 2.048 MS/s", 2048000.0, false },
                                      { "shared 2.4 MS/s",   2400000.0, false },
                                      { "per-listener @ 2.048 MS/s", 2048000.0, true } };

    std::printf("DSP cost vs passband, every mode (CPU per second of signal, relative to the mode's default)\n");
    std::printf("  ratio = worse of built / dragged;  taps = channel cascade (last stage) + Weaver pair\n");
    for (const auto& h : hosts) {
        std::printf("\n══ %s ══\n", h.name);
        for (const auto& md : modes) {
            const bool wfm = md.mode == M::WFM;
            // IQ per input rate, generated once.
            std::vector<std::pair<double, std::vector<cf32>>> cache;
            auto iqFor = [&](double fs) -> const std::vector<cf32>& {
                for (auto& c : cache) if (c.first == fs) return c.second;
                cache.emplace_back(fs, makeIq(fs, h.perListener ? 0.0 : 200000.0, wfm));
                return cache.back().second;
            };
            const double off = h.perListener ? 0.0 : 200000.0;
            auto run = [&](double from, double bw) {
                const double fs = h.perListener ? listenerRate(h.fs, md.mode, bw) : h.fs;
                return costAt(fs, iqFor(fs), off, md.mode, from, bw);
            };
            const double ceil = RxPipeline::maxBwHz(md.mode);
            const Result base = run(0.0, md.def);
            const Result top  = run(0.0, ceil);
            std::printf("\n  %s — default %s Hz: %.2f%% of real time; ceiling %s Hz: %.2fx\n", md.name,
                        wstr(md.def).c_str(), base.cpu * 100.0, wstr(ceil).c_str(), top.cpu / base.cpu);
            for (double w : md.widths) {
                const Result b = run(0.0, w);
                const Result d = run(md.def, w);
                const double ratio = std::max(b.cpu, d.cpu) / std::max(base.cpu, 1e-9);
                const double eff = RxPipeline::clampBwHz(md.mode, w);
                std::printf("    %-4s %9s Hz -> %7s  chFs %8.0f  taps %5d (%4d) + 2x%-5d  %5.2fx%s\n",
                            md.name, wstr(w).c_str(), wstr(eff).c_str(), b.shape.chFs, b.shape.chanTaps,
                            b.shape.lastTaps, b.shape.weaverTaps, ratio, ratio > 2.0 ? "  (>2x)" : "");
                const std::string tag = std::string(h.name) + " " + md.name + " " + wstr(w) + " Hz";
                {   // rule 4
                    const bool ssbLike = md.mode == M::SSB_USB || md.mode == M::SSB_LSB || md.mode == M::CW;
                    const double edge = ssbLike ? eff : eff * 0.5;
                    char m[200];
                    std::snprintf(m, sizeof m, ": the chain carries ±%.0f Hz (built %.0f, dragged %.0f >= %.0f)",
                                  edge, 0.45 * b.shape.chFs, 0.45 * d.shape.chFs, edge);
                    ok(edge <= 0.45 * b.shape.chFs + 1.0 && edge <= 0.45 * d.shape.chFs + 1.0, tag + m);
                    // ★ …and DRAGGED OUT from half the width — the route that found the WFM fault (built
                    //   low in a band, then widened to its top without a rebuild).
                    if (w > 0.0 && std::isfinite(w)) {
                        const double fs = h.perListener ? listenerRate(h.fs, md.mode, w) : h.fs;
                        const auto u = shapeAfterDrag(fs, iqFor(fs), off, md.mode, w * 0.55, w);
                        std::snprintf(m, sizeof m, ": dragged out from %.0f Hz, the chain carries ±%.0f Hz (%.0f)",
                                      w * 0.55, edge, 0.45 * u.chFs);
                        ok(edge <= 0.45 * u.chFs + 1.0, tag + m);
                    }
                }
                if (!(w > 0.0) || !std::isfinite(w) || w >= ceil) {
                    ok(sameShape(b.shape, top.shape) && sameShape(d.shape, top.shape),
                       tag + ": builds the ceiling's chain");
                    ok(ratio <= 1.3 * top.cpu / base.cpu + 0.15, tag + ": costs what the ceiling costs");
                } else if (eff <= md.def) {
                    const double allow = h.perListener ? 3.0 : 1.35;   // see rule 1
                    char m[160]; std::snprintf(m, sizeof m, ": narrower is not dearer (%.2fx <= %.2f)", ratio, allow);
                    ok(ratio <= allow, tag + m);
                } else {
                    const double allow = 2.5 * eff / md.def;
                    char m[160]; std::snprintf(m, sizeof m, ": grows no faster than the width (%.2fx <= %.2f)", ratio, allow);
                    ok(ratio <= allow, tag + m);
                }
            }
        }
    }
    std::printf("\n%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
