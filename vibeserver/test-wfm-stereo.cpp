// ★★★ DOES A FRESHLY-CONNECTED LISTENER GET STEREO?
//
// The field bug (Stuart, 2026-08-08, Airspy on 96.6 MHz): stereo and RDS never engage on connect,
// and never after a tune — until the listener opens and closes the Advanced RDS box. Measured on
// the Pi, THREE unrelated actions each fix it: toggling rdsx, bouncing the mode, and setting a
// bandwidth. What they share is that each forces a rebuildAudio(); the pilot PLL is configured
// there, so the FIRST build produces a pipeline whose pilot never locks and the second always
// works.
//
// ★★ THIS TEST FEEDS A PERFECT SIGNAL. A synthetic 100% textbook MPX — full-deviation pilot, clean
//    L-R — so a failure can only be the pipeline's own state machine, never signal quality. If
//    stereo does not lock on THIS, no real station will do better.
#include "vibedsp.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using vibedsp::RxPipeline;
using vibedsp::cf32;

static int failures = 0, checks = 0;
static void ok(bool cond, const char* what, const std::string& extra = "") {
    checks++;
    if (cond) { std::printf("   ok   %s\n", what); return; }
    failures++;
    std::printf("   FAIL %s %s\n", what, extra.c_str());
}

namespace {

constexpr double kFs      = 1000000.0;   // IQ rate
constexpr double kDevHz   = 75000.0;     // full FM deviation
constexpr double kPilotHz = 19000.0;
constexpr double kAudioHz = 1000.0;      // the tone we put in L only, so L-R != 0

/** One second of textbook stereo MPX, FM-modulated onto IQ at kFs.
 *  L = tone, R = silence  ->  L+R and L-R are both the tone, so a working decoder MUST see a
 *  pilot AND a difference signal. */
struct MpxGen {
    double phase = 0.0, tAcc = 0.0;
    void fill(std::vector<cf32>& out, int n) {
        out.resize((size_t)n);
        for (int i = 0; i < n; ++i) {
            const double t = tAcc;
            const double L = std::sin(2.0 * M_PI * kAudioHz * t), R = 0.0;
            // 45% sum, 10% pilot, 45% difference on the 38 kHz subcarrier — the standard budget.
            const double mpx = 0.45 * (L + R)
                             + 0.10 * std::sin(2.0 * M_PI * kPilotHz * t)
                             + 0.45 * (L - R) * std::sin(2.0 * M_PI * 2.0 * kPilotHz * t);
            phase += 2.0 * M_PI * kDevHz * mpx / kFs;
            if (phase >  M_PI * 1e6) phase -= M_PI * 2e6;   // keep it bounded
            out[(size_t)i] = cf32{ (float)std::cos(phase), (float)std::sin(phase) };
            tAcc += 1.0 / kFs;
        }
    }
};

struct Sink {
    bool   sawStereo = false;
    int    transitions = 0;
    long   audioSamples = 0;
    double peak = 0.0;
    static void onStereo(void* c, bool locked) {
        auto* s = (Sink*)c;
        s->transitions++;
        if (locked) s->sawStereo = true;
    }
    // ★ Without this the test cannot tell "stereo is broken" from "nothing is demodulating at
    //   all" — and those need completely different fixes.
    static void onAudio(void* c, const float* pcm, int n, int ch, int /*rate*/) {
        n *= std::max(1, ch);
        auto* s = (Sink*)c;
        s->audioSamples += n;
        for (int i = 0; i < n; ++i) s->peak = std::max(s->peak, (double)std::fabs(pcm[i]));
    }
};

/** Feed `seconds` of MPX through the pipeline in realistic blocks. */
void run(RxPipeline& rx, MpxGen& gen, double seconds) {
    const int block = 8192;
    const int total = (int)(kFs * seconds);
    std::vector<cf32> buf;
    for (int done = 0; done < total; done += block) {
        gen.fill(buf, std::min(block, total - done));
        rx.feed(buf.data(), (int)buf.size());
    }
}

}  // namespace

int main() {
    std::printf("\nA listener connects and tunes a stereo station\n");

    Sink sink;
    RxPipeline::Callbacks cb{};
    cb.ctx    = &sink;
    cb.stereo = &Sink::onStereo;
    cb.audio  = &Sink::onAudio;

    MpxGen gen;
    RxPipeline rx;
    rx.start(kFs, 1024, 10.0, 48000, cb);

    // This is precisely what the shim does for a new listener: build the pipeline, then tune it to
    // the station in WFM. setTune() sees a mode change and marks the chain dirty, so the WFM branch
    // of rebuildAudio() — the one that configures the 19 kHz pilot PLL — should run here.
    rx.setTune(0.0, RxPipeline::Mode::WFM, 250000.0);
    run(rx, gen, 2.0);

    std::printf("   .. %ld audio samples, peak %.3f\n", sink.audioSamples, sink.peak);
    ok(sink.sawStereo,
       "★ stereo locks on a perfect signal after the FIRST tune (the field bug)",
       "no stereo in 2 s of full-deviation pilot");

    // ── Now do what the listener has to do by hand: force a SECOND rebuild. ──
    // If this succeeds where the first failed, the signal was never the problem and the first
    // build is the broken one.
    const bool afterFirst = sink.sawStereo;
    rx.setTune(0.0, RxPipeline::Mode::NFM, 50000.0);
    run(rx, gen, 0.2);
    rx.setTune(0.0, RxPipeline::Mode::WFM, 250000.0);
    run(rx, gen, 2.0);

    ok(sink.sawStereo, "stereo locks after a forced rebuild", "not even after a rebuild");

    if (!afterFirst && sink.sawStereo)
        std::printf("\n   ▶ REPRODUCED: the first build never locks, the second does.\n");

    // ★★★ THE FIELD BUG. The shim CLEARS its cached stereo flag on every retune (it has to: the
    //     next station may be mono). It then waits for the pipeline to tell it the state again —
    //     but the report is EDGE-TRIGGERED, and a retune inside the same mode deliberately does
    //     NOT rebuild, so the pilot never unlocks and there is no edge to send. The flag stays
    //     false for ever and the listener sees mono on a station that is plainly in stereo, until
    //     they do something that rebuilds the chain (open/close Advanced RDS, change bandwidth).
    //
    // ★ So the pipeline must be able to RE-ANNOUNCE a state that has not changed.
    std::printf("\nA retune inside WFM — the shim has just cleared its cached flag\n");
    sink.transitions = 0;
    rx.requestStereoReport();
    rx.setTune(1000.0, RxPipeline::Mode::WFM, 250000.0);   // same chain: no rebuild by design
    run(rx, gen, 0.5);

    ok(sink.transitions > 0,
       "★ the pipeline re-announces stereo after a same-chain retune",
       "silent — the listener is stuck in mono until something rebuilds the chain");

    std::printf("\n%s%d checks\n", failures ? "FAILURES — " : "", checks);
    if (failures) std::printf("%d FAILED\n", failures);
    return failures ? 1 : 0;
}
