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

/** The remote families, strongest checks first. */
inline bool decodeRemotes(const Burst& b, std::vector<Reading>& out) {
    if (lightwave::decode(b, out)) return true;
    return false;
}

} // namespace vibeism
