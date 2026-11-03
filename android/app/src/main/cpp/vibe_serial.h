// vibe_serial.h — a radio's CONFIG serial, made from whatever its driver reports. Header-only.
//
// ★★★ WHY (2026-11-03, a real IC-7300 on a Pi 4 that never appeared): an Icom programs its model AND its
//     number into the CP2102's USB serial with a SPACE between them — "IC-7300 03001234", exactly as
//     /dev/serial/by-id shows it ("…_IC-7300_03001234-if00-port0", udev's own replacement of that space).
//     A radio's serial is its identity everywhere: the config's radios[], the status files, the sdr-change
//     add/pause/remove/replace actions, and — on Linux — the `vibeserver@<serial>` systemd unit, which
//     vibeserver-radios (root) refuses unless it matches ^[A-Za-z0-9_:][A-Za-z0-9._:-]{0,63}$. That check is
//     a security boundary and stays exactly as it is. So the radio is given a serial that FITS it instead.
//
// ★★ REPLACE, NEVER CUT. Every byte outside the alphabet becomes '_' (the same choice udev makes), so
//    "IC-7300 03001234" is "IC-7300_03001234". Cutting at the space — what usrpcaps::cleanSerial does for a
//    clone USRP's junk tail — would leave "IC-7300" for EVERY 7300, and two of them on one machine would be
//    one radio. The number after the space is what makes each one unique.
//  ★ Leading/trailing blanks are dropped first (no '_' prefix from a padded descriptor); a leading '-' or '.'
//    becomes '_' (a unit name must never read as an option or a relative path); never longer than 64.
//  ★ An already-valid serial comes back byte-for-byte unchanged — every RTL, RSP, Airspy, HackRF and USRP
//    serial seen so far — so no existing config, status file or unit name moves.
//  ★ Use the RAW string for anything that READS it (icom::modelBySerial, the display name); use this one for
//    anything that STORES or MATCHES it.
#pragma once
#include <string>

namespace vibe {

inline bool serialCharOk(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || c == '.' || c == '_' || c == ':' || c == '-';
}

inline std::string configSerial(const std::string& raw) {
    size_t a = 0, b = raw.size();
    auto blank = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (a < b && blank((unsigned char)raw[a])) a++;
    while (b > a && blank((unsigned char)raw[b - 1])) b--;
    std::string out;
    out.reserve(b - a < 64 ? b - a : 64);
    for (size_t i = a; i < b && out.size() < 64; i++) {
        const unsigned char c = (unsigned char)raw[i];
        out.push_back(serialCharOk(c) ? (char)c : '_');
    }
    if (!out.empty() && (out[0] == '-' || out[0] == '.')) out[0] = '_';
    return out;
}

}  // namespace vibe
