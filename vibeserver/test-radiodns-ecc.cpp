// test-radiodns-ecc.cpp — the ECC a station's country and PI nibble imply.
//
// ★★★ WHY THIS EXISTS. The RadioDNS FQDN cannot be built without an ECC, and the ECC rides in RDS
//     group 1A — which a great many transmitters never send. On one receiver, on one afternoon,
//     BBC Radio 1 (1A at 11% of its groups) showed its real artwork while Heart on 96.6 showed a
//     generic favicon, purely because no 1A group had been decoded yet; the SAME station on 103.3,
//     where a 1A had arrived, showed the proper logo (Stuart, 2026-08-14, three screenshots). So
//     the browser now sends no ECC and the server derives one.
//
// ★★ THE STRONGEST ASSERTION HERE IS THE AGREEMENT ONE: for both stations the derived value is
//    E1, which is exactly what those transmitters actually broadcast. A derivation that disagreed
//    with the wire would be worse than none — it would resolve to the wrong broadcaster.
//
// ★ And the refusals matter as much as the answers. Given a country that does NOT sit at this PI's
//   country nibble, this must return nothing rather than pick something: a wrong GCC resolves to
//   no station, which is indistinguishable from "not in RadioDNS" and sends the next person
//   hunting for a fault that is not there.

#include "radiodns.h"

#include <cstdio>
#include <string>

static int fails = 0;

static void ck(const char* iso, const char* pi, const char* want, const char* why) {
    const std::string got = vsradiodns::eccForIso(iso, pi);
    const bool ok = (got == want);
    if (!ok) ++fails;
    std::printf("  %s   %-3s %-5s -> %-3s   %s\n",
                ok ? "ok  " : "FAIL", iso, pi,
                got.empty() ? "--" : got.c_str(), why);
}

int main() {
    std::printf("RadioDNS: the ECC a country and PI nibble imply\n");

    // ── It agrees with what the transmitter actually said ──────────────────────────────────────
    ck("GB", "C363", "E1", "Heart — derived E1, which is what 103.3 transmits");
    ck("GB", "C201", "E1", "BBC R1 — derived E1, which is what it transmits");
    ck("gb", "c363", "E1", "case-insensitive, both arguments");

    // ── Other countries, other nibbles ────────────────────────────────────────────────────────
    ck("DE", "D3AB", "E0", "Germany at nibble D");
    ck("FR", "F201", "E1", "France at nibble F");
    // ★ I FIRST ASSERTED IRELAND AT NIBBLE E AND THE TEST FAILED — the TEST was wrong, not the
    //   table: row E3 holds Ireland at nibble 2, and nibble E of that row is SWEDEN. Recorded
    //   because the reflex on a red test is to "fix" the code, and here that would have broken a
    //   correct lookup to satisfy a wrong expectation.
    ck("IE", "2201", "E3", "Ireland at nibble 2 — NOT nibble E, which is Sweden");
    ck("SE", "E301", "E3", "and Sweden really is the nibble-E entry of that row");

    // ── It declines rather than guesses ───────────────────────────────────────────────────────
    ck("GB", "1234", "", "nibble 1 is not GB — say nothing, do not pick");
    ck("ZZ", "C363", "", "unknown country");
    ck("G",  "C363", "", "an ITU code is not an ISO one");
    ck("GB", "",     "", "no PI");
    ck("",   "C363", "", "no country");
    ck("GB", "ZZZZ", "", "a PI that is not hex");

    // ── The candidates, which is what actually runs when no ECC is transmitted ────────────────
    // ★★★ THE COUNTRY-DERIVED VERSION FAILED IN THE FIELD FOR THE DULLEST POSSIBLE REASON: the
    //     demo server has NO COUNTRY CONFIGURED, so eccForIso() returned nothing and the whole
    //     feature silently did nothing at all ("RadioDNS is not showing up any icons anymore").
    //     Every test here passed throughout — because every one of them PASSED A COUNTRY IN. The
    //     untested path was the one every real server takes.
    std::printf("\nECC candidates (what runs when the station sends no ECC)\n");
    {
        const auto withIso = vsradiodns::eccCandidates("C363", "GB");
        const auto noIso   = vsradiodns::eccCandidates("C363", "");
        std::printf("   with GB: %zu candidates, first %s\n", withIso.size(),
                    withIso.empty() ? "--" : withIso[0].c_str());
        std::printf("   with no country: %zu candidates\n", noIso.size());
        if (!(withIso.size() > 1 && withIso[0] == "E1")) {
            ++fails; std::printf("  FAIL the receiver's own country must be tried FIRST\n");
        } else std::printf("  ok   the receiver's own country is tried FIRST\n");
        if (noIso.size() < 5) {
            ++fails; std::printf("  FAIL NO COUNTRY must still produce candidates\n");
        } else std::printf("  ok   ★★★ NO COUNTRY still produces candidates — the case that shipped broken\n");
        bool hasE1 = false;
        for (const auto& c : noIso) if (c == "E1") hasE1 = true;
        if (!hasE1) { ++fails; std::printf("  FAIL E1 missing from the nibble-C candidates\n"); }
        else std::printf("  ok   E1 is among them, so a GB station resolves from any server\n");
    }

    std::printf(fails ? "FAILED %d\n" : "all good\n", fails);
    return fails ? 1 : 0;
}
