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

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  VOLTCRAFT ENERGYCOUNT 3000 ("EC3k", Conrad) — and its rebadges Technoline Cost Control RT-110, Velleman NETBESEM4,
//  La Crosse RS3620 — plug-in energy meters, 868.3 MHz FSK, 50 µs bits. Facts: the AX5042 transceiver's HDLC mode
//  (onsemi AX5042 datasheet), the write-ups at sevenwatt.com ("RFM69 Energy Count 3000") and github.com/avian2/ec3k's
//  README, cross-checked with rtl_433's ec3k.c notes (read for facts only). On the air: NRZI (no change = 1), scrambled
//  by the self-synchronising x¹⁷ + x¹² + 1 (each bit XOR the ones 12 and 17 before it), then HDLC — 0x7E flags, a 0
//  stuffed after five 1s, bytes LSB first: 41 bytes, the last two a CRC-16 (poly 0x1021 reflected, init 0xFFFF,
//  complemented, low byte first) over the 39 before. Nibbles, high first: ID 1–4 · seconds total 5–8 (low 16) · 0 ×4 ·
//  seconds on 13–16 (low) · 0 ×7 · energy Ws 24–30 (low 28 bits) · power 31–34 (0.1 W) · peak power 35–38 (0.1 W) ·
//  39–44 a second energy count · 59–61 seconds total (high 12) · 0 ×5 · 67–70 energy (high 16) · 71–73 seconds on
//  (high) · 74–75 resets · 76 flags · 0.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace ec3k {

inline bool frame(const uint8_t* p, std::vector<Reading>& out) {
    auto nib = [&](int from, int n) { uint64_t v = 0; for (int i = 0; i < n; ++i) v = v << 4 | uint64_t((p[(from + i) / 2] >> ((from + i) & 1 ? 0 : 4)) & 0xF); return v; };
    const uint16_t crc = uint16_t(~kit::crc16r(p, 39, 0x8408, 0xFFFF));
    if (crc != uint16_t(p[39] | p[40] << 8)) return false;
    if (nib(9, 4) || nib(17, 7) || nib(62, 5) || nib(77, 1)) return false;
    const int id = int(nib(1, 4));
    const uint64_t ws = nib(67, 4) << 28 | nib(24, 7);
    const double power = double(nib(31, 4)) / 10.0, peak = double(nib(35, 4)) / 10.0;
    const uint32_t total = uint32_t(nib(59, 3) << 16 | nib(5, 4)), on = uint32_t(nib(71, 3) << 16 | nib(13, 4));
    Reading r;
    r.kind = "energy"; r.make = "Voltcraft"; r.model = "EnergyCount 3000";
    char ib[8]; std::snprintf(ib, sizeof ib, "%04X", id); r.pubId = ib;
    r.watts = power;
    r.extra = kit::fmt1v(ws / 3.6e6, 2) + " kWh \xc2\xb7 peak " + kit::fmt1v(peak, 1) + " W";
    r.fact("id", (long long)id); r.fact("power", power); r.fact("energy", ws / 3.6e6); r.fact("power_max", peak);
    r.fact("time_total", (long long)total); r.fact("time_on", (long long)on);
    r.fact("reset_counter", (long long)nib(74, 2)); r.fact("flags", (long long)nib(76, 1));
    out.push_back(r);
    return true;
}

/** ★ Every frame in the burst is read: two plugs a few ms apart arrive as ONE burst (corpus ec3k gfile007), and each
 *  is its own reading. A frame already read (the same 41 bytes, its repeat) is not read twice. */
