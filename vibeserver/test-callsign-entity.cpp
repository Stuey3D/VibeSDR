// test-callsign-entity.cpp — V12 beta92 the DIGI MODES MAP's false-decode filter (vibe_callsign_entity.h),
// docs/v12/DIGI-MAP-FILTER.md.
//
//  1. REAL CALLS AT REAL GRIDS PASS — every continent, the UK home nations, the US states and its islands (KH6, KL7 both
//     sides of the antimeridian, KP4, KH2, KH8, KG4, KC4 at McMurdo), Russia both sides (Kaliningrad, Chukotka), Japan's
//     far islands, Australia's (VK9, VK0 Heard / Macquarie / Davis), NZ's (Chatham, Campbell), CE0 Easter + Juan
//     Fernandez, 3Y Bouvet + Peter I, France's (FO, FK, FR, FT), the UK's (VP8, VP9, ZB2, ZD7, ZD8, ZD9, VQ9, VP6), /MM,
//     /AM, /P with another prefix, special-event series.
//  2. Stuart's Pi 500 false decodes (2026-11-22): 132XQA and 1H7IJH have no entity; 7Q8FAZ OB45, YE9HMP PC24, N6LCO
//     OC08, FQ2HMC PD93 are allocated calls at impossible grids → implausible (keep the station, drop the grid).
//  3. The whole ITU table: every two-character series A0…Z9 / 0A…9Z resolves to the administration it should (spot
//     checks), Q and leading-0/1 series resolve to nobody; random FT8-shaped calls mostly land nowhere or implausible.
//  4. Cost: judgements per second.
#include "../android/app/src/main/cpp/vibe_callsign_entity.h"

#include <chrono>
#include <cstdio>
#include <random>
#include <string>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

namespace ce = vibe::callentity;

struct Case { const char* call; const char* grid; const char* where; };

