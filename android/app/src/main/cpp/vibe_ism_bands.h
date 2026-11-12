// vibe_ism_bands.h — V12 ISM mode's bands (split from vibe_ism_service.h, 2026-10-25, so the background-decoding config
// — vibe_bgdecode_config.h, read by the daemon's save path — can name and check a band without the decoders).
#pragma once
#include <cmath>
#include <string>
#include <vector>

namespace vibeism {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE BANDS — the tuning controls step between these in ISM mode (Stuart, 2026-10-25: "I always found it quite hard
//  to tune the ISM on OWRX"). Each is one whole sub-band capture; nobody fine-tunes inside a band.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Band {
    const char* id;       ///< what the clients send ("433")
    const char* name;     ///< the frequency readout's text
    double centreHz;      ///< the band's middle — the "frequency" the clients show
    double dcOffsetHz;    ///< the RADIO is tuned this far above the middle, so its DC remnant misses the busy channel
    double needHz;        ///< the span the band's devices occupy around centreHz (for reachability)
};
inline const std::vector<Band>& bands() {
    static const std::vector<Band> b = {
        {"315", "315 MHz ISM",    315.00e6, 250e3, 200e3},   // US/Japan TPMS, fobs, remotes
        {"433", "433.92 MHz ISM", 433.92e6, 250e3, 870e3},   // 433.05–434.79: EU/UK sensors, weather, doorbells, TPMS
        {"868", "868 MHz SRD",    868.60e6, 200e3, 450e3},   // 868.3 + 868.95 (EU): LaCrosse IT+, Bresser, Fine Offset, wM-Bus meters
        /* ★★ THE RADIO 200 kHz ABOVE 868.6 (V12 2026-11-12, LORA-WORLD-BANDS.md): centred on 868.6 itself, Meshtastic
         *  LongFast (869.525, BW250: up to 869.65) needed 1.05 MHz of reach and a 2.048 MS/s capture holds 0.92 — so it
         *  was heard only at 2.4 MS/s. At 868.80: 0.85 MHz (fits at 2.048), and the sensors' window 868.11–869.09 (centre
         *  ± needHz + 40 kHz) is still inside (867.88–869.72 at 2.048). The DC sits between LoRaWAN 868.5 (to 868.5625)
         *  and the 868.95 meters' lower tone (868.90), 100 kHz from each. Lost at 2.4 MS/s: LoRaWAN 867.7 (tier 1). */
        // ★ 450 kHz, not 350 (V12 wM-Bus, 2026-10-26): a T1/C1 meter's upper tone sits at 868.95 + 50 kHz + up to 52 kHz of
        //   crystal error = 869.05 MHz; the detector searches centre ± (needHz + 40 kHz) = 868.11-869.09
        /* ★★ V12 2026-11-12 — THE 902–928 MHz LoRa MESH BANDS (docs/v12/LORA-MESH.md §Plans; Stuart: "we are missing an
         *  ISM/meshcore band" — Kiko's server in Brazil had nothing to decode, MeshCore Brasil being on 923.125 MHz). An
         *  RTL captures ~2 MHz, so 902–928 is FIVE captures, each placed on where a region's mesh actually is; the 915
         *  band keeps its sensors where they were. Step order = frequency order (≤ 8 bands in all: what every client
         *  draws). Centres are where the RADIO sits (dcOffsetHz 0): each is placed so the band's channels fit at 2.048
         *  and 2.4 MS/s and the DC remnant falls between them. */
        // ★ ids and centres as docs/v12/LORA-WORLD-BANDS.md's blocks (907 at 906.60, 910 at 910.40), so its rework builds on these
        {"907", "907 MHz ISM (Meshtastic US)", 906.60e6, 450e3, 200e3},   // Meshtastic US LongFast 906.875, BR_902 MediumFast 906.625 —
                                                                          // the radio at 907.05 (DC clear of both; 906.60 is inside 906.625's)
        {"910", "910 MHz ISM (MeshCore US)",   910.40e6, 0, 300e3},   // MeshCore USA/Canada 910.525 (BW62.5 SF7); US915 sub-band 5; DC between 910.3 and 910.525
        {"915", "915 MHz ISM",    915.00e6, 250e3, 200e3},   // US: LaCrosse, Ambient, Fine Offset 915 versions; MeshCore AU 915.8; AU915 FSB1
        {"920", "920 MHz ISM (Meshtastic ANZ)", 920.65e6, 0, 800e3},   // Meshtastic ANZ LongFast 919.875; MeshCore VN 920.25 / TH 921.0; AS923-2
        {"923", "923 MHz ISM (MeshCore BR/AU, AS923)", 923.30e6, 300e3, 600e3},   // MeshCore Brazil + AU SA/WA/QLD 923.125; AS923-1; Meshtastic JP/KR/TW/TH/MY
    };
    return b;
}
inline const Band* bandById(const std::string& id) { for (const auto& b : bands()) if (id == b.id) return &b; return nullptr; }
inline const Band* bandNearest(double hz) {
    const Band* best = nullptr;
    for (const auto& b : bands()) if (!best || std::fabs(b.centreHz - hz) < std::fabs(best->centreHz - hz)) best = &b;
    return best;
}
/** ★★★ MAY THE FREQUENCY BOX PUT ISM ON `hz`? Only inside the band's own allocation (V12, 2026-11-12). The exact-frequency
 *  door took ANY frequency whose NEAREST band this radio reaches — so on an RTL every frequency on the dial passed (the
 *  nearest of 315/433/868/915 always is), and an app still showing ISM over a radio the server had put in ADS-B sent
 *  1089.59 MHz and got "ISM" at 1089.590 (Stuart, iPhone, the Pi 500's V4L). The allocations, generously:
 *    315 — 310–320 MHz (US/Japan fobs and TPMS: 310, 314.9, 315, 318);  433 — 433.05–434.79 MHz (ITU Region 1 ISM);
 *    868 — 863–870 MHz (the European SRD band, wM-Bus 868.3/868.95 inside it);  915 — 902–928 MHz (Region 2 ISM). */
inline bool exactAllowed(const Band& b, double hz) {
    const std::string id = b.id;
    if (id == "315") return hz >= 310.0e6 && hz <= 320.0e6;
    if (id == "433") return hz >= 433.05e6 && hz <= 434.79e6;
    if (id == "868") return hz >= 863.0e6 && hz <= 870.0e6;
    /* ★ the 902–928 bands: 907/910/915 take Region 2's whole 902–928; 920 and 923 serve Brazil (Anatel: 915–928, Ato
     *  14.448 item 10.1), Australia / NZ (915–928) and the AS923 countries (915–928 at the widest), so 915–928 MHz */
    if (id == "907" || id == "910" || id == "915") return hz >= 902.0e6 && hz <= 928.0e6;
    if (id == "920" || id == "923") return hz >= 915.0e6 && hz <= 928.0e6;
    return std::fabs(hz - b.centreHz) <= b.needHz;
}
/** ★★ V12 2026-11-12 — WHICH 902–928 MHz WORLD THE SERVER IS IN (docs/v12/LORA-MESH.md §Plans). The mesh networks and
 *  LoRaWAN plans of ITU Regions 2 and 3 do not share frequencies (Meshtastic US 906.875, MeshCore US 910.525, MeshCore
 *  Brasil 923.125, Meshtastic ANZ 919.875, AS923 923.2/923.4), so the landing band and the LoRa default set follow the
 *  receiver's own position. Rough boxes, never a border: a wrong guess only changes where ISM LANDS and which channels
 *  are in the always-on set — every band and every channel is still there to step to.
 *    NorthAmerica: lon −170…−25 north of 12.5° N, or west of 82° W (Central America, the Caribbean's north);
 *    SouthAmerica: lon −82…−25 south of 12.5° N (Brazil's rules: 902–907.5 + 915–928 MHz, AU915 LoRaWAN);
 *    Oceania: Australia / New Zealand (lat −50…−10, lon 110…180);
 *    Asia: east and south-east Asia's AS923 countries (lat −11…50, lon 95…150) outside a box for mainland China (whose
 *          LoRa is CN470, not in these bands);  Unknown: no position;  Other: everywhere else (Europe, Africa, …). */
enum class Region { Unknown = 0, Other, NorthAmerica, SouthAmerica, Oceania, Asia };
inline Region regionAt(double lat, double lon) {
    if (!(lat == lat) || !(lon == lon)) return Region::Unknown;
    if (lon < -25.0 && lon > -170.0) return (lat < 12.5 && lon > -82.0) ? Region::SouthAmerica : Region::NorthAmerica;
    if (lat <= -10.0 && lat >= -50.0 && lon >= 110.0) return Region::Oceania;
    if (lat >= -11.0 && lat <= 50.0 && lon >= 95.0 && lon <= 150.0) {
        const bool china = lat >= 21.5 && lon <= 119.5;   // ★ Taiwan (120–122° E), Korea and Japan stay AS923
        if (!china) return Region::Asia;
    }
    return Region::Other;
}
inline const char* regionName(Region r) {
    switch (r) {
        case Region::NorthAmerica: return "North America"; case Region::SouthAmerica: return "South America";
        case Region::Oceania: return "Australia / New Zealand"; case Region::Asia: return "Asia (AS923)";
        case Region::Other: return "elsewhere"; default: return "no position";
    }
}
/** ★ The landing band by the server's location (regionAt). V12 2026-11-12: where the region's MESH is, then its sensors —
 *  North America → 907 (Meshtastic US LongFast, the biggest mesh there), then 910 (MeshCore US), 315, 915;
 *  South America → 923 (MeshCore Brasil), then 915, 920; Australia / NZ → 920 (Meshtastic ANZ), then 915 (MeshCore AU),
 *  923; Asia → 923 (AS923-1 and four countries' Meshtastic LongFast), then 920; everywhere else and no position → 433.92,
 *  868. After the region's own list, every other band in step order, so a radio that reaches only some still lands.
 *  `can(id)` = reachable. (Before 2026-11-12 the Americas landed on 315 — Stuart's "US → 315/915 by what's most common".) */
template <class Can> inline const Band* landingBand(double lat, double lon, Can can) {
    static const char* const na[] = {"907", "910", "315", "915", nullptr};
    static const char* const sa[] = {"923", "915", "920", "907", nullptr};
    static const char* const oc[] = {"920", "915", "923", "433", nullptr};
    static const char* const as[] = {"923", "920", "433", "315", nullptr};
    static const char* const eu[] = {"433", "868", "315", "915", nullptr};
    const char* const* order = eu;
    switch (regionAt(lat, lon)) {
        case Region::NorthAmerica: order = na; break;
        case Region::SouthAmerica: order = sa; break;
        case Region::Oceania: order = oc; break;
        case Region::Asia: order = as; break;
        default: break;
    }
    for (int i = 0; order[i]; ++i) { const Band* b = bandById(order[i]); if (b && can(*b)) return b; }
    for (const auto& b : bands()) if (can(b)) return &b;
    return nullptr;
}
static constexpr double kMinRateHz = 0.9e6;   ///< below this a band's devices do not fit (and the HF+ cannot reach one)

} // namespace vibeism
