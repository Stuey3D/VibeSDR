// test-stereo-highblend.cpp — the stereo hiss cure, and the promise that it leaves good signals alone.
//
// ★★★ WHAT THIS IS FOR. On a weak-but-locked station the decoder gave FULL stereo, and FM noise is
//     triangular — its power rises with the square of frequency — so the L-R subcarrier at 38 kHz
//     sits exactly where the noise is worst. That is why a hissy station hisses in stereo and goes
//     quiet in mono (Stuart, 2026-08-14, 107.4 MHz at S8: pilot nominal, RDS not measurable,
//     constellation "noisy · 135% scatter"). High-blend rolls the TOP off L-R in proportion to the
//     measured noise, keeping the bass and mid separation where the ear actually localises.
//
// ★★★ THE TWO ASSERTIONS ARE OF EQUAL WEIGHT, AND THE SECOND IS THE ONE THAT WILL CATCH A
//     REGRESSION. "It narrows a noisy signal" is what the feature is for; "it does NOT touch a
//     clean one" is what stops it being a tone control that quietly dulls every station on the
//     band. A high-blend that always engages would measure as a success on the first test alone.
//
// ★★ AND THE SIGNAL HERE IS SYNTHETIC, WHICH THIS PROJECT HAS BEEN BITTEN BY TWICE — a synthetic
//    test agreed with the MSF bug and again with the WWV one. So this deliberately does NOT assert
//    an exact corner frequency or dB figure, which would only be re-deriving the implementation's
//    own arithmetic and would "pass" against a wrong curve. It asserts the DIRECTION and the
//    ORDERING: clean stays open, noisy narrows, and noisier narrows further than noisy.

#include "../android/app/src/main/cpp/vibedsp/vibedsp.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using vibedsp::RxPipeline;
using vibedsp::cf32;

static int failures = 0;
static void ok(bool cond, const char* what, const std::string& extra = "") {
    if (cond) { std::printf("   ok   %s\n", what); return; }
    ++failures;
    std::printf("   FAIL %s %s\n", what, extra.c_str());
}

