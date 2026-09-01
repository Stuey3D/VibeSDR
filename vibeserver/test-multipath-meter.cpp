// test-multipath-meter.cpp — telling MULTIPATH apart from NOISE, which is the whole point.
//
// ★★★ WHY THIS METER EARNS ITS KEEP. Noise and multipath are both "a bad signal" and they want
//     OPPOSITE treatments. Noise is cured by throwing bandwidth away — blend the stereo, cut the
//     top, narrow the IF. Multipath is a DISTORTION: narrowing the IF can make it worse, and the
//     signal may be strong, so dulling it is a pure loss. A receiver that cannot tell them apart
//     has to treat every bad signal as a weak one, and that compromise is a good part of why an
//     ordinary SDR sounds worse than a TEF6686 on a difficult signal.
//
// ★★★ SO THE CENTRAL ASSERTION IS THE NEGATIVE ONE: a signal with plenty of NOISE and no
//     reflection must NOT read as multipath. Without that this is just a second, worse noise
//     meter — and it would read high on every weak station, which would then be "corrected" for a
//     reflection that is not there. Any future IMS built on it would misfire everywhere.
//
// ★★ THE CHANNEL MODEL. A reflection is a delayed, attenuated, phase-rotated copy:
//        y[n] = x[n] + a * e^{j phi} * x[n-D]
//     That is genuinely what a reflection is, and it is enough to produce the effect that matters:
//     the sum is frequency-SELECTIVE (a comb, notches spaced 1/D apart), so as FM's instantaneous
//     frequency sweeps +/-75 kHz it passes through notches and the received ENVELOPE — constant at
//     the transmitter — starts to move. No fading, no motion, no Doppler required: this models the
//     STATIC multipath a rooftop aerial actually suffers.
//
// ★ Delays are in whole samples at 1 MHz = 1 us steps, i.e. 300 m of path difference per sample.
//   Typical urban reflections are tens to hundreds of metres, so a few samples is realistic.

#include "../android/app/src/main/cpp/vibedsp/vibedsp.h"

#include <cmath>
#include <cstdio>
#include <deque>
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

struct MpxGen {
    double phase = 0.0, tAcc = 0.0;
    double noise = 0.0;          // AWGN added to the IQ
    double echoAmp = 0.0;        // reflection amplitude, 0..1 (0.5 = -6 dB)
    int    echoDelay = 3;        // samples at kFs (1 us each ~ 300 m)
    double echoPhase = 1.1;      // radians
    // ★ Impulse noise: brief, enormous, wideband — ignition, a thermostat, an electric fence.
    //   Modelled as short bursts at a realistic repetition rate (100/s ~ mains-related buzz).
    // ★★★ AN ADJACENT STATION, which is the ONLY thing IMS is meant to act on. The harness had
    //     never had one, so every IMS assertion here was a negative — "leave a lone station alone"
    //     — and the +3 dB engage threshold had no test at all. A neighbour is its own FM signal
    //     with its own programme, offset by `nbrOffsetHz`; the interference is what its skirts put
    //     inside our channel, which is exactly what a narrower IF removes.
    double nbrAmp = 0.0;
    double nbrOffsetHz = 200000.0;
    double nbrDevHz = 75000.0;      // a loud local runs heavy processing and deviates hard
    double nbrPhase = 0.0;
    double impAmp = 0.0;
    int    impEvery = 10000;     // samples between impulses (1 MHz / 10000 = 100 per second)
    int    impLen = 3;           // samples — microseconds
    long   impN = 0;
    std::mt19937 rng{2024};
    std::normal_distribution<double> gauss{0.0, 1.0};
    std::deque<cf32> hist;

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
            cf32 direct{ (float)std::cos(phase), (float)std::sin(phase) };

