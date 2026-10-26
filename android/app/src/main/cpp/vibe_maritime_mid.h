// vibe_maritime_mid.h — MARITIME IDENTIFICATION DIGITS: the three digits of an MMSI that say which administration
// issued it (V12, 2026-10-26, for DSC — and there for AIS too).
//
// ★★ TYPED IN BY HAND from the ITU's published "Table of Maritime Identification Digits" (ITU-R M.585 Annex 1 and the
//    ITU MARS list it points to — facts, a numbering plan, not anybody's software). Where one administration has several
//    MIDs (the United Kingdom 232–235, Panama 351–357/370–373, the United States 338/366–369) every one is listed; a
//    territory with its own MID is named as the territory with its administration in brackets, as the ITU lists it.
//    ★ A MID that is not in this table is shown as the number alone ("MID 217") — never guessed.
//
// ★ How the MID is found in an MMSI (M.585 §2–§5) — mmsiKind() below:
//     ship station            M I D x x x x x x     (first digit 2–7)
//     group of ships          0 M I D x x x x x
//     coast station           0 0 M I D x x x x
//     SAR aircraft            1 1 1 M I D x x x
//     handheld VHF            8 M I D x x x x x
//     craft with a parent     9 8 M I D x x x x
//     aid to navigation       9 9 M I D x x x x
//     AIS-SART / MOB / EPIRB  970 / 972 / 974 x x x x x x   (no MID)
#pragma once
#include <cstdint>
#include <string>