namespace {

constexpr double kFs      = 1000000.0;
constexpr double kDevHz   = 75000.0;
constexpr double kPilotHz = 19000.0;
constexpr double kAudioHz = 1000.0;

/** Textbook stereo MPX on IQ, with a controllable amount of white noise ADDED TO THE IQ.
 *  ★ Noise belongs on the IQ, not on the MPX: that is where it arrives in life, and it is what
 *    makes the FM demodulator produce the triangular noise spectrum this feature responds to.
 *    Adding it to the baseband audio instead would give a flat noise floor and would exercise
 *    nothing that matters. */
struct MpxGen {
    double phase = 0.0, tAcc = 0.0;
    double noise = 0.0;
    // ★★★ THE NEIGHBOUR. A DXer narrows the IF to reject the station 200 kHz away, NOT to reject
    //     noise — and the first version of this test had an empty band, so it could only ever
    //     measure the COST of narrowing and never the benefit. A test that cannot show the
    //     upside is not evidence that there is none.
    double adjAmp = 0.0;                 // adjacent channel amplitude (0 = alone in the band)
    double adjPhase = 0.0;
    std::mt19937 rng{12345};                       // fixed seed: a flaky test is worse than none
    std::normal_distribution<double> gauss{0.0, 1.0};
    void fill(std::vector<cf32>& out, int n) {
        out.resize((size_t)n);
        for (int i = 0; i < n; ++i) {
            const double t = tAcc;
            const double L = std::sin(2.0 * M_PI * kAudioHz * t), R = 0.0;
            const double mpx = 0.45 * (L + R)
                             + 0.10 * std::sin(2.0 * M_PI * kPilotHz * t)
                             + 0.45 * (L - R) * std::sin(2.0 * M_PI * 2.0 * kPilotHz * t);
            phase += 2.0 * M_PI * kDevHz * mpx / kFs;
            if (phase >  M_PI * 1e6) phase -= M_PI * 2e6;
            double re = std::cos(phase), im = std::sin(phase);
            if (adjAmp > 0.0) {
                // A second FM carrier 100 kHz up — the European spacing, and INSIDE our own channel
                // filter, which is the only case where a narrower IF can possibly help.
                adjPhase += 2.0 * M_PI * (100000.0 + kDevHz * 0.6 *
                            std::sin(2.0 * M_PI * 700.0 * t)) / kFs;
                re += adjAmp * std::cos(adjPhase);
                im += adjAmp * std::sin(adjPhase);
            }
            if (noise > 0.0) { re += noise * gauss(rng); im += noise * gauss(rng); }
            out[(size_t)i] = cf32{ (float)re, (float)im };
            tAcc += 1.0 / kFs;
        }
    }
};

struct Sink {
    bool sawStereo = false;
    static void onStereo(void* c, bool locked) { if (locked) ((Sink*)c)->sawStereo = true; }
    static void onAudio(void*, const float*, int, int, int) {}
};

void run(RxPipeline& rx, MpxGen& gen, double seconds) {
    const int block = 8192;
    const int total = (int)(kFs * seconds);
    std::vector<cf32> buf;
    for (int done = 0; done < total; done += block) {
        gen.fill(buf, std::min(block, total - done));
        rx.feed(buf.data(), (int)buf.size());
    }
}

/** Tune a fresh pipeline at a given noise level and report where the L-R corner settled.
 *  ★ A FRESH pipeline each time, and long enough to settle: the corner moves on a ~1 second time
 *    constant on purpose (a corner that chases the signal turns fading into PUMPING, which is more
 *    objectionable than the hiss), so a short run would measure the glide, not the destination. */
struct Result { float cutHz; float snrDb; bool stereo; float ifGain; double ifBw; };
Result measure(double noise, double seconds = 6.0, double adj = 0.0) {
    Sink sink;
    RxPipeline::Callbacks cb{};
    cb.ctx = &sink; cb.stereo = &Sink::onStereo; cb.audio = &Sink::onAudio;
    MpxGen gen; gen.noise = noise; gen.adjAmp = adj;
    RxPipeline rx;
    rx.start(kFs, 1024, 10.0, 48000, cb);
    rx.setTune(0.0, RxPipeline::Mode::WFM, 250000.0);
    run(rx, gen, seconds);
    return { rx.lmrHiCutHz(), rx.blendSnrDb(), sink.sawStereo, rx.ifGainDb(), rx.ifBandwidth() };
}

}  // namespace

