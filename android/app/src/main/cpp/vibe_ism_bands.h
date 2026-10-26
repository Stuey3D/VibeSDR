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
        {"868", "868 MHz SRD",    868.60e6, 0,     450e3},   // 868.3 + 868.95 (EU): LaCrosse IT+, Bresser, Fine Offset, wM-Bus meters
        // ★ 450 kHz, not 350 (V12 wM-Bus, 2026-10-26): a T1/C1 meter's upper tone sits at 868.95 + 50 kHz + up to 52 kHz of
        //   crystal error = 869.05 MHz; the detector searches centre ± (needHz + 40 kHz) = 868.11-869.09
        {"915", "915 MHz ISM",    915.00e6, 250e3, 200e3},   // US: LaCrosse, Ambient, Fine Offset 915 versions
    };
    return b;
}
inline const Band* bandById(const std::string& id) { for (const auto& b : bands()) if (id == b.id) return &b; return nullptr; }
inline const Band* bandNearest(double hz) {
    const Band* best = nullptr;
    for (const auto& b : bands()) if (!best || std::fabs(b.centreHz - hz) < std::fabs(best->centreHz - hz)) best = &b;
    return best;
}
/** ★ The landing band by the server's location: the Americas (ITU Region 2) → 315 MHz (TPMS and fobs there; Stuart's
 *  rule "US → 315/915 by what's most common"), everywhere else → 433.92. NAN position → 433.92. `can(id)` = reachable. */
template <class Can> inline const Band* landingBand(double lat, double lon, Can can) {
    const bool americas = lon == lon && lat == lat && lon < -25.0 && lon > -170.0;
    const char* order[4] = {"433", "868", "315", "915"};
    const char* amer[4] = {"315", "915", "433", "868"};
    for (int i = 0; i < 4; ++i) { const Band* b = bandById(americas ? amer[i] : order[i]); if (b && can(*b)) return b; }
    return nullptr;
}
static constexpr double kMinRateHz = 0.9e6;   ///< below this a band's devices do not fit (and the HF+ cannot reach one)

} // namespace vibeism
