// test-dab-superframe.cpp — the two bits that decide the DAB+ sample rate.
//
// ★★★ THIS TEST EXISTS BECAUSE OF A SHIPPED BUG IN SOMEBODY ELSE'S RECEIVER. Stuart, 2026-09-04:
//     "OWRX has mismatched sample rates hence the chipmunks." A wrong rate here does not fail, it
//     just plays fast — so nothing catches it except somebody listening, and by then it has
//     shipped. The four cases are exhaustive; pin all four.
#include "vibe_dab_superframe.h"
#include <cstdio>

using namespace vibedab;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); ++fails; } } while (0)

int main() {
    printf("test-dab-superframe\n");

    // TS 102 563 table 2, all four combinations.
    AudioFormat a = dabPlusFormat(false, true);
    CHECK(a.coreRateHz == 16000 && a.outputRateHz == 32000 && a.accessUnits == 2 && a.sbr,
          "dac_rate=0 sbr=1: core 16k, out 32k, 2 AUs");
    AudioFormat b = dabPlusFormat(true, true);
    CHECK(b.coreRateHz == 24000 && b.outputRateHz == 48000 && b.accessUnits == 3 && b.sbr,
          "dac_rate=1 sbr=1: core 24k, out 48k, 3 AUs");
    AudioFormat c = dabPlusFormat(false, false);
    CHECK(c.coreRateHz == 32000 && c.outputRateHz == 32000 && c.accessUnits == 4 && !c.sbr,
          "dac_rate=0 sbr=0: 32k, 4 AUs");
    AudioFormat d = dabPlusFormat(true, false);
    CHECK(d.coreRateHz == 48000 && d.outputRateHz == 48000 && d.accessUnits == 6 && !d.sbr,
          "dac_rate=1 sbr=0: 48k, 6 AUs");

    // ★★ THE TWO CHIPMUNKS, stated as ratios so the failure is named rather than implied.
    //    1) Assuming 48 kHz for a 32 kHz service plays 1.5x fast.
    CHECK(48000.0 / c.outputRateHz == 1.5, "assuming 48k on a 32k service = 1.5x = chipmunks");
    //    2) Taking the CORE rate as the output rate when SBR is on plays 2x fast.
    CHECK(a.outputRateHz / a.coreRateHz == 2 && b.outputRateHz / b.coreRateHz == 2,
          "SBR doubles: core is NOT the playback rate");

    // ★★★ The ADTS/ASC header we synthesise for the browser describes the CORE rate. Writing the
    //     output rate there is us committing the 2x chipmunk in the one place we still touch DAB+.
    CHECK(mpeg4SamplingFrequencyIndex(a.coreRateHz) == 8,  "16 kHz core -> index 8");
    CHECK(mpeg4SamplingFrequencyIndex(b.coreRateHz) == 6,  "24 kHz core -> index 6");
    CHECK(mpeg4SamplingFrequencyIndex(c.coreRateHz) == 5,  "32 kHz -> index 5");
    CHECK(mpeg4SamplingFrequencyIndex(d.coreRateHz) == 3,  "48 kHz -> index 3");
    CHECK(mpeg4SamplingFrequencyIndex(a.outputRateHz) != mpeg4SamplingFrequencyIndex(a.coreRateHz),
          "core and output indices DIFFER under SBR — that is the trap");
    CHECK(mpeg4SamplingFrequencyIndex(1234) == -1, "an unenumerated rate is rejected, not guessed");

    // Plain DAB, which we decode ourselves.
    CHECK(isValidLayerIIRate(48000) && isValidLayerIIRate(24000), "Layer II is 48k or 24k");
    CHECK(!isValidLayerIIRate(32000), "32 kHz is not a Layer II rate");

    if (fails == 0) printf("  all passed\n");
    else            printf("  %d FAILED\n", fails);
    return fails ? 1 : 0;
}