namespace vibe {
namespace mid {

struct Row { uint16_t mid; const char* iso; const char* name; };

inline const Row* table(size_t& n) {
    static const Row t[] = {
        { 201, "AL", "Albania" }, { 202, "AD", "Andorra" }, { 203, "AT", "Austria" }, { 204, "PT", "Azores (Portugal)" },
        { 205, "BE", "Belgium" }, { 206, "BY", "Belarus" }, { 207, "BG", "Bulgaria" }, { 208, "VA", "Vatican City" },
        { 209, "CY", "Cyprus" }, { 210, "CY", "Cyprus" }, { 211, "DE", "Germany" }, { 212, "CY", "Cyprus" },
        { 213, "GE", "Georgia" }, { 214, "MD", "Moldova" }, { 215, "MT", "Malta" }, { 216, "AM", "Armenia" },
        { 218, "DE", "Germany" }, { 219, "DK", "Denmark" }, { 220, "DK", "Denmark" }, { 224, "ES", "Spain" },
        { 225, "ES", "Spain" }, { 226, "FR", "France" }, { 227, "FR", "France" }, { 228, "FR", "France" },
        { 229, "MT", "Malta" }, { 230, "FI", "Finland" }, { 231, "FO", "Faroe Islands (Denmark)" },
        { 232, "GB", "United Kingdom" }, { 233, "GB", "United Kingdom" }, { 234, "GB", "United Kingdom" },
        { 235, "GB", "United Kingdom" }, { 236, "GI", "Gibraltar (United Kingdom)" }, { 237, "GR", "Greece" },
        { 238, "HR", "Croatia" }, { 239, "GR", "Greece" }, { 240, "GR", "Greece" }, { 241, "GR", "Greece" },
        { 242, "MA", "Morocco" }, { 243, "HU", "Hungary" }, { 244, "NL", "Netherlands" }, { 245, "NL", "Netherlands" },
        { 246, "NL", "Netherlands" }, { 247, "IT", "Italy" }, { 248, "MT", "Malta" }, { 249, "MT", "Malta" },
        { 250, "IE", "Ireland" }, { 251, "IS", "Iceland" }, { 252, "LI", "Liechtenstein" }, { 253, "LU", "Luxembourg" },
        { 254, "MC", "Monaco" }, { 255, "PT", "Madeira (Portugal)" }, { 256, "MT", "Malta" }, { 257, "NO", "Norway" },
        { 258, "NO", "Norway" }, { 259, "NO", "Norway" }, { 261, "PL", "Poland" }, { 262, "ME", "Montenegro" },
        { 263, "PT", "Portugal" }, { 264, "RO", "Romania" }, { 265, "SE", "Sweden" }, { 266, "SE", "Sweden" },
        { 267, "SK", "Slovakia" }, { 268, "SM", "San Marino" }, { 269, "CH", "Switzerland" }, { 270, "CZ", "Czechia" },
        { 271, "TR", "T\xc3\xbcrkiye" }, { 272, "UA", "Ukraine" }, { 273, "RU", "Russia" }, { 274, "MK", "North Macedonia" },
        { 275, "LV", "Latvia" }, { 276, "EE", "Estonia" }, { 277, "LT", "Lithuania" }, { 278, "SI", "Slovenia" },
        { 279, "RS", "Serbia" },
        { 301, "AI", "Anguilla (United Kingdom)" }, { 303, "US", "Alaska (United States)" }, { 304, "AG", "Antigua and Barbuda" },
        { 305, "AG", "Antigua and Barbuda" }, { 306, "CW", "Cura\xc3\xa7" "ao / Sint Maarten / Caribbean Netherlands" },
        { 307, "AW", "Aruba (Netherlands)" }, { 308, "BS", "Bahamas" }, { 309, "BS", "Bahamas" }, { 310, "BM", "Bermuda (United Kingdom)" },
        { 311, "BS", "Bahamas" }, { 312, "BZ", "Belize" }, { 314, "BB", "Barbados" }, { 316, "CA", "Canada" },
        { 319, "KY", "Cayman Islands (United Kingdom)" }, { 321, "CR", "Costa Rica" }, { 323, "CU", "Cuba" },
        { 325, "DM", "Dominica" }, { 327, "DO", "Dominican Republic" }, { 329, "GP", "Guadeloupe (France)" },
        { 330, "GD", "Grenada" }, { 331, "GL", "Greenland (Denmark)" }, { 332, "GT", "Guatemala" }, { 334, "HN", "Honduras" },
        { 336, "HT", "Haiti" }, { 338, "US", "United States" }, { 339, "JM", "Jamaica" }, { 341, "KN", "Saint Kitts and Nevis" },
        { 343, "LC", "Saint Lucia" }, { 345, "MX", "Mexico" }, { 347, "MQ", "Martinique (France)" },
        { 348, "MS", "Montserrat (United Kingdom)" }, { 350, "NI", "Nicaragua" }, { 351, "PA", "Panama" }, { 352, "PA", "Panama" },
        { 353, "PA", "Panama" }, { 354, "PA", "Panama" }, { 355, "PA", "Panama" }, { 356, "PA", "Panama" }, { 357, "PA", "Panama" },
        { 358, "PR", "Puerto Rico (United States)" }, { 359, "SV", "El Salvador" }, { 361, "PM", "Saint Pierre and Miquelon (France)" },
        { 362, "TT", "Trinidad and Tobago" }, { 364, "TC", "Turks and Caicos Islands (United Kingdom)" },
        { 366, "US", "United States" }, { 367, "US", "United States" }, { 368, "US", "United States" }, { 369, "US", "United States" },
        { 370, "PA", "Panama" }, { 371, "PA", "Panama" }, { 372, "PA", "Panama" }, { 373, "PA", "Panama" }, { 374, "PA", "Panama" },
        { 375, "VC", "Saint Vincent and the Grenadines" }, { 376, "VC", "Saint Vincent and the Grenadines" },
        { 377, "VC", "Saint Vincent and the Grenadines" }, { 378, "VG", "British Virgin Islands (United Kingdom)" },
        { 379, "VI", "US Virgin Islands (United States)" },
        { 401, "AF", "Afghanistan" }, { 403, "SA", "Saudi Arabia" }, { 405, "BD", "Bangladesh" }, { 408, "BH", "Bahrain" },
        { 410, "BT", "Bhutan" }, { 412, "CN", "China" }, { 413, "CN", "China" }, { 414, "CN", "China" }, { 416, "TW", "Taiwan" },
        { 417, "LK", "Sri Lanka" }, { 419, "IN", "India" }, { 422, "IR", "Iran" }, { 423, "AZ", "Azerbaijan" }, { 425, "IQ", "Iraq" },
        { 428, "IL", "Israel" }, { 431, "JP", "Japan" }, { 432, "JP", "Japan" }, { 434, "TM", "Turkmenistan" },
        { 436, "KZ", "Kazakhstan" }, { 437, "UZ", "Uzbekistan" }, { 438, "JO", "Jordan" }, { 440, "KR", "South Korea" },
        { 441, "KR", "South Korea" }, { 443, "PS", "Palestine" }, { 445, "KP", "North Korea" }, { 447, "KW", "Kuwait" },
        { 450, "LB", "Lebanon" }, { 451, "KG", "Kyrgyzstan" }, { 453, "MO", "Macao (China)" }, { 455, "MV", "Maldives" },
        { 457, "MN", "Mongolia" }, { 459, "NP", "Nepal" }, { 461, "OM", "Oman" }, { 463, "PK", "Pakistan" }, { 466, "QA", "Qatar" },
        { 468, "SY", "Syria" }, { 470, "AE", "United Arab Emirates" }, { 471, "AE", "United Arab Emirates" },
        { 472, "TJ", "Tajikistan" }, { 473, "YE", "Yemen" }, { 475, "YE", "Yemen" }, { 477, "HK", "Hong Kong (China)" },
        { 478, "BA", "Bosnia and Herzegovina" },
        { 501, "TF", "Ad\xc3\xa9lie Land (France)" }, { 503, "AU", "Australia" }, { 506, "MM", "Myanmar" }, { 508, "BN", "Brunei" },
        { 510, "FM", "Micronesia" }, { 511, "PW", "Palau" }, { 512, "NZ", "New Zealand" }, { 514, "KH", "Cambodia" },
        { 515, "KH", "Cambodia" }, { 516, "CX", "Christmas Island (Australia)" }, { 518, "CK", "Cook Islands (New Zealand)" },
        { 520, "FJ", "Fiji" }, { 523, "CC", "Cocos (Keeling) Islands (Australia)" }, { 525, "ID", "Indonesia" },
        { 529, "KI", "Kiribati" }, { 531, "LA", "Laos" }, { 533, "MY", "Malaysia" }, { 536, "MP", "Northern Mariana Islands (United States)" },
        { 538, "MH", "Marshall Islands" }, { 540, "NC", "New Caledonia (France)" }, { 542, "NU", "Niue (New Zealand)" },
        { 544, "NR", "Nauru" }, { 546, "PF", "French Polynesia (France)" }, { 548, "PH", "Philippines" }, { 550, "TL", "Timor-Leste" },
        { 553, "PG", "Papua New Guinea" }, { 555, "PN", "Pitcairn Island (United Kingdom)" }, { 557, "SB", "Solomon Islands" },
        { 559, "AS", "American Samoa (United States)" }, { 561, "WS", "Samoa" }, { 563, "SG", "Singapore" }, { 564, "SG", "Singapore" },
        { 565, "SG", "Singapore" }, { 566, "SG", "Singapore" }, { 567, "TH", "Thailand" }, { 570, "TO", "Tonga" }, { 572, "TV", "Tuvalu" },
        { 574, "VN", "Viet Nam" }, { 576, "VU", "Vanuatu" }, { 577, "VU", "Vanuatu" }, { 578, "WF", "Wallis and Futuna (France)" },
        { 601, "ZA", "South Africa" }, { 603, "AO", "Angola" }, { 605, "DZ", "Algeria" },
        { 607, "TF", "Saint Paul and Amsterdam Islands (France)" }, { 608, "SH", "Ascension Island (United Kingdom)" },
        { 609, "BI", "Burundi" }, { 610, "BJ", "Benin" }, { 611, "BW", "Botswana" }, { 612, "CF", "Central African Republic" },
        { 613, "CM", "Cameroon" }, { 615, "CG", "Congo" }, { 616, "KM", "Comoros" }, { 617, "CV", "Cabo Verde" },
        { 618, "TF", "Crozet Archipelago (France)" }, { 619, "CI", "C\xc3\xb4te d'Ivoire" }, { 620, "KM", "Comoros" },
        { 621, "DJ", "Djibouti" }, { 622, "EG", "Egypt" }, { 624, "ET", "Ethiopia" }, { 625, "ER", "Eritrea" }, { 626, "GA", "Gabon" },
        { 627, "GH", "Ghana" }, { 629, "GM", "Gambia" }, { 630, "GW", "Guinea-Bissau" }, { 631, "GQ", "Equatorial Guinea" },
        { 632, "GN", "Guinea" }, { 633, "BF", "Burkina Faso" }, { 634, "KE", "Kenya" }, { 635, "TF", "Kerguelen Islands (France)" },
        { 636, "LR", "Liberia" }, { 637, "LR", "Liberia" }, { 638, "SS", "South Sudan" }, { 642, "LY", "Libya" }, { 644, "LS", "Lesotho" },
        { 645, "MU", "Mauritius" }, { 647, "MG", "Madagascar" }, { 649, "ML", "Mali" }, { 650, "MZ", "Mozambique" },
        { 654, "MR", "Mauritania" }, { 655, "MW", "Malawi" }, { 656, "NE", "Niger" }, { 657, "NG", "Nigeria" }, { 659, "NA", "Namibia" },
        { 660, "RE", "R\xc3\xa9union (France)" }, { 661, "RW", "Rwanda" }, { 662, "SD", "Sudan" }, { 663, "SN", "Senegal" },
        { 664, "SC", "Seychelles" }, { 665, "SH", "Saint Helena (United Kingdom)" }, { 666, "SO", "Somalia" },
        { 667, "SL", "Sierra Leone" }, { 668, "ST", "S\xc3\xa3o Tom\xc3\xa9 and Pr\xc3\xadncipe" }, { 669, "SZ", "Eswatini" },
        { 670, "TD", "Chad" }, { 671, "TG", "Togo" }, { 672, "TN", "Tunisia" }, { 674, "TZ", "Tanzania" }, { 675, "UG", "Uganda" },
        { 676, "CD", "DR Congo" }, { 677, "TZ", "Tanzania" }, { 678, "ZM", "Zambia" }, { 679, "ZW", "Zimbabwe" },
        { 701, "AR", "Argentina" }, { 710, "BR", "Brazil" }, { 720, "BO", "Bolivia" }, { 725, "CL", "Chile" }, { 730, "CO", "Colombia" },
        { 735, "EC", "Ecuador" }, { 740, "FK", "Falkland Islands (United Kingdom)" }, { 745, "GF", "French Guiana (France)" },
        { 750, "GY", "Guyana" }, { 755, "PY", "Paraguay" }, { 760, "PE", "Peru" }, { 765, "SR", "Suriname" }, { 770, "UY", "Uruguay" },
        { 775, "VE", "Venezuela" },
    };
    n = sizeof t / sizeof t[0];
    return t;
}

/** The row for a MID, or null (a MID the table does not have). Binary search: the table is in MID order. */
inline const Row* find(int m) {
    size_t n = 0; const Row* t = table(n);
    size_t lo = 0, hi = n;
    while (lo < hi) {
        const size_t mi = (lo + hi) / 2;
        if (t[mi].mid == m) return &t[mi];
        if (t[mi].mid < m) lo = mi + 1; else hi = mi;
    }
    return nullptr;
}

/** A flag from a two-letter ISO code (two regional-indicator symbols), "" if the code is not two capitals. */
inline std::string flag(const char* iso) {
    if (!iso || !(iso[0] >= 'A' && iso[0] <= 'Z') || !(iso[1] >= 'A' && iso[1] <= 'Z') || iso[2]) return "";
    std::string s;
    for (int i = 0; i < 2; ++i) {
        const uint32_t cp = 0x1F1E6u + (uint32_t)(iso[i] - 'A');
        s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F));
        s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F));
    }
    return s;
}

