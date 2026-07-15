// ADJACENT-CHANNEL / ALIAS REJECTION — the regression guard for the DDC cascade.
//
// The DDC decimates the IQ to a channel rate that is far lower than the input rate.
// Anything the final decimation filter fails to attenuate does not merely leak — it
// FOLDS, landing inside the audio at a new frequency, where nothing downstream can
// tell it from a real signal and no later filter can remove it.
//
// The dangerous offsets are (channel rate +/- passband). With the channel at 12 kHz
// that is only ~10 kHz off tune — and on a crowded band, a neighbour 10 kHz away is
// routinely 40-60 dB STRONGER than the DX you are straining to hear. So the final
// stage runs a Blackman window (~74 dB) rather than a Hamming (~53 dB).
//
// This test tunes a weak wanted signal, parks a HUGE interferer exactly on the fold
// frequency, and demands the interferer stay out of the audio. It exists because
// adjacent-channel rejection was hard-won and is the first thing a DDC "optimisation"
// silently destroys.
#include "../vibedsp.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <complex>

using namespace vibedsp;

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

// Power in a narrow band around `hz` of a real audio buffer, via Goertzel.
static double bandPower(const std::vector<float>& x, double rate, double hz) {
    const double w = 2.0 * M_PI * hz / rate;
    const double c = 2.0 * std::cos(w);
    double s0 = 0, s1 = 0, s2 = 0;
    for (float v : x) { s0 = v + c * s1 - s2; s2 = s1; s1 = s0; }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

struct Cap {
    std::vector<float> audio;
    int rate = 48000;
};

static void audioCb(void* ctx, const float* pcm, int frames, int ch, int rate) {
    auto* c = (Cap*)ctx;
    c->rate = rate;
    for (int i = 0; i < frames; ++i) c->audio.push_back(pcm[i * ch]);   // left/mono
}

int main() {
    std::printf("DDC adjacent-channel / alias rejection\n\n");

    const double fs   = 2400000.0;   // 2.4 MSPS input
    const double bw   = 2800.0;      // SSB channel
    const double want = 0.0;         // tuned dead centre

    // A weak wanted signal: an SSB-ish tone 1 kHz inside the passband.
    // A MONSTER interferer 60 dB stronger, swept across the offsets that fold.
    const double wantAmp  = 0.001;   // -60 dBFS
    const double jamAmp   = 1.0;     //   0 dBFS  => 60 dB stronger

    // Offsets to try. These bracket the fold frequencies for a 12 kHz channel
    // (chFs +/- passband ~= 9-15 kHz) plus a couple further out for good measure.
    const double offsets[] = { 6000.0, 9000.0, 10600.0, 12000.0, 13400.0, 24000.0, 48000.0 };

    for (double jamHz : offsets) {
        Cap cap;
        RxPipeline::Callbacks cb;
        cb.ctx = &cap;
        cb.audio = audioCb;

        RxPipeline pipe;
        pipe.start(fs, 1024, 20.0, 48000, cb);
        pipe.setTune(want, RxPipeline::Mode::SSB_USB, bw);

        // Build IQ: weak wanted tone at +1 kHz, huge interferer at +jamHz.
        const int block = 65536;
        const int blocks = 24;
        std::vector<cf32> iq(block);
        double pw = 0.0, pj = 0.0;
        const double dwW = 2.0 * M_PI * 1000.0 / fs;
        const double dwJ = 2.0 * M_PI * jamHz / fs;
        for (int b = 0; b < blocks; ++b) {
            for (int i = 0; i < block; ++i) {
                pw += dwW; pj += dwJ;
                iq[i] = cf32((float)(wantAmp * std::cos(pw) + jamAmp * std::cos(pj)),
                             (float)(wantAmp * std::sin(pw) + jamAmp * std::sin(pj)));
            }
            pipe.feed(iq.data(), block);
        }
        pipe.stop();

        if (cap.audio.size() < 8192) { check(false, "pipeline produced audio"); continue; }
        // Drop the startup transient (filters filling, AGC settling).
        std::vector<float> tail(cap.audio.end() - 8192, cap.audio.end());

        // Where would the interferer LAND if it folded? Anything non-zero in the audio
        // that isn't our 1 kHz tone is contamination. Measure the worst offender by
        // sweeping the audio band and comparing against the wanted tone.
        const double wantPwr = bandPower(tail, cap.rate, 1000.0);
        double worst = 0.0; double worstHz = 0.0;
        for (double f = 150.0; f < 3000.0; f += 25.0) {
            if (std::fabs(f - 1000.0) < 60.0) continue;      // skip the wanted tone
            const double p = bandPower(tail, cap.rate, f);
            if (p > worst) { worst = p; worstHz = f; }
        }
        const double rejDb = 10.0 * std::log10((wantPwr + 1e-30) / (worst + 1e-30));

        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "jammer +%.1f kHz (60 dB up): wanted is %+.0f dB above worst spur "
                      "(%.0f Hz)", jamHz / 1000.0, rejDb, worstHz);
        // The wanted signal is 60 dB WEAKER at the antenna. If the DDC is doing its job
        // the jammer is gone, and the wanted tone dominates the audio. Demand it comes
        // out on top by a clear margin.
        // 30 dB: Blackman clears this with room (+36 dB). A Hamming last stage does NOT
        // (+24 dB at the 13.4 kHz fold) — verified by disabling deepStop. That gap is
        // exactly what this test exists to catch.
        check(rejDb > 30.0, msg);
    }

    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
