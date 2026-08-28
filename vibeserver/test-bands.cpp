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


    std::printf("\nThe band plan follows the ITU region\n");
    {
        // ★★ A US server given Region 1 edges would refuse 7.25 MHz to an operator entitled to it,
        //    and a European one given Region 2 edges would offer 147 MHz that is not theirs. The
        //    differences are small in Hz and large in consequence.
        ok(ituRegion(52.0,  -0.9, true) == 1, "★ Northampton is Region 1");
        ok(ituRegion(40.7, -74.0, true) == 2, "★ New York is Region 2");
        ok(ituRegion(35.7, 139.7, true) == 3, "★ Tokyo is Region 3");
        ok(ituRegion(-33.9, 151.2, true) == 3, "Sydney is Region 3");
        ok(ituRegion(-23.5, -46.6, true) == 2, "São Paulo is Region 2");
        ok(ituRegion(-33.9,  18.4, true) == 1, "Cape Town is Region 1");
        ok(ituRegion(62.0, 129.7, true) == 1, "★ Siberia is Region 1, not 3 — Russia is R1 throughout");
        ok(ituRegion(0, 0, false) == 1, "no position falls back to Region 1");

        auto edge = [](int region, const char* id, bool wantLo) {
            for (const auto& b : namedBands(region)) if (std::string(b.id) == id)
                return wantLo ? b.lo : b.hi;
            return -1.0;
        };
        ok(edge(1, "40m", false) ==  7200000.0, "★ 40 m ends at 7.200 in Region 1");
        ok(edge(2, "40m", false) ==  7300000.0, "★ and at 7.300 in Region 2");
        ok(edge(1, "2m",  false) == 146000000.0, "★ 2 m ends at 146 in Region 1");
        ok(edge(2, "2m",  false) == 148000000.0, "★ and at 148 in Region 2");
        ok(edge(2, "mw",  false) == 1705000.0,  "★ medium wave runs to 1705 in Region 2");
        ok(edge(2, "fm",  true)  == 88000000.0, "FM starts at 88 in Region 2");
        ok(edge(1, "air", true)  == edge(2, "air", true),
           "★ where the regions agree there is ONE entry — the airband is the same everywhere");
        ok(edge(3, "80m", false) == 3900000.0, "80 m ends at 3.900 in Region 3");

        // The parser follows whatever region the server has been told it is in.
        defaultRegion() = 2;
        eq(parseList("40m"), "[[7000000,7300000]]", "★ a NAME resolves against the server's region");
        defaultRegion() = 1;
        eq(parseList("40m"), "[[7000000,7200000]]", "and back again");
    }

    // ── ★★★ PER-BAND GAIN CEILINGS ──────────────────────────────────────────────────────────
    {
        std::printf("\nGain ceilings\n");
        const auto rules = parseGainList("fm:250, 0-30M:400");
        ok(rules.size() == 2, "two rules parsed");
        ok(gainCapAt(rules, 96'600'000) == 250, "★ inside FM the FM ceiling applies");
        ok(gainCapAt(rules, 7'100'000)  == 400, "★ on HF the HF ceiling applies");
        ok(gainCapAt(rules, 450'000'000) == -1, "★ outside every rule there is NO ceiling");

        // ★★★ THE LOWEST WINS WHERE RULES OVERLAP — an owner who writes a tight FM limit and a
        //     broad catch-all means the tight one on FM. Reading it the other way would let the
        //     catch-all silently undo the limit written for the band that was overloading.
        const auto overlap = parseGainList("fm:250, 0-2000M:400");
        ok(gainCapAt(overlap, 96'600'000) == 250, "★★ the TIGHTER of two overlapping rules wins");
        ok(gainCapAt(overlap, 14'200'000) == 400, "and the broad one still applies elsewhere");

        // Rubbish must be skipped, not fatal, and must never read as "no limit anywhere".
        ok(parseGainList("").empty(), "an empty list is no rules");
        ok(parseGainList("fm").empty(), "★ a band with no value is not a rule");
        ok(parseGainList("nonsense:250").empty(), "★ an unknown band name is skipped");
        const auto mixed = parseGainList("rubbish, fm:250");
        ok(mixed.size() == 1 && gainCapAt(mixed, 96'600'000) == 250,
           "★★ one bad entry does not discard the good ones beside it");
        // ★ 0 is a REAL ceiling (minimum gain), not "unset" — the distinction matters because -1
        //   means no limit and 0 means the tightest possible one.
        const auto zero = parseGainList("fm:0");
        ok(zero.size() == 1 && gainCapAt(zero, 96'600'000) == 0, "★★ a ceiling of 0 is a ceiling");

        /* valueAt: the SAME parser read as a preference rather than a limit — the HackRF's LNA
         * share of a locked band's total, and the SDRplay's IF ceiling both ride on this list. */
        ok(valueAt(rules, 96'600'000) == 250, "★ valueAt reads the rule that covers the frequency");
        ok(valueAt(rules, 450'000'000) == -1, "★ and -1 where no rule covers it");
        // ★★ FIRST match wins here, where gainCapAt takes the LOWEST. A split is a preference, not
        //    a limit: "the smallest share of LNA" is not a safer answer, merely an arbitrary one,
        //    so the owner's own order is the tie-break. Same list, two deliberate readings.
        // Written broad-first, so "first" and "tightest" are genuinely different answers.
        const auto broadFirst = parseGainList("0-2000M:400, fm:250");
        ok(gainCapAt(broadFirst, 96'600'000) == 250 && valueAt(broadFirst, 96'600'000) == 400,
           "★★ overlapping rules: gainCapAt takes the tightest, valueAt takes the owner's first");
    }

    std::printf("\n%s%d checks\n", failures ? "FAILURES — " : "", checks);
    if (failures) std::printf("%d FAILED\n", failures);
    return failures ? 1 : 0;
}
