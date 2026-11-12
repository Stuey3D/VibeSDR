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
#include "vibe_ism_wxkit.h"

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

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  DANFOSS CFR room thermostat (for the Danfoss RX1/RX2 boiler receivers) — 433.92 MHz FSK, NRZ 100 µs. Facts: Tommy
//  Vestermark's reverse engineering as noted in rtl_433's danfoss.c (read for facts only; no other description
//  exists). ~128 bits of AA, then 21 nibbles, each sent as a 6-bit code (three 1s, three 0s — the code for each nibble
//  below): E 0 2 (the prefix) · ID 4 nibbles · message counter · switch (2 day, 4 timer, 8 night) · temperature as two
//  bytes, fraction then whole °C · set point the same · CRC-16 poly 0x1021 init 0 over the nibbles from the 0 (as
//  bytes 02 … set point) = the last 4 nibbles.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace danfoss {

inline int nibbleOf(uint32_t code) {
    static const uint8_t c[16] = {0x25, 0x29, 0x32, 0x0E, 0x13, 0x2A, 0x1A, 0x26, 0x34, 0x19, 0x15, 0x31, 0x2C, 0x0B, 0x0D, 0x16};
    for (int i = 0; i < 16; ++i) if (c[i] == code) return i;
    return -1;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    if (b.fsk.empty()) return false;
    for (const Bits& row : kit::nrzRows(b.fsk, 100.0, 6, true, 140)) {
        for (size_t s : kit::findAll(row, 0xD972, 18)) {     // the codes of E, 0, 2
            if (s + 18 + 6 * 18 > row.size()) break;
            uint8_t by[10]; by[0] = 0x02; bool ok = true;
            for (int k = 0; k < 18 && ok; ++k) {
                const int n = nibbleOf(bitsAt(row, s + 18 + 6 * size_t(k), 6));
                if (n < 0) { ok = false; break; }
                if (k % 2 == 0) by[1 + k / 2] = uint8_t(n << 4); else by[1 + k / 2] |= uint8_t(n);
            }
            if (!ok || crc16(by, 8, 0x1021, 0) != uint16_t(by[8] << 8 | by[9])) continue;
            const int id = by[1] << 8 | by[2], sw = by[3] & 0xF;
            const char* swName = sw == 2 ? "DAY" : sw == 4 ? "TIMER" : sw == 8 ? "NIGHT" : nullptr;
            if (!swName) continue;
            const double t = by[5] + by[4] / 256.0, set = by[7] + by[6] / 256.0;
            Reading r;
            r.kind = "sensor"; r.make = "Danfoss"; r.model = "CFR thermostat";
            r.pubId = std::to_string(id);
            r.tempC = t;
            r.extra = "set " + kit::fmt1v(set, 1) + " \xc2\xb0" "C \xc2\xb7 " + (sw == 2 ? "day" : sw == 4 ? "timer" : "night");
            r.fact("id", (long long)id); r.fact("setpoint_C", set); r.fact("switch", swName);
            out.push_back(r);
            return true;
        }
    }
    return false;
}

} // namespace danfoss

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  HONEYWELL evohome / RAMSES II (CM921 room thermostat, HR92 radiator valves, BDR91 relays, DTS92 …) — 868.3 MHz FSK,
//  38.4 kbit/s (26 µs). Facts: the RAMSES II write-ups (github.com/Evsdd/The-Evohome-Protocol wiki; domoticaforum.eu
//  "Evohome" thread) and rtl_433's honeywell_cm921.c notes (read for facts only). Every byte is sent UART-style — a 0
//  start bit, 8 bits LSB first, a 1 stop bit. Preamble 55 FF 00, then bytes 33 55 53 (a deliberate Manchester break),
//  the message Manchester-coded two bytes per byte, 35, then 55s. The message: header (it says how many 3-byte device
//  addresses follow: 14 → 1; 10 / 18 / 1C / 3C → 2; else bits 3–2) · addresses · command 16 · length · payload ·
//  a check byte making the sum of every byte 0. Read here: 0008 relay demand, 3EF0 relay status / boiler
//  modulation, 2309 set point, 30C9 zone temperatures, 3150 heat demand, 3B00 TPI state, 1100 boiler cycle settings;
//  any other command is named by number. The Manchester break, the 35 footer and the byte sum together are the check.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace evohome {

