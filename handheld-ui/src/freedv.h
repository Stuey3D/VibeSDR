// freedv.h — V12 FreeDV digital voice on the R36S: the rules of src/services/freedv.ts (the app and the web page read
// that file), line for line. docs/v12/FREEDV.md.
//
// ★★ OFFERED ONLY WHERE THE SERVER LISTS IT: vibeserver.json `modes` has "freedv" only when the server's libcodec2
//    loaded, and `freedvSubs` the sub-modes it runs. No "freedv" = no FreeDV key (app.h offeredModes).
// ★★ THE SIDEBAND IS THE SERVER'S: LSB below 10 MHz, USB from 10 MHz up; its report says which ("sb") and the passband
//    follows the report. freedvSidebandAt is only for the moment before the first report.
// ★ AUTO finds 700D / 700E / 1600; a manual choice is the fallback ({"type":"freedv_sub","sub":"700D"}, "auto" goes
//   back; no sub = "tell me the current state", sent on entering the mode).
// ★ The VTS line is short — the screen is small: "FreeDV · searching", "FreeDV 1600 · searching" (a manual choice),
//   "FreeDV 700D · 6 dB · G4ABC" (in sync).
// Golden-tested against the TS: test/gen_vts_golden.ts (ops FV / FO / FS) → test/vts_golden_test.cpp.
#pragma once
#include "json.h"

#include <cmath>
#include <string>
#include <vector>

namespace hh {

inline const char* const kFreeDvSubs[] = {"700D", "700E", "1600"};
inline const char* const kFreeDvLabel = "FreeDV";

inline bool freedvIsSub(const std::string& s) {
    for (const char* k : kFreeDvSubs) if (s == k) return true;
    return false;
}

/** The server's {"type":"freedv"} report. */
struct FreeDvState {
    std::string sub = "auto";    // what the listener (or the room) chose: auto | 700D | 700E | 1600
    std::string sb = "usb";      // the sideband the server demodulates
    bool sync = false;           // decoding speech now
    std::string mode;            // the sub-mode in sync ("" while searching)
    bool hasSnr = false; int snr = 0;   // dB, while in sync
    std::string text;            // the station's text channel (callsign etc.), "" = none
};

/** parseFreeDv: false when the message is not a report. Text is kept printable ASCII, trimmed, ≤ 40 characters. */
inline bool parseFreeDv(const Json& m, FreeDvState& st) {
    if (m.kind != Json::Obj || m["type"].str() != "freedv") return false;
    st = FreeDvState{};
    if (m["sub"].kind == Json::Str && freedvIsSub(m["sub"].str())) st.sub = m["sub"].str();
    st.sync = m["state"].str() == "sync";
    st.sb = m["sb"].str() == "lsb" ? "lsb" : "usb";
    if (st.sync && m["mode"].kind == Json::Str && freedvIsSub(m["mode"].str())) st.mode = m["mode"].str();
    if (st.sync && m["snr"].kind == Json::Num && std::isfinite(m["snr"].n)) { st.hasSnr = true; st.snr = (int)std::floor(m["snr"].n + 0.5); }   // Math.round
    if (m["text"].kind == Json::Str) {
        std::string t;
        for (unsigned char c : m["text"].str()) if (c >= 0x20 && c <= 0x7e) t += (char)c;   // every byte of a UTF-8 character is ≥ 0x80
        size_t a = 0, b = t.size();
        while (a < b && t[a] == ' ') a++;
        while (b > a && t[b - 1] == ' ') b--;
        t = t.substr(a, std::min<size_t>(b - a, 40));
        st.text = t;
    }
    return true;
}

/** The sub-modes THIS server offers (vibeserver.json), in key order; empty = FreeDV not offered. */
inline std::vector<std::string> freedvOffered(const Json& j) {
    std::vector<std::string> out;
    bool listed = false;
    for (auto& m : j["modes"].a) if (m.str() == "freedv") listed = true;
    if (!listed) return out;
    for (const char* k : kFreeDvSubs)
        for (auto& s : j["freedvSubs"].a) if (s.str() == k) { out.push_back(k); break; }
    return out;
}

/** The VTS line (freedvVts). `st` null = no report yet. */
inline std::string freedvVts(const FreeDvState* st) {
    static const std::string dot = " \xc2\xb7 ";
    if (!st) return std::string(kFreeDvLabel) + dot + "searching";
    if (!st->sync) return st->sub == "auto" ? std::string(kFreeDvLabel) + dot + "searching" : std::string(kFreeDvLabel) + " " + st->sub + dot + "searching";
    std::string s = std::string(kFreeDvLabel) + " " + st->mode;
    if (st->hasSnr) s += dot + std::to_string(st->snr) + " dB";
    if (!st->text.empty()) s += dot + st->text;
    return s;
}

/** The passband to draw (Hz from the dial): the SSB side the server demodulates. */
inline void freedvPassband(const std::string& sb, int& lo, int& hi) {
    if (sb == "lsb") { lo = -2700; hi = -50; } else { lo = 50; hi = 2700; }
}
/** The server's rule before its first report: LSB below 10 MHz. The report wins. */
inline const char* freedvSidebandAt(double hz) { return hz > 0 && hz < 10e6 ? "lsb" : "usb"; }

/** The manual-choice message; "" = ask for the current report. */
inline std::string freedvSubMessage(const std::string& sub) {
    return sub.empty() ? "{\"type\":\"freedv_sub\"}" : "{\"type\":\"freedv_sub\",\"sub\":\"" + sub + "\"}";
}

}  // namespace hh
