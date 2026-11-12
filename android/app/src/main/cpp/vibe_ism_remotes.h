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

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  MICROCHIP HCS361 KeeLoq encoders (car and gate remotes: Lear, Marelli, Jeep / Chrysler fobs) — Microchip HCS361
//  datasheet DS40146F: PWM, TE 100–400 µs, a bit = 3 TE (1 TE high = 1, 2 TE high = 0), a 10 TE high + 10 TE low sync
//  after a short preamble; 67 bits LSB first: 32 encrypted · 28 serial · buttons S3 S0 S1 S2 · VLOW · a 2-bit CRC over the
//  first 65 (the datasheet's serial CRC; its VLOW-flipped value is accepted too). ★ PRIVACY: only "Key fob · Microchip
//  HCS361 · button 2" — never the serial, never the hop code. The 2-bit CRC is weak: confirm.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace hcs361 {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    const Runs& r = b.ook;
    for (size_t i = 0; i + 2 * 67 + 1 < r.size(); ++i) {
        if (r[i] <= 0 || r[i + 1] >= 0) continue;
        const double te = r[i] / 10.0;
        if (te < 80 || te > 450 || !near(r[i + 1], 10 * te, 0.25)) continue;
        Bits bits; size_t k = i + 2;
        while (bits.size() < 67 && k < r.size() && r[k] > 0) {
            const int mk = r[k];
            const bool one = mk < 1.5 * te;
            if (!near(mk, one ? te : 2 * te, 0.4)) break;
            if (bits.size() < 66 && (k + 1 >= r.size() || r[k + 1] >= 0 || !near(mk - r[k + 1], 3 * te, 0.3))) break;
            bits.push_back(one ? 1 : 0);
            k += 2;
        }
        if (bits.size() != 67) continue;
        int crc = 0, crcLow = 0;
        for (int j = 0; j < 65; ++j) {
            const int bit = bits[size_t(j)];
            if (j == 64) { const int c = ((crc >> 1) ^ (bit ^ 1)) & 1; crcLow = c | (((c ^ crc) << 1) & 2); }
            const int c = ((crc >> 1) ^ bit) & 1;
            crc = c | (((c ^ crc) << 1) & 2);
        }
        const int got = bits[65] << 1 | bits[66];
        if (got != crc && got != crcLow) continue;
        uint32_t serial = 0; for (int j = 0; j < 28; ++j) serial |= uint32_t(bits[size_t(32 + j)]) << j;
        if (serial == 0) continue;
        const int s3 = bits[60], s0 = bits[61], s1 = bits[62], s2 = bits[63];
        Reading rd = remoteReading("keyfob", "Microchip HCS361", "Key fob / remote", 22, serial);
        std::string bt; const int bv[4] = {s0, s1, s2, s3};
        for (int q = 0; q < 4; ++q) if (bv[q]) { if (!bt.empty()) bt += "+"; bt += char('1' + q); }
        rd.button = bt;
        rd.battOk = bits[64] ? 0 : 1;
        rd.confirm = true;
        rd.fact("button", (long long)(s0 | s1 << 1 | s2 << 2 | s3 << 3));
        out.push_back(rd);
        return true;
    }
    return false;
}

} // namespace hcs361

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  NEXA / KLIKAANKLIKUIT / PROOVE / HOME EASY "self-learning" switches, remotes and door sensors (also Anslut, Intertechno
//  ITLS, Smartwares, REV Telecontrol) — 433.92 MHz OOK. Source: "RF Protokoll – Proove self learning", elektronikforumet.com
//  wiki: pulses of T ≈ 270 µs; a gap of T or 5T after each — every data bit sent as two such (1 → long-short as the
//  first-of-pair rule reads it); a sync gap of ~10T before, ~40T after; 32 data bits (36 with a dim level): a 26-bit
//  address · group · on/off · channel 2 + unit 2 (both inverted) — KlikAanKlikUit reads those last four as one unit
//  number; an on/off pair sent "11" means DIM, with 4 level bits added (rtl_433's newkaku.c notes). Five to fifteen
//  repeats: twice alike in the burst.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace nexa {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    const auto rows = kit::ppmRows(b.ook, 270, 1300, 2000, 120, 500, 0.45);
    for (const Bits& row0 : rows) {
        Bits row = row0;
        if (row.size() == 65 || row.size() == 73) row.pop_back();   // the stop pulse's gap read as a bit
        if (row.size() != 64 && row.size() != 72) continue;
        if (!kit::twice(rows, row0)) continue;
        Bits d; bool ok = true, dim = false;
        for (size_t i = 0; i + 1 < row.size() && ok; i += 2) {
            if (row[i] == row[i + 1]) { if (i == 54 && row[i] == 1) { dim = true; d.push_back(1); continue; } ok = false; break; }
            d.push_back(row[i]);
        }
        if (!ok || d.size() < 32) continue;
        const uint32_t id = bitsAt(d, 0, 26);
        if (id == 0 || id == 0x3FFFFFF) continue;
        const int group = d[26], on = d[27];
        const int chan = int(bitsAt(d, 28, 2)) ^ 3, unitP = int(bitsAt(d, 30, 2)) ^ 3, unitK = int(bitsAt(d, 28, 4));
        Reading r = remoteReading("doorbell", "Nexa / KlikAanKlikUit", "self-learning", 23, id);
        r.button = std::to_string(unitK + 1);
        r.extra = dim ? "dim" : on ? "on" : "off";
        if (dim && d.size() >= 36) r.extra += " \xc2\xb7 level " + std::to_string(bitsAt(d, 32, 4));
        if (group) r.extra += " \xc2\xb7 group";
        // ★ the bench's values: the write-ups disagree on what to call these fields — rtl_433's Intertechno reading
        //   (an address starting 0x56 / 0x69 on the air) reports raw on-air nibbles where the others report on/off
        const uint32_t lead = bitsAt(row, 0, 8);
        if (lead == 0x56 || lead == 0x69) {
            r.fact("command", (long long)bitsAt(row, 53, 3)); r.fact("master", (long long)bitsAt(row, 56, 4)); r.fact("slave", (long long)bitsAt(row, 60, 4));
        } else r.fact("command", on || dim ? "On" : "Off");
        r.fact("state", on || dim ? "ON" : "OFF");
        r.fact("group", (long long)group); r.fact("group_call", group ? "Yes" : "No"); r.fact("dim", dim ? "Yes" : "No");
        r.fact("channel", (long long)chan);
        (void)unitP;
        out.push_back(r);
        return true;
    }
    return false;
}

} // namespace nexa

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  X10 RF (lamp / appliance remotes and their motion sensors, 310 / 433 MHz) — OOK, NEC-like: a 9 ms mark and 4.5 ms gap,
//  then 32 bits of 562 µs marks with a 562 µs or a 1687 µs gap; five copies. Source: WGL Designs, "W800RF32 protocol"
//  (wgldesigns.com/protocols/w800rf32_protocol.txt): byte 2 = ¬byte 1, byte 4 = ¬byte 3; the house code (the standard X10
//  RF table — not the power-line one: A = 0110, B = 0111, C = 0100 … P = 0011) in byte 1's top nibble; the unit in byte 1 bit 2 and byte 3 bits 6, 4, 3; byte 3 bit 5 =
//  off; byte 3 bit 7 = a special (0x80 all off, 0x90 all lights on, 0x88 bright, 0x98 dim). The fixed bits as rtl_433's
//  x10_rf.c checks them. ★ The house + unit is the address: secret.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace x10 {