inline bool decode(const Burst& b, std::vector<Reading>& out) {
    if (b.fsk.empty()) return false;
    std::vector<std::vector<uint8_t>> seen;
    for (const Bits& raw : kit::nrzRows(b.fsk, 50.0, 40, false, 400)) {
        const size_t n = raw.size();
        std::vector<uint8_t> sy(n), d(n);
        for (size_t i = 1; i < n; ++i) sy[i] = raw[i] == raw[i - 1];
        for (size_t i = 0; i < n; ++i) d[i] = uint8_t(sy[i] ^ (i >= 17 ? sy[i - 17] : 0) ^ (i >= 12 ? sy[i - 12] : 0));
        // HDLC: a flag (0 111111 0) opens; five 1s then a 0 = a stuffed 0; bytes LSB first
        int ones = 0; bool in = false; uint8_t pk[41]; size_t nb = 0; int bit = 0; uint8_t cur = 0;
        for (size_t i = 18; i < n; ++i) {
            if (d[i]) {
                if (++ones > 6) { in = false; ones = 0; }
                else if (in) { cur = uint8_t(cur >> 1 | 0x80); if (++bit == 8) { pk[nb++] = cur; bit = 0; } }
            } else {
                if (ones == 6) { in = true; nb = 0; bit = 0; cur = 0; }
                else if (ones != 5 && in) { cur = uint8_t(cur >> 1); if (++bit == 8) { pk[nb++] = cur; bit = 0; } }
                ones = 0;
            }
            if (nb == 41) {
                std::vector<uint8_t> v(pk, pk + 41);
                if (std::find(seen.begin(), seen.end(), v) == seen.end() && frame(pk, out)) seen.push_back(v);
                in = false; nb = 0;
            }
        }
    }
    return !seen.empty();
}

} // namespace ec3k

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  ESIC EMT7110 plug-in energy meter (for the EMR7370 display; sold by Pearl / Velleman) — 868.28 MHz FSK (±90 kHz),
//  NRZ at 9.579 kbit/s (~104.4 µs). Facts: Petter Reinholdtsen's samples and analysis as summarised in rtl_433's
//  esic_emt7110.c notes (read for facts only). AA AA, sync 2D D4, 12 bytes: ID 32 · pairing (bit 7), load on (bit 6),
//  power 14 bits × 0.5 W · current 16 bits mA · voltage = (byte + 256) / 2 V · 2 unknown bits + energy 14 bits ×
//  0.01 kWh · a check byte making the sum of all 12 zero. A weak check: believed at once when the burst's two packets
//  (14 ms apart) agree, else shown once heard twice (confirm).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace emt7110 {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    std::vector<std::vector<uint8_t>> got;
    for (const Bits& row : kit::nrzRowsAny(b, 104.4, 48, 110))
        for (size_t s : kit::findAll(row, 0xAA2DD4, 24)) {
            if (s + 24 + 96 > row.size()) break;
            const auto by = bytesAt(row, s + 24, 12);
            if (kit::sumBytes(by.data(), 12) & 0xFF) continue;
            if ((by[0] | by[1] | by[2] | by[3]) == 0 || by[8] < 160 || by[8] > 250) continue;   // 208–253 V: mains
            got.push_back(by);
        }
    for (const auto& by : got) {
        const uint32_t id = uint32_t(by[0]) << 24 | uint32_t(by[1]) << 16 | uint32_t(by[2]) << 8 | by[3];
        const double w = (((by[4] & 0x3F) << 8) | by[5]) * 0.5, amps = (by[6] << 8 | by[7]) * 0.001, volts = (by[8] + 256) * 0.5;
        const double kwh = (((by[9] & 0x3F) << 8) | by[10]) * 0.01;
        Reading r;
        r.kind = "energy"; r.make = "ESIC"; r.model = "EMT7110 energy meter";
        char ib[12]; std::snprintf(ib, sizeof ib, "%08X", id); r.pubId = ib;
        r.watts = w;
        r.extra = kit::fmt1v(volts, 1) + " V \xc2\xb7 " + kit::fmt1v(amps, 3) + " A \xc2\xb7 " + kit::fmt1v(kwh, 2) + " kWh";
        if (by[4] & 0x80) r.extra += " \xc2\xb7 pairing";
        r.confirm = std::count(got.begin(), got.end(), by) < 2;
        r.fact("current_A", amps); r.fact("voltage_V", volts); r.fact("energy_kWh", kwh);
        out.push_back(r);
        return true;
    }
    return false;
}

} // namespace emt7110

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  RFXCOM RFXMeter / RFXPower pulse counters (gas / water / electricity meter-pulse transmitters) — 433.92 MHz OOK, an
//  X10-like PPM frame: a ~9 ms mark, a ~4.5 ms gap, then 48 bits, each a 0.5 ms mark and a 0.5 ms (0) or 1.5 ms (1) gap;
//  four copies 38 ms apart. Facts: the RFXCOM RFXMeter / RFXPower user notes (the packet-type list) and rtl_433 issue
//  #2141 / rfxmeter.c notes (read for facts only). Bytes: address · the address with its high nibble complemented ·
//  counter middle · counter low · counter high · packet type (high nibble; 0 = a reading) and a check nibble making the
//  sum of all twelve nibbles ≡ 0xF. RFXPower counts 0.001 kWh; the meter's own pulses otherwise (shown as a count).
//  A ~12-bit check: believed when two copies in the burst agree, else shown once heard twice (confirm).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace rfxmeter {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    const Runs& r = b.ook;
    std::vector<std::vector<uint8_t>> got;
    for (size_t i = 0; i + 2 < r.size(); ++i) {
        if (r[i] < 7500 || r[i] > 10500 || -r[i + 1] < 3500 || -r[i + 1] > 5500) continue;
        Bits w;
        size_t k = i + 2;
        for (; k < r.size() && w.size() < 48; k += 2) {
            if (r[k] < 350 || r[k] > 750) break;
            const int g = k + 1 < r.size() ? -r[k + 1] : 100000;
            if (g >= 350 && g <= 750) w.push_back(0);
            else if (g >= 1250 && g <= 1900) w.push_back(1);
            else if (w.size() == 47) w.push_back(2);   // the last gap runs into the next copy or the end: try both
            else break;
        }
        i = k - 1;
        if (w.size() != 48) continue;
        for (int last = 0; last < 2; ++last) {
            if (w[47] != 2 && w[47] != last) continue;
            Bits v = w; v[47] = uint8_t(last);
            const auto by = kit::packBits(v);
            if ((by[0] ^ 0xF0) != by[1]) continue;
            unsigned ns = 0; for (uint8_t x : by) ns += (x >> 4) + (x & 0xF);
            if ((ns & 0xF) != 0xF) continue;
            got.push_back(by);
            break;
        }
    }
    for (const auto& by : got) {
        const int type = by[5] >> 4;
        const uint32_t value = uint32_t(by[4]) << 16 | uint32_t(by[2]) << 8 | by[3];
        Reading rd;
        rd.kind = "energy"; rd.make = "RFXCOM"; rd.model = "RFXMeter / RFXPower";
        rd.pubId = std::to_string(by[0]);
        rd.extra = type == 0 ? "count " + std::to_string(value) : type == 0xF ? "identification" : "set-up message " + std::to_string(type);
        rd.confirm = std::count(got.begin(), got.end(), by) < 2;
        rd.fact("id", (long long)by[0]); rd.fact("msg_type", (long long)type); rd.fact("msg_value", (long long)value);
        out.push_back(rd);
        return true;
    }
    return false;
}

} // namespace rfxmeter

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  REVOLT NC-5462 plug-in energy meter (Pearl) — 433.92 MHz OOK PWM: a ~10 ms sync mark, then 104 bits, a ~320 µs
//  mark = 1, ~180 µs = 0, ~160 µs gaps. Facts: rtl_433's revolt_nc5462.c notes (Nicolai Hess; read for facts only).
//  Bytes: button (bit 7) + ID 15 · volts · current 16 × 0.01 A · Hz · power 16 × 0.1 W · power factor × 0.01 · energy
//  16 × 0.01 kWh · the sum of bytes 0–10 · one more byte. An 8-bit sum: plausibility (50/60 Hz, 90–260 V) and two
//  copies, or confirm.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace revolt {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    const Runs& r = b.ook;
    std::vector<std::vector<uint8_t>> got;
    for (size_t i = 0; i + 2 < r.size(); ++i) {
        if (r[i] < 8000 || r[i] > 12000) continue;
        Bits w;
        size_t k = i + 2;
        for (; k < r.size() && w.size() < 104; k += 2) {
            const int m = r[k], g = k > 0 ? -r[k - 1] : 0;
            if (m <= 0 || g < 80 || g > 400) break;
            if (m >= 120 && m < 250) w.push_back(0); else if (m >= 250 && m <= 420) w.push_back(1); else break;
        }
        i = k - 1;
        if (w.size() != 104) continue;
        const auto by = kit::packBits(w);
        const unsigned sum = kit::sumBytes(by.data(), 11);
        if (!sum || (sum & 0xFF) != by[11]) continue;
        if ((by[5] != 50 && by[5] != 60) || by[2] < 90) continue;
        got.push_back(by);
    }
    for (const auto& by : got) {
        Reading rd;
        rd.kind = "energy"; rd.make = "Revolt"; rd.model = "NC-5462 energy meter";
        const int id = (by[0] & 0x7F) << 8 | by[1];
        rd.pubId = std::to_string(id);
        rd.watts = (by[6] << 8 | by[7]) * 0.1;
        const double amps = (by[3] << 8 | by[4]) * 0.01, kwh = (by[9] << 8 | by[10]) * 0.01;
        rd.extra = std::to_string(by[2]) + " V \xc2\xb7 " + kit::fmt1v(amps, 2) + " A \xc2\xb7 " + kit::fmt1v(kwh, 2) + " kWh";
        rd.confirm = std::count(got.begin(), got.end(), by) < 2;
        rd.fact("id", (long long)id); rd.fact("voltage_V", (long long)by[2]); rd.fact("current_A", amps); rd.fact("energy_kWh", kwh);
        rd.fact("frequency_Hz", (long long)by[5]); rd.fact("button", (long long)(by[0] >> 7));
        out.push_back(rd);
        return true;
    }
    return false;
}

} // namespace revolt

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  FLOWIS water meters — 868 MHz FSK (CC110x), 100 kbit/s (10 µs). Facts: rtl_433's flowis.c notes (Benjamin Larsson;
//  read for facts only) and the rtl_433_tests flowis README / capture log. AA AA, sync D3 91 D3 91, then LEN · type ·
//  … · CRC-16 (poly 0x8005, init 0xFFFF) over LEN … the last data byte. Type 1 (the reading): meter ID 32 LE (as
//  printed on the meter) · ? · time stamp 32 bits packed (seconds 6, minutes 6, hours 5, day 5, month 4, year 6 from
//  2000) · volume 24 LE in litres · backflow · alarm. Other types are counted by the CRC and not read.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace flowis {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    if (b.fsk.empty()) return false;
    for (const Bits& row : kit::nrzRows(b.fsk, 10.0, 64, true, 200)) {
        for (size_t s : kit::findAll(row, 0xD391D391u, 32)) {
            if (s + 40 > row.size()) break;
            const int len = int(bitsAt(row, s + 32, 8));
            if (len < 16 || s + 32 + size_t(len + 3) * 8 > row.size()) continue;
            const auto f = bytesAt(row, s + 32, len + 3);
            if (crc16(f.data(), size_t(len + 1), 0x8005, 0xFFFF) != uint16_t(f[size_t(len + 1)] << 8 | f[size_t(len + 2)])) continue;
            if (f[1] != 1) continue;
            const uint32_t id = uint32_t(f[5]) << 24 | uint32_t(f[4]) << 16 | uint32_t(f[3]) << 8 | f[2];
            const uint32_t litres = uint32_t(f[13]) << 16 | uint32_t(f[12]) << 8 | f[11];
            Reading r;
            r.kind = "energy"; r.make = "Flowis"; r.model = "water meter";
            r.pubId = std::to_string(id);
            char ts[24]; std::snprintf(ts, sizeof ts, "%04d-%02d-%02d %02d:%02d", 2000 + (f[10] >> 2), (f[9] >> 6) | (f[10] & 3) << 2,
                                       (f[9] & 0x3E) >> 1, (f[8] >> 4) | (f[9] & 1) << 4, (f[8] & 0xF) << 2 | (f[7] & 0xC0) >> 6);
            r.extra = kit::fmt1v(litres / 1000.0, 3) + " m\xc2\xb3 \xc2\xb7 meter clock " + ts;
            if (f[14]) r.extra += " \xc2\xb7 backflow";
            r.fact("id", (long long)id); r.fact("volume_m3", litres / 1000.0); r.fact("alarm", (long long)f[15]); r.fact("backflow", (long long)f[14]);
            out.push_back(r);
            return true;
        }
    }
    return false;
}

} // namespace flowis

