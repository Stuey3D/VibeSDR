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
            out[(size_t)i] = y;
            tAcc += 1.0 / kFs;
        }
    }
};

struct Sink {
    static void onStereo(void*, bool) {}
    static void onAudio(void*, const float*, int, int, int) {}
};

struct Result { float mpDepth; float snrDb; };

Result measure(double noise, double echoAmp, int echoDelay = 3, double seconds = 4.0) {
    Sink sink;
    RxPipeline::Callbacks cb{};
    cb.ctx = &sink; cb.stereo = &Sink::onStereo; cb.audio = &Sink::onAudio;
    MpxGen gen; gen.noise = noise; gen.echoAmp = echoAmp; gen.echoDelay = echoDelay;
    RxPipeline rx;
    rx.start(kFs, 1024, 10.0, 48000, cb);
    rx.setTune(0.0, RxPipeline::Mode::WFM, 250000.0);
    const int block = 8192, total = (int)(kFs * seconds);
    std::vector<cf32> buf;
    for (int done = 0; done < total; done += block) {
        gen.fill(buf, std::min(block, total - done));
        rx.feed(buf.data(), (int)buf.size());
    }
    return { rx.multipathDepth(), rx.blendSnrDb() };
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
    ok(noisy.mpDepth < echo.mpDepth,
       "★★★ NOISE DOES NOT READ AS MULTIPATH — the discrimination this meter exists for",
       "noise-only " + std::to_string(noisy.mpDepth) + " vs reflection "
                     + std::to_string(echo.mpDepth));

    std::printf(failures ? "\nFAILED %d\n" : "\nall good\n", failures);
    return failures ? 1 : 0;
}