int main() {
    std::printf("\nFM stereo high-blend — the hiss goes, the separation stays\n");

    const Result clean = measure(0.0);
    std::printf("   .. clean:      corner %7.0f Hz   ratio %5.1f dB   stereo %s\n",
                clean.cutHz, clean.snrDb, clean.stereo ? "yes" : "NO");
    ok(clean.stereo, "a clean signal still decodes stereo at all");
    // ★★★ THE ANTI-REGRESSION ASSERTION. A strong station must sound exactly as it did before
    //     this feature existed. If this ever fails, high-blend has become a treble control.
    ok(clean.cutHz > 14000.0f,
       "★ a CLEAN signal is left wide open — high-blend is not a tone control",
       "corner " + std::to_string((int)clean.cutHz) + " Hz, expected > 14000");

    // ★★★ NOISE LEVELS CHOSEN TO MATCH REAL SIGNALS, not round numbers. Four stations measured on
    //     air read 6 dB (very noisy), 11 dB (weak and hissy) and 27-30 dB (weak but perfectly
    //     listenable) — so 28 dB is a GOOD signal, not a bad one. The old 0.30 produced 28 dB and
    //     was being called "noisy", which is why the thresholds ended up mis-scaled: the lab's idea
    //     of noisy was the field's idea of fine (Stuart, 2026-08-14).
    const Result noisy = measure(0.50);   // ~24 dB — a weak but usable station
    std::printf("   .. noisy:      corner %7.0f Hz   ratio %5.1f dB   stereo %s\n",
                noisy.cutHz, noisy.snrDb, noisy.stereo ? "yes" : "NO");
    ok(noisy.cutHz < clean.cutHz - 1000.0f,
       "★ a NOISY signal narrows the L-R band — this is the hiss cure",
       "clean " + std::to_string((int)clean.cutHz) + " Hz vs noisy "
                + std::to_string((int)noisy.cutHz) + " Hz");
    ok(noisy.snrDb < clean.snrDb,
       "and the meter agrees the signal got worse (it is not narrowing at random)");

    const Result worse = measure(0.80);   // ~12 dB — hissy, like 107.4 on air
    std::printf("   .. worse:      corner %7.0f Hz   ratio %5.1f dB   stereo %s\n",
                worse.cutHz, worse.snrDb, worse.stereo ? "yes" : "NO");
    ok(worse.cutHz <= noisy.cutHz,
       "★ MONOTONIC: noisier never opens the image back up",
       "noisy " + std::to_string((int)noisy.cutHz) + " Hz vs worse "
                + std::to_string((int)worse.cutHz) + " Hz");

    // ★★ SEPARATION SURVIVES. The floor exists because even 2 kHz of difference signal reads as
    //    "stereo, quietly" and is preferable to a hard collapse — the point of high-blend over a
    //    plain blend is that the bass and mid image is still there when the top has gone.
    ok(worse.cutHz > 500.0f,
       "★ and it never collapses to mono — bass/mid separation is kept",
       "corner " + std::to_string((int)worse.cutHz) + " Hz");

    // ── Would a narrower IF help? Printed, not asserted — it is a MEASUREMENT of the signal, and
    //    the answer legitimately differs per signal. Sanity only: a clean wideband signal should
    //    not show a large gain (there is little noise to exclude and real sidebands to lose),
    //    while a noisy one should show more.
    std::printf("   .. IF-narrowing benefit at 110 kHz, ALONE in the band:\n");
    std::printf("      clean %+.1f dB   noisy %+.1f dB   hissy %+.1f dB\n",
                clean.ifGain, noisy.ifGain, worse.ifGain);
    // ★★ WITH A NEIGHBOUR 200 kHz AWAY — the case narrowing actually exists for.
    const Result adjW = measure(0.20, 6.0, 0.7);
    const Result adjS = measure(0.20, 6.0, 1.4);
    std::printf("   .. with an ADJACENT station 100 kHz up:\n");
    std::printf("      neighbour -3 dB %+.1f dB   neighbour +3 dB %+.1f dB\n",
                adjW.ifGain, adjS.ifGain);

    // ★★★ DOES THE ADAPTIVE IF ENGAGE ON THE RIGHT EVIDENCE? This is the assertion that keeps the
    //     policy honest, because the obvious wiring — narrow when noisy — is WRONG and measured so:
    //     narrowing costs 1-10 dB against noise and gains 10 dB against a strong neighbour. A
    //     control that fires on the wrong evidence is worse than no control, since it degrades the
    //     signals it was meant to rescue.
    {
        const Result quiet = measure(0.80, 12.0, 0.0);   // hissy, but ALONE
        const Result crowd = measure(0.20, 12.0, 1.4);   // strong neighbour
        std::printf("   .. benefit: hissy-but-alone %+.1f dB   strong-neighbour %+.1f dB"
                    "   (IF %.0f / %.0f Hz)\n",
                    quiet.ifGain, crowd.ifGain, quiet.ifBw, crowd.ifBw);
        /* ★★★ THIS PAIR ASSERTED THAT IMS NARROWS THE FILTER, AND IT NO LONGER DOES — the
         *   bandwidth logic moved to auto bandwidth, which owns that filter and is driven from
         *   OUTSIDE the DSP via setAutoBandwidth(); a unit test that only drives the pipeline
         *   never calls it, so ifBw is correctly 0 in both cases here. See the note in
         *   pipeline.cpp: IMS and auto bandwidth were the same feature under two names and fought
         *   over the one slot.
         * ★★★ THE EVIDENCE TEST IS THE PART THAT MATTERED AND IT SURVIVES INTACT. The obvious
         *   wiring — narrow when noisy — is WRONG and measured so: narrowing costs 1-10 dB
         *   against noise and gains 10 dB against a strong neighbour. Whatever commands the
         *   filter, it must steer from THIS, and this is where that is pinned. */
        ok(quiet.ifGain < 0.0f,
           "★★★ NOISE ALONE measures as a LOSS — narrowing cannot help it and would cost",
           "read " + std::to_string(quiet.ifGain) + " dB");
        ok(crowd.ifGain > 3.0f,
           "★ a STRONG NEIGHBOUR measures as a large GAIN — the case narrowing exists for",
           "read only " + std::to_string(crowd.ifGain) + " dB");
        ok(quiet.ifBw == 0.0 && crowd.ifBw == 0.0,
           "★★ and IMS commands the filter in NEITHER case — auto bandwidth owns it",
           "IMS narrowed to " + std::to_string((int)crowd.ifBw) + " Hz");
    }

    // ★★★ A PILOT-LOCK FLICKER MUST NOT REOPEN THE FILTER. On a marginal signal the lock drops in
    //     and out constantly, and the not-eligible branch used to rush the L-R corner to 15 kHz at
    //     full speed — so the instant lock returned, the listener got the whole unfiltered
    //     difference band back: "a couple of times in quick succession I just got a load of treble
    //     come back" at 4-7 dB (Stuart, on air), where the curve is pinned at its floor and nothing
    //     should have moved at all.
    // ★★ Losing lock is not evidence the signal improved — usually the reverse. So the corner must
    //    HOLD. Modelled here by toggling stereo off and back on, which takes the identical branch.
    {
        Sink sink;
        RxPipeline::Callbacks cb{};
        cb.ctx = &sink; cb.stereo = &Sink::onStereo; cb.audio = &Sink::onAudio;
        MpxGen gen; gen.noise = 0.80;                  // ~12 dB, hissy — corner should be well down
        RxPipeline rx;
        rx.start(kFs, 1024, 10.0, 48000, cb);
        rx.setTune(0.0, RxPipeline::Mode::WFM, 250000.0);
        run(rx, gen, 6.0);
        const float settled = rx.lmrHiCutHz();
        for (int i = 0; i < 4; ++i) {                  // four flickers in quick succession
            rx.setStereoEnabled(false); run(rx, gen, 0.4);
            rx.setStereoEnabled(true);  run(rx, gen, 0.4);
        }
        const float after = rx.lmrHiCutHz();
        std::printf("   .. lock flicker: corner %7.0f Hz -> %7.0f Hz\n", settled, after);
        ok(after < settled + 1500.0f,
           "★ a pilot-lock FLICKER does not throw the L-R filter open again",
           "settled " + std::to_string((int)settled) + " Hz, after flicker "
                      + std::to_string((int)after) + " Hz");
    }

    // ★★★ DOES FORCED MONO STILL GET THE AUDIO HIGH-CUT? Two on-air recordings of the same
    //     station a minute apart showed the stereo take rolling off at ~4 kHz and the MONO take at
    //     ~8 kHz. The innocent explanation is that the signal was simply better for the second one
    //     — 8 kHz implies about 20 dB, which is plausible. The guilty one is that forced mono takes
    //     a different path and never gets the cut at all, in which case the listener who switches
    //     to mono to escape the hiss gets LESS help, not more.
    // ★★ Not guessable from the recordings, so assert it: with stereo forced OFF, a noisy signal
    //    must still close the audio corner. The measurement is why this cannot be settled by
    //    reading the code — the cut is applied in the stereo branch, which forced mono still uses.
    {
        Sink sink;
        RxPipeline::Callbacks cb{};
        cb.ctx = &sink; cb.stereo = &Sink::onStereo; cb.audio = &Sink::onAudio;
        MpxGen gen; gen.noise = 0.80;           // ~12 dB, hissy
        RxPipeline rx;
        rx.start(kFs, 1024, 10.0, 48000, cb);
        rx.setStereoEnabled(false);             // the listener presses MONO
        rx.setTune(0.0, RxPipeline::Mode::WFM, 250000.0);
        run(rx, gen, 6.0);
        std::printf("   .. forced MONO: audio cut %7.0f Hz   ratio %5.1f dB\n",
                    rx.audioHiCutHz(), rx.blendSnrDb());
        ok(rx.audioHiCutHz() < 14000.0f,
           "★ FORCED MONO still gets the audio high-cut — the hiss cure is not stereo-only",
           "corner " + std::to_string((int)rx.audioHiCutHz()) + " Hz");
    }

    std::printf(failures ? "\nFAILED %d\n" : "\nall good\n", failures);
    return failures ? 1 : 0;
}
