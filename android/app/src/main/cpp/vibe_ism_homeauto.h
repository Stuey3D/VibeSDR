// vibe_ism_homeauto.h — V12 ISM decoder: HOME AUTOMATION (2026-11-12) — heating controls, thermostats, blinds, lighting
// and smart-home links.
//
// ★★★ SOURCES — the same rule as vibe_ism_meters.h (Stuart, 2026-11-11: "we dont want to copy but if we can take
//     inspiration"). Frame FACTS come from public write-ups first; where the only description is a GPL decoder's notes
//     (rtl_433, src/devices/*.c), those were read for the facts and nothing else: no code, table, comment or identifier
//     is copied. Every decoder here is our own, on our own slicers (vibe_ism_kit.h). Checked on the rtl_433_tests
//     recordings (test INPUT only — scripts/ism-corpus.mjs). Each family says where its facts came from.
#pragma once
#include "vibe_ism_kit.h"

namespace vibeism {

/** A run list with its slivers removed: a run shorter than `minUs` (a noise spike inside a longer run) is folded into
 *  the run before it, and runs of one sign that then meet are joined. For the fast FSK families, where one 20 µs spike
 *  otherwise ends an NRZ row (kit::nrzRows rounds it to 0 chips). */
inline Runs deglitch(const Runs& in, double minUs) {
    Runs o;
    for (int d : in) {
        if (std::abs(d) < minUs && !o.empty()) { o.back() += o.back() > 0 ? std::abs(d) : -std::abs(d); continue; }
        if (!o.empty() && (o.back() > 0) == (d > 0)) o.back() += d;
        else o.push_back(d);
    }
    return o;
}
/** NRZ rows of both slices at `bitUs`, with and without slivers (deglitch at 0.4 of a bit). */
inline std::vector<Bits> nrzRowsClean(const Burst& b, double bitUs, int maxBits, size_t minLen) {
    std::vector<Bits> out = kit::nrzRowsAny(b, bitUs, maxBits, minLen);
    for (const Runs* r : {&b.fsk, &b.ook}) {
        if (r->empty()) continue;
        for (auto& row : kit::nrzRows(deglitch(*r, 0.4 * bitUs), bitUs, maxBits, true, minLen)) out.push_back(std::move(row));
    }
    return out;
}

inline Reading homeRemote(const char* make, const char* model, uint64_t family, uint64_t id) {
    Reading r;
    r.kind = "doorbell"; r.make = make; r.model = model;
    r.secret = true; r.secretId = (family << 40) | (id & 0xFFFFFFFFFFull);
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  INSTEON RF (SmartLabs light switches, dimmers, plugs, sensors, remotes) — 915 MHz (US) / 869.85 MHz (EU) FSK,
//  Manchester at 9 124 chips/s (~109.6 µs a chip). Facts: Peter Shipley's Insteon RF Toolkit notes
//  (github.com/evilpete/insteonrf, Doc/) and the protocol notes in rtl_433's insteon.c (read for facts only). Every byte
//  is its own 28-chip block: "11", then a 5-bit block index and the 8-bit byte, both Manchester (01 = 1) and LSB first.
//  The first block's index is 31 and each next one is lower. Bytes: flags (bits 7–5 message type, 4 extended,
//  3–2 hops left, 1–0 max hops) · to address 3 · from address 3 · command 1, 2 · check — a standard message's check is
//  our own reading of the toolkit's rule: r ^= byte; r ^= ((r ^ (r << 1)) & 0x0F) << 4 over bytes 0–8; an extended
//  message (23 bytes) carries 14 more data bytes and the two's complement of the sum of bytes 7–21.
//  ★ PRIVACY: an Insteon address is all it takes to command that switch (the protocol has no security) — SECRET, as
//    every remote here: "Doorbell / remote #n · Insteon · on". The addresses are never shown.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace insteon {

inline uint8_t check(const uint8_t* d) {
    uint8_t r = 0;
    for (int i = 0; i < 9; ++i) { r ^= d[i]; r ^= uint8_t(((r ^ (r << 1)) & 0x0F) << 4); }
    return r;
}

inline const char* command(int c) {
    switch (c) {
        case 0x11: return "on"; case 0x12: return "fast on"; case 0x13: return "off"; case 0x14: return "fast off";
        case 0x15: return "brighter"; case 0x16: return "dimmer"; case 0x17: return "dimming"; case 0x18: return "dimming stopped";
        case 0x19: return "status request"; case 0x2E: return "settings"; case 0x01: case 0x02: return "linking";
        default: return nullptr;
    }
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    if (b.fsk.empty() && b.ook.size() < 100) return false;
    for (const Bits& row : kit::nrzRowsAny(b, 109.6, 12, 280)) {
        for (size_t p = 0; p + 28 * 10 <= row.size() + 2; ++p) {
            if (!row[p] || !row[p + 1]) continue;
            std::vector<uint8_t> by;
            int prevIdx = 32;
            size_t q = p;
            while (q + 28 <= row.size() + 2 && by.size() < 32) {
                if (!row[q] || !row[q + 1]) break;
                const Bits d = kit::manchesterAt(row, q + 2, 13);
                if (d.size() != 13) break;
                const int idx = int(bitsAtLsb(d, 0, 5));
                if (by.empty() ? idx != 31 : idx >= prevIdx) break;
                prevIdx = idx;
                by.push_back(uint8_t(bitsAtLsb(d, 5, 8)));
                q += 28;
            }
            if (by.size() < 10) continue;
            const bool ext = by[0] & 0x10;
            if (ext) {
                if (by.size() < 23) continue;
                unsigned s = 0; for (int i = 7; i < 22; ++i) s += by[size_t(i)];
                if (uint8_t(-int(s)) != by[22]) continue;
            } else if (check(by.data()) != by[9]) continue;
            const uint32_t from = uint32_t(by[6]) << 16 | uint32_t(by[5]) << 8 | by[4];
            Reading r = homeRemote("Insteon", "", 0x1A5, from);
            static const char* const type[8] = {"direct", "ack", "group clean-up", "group clean-up ack", "broadcast", "nak",
                                                "group broadcast", "group clean-up nak"};
            const int mt = by[0] >> 5;
            const char* cmd = command(by[7]);
            r.extra = std::string(cmd ? cmd : "command") + " (" + type[mt] + ")";
            r.fact("msg_type", (long long)mt); r.fact("extended", (long long)(ext ? 1 : 0));
            r.fact("hopsmax", (long long)(by[0] & 3)); r.fact("hopsleft", (long long)((by[0] >> 2) & 3));
            out.push_back(r);
            return true;
        }
    }
    return false;
}

} // namespace insteon

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  EQUATION / SIEMENS ADLM FPRF (RDE100.1 FPRF) three-zone electric-heater programmer (Leroy Merlin, France) — 433.86
//  MHz OOK PWM. Facts: the rtl_433_tests ADLM README and rtl_433's conf/adlm_fprf.conf description (read for facts
//  only): a ~50 ms wake-up mark, a gap, then three frames, each a 3 ms start mark and 40 bits of 2 ms (a 500 µs mark
//  = 1, 1500 µs = 0, the rest of the 2 ms a gap before it), ~5 ms between frames. Counting the start mark as bit 0
//  (a 0), bits 8–23 = the programmer's ID, 28–31 the zone, 32–35 the mode (8 off, 9 eco, 10 comfort); bits 0–7 have
//  been 0x40 on every unit recorded.
//  NO CHECK: a frame is believed when the burst carries the wake-up mark and two identical frames, or three identical
//  frames; otherwise it is shown once heard twice (Reading::confirm). The ID is a remote's address: SECRET.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace adlm {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    const Runs& r = b.ook;
    bool wake = false;
    std::vector<Bits> frames;
    for (size_t i = 0; i < r.size(); ++i) {
        if (r[i] >= 40000 && r[i] <= 70000) { wake = true; continue; }
        if (r[i] < 2500 || r[i] > 3600 || i == 0 || -r[i - 1] < 4000) continue;
        Bits f(1, 0);
        size_t k = i + 1;
        for (; k + 1 < r.size() && f.size() < 41; k += 2) {
            const int g = -r[k], m = r[k + 1];
            if (g < 300 || g > 2000 || m <= 0) break;
            if (m >= 300 && m <= 800 && g >= 1100) f.push_back(1);
            else if (m >= 1100 && m <= 2000 && g <= 800) f.push_back(0);
            else break;
        }
        if (f.size() == 41 && bitsAt(f, 0, 8) == 0x40) frames.push_back(f);
        i = k - 1;
    }
    for (const Bits& f : frames) {
        const int copies = int(std::count(frames.begin(), frames.end(), f));
        if (copies < 2) continue;
        const int mode = int(bitsAt(f, 32, 4)), zone = int(bitsAt(f, 28, 4));
        if (mode < 8 || mode > 10) continue;
        const uint32_t id = bitsAt(f, 8, 16);
        Reading rd = homeRemote("ADLM FPRF", "heater programmer", 0xAD1, id);
        static const char* const words[3] = {"OFF", "ECO", "COMFORT"};
        static const char* const lower[3] = {"off", "eco", "comfort"};
        rd.extra = "zone " + std::to_string(zone) + " " + lower[mode - 8];
        rd.confirm = !((wake && copies >= 2) || copies >= 3);
        rd.fact("zone", (long long)zone); rd.fact("mode", words[mode - 8]);
        out.push_back(rd);
        return true;
    }
    return false;
}

} // namespace adlm

/** The home-automation families, strongest checks first. */
inline bool decodeHomeauto(const Burst& b, std::vector<Reading>& out) {
    if (insteon::decode(b, out)) return true;
    if (adlm::decode(b, out)) return true;
    return false;
}

/** ★ Where each home-automation family transmits (bandPlausible). `false` = refuse. */
inline bool homeautoBandOk(const Reading& r, double f) {
    if (r.make == "ADLM FPRF") return f >= 433.0e6 && f <= 435.0e6;
    if (r.make == "Insteon") return (f >= 902e6 && f <= 928e6) || (f >= 863e6 && f <= 870e6);
    return true;
}

} // namespace vibeism
