// test-moveloc.cpp — the moving location's pure parts (moveloc.h). No network, no radio.
#include "moveloc.h"
#include <cstdio>
#include <string>

static int g_fail = 0;
static void ok(bool c, const char* what) { std::printf("  %s  %s\n", c ? "ok  " : "FAIL", what); if (!c) g_fail++; }

int main() {
    using namespace vsmoveloc;
    std::printf("moveloc — the nearest town as a server moves\n");

    // ── NetworkManager's terse list → the BeaconDB body ──
    int n = 0;
    const std::string body = beaconBody("AA\\:BB\\:CC\\:DD\\:EE\\:01:80\nAA\\:BB\\:CC\\:DD\\:EE\\:02:40\n"
                                        "garbage line\n\\:\\:\\::50\n", n);
    ok(n == 2, "two real access points read, the junk lines skipped");
    ok(body.find("\"macAddress\":\"AA:BB:CC:DD:EE:01\",\"signalStrength\":-60") != std::string::npos,
       "escaped colons undone; 80 % → -60 dBm");
    ok(body.find("\"considerIp\":false") != std::string::npos, "the IP address is never used to locate");

    // ── village level (zoom 13), shaped like the real answers of 2026-10-11 ──
    Place t = placeFromNominatim(R"({"lat":"52.4887","lon":"-0.6968","addresstype":"suburb","name":"Corby Village",
        "address":{"suburb":"Corby Village","town":"Corby","county":"North Northamptonshire","state":"England","country_code":"gb"}})");
    ok(t.valid && t.label == "Corby" && !t.region, "in a town (as a suburb): the TOWN is the answer");
    ok(t.grid.size() == 6 && t.iso == "GB", "…6-character grid of that centre, country GB");
    Place mo = placeFromNominatim(R"({"lat":"52.29","lon":"-0.85","addresstype":"village","name":"Moulton",
        "address":{"village":"Moulton","county":"West Northamptonshire","state":"England","country_code":"gb"},"extratags":{}})");
    ok(!mo.valid, "a village of UNKNOWN size is not large (Moulton, Holcot): ask for the region");
    Place big = placeFromNominatim(R"({"lat":"52.35","lon":"-0.90","addresstype":"village","name":"Bigvillage",
        "address":{"village":"Bigvillage","country_code":"gb"},"extratags":{"population":"4200"}})");
    ok(big.valid && big.label == "Bigvillage", "a village OSM records at 1,000+ people: named");
    Place sm = placeFromNominatim(R"({"lat":"52.35","lon":"-0.90","addresstype":"village","name":"Tiny",
        "address":{"village":"Tiny","country_code":"gb"},"extratags":{"population":"120"}})");
    ok(!sm.valid, "a village of 120 is never named (a cabin beside it would be found)");
    Place gh = placeFromNominatim(R"({"lat":"-24.9","lon":"133.4","addresstype":"city_district","name":"Ghan",
        "address":{"city_district":"Ghan","state":"Northern Territory","country_code":"au"},"extratags":{"population":"388"}})");
    ok(!gh.valid, "the outback's 'Ghan' district is not a town: ask for the region");

    // ── county level (zoom 8): the general region ──
    Place r = regionFromNominatim(R"({"lat":"57.5","lon":"-4.9","addresstype":"county","name":"Highland",
        "address":{"county":"Highland","state":"Scotland","country_code":"gb"}})");
    ok(r.valid && r.region && r.label == "Highland" && r.grid.size() == 4, "Knoydart: the region 'Highland', 4-character grid");
    Place nt = regionFromNominatim(R"({"lat":"-19.4","lon":"133.4","addresstype":"state","name":"Northern Territory",
        "address":{"state":"Northern Territory","country":"Australia","country_code":"au"}})");
    ok(nt.valid && nt.label == "Northern Territory" && nt.iso == "AU", "the outback: 'Northern Territory'");

    // ── nothing ──
    ok(!placeFromNominatim(R"({"error":"Unable to geocode"})").valid && !regionFromNominatim("").valid,
       "no answer: nothing (the last place stays)");

    // ── a hostile name cannot break the hand-built JSON downstream ──
    Place h = placeFromNominatim(R"({"lat":"1","lon":"1","addresstype":"city","name":"x",
        "address":{"city":"Bad\"Town\\","country_code":"zz"}})");
    ok(h.valid && h.label.find('"') == std::string::npos && h.label.find('\\') == std::string::npos,
       "quotes and backslashes are stripped from a place name");

    // ── the grid ──
    ok(gridOf(52.2604, -0.8650, 6) == "IO92ng", "Northampton centre → IO92ng");
    ok(gridOf(52.3000, -0.8500, 6) == "IO92nh", "Moulton → IO92nh");
    ok(gridOf(52.2604, -0.8650, 4) == "IO92", "…and IO92 at 4 characters");

    std::printf(g_fail ? "\n%d failed\n" : "\nall passed\n", g_fail);
    return g_fail ? 1 : 0;
}
