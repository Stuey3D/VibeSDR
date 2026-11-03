// vibe_memcheck.h — the daemon's half of "Check this radio's memory" (maintenance action `memcheck`, 2026-11-03).
//
// ★★★ WHAT IT IS FOR. A USRP clone's radio process died within a second of every start, in calloc on a heap something had
//     already corrupted. The root helper (vibeserver-maintenance memcheck) runs the package's AddressSanitizer build in
//     that radio's own unit for 60 s and saves ASan's report; the admin page asks for it with the radio's serial.
// ★★ THREE CHECKS OF THE SERIAL, EACH AGAINST SOMETHING DIFFERENT: the endpoint holds it to the serial alphabet
//    (serialOk), the daemon finds it among the radios in the settings, and the root helper finds it again in the list
//    the binary prints — and uses the LISTED value, never the one that arrived over the network.
// ★ Pure functions, so test-memcheck-action.cpp can check every refusal without a server.
#pragma once
#include <cctype>
#include <cstdlib>
#include <string>

namespace vibememcheck {

/** The serial alphabet — the same rule as vibeserver-radios' valid_serial and the helper's mc_valid_serial: letters,
 *  digits, '_' ':' '.' '-', at most 64, and never starting with '-' or '.' (an option, a relative path). */
inline bool serialOk(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char c = (unsigned char)s[i];
        const bool word = std::isalnum(c) || c == '_' || c == ':';
        if (i == 0 ? !word : !(word || c == '.' || c == '-')) return false;
    }
    return true;
}

/** "memcheck <serial>" → the serial (true). Anything else, or a serial outside the alphabet: false. */
inline bool parseAction(const std::string& action, std::string& serial) {
    static const std::string kName = "memcheck ";
    if (action.compare(0, kName.size(), kName) != 0) return false;
    serial = action.substr(kName.size());
    return serialOk(serial);
}

/** The `"listeners":N` of a /vibeserver.json answer; -1 when the field is not there (not a VibeServer, cut short). */
inline int listenersIn(const std::string& json) {
    const std::string k = "\"listeners\":";
    const size_t at = json.find(k);
    if (at == std::string::npos) return -1;
    size_t p = at + k.size();
    while (p < json.size() && json[p] == ' ') p++;
    if (p >= json.size() || !std::isdigit((unsigned char)json[p])) return -1;
    return std::atoi(json.c_str() + p);
}

/** Is a maintenance action still running? The helper's log ends with "__VIBESERVER_DONE__ <rc>" from its EXIT trap; a
 *  non-empty log without it is a run in progress. (An empty log: nothing has run since the box started.) */
inline bool logShowsRunning(const std::string& log) {
    return !log.empty() && log.find("__VIBESERVER_DONE__") == std::string::npos;
}

}  // namespace vibememcheck