int main() {
    std::printf("── 1. real calls at real grids ──\n");
    static const Case ok[] = {
        { "G4ABC", "IO91", "England" }, { "M0XYZ", "IO83", "England" }, { "2E0ABC", "JO02", "England" },
        { "GM4ABC", "IO85", "Scotland" }, { "GI4ABC", "IO64", "N. Ireland" }, { "GW4ABC", "IO81", "Wales" },
        { "GU4ABC", "IN89", "Guernsey" }, { "GD4ABC", "IO74", "Isle of Man" }, { "MM0ABC", "IP90", "Shetland" },
        { "EI2ABC", "IO63", "Ireland" }, { "DL1ABC", "JO62", "Germany" }, { "F4ABC", "JN18", "France" },
        { "TK5ABC", "JN42", "Corsica" }, { "EA1ABC", "IN73", "Spain" }, { "EA8ABC", "IL18", "Canaries" },
        { "EA6ABC", "JM19", "Balearics" }, { "CT1ABC", "IM58", "Portugal" }, { "CU2ABC", "HM77", "Azores" },
        { "CT3ABC", "IM12", "Madeira" }, { "I2ABC", "JN45", "Italy" }, { "IS0ABC", "JM49", "Sardinia" },
        { "9H1ABC", "JM75", "Malta" }, { "SV1ABC", "KM18", "Greece" }, { "SV9ABC", "KM25", "Crete" },
        { "5B4ABC", "KM64", "Cyprus" }, { "TA1ABC", "KN41", "Turkey" }, { "4X1ABC", "KM72", "Israel" },
        { "OH1ABC", "KP20", "Finland" }, { "SM5ABC", "JO89", "Sweden" }, { "LA1ABC", "JO59", "Norway" },
        { "OZ1ABC", "JO65", "Denmark" }, { "OY1ABC", "IP62", "Faroes" }, { "OX3ABC", "GP44", "Greenland" },
        { "TF3ABC", "HP94", "Iceland" }, { "JW5ABC", "JQ78", "Svalbard" }, { "JX9ABC", "IQ51", "Jan Mayen" },
        { "PA3ABC", "JO22", "Netherlands" }, { "ON4ABC", "JO20", "Belgium" }, { "HB9ABC", "JN47", "Switzerland" },
        { "HB0ABC", "JN47", "Liechtenstein" }, { "OK1ABC", "JO70", "Czechia" }, { "SP5ABC", "KO02", "Poland" },
        { "UR5ABC", "KO50", "Ukraine" }, { "YO3ABC", "KN34", "Romania" }, { "LZ1ABC", "KN22", "Bulgaria" },
        // Russia both sides
        { "UA3ABC", "KO85", "Moscow" }, { "RA2ABC", "KO04", "Kaliningrad" }, { "UA9ABC", "MO15", "Novosibirsk" },
        { "UA0ABC", "PN53", "Vladivostok" }, { "RA0ABC", "RP84", "Anadyr" }, { "UA0ABC", "AP34", "Provideniya (W of 180)" },
        { "R1FJL", "LR90", "Franz Josef Land" },
        // North America
        { "W1AW", "FN31", "Connecticut" }, { "K1ABC", "FN42", "Massachusetts" }, { "W6ABC", "CM87", "California" },
        { "N0ABC", "EN34", "Minnesota" }, { "K5ABC", "EM12", "Texas" }, { "N7ABC", "DN17", "Idaho" },
        { "W4ABC", "EL94", "Florida Keys" }, { "AA1ABC", "FN42", "AA series" }, { "KH6ABC", "BL11", "Hawaii" },
        { "KL7ABC", "BP51", "Anchorage" }, { "AL7ABC", "AO11", "Adak" }, { "KL7ABC", "RO62", "Attu (E of 180)" },
        { "KP4ABC", "FK68", "Puerto Rico" }, { "KH2ABC", "QK23", "Guam" }, { "KH8ABC", "AH45", "American Samoa" },
        { "KG4ABC", "FK29", "Guantanamo" }, { "KC4AAA", "RB32", "McMurdo" }, { "KC4USV", "RB32", "McMurdo" },
        { "N6ABC", "RB32", "a home call at McMurdo" }, { "K1ABC", "JA00", "South Pole" },
        { "VE3ABC", "FN03", "Ontario" }, { "VY0ABC", "FP53", "Nunavut" }, { "VO1ABC", "GN37", "Newfoundland" },
        { "XE1ABC", "EK09", "Mexico" }, { "XF4ABC", "DK48", "Revillagigedo" },
        // Asia
        { "JA1ABC", "PM95", "Tokyo" }, { "JA6ABC", "PL36", "Okinawa" }, { "JD1ABC", "QL17", "Ogasawara" },
        { "JD1BMM", "QL74", "Minami-Torishima" }, { "HL1ABC", "PM37", "Korea" }, { "BY1ABC", "OM89", "Beijing" },
        { "BV2ABC", "PL05", "Taiwan" }, { "VR2ABC", "OL72", "Hong Kong" }, { "VU2ABC", "MK82", "India" },
        { "4S7ABC", "NJ06", "Sri Lanka" }, { "9N1ABC", "NL07", "Nepal" }, { "AP2ABC", "MM63", "Pakistan" },
        { "9M2ABC", "OJ03", "Malaysia" }, { "9V1ABC", "OJ11", "Singapore" }, { "YB1ABC", "OI33", "Jakarta" },
        { "DU1ABC", "PK04", "Manila" }, { "HS0ABC", "OK03", "Bangkok" }, { "XV2ABC", "OL21", "Hanoi" },
        { "A61ABC", "LL75", "UAE" }, { "HZ1ABC", "LL24", "Saudi Arabia" }, { "UN7ABC", "MN83", "Kazakhstan" },
        // Oceania
        { "VK2ABC", "QF56", "Sydney" }, { "VK6ABC", "OF78", "Perth" }, { "VK9NABC", "RG30", "Norfolk" },
        { "VK0EK", "MD66", "Heard Island" }, { "VK0ABC", "QD95", "Macquarie" }, { "VK0ABC", "MC81", "Davis" },
        { "ZL1ABC", "RF73", "Auckland" }, { "ZL7ABC", "AE16", "Chatham (W of 180)" }, { "ZL9ABC", "RD47", "Campbell" },
        { "3D2ABC", "RH91", "Fiji (Suva)" }, { "3D2ABC", "AH02", "Fiji (Lau, W of 180)" }, { "T32ABC", "BJ11", "Kiritimati" },
        { "T30ABC", "RJ61", "Tarawa" }, { "FO5ABC", "BH52", "Tahiti" }, { "FO5ABC", "BI91", "Marquesas" },
        { "FK8ABC", "RG37", "New Caledonia" }, { "P29ABC", "QI30", "Port Moresby" }, { "E51ABC", "BG08", "Rarotonga" },
        // South America and the South Atlantic
        { "LU1ABC", "GF05", "Buenos Aires" }, { "LU1ZA", "GC16", "Esperanza" }, { "CE3ABC", "FF46", "Santiago" },
        { "CE0YABC", "DG52", "Easter Island" }, { "CE0ZABC", "FF06", "Juan Fernandez" }, { "PY2ABC", "GG66", "Sao Paulo" },
        { "PY0FABC", "HI36", "Fernando de Noronha" }, { "VP8ABC", "GD18", "Falklands" }, { "VP8ABC", "HD15", "South Georgia" },
        { "VP8ABC", "GC79", "Signy (South Orkney)" }, { "3Y0J", "JD15", "Bouvet" }, { "3Y0Z", "EC41", "Peter I" },
        { "ZD7ABC", "IH74", "St Helena" }, { "ZD8ABC", "II22", "Ascension" }, { "ZD9ABC", "IF32", "Tristan da Cunha" },
        { "VP9ABC", "FM72", "Bermuda" },
        // Africa and the Indian Ocean
        { "ZS1ABC", "JF96", "Cape Town" }, { "ZS8ABC", "KE83", "Marion Island" }, { "5Z4ABC", "KI88", "Kenya" },
        { "5N0ABC", "JJ16", "Lagos" }, { "7Q7ABC", "KH66", "Malawi" }, { "7X2ABC", "JM16", "Algiers" },
        { "SU1ABC", "KL30", "Cairo" }, { "CN8ABC", "IM63", "Morocco" }, { "FR5ABC", "LG78", "Reunion" },
        { "FT5XO", "ME50", "Kerguelen" }, { "FT5WQ", "LE53", "Crozet" }, { "FT4YM", "QC03", "Dumont d'Urville" },
        { "FT5ZM", "MF82", "Amsterdam Island" }, { "VQ9ABC", "MI62", "Chagos" }, { "VP6ABC", "CG44", "Pitcairn" },
        { "3B8ABC", "LG89", "Mauritius" }, { "8Q7ABC", "MJ63", "Maldives" },
        // portable, maritime, special
        { "EA8/G4ABC", "IL18", "EA8/ prefix" }, { "G4ABC/EA8", "IL18", "/EA8 suffix" }, { "F/G4ABC", "JN18", "F/ prefix" },
        { "G4ABC/P", "IO91", "/P" }, { "W1AW/7", "DN17", "/7 district" }, { "KH6/W1AW", "BL11", "KH6/ prefix" },
        { "MM/W1AW", "IO85", "MM/ = in Scotland" }, { "G4ABC/MM", "GG00", "maritime mobile mid-Atlantic" },
        { "K1ABC/AM", "HL55", "aeronautical mobile" }, { "4U1UN", "FN30", "United Nations, New York" },
        { "4U1ITU", "JN36", "ITU, Geneva" }, { "GB2ABC", "IO91", "UK special event" }, { "TM1ABC", "JN18", "French special event" },
        { "8J1RL", "KC91", "Japan at Syowa" }, { "RI1ANC", "NC63", "Russia at Mirny" }, { "DP0GVN", "IB59", "Germany at Neumayer" },
        { "1A0KM", "JN61", "SMOM, Rome" },
    };
    for (const Case& c : ok) {
        int admin = -1;
        const ce::Verdict v = ce::judge(c.call, c.grid, &admin);
        CHECK(v == ce::kOk, "%s at %s (%s) → %s (%s)", c.call, c.grid, c.where, ce::verdictWord(v), ce::adminName(admin));
    }
    std::printf("   %zu real call/grid pairs\n", sizeof ok / sizeof ok[0]);

    std::printf("── 2. Stuart's Pi 500 false decodes (2026-11-22) ──\n");
    CHECK(ce::judge("132XQA", "PC46") == ce::kNoEntity, "132XQA: no administration has 13");
    CHECK(ce::judge("1H7IJH", "EB72") == ce::kNoEntity, "1H7IJH: no administration has 1H");
    CHECK(ce::judge("132XQA", "") == ce::kNoEntity, "no entity without a grid either");
    static const Case bad[] = {
        { "7Q8FAZ", "OB45", "Malawi at -74.5 (no Antarctic programme)" }, { "YE9HMP", "PC24", "Indonesia at -65.5" },
        { "N6LCO", "OC08", "USA at -61.5 in open sea" }, { "FQ2HMC", "PD93", "France at -56.5 south of Tasmania" },
        { "JA1ABC", "JJ00", "Japan in the Gulf of Guinea" }, { "DL1ABC", "RF73", "Germany in New Zealand" },
        { "ZL1ABC", "IO91", "NZ in England" }, { "7Q8ABC", "RB32", "Malawi at McMurdo" }, { "VK2ABC", "FN42", "Australia in Boston" },
        { "W1AW", "MH00", "USA in the Indian Ocean" }, { "UA3ABC", "GF05", "Russia in Buenos Aires" },
        { "K1ABC", "OC08", "USA, the same empty sea" }, { "G4ABC", "PC24", "UK, the Southern Ocean" },
    };
    for (const Case& c : bad) {
        const ce::Verdict v = ce::judge(c.call, c.grid);
        CHECK(v == ce::kImplausible, "%s at %s (%s) → %s", c.call, c.grid, c.where, ce::verdictWord(v));
    }
    CHECK(ce::judge("7Q8FAZ", "") == ce::kOk && std::string(ce::nameOf("7Q8FAZ")) == "Malawi", "7Q8FAZ is a real-shaped Malawi call");
    CHECK(std::string(ce::nameOf("FQ2HMC")) == "France", "FQ is France's series");

    std::printf("── 3. the ITU table ──\n");
    static const std::pair<const char*, const char*> names[] = {
        { "9N1AA", "Nepal" }, { "ZD7X", "United Kingdom" }, { "3Y0J", "Norway" }, { "V31AB", "Belize" }, { "3DA0AB", "Eswatini" },
        { "3D2AB", "Fiji" }, { "SSN1AB", "Sudan" }, { "ST2AB", "Sudan" }, { "SU1AB", "Egypt" }, { "XX9AB", "China" },
        { "T88AB", "Palau" }, { "E51AB", "Cook Islands" }, { "Z81AB", "South Sudan" }, { "Z60AB", "Kosovo" },
        { "S01AB", "Western Sahara" }, { "1S1AB", "Spratly Islands" }, { "HV0A", "Vatican" }, { "A61AB", "United Arab Emirates" },
        { "AX2AB", "Australia" }, { "AY1AB", "Argentina" }, { "AM1AB", "Spain" }, { "AL7AB", "United States" },
        { "C91AB", "Mozambique" }, { "D44AB", "Cape Verde" }, { "J28AB", "Djibouti" }, { "P29AB", "Papua New Guinea" },
        { "P51AB", "North Korea" }, { "YZ1AB", "Serbia" }, { "4O3AB", "Montenegro" }, { "9X0AB", "Rwanda" },
        { "OX3AB", "Denmark" }, { "XP1AB", "Denmark" }, { "VP2EAB", "United Kingdom" }, { "ZF1AB", "United Kingdom" },
        { "8S0AB", "Sweden" }, { "HF0POL", "Poland" }, { "Y21AB", "Germany" }, { "R1ANC", "Russia" }, { "UK8AB", "Uzbekistan" },
    };
    for (const auto& n : names) CHECK(std::string(ce::nameOf(n.first)) == n.second, "%s → '%s', want %s", n.first, ce::nameOf(n.first), n.second);
    for (const char* none : { "QA1ABC", "0A1ABC", "1B1ABC", "13ABC", "E91AB", "T91AB", "Z91AB", "D11AB" })
        CHECK(ce::judge(none, "") == ce::kNoEntity, "%s has no administration", none);
    // ★ every leading pair: count the allocated ones (Appendix 42 allocates most letter pairs, few digit-led ones)
    {
        int allocated = 0, total = 0;
        const char* al = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        for (const char* a = al; *a; ++a) for (const char* b = al; *b; ++b) {
            std::string p; p += *a; p += *b; ++total;
            if (ce::adminOfPrefix(p + "A") >= 0) ++allocated;
        }
        std::printf("   %d of %d leading pairs allocated\n", allocated, total);
        CHECK(allocated > 800 && allocated < 1100, "allocated pairs %d in a sane range", allocated);
    }
    // ★ random FT8-shaped (c28 standard) calls with random grids: how many pass both tests — a false decode's odds
    {
        std::mt19937 rng(42);
        const char* a37 = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        const char* a36 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        const char* a27 = " ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        int none = 0, impl = 0, pass = 0;
        const int N = 200000;
        for (int i = 0; i < N; ++i) {
            std::string c;
            const char c1 = a37[rng() % 37]; if (c1 != ' ') c += c1;
            c += a36[rng() % 36]; c += (char)('0' + rng() % 10);
            for (int k = 0; k < 3; ++k) { const char s = a27[rng() % 27]; if (s != ' ') c += s; }
            if (c.back() >= '0' && c.back() <= '9') continue;
            std::string g; g += (char)('A' + rng() % 18); g += (char)('A' + rng() % 18); g += (char)('0' + rng() % 10); g += (char)('0' + rng() % 10);
            const ce::Verdict v = ce::judge(c, g);
            if (v == ce::kNoEntity) ++none; else if (v == ce::kImplausible) ++impl; else ++pass;
        }
        const int n = none + impl + pass;
        std::printf("   random FT8-shaped call + grid: %.1f %% no entity, %.1f %% implausible, %.1f %% pass\n",
                    100.0 * none / n, 100.0 * impl / n, 100.0 * pass / n);
        CHECK(pass * 100 < n * 15, "fewer than 15 %% of random call+grid pairs survive (%d of %d)", pass, n);
    }

    std::printf("── 4. cost ──\n");
    {
        const auto t0 = std::chrono::steady_clock::now();
        int okN = 0; const int N = 200000;
        for (int i = 0; i < N; ++i) { const Case& c = ok[i % (sizeof ok / sizeof ok[0])]; if (ce::judge(c.call, c.grid) == ce::kOk) ++okN; }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("   %.0f judgements/s (%.2f us each)\n", N / s, s / N * 1e6);
        CHECK(okN == N, "all pass in the loop");
    }

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
