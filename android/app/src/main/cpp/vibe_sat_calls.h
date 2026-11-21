// vibe_sat_calls.h — V12: which satellite a packet came from, when its callsign (or a digipeater in its path) says so.
// docs/v12/AX25-9600-FX25-IL2P.md "Satellite names".
//
// ★★ ONLY CALLSIGNS WE ARE SURE OF. A wrong name on a frame is worse than none, so the table is short and every entry
//    has a public source: the ISS's amateur station callsigns (ARISS, ariss.org — the packet digipeater is RS0ISS, the
//    station's other calls NA1SS, DP0ISS, OR4ISS, IR0ISS) and GRBAlpha's OM9GRB (its alternative name in the SatNOGS
//    DB, db.satnogs.org/satellite/47959 — the satellite has since re-entered; harmless to keep, it shows how). When the
//    SATELLITE ENGINE tunes a pass it knows the satellite's name itself and hands it to the decoder ("sat" on attach),
//    which is the right answer for every satellite not in this list.
// ★ To add one: its callsign and where it is published, in the same line. No guessing from a name that "looks like" one.
#pragma once
#include <string>

namespace vibesat {

/** The satellite a callsign (without SSID) belongs to, or "" — case as sent (AX.25 calls are capitals). */
inline std::string byCallsign(const std::string& call) {
    struct E { const char* call; const char* name; };
    static const E t[] = {
        { "RS0ISS", "ISS" },    // ARISS packet digipeater (145.825 MHz)
        { "NA1SS", "ISS" },     // ARISS
        { "DP0ISS", "ISS" },    // ARISS (Columbus)
        { "OR4ISS", "ISS" },    // ARISS
        { "IR0ISS", "ISS" },    // ARISS
        { "OM9GRB", "GRBAlpha" },
        { "DP0SNX", "SONATE-2" },   // Univ. Würzburg, "SONATE-2 — information for radio amateurs" (telemetry, CW, APRS digi)   // SatNOGS DB alt. name, NORAD 47959; 9k6 G3RUH, 437.025 MHz (re-entered)
    };
    for (const E& e : t) if (call == e.call) return e.name;
    return "";
}

} // namespace vibesat
