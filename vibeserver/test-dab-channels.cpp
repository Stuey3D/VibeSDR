// test-dab-channels.cpp — the Band III plan and the DAB capability gate.
//
// ★ Header-only and device-free, like test-utf8: the rules here belong to the BAND and to the
//   radio's capability, not to any one driver, so the test must run with no hardware present.
#include "vibe_dab_channels.h"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace vibedab;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); ++fails; } } while (0)

int main() {
    printf("test-dab-channels\n");

    // ── The table itself ─────────────────────────────────────────────────────
    CHECK(kBandIIICount == 41, "Band III: 38 lettered blocks plus 10N, 11N and 12N");

    // ★★ THE OFFSET BLOCKS. Missing from the first draft; Stuart's own block list caught it. A
    //    receiver that cannot step onto 11N silently skips a multiplex that is on air.
    CHECK(channelByName("10N") && channelByName("10N")->centreHz == 210096000, "10N = 210.096");
    CHECK(channelByName("11N") && channelByName("11N")->centreHz == 217088000, "11N = 217.088");
    CHECK(channelByName("12N") && channelByName("12N")->centreHz == 224096000, "12N = 224.096");

    // ★★ The one everybody quotes, and the one on Stuart's screen: 11D = 222.064 MHz.
    const Channel* c11d = channelByName("11D");
    CHECK(c11d && c11d->centreHz == 222064000, "11D must be 222.064 MHz");
    CHECK(channelByName("11d") == c11d, "channel lookup is case-insensitive");
    CHECK(channelByName("11E") == nullptr, "11E does not exist");
    CHECK(channelByName(nullptr) == nullptr, "a null name is not a channel");

    // ★★★ THE POINT OF A TABLE. Spot-check the irregularities a formula would get wrong: the step
    //     INSIDE a group is 1.712 MHz, but the step ACROSS a group boundary is not, and 13D->13E
    //     breaks again at the top.
    const Channel* c11a = channelByName("11A");
    const Channel* c11b = channelByName("11B");
    CHECK(c11b->centreHz - c11a->centreHz == 1712000, "within a group the step is 1.712 MHz");
    const Channel* c10d = channelByName("10D");
    CHECK(c11a->centreHz - c10d->centreHz != 1712000, "the step ACROSS a group is NOT 1.712 MHz");
    const Channel* c13d = channelByName("13D");
    const Channel* c13e = channelByName("13E");
    CHECK(c13e->centreHz - c13d->centreHz == 1712000, "13D->13E");
    CHECK(channelByName("13C")->centreHz + 1568000 == c13d->centreHz, "13C->13D is the odd one");

    // Monotonic and unique — a duplicate or an out-of-order entry would make nearestChannel lie.
    for (size_t i = 1; i < kBandIIICount; ++i)
        CHECK(kBandIII[i].centreHz > kBandIII[i-1].centreHz, "channels ascend and are unique");

    // ── nearestChannel ───────────────────────────────────────────────────────
    CHECK(strcmp(kBandIII[nearestChannel(222064000)].name, "11D") == 0, "exact centre -> 11D");
    CHECK(strcmp(kBandIII[nearestChannel(222000000)].name, "11D") == 0, "just below -> 11D");
    CHECK(strcmp(kBandIII[nearestChannel(0)].name, "5A") == 0, "far below the band -> the lowest");
    CHECK(strcmp(kBandIII[nearestChannel(4000000000u)].name, "13F") == 0, "far above -> the highest");

    // ── stepChannel: the tuning buttons in DAB mode ──────────────────────────
    int i11d = nearestChannel(222064000);
    CHECK(strcmp(kBandIII[stepChannel(i11d, +1)].name, "12A") == 0, "11D + 1 = 12A");
    CHECK(strcmp(kBandIII[stepChannel(i11d, -1)].name, "11C") == 0, "11D - 1 = 11C");
    // ★ Stepping up from 11A must land on 11N, not jump over it to 11B.
    int i11a = nearestChannel(216928000);
    CHECK(strcmp(kBandIII[stepChannel(i11a, +1)].name, "11N") == 0, "11A + 1 = 11N, not 11B");
    CHECK(strcmp(kBandIII[stepChannel(i11a, +2)].name, "11B") == 0, "11A + 2 = 11B");
    // ★ CLAMPS, never wraps — 13F -> 5A in one press is not what a band scan meant.
    CHECK(stepChannel(0, -1) == 0, "stepping below 5A stays on 5A");
    CHECK(stepChannel(int(kBandIIICount) - 1, +1) == int(kBandIIICount) - 1, "stepping past 13F stays");

    // ── The capability gate ──────────────────────────────────────────────────
    // RTL-SDR V4: 500 kHz - 1.766 GHz at 2.4 MSPS — capable.
    CHECK(radioCanDab(500000, 1766000000u, 2400000) == true, "RTL-SDR V4 can do DAB");
    // ★★★ Airspy HF+: reaches 31 MHz and captures ~912 kHz. BOTH halves disqualify it, and it is
    //     Stuart's own shared radio — the obvious test receiver is the wrong one.
    CHECK(radioCanDab(1000, 31000000, 912000) == false, "Airspy HF+ cannot do DAB");
    // A wide radio that cannot REACH Band III is still out (a hypothetical HF-only 2.5 MSPS rig).
    CHECK(radioCanDab(1000, 30000000, 2500000) == false, "wide but out of band = no DAB");
    // And one that reaches the band but cannot capture a whole mux is out — this is the case that
    // matters, because it LOOKS like it should work and demodulates to nothing.
    CHECK(radioCanDab(24000000, 1700000000u, 1024000) == false, "in band but too narrow = no DAB");
    // Touching the band edge without containing a channel centre is not enough.
    CHECK(radioCanDab(174000000, 174900000, 2400000) == false, "band edge without a channel = no");

    // ── THE OPERATOR'S RESTRICTIONS ARE PART OF THE ANSWER ───────────────────
    // Stuart, 2026-09-04: a restricted V4 must not offer the button at all. These are his three
    // radios and the two ways an operator disqualifies a capable one.
    CHECK(radioCanDab(500000, 1766000000u, 2400000) == true,  "V4 unrestricted: offered");
    CHECK(radioCanDab(500000, 1766000000u, 1200000) == false, "V4 with a LOCKED RATE below 2.048: not offered");
    CHECK(radioCanDab(87500000, 108000000, 2400000) == false, "V4 restricted to FM: not offered");
    // ★★ SDRplay RSP1B: sample rate is ample, the operator has locked it to 2.8-10.8 MHz. Out on
    //    range alone — the hardware is blameless, which is why the gate reads the EFFECTIVE set.
    CHECK(radioCanDab(2800000, 10800000, 8000000) == false, "RSP1B locked to 2.8-10.8 MHz: not offered");

    // Disjoint allowed ranges: a channel must fall INSIDE one of them, not between two.
    const Range gap[] = { {87500000, 108000000}, {225000000, 1700000000u} };
    CHECK(radioCanDab(gap, 2, 2400000) == true, "an allowed range containing 12B+ qualifies");
    const Range gap2[] = { {87500000, 108000000}, {240000000, 1700000000u} };
    CHECK(radioCanDab(gap2, 2, 2400000) == false, "allowed ranges either side of Band III do not");
    CHECK(radioCanDab(nullptr, 0, 2400000) == false, "no allowed ranges at all = no DAB");

    /* ★★★ THE PI'S OWN THREE RADIOS, AS THE SERVER ACTUALLY REPORTS THEM. The first version of the
     *  gate assumed every receiver could reach 2.048 MS/s and the live Pi answered `dab: true` for
     *  the Airspy HF+ — which tops out near 912 kHz and can see about a third of an ensemble. The
     *  range test alone does NOT exclude it, because the HF+ Discovery reaches 60-260 MHz and so
     *  covers Band III; only the RATE does. */
    {
        const Range hfPlus[] = { {500, 31000000}, {60000000, 260000000} };
        CHECK(radioCanDab(hfPlus, 2, 912000) == false,
              "★ Airspy HF+ REACHES Band III but caps at 912 kHz — refused on rate alone");
        CHECK(radioCanDab(hfPlus, 2, 2400000) == true,
              "…and the same ranges at a usable rate would qualify, so it is the rate that decides");
        const Range v4[] = { {500000, 1766000000} };
        CHECK(radioCanDab(v4, 1, 2400000) == true,  "RTL-SDR V4 unrestricted: offered");
        CHECK(radioCanDab(v4, 1, 1200000) == false, "V4 held below 2.048 MS/s: not offered");
        const Range rsp[] = { {2800000, 10800000} };
        CHECK(radioCanDab(rsp, 1, 8000000) == false, "RSP1B locked to 2.8-10.8 MHz: not offered");
    }

    // ── The readout Stuart specified ─────────────────────────────────────────
    CHECK(displayLabel(i11d) == "222.064 (11D)", "readout is 'frequency (channel)'");
    CHECK(displayLabel(-1).empty(), "an invalid index yields no label, not a crash");

    if (fails == 0) printf("  all passed\n");
    else            printf("  %d FAILED\n", fails);
    return fails ? 1 : 0;
}