            cf32 y = direct;
            if (nbrAmp > 0.0) {
                // A different programme tone, so it cannot correlate with ours.
                const double nm = 0.45 * std::sin(2.0 * M_PI * 700.0 * t)
                                + 0.10 * std::sin(2.0 * M_PI * kPilotHz * t);
                nbrPhase += 2.0 * M_PI * nbrDevHz * nm / kFs;
                if (nbrPhase >  M_PI * 1e6) nbrPhase -= M_PI * 2e6;
                const double car = 2.0 * M_PI * nbrOffsetHz * t;
                const double a = nbrPhase + car;
                y = cf32{ (float)(y.real() + nbrAmp * std::cos(a)),
                          (float)(y.imag() + nbrAmp * std::sin(a)) };
            }
            // ── the reflection ────────────────────────────────────────────────────────────────
            hist.push_back(direct);
            if ((int)hist.size() > echoDelay + 1) hist.pop_front();
            if (echoAmp > 0.0 && (int)hist.size() > echoDelay) {
                const cf32 d = hist.front();
                const double cr = std::cos(echoPhase), sr = std::sin(echoPhase);
                y = cf32{ (float)(y.real() + echoAmp * (d.real() * cr - d.imag() * sr)),
                          (float)(y.imag() + echoAmp * (d.real() * sr + d.imag() * cr)) };
            }
            if (noise > 0.0)
                y = cf32{ (float)(y.real() + noise * gauss(rng)),
                          (float)(y.imag() + noise * gauss(rng)) };
            if (impAmp > 0.0 && (impN % impEvery) < impLen) {
                y = cf32{ (float)(y.real() + impAmp), (float)(y.imag() + impAmp * 0.4) };
            }
            ++impN;
            out[(size_t)i] = y;
            tAcc += 1.0 / kFs;
        }
    }
};

struct Sink {
    static void onStereo(void*, bool) {}
    static void onAudio(void*, const float*, int, int, int) {}
};

struct Result { float mpDepth; float snrDb; bool ceqOn; float ceqAfter; float effort;
                float nbRate; unsigned ifBuilds, shBuilds; float ifBw; float ifGain; };

Result measure(double noise, double echoAmp, int echoDelay = 3, double seconds = 4.0,
               double impAmp = 0.0, bool nbOn = true, bool imsOn = true,
               double nbrAmp = 0.0, double retuneAt = -1.0,
               double nbrOffsetHz = 200000.0, double nbrDevHz = 75000.0) {
    Sink sink;
    RxPipeline::Callbacks cb{};
    cb.ctx = &sink; cb.stereo = &Sink::onStereo; cb.audio = &Sink::onAudio;
    MpxGen gen; gen.noise = noise; gen.echoAmp = echoAmp; gen.echoDelay = echoDelay;
    gen.nbrAmp = nbrAmp; gen.nbrOffsetHz = nbrOffsetHz; gen.nbrDevHz = nbrDevHz;
    gen.impAmp = impAmp;
    RxPipeline rx;
    rx.start(kFs, 1024, 10.0, 48000, cb);
    rx.setNoiseBlanker(nbOn);
    rx.setIms(imsOn);
    rx.setTune(0.0, RxPipeline::Mode::WFM, 250000.0);
    const int block = 8192, total = (int)(kFs * seconds);
    const int retuneSample = retuneAt > 0.0 ? (int)(kFs * retuneAt) : -1;
    bool retuned = false;
    std::vector<cf32> buf;
    for (int done = 0; done < total; done += block) {
        // ★ A RETUNE PART-WAY THROUGH, with the neighbour switched off at the same moment: this is
        //   "you were on a station that needed narrowing, now you are on one that does not".
        if (retuneSample >= 0 && !retuned && done >= retuneSample) {
            retuned = true;
            gen.nbrAmp = 0.0;
            rx.setTune(1.0, RxPipeline::Mode::WFM, 250000.0);
        }
        gen.fill(buf, std::min(block, total - done));
        rx.feed(buf.data(), (int)buf.size());
    }
    return { rx.multipathDepth(), rx.blendSnrDb(), rx.ceqEngaged(),
             rx.multipathAfterCeq(), rx.ceqEffort(), rx.noiseBlankRate(),
             rx.ifRebuilds(), rx.shadowRebuilds(), (float)rx.ifBandwidth(), rx.ifGainDb() };
}

}  // namespace