inline bool message(const std::vector<uint8_t>& m, std::vector<Reading>& out) {
    if (m.size() < 7 || (kit::sumBytes(m.data(), m.size()) & 0xFF)) return false;
    const uint8_t h = m[0];
    const size_t nId = (h == 0x14) ? 1 : (h == 0x10 || h == 0x18 || h == 0x1C || h == 0x3C) ? 2 : size_t((h >> 2) & 3);
    size_t i = 1 + 3 * nId;
    if (nId == 0 || i + 3 > m.size()) return false;
    const int cmd = m[i] << 8 | m[i + 1];
    const size_t len = m[i + 2];
    i += 3;
    if (i + len + 1 != m.size()) return false;
    const uint8_t* p = m.data() + i;
    const uint32_t dev = uint32_t(m[1]) << 16 | uint32_t(m[2]) << 8 | m[3];
    Reading r;
    r.kind = "sensor"; r.make = "Honeywell"; r.model = "evohome";
    char ib[8]; std::snprintf(ib, sizeof ib, "%06X", dev); r.pubId = ib;
    auto pct = [](int v) { return kit::fmt1v(v / 2.0, 0) + " %"; };
    char hx[24];
    switch (cmd) {
        case 0x0008:
            if (len != 2) goto other;
            r.extra = "heat demand " + pct(p[1]);
            r.fact("domain_id", (long long)p[0]); r.fact("demand", p[1] / 200.0);
            break;
        case 0x3EF0:
            if (len == 3) { r.extra = "relay " + pct(p[1]); r.fact("status", p[1] / 200.0); }
            else if (len == 6) { r.extra = "boiler " + pct(p[1]) + (p[3] ? " \xc2\xb7 flame on" : " \xc2\xb7 flame off"); r.fact("boiler_modulation_level", p[1] / 200.0); r.fact("flame_status", (long long)p[3]); }
            else goto other;
            break;
        case 0x2309:
            if (len != 3) goto other;
            r.extra = "zone " + std::to_string(p[0]) + " set " + kit::fmt1v((p[1] << 8 | p[2]) / 100.0, 1) + " \xc2\xb0" "C";
            r.fact("zone", (long long)p[0]); r.fact("setpoint", (p[1] << 8 | p[2]) / 100.0);
            break;
        case 0x30C9:
            if (len < 3 || len % 3) goto other;
            r.tempC = int16_t(p[1] << 8 | p[2]) / 100.0;
            if (len > 3) r.extra = std::to_string(len / 3) + " zones";
            break;
        case 0x3150:
            if (len < 2) goto other;
            r.extra = "zone " + std::to_string(p[0]) + " heat demand " + pct(p[1]);
            r.fact("zone", (long long)p[0]); r.fact("heat_demand", (long long)p[1]);
            break;
        case 0x3B00:
            if (len != 2) goto other;
            r.extra = "TPI " + pct(p[1]);
            r.fact("domain_id", (long long)p[0]); r.fact("state", p[1] / 200.0);
            break;
        case 0x1100:
            if (len != 5 && len != 8) goto other;
            r.extra = "boiler cycle " + kit::fmt1v(p[1] / 4.0, 0) + " /h";
            r.fact("domain_id", (long long)p[0]); r.fact("cycle_rate", p[1] / 4.0);
            break;
        default:
        other:
            std::snprintf(hx, sizeof hx, "command %04X", cmd); r.extra = hx;
            break;
    }
    out.push_back(r);
    return true;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    if (b.fsk.empty()) return false;
    for (const Bits& row : kit::nrzRows(b.fsk, 26.04, 12, true, 200)) {
        for (size_t s : kit::findAll(row, 0x555FF004 >> 2, 30)) {
            // UART bytes (start 0, LSB first, stop 1) → their values
            std::vector<uint8_t> u;
            for (size_t q = s + 30; q + 10 <= row.size() && u.size() < 300; q += 10) {
                if (row[q] != 0 || row[q + 9] != 1) break;
                u.push_back(uint8_t(bitsAtLsb(row, q + 1, 8)));
            }
            if (u.size() < 8 || u[0] != 0x33 || u[1] != 0x55 || u[2] != 0x53) continue;
            size_t f = u.size();
            while (f > 3 && u[f - 1] == 0x55) --f;
            if (f == u.size() || f < 5 || u[f - 1] != 0x35) continue;
            Bits mb;
            for (size_t k = 3; k + 1 < f; ++k) for (int q = 7; q >= 0; --q) mb.push_back((u[k] >> q) & 1);
            for (int conv = 0; conv < 2; ++conv) {
                size_t end = 0;
                const Bits d = kit::manchesterAt(mb, 0, mb.size() / 2, conv == 0, &end);
                if (d.size() != mb.size() / 2 || d.size() % 8) continue;
                std::vector<uint8_t> m;
                for (size_t k = 0; k < d.size(); k += 8) m.push_back(uint8_t(bitsAt(d, k, 8)));
                if (message(m, out)) return true;
            }
        }
    }
    return false;
}

} // namespace evohome

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  WATTS WFHT-RF room thermostat (an ADEV design, France; underfloor-heating controllers) — 433.92 MHz OOK PWM: a ~6 ms
//  sync mark, then 54 bits, a ~600 µs mark (then ~260 µs gap) = 1, a ~260 µs mark (then ~600 µs) = 0. Facts: the
//  rtl_433_tests watts_thermostat README (manual + pulse captures) and rtl_433's watts_thermostat.c notes (read for
//  facts only; protocol work by Ådne Hovda and Christian Zuckschwerdt). Fields, each LSB first: preamble 0x5A (as sent
//  MSB first: 1010 0101) · ID 16 · flags 4 (bit 0 pairing) · temperature 9 bits × 0.1 °C · set point 9 × 0.1 °C ·
//  check 8 = the sum of the ID's, flags', temperature's and set point's bytes (low and high). A weak check: believed
//  when the burst holds two identical frames, else shown once heard twice (confirm).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace wattsrf {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    const Runs& r = b.ook;
    std::vector<Bits> frames;
    for (size_t i = 0; i < r.size(); ++i) {
        if (r[i] < 4000 || r[i] > 8000) continue;
        Bits f;
        size_t k = i + 1;
        for (; k + 1 < r.size() && f.size() < 54; k += 2) {
            const int g = -r[k], m = r[k + 1];
            if (g < 120 || g > 900 || m <= 0) break;
            if (m >= 150 && m < 450) f.push_back(0); else if (m >= 450 && m <= 900) f.push_back(1); else break;
        }
        if (f.size() == 54 && bitsAt(f, 0, 8) == 0xA5) frames.push_back(f);
        i = k - 1;
    }
    for (const Bits& f : frames) {
        const int id = int(bitsAtLsb(f, 8, 16)), flags = int(bitsAtLsb(f, 24, 4));
        const int t = int(bitsAtLsb(f, 28, 9)), sp = int(bitsAtLsb(f, 37, 9)), chk = int(bitsAtLsb(f, 46, 8));
        const int sum = (id & 0xFF) + (id >> 8) + flags + (t & 0xFF) + (t >> 8) + (sp & 0xFF) + (sp >> 8);
        if ((sum & 0xFF) != chk || (id == 0 && t == 0 && sp == 0)) continue;
        if (t > 500 || sp > 400) continue;
        Reading rd;
        rd.kind = "sensor"; rd.make = "Watts"; rd.model = "WFHT-RF thermostat";
        rd.pubId = std::to_string(id);
        rd.tempC = t / 10.0;
        rd.extra = "set " + kit::fmt1v(sp / 10.0, 1) + " \xc2\xb0" "C";
        if (flags & 1) rd.extra += " \xc2\xb7 pairing";
        rd.confirm = std::count(frames.begin(), frames.end(), f) < 2;
        rd.fact("id", (long long)id); rd.fact("setpoint_C", sp / 10.0); rd.fact("pairing", (long long)(flags & 1)); rd.fact("flags", (long long)flags);
        out.push_back(rd);
        return true;
    }
    return false;
}

} // namespace wattsrf

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  WATTS WFHT-LCD-RF underfloor-heating thermostat (the newer, CC1101-based generation) — 433.92 MHz (+~73 kHz) OOK
//  Manchester, ~2 170 chips/s (~461 µs). Facts: Steven Bontius, "WATTS_WFHT_RF_PROTOCOL.md"
//  (github.com/StevenBontius/watts-wfht-ha, docs/), cross-checked with rtl_433's watts_wfht_rf.c notes. Preamble
//  2A AA AA AA, sync D3 91 D3 91, then 16 bytes: length 0D · FF FF FE · mode (bit 0 pairing, bit 1 heat, the rest 0)
//  · device ID 3 · temperature 16 BE × 0.1 °C · set point 16 BE × 0.1 °C · call for heat (0 / 100) · CRC-8 (poly
//  0xE6, init 0, over length … set point, then XOR 0xBE and the call-for-heat byte) · CRC-16 (poly 0x8005, init 0xFFFF)
//  over length … CRC-8.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace wattslcd {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& d : kit::manchesterRows(b, 460.8, 150)) {
        for (size_t s : kit::findAll(d, 0xD391D391u, 32)) {
            if (s + 32 + 128 > d.size()) break;
            const auto by = bytesAt(d, s + 32, 16);
            if (by[0] != 0x0D || by[1] != 0xFF || by[2] != 0xFF || by[3] != 0xFE || (by[4] & 0xFC)) continue;
            if ((by[5] | by[6] | by[7]) == 0) continue;
            if (uint8_t(crc8(by.data(), 12, 0xE6, 0) ^ 0xBE ^ by[12]) != by[13]) continue;
            if (crc16(by.data(), 14, 0x8005, 0xFFFF) != uint16_t(by[14] << 8 | by[15])) continue;
            const double t = int16_t(by[8] << 8 | by[9]) / 10.0, sp = int16_t(by[10] << 8 | by[11]) / 10.0;
            Reading r;
            r.kind = "sensor"; r.make = "Watts"; r.model = "WFHT-LCD-RF thermostat";
            char ib[12]; std::snprintf(ib, sizeof ib, "%02X:%02X:%02X", by[5], by[6], by[7]); r.pubId = ib;
            r.tempC = t;
            r.extra = std::string(by[4] & 2 ? "heat" : "cool") + " to " + kit::fmt1v(sp, 1) + " \xc2\xb0" "C" +
                      (by[12] ? (by[4] & 2 ? " \xc2\xb7 calling for heat" : " \xc2\xb7 calling for cooling") : "") +
                      (by[4] & 1 ? " \xc2\xb7 pairing" : "");
            r.fact("setpoint_C", sp); r.fact("mode", by[4] & 2 ? "heat" : "cool"); r.fact("pairing", by[4] & 1 ? "true" : "false");
            r.fact("call_for_heat", (long long)by[12]);
            out.push_back(r);
            return true;
        }
    }
    return false;
}

} // namespace wattslcd

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  WATTS VISION thermostats and their central unit (CC110L) — 868.3 MHz FSK, 38.4 kbit/s (26 µs). Facts: rtl_433 issue
//  #2885 (analysis by floe, klohner, zuckschwerdt), the rtl_433_tests watts_vision cc110l analysis note, and rtl_433's
//  watts_vision.c notes (read for facts only). AA, sync D3 91 D3 91, then LEN (0x14 a command from the central unit,
//  0x22 a thermostat's status) · source ID 32 · C6 · destination ID 32 · records · CRC-16/MODBUS (0xA001 reflected,
//  init 0xFFFF, low byte first) over source … records · CRC-16 (0x8005, init 0xFFFF, high byte first) over LEN …
//  the MODBUS CRC. A record is a tag and (tag >> 6) + 1 value bytes; 0 ends them. Read: 0x4B measured temperature and
//  0x8A set point (16 BE, 0.1 °F; 0x084C = no sensor) + its mode byte, 0x03 the endpoint's slot.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace wattsvision {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    if (b.fsk.empty()) return false;
    for (const Bits& row : kit::nrzRows(b.fsk, 26.04, 24, true, 200)) {
        for (size_t s : kit::findAll(row, 0xD391D391u, 32)) {
            if (s + 32 + 8 > row.size()) break;
            const int len = int(bitsAt(row, s + 32, 8));
            if (len != 0x14 && len != 0x22) continue;
            if (s + 32 + size_t(len + 3) * 8 > row.size()) continue;
            const auto by = bytesAt(row, s + 32, len + 3);
            if (by[5] != 0xC6) continue;
            if (kit::crc16r(by.data() + 1, size_t(len - 2), 0xA001, 0xFFFF) != uint16_t(by[size_t(len)] << 8 | by[size_t(len - 1)])) continue;
            if (crc16(by.data(), size_t(len + 1), 0x8005, 0xFFFF) != uint16_t(by[size_t(len + 1)] << 8 | by[size_t(len + 2)])) continue;
            const uint32_t src = uint32_t(by[1]) << 24 | uint32_t(by[2]) << 16 | uint32_t(by[3]) << 8 | by[4];
            Reading r;
            r.kind = "sensor"; r.make = "Watts"; r.model = len == 0x14 ? "Vision central unit" : "Vision thermostat";
            char ib[12]; std::snprintf(ib, sizeof ib, "%08X", src); r.pubId = ib;
            r.fact("msg_type", len == 0x14 ? "command" : "status");
            static const char* const modes[17] = {"comfort", "off", "anti-freeze", "eco", "boost", nullptr, nullptr, nullptr, "auto (comfort)",
                                                  nullptr, nullptr, "auto (eco)", nullptr, nullptr, nullptr, nullptr, "manual"};
            std::string ex = len == 0x14 ? "command" : "";
            const int recEnd = 10 + (len - 11);
            for (int i = 10; i < recEnd;) {
                const int tag = by[size_t(i)];
                if (!tag) break;
                const int vl = (tag >> 6) + 1;
                if (i + 1 + vl > recEnd) break;
                const uint8_t* v = by.data() + i + 1;
                if (tag == 0x4B) { const int raw = v[0] << 8 | v[1]; if (raw != 0x084C) r.tempC = (raw / 10.0 - 32.0) / 1.8; }
                else if (tag == 0x8A) {
                    const double f = (v[0] << 8 | v[1]) / 10.0;
                    const char* m = v[2] <= 16 ? modes[v[2]] : nullptr;
                    ex += (ex.empty() ? "" : " \xc2\xb7 ") + std::string(m ? m : "mode") + " set " + kit::fmt1v((f - 32.0) / 1.8, 1) + " \xc2\xb0" "C";
                    r.fact("mode_setpoint_F", f);
                }
                else if (tag == 0x03) r.fact("association_id", (long long)v[0]);
                else if (tag == 0x3B) r.fact("flags_raw", (long long)v[0]);
                i += 1 + vl;
            }
            r.extra = ex;
            out.push_back(r);
            return true;
        }
    }
    return false;
}

} // namespace wattsvision

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  FUNKBUS / INSTAFUNK wall and hand remotes (Insta GmbH; sold by Berker, Gira, Jung …) — 433.42 MHz OOK, biphase-mark
//  (every bit boundary a transition; a 0 is one 1 ms run, a 1 two 500 µs runs) after a ~4 ms mark. Facts: the Insta
//  "instafunk RX/TX-Modul" notes as summarised in rtl_433's funkbus.c (Markus Sattler; read for facts only). 48 bits,
//  every field LSB first: type 4 (4 = remote) · subtype 4 (3) · serial 20 · 2 unknown · battery low · 2 unknown ·
//  button 3 · group 2 (A–C; 3 = scenes) · 1 unknown · action 2 (stop, off, on, scene) · repeat · long press · parity
//  · check 4. The check: XOR the first 43 bits as bytes (MSB first, the last byte's unused bits 0), fold to a nibble;
//  each of its bits 3…0 XORs in 0x8C, 0x32, 0xC8, 0x23; the low nibble, sent bit-reversed, is the check, and the
//  XOR byte's parity is the parity bit — with the fixed type a ~13-bit check, so a single frame is shown once heard
//  twice (confirm). ★ The serial commands that switch: SECRET, as every remote.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace funkbus {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    const Runs& r = b.ook;
    for (size_t i = 0; i + 40 < r.size(); ++i) {
        if (r[i] < 3000 || r[i] > 5500) continue;
        const Runs tail(r.begin() + long(i + 1), r.end());
        const auto rows = wx::dmc(tail, 500, 250);
        for (const Bits& row : rows) {
            if (row.size() < 48) continue;
            auto lsb = [&](int at, int n) { return int(bitsAtLsb(row, size_t(at), n)); };
            if (lsb(0, 4) != 4 || lsb(4, 4) != 3) continue;
            const auto by = kit::packBits(row, 0, 43);
            const uint8_t x = kit::xorBytes(by.data(), by.size());
            const int nib = (x >> 4) ^ (x & 0xF);
            uint8_t c = 0;
            if (nib & 8) c ^= 0x8C;
            if (nib & 4) c ^= 0x32;
            if (nib & 2) c ^= 0xC8;
            if (nib & 1) c ^= 0x23;
            c &= 0xF;
            const int rev = ((c & 1) << 3) | ((c & 2) << 1) | ((c & 4) >> 1) | ((c & 8) >> 3);
            if (lsb(44, 4) != rev || lsb(43, 1) != kit::parity(x)) continue;
            const int sn = lsb(8, 20), btn = lsb(33, 3), grp = lsb(36, 2), act = lsb(39, 2);
            Reading rd = homeRemote("Funkbus", "", 0xF0B, uint64_t(sn));
            static const char* const acts[4] = {"stop", "off", "on", "scene"};
            rd.button = (grp == 3 ? std::string("scene ") : std::string(1, char('A' + grp))) + std::to_string(btn);
            rd.extra = acts[act];
            if (lsb(42, 1)) rd.extra += " (long press)";
            rd.battOk = lsb(30, 1) ? 0 : 1;
            rd.confirm = !kit::twice(rows, row);   // a 4-bit check + parity: believed at once only when sent twice alike
            rd.fact("command", (long long)btn); rd.fact("group", (long long)grp); rd.fact("action", (long long)act);
            rd.fact("repeat", (long long)lsb(41, 1)); rd.fact("longpress", (long long)lsb(42, 1));
            out.push_back(rd);
            return true;
        }
    }
    return false;
}

} // namespace funkbus

