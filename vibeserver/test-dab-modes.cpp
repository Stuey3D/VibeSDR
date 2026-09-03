// test-dab-modes.cpp — the four transmission modes, checked against their own arithmetic.
//
// ★★★ THE POINT OF THIS TEST IS TRANSCRIPTION. These eight numbers per mode came off a slide, and
//     a single mistyped digit would send the demodulator hunting for a null symbol that is not
//     there — which presents as "DAB does not work here" rather than as a typo. But the table is
//     internally redundant: symbol = useful + guard, useful = 1/spacing, frame = null + N x symbol,
//     and carriers x spacing = 1.536 MHz for every mode. So the numbers can check EACH OTHER, and
//     a typo has to be consistent across four independent relationships to survive. That is a far
//     stronger guarantee than "I read it twice".
#include "vibe_dab_modes.h"
#include <cstdio>
#include <cstdlib>

using namespace vibedab;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); ++fails; } } while (0)

int main() {
    printf("test-dab-modes\n");
    CHECK(kModeCount == 4, "DAB defines four transmission modes");

    for (size_t i = 0; i < kModeCount; ++i) {
        const Mode& m = kModes[i];
        char what[80];

        // 1. useful symbol time is exactly 1/spacing, and 2.048 MHz / spacing gives the FFT length.
        snprintf(what, sizeof what, "mode %d: 1/spacing divides evenly", m.number);
        CHECK(1000000 % m.spacingHz == 0, what);
        snprintf(what, sizeof what, "mode %d: usefulSamples = 2.048MHz / spacing", m.number);
        CHECK(m.usefulSamples == int(kCanonicalRateHz / uint32_t(m.spacingHz)), what);

        // 2. ★★ EVERY MODE OCCUPIES 1.536 MHz — the invariant that keeps the capability gate valid
        //    for all four, and for anything added later.
        snprintf(what, sizeof what, "mode %d: carriers x spacing = 1.536 MHz", m.number);
        CHECK(occupiedHz(m) == 1536000u, what);

        // 3. ★★★ EXACT, NOT APPROXIMATE. frame = null + N x (useful + guard), to the sample.
        //    In rounded microseconds this identity is off by up to 36 us and can only be checked
        //    with a tolerance — which would hide a real transcription error. In samples it is an
        //    equality, so a mistyped digit cannot survive.
        snprintf(what, sizeof what, "mode %d: frame = null + N x symbol, EXACTLY", m.number);
        CHECK(m.nullSamples + m.symbolsPerFrame * symbolSamples(m) == m.frameSamples, what);

        // 4. and the frame is a whole number of milliseconds.
        snprintf(what, sizeof what, "mode %d: frame is a whole ms", m.number);
        CHECK(frameUs(m) % 1000 == 0, what);
    }

    // Mode I, spelled out — Band III and everything the UK transmits.
    const Mode& m1 = modeI();
    CHECK(m1.number == 1 && m1.carriers == 1536 && m1.spacingHz == 1000, "Mode I: 1536 x 1 kHz");
    CHECK(frameUs(m1) == 96000, "Mode I frame is 96 ms");
    CHECK(m1.nullSamples == 2656 && symbolSamples(m1) == 2552, "Mode I: 2656-sample null, 2552-sample symbol");
    CHECK(usefulUs(m1) == 1000, "Mode I useful symbol is 1000 us");

    // ★ At the canonical 2.048 MSPS Mode I wants a 2048-point FFT — which is WHY that rate is
    //   canonical, and a sanity check on fftSizeFor at the same time.
    CHECK(fftSizeFor(m1, 2048000) == 2048, "Mode I at 2.048 MSPS = 2048-point FFT");
    CHECK(guardSamplesFor(m1, 2048000) == 504, "Mode I guard at the canonical rate = 504 samples");
    // A non-canonical rate still answers sensibly (the RTL-SDR's own 2.4 MSPS).
    CHECK(fftSizeFor(m1, 2400000) == 2400, "Mode I at 2.4 MSPS = 2400-point FFT");

    CHECK(modeByNumber(3) && modeByNumber(3)->carriers == 192, "mode lookup by number");
    CHECK(modeByNumber(5) == nullptr, "there is no mode V");

    if (fails == 0) printf("  all passed\n");
    else            printf("  %d FAILED\n", fails);
    return fails ? 1 : 0;
}
