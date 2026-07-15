// SSB PASSBAND — does the voice actually get through?
//
// In the Weaver scheme the wanted sideband runs from the carrier out to the FULL
// channel bandwidth on ONE side (SsbDemod down-mixes by bw/2 to centre a sideband
// spanning 0..bw). So the DDC's channel filter must pass 0..bw. Building it with a
// bw/2 half-width — the obvious-looking thing, and what the code did for years —
// closes the filter at 1.4 kHz on a 2.8 kHz SSB channel and takes the consonants
// with it. The audio still sounds like speech; it just sounds muffled, and you only
// notice when you A/B it against another receiver.
//
// (We did. Against an UberSDR recording of the same 7.187 MHz LSB signal, VibeSDR was
// 37 dB down across 2.0-2.7 kHz — the exact band that carries intelligibility.)
//
// This test puts tones ACROSS the sideband and demands they all survive.
#include "../vibedsp.h"
#include <cstdio>
#include <cmath>
#include <vector>

using namespace vibedsp;

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

static double bandPower(const std::vector<float>& x, double rate, double hz) {
    const double w = 2.0 * M_PI * hz / rate;
    const double c = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0, s0 = 0;
    for (float v : x) { s0 = v + c * s1 - s2; s2 = s1; s1 = s0; }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

struct Cap { std::vector<float> audio; int rate = 48000; };
static void audioCb(void* ctx, const float* pcm, int frames, int ch, int rate) {
    auto* c = (Cap*)ctx;
    c->rate = rate;
    for (int i = 0; i < frames; ++i) c->audio.push_back(pcm[i * ch]);
}

// Drive one audio tone through as USB: a real tone `af` above the carrier appears in
// the IQ as a single complex exponential at +af (the sideband).
static double toneThrough(double af, double fs, double bw) {
    Cap cap;
    RxPipeline::Callbacks cb; cb.ctx = &cap; cb.audio = audioCb;
    RxPipeline pipe;
    pipe.start(fs, 1024, 20.0, 48000, cb);
    pipe.setTune(0.0, RxPipeline::Mode::SSB_USB, bw);

    const int block = 65536, blocks = 20;
    std::vector<cf32> iq(block);
    double ph = 0.0;
    const double dw = 2.0 * M_PI * af / fs;
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < block; ++i) {
            ph += dw;
            iq[i] = cf32((float)(0.2 * std::cos(ph)), (float)(0.2 * std::sin(ph)));
        }
        pipe.feed(iq.data(), block);
    }
    pipe.stop();
    if (cap.audio.size() < 8192) return -999.0;
    std::vector<float> tail(cap.audio.end() - 8192, cap.audio.end());
    // AGC normalises level, so compare the tone against the TOTAL energy: a tone that
    // survived dominates its own buffer; one that was filtered out leaves only noise.
    double total = 0.0;
    for (float v : tail) total += (double)v * v;
    const double tone = bandPower(tail, cap.rate, af);
    return 10.0 * std::log10((tone + 1e-30) / (total + 1e-30));
}

int main() {
    std::printf("SSB passband (2.8 kHz channel — the whole sideband must get through)\n\n");
    const double fs = 2400000.0;
    const double bw = 2800.0;

    // Tones across the sideband. 2.4 kHz is the one that matters: it is inside a 2.8 kHz
    // channel, it is where speech intelligibility lives, and a bw/2 filter kills it.
    const double afs[]  = { 400.0, 1000.0, 1800.0, 2400.0 };
    double ref = 0.0;
    for (size_t i = 0; i < 4; ++i) {
        const double d = toneThrough(afs[i], fs, bw);
        if (i == 0) ref = d;
        char msg[140];
        std::snprintf(msg, sizeof(msg),
                      "%.1f kHz tone survives the channel filter (%.1f dB, ref %.1f dB)",
                      afs[i] / 1000.0, d, ref);
        // Within 12 dB of the 400 Hz reference tone. A tone that has been filtered out
        // sits 30-40 dB down, so this is a wide-open door that a bw/2 cutoff still
        // cannot walk through.
        check(d > ref - 12.0, msg);
    }

    // And the far side must still be REJECTED — passing MORE of the wanted sideband
    // must not mean passing the WRONG one. Feed both at once: a wanted tone at +900 Hz
    // and an image at -2100 Hz, equal strength. Both land at a different audio pitch,
    // so we can read them off the same buffer.
    //
    // They must be measured TOGETHER. Feeding the image alone proves nothing: the AGC
    // simply winds the gain up until the surviving remnant is loud again, and an
    // attenuated tone looks exactly like a passed one. (This test got that wrong first
    // time round and "failed" a working DDC.)
    std::printf("\n");
    {
        Cap cap;
        RxPipeline::Callbacks cb; cb.ctx = &cap; cb.audio = audioCb;
        RxPipeline pipe;
        pipe.start(fs, 1024, 20.0, 48000, cb);
        pipe.setTune(0.0, RxPipeline::Mode::SSB_USB, bw);
        const int block = 65536, blocks = 20;
        std::vector<cf32> iq(block);
        double pw = 0.0, pi_ = 0.0;
        const double dwW = 2.0 * M_PI * 900.0 / fs;      // + => wanted (USB)
        const double dwI = -2.0 * M_PI * 2100.0 / fs;    // - => image  (LSB)
        for (int b = 0; b < blocks; ++b) {
            for (int i = 0; i < block; ++i) {
                pw += dwW; pi_ += dwI;
                iq[i] = cf32((float)(0.2 * (std::cos(pw) + std::cos(pi_))),
                             (float)(0.2 * (std::sin(pw) + std::sin(pi_))));
            }
            pipe.feed(iq.data(), block);
        }
        pipe.stop();
        std::vector<float> tail(cap.audio.end() - 8192, cap.audio.end());
        const double w = bandPower(tail, cap.rate, 900.0);
        const double im = bandPower(tail, cap.rate, 2100.0);
        const double rej = 10.0 * std::log10((w + 1e-30) / (im + 1e-30));
        char msg[140];
        std::snprintf(msg, sizeof(msg),
                      "image rejection: wanted (900 Hz) is %.1f dB above the wrong "
                      "sideband (2100 Hz)", rej);
        check(rej > 30.0, msg);
    }

    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
