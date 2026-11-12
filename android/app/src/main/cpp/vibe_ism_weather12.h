// vibe_ism_weather12.h — V12 ISM decoder, weather pass 2 (2026-11-12): La Crosse Technology View (LTV) sensors.
// Sources: La Crosse's product pages (LTV-TH2 / TH3, WR1, R1 / R3, W1 / W2, WSDTH01 Breeze Pro: ranges, update
// intervals), HopeRF's CMT2119A datasheet (the FSK transmitter they use), and the protocol notes at the head of
// rtl_433's lacrosse_th3.c, lacrosse_r1.c, lacrosse_wr1.c, lacrosse_breezepro.c (M. Bruski) — FACTS only (layout, bit
// time, sync, check, scaling); our own code, on our own slicers (vibe_ism_wxkit.h). Scored on the rtl_433_tests
// recordings (scripts/ism-corpus.mjs).
#pragma once
#include "vibe_ism_wxkit.h"

namespace vibeism {
namespace wx12 {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  LA CROSSE VIEW (LTV) — 915 MHz (TFA View 30.3802: 868) FSK NRZ, 104 µs bits (TH2 107, Breeze Pro 106.8); preamble of
//  0xAA, sync D2 AA 2D D4, then: ID 24 · flags 4 (bit 7 battery low, bit 6 start-up / re-send) with a 3-bit sequence in
//  bits 3..1 · the payload · CRC-8 (poly 0x31) over everything after the sync, so the CRC run over the check byte too
//  leaves 0. The payload and the CRC's start by model:
//   TH2 / TH3 / TH2i   temperature 12 ((raw − 400) / 10 °C) · humidity 12 · CRC over 7 bytes, init 0x00, 0xAC or 0xB2
//   R1 rain            rain 24, stored HH^0xAA · MM · LL — (HH^0xAA) << 16 | MM << 8 | LL counts × 0.25 mm · CRC (init 0)
//                      over 7 bytes, a second check byte, then zeros
//   R3 / TFA 30.3802   rain 24 twice (the second: a second counter) · CRC over 10 bytes
//   W1 / W2 wind       AA AA AA · wind 12 (× 0.1 km/h) · AAA · CRC over 10 bytes
//   WR1 wind + rain    wind 12 (× 0.1 km/h) · direction 12 (°) · rain counters 12 + 12 (their scale unpublished) · CRC
//                      over 10 bytes; the transmission ends there
//   Breeze Pro         temperature 12 · humidity 12 · wind 12 (× 0.1 km/h) · direction 12 · CRC over 10 bytes, then a
//                      D2 D2 D2 trailer
//  The 10-byte models share one CRC: told apart by their constant bytes (W1's AAs, R3's 0xAA-whitened high bytes),
//  then by the Breeze Pro's trailer.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool ltvFrame(const Bits& row, size_t s, std::vector<Reading>& out) {
    const size_t avail = row.size() > s ? row.size() - s : 0;
    // ★ a frame's last bits may be lost: a 0 run into the zero trailer is longer than a row allows, so the row ends
    //   early and bytesAt reads the missing bits as the 0s they were
    if (avail < 57) return false;
    const auto b = bytesAt(row, s, 14);
    const std::string id = wx::hex(unsigned(b[0] << 16 | b[1] << 8 | b[2]), 6);
    const int batLow = b[3] >> 7;
    auto base = [&](const char* model) {
        Reading r = wx::wxReading("La Crosse", model, id);
        r.battOk = batLow ? 0 : 1;
        if (b[3] & 0x40) r.note = "start-up";
        return r;
    };
    const bool crc8ok = crc8(b.data(), 8, 0x31, 0x00) == 0;
    // ── the 10-byte frames
    if (avail >= 81 && crc8(b.data(), 11, 0x31, 0x00) == 0) {
        if (b[4] == 0xAA && b[5] == 0xAA && b[6] == 0xAA && (b[8] & 0x0F) == 0x0A && b[9] == 0xAA) {
            Reading r = base("View LTV-W1 wind");
            r.windKmh = (b[7] << 4 | b[8] >> 4) * 0.1;
            out.push_back(r);
            return true;
        }
        if (((b[5] ^ 0xAA) & 0xFC) == 0 && ((b[8] ^ 0xAA) & 0xFC) == 0) {
            Reading r = base("View LTV-R3 rain");
            r.rainMm = (((b[5] ^ 0xAA) << 16) | b[4] << 8 | b[6]) * 0.25;
            r.fact("rain_mm", r.rainMm);
            r.fact("id", (long long)(b[0] << 16 | b[1] << 8 | b[2]));
            out.push_back(r);
            return true;
        }
        const bool trailer = avail >= 88 + 16 && (findBits(row, 0xD2D2, 16, s + 88) != size_t(-1)) &&
                             findBits(row, 0xD2D2, 16, s + 88) <= s + 88 + 12;
        const int t12 = b[4] << 4 | b[5] >> 4, h12 = (b[5] & 0xF) << 8 | b[6];
        const int w12 = b[7] << 4 | b[8] >> 4, d12 = (b[8] & 0xF) << 8 | b[9];
        if (trailer) {
            Reading r = base("View LTV-WSDTH01 Breeze Pro");
            r.battOk = -1;
            r.tempC = (t12 - 400) / 10.0;
            if (h12 > 100 || d12 > 360 || !wx::tempOk(r.tempC)) return false;
            r.hum = h12;
            r.windKmh = w12 * 0.1;
            r.windDir = d12;
            out.push_back(r);
            return true;
        }
        if (avail <= 88 + 24) {
            const int wind = b[4] << 4 | b[5] >> 4, dir = (b[5] & 0xF) << 8 | b[6];
            if (dir > 360 || wind > 2000) return false;
            Reading r = base("View LTV-WR1 wind / rain");
            r.battOk = -1;
            r.windKmh = wind * 0.1;
            r.windDir = dir;
            r.fact("rain1", (long long)(b[7] << 4 | b[8] >> 4));
            r.fact("rain2", (long long)((b[8] & 0xF) << 8 | b[9]));
            out.push_back(r);
            return true;
        }
        // a 10-byte frame of none of these shapes: fall through to the 7-byte ones (a short frame + chance zero CRC)
    }
    // ── the 7-byte frames
    if (((b[5] ^ 0xAA) & 0xFC) == 0 && crc8ok && b[10] == 0) {
        Reading r = base("View LTV-R1 rain");
        r.rainMm = (((b[5] ^ 0xAA) << 16) | b[4] << 8 | b[6]) * 0.25;
        r.fact("rain_mm", r.rainMm);
        r.fact("id", (long long)(b[0] << 16 | b[1] << 8 | b[2]));
        out.push_back(r);
        return true;
    }
    if (crc8ok || crc8(b.data(), 8, 0x31, 0xAC) == 0 || crc8(b.data(), 8, 0x31, 0xB2) == 0) {
        const int t12 = b[4] << 4 | b[5] >> 4, h12 = (b[5] & 0xF) << 8 | b[6];
        Reading r = base(crc8ok ? "View LTV-TH3 / TH2" : "View LTV-TH2");
        r.tempC = (t12 - 400) / 10.0;
        if (h12 > 100 || !wx::tempOk(r.tempC, -50, 70)) return false;
        r.hum = h12;
        out.push_back(r);
        return true;
    }
    return false;
}

inline bool ltv(const Burst& b, std::vector<Reading>& out) {
    if (b.fsk.empty() && b.ook.size() < 40) return false;
    for (double bit : {104.0, 107.0}) {
        for (const Bits& row : wx::fskRows(b, bit, 40, 96)) {
            for (size_t s : wx::findAll(row, 0xD2AA2DD4u, 32)) {
                if (ltvFrame(row, s + 32, out)) return true;
            }
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  BRESSER SMARTHOME GARDEN (Fujian Baldr / HomGar / RainPoint) — soil moisture sensor, water timer, gateway. Facts:
//  FCC 2AWDBHWS388WRF / 2AWDBTCS005FRF and the notes at the head of rtl_433's bresser_garden.c (B. Octau, M. Jonsson).
//  433 MHz FSK NRZ, 50 µs bits; preamble of AA, sync F3 E9 10 5E 51, then a fixed 33-byte frame: target ID 32 (LE) ·
//  source ID 32 (LE; its top byte is the class — 0x47 soil sensor, 0x1F water timer, 0x01 gateway) · counter · message
//  type (bit 7 = a reply) · payload length · payload from byte 11, zero-padded · CRC-16 (poly 0x1021, init 0xD636) over
//  bytes 0–30, big-endian at 31–32. The soil reading (type 0x09 sensor → timer, 0x0A relayed by the timer with its
//  RSSI of the sensor inserted, 0x03 direct to the gateway): battery (bit 4 low, low nibble level) · 0x88 · moisture % ·
//  0x85 · temperature 16 (LE, signed, °F × 10). The rest is control traffic, named by its type.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline const char* gardenMsg(int t) {
    switch (t) {
    case 0x01: return "init / pairing";   case 0x02: return "status";          case 0x03: case 0x09: return "soil reading";
    case 0x04: return "watering";         case 0x05: return "config request";  case 0x06: return "schedule request";
    case 0x08: return "moisture request"; case 0x0A: return "relayed soil reading"; case 0x20: return "config change";
    case 0x21: return "run / keep-alive"; case 0x81: return "pairing reply";   case 0x82: return "status reply";
    case 0x85: return "schedule config";  case 0x86: return "schedule";        case 0x88: return "moisture reply";
    case 0xA0: return "config ack";       case 0xA1: return "run reply / beacon";
    default: return (t & 0x80) ? "acknowledgement" : "message";
    }
}
inline bool garden(const Burst& b, std::vector<Reading>& out) {
    if (b.fsk.empty() && b.ook.size() < 100) return false;
    // ★ a gateway's reply can start while the timer's frame is still on the air (another channel, the same burst):
    //   every distinct frame with a good CRC is read, not only the first
    std::vector<std::vector<uint8_t>> seen;
    const size_t before = out.size();
    for (const Bits& row : wx::fskRows(b, 50, 200, 200)) {
        for (size_t s : wx::findAll(row, 0xF3E9105Eu, 32)) {
            if (s + 40 + 33 * 8 > row.size() + 8) continue;
            if (bytesAt(row, s + 32, 1)[0] != 0x51) continue;
            const auto f = bytesAt(row, s + 40, 33);
            uint16_t c = 0xD636;
            for (int i = 0; i < 31; ++i) { c ^= uint16_t(f[size_t(i)] << 8); for (int k = 0; k < 8; ++k) c = uint16_t(c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1); }
            if (c != uint16_t(f[31] << 8 | f[32])) continue;
            if (std::find(seen.begin(), seen.end(), f) != seen.end() || seen.size() >= 4) continue;
            seen.push_back(f);
            const uint32_t src = uint32_t(f[7]) << 24 | f[6] << 16 | f[5] << 8 | f[4];
            const int cls = int(src >> 24), type = f[9], len = f[10];
            Reading r;
            r.kind = "sensor"; r.make = "Bresser";
            r.model = cls == 0x47 ? "Garden soil sensor" : cls == 0x1F ? "Garden water timer" : cls == 0x01 ? "Garden gateway" : "Garden";
            r.pubId = wx::hex(src & 0xFFFF, 4);
            r.fact("id", (long long)src);
            r.fact("msg_counter", (long long)f[8]);
            r.fact("msg_type", (long long)type);
            const int at = type == 0x03 ? 12 : 14;      // the battery byte of the soil block
            if ((type == 0x03 && len == 7) || ((type == 0x09 || type == 0x0A) && len == 9)) {
                if (f[size_t(at + 1)] != 0x88 || f[size_t(at + 3)] != 0x85) continue;
                r.battOk = (f[size_t(at)] & 0x10) ? 0 : 1;
                const int moist = f[size_t(at + 2)];
                r.tempC = (int16_t(f[size_t(at + 5)] << 8 | f[size_t(at + 4)]) / 10.0 - 32) / 1.8;
                if (moist > 100 || !wx::tempOk(r.tempC, -40, 80)) continue;
                char e[32]; std::snprintf(e, sizeof e, "soil moisture %d %%", moist); r.extra = e;
                r.fact("moisture", (long long)moist);
            } else if (type == 0x04 && len >= 14) {
                char e[48]; std::snprintf(e, sizeof e, "watering %d s", f[23] | f[24] << 8); r.extra = e;
            } else r.note = gardenMsg(type);
            out.push_back(r);
        }
    }
    return out.size() > before;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    if (ltv(b, out)) return true;
    if (garden(b, out)) return true;
    return false;
}

} // namespace wx12
} // namespace vibeism
