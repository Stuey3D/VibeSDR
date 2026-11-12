// vibe_ism_remotes.h — V12 ISM decoder: REMOTES, SWITCHES, DOORBELLS, GARAGE / GATE REMOTES AND CAR KEY FOBS that have
// a known frame (the unknown fixed-code ones stay with fixedcode:: in vibe_ism_protocols.h; unknown car keys with
// FobWatch in vibe_ism_service.h).
//
// ★★★ SOURCES: as vibe_ism_meters.h — frame FACTS from public descriptions and, where that is all there is, the protocol
//     notes at the head of rtl_433's device files (GPL — read for facts only; nothing copied, our own code).
// ★★★ PRIVACY (Stuart, 2026-10-25): a remote's address IS the key of a light, a socket, a gate, a garage, a car — SECRET:
//     Reading::secret + secretId (hashed by the Tracker), never pubId. A rolling code is never decrypted or shown. Shown:
//     "Doorbell / remote #2 · LightwaveRF · button 3 · on" — the make, the button, the command; never an address.
#pragma once
#include "vibe_ism_kit.h"

namespace vibeism {

inline Reading remoteReading(const char* kind, const char* make, const char* model, uint64_t family, uint64_t id) {
    Reading r;
    r.kind = kind; r.make = make; r.model = model;
    r.secret = true; r.secretId = (family << 40) | (id & 0xFFFFFFFFFFull);
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  LIGHTWAVERF (JSJ / Siemens LightwaveRF switches, dimmers, sockets, mood switches) — 433.92 MHz OOK
//  Source: "LightwaveRF RF Protocol", So Make It wiki (wiki.somakeit.org.uk) — 250 µs pulses; a short (~250 µs) gap
//  between two is a 1, a long one (~1250 µs) is a 1 then a 0; the message = a start 1, then 10 bytes each led by a 1,
//  each byte one of 16 symbols (six 1s, no two 0s together) standing for a nibble: parameter (2 nibbles), sub-unit,
//  command, then the 24-bit transmitter ID (6 nibbles). The symbol table is the wiki's.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace lightwave {

inline int nibble(uint8_t sym) {
    static const uint8_t k[16] = {0xF6, 0xEE, 0xED, 0xEB, 0xDE, 0xDD, 0xDB, 0xBE, 0xBD, 0xBB, 0xB7, 0x7E, 0x7D, 0x7B, 0x77, 0x6F};
    for (int i = 0; i < 16; ++i) if (k[i] == sym) return i;
    return -1;
}

/** One frame from one row of gaps, or -1. ★ The whole row must be the frame — 72 pulses, so 71 gaps (70–72 allowed),
 *  the frame starting at the first: the symbol table alone is a weak test (any PPM row with two long gaps per
 *  seven passes it — La Crosse and Oregon bursts did, before). */
inline int64_t frame(const Bits& g, int nib[10]) {
    if (g.size() < 70 || g.size() > 72) return -1;
    Bits line;
    for (uint8_t x : g) { line.push_back(1); if (x) line.push_back(0); }
    if (line.size() < 90) return -1;
    size_t i = 1;                                   // line[0]: the start 1
    for (int k = 0; k < 10; ++k) {
        if (i + 9 > line.size() || line[i] != 1) return -1;
        nib[k] = nibble(uint8_t(bitsAt(line, i + 1, 8)));
        if (nib[k] < 0) return -1;
        i += 9;
    }
    int64_t v = 0; for (int k = 0; k < 10; ++k) v = v << 4 | nib[k];
    return v;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    // ★ AND TWICE ALIKE in the burst (a press sends the frame several times, ~10 ms apart)
    std::vector<int64_t> seen;
    for (const Bits& g : kit::ppmRows(b.ook, 250, 1250, 1600, 120, 450, 0.4)) {
        int nib[10];
        const int64_t v = frame(g, nib);
        if (v < 0) continue;
        if (std::find(seen.begin(), seen.end(), v) == seen.end()) { seen.push_back(v); continue; }
        const int param = nib[0] << 4 | nib[1], sub = nib[2], cmd = nib[3];
        uint32_t id = 0; for (int k = 4; k < 10; ++k) id = id << 4 | uint32_t(nib[k]);
        Reading r = remoteReading("doorbell", "LightwaveRF", "switch", 20, id);
        r.button = std::to_string(sub + 1);
        r.extra = cmd == 1 ? "on" : cmd == 0 ? "off" : cmd == 2 ? "mood" : "command " + std::to_string(cmd);
        if (param && cmd == 1) r.extra += " \xc2\xb7 level " + std::to_string(param);
        r.fact("subunit", (long long)sub); r.fact("command", (long long)cmd); r.fact("parameter", (long long)param);
        out.push_back(r);
        return true;
    }
    return false;
}

} // namespace lightwave

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  GE COLOR EFFECTS (G-35) Christmas-light remote — 433 MHz FSK, ~52 µs chips. Sources: Robert Quattlebaum, "Hacking
//  Christmas Lights", deepdarc.com, 2010-11-27 and Luke Cyca, "G35 RF Remote", lukecyca.com, 2013 — chips 10 = 0, 1100 = 1;
//  a preamble of 1100 repeats, then 8 high and 8 low chips; then 17 bits: 00 · a 6-bit unit ID · an 8-bit command (0xAA
//  on, 0x55 off, 0x5A change, else a function) · 0. No check beyond those fixed bits: heard twice alike in the burst.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace gecolor {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    if (b.fsk.empty()) return false;
    std::vector<uint32_t> seen;
    for (const Bits& row : kit::nrzRows(b.fsk, 52, 12, true, 40)) {
        for (size_t s : kit::findAll(row, 0xCCFF00, 24)) {
            size_t i = s + 24; Bits d;
            while (i + 1 < row.size() && d.size() < 18) {
                if (row[i] == 1 && row[i + 1] == 0) { d.push_back(0); i += 2; }
                else if (i + 3 < row.size() && row[i] == 1 && row[i + 1] == 1 && row[i + 2] == 0 && row[i + 3] == 0) { d.push_back(1); i += 4; }
                else break;
            }
            if (d.size() != 17 || d[0] || d[1] || d[16]) continue;
            const uint32_t v = bitsAt(d, 0, 17);
            if (std::find(seen.begin(), seen.end(), v) == seen.end()) { seen.push_back(v); continue; }
            const int id = int(bitsAt(d, 2, 6)), cmd = int(bitsAt(d, 8, 8));
            Reading r = remoteReading("doorbell", "GE", "Color Effects remote", 21, uint64_t(id));
            r.extra = cmd == 0xAA ? "on" : cmd == 0x55 ? "off" : cmd == 0x5A ? "change" : "function " + std::to_string(cmd);
            char c[8]; std::snprintf(c, sizeof c, cmd == 0xAA ? "on" : cmd == 0x55 ? "off" : cmd == 0x5A ? "change" : "0x%x", cmd);
            r.fact("command", c);
            out.push_back(r);
            return true;
        }
    }
    return false;
}

} // namespace gecolor

/** The remote families, strongest checks first. */
inline bool decodeRemotes(const Burst& b, std::vector<Reading>& out) {
    if (lightwave::decode(b, out)) return true;
    if (gecolor::decode(b, out)) return true;
    return false;
}

} // namespace vibeism
