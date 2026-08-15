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
                float nbRate; };

Result measure(double noise, double echoAmp, int echoDelay = 3, double seconds = 4.0,
               double impAmp = 0.0, bool nbOn = true) {
    Sink sink;
    RxPipeline::Callbacks cb{};
    cb.ctx = &sink; cb.stereo = &Sink::onStereo; cb.audio = &Sink::onAudio;
    MpxGen gen; gen.noise = noise; gen.echoAmp = echoAmp; gen.echoDelay = echoDelay;
    gen.impAmp = impAmp;
    RxPipeline rx;
    rx.start(kFs, 1024, 10.0, 48000, cb);
    rx.setNoiseBlanker(nbOn);
    rx.setTune(0.0, RxPipeline::Mode::WFM, 250000.0);
    const int block = 8192, total = (int)(kFs * seconds);
    std::vector<cf32> buf;
    for (int done = 0; done < total; done += block) {
        gen.fill(buf, std::min(block, total - done));
        rx.feed(buf.data(), (int)buf.size());
    }
    return { rx.multipathDepth(), rx.blendSnrDb(), rx.ceqEngaged(),
             rx.multipathAfterCeq(), rx.ceqEffort(), rx.noiseBlankRate() };
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

    std::printf(failures ? "\nFAILED %d\n" : "\nall good\n", failures);
    return failures ? 1 : 0;
}