/** What kind of station a nine-digit MMSI names, and its MID (-1 = none: a SART, MOB or EPIRB-AIS carries no MID). */
enum class Kind { Ship, Group, Coast, SarAircraft, Handheld, ParentCraft, AtoN, Sart, Mob, Epirb, Unknown };
inline Kind kindOf(const std::string& d9, int& midOut) {
    midOut = -1;
    if (d9.size() != 9) return Kind::Unknown;
    for (char c : d9) if (c < '0' || c > '9') return Kind::Unknown;
    auto at = [&](size_t p) { return (d9[p] - '0') * 100 + (d9[p + 1] - '0') * 10 + (d9[p + 2] - '0'); };
    if (d9.compare(0, 3, "970") == 0) return Kind::Sart;
    if (d9.compare(0, 3, "972") == 0) return Kind::Mob;
    if (d9.compare(0, 3, "974") == 0) return Kind::Epirb;
    if (d9.compare(0, 2, "00") == 0) { midOut = at(2); return Kind::Coast; }
    if (d9[0] == '0') { midOut = at(1); return Kind::Group; }
    if (d9.compare(0, 3, "111") == 0) { midOut = at(3); return Kind::SarAircraft; }
    if (d9.compare(0, 2, "98") == 0) { midOut = at(2); return Kind::ParentCraft; }
    if (d9.compare(0, 2, "99") == 0) { midOut = at(2); return Kind::AtoN; }
    if (d9[0] == '8') { midOut = at(1); return Kind::Handheld; }
    if (d9[0] >= '2' && d9[0] <= '7') { midOut = at(0); return Kind::Ship; }
    return Kind::Unknown;
}
inline const char* kindWord(Kind k) {
    switch (k) {
        case Kind::Ship: return "ship";
        case Kind::Group: return "group";
        case Kind::Coast: return "coast station";
        case Kind::SarAircraft: return "SAR aircraft";
        case Kind::Handheld: return "handheld";
        case Kind::ParentCraft: return "craft of a parent ship";
        case Kind::AtoN: return "aid to navigation";
        case Kind::Sart: return "AIS-SART";
        case Kind::Mob: return "man-overboard beacon";
        case Kind::Epirb: return "EPIRB-AIS";
        default: return "";
    }
}
/** The administration an MMSI belongs to ("United Kingdom"), "MID 217" for one the table lacks, "" for none. */
inline std::string countryOf(const std::string& d9, std::string* isoOut = nullptr) {
    int m = -1;
    kindOf(d9, m);
    if (isoOut) isoOut->clear();
    if (m < 0) return "";
    if (const Row* r = find(m)) { if (isoOut) *isoOut = r->iso; return r->name; }
    return "MID " + std::to_string(m);
}

} // namespace mid
} // namespace vibe