/** PWM words of an OOK burst: a mark of ~`oneUs` = 1, ~`zeroUs` = 0 (each ±`tol`), every gap under `gapMax`; a longer gap
 *  (or a mark out of range) ends the word. */
inline std::vector<Bits> pwmWords(const Runs& r, double oneUs, double zeroUs, double tol, double gapMax) {
    std::vector<Bits> rows(1);
    for (size_t i = 0; i < r.size(); ++i) {
        const int d = r[i];
        if (d > 0) {
            if (std::fabs(d - oneUs) <= tol) rows.back().push_back(1);
            else if (std::fabs(d - zeroUs) <= tol) rows.back().push_back(0);
            else if (!rows.back().empty()) rows.emplace_back();
        } else if (-d > gapMax && !rows.back().empty()) rows.emplace_back();
    }
    if (rows.back().empty()) rows.pop_back();
    return rows;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  FERON LED-light remotes — 433.92 MHz OOK PWM: a 750 µs mark + 350 µs gap = 1, 350 + 750 = 0; 32 bits, then a 500 µs
//  gap, a 500 µs mark and ~4.5 ms, repeated. ID 16 · command 8 (0x85 on, 0x84 off, 0x86 brighter, 0x87 dimmer, 0x82
//  mode) · the command inverted. Facts: the Flipper Zero firmware's protocol notes (lib/subghz/protocols/feron.c) as
//  given in rtl_433's feron.c notes (read for facts only). A weak check (the inverted byte): two identical words in the
//  burst before it is believed — as rtl_433 itself asks. SECRET (a remote).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace feron {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    std::vector<Bits> rows = pwmWords(b.ook, 750, 350, 150, 1000);
    for (auto& w : rows) if (w.size() == 33) w.pop_back();   // the 500 µs end mark can read as a 33rd bit
    for (const Bits& w : rows) {
        if (w.size() != 32 || !kit::twice(rows, w)) continue;
        const uint32_t v = bitsAt(w, 0, 32);
        const int cmd = int(v >> 8 & 0xFF);
        if (((v ^ (v >> 8)) & 0xFF) != 0xFF) continue;
        Reading r = homeRemote("Feron", "LED light remote", 0xFE0, v >> 16);
        const char* c = cmd == 0x85 ? "on" : cmd == 0x84 ? "off" : cmd == 0x86 ? "brighter" : cmd == 0x87 ? "dimmer" : cmd == 0x82 ? "mode" : nullptr;
        char hx[16]; std::snprintf(hx, sizeof hx, "command %02X", cmd);
        r.extra = c ? c : hx;
        r.fact("command", (long long)cmd);
        out.push_back(r);
        return true;
    }
    return false;
}

} // namespace feron

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  POWER SMART blind remotes — 433.92 MHz OOK Manchester, 225 µs half-bits (1 = mark then gap), 64 bits sent 8 times.
//  Facts: the Flipper Zero firmware's notes (lib/subghz/protocols/power_smart.c) as given in rtl_433's power_smart.c
//  (read for facts only). FD · K CCCCCC D / DDDDDDD K (K = the button's two bits, first and last: 1 down, 2 up, 3 stop;
//  C = 6-bit channel) · Y · AA · the two K…K bytes inverted · W, where Y = ~W − 1. 16 fixed bits, a 16-bit inverted copy
//  and the Y/W pair: shown at once. SECRET (the C D Y bits are the remote's address).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace powersmart {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& d : kit::manchesterRows(b, 225, 64)) {
        for (size_t s : kit::findAll(d, 0xFD, 8)) {
            if (s + 64 > d.size()) break;
            const auto by = bytesAt(d, s, 8);
            if (by[4] != 0xAA || (by[1] ^ by[5]) != 0xFF || (by[2] ^ by[6]) != 0xFF || by[3] != uint8_t(~by[7] - 1)) continue;
            const int button = ((by[1] >> 6) & 2) | (by[2] & 1), channel = (by[1] >> 1) & 0x3F;
            const uint32_t id = ((uint32_t(by[1] & 0x7F) << 7 | (by[2] >> 1)) << 8) | by[3];
            Reading r = homeRemote("Power Smart", "blind remote", 0x957, id);
            static const char* const ctl[4] = {"?", "down", "up", "stop"};
            r.button = std::to_string(channel);
            r.extra = ctl[button];
            r.fact("channel", (long long)channel); r.fact("button", (long long)button);
            out.push_back(r);
            return true;
        }
    }
    return false;
}

} // namespace powersmart

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  MARTEC MPLCD ceiling-fan / light remotes — 433.92 MHz OOK PWM (a ~300 µs mark = 1, ~650 µs = 0), 22 bits, repeated.
//  Facts: rtl_433 PR #3133 and its martec_mplcd.c notes (read for facts only). 1000 · channel 4 (bit-reversed,
//  inverted) · dimmer 7 (0 off, 1–41 on, 1 the brightest) · fan 2 (off, high, medium, low) · 1 unknown · check 4 = the
//  sum of the four nibbles from bit 1. A 4-bit check: two identical rows in the burst, and not all zero.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace martec {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    const std::vector<Bits> rows = pwmWords(b.ook, 320, 676, 150, 900);
    for (const Bits& w : rows) {
        if (w.size() != 22 || !kit::twice(rows, w) || bitsAt(w, 0, 4) != 0x8) continue;
        const auto by = kit::packBits(w, 1, 21);
        const int sum = ((by[0] >> 4) + (by[0] & 0xF) + (by[1] >> 4) + (by[1] & 0xF)) & 0xF;
        if (sum != ((by[2] >> 3) & 0xF) || (by[0] == 0 && by[1] == 0)) continue;
        int ch = (~by[0] >> 1) & 0xF; ch = ((ch & 1) << 3) | ((ch & 2) << 1) | ((ch & 4) >> 1) | ((ch & 8) >> 3);
        int dim = ((by[0] & 1) << 6) + ((by[1] >> 2) & 0x3F);
        if (dim > 41) continue;
        if (dim > 0) dim = 42 - dim;
        static const char* const fan[4] = {"off", "high", "medium", "low"};
        Reading r = homeRemote("Martec", "fan remote", 0x3A7, uint64_t(ch));
        r.button = std::to_string(ch);
        r.extra = (dim ? "light " + std::to_string(dim * 100 / 41) + " %" : std::string("light off")) + " \xc2\xb7 fan " + fan[by[1] & 3];
        r.fact("dimmer", (long long)dim); r.fact("speed", fan[by[1] & 3]);
        out.push_back(r);
        return true;
    }
    return false;
}

} // namespace martec

