// vibe_dab_landing.h — WHICH MULTIPLEX DAB OPENS ON WHEN NOBODY NAMED ONE (V12, 2026-11-12).
//
// ★★★ WHY. Stuart: "ISM remembers the last frequency it was tuned to, DAB should also follow suit as right now it
//     defaults to 12B which is fine for the UK as the BBC is there, but for other countries 12B may be dead".
//     The rule, in order — choose() below. The server decides (the shared-dial contract), so every client gets it:
//       1. the LAST block a listener chose on this radio (vibe_dab_last_service.h keeps it, per radio, on disk), if this
//          radio still reaches it;
//       2. the STRONGEST block of this radio's saved band scan (vibe_dab_scan_store.h), if there is one;
//       3. the COUNTRY's main national multiplex — the owner's configured country first, else the server's position
//          (a conservative inner-box lookup: a point near a border answers "don't know", never a neighbour's block);
//       4. 12B, the last resort (and the UK's BBC National DAB).
//     A block or service a listener names always wins (the shim never asks here then). The DX sweep, the band scans and
//     background decoding never set rule 1 (the shim records only a listener's entry or block change).
// ★★ THE TABLE IS PUBLIC FACT, EACH ROW WITH ITS SOURCE. A country whose main multiplex is not one block nationwide
//    (regional SFNs on different blocks) is LEFT OUT — a wrong block is worse than 12B plus "no scan yet, press SCAN".
#pragma once
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace vibedabland {

struct National {
    const char* iso;      ///< ISO 3166-1 alpha-2, upper case
    const char* block;    ///< a kBandIII name
    const char* mux;      ///< what it carries, for the log
    const char* source;   ///< where the fact comes from
};

// ★ Verified 2026-11-12 from the sources named. Keep it to ONE block that a new listener anywhere in the country is
//   most likely to receive.
inline const std::vector<National>& nationals() {
    static const std::vector<National> t = {
        // (filled from the verified research — see the commit that adds each row)
    };
    return t;
}

/** The national block for a two-letter country code (any case). nullptr = not in the table. */
inline const National* nationalFor(const std::string& iso) {
    if (iso.size() != 2) return nullptr;
    char c[3] = { char(std::toupper((unsigned char)iso[0])), char(std::toupper((unsigned char)iso[1])), 0 };
    if (c[0] == 'U' && c[1] == 'K') { c[0] = 'G'; c[1] = 'B'; }   // ★ "UK" is what people type; GB is the code
    for (const auto& n : nationals()) if (!std::strcmp(n.iso, c)) return &n;
    return nullptr;
}

/** ★★ A COUNTRY FROM A POSITION — CONSERVATIVE. Each country is a few boxes lying wholly INSIDE it (an inner
 *  approximation), so a point near a border falls in none and the answer is "" (unknown) rather than a neighbour.
 *  Only countries in the table above have boxes; the owner's configured country, when set, is used before this. */
struct Box { const char* iso; double lat0, lat1, lon0, lon1; };
inline const std::vector<Box>& boxes() {
    static const std::vector<Box> b = {
        // (filled with the table)
    };
    return b;
}
inline std::string isoFromLatLon(double lat, double lon) {
    if (!std::isfinite(lat) || !std::isfinite(lon)) return std::string();
    for (const auto& x : boxes())
        if (lat >= x.lat0 && lat <= x.lat1 && lon >= x.lon0 && lon <= x.lon1) return x.iso;
    return std::string();
}

/** One block of a saved scan, as far as choosing goes. */
struct ScanBlock { std::string block; float merDb = 0, fibRate = 0; int services = 0; bool complete = false; };

/** The strongest block of a scan: an ensemble with stations, best MER, then FIB rate. "" = none usable. */
inline std::string strongest(const std::vector<ScanBlock>& hits) {
    const ScanBlock* best = nullptr;
    for (const auto& h : hits) {
        if (h.block.empty() || h.services <= 0) continue;
        if (!best || h.merDb > best->merDb + 0.05f || (std::fabs(h.merDb - best->merDb) <= 0.05f && h.fibRate > best->fibRate))
            best = &h;
    }
    return best ? best->block : std::string();
}

enum class Why { Last, Scan, Country, Fallback };
inline const char* whyWord(Why w) {
    switch (w) {
        case Why::Last: return "the last block listened to";
        case Why::Scan: return "the strongest block of the last scan";
        case Why::Country: return "the country's national multiplex";
        default: return "the default";
    }
}
struct Choice { std::string block; Why why = Why::Fallback; };

/** ★ The rule (top of the file). `reach(block)` = this radio can tune that block now. `iso` = the owner's country ("" =
 *  not set); lat/lon = the server's position (NaN = not set). */
template <class Reach>
inline Choice choose(const std::string& lastBlock, const std::vector<ScanBlock>& scan, const std::string& iso,
                     double lat, double lon, Reach reach) {
    if (!lastBlock.empty() && reach(lastBlock)) return { lastBlock, Why::Last };
    const std::string s = strongest(scan);
    if (!s.empty() && reach(s)) return { s, Why::Scan };
    const National* n = nationalFor(iso);
    if (!n && iso.size() != 2) n = nationalFor(isoFromLatLon(lat, lon));
    if (n && reach(n->block)) return { n->block, Why::Country };
    return { "12B", Why::Fallback };
}

/** ★ Has this radio any reason to believe in the block it opened on? No scan list and no national entry for its country:
 *  the DAB box says so plainly (no scan yet), because 12B may be silence here. */
inline bool countryKnown(const std::string& iso, double lat, double lon) {
    if (iso.size() == 2) return nationalFor(iso) != nullptr;
    return nationalFor(isoFromLatLon(lat, lon)) != nullptr;
}

}  // namespace vibedabland
