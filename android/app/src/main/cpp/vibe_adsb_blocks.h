// vibe_adsb_blocks.h — which STATE an ICAO 24-bit aircraft address belongs to, from the address alone.
//
// ★★★ OUR OWN TABLE, FROM THE STANDARD. ICAO Annex 10 Volume III, Part I, Chapter 9, Appendix, Table 9-1 allocates
//     the 24-bit addresses to states in blocks with fixed leading bits (4, 6, 9, 12 or 14 of them: blocks of
//     1 048 576 … 1 024 addresses). An allocation is a fact, and this is our transcription of it — nothing here is
//     copied from dump1090/readsb (GPL) — written as [first, last] address ranges for readability.
// ★★ CROSS-CHECKED, not trusted: test-adsb-data asserts that every non-military block in VRS's code-blocks.csv
//    (CC0) maps to the same country here (docs/v12/ADSB-DATA-RESEARCH.md §2.4). Where Annex 10 has been amended
//    (the 2024 tables split some blocks to 11/13 bits), the VRS file is the newer witness and the test says so.
// ★ Malta is 4D2000–4D2FFF, not the 1 024 addresses of the original table: an amendment, which the VRS table also
//   shows (and Malta Air's fleet sits above 4D2400).
// ★ TERRITORIES ARE NOT HERE EITHER: Cayman (KY) and Bermuda (BM) inside the UK's block, Aruba/Curaçao inside the
//   Netherlands', Hong Kong/Macau inside China's are a STATE's own sub-allocations, published by its registry, not
//   by ICAO. VRS's table carries them, and the lookup lets its smaller block win (vibe_adsb_data.h).
// ★ MILITARY IS NOT HERE. Annex 10 allocates to states, not to air forces; the military sub-blocks (43C000–43FFFF
//   for the UK, AE0000–AFFFFF for the US …) are community knowledge, and VRS's CC0 table is the cleanest source
//   of them — they arrive with the VRS data (and its starter copy), see vibe_adsb_data.h.
// ★ The state of REGISTRY, not of the operator: a Ryanair 737 on the Maltese register is MT.
#pragma once
#include <cstdint>