/** The second-pass meter families, strongest checks first. (★ OWL is NOT here: decodeOne tries it after the fixed-code
 *  remotes — a remote and a neighbour's OWL CM160 in one burst (corpus status-rc202) must give the remote, the
 *  recording's own device; OWL's nibble sum is the weaker check.) */
inline bool decodeMeters2(const Burst& b, std::vector<Reading>& out) {
    if (sparsnas::decode(b, out)) return true;
    if (ec3k::decode(b, out)) return true;
    if (emt7110::decode(b, out)) return true;
    if (flowis::decode(b, out)) return true;
    if (rfxmeter::decode(b, out)) return true;
    if (revolt::decode(b, out)) return true;
    return false;
}

/** ★ Where each second-pass family transmits — a reading anywhere else passed its check by chance (bandPlausible).
 *  `false` = refuse. Families not listed are left to the general rule. */
inline bool meters2BandOk(const Reading& r, double f) {
    const bool b868 = f >= 863e6 && f <= 870e6;
    if (r.make == "IKEA") return b868;
    if (r.make == "ESIC") return b868;
    if (r.make == "RFXCOM" || r.make == "Revolt") return f >= 433.0e6 && f <= 435.0e6;
    if (r.make == "OWL") return f >= 433.0e6 && f <= 435.0e6;
    return true;
}

} // namespace vibeism
