// Does our band-condition model agree with UberSDR's, on the same solar numbers?
//
// ★★★ WHY A REFERENCE TEST AND NOT AN EYEBALL. "Good" and "Excellent" are judgement calls, so the
//     only way to know ours are not systematically pessimistic is to compare them with a model
//     people already use and trust. The first version of ours was a band or two low — 160/80/60m
//     read "Good" at night where UberSDR (and every operator) says "Excellent".
// ★★ The reference is a CAPTURED SNAPSHOT, deliberately: UberSDR's table at SFI 158 / K 1, taken
//    2026-08-06. Fetching it live would make this test depend on somebody else's server being up
//    and on the sun not moving, and it would stop being a regression test at all.
// ★ Agreement is not the goal — CALIBRATION is. A disagreement here is a prompt to look, not
//   automatically a bug: if we ever deliberately diverge, update the table and say why.
//
//   c++ -std=c++17 -I vibeserver tools/bandmodel-check.cpp vibeserver/solar.cpp -o /tmp/bmc
#include "solar.h"
#include <cstdio>
#include <cstring>

int main() {
    // UberSDR /api/spaceweather, reading-ubersdr.m0lte.uk, 2026-08-06, SFI 158, K 1.
    struct Row { const char* band; const char* day; const char* night; };
    const Row ref[] = {
        {"160m","Poor",     "Excellent"},
        {"80m", "Fair",     "Excellent"},
        {"60m", "Good",     "Excellent"},
        {"40m", "Good",     "Excellent"},
        {"30m", "Good",     "Good"},
        {"20m", "Excellent","Fair"},
        {"17m", "Good",     "Poor"},
        {"15m", "Good",     "Poor"},
        {"12m", "Good",     "Poor"},
        {"10m", "Good",     "Poor"},
    };
    vssolar::Solar s; s.sfi = 158; s.kp = 1;

    int agree = 0, total = 0;
    printf("  %-5s  %-22s  %-22s\n", "band", "DAY  ours / UberSDR", "NIGHT  ours / UberSDR");
    for (const auto& r : ref) {
        const std::string d = vssolar::bandVerdict(s, r.band, true);
        const std::string n = vssolar::bandVerdict(s, r.band, false);
        const bool dOk = d == r.day, nOk = n == r.night;
        agree += (dOk ? 1 : 0) + (nOk ? 1 : 0);
        total += 2;
        printf("  %-5s  %-10s %-10s %s  %-10s %-10s %s\n",
               r.band, d.c_str(), r.day, dOk ? " " : "<", n.c_str(), r.night, nOk ? " " : "<");
    }
    printf("\n  %d/%d agree\n", agree, total);
    const bool ok = agree == total;
    printf("%s\n", ok ? "PASS — our verdicts match the reference model."
                      : "FAIL — we disagree with the reference; '<' marks where.");
    return ok ? 0 : 1;
}
