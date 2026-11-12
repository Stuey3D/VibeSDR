// vibe_ism_meters2.h — V12 ISM decoder: METERS AND MONITORS, second pass (2026-11-12) — the energy and utility
// families vibe_ism_meters.h did not read yet.
//
// ★★★ SOURCES — the same rule as vibe_ism_meters.h (Stuart, 2026-11-11: "we dont want to copy but if we can take
//     inspiration"). Frame FACTS — layout, timing, line code, sync word, check polynomial, scaling — come from public
//     write-ups first; where the only description is a GPL decoder's notes (rtl_433, src/devices/*.c), those were read
//     for the facts and nothing else: no code, table, comment or identifier is copied. Every decoder here is our own,
//     on our own slicers (vibe_ism_kit.h). Checked on the rtl_433_tests recordings (test INPUT only —
//     scripts/ism-corpus.mjs; never in the repository). Each family says where its facts came from.
// ★ Meters may show their serial (Stuart, 2026-10-25), as in vibe_ism_meters.h.
#pragma once
#include "vibe_ism_kit.h"

namespace vibeism {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  IKEA SPARSNÄS electricity monitor (a meter-LED reader + display) — 868 MHz FSK NRZ, ~26 µs bits (38.4 kbit/s).
//  Facts: the reverse-engineering write-ups at github.com/kodarn/Sparsnas (README, the packet and the obfuscation) and
//  github.com/strigeus/sparsnas_decoder (README), cross-checked with rtl_433's ikea_sparsnas.c notes. Preamble AA AA,
//  sync D2 01, then 20 bytes: length 0x11 · ID low byte · ? · version 07 · mode byte (mode = byte ^ 0x0F) · then 13
//  bytes XOR-ed with a 5-byte key repeating (sensor ID 32 · sequence 16 · power 16 · pulse count 32 · battery %) ·
//  CRC-16 poly 0x8005, init 0xFFFF, MSB first, over bytes 0–17.
//  The key is (ID − 0x5D38E8CB) as bytes [3], [0], [1], 0x47, [2] (3 = the top byte). The ID is the last six digits
//  on the sender's sticker, so ≤ 999 999 (top byte 0, next ≤ 0x0F): our own solve — the 4 096 IDs that fit are each
//  tried until the ID's own encrypted bytes and the key agree (the rtl_433 notes brute-force 2³²; the arithmetic here
//  is ours). The obfuscation is keyed by the sender's own ID, which the frame carries: it hides nothing from anyone.
//  ASSUMED: "power" is in watts as the write-ups label it (the display's own figure); kWh needs the meter's imp/kWh,
//  which the frame does not carry — the pulse count is shown as it is.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace sparsnas {

inline bool frame(const std::vector<uint8_t>& by, std::vector<Reading>& out) {
    if (by[0] != 0x11 || by[3] != 0x07) return false;
    if (crc16(by.data(), 18, 0x8005, 0xFFFF) != uint16_t(by[18] << 8 | by[19])) return false;
    for (uint32_t hi = 0; hi < 0x1000; ++hi) {
        const uint32_t id = (hi << 8) | uint8_t(by[8] ^ 0x47);
        if (id > 999999) break;
        const uint32_t k = id - 0x5D38E8CBu;
        const uint8_t key[5] = {uint8_t(k >> 24), uint8_t(k), uint8_t(k >> 8), 0x47, uint8_t(k >> 16)};
        uint8_t d[13];
        for (int i = 0; i < 13; ++i) d[i] = by[size_t(5 + i)] ^ key[i % 5];
        if ((uint32_t(d[0]) << 24 | uint32_t(d[1]) << 16 | uint32_t(d[2]) << 8 | d[3]) != id) continue;
        if (d[12] > 100) continue;
        const int seq = d[4] << 8 | d[5], power = d[6] << 8 | d[7];
        const uint32_t pulses = uint32_t(d[8]) << 24 | uint32_t(d[9]) << 16 | uint32_t(d[10]) << 8 | d[11];
        Reading r;
        r.kind = "energy"; r.make = "IKEA"; r.model = "Sparsn\xc3\xa4s energy monitor";
        r.pubId = std::to_string(id);
        r.watts = power;
        r.extra = std::to_string(pulses) + " meter pulses";
        r.battOk = d[12] >= 10 ? 1 : 0;
        r.fact("id", (long long)id); r.fact("sequence", (long long)seq); r.fact("effect", (long long)power);
        r.fact("pulses", (long long)pulses); r.fact("mode", (long long)(by[4] ^ 0x0F));
        out.push_back(r);
        return true;
    }
    return false;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    if (b.fsk.empty()) return false;
    for (const Bits& row : kit::nrzRows(b.fsk, 26.04, 24, true, 180))
        for (size_t s : kit::findAll(row, 0xAAD201, 24)) {
            if (s + 24 + 160 > row.size()) break;
            if (frame(bytesAt(row, s + 24, 20), out)) return true;
        }
    return false;
}

} // namespace sparsnas

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  OWL CM160 / CM180 / CM130 electricity monitors (2 Save Energy Ltd; sold as OWL, sensor clamp + display) — 433.92
//  OOK Manchester at the Oregon Scientific v3 rate (~1024 Hz), nibbles LSB first, as our Oregon reader takes them
//  (vibe_ism_protocols.h, the "RF Protocols" PDF). Facts: the OWL notes in rtl_433's oregon_scientific.c (read for the
//  facts only), cross-checked on the corpus. Nibble k below = the k-th nibble after the preamble.
//   • CM160: after the 0101 sync, nibble 0 = 2, nibble 1 = 0, 1 or 4 (★ 2 and 3 are NOT read: the EBP50 boiler
//     thermostat sends this very framing with 2 = off, 3 = on — corpus ebp50 — and its frame is no power reading); ID = nibble 3; current count = nibbles 6–8 (LE) ×
//     0.07 A; lifetime count = nibbles 11–21 (LE). Check: the Oregon nibble sum of nibbles 1–21 = nibbles 22, 23.
//     Watts and kWh need the mains voltage, which the clamp does not know: 230 V ASSUMED (as the display's set-up),
//     and the notes' ×1.12 display factor for kWh.
//   • CM180: NO sync — the first nibbles after the preamble are 2, 6. Nibble sum of 1–22 = nibbles 23, 24.
//     Sequence nibble 2 · ID nibbles 3–5 · battery low = nibble 6 bit 0 · power = nibbles 7–9 (as a 16-bit value's top
//     12 bits) × 1.00625 W · when the sequence is 0, a lifetime count in watt-seconds = nibbles 10–21 (LE).
//   • CM130: NO sync — 6, 0 first; byte k = nibbles (2k, 2k+1) high-low; CRC-8 poly 0x07 init 0 over bytes 1–10 =
//     byte 11 with its nibbles swapped. With every byte's nibbles swapped: ID byte 2, power = LE16 bytes 3–4 × 16 W,
//     lifetime = LE32 bytes 6–9 / 8192 kWh.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace owl {

