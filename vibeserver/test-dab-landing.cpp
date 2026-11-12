// test-dab-landing.cpp — which multiplex DAB opens on when nobody named one (vibe_dab_landing.h).
//
// ★★★ Stuart, 2026-11-12: "for other countries 12B may be dead". The rule's ORDER (last → scan → country → 12B), the
//     country table, and the position lookup's CONSERVATISM: a point near a border must answer "don't know", never a
//     neighbour's block. The server end to end is scripts/test-server-dab-landing-block.mjs.
#include "vibe_dab_landing.h"
#include <cmath>
#include <cstdio>

using namespace vibedabland;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("  FAIL: %s\n", msg); ++fails; } } while (0)

int main() {
    // ── The table ──
    CHECK(nationalFor("GB") && std::string(nationalFor("GB")->block) == "12B", "GB → 12B (BBC National)");
    CHECK(nationalFor("gb") && std::string(nationalFor("gb")->block) == "12B", "any case");
    CHECK(nationalFor("UK") && std::string(nationalFor("UK")->block) == "12B", "UK is read as GB");
    CHECK(nationalFor("DE") && std::string(nationalFor("DE")->block) == "5C", "DE → 5C (1. Bundesmux)");
    CHECK(nationalFor("NL") && std::string(nationalFor("NL")->block) == "12C", "NL → 12C (NPO)");
    CHECK(!nationalFor("NO") && !nationalFor("CH") && !nationalFor("IT") && !nationalFor("FR") && !nationalFor("AU"),
          "regional or unconfirmed countries are left out");
    CHECK(!nationalFor("") && !nationalFor("GBR") && !nationalFor("US"), "no entry for empty, three letters, or no DAB");
    for (const auto& n : nationals()) CHECK(n.source && *n.source, "every row names its source");
    for (const auto& b : boxes()) CHECK(nationalFor(b.iso) != nullptr, "every box belongs to a country in the table");

    // ── Positions: the cities, and the borders that must NOT answer ──
    CHECK(isoFromLatLon(51.507, -0.128) == "GB", "London");
    CHECK(isoFromLatLon(55.953, -3.188) == "GB", "Edinburgh");
    CHECK(isoFromLatLon(54.597, -5.930) == "GB", "Belfast");
    CHECK(isoFromLatLon(53.48, -2.24) == "GB", "Manchester");
    CHECK(isoFromLatLon(52.520, 13.405) == "DE", "Berlin");
    CHECK(isoFromLatLon(48.137, 11.575) == "DE", "Munich");
    CHECK(isoFromLatLon(50.110, 8.682) == "DE", "Frankfurt");
    CHECK(isoFromLatLon(53.551, 9.993) == "DE", "Hamburg");
    CHECK(isoFromLatLon(52.370, 4.895) == "NL", "Amsterdam");
    CHECK(isoFromLatLon(51.924, 4.478) == "NL", "Rotterdam");
    CHECK(isoFromLatLon(53.219, 6.567) == "NL", "Groningen");
    CHECK(isoFromLatLon(50.72, 1.61).empty(), "Boulogne-sur-Mer is not GB");
    CHECK(isoFromLatLon(50.95, 1.86).empty(), "Calais is not GB");
    CHECK(isoFromLatLon(53.35, -6.26).empty(), "Dublin is not GB");
    CHECK(isoFromLatLon(55.13, -7.45).empty(), "Inishowen (Donegal) is not GB");
    CHECK(isoFromLatLon(48.97, 8.18).empty(), "Lauterbourg (France, on the Rhine) is not DE");
    CHECK(isoFromLatLon(49.04, 7.94).empty(), "Wissembourg is not DE");
    CHECK(isoFromLatLon(52.21, 7.02).empty(), "Gronau's border with Enschede is not decided either way");
    CHECK(isoFromLatLon(51.22, 4.40).empty(), "Antwerp is not NL");
    CHECK(isoFromLatLon(47.56, 7.59).empty(), "Basel is not DE");
    CHECK(isoFromLatLon(48.21, 16.37).empty(), "Vienna: no table entry");
    CHECK(isoFromLatLon(40.71, -74.0).empty(), "New York: no table entry");
    CHECK(isoFromLatLon(NAN, NAN).empty(), "no position: nothing");

    // ── The rule's order ──
    auto all = [](const std::string&) { return true; };
    auto no11 = [](const std::string& b) { return b.rfind("11", 0) != 0; };
    const std::vector<ScanBlock> scan = { {"11D", 12.0f, 0.9f, 8, true}, {"12B", 18.5f, 0.99f, 11, true}, {"10C", 22.0f, 1.0f, 0, true} };
    Choice c = choose("10D", scan, "DE", NAN, NAN, all);
    CHECK(c.block == "10D" && c.why == Why::Last, "1. the last block wins");
    c = choose("11A", scan, "DE", NAN, NAN, no11);
    CHECK(c.block == "12B" && c.why == Why::Scan, "…unless this radio no longer reaches it: then the scan");
    c = choose("", scan, "DE", NAN, NAN, all);
    CHECK(c.block == "12B" && c.why == Why::Scan, "2. the strongest scanned block (best MER, with stations)");
    CHECK(strongest({ {"10C", 22.0f, 1.0f, 0, true} }).empty(), "a block with no stations is never 'strongest'");
    c = choose("", {}, "DE", NAN, NAN, all);
    CHECK(c.block == "5C" && c.why == Why::Country, "3. the configured country");
    c = choose("", {}, "", 52.52, 13.405, all);
    CHECK(c.block == "5C" && c.why == Why::Country, "3. …else the position (Berlin → 5C)");
    c = choose("", {}, "FR", 52.52, 13.405, all);
    CHECK(c.block == "12B" && c.why == Why::Fallback, "a configured country outranks the position, even with no entry");
    c = choose("", {}, "", 40.71, -74.0, all);
    CHECK(c.block == "12B" && c.why == Why::Fallback, "4. 12B as the last resort");
    CHECK(countryKnown("DE", NAN, NAN) && countryKnown("", 52.37, 4.895) && !countryKnown("US", 52.52, 13.4)
          && !countryKnown("", NAN, NAN), "countryKnown (the box's 'no scan yet' line)");

    std::printf("%s\n", fails ? "test-dab-landing: FAILED" : "test-dab-landing: all passed");
    return fails ? 1 : 0;
}