namespace vibeadsbdata {

struct StateBlock { uint32_t first, last; char iso[3]; };

// ★ Sorted by `first`, no overlaps — stateFor() binary-searches it.
inline const StateBlock* stateBlocks(int& n) {
    static const StateBlock t[] = {
        {0x004000, 0x0043FF, "ZW"}, {0x006000, 0x006FFF, "MZ"}, {0x008000, 0x00FFFF, "ZA"}, {0x010000, 0x017FFF, "EG"},
        {0x018000, 0x01FFFF, "LY"}, {0x020000, 0x027FFF, "MA"}, {0x028000, 0x02FFFF, "TN"}, {0x030000, 0x0303FF, "BW"},
        {0x032000, 0x032FFF, "BI"}, {0x034000, 0x034FFF, "CM"}, {0x035000, 0x0353FF, "KM"}, {0x036000, 0x036FFF, "CG"},
        {0x038000, 0x038FFF, "CI"}, {0x03E000, 0x03EFFF, "GA"}, {0x040000, 0x040FFF, "ET"}, {0x042000, 0x042FFF, "GQ"},
        {0x044000, 0x044FFF, "GH"}, {0x046000, 0x046FFF, "GN"}, {0x048000, 0x0483FF, "GW"}, {0x04A000, 0x04A3FF, "LS"},
        {0x04C000, 0x04CFFF, "KE"}, {0x050000, 0x050FFF, "LR"}, {0x054000, 0x054FFF, "MG"}, {0x058000, 0x058FFF, "MW"},
        {0x05A000, 0x05A3FF, "MV"}, {0x05C000, 0x05CFFF, "ML"}, {0x05E000, 0x05E3FF, "MR"}, {0x060000, 0x0603FF, "MU"},
        {0x062000, 0x062FFF, "NE"}, {0x064000, 0x064FFF, "NG"}, {0x068000, 0x068FFF, "UG"}, {0x06A000, 0x06A3FF, "QA"},
        {0x06C000, 0x06CFFF, "CF"}, {0x06E000, 0x06EFFF, "RW"}, {0x070000, 0x070FFF, "SN"}, {0x074000, 0x0743FF, "SC"},
        {0x076000, 0x0763FF, "SL"}, {0x078000, 0x078FFF, "SO"}, {0x07A000, 0x07A3FF, "SZ"}, {0x07C000, 0x07CFFF, "SD"},
        {0x080000, 0x080FFF, "TZ"}, {0x084000, 0x084FFF, "TD"}, {0x088000, 0x088FFF, "TG"}, {0x08A000, 0x08AFFF, "ZM"},
        {0x08C000, 0x08CFFF, "CD"}, {0x090000, 0x090FFF, "AO"}, {0x094000, 0x0943FF, "BJ"}, {0x096000, 0x0963FF, "CV"},
        {0x098000, 0x0983FF, "DJ"}, {0x09A000, 0x09AFFF, "GM"}, {0x09C000, 0x09CFFF, "BF"}, {0x09E000, 0x09E3FF, "ST"},
        {0x0A0000, 0x0A7FFF, "DZ"}, {0x0A8000, 0x0A8FFF, "BS"}, {0x0AA000, 0x0AA3FF, "BB"}, {0x0AB000, 0x0AB3FF, "BZ"},
        {0x0AC000, 0x0ACFFF, "CO"}, {0x0AE000, 0x0AEFFF, "CR"}, {0x0B0000, 0x0B0FFF, "CU"}, {0x0B2000, 0x0B2FFF, "SV"},
        {0x0B4000, 0x0B4FFF, "GT"}, {0x0B6000, 0x0B6FFF, "GY"}, {0x0B8000, 0x0B8FFF, "HT"}, {0x0BA000, 0x0BAFFF, "HN"},
        {0x0BC000, 0x0BC3FF, "VC"}, {0x0BE000, 0x0BEFFF, "JM"}, {0x0C0000, 0x0C0FFF, "NI"}, {0x0C2000, 0x0C2FFF, "PA"},
        {0x0C4000, 0x0C4FFF, "DO"}, {0x0C6000, 0x0C6FFF, "TT"}, {0x0C8000, 0x0C8FFF, "SR"}, {0x0CA000, 0x0CA3FF, "AG"},
        {0x0CC000, 0x0CC3FF, "GD"}, {0x0D0000, 0x0D7FFF, "MX"}, {0x0D8000, 0x0DFFFF, "VE"}, {0x100000, 0x1FFFFF, "RU"},
        {0x201000, 0x2013FF, "NA"}, {0x202000, 0x2023FF, "ER"}, {0x300000, 0x33FFFF, "IT"}, {0x340000, 0x37FFFF, "ES"},
        {0x380000, 0x3BFFFF, "FR"}, {0x3C0000, 0x3FFFFF, "DE"}, {0x400000, 0x43FFFF, "GB"}, {0x440000, 0x447FFF, "AT"},
        {0x448000, 0x44FFFF, "BE"}, {0x450000, 0x457FFF, "BG"}, {0x458000, 0x45FFFF, "DK"}, {0x460000, 0x467FFF, "FI"},
        {0x468000, 0x46FFFF, "GR"}, {0x470000, 0x477FFF, "HU"}, {0x478000, 0x47FFFF, "NO"}, {0x480000, 0x487FFF, "NL"},
        {0x488000, 0x48FFFF, "PL"}, {0x490000, 0x497FFF, "PT"}, {0x498000, 0x49FFFF, "CZ"}, {0x4A0000, 0x4A7FFF, "RO"},
        {0x4A8000, 0x4AFFFF, "SE"}, {0x4B0000, 0x4B7FFF, "CH"}, {0x4B8000, 0x4BFFFF, "TR"}, {0x4C0000, 0x4C7FFF, "RS"},
        {0x4C8000, 0x4C83FF, "CY"}, {0x4CA000, 0x4CAFFF, "IE"}, {0x4CC000, 0x4CCFFF, "IS"}, {0x4D0000, 0x4D03FF, "LU"},
        {0x4D2000, 0x4D2FFF, "MT"}, {0x4D4000, 0x4D43FF, "MC"}, {0x500000, 0x5003FF, "SM"}, {0x501000, 0x5013FF, "AL"},
        {0x501C00, 0x501FFF, "HR"}, {0x502C00, 0x502FFF, "LV"}, {0x503C00, 0x503FFF, "LT"}, {0x504C00, 0x504FFF, "MD"},
        {0x505C00, 0x505FFF, "SK"}, {0x506C00, 0x506FFF, "SI"}, {0x507C00, 0x507FFF, "UZ"}, {0x508000, 0x50FFFF, "UA"},
        {0x510000, 0x5103FF, "BY"}, {0x511000, 0x5113FF, "EE"}, {0x512000, 0x5123FF, "MK"}, {0x513000, 0x5133FF, "BA"},
        {0x514000, 0x5143FF, "GE"}, {0x515000, 0x5153FF, "TJ"}, {0x516000, 0x5163FF, "ME"}, {0x600000, 0x6003FF, "AM"},
        {0x600800, 0x600BFF, "AZ"}, {0x601000, 0x6013FF, "KG"}, {0x601800, 0x601BFF, "TM"}, {0x680000, 0x6803FF, "BT"},
        {0x681000, 0x6813FF, "FM"}, {0x682000, 0x6823FF, "MN"}, {0x683000, 0x6833FF, "KZ"}, {0x684000, 0x6843FF, "PW"},
        {0x700000, 0x700FFF, "AF"}, {0x702000, 0x702FFF, "BD"}, {0x704000, 0x704FFF, "MM"}, {0x706000, 0x706FFF, "KW"},
        {0x708000, 0x708FFF, "LA"}, {0x70A000, 0x70AFFF, "NP"}, {0x70C000, 0x70C3FF, "OM"}, {0x70E000, 0x70EFFF, "KH"},
        {0x710000, 0x717FFF, "SA"}, {0x718000, 0x71FFFF, "KR"}, {0x720000, 0x727FFF, "KP"}, {0x728000, 0x72FFFF, "IQ"},
        {0x730000, 0x737FFF, "IR"}, {0x738000, 0x73FFFF, "IL"}, {0x740000, 0x747FFF, "JO"}, {0x748000, 0x74FFFF, "LB"},
        {0x750000, 0x757FFF, "MY"}, {0x758000, 0x75FFFF, "PH"}, {0x760000, 0x767FFF, "PK"}, {0x768000, 0x76FFFF, "SG"},
        {0x770000, 0x777FFF, "LK"}, {0x778000, 0x77FFFF, "SY"}, {0x780000, 0x7BFFFF, "CN"}, {0x7C0000, 0x7FFFFF, "AU"},
        {0x800000, 0x83FFFF, "IN"}, {0x840000, 0x87FFFF, "JP"}, {0x880000, 0x887FFF, "TH"}, {0x888000, 0x88FFFF, "VN"},
        {0x890000, 0x890FFF, "YE"}, {0x894000, 0x894FFF, "BH"}, {0x895000, 0x8953FF, "BN"}, {0x896000, 0x896FFF, "AE"},
        {0x897000, 0x8973FF, "SB"}, {0x898000, 0x898FFF, "PG"}, {0x899000, 0x8993FF, "TW"}, {0x8A0000, 0x8A7FFF, "ID"},
        {0x900000, 0x9003FF, "MH"}, {0x901000, 0x9013FF, "CK"}, {0x902000, 0x9023FF, "WS"}, {0xA00000, 0xAFFFFF, "US"},
        {0xC00000, 0xC3FFFF, "CA"}, {0xC80000, 0xC87FFF, "NZ"}, {0xC88000, 0xC88FFF, "FJ"}, {0xC8A000, 0xC8A3FF, "NR"},
        {0xC8C000, 0xC8C3FF, "LC"}, {0xC8D000, 0xC8D3FF, "TO"}, {0xC8E000, 0xC8E3FF, "KI"}, {0xC90000, 0xC903FF, "VU"},
        {0xE00000, 0xE3FFFF, "AR"}, {0xE40000, 0xE7FFFF, "BR"}, {0xE80000, 0xE80FFF, "CL"}, {0xE84000, 0xE84FFF, "EC"},
        {0xE88000, 0xE88FFF, "PY"}, {0xE8C000, 0xE8CFFF, "PE"}, {0xE90000, 0xE90FFF, "UY"}, {0xE94000, 0xE94FFF, "BO"},
    };
    n = int(sizeof t / sizeof t[0]);
    return t;
}

/** ISO 3166 alpha-2 of the state the address is allocated to, or nullptr (unallocated, ICAO's own blocks). */
inline const char* stateFor(uint32_t addr) {
    int n = 0;
    const StateBlock* t = stateBlocks(n);
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (addr < t[mid].first) hi = mid - 1;
        else if (addr > t[mid].last) lo = mid + 1;
        else return t[mid].iso;
    }
    return nullptr;
}

} // namespace vibeadsbdata