int main() {
    std::printf("\nMultipath meter — a reflection is not the same thing as noise\n");

    const Result clean = measure(0.0, 0.0);
    std::printf("   .. clean:            multipath %.4f   noise-ratio %5.1f dB\n",
                clean.mpDepth, clean.snrDb);
    // ★★ An FM carrier has CONSTANT amplitude. With no channel at all there is nothing to see,
    //    and if this is not ~0 the meter is measuring its own arithmetic.
    ok(clean.mpDepth < 0.02f,
       "★ a clean carrier has a constant envelope — nothing to report",
       "depth " + std::to_string(clean.mpDepth));

    const Result echo = measure(0.0, 0.5);
    std::printf("   .. reflection -6 dB: multipath %.4f   noise-ratio %5.1f dB\n",
                echo.mpDepth, echo.snrDb);
    ok(echo.mpDepth > clean.mpDepth * 5.0f + 0.01f,
       "★ a -6 dB reflection is DETECTED, with no motion or Doppler needed",
       "clean " + std::to_string(clean.mpDepth) + " vs echo " + std::to_string(echo.mpDepth));

    const Result echoBig = measure(0.0, 0.8);
    std::printf("   .. reflection -2 dB: multipath %.4f   noise-ratio %5.1f dB\n",
                echoBig.mpDepth, echoBig.snrDb);
    ok(echoBig.mpDepth > echo.mpDepth,
       "★ and a WORSE reflection reads worse — it is a measure, not a flag",
       "-6 dB " + std::to_string(echo.mpDepth) + " vs -2 dB " + std::to_string(echoBig.mpDepth));

    // ★★★ THE ONE THAT MATTERS. Noise must not masquerade as a reflection, or every weak station
    //     gets "corrected" for multipath that is not there.
    const Result noisy = measure(0.30, 0.0);
    std::printf("   .. noise, no echo:   multipath %.4f   noise-ratio %5.1f dB\n",
                noisy.mpDepth, noisy.snrDb);
    ok(noisy.snrDb < clean.snrDb - 3.0f,
       "the noise meter sees the noise (the signal really did get worse)");
    // ★★★ THE FIELD RAISED THE BAR ON THIS ONE. It used to be enough that noise read LOWER than a
    //     reflection; on air that was not enough at all — a 6 dB signal read 25.7% and was labelled
    //     "severe", which was the noise (Stuart, 107.8 MHz). With the measured noise contribution
    //     now subtracted in the power domain, noise alone must correct to very nearly NOTHING.
    ok(noisy.mpDepth < 0.03f,
       "★★★ NOISE CORRECTS TO ~ZERO MULTIPATH — not merely 'less than a reflection'",
       "noise-only corrected to " + std::to_string(noisy.mpDepth));

    // ★★ AND THE CORRECTION MUST NOT EAT A REAL REFLECTION THAT HAPPENS TO ARRIVE IN NOISE, which
    //    is the failure mode a subtraction invites and the case a DXer most cares about: a distant
    //    station is BOTH weak and reflected.
    const Result both = measure(0.30, 0.5);
    std::printf("   .. noise + echo:     multipath %.4f   noise-ratio %5.1f dB\n",
                both.mpDepth, both.snrDb);
    ok(both.mpDepth > noisy.mpDepth + 0.05f,
       "★ a reflection is still seen THROUGH noise — the correction does not eat it",
       "noise-only " + std::to_string(noisy.mpDepth) + " vs noise+echo "
                     + std::to_string(both.mpDepth));

    // ── CALIBRATION SWEEP: what does NOISE ALONE read, right down to the floor? ──────────────
    // ★★★ THIS EXISTS BECAUSE THE FIELD BROKE MY ASSUMPTION. On air at 107.8 (S8) the panel showed
    //     MPX S/N 6 dB and multipath 25.7% "severe" — a reading as high as a -2 dB reflection in
    //     the lab. But the discrimination above was only ever demonstrated at MODERATE noise
    //     (28 dB), and 6 dB is far outside it. Noise shakes the envelope too, so at the bottom of
    //     the range the meter may simply be reporting the noise back as multipath — which would
    //     send a DXer rotating an aerial to fix a reflection that is not there.
    // ★★ So measure the noise-only curve instead of assuming it. Printed, not asserted: it is the
    //    evidence the correction is built on, and it must stay visible when the filters change.
    std::printf("\n   noise-only calibration (echo = 0) — what noise alone looks like:\n");
    std::printf("   %-8s %-12s %s\n", "noise", "MPX S/N dB", "multipath");
    for (double nz : { 0.0, 0.15, 0.30, 0.50, 0.80, 1.20, 2.00 }) {
        const Result r = measure(nz, 0.0);
        std::printf("   %-8.2f %-12.1f %.4f\n", nz, r.snrDb, r.mpDepth);
    }

    // ── CEQ: does the equaliser actually undo a reflection? ───────────────────────────────────
    // ★★★ SCORED BEFORE AND AFTER ON THE SAME SIGNAL, because "it ran" is not evidence that it
    //     helped. A CMA equaliser fed the wrong conditions contorts itself trying to correct
    //     randomness and makes things worse, so the only honest test is whether the measured
    //     multipath depth FALLS.
    std::printf("\nCEQ — undoing the reflection\n");
    {
        const Result r = measure(0.05, 0.5, 3, 12.0);
        std::printf("   .. -6 dB reflection, clean:  engaged %s  before %.4f  after %.4f  effort %.3f\n",
                    r.ceqOn ? "yes" : "NO", r.mpDepth, r.ceqAfter, r.effort);
        ok(r.ceqOn, "★ CEQ engages on a good signal with real multipath");
        ok(r.ceqAfter < r.mpDepth * 0.8f,
           "★★★ AND THE REFLECTION IS MEASURABLY REDUCED — not merely 'it ran'",
           "before " + std::to_string(r.mpDepth) + " after " + std::to_string(r.ceqAfter));

        // ★★ AND IT MUST STAY OUT OF THE WAY WHERE IT CANNOT HELP. Noise is not a reflection, and
        //    an equaliser turned loose on it amplifies what it cannot fix.
        const Result n = measure(1.20, 0.0, 3, 12.0);
        std::printf("   .. noise, no reflection:     engaged %s\n", n.ceqOn ? "YES" : "no");
        ok(!n.ceqOn,
           "★★★ CEQ does NOT engage on a noisy signal with no reflection",
           "it engaged, and CMA on noise makes things worse");

        const Result c = measure(0.0, 0.0, 3, 12.0);
        std::printf("   .. clean, no reflection:     engaged %s\n", c.ceqOn ? "YES" : "no");
        ok(!c.ceqOn, "★ and not on a clean signal either — there is nothing to correct");
    }

    // ── Noise blanker ─────────────────────────────────────────────────────────────────────────
    // ★★★ THE FIRST ASSERTION IS THAT IT DOES NOTHING WHEN THERE IS NOTHING TO DO. A blanker that
    //     triggers on ordinary signal is a distortion generator: it removes real samples and
    //     replaces them with held ones, and on a clean carrier that is pure damage.
    std::printf("\nNoise blanker — impulses, not noise\n");
    {
        const Result clean = measure(0.0, 0.0, 3, 6.0, 0.0);
        std::printf("   .. clean signal:        blanked %.4f%%\n", clean.nbRate * 100.0f);
        ok(clean.nbRate < 0.0005f,
           "★★★ a CLEAN signal is not blanked at all — a blanker that fires on signal is damage",
           "blanked " + std::to_string(clean.nbRate * 100.0f) + "%");

        // ★★ AND IT MUST NOT MISTAKE GAUSSIAN NOISE FOR IMPULSES. Noise has no outliers of the
        //    kind that matter; if this fires on a hissy station it will chew the signal up.
        const Result noisy = measure(0.60, 0.0, 3, 6.0, 0.0);
        std::printf("   .. noisy (no impulses): blanked %.4f%%\n", noisy.nbRate * 100.0f);
        ok(noisy.nbRate < 0.02f,
           "★★ and hiss is not impulses — the blanker stays out of a merely NOISY signal",
           "blanked " + std::to_string(noisy.nbRate * 100.0f) + "%");

        const Result imp = measure(0.05, 0.0, 3, 6.0, 6.0);
        std::printf("   .. with impulses:       blanked %.4f%%\n", imp.nbRate * 100.0f);
        // ★ THE RATE IS MEANT TO BE TINY, and my first threshold (0.02%) was simply wrong: the
        //   generator fires 3 samples in every 10000 at 1 MHz, and the chain decimates by four
        //   before this sees anything, so a few hundredths of a percent IS the whole of the
        //   interference. A blanker removing a large fraction of a signal would be the alarming
        //   result, not this one — which is why the assertion that matters is the dB one below.
        ok(imp.nbRate > clean.nbRate * 10.0f + 0.00005f,
           "★ impulses ARE detected and excised",
           "blanked " + std::to_string(imp.nbRate * 100.0f) + "%");

        // ★★★ AND DOES IT ACTUALLY HELP? Detecting them is not the point; the point is that the
        //     signal measures better afterwards. Blanking at the CHANNEL rate is a compromise —
        //     the decimation filters have already smeared each impulse into a ring — so this is
        //     the number that decides whether the placement is good enough or the blanker belongs
        //     earlier in the chain, at real CPU cost.
        const Result impOff = measure(0.05, 0.0, 3, 6.0, 6.0, /*nbOn=*/false);
        std::printf("   .. impulses, NB off %.1f dB   NB on %.1f dB   -> %+.1f dB\n",
                    impOff.snrDb, imp.snrDb, imp.snrDb - impOff.snrDb);
        ok(imp.snrDb > impOff.snrDb + 0.5f,
           "★★★ AND THE SIGNAL MEASURES BETTER FOR IT — the only result that matters",
           "off " + std::to_string(impOff.snrDb) + " dB, on " + std::to_string(imp.snrDb) + " dB");
    }

    // ── The IF filters must not be REBUILT while nothing is changing ──────────────────────────
    // ★★★ THE SHADOW IS TOLD ITS BANDWIDTH ON EVERY EVALUATION — about fifteen times a second,
    //     with the same value each time. Each call used to design a fresh windowed-sinc and
    //     heap-allocate a new FirDecimator on the DSP thread, and, worse, THROW AWAY THE FILTER'S
    //     HISTORY: every shadow measurement was taken on a filter that had just been reset, so it
    //     measured its own startup transient. That reading steers the narrowing decision, and a
    //     decision that chatters rebuilds the REAL filter in the audio path, which is audible.
    //     Reported as "audio dropouts on HFM, which is a weaker signal" (Stuart, 2026-08-15).
    std::printf("\nThe IF filters must not be rebuilt while nothing changes\n");
    {
        const Result r = measure(0.05, 0.0, 3, 8.0);
        // ★★★ AND THE DECISION MUST SURVIVE THE FIX. With the shadow filter finally keeping its
        //     history, the measurement changed — and it changed to AGREE with the figure measured
        //     on air: held wide, a clean station settles at about -9.5 dB, against the -9.8 dB in
        //     the policy note. The calibration was right all along.
        // ★★ What was wrong is that the decision acted before the averages meant anything. A dwell
        //    counts AGREEMENT and cannot tell agreement from a shared start-up transient, so the
        //    receiver narrowed a clean signal — and narrowing a clean signal measurably RUINS it:
        //    multipath went 0.0003 -> 0.052, a hundredfold, because the cost of narrowing is
        //    distortion on deviation peaks and the guard-band meter cannot see that.
        for (double nz : {0.0, 0.05}) {
            const Result wide = measure(nz, 0.0, 3, 8.0, 0.0, true, /*imsOn=*/false);
            const Result auto_ = measure(nz, 0.0, 3, 8.0, 0.0, true, /*imsOn=*/true);
            std::printf("   .. noise %.2f: settled benefit %+.1f dB   IMS left it %s\n",
                        nz, wide.ifGain, auto_.ifBw > 0.0f ? "NARROWED" : "wide");
            ok(wide.ifGain < -5.0f,
               "★ narrowing a station with no neighbour is measured as a LOSS, as on air",
               "read " + std::to_string(wide.ifGain) + " dB");
            ok(auto_.ifBw <= 0.0f,
               "★★★ SO IMS LEAVES IT ALONE — it does not act on an unsettled measurement",
               "narrowed to " + std::to_string(auto_.ifBw / 1000.0f) + " kHz");
            ok(auto_.mpDepth < 0.03f,
               "★★ and the signal is not damaged by a filter it never needed",
               "multipath " + std::to_string(auto_.mpDepth));
        }
        std::printf("   .. over 8 s: audio-path designs %u, shadow designs %u  (IF now %.0f kHz)\n",
                    r.ifBuilds, r.shBuilds, r.ifBw / 1000.0f);
        // A handful covers configure() plus any genuine decision; the broken version reached
        // hundreds, one per evaluation.
        ok(r.shBuilds <= 4,
           "★★★ THE SHADOW FILTER IS DESIGNED ONCE, not once per evaluation",
           "designed " + std::to_string(r.shBuilds) + " times in 8 s");
        ok(r.ifBuilds <= 4,
           "★★ and the AUDIO path's filter is not rebuilt behind the listener's back",
           "designed " + std::to_string(r.ifBuilds) + " times in 8 s");
    }

    // ── IMS: the one case it exists for, and the one it must let go of ────────────────────────
    std::printf("\nIMS — a NEIGHBOUR is the only thing narrowing helps against\n");
    {
        // ★★★ THE FIRST POSITIVE TEST THIS CONTROL HAS EVER HAD. Everything before it asserted the
        //     negative — leave a lone station alone — so the +3 dB engage threshold was carried on
        //     one on-air observation and nothing else.
        // ★★ CHOSEN BY MEASUREMENT, NOT BY TASTE. A neighbour at the nominal 200 kHz spacing is
        //    already rejected by the 250 kHz channel filter, and narrowing then only costs (-9.7 dB
        //    measured) — so the harness would have been asserting that IMS does something it
        //    should not. A STRONG one 150 kHz away genuinely intrudes, and that is the case built
        //    here. Real transmitters have wider skirts than this clean synthetic, which is why the
        //    on-air case engages at 200 kHz and this one needs 150.
        const Result nbr = measure(0.02, 0.0, 3, 14.0, 0.0, true, true,
                                   /*nbrAmp=*/1.4, /*retuneAt=*/-1.0, /*off=*/150000.0);
        std::printf("   .. strong neighbour:  benefit %+.1f dB -> IF %.0fk\n",
                    nbr.ifGain, nbr.ifBw / 1000.0f);
        /* ★★★ THIS ASSERTED A CONTRACT THAT WAS DELIBERATELY RETIRED, and stayed red for it.
         *   It used to read `nbr.ifBw > 0` — "IMS engages" — and that was right when IMS
         *   commanded the IF filter. It no longer does: auto bandwidth owns that filter now,
         *   because the two were the same feature wearing different names and fought over the
         *   slot (see the long note in pipeline.cpp beside ifWarm_). The shadow measurement
         *   stayed, and it is the only honest answer to "would narrowing help here?" — it simply
         *   commands nothing.
         * ★★★ SO THE TEST FOLLOWS THE BEHAVIOUR TO ITS NEW ADDRESS, rather than being deleted.
         *   The MEASUREMENT is the part worth pinning and it is still exactly as valuable: a
         *   strong neighbour must read as a large positive benefit, where a lone station reads
         *   negative. That is what the IF NARROW readout shows the listener, and it is what any
         *   future narrowing logic would steer from.
         * ★★ AND THE SEPARATION IS ASSERTED TOO, not assumed. If someone re-wires IMS to the
         *   filter, ifBwReq_ starts winning the min() in pipeline.cpp again and the two features
         *   are back to fighting — this catches that the moment it happens.
         * ★ A stale test that cries wolf is worse than no test: a suite with a permanent red in
         *   it trains everyone to stop reading it. */
        ok(nbr.ifGain > 3.0f,
           "★★★ THE BENEFIT IS MEASURED against an adjacent station — what the meter is FOR",
           "benefit only " + std::to_string(nbr.ifGain) + " dB");
        ok(nbr.ifBw <= 0.0f,
           "★★★ ...and IMS does NOT command the filter — auto bandwidth owns it",
           "IMS narrowed to " + std::to_string(nbr.ifBw / 1000.0f) + " kHz");

        // ★★★ AND IT LETS GO ON A RETUNE. The engage rule needs 3 dB in EITHER direction so it
        //     cannot chatter — which makes BOTH states stable, so a filter earned on one station
        //     was carried into the next: "tune from 103.8 which needs the IMS up to 104.2, the
        //     super strong Radio Northampton, and the IMS stays on; tune DOWN to 104.2 from above
        //     and it doesn't activate" (Stuart, 2026-08-15). Two routes to one dial reading, two
        //     different receivers — and narrowing is supposed to be EARNED per station.
        const Result after = measure(0.02, 0.0, 3, 20.0, 0.0, true, true,
                                     /*nbrAmp=*/1.4, /*retuneAt=*/10.0, /*off=*/150000.0);
        std::printf("   .. then retuned away: benefit %+.1f dB, IF %.0fk\n",
                    after.ifGain, after.ifBw / 1000.0f);
        /* ★ THE MEASUREMENT LETS GO TOO, which is the half that still exists. Retuning away from
         *   the neighbour must collapse the measured benefit — otherwise the readout would carry
         *   one station's verdict onto the next, which is the fault Stuart described on air even
         *   when it was the FILTER being carried: "tune from 103.8 which needs the IMS up to
         *   104.2 and the IMS stays on". Same fault, now visible in the meter rather than the
         *   audio, and still worth catching. */
        ok(after.ifGain < 3.0f,
           "★★★ AND THE MEASUREMENT LETS GO ON A RETUNE — the next station has not earned it",
           "benefit still " + std::to_string(after.ifGain) + " dB");
        ok(after.ifBw <= 0.0f,
           "★★ and still nothing is commanded",
           "IF still " + std::to_string(after.ifBw / 1000.0f) + " kHz");
    }

    std::printf(failures ? "\nFAILED %d\n" : "\nall good\n", failures);
    return failures ? 1 : 0;
}
