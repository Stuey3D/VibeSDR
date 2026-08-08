// ★★★ WHAT THE OWNER SAID, AND NOTHING ELSE. This decides where a public receiver may be tuned,
//     so both kinds of mistake matter and they are not symmetrical: letting a listener onto a band
//     the owner blocked could be a legal problem for them, and blocking one they allowed takes
//     their receiver off the air for no reason they can see. Most of this file is therefore about
//     combinations — an allow list AND a block list, overlapping entries, reversed pairs — because
//     a single allow entry is the case that was always going to work.
#include "vibe_bands.h"

#include <cstdio>
#include <string>

using namespace vibebands;

static int failures = 0, checks = 0;
static void ok(bool cond, const char* what, const std::string& extra = "") {
    checks++;
    if (cond) { std::printf("   ok   %s\n", what); return; }
    failures++;
    std::printf("   FAIL %s %s\n", what, extra.c_str());
}
static std::string show(const Ranges& r) { return toJson(r); }
static void eq(const Ranges& got, const std::string& want, const char* what) {
    ok(toJson(got) == want, what, "got " + show(got) + " want " + want);
}

/** An Airspy HF+: HF, then a hole, then VHF — the real shape this has to survive. */
static Ranges airspy() { return { {500, 31000000}, {60000000, 260000000} }; }
static Ranges wide()   { return { {0, 2000000000} }; }

int main() {
    std::printf("\nReading what the owner typed\n");
    {
        eq(parseList("87.5MHz-108MHz"), "[[87500000,108000000]]", "a typed range with units");
        eq(parseList("fm"), "[[87500000,108000000]]", "★ a named band");
        eq(parseList("530k-1710k"), "[[530000,1710000]]", "kHz suffix");
        eq(parseList("7074000-7100000"), "[[7074000,7100000]]", "bare numbers are Hz");
        eq(parseList("108MHz-87.5MHz"), "[[87500000,108000000]]",
           "★ a reversed pair is read as intended, not discarded");
        eq(parseList("fm, air"), "[[87500000,108000000],[108000000,137000000]]",
           "two entries");
        eq(parseList(""), "[]", "nothing");
        eq(parseList("not a band, 87.5MHz-108MHz"), "[[87500000,108000000]]",
           "★ one bad entry does not throw away the good ones");
        eq(parseList("87.5MHz"), "[]", "a single frequency is not a range");
    }

    std::printf("\nAn empty allow list means EVERYTHING, never nothing\n");
    {
        // ★★★ The owner who only wanted to block the airband must not lose their receiver.
        eq(permitted(airspy(), "", ""), "[[500,31000000],[60000000,260000000]]",
           "★ no lists at all = the hardware's own coverage");
        eq(permitted(airspy(), "", "air"),
           "[[500,31000000],[60000000,108000000],[137000000,260000000]]",
           "★ block only: everything else stays reachable");
    }

    std::printf("\nAllow, block, and both together\n");
    {
        eq(permitted(wide(), "fm", ""), "[[87500000,108000000]]", "allow one band");
        eq(permitted(wide(), "mw, fm", ""), "[[526500,1606500],[87500000,108000000]]",
           "★ AM and FM broadcast, the two-entry case");
        eq(permitted(wide(), "fm", "100MHz-102MHz"),
           "[[87500000,100000000],[102000000,108000000]]",
           "★ a block carves a hole out of an allowed band");
        eq(permitted(wide(), "", "air"), "[[0,108000000],[137000000,2000000000]]",
           "block the airband on a wide radio");
        eq(permitted(wide(), "fm", "fm"), "[]",
           "★ blocking exactly what you allowed leaves nothing — and says so");
    }

    std::printf("\nThe hardware always wins\n");
    {
        // An owner may allow 40 MHz on an HF+; it still cannot hear it.
        eq(permitted(airspy(), "40MHz-50MHz", ""), "[]",
           "★ allowing what the radio cannot reach yields nothing, not a lie");
        eq(permitted(airspy(), "20MHz-70MHz", ""), "[[20000000,31000000],[60000000,70000000]]",
           "★ an allowed span across the tuning hole keeps only the reachable parts");
    }

    std::printf("\nOverlaps are merged, not double-counted\n");
    {
        eq(permitted(wide(), "1MHz-5MHz, 4MHz-8MHz", ""), "[[1000000,8000000]]",
           "overlapping allows merge");
        eq(permitted(wide(), "1MHz-5MHz, 5MHz-8MHz", ""), "[[1000000,8000000]]",
           "touching allows merge");
        eq(permitted(wide(), "1MHz-10MHz", "3MHz-4MHz, 6MHz-7MHz"),
           "[[1000000,3000000],[4000000,6000000],[7000000,10000000]]",
           "★ two holes in one band");
        eq(permitted(wide(), "1MHz-10MHz", "0-100MHz"), "[]",
           "a block that swallows the allow");
    }

    std::printf("\nAnswering the question the radio actually asks\n");
    {
        const Ranges p = permitted(wide(), "fm", "100MHz-102MHz");
        ok(allows(p, 96600000),  "★ an allowed frequency is allowed");
        ok(!allows(p, 101000000), "★ one inside the hole is not");
        ok(!allows(p, 120000000), "★ and one outside the allow list is not");
        ok(clamp(p, 101000000) == 100000000 || clamp(p, 101000000) == 102000000,
           "★ a blocked frequency clamps to an edge of the hole",
           std::to_string((long long)clamp(p, 101000000)));
        ok(clamp(p, 96600000) == 96600000, "an allowed one is left alone");
        // ★ With nothing permitted there is no sane place to send anybody, so the request is
        //   returned unchanged rather than snapped to a fabricated edge.
        ok(clamp(Ranges{}, 96600000) == 96600000, "an empty set clamps to nothing");
    }

    std::printf("\n%s%d checks\n", failures ? "FAILURES — " : "", checks);
    if (failures) std::printf("%d FAILED\n", failures);
    return failures ? 1 : 0;
}
