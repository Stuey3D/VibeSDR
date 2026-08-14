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
#include <random>
#include <algorithm>
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
    // ★ Pilot injection as a fraction of full deviation. 0.10 is the textbook 7.5 kHz; real
    //   stations under-inject, and 0.063 is the 4.7 kHz measured on air at 102.3.
    double pilot = 0.10;
    // ★★★ PURE STATIC — no carrier at all, only noise. This is the case that must NEVER produce
    //     stereo, and the reason the lock threshold cannot simply be lowered.
    bool   staticOnly = false;
    double noise = 0.0;
    std::mt19937 rng{7};
    std::normal_distribution<double> gauss{0.0, 1.0};
    void fill(std::vector<cf32>& out, int n) {
        out.resize((size_t)n);
        for (int i = 0; i < n; ++i) {
            const double t = tAcc;
            const double L = std::sin(2.0 * M_PI * kAudioHz * t), R = 0.0;
            // 45% sum, 10% pilot, 45% difference on the 38 kHz subcarrier — the standard budget.
            const double mpx = 0.45 * (L + R)
                             + pilot * std::sin(2.0 * M_PI * kPilotHz * t)
                             + 0.45 * (L - R) * std::sin(2.0 * M_PI * 2.0 * kPilotHz * t);
            phase += 2.0 * M_PI * kDevHz * mpx / kFs;
            if (phase >  M_PI * 1e6) phase -= M_PI * 2e6;   // keep it bounded
            if (staticOnly) {
                // Nothing but receiver noise: no carrier, no pilot, no programme.
                out[(size_t)i] = cf32{ (float)gauss(rng), (float)gauss(rng) };
                tAcc += 1.0 / kFs;
                continue;
            }
            double re = std::cos(phase), im = std::sin(phase);
            if (noise > 0.0) { re += noise * gauss(rng); im += noise * gauss(rng); }
            out[(size_t)i] = cf32{ (float)re, (float)im };
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

    // ── An RDS RESYNC MUST NOT TAKE THE STEREO WITH IT ───────────────────────────────────────
    // ★★★ IT DID, FOR AS LONG AS THE FEATURE HAS EXISTED. The resync called pll_.configure(), which
    //     resets phase, the loop integrator, lockAmp and lockState — so a perfectly good pilot lock
    //     was destroyed and had to re-acquire, and the listener heard stereo drop and return along
    //     with the RDS data (Stuart, 2026-08-14: "stereo always dropped when switching between
    //     standard and advanced RDS too"). A resync is asked for on every retune and whenever a
    //     decoder is attached or detached — i.e. constantly, while somebody is listening.
    // ★★ The bit clock is all RDS needed re-timing, and that is a counter, not the loop.
    {
        Sink s4;
        RxPipeline::Callbacks cb4{};
        cb4.ctx = &s4; cb4.stereo = &Sink::onStereo; cb4.audio = &Sink::onAudio;
        MpxGen g4;
        RxPipeline rx4;
        rx4.start(kFs, 1024, 10.0, 48000, cb4);
        rx4.setTune(0.0, RxPipeline::Mode::WFM, 250000.0);
        run(rx4, g4, 2.0);
        const int before = s4.transitions;
        for (int i = 0; i < 5; ++i) {          // as if the panel were opened and closed five times
            rx4.requestRdsResync();
            run(rx4, g4, 0.3);
        }
        std::printf("   .. five RDS resyncs: stereo transitions %d -> %d\n", before, s4.transitions);
        ok(s4.transitions == before,
           "★★★ an RDS RESYNC does not drop the stereo lock — it only re-times the bit clock",
           "stereo changed state " + std::to_string(s4.transitions - before) + " time(s)");
    }

    // ── TUNE AWAY AND BACK: THE PILOT MUST RE-ACQUIRE ────────────────────────────────────────
    // ★★★ THE REGRESSION 3.0.0-92 SHIPPED. Removing pll_.configure() from the RDS resync stopped
    //     it tearing down a good lock when a decoder was attached — but that ONE CALL was doing TWO
    //     jobs, and the second was essential: re-acquiring after a RETUNE. Moving the NCO steps the
    //     whole chain, and this PLL is second-order with a loop bandwidth of 1% of the pilot
    //     frequency, so its integrator can be kicked outside the pull-in range and never return.
    //     The pilot sits plainly visible in the MPX while the loop fails to find it: "RDS worked
    //     when I first switched to advanced, then I tuned away and tuned back and it never came
    //     back" (Stuart, 2026-08-14).
    // ★★ A call whose comment describes one of its two purposes is a trap for whoever removes it.
    {
        Sink s6;
        RxPipeline::Callbacks cb6{};
        cb6.ctx = &s6; cb6.stereo = &Sink::onStereo; cb6.audio = &Sink::onAudio;
        MpxGen g6;
        RxPipeline rx6;
        rx6.start(kFs, 1024, 10.0, 48000, cb6);
        rx6.setTune(0.0, RxPipeline::Mode::WFM, 250000.0);
        run(rx6, g6, 2.0);
        const bool lockedFirst = rx6.pilotLockAmp() > 0.042f;
        // Tune away (a same-chain retune — the NCO moves, nothing rebuilds), then back.
        rx6.setTune(300000.0, RxPipeline::Mode::WFM, 250000.0);
        run(rx6, g6, 1.0);
        rx6.setTune(0.0, RxPipeline::Mode::WFM, 250000.0);
        run(rx6, g6, 3.0);
        std::printf("   .. tune away and back: lock metric %.4f (engage 0.042), first tune %s\n",
                    rx6.pilotLockAmp(), lockedFirst ? "locked" : "NOT locked");
        ok(lockedFirst, "the first tune locks at all");
        ok(rx6.pilotLockAmp() > 0.042f,
           "★★★ AND IT RE-ACQUIRES AFTER A RETUNE — the half of the old call that mattered",
           "lock metric " + std::to_string(rx6.pilotLockAmp()));
    }

    // ── OPENING AND CLOSING ADVANCED RDS MUST NOT TOUCH THE AUDIO ────────────────────────────
    // ★★★ THE SECOND CAUSE, AND THE ONE THAT SURVIVED THE FIRST FIX. setRdsNoiseCorrection() set
    //     dirty_ = true, which rebuilds the WHOLE audio chain — filters, AGC, pilot PLL — and the
    //     shim calls it every time the Advanced RDS panel opens or closes, because the panel being
    //     open IS the switch. So 3.0.0-92 fixed the resync path and the symptom remained, exactly
    //     as Stuart reported it against that build. Two independent causes, one symptom; fixing
    //     the first proved nothing about the second, which is why this test exercises the USER'S
    //     ACTION rather than the internal call the last one happened to use.
    {
        Sink s5;
        RxPipeline::Callbacks cb5{};
        cb5.ctx = &s5; cb5.stereo = &Sink::onStereo; cb5.audio = &Sink::onAudio;
        MpxGen g5;
        RxPipeline rx5;
        rx5.start(kFs, 1024, 10.0, 48000, cb5);
        rx5.setTune(0.0, RxPipeline::Mode::WFM, 250000.0);
        run(rx5, g5, 2.0);
        const int before5 = s5.transitions;
        const unsigned rb0 = rx5.rebuildCount();
        for (int i = 0; i < 4; ++i) {          // open, close, open, close
            rx5.setRdsNoiseCorrection(true);  run(rx5, g5, 0.3);
            rx5.setRdsNoiseCorrection(false); run(rx5, g5, 0.3);
        }
        std::printf("   .. RDS panel toggled 8x: stereo transitions %d -> %d, rebuilds %u -> %u\n",
                    before5, s5.transitions, rb0, rx5.rebuildCount());
        ok(s5.transitions == before5,
           "★★★ opening/closing ADVANCED RDS does not drop the stereo lock",
           "stereo changed state " + std::to_string(s5.transitions - before5) + " time(s)");
        // ★ And the reason, asserted directly: no audio chain was rebuilt. Asserting the CAUSE as
        //   well as the effect means a future change that reintroduces the rebuild fails here even
        //   if the lock happens to survive it on a strong test signal.
        ok(rx5.rebuildCount() == rb0,
           "★★ ...and rebuilds NO audio chain — the cause, not just the symptom",
           "rebuilt " + std::to_string(rx5.rebuildCount() - rb0) + " time(s)");
    }

    // ── An UNDER-INJECTED pilot must still lock ──────────────────────────────────────────────
    // ★★★ THE CASE FROM THE AIR. H F M on 102.3 transmits 4.7 kHz of pilot where the spec says
    //     6.0-7.5, which sat a whisker above the old engage threshold (4.5 kHz) — so its stereo
    //     dropped in and out on an otherwise clean 26 dB signal (Stuart, 2026-08-14). Stations do
    //     not always comply, and a receiver that only works with compliant ones is not finished.
    {
        Sink s2;
        RxPipeline::Callbacks cb2{};
        cb2.ctx = &s2; cb2.stereo = &Sink::onStereo; cb2.audio = &Sink::onAudio;
        MpxGen g2; g2.pilot = 0.063;              // 4.7 kHz, as measured on air
        RxPipeline rx2;
        rx2.start(kFs, 1024, 10.0, 48000, cb2);
        rx2.setTune(0.0, RxPipeline::Mode::WFM, 250000.0);
        run(rx2, g2, 3.0);
        ok(s2.sawStereo,
           "★★★ a 4.7 kHz UNDER-INJECTED pilot still locks — real stations are not always compliant",
           "no stereo on a pilot the receiver should manage");
    }

    // ── ...AND PURE STATIC MUST NOT ─────────────────────────────────────────────────────────
    // ★★★ THE ASSERTION THAT GUARDS THE CHANGE ABOVE, AND THE ONE STUART NAMED: "we must not risk
    //     it going back to how it was in the early days and give us stereo on pure static". The
    //     engage threshold was lowered by 30%, which on its own WOULD risk exactly that — it is
    //     paid for by averaging 2.2x longer, because a real pilot correlates coherently (as N)
    //     while noise correlates randomly (as sqrt(N)). If that reasoning is ever wrong, or the
    //     averaging is ever shortened without moving the threshold back, this test is what says so.
    {
        Sink s3;
        RxPipeline::Callbacks cb3{};
        cb3.ctx = &s3; cb3.stereo = &Sink::onStereo; cb3.audio = &Sink::onAudio;
        MpxGen g3; g3.staticOnly = true;          // no carrier, no pilot — receiver noise alone
        RxPipeline rx3;
        rx3.start(kFs, 1024, 10.0, 48000, cb3);
        rx3.setTune(0.0, RxPipeline::Mode::WFM, 250000.0);
        // ★★★ MEASURE THE MARGIN, DO NOT JUST WATCH FOR A FAILURE. A binary "did it lock?" over a
        //     few seconds of synthetic noise passes even with the OLD averaging and the NEW lower
        //     threshold — I checked, and it did — so on its own it proves nothing about why we are
        //     safe. False locks are rare events; a test that waits for one is a test that usually
        //     says nothing. So watch the lock METRIC and record how close it ever came.
        float peak = 0.0f;
        for (int i = 0; i < 40; ++i) {
            run(rx3, g3, 0.4);
            peak = std::max(peak, std::fabs(rx3.pilotLockAmp()));
        }
        const float engage = 0.042f;              // kLockEngage — see the note beside it
        std::printf("   .. static: peak lock metric %.4f against an engage threshold of %.3f"
                    "  (%.1fx margin)\n", peak, engage, peak > 0 ? engage / peak : 99.0f);
        ok(!s3.sawStereo,
           "★★★ PURE STATIC NEVER GIVES STEREO — the guarantee the lower threshold is measured against",
           "stereo declared on noise alone");
        // ★★ AND WITH ROOM TO SPARE. This is the assertion with teeth: shorten the averaging, or
        //    drop the threshold again, and the margin closes measurably long before a false lock
        //    becomes likely enough for a short test to catch one.
        ok(peak < engage * 0.5f,
           "★★★ ...and not even CLOSE — the noise floor of the metric stays well under the threshold",
           "peak " + std::to_string(peak) + " vs engage " + std::to_string(engage));
    }

    std::printf("\n%s%d checks\n", failures ? "FAILURES — " : "", checks);
    if (failures) std::printf("%d FAILED\n", failures);
    return failures ? 1 : 0;
}