/** The home-automation families, strongest checks first. */
inline bool decodeHomeauto(const Burst& b, std::vector<Reading>& out) {
    if (insteon::decode(b, out)) return true;
    if (danfoss::decode(b, out)) return true;
    if (evohome::decode(b, out)) return true;
    if (wattslcd::decode(b, out)) return true;
    if (wattsvision::decode(b, out)) return true;
    if (powersmart::decode(b, out)) return true;
    if (funkbus::decode(b, out)) return true;
    if (adlm::decode(b, out)) return true;
    if (wattsrf::decode(b, out)) return true;
    if (feron::decode(b, out)) return true;
    if (martec::decode(b, out)) return true;
    return false;
}

/** ★ Where each home-automation family transmits (bandPlausible). `false` = refuse. */
inline bool homeautoBandOk(const Reading& r, double f) {
    if (r.make == "Honeywell" && r.model == "evohome") return f >= 863e6 && f <= 870e6;
    if (r.make == "Watts" && r.model == "WFHT-RF thermostat") return f >= 433.0e6 && f <= 435.0e6;
    if (r.make == "Funkbus") return f >= 433.0e6 && f <= 435.0e6;
    if (r.make == "Feron" || r.make == "Power Smart" || r.make == "Martec") return f >= 433.0e6 && f <= 435.0e6;
    if (r.make == "ADLM FPRF") return f >= 433.0e6 && f <= 435.0e6;
    if (r.make == "Insteon") return (f >= 902e6 && f <= 928e6) || (f >= 863e6 && f <= 870e6);
    return true;
}

} // namespace vibeism