inline char house(int nib) {
    static const int code[16] = {0x6, 0x7, 0x4, 0x5, 0x8, 0x9, 0xA, 0xB, 0xE, 0xF, 0xC, 0xD, 0x0, 0x1, 0x2, 0x3};   // A … P on the air
    for (int i = 0; i < 16; ++i) if (code[i] == nib) return char('A' + i);
    return '?';
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    const auto rows = kit::ppmRows(b.ook, 562, 1687, 3000, 300, 900, 0.35);
    for (const Bits& w0 : rows) {
        Bits w = w0;
        if (w.size() == 33) w.pop_back();
        if (w.size() != 32) continue;   // ★ not "twice in the burst": the copies come 40 ms apart, often a burst each
        const auto x = bytesAt(w, 0, 4);   // ★ bytes as the long-gap = 1 reading gives them (the doc calls that 0; the checks are symmetric)
        if ((x[0] ^ x[1]) != 0xFF || (x[2] ^ x[3]) != 0xFF) continue;
        if ((x[0] & 0x0B) != 0x00 || (x[1] & 0x0B) != 0x0B || (x[2] & 0x07) != 0 || (x[3] & 0x07) != 0x07) continue;
        const char hc = house(x[0] >> 4);
        int unit = ((x[0] & 0x04) << 1 | (x[2] & 0x40) >> 4 | (x[2] & 0x08) >> 2 | (x[2] & 0x10) >> 4) + 1;
        std::string ev = (x[2] & 0x20) ? "OFF" : "ON";
        if (x[2] & 0x80) {
            unit = 0;
            ev = x[2] == 0x98 ? "DIM" : x[2] == 0x88 ? "BRI" : x[2] == 0x90 ? "ALL LTS ON" : x[2] == 0x80 ? "ALL OFF" : "UNKNOWN";
        }
        Reading r = remoteReading("doorbell", "X10", "remote", 24, uint64_t(hc) << 8 | uint64_t(unit));
        r.confirm = !kit::twice(rows, w0);   // one copy in this burst: shown once heard twice alike
        std::string low = ev; for (auto& c : low) c = char(std::tolower(c));
        r.extra = low;
        r.fact("state", ev.c_str()); r.fact("channel", std::string(1, hc).c_str());
        out.push_back(r);
        return true;
    }
    return false;
}

} // namespace x10

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  SOMFY RTS (blinds, awnings, shutters; 433.42 MHz) — OOK, ~604 µs symbols. Sources: "Somfy RTS protocol",
//  pushstack.wordpress.com, and US patent 8 189 620: a hardware sync (2416 µs on / off, twice; seven times on a repeat),
//  a ~4.8 ms software sync and a 604 µs gap, then 56 Manchester bits (low-then-high = 1), each byte XORed with the
//  previous scrambled one; descrambled: a seed · command nibble + checksum nibble (all 14 nibbles XOR to 0) · a 16-bit
//  rolling counter · the 24-bit address. A command of 0xF puts the button in the seed's low nibble (TEL-FIX and
//  SCE-R1 remotes, per rtl_433's somfy_rts.c notes). ★ PRIVACY: the address and the rolling counter are never shown.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace somfy {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    static const char* const ctl[16] = {"? (0)", "My (1)", "Up (2)", "My + Up (3)", "Down (4)", "My + Down (5)", "Up + Down (6)",
                                        "My + Up + Down (7)", "Prog (8)", "Sun + Flag (9)", "Flag (10)", "? (11)", "? (12)",
                                        "? (13)", "? (14)", "? (15)"};
    static const char* const seedCtl[16] = {"? (0)", "? (1)", "? (2)", "? (3)", "? (4)", "Stop (5)", "Up (6)", "? (7)",
                                            "Down (8)", "? (9)", "? (10)", "? (11)", "Prog (12)", "? (13)", "? (14)", "? (15)"};
    for (const Bits& row : kit::nrzRows(b.ook, 604, 20, false, 120)) {
        // the end of a sync: … 1111 0000 then the software sync 1111 1111 (7 or 8 chips) and one 0
        for (uint32_t pat : {0x1E1E1FEu, 0x0F0F0FEu}) {
            const int n = pat == 0x1E1E1FEu ? 25 : 24;
            for (size_t s : kit::findAll(row, pat, n)) {
                const Bits d = kit::manchesterAt(row, s + size_t(n), 80);
                if (d.size() < 56) continue;
                auto by = bytesAt(d, 0, 7);
                for (int i = 6; i > 0; --i) by[size_t(i)] ^= by[size_t(i - 1)];
                uint8_t x = kit::xorBytes(by.data(), 7);
                if (((x & 0xF) ^ (x >> 4)) != 0) continue;
                const int control = by[1] >> 4;
                const uint32_t addr = uint32_t(by[6]) << 16 | uint32_t(by[5]) << 8 | by[4];
                if (addr == 0 || addr == 0xFFFFFF) continue;
                const char* cs = control == 0xF ? seedCtl[by[0] & 0xF] : ctl[control];
                Reading r = remoteReading("doorbell", "Somfy", "RTS remote", 25, addr);
                std::string w = cs; const size_t sp = w.find(" ("); if (sp != std::string::npos) w.resize(sp);
                for (auto& c : w) c = char(std::tolower(c));
                r.extra = w;
                r.fact("control", cs);
                out.push_back(r);
                return true;
            }
        }
    }
    return false;
}

} // namespace somfy

/** The remote families, strongest checks first. */
inline bool decodeRemotes(const Burst& b, std::vector<Reading>& out) {
    if (lightwave::decode(b, out)) return true;
    if (gecolor::decode(b, out)) return true;
    if (hcs361::decode(b, out)) return true;
    if (nexa::decode(b, out)) return true;
    if (x10::decode(b, out)) return true;
    if (somfy::decode(b, out)) return true;
    return false;
}

} // namespace vibeism