inline bool fromNibbles(const std::vector<int>& nb, bool synced, std::vector<Reading>& out) {
    auto le = [&](int from, int count) { uint64_t v = 0; for (int i = count - 1; i >= 0; --i) v = v << 4 | uint64_t(nb[size_t(from + i)]); return v; };
    Reading r;
    r.kind = "energy"; r.make = "OWL";
    if (synced && nb.size() >= 24 && nb[0] == 2 && (nb[1] <= 1 || nb[1] == 4)) {
        int sum = 0; for (int i = 1; i <= 21; ++i) sum += nb[size_t(i)];
        if ((sum & 0xFF) != (nb[22] | nb[23] << 4)) return false;
        const int amps = int(le(6, 3));
        const double kwh = double(le(11, 11)) * 230.0 / 3.6e6 * 1.12;
        r.model = "CM160";
        r.pubId = std::to_string(nb[3]);
        r.watts = amps * 0.07 * 230.0;
        r.extra = kit::fmt1v(kwh, 1) + " kWh";
        r.note = "230 V assumed";
        r.fact("id", (long long)nb[3]); r.fact("energy_kWh", kwh);
        out.push_back(r);
        return true;
    }
    if (synced) return false;
    if (nb.size() >= 25 && nb[0] == 2 && nb[1] == 6) {
        int sum = 0; for (int i = 1; i <= 22; ++i) sum += nb[size_t(i)];
        if ((sum & 0xFF) != (nb[23] | nb[24] << 4)) return false;
        const int seq = nb[2];
        const int id = nb[5] << 12 | nb[4] << 8 | nb[3] << 4;
        const unsigned power = unsigned(double(nb[9] << 12 | nb[8] << 8 | nb[7] << 4) * 1.00625);
        r.model = "CM180";
        r.pubId = std::to_string(id);
        r.watts = power;
        r.battOk = (nb[6] & 1) ? 0 : 1;
        r.fact("id", (long long)id); r.fact("sequence", (long long)seq);
        if (seq == 0) {
            const double kwh = double(le(10, 12)) / 3.6e6;
            if (kwh > 0) { r.extra = kit::fmt1v(kwh, 2) + " kWh"; r.fact("energy_kWh", kwh); }
        }
        out.push_back(r);
        return true;
    }
    if (nb.size() >= 24 && nb[0] == 6 && nb[1] == 0) {
        uint8_t by[12];
        for (int k = 0; k < 12; ++k) by[k] = uint8_t(nb[size_t(2 * k)] << 4 | nb[size_t(2 * k + 1)]);
        if (crc8(by + 1, 10, 0x07, 0) != uint8_t(nb[23] << 4 | nb[22])) return false;
        uint8_t m[12];
        for (int k = 0; k < 12; ++k) m[k] = uint8_t(by[k] << 4 | by[k] >> 4);
        const int power = (m[4] << 8 | m[3]) * 16;
        const uint32_t cnt = uint32_t(m[6]) | uint32_t(m[7]) << 8 | uint32_t(m[8]) << 16 | uint32_t(m[9]) << 24;
        r.model = "CM130";
        r.pubId = std::to_string(m[2]);
        r.watts = power;
        r.extra = kit::fmt1v(cnt / 8192.0, 1) + " kWh";
        r.fact("id", (long long)m[2]); r.fact("energy_kWh", cnt / 8192.0);
        out.push_back(r);
        return true;
    }
    return false;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    const HalfLimits L{615, 1100, 850, 1400, 200, 300};
    for (const Bits& row : halfBits(b.ook, L)) {
        if (row.size() < 80) continue;
        for (const Bits& bits : oregon::segments(row, true)) {
            for (size_t i = 0; i + 8 < bits.size(); ++i) {
                size_t run = 0;
                while (i + run < bits.size() && bits[i + run]) ++run;
                if (run >= 12) {
                    const size_t s = i + run;
                    for (int synced = 0; synced < 2; ++synced) {
                        const size_t p0 = s + (synced ? 4 : 0);
                        if (synced && !(s + 4 <= bits.size() && bitsAt(bits, s, 4) == 0x5)) continue;
                        std::vector<int> nb;
                        for (size_t p = p0; p + 4 <= bits.size(); p += 4) nb.push_back(int(bitsAtLsb(bits, p, 4)));
                        if (fromNibbles(nb, synced, out)) return true;
                    }
                }
                i += run;
            }
        }
    }
    return false;
}

} // namespace owl

/** The second-pass meter families, strongest checks first. */
inline bool decodeMeters2(const Burst& b, std::vector<Reading>& out) {
    if (sparsnas::decode(b, out)) return true;
    if (owl::decode(b, out)) return true;
    return false;
}

/** ★ Where each second-pass family transmits — a reading anywhere else passed its check by chance (bandPlausible).
 *  `false` = refuse. Families not listed are left to the general rule. */
inline bool meters2BandOk(const Reading& r, double f) {
    const bool b868 = f >= 863e6 && f <= 870e6;
    if (r.make == "IKEA") return b868;
    if (r.make == "OWL") return f >= 433.0e6 && f <= 435.0e6;
    return true;
}

} // namespace vibeism
