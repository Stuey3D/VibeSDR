// moveloc.h — "Update this server's location as I move about" (Stuart, 2026-10-11, for RC39).
//
// ★★★ PRIVACY FIRST, BY CONSTRUCTION. A server on the move may be publicly listed, so its position
//     is only ever the NEAREST LARGE POPULATION CENTRE — a large village, town or city — and where
//     there is none (the outback, the Sahara) a GENERAL REGION. Never where the box actually is, never
//     a 6-character square of it. Only that place is kept; the Wi-Fi fix it came from is dropped.
//
// ★★ HOW. The FRONT DOOR alone, when the owner has ticked it on: the Wi-Fi networks the machine can
//    see (NetworkManager's own scan list — no root, no extra scan) → BeaconDB (community, MLS-compatible;
//    Mozilla's service closed in 2024) for a rough fix → OpenStreetMap's Nominatim at city level for the
//    place and ITS centre. On every network change and every 15 minutes. Written to one small file ONLY
//    when the place CHANGES (an SD card on a pocket box); every process — the door and each radio —
//    watches that file and republishes. Tick off, or no file: the HOME location, exactly as before.
//
// ★ No internet, no Wi-Fi, no answer: the last place found stays; nothing is guessed.
#pragma once
#include <functional>
#include <string>

namespace vsmoveloc {

struct Place {
    bool        valid = false;
    std::string label;      // "Corby" — or a region name when there is no town
    bool        region = false;
    double      lat = 0, lon = 0;   // the PLACE's centre, never the fix
    std::string grid;       // derived from that centre (6 for a town, 4 for a region)
    std::string iso;        // ISO 3166 alpha-2, upper case, when known
};

/** Where the shared file lives — the server's data directory (vsDataDir). Set once, before start(). */
void setDir(const std::string& dir);
/** The shared file every process watches. */
std::string stateFile();

/** Front door only: start the worker. `enabled` is read each round (the owner's tick). */
void start(std::function<bool()> enabled);

/** Any process: the current moving place, re-read only when the file changed. `changed` is set when
 *  the answer differs from the last call's (including going back to "none"). Cheap: one stat. */
Place current(bool& changed);

/** Maidenhead locator of a point, 4 or 6 characters. */
std::string gridOf(double lat, double lon, int chars);

// ── exposed for the tests ──
/** Parse NetworkManager's terse "BSSID:SIGNAL" list (colons in the BSSID escaped as \:). */
std::string beaconBody(const std::string& nmcliTerse, int& count);
/** From a Nominatim jsonv2 reverse answer at zoom 13: the town/city the spot is in, or a village of
 *  1,000+ people — else invalid (ask for the region). */
Place placeFromNominatim(const std::string& json);
/** From a Nominatim jsonv2 reverse answer at zoom 8: the county (else state) and its centre. */
Place regionFromNominatim(const std::string& json);

} // namespace vsmoveloc
