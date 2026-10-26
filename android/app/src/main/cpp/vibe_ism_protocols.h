// vibe_ism_protocols.h — V12 ISM decoder: the PROTOCOLS. Each takes a Burst (vibe_ism.h) and appends Readings.
//
// ★★★ CLEAN ROOM — NO GPL CODE. Every decoder here was written from a published description of the protocol (a
//     datasheet, a paper, a protocol write-up), cited above the decoder and in docs/v12/ISM-DECODER.md §Protocols.
//     rtl_433 (GPL-2) and other GPL decoders were NOT read. A protocol whose only description is GPL source code is
//     not here; the doc lists it as "not done: no non-GPL description".
// ★★★ PRIVACY (Stuart, 2026-10-25). TPMS, key fobs and fixed-code remotes NEVER carry their identity out of here: the
//     decoder puts it in Reading::secretId, the Tracker (vibe_ism_service.h) turns it into a per-server nickname with a
//     salted hash kept only in memory, and clears it. No code, rolling or fixed, no raw bits, no sensor ID — on public
//     AND private servers, in any JSON. Weather stations and other sensors may show a channel and a short ID (it is how
//     an owner finds their own).
#pragma once

#include "vibe_ism.h"
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace vibeism {

namespace wmbus { struct Meter; }   // vibe_wmbus.h

/** What a decoder heard, in the units it reads in. NAN / -1 = not carried. */
struct Reading {
    std::string kind;        ///< weather | tpms | sensor | doorbell | energy | keyfob
    std::string make;        ///< "Oregon Scientific", "Fine Offset" … plain text, no logos
    std::string model;       ///< "THGR810" — or a family when the model is not known
    std::string pubId;       ///< ONLY for kinds whose ID may be shown (weather/sensor/energy): "6B" — never for secret kinds
    int channel = -1;
    bool secret = false;     ///< TPMS / key fob / fixed-code remote: the identity below never leaves the Tracker
    uint64_t secretId = 0;   ///< hashed with the server's in-memory salt, then cleared — see Tracker::accept
    bool confirm = false;    ///< a weak check: shown only once heard twice alike (Tracker)
    double tempC = NAN, hum = NAN, windKmh = NAN, gustKmh = NAN, windDir = NAN, rainMm = NAN, rainRateMmH = NAN;
    double kpa = NAN, uv = NAN, watts = NAN, baroHpa = NAN;
    int battOk = -1;         ///< 1 OK, 0 low, -1 not carried
    std::string button;      ///< a fob/remote's button, when that is not secret ("A", "1+2")
    std::string note;        ///< anything else worth a word ("learn", "pressure scale unknown")
    std::string extra;       ///< a reading in words, shown in the line ("13 bucket tips", "pairing", "new battery")
    double freqHz = 0;       ///< filled by the service: the burst's frequency
    float levelDb = -99, snrDb = 0;
    /** ★ V12 kind "meter" only (vibe_wmbus.h): what the meter's frame said — never its encrypted bytes. */
    std::shared_ptr<const wmbus::Meter> meter;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  OREGON SCIENTIFIC v2.1 and v3.0
//  Source: "Oregon Scientific RF Protocol Description (Versions 1.0, 2.1, 3.0)", June 2011 (community document,
//  wmrx00.sourceforge.net; copy at espruino.com/forumattachments/e25dc526588cdb17475ca319d441e837a76f32df.pdf).
//  OOK Manchester, 1024 Hz clock, "normal" polarity (bit = RF state before the mid-bit transition: on→off = 1). v2.1
//  sends each bit twice, inverted first; v3 does not. Preamble of 1s (v3: 24; v2.1: 16 before doubling), sync nibble
//  0101 in transmission order (= 0xA, nibbles are LSB first). Payload nibbles: 0–3 sensor ID, 4 channel, 5–6 rolling
//  code, 7 flags (0x4 = battery low), 8… data (BCD, LSD first), then an 8-bit checksum = the sum of nibbles 0..n-3
//  (the sync nibble is NOT included — checked on the document's two worked THGR122NX messages in test-ism).
//  Pulse widths are trimmed (on ~96/138 µs shorter): the document's thresholds — on: short 200–615, long 615–1100 µs;
//  off: short 400–850, long 850–1400 µs.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace oregon {

struct Sensor { const char* id; const char* model; int len; char kind; };   // kind: T th, t temp, U uv, W wind, R pcr800, r rgr968
inline const Sensor* find(const std::string& id) {
    static const Sensor k[] = {
        {"1D20", "THGR122NX / THGN123N", 17, 'T'}, {"F824", "THGR810 / THGN801", 17, 'T'}, {"F8B4", "THGR810", 17, 'T'},
        {"EC40", "THN132N / THR238NF", 14, 't'}, {"C844", "THWR800", 14, 't'},
        {"EC70", "UVR128", 14, 'U'}, {"D874", "UVN800", 14, 'U'},
        {"1984", "WGR800", 19, 'W'}, {"1994", "WGR800", 19, 'W'},
        {"2914", "PCR800", 20, 'R'}, {"2D10", "RGR968", 18, 'r'},
        {"5D60", "BTHR968", 21, 'T'},
    };
    for (const auto& s : k) if (id == s.id) return &s;
    return nullptr;
}

/** All Manchester segments of a half-bit row, both phases: a violation restarts on the other phase. */
inline std::vector<Bits> segments(const Bits& h, bool hiFirstIsOne) {
    std::vector<Bits> out;
    for (int ph = 0; ph < 2; ++ph) {
        Bits cur;
        size_t i = size_t(ph);
        while (i + 1 < h.size()) {
            if (h[i] == h[i + 1]) { if (cur.size() >= 32) out.push_back(cur); cur.clear(); i += 1; continue; }   // re-phase
            cur.push_back(uint8_t((h[i] == 1) == hiFirstIsOne));
            i += 2;
        }
        if (cur.size() >= 32) out.push_back(cur);
    }
    return out;
}

inline int bcd(int n) { return n <= 9 ? n : -1000; }

/** Parse a payload that starts at the first sensor-ID nibble. True = a reading. */
inline bool parse(const std::vector<int>& nb, bool v21, Reading& r) {
    if (nb.size() < 14) return false;
    static const char* hx = "0123456789ABCDEF";
    std::string id; for (int i = 0; i < 4; ++i) id += hx[nb[size_t(i)]];
    const Sensor* s = find(id);
    if (!s || int(nb.size()) < s->len) return false;
    int sum = 0; for (int i = 0; i < s->len - 2; ++i) sum += nb[size_t(i)];
    const int ck = nb[size_t(s->len - 2)] | (nb[size_t(s->len - 1)] << 4);
    if ((sum & 0xFF) != ck) return false;
    r.kind = "weather"; r.make = "Oregon Scientific"; r.model = s->model;
    const int chn = nb[4];
    r.channel = v21 ? (chn == 1 ? 1 : chn == 2 ? 2 : chn == 4 ? 3 : chn) : chn;   // ★ v2.1 sensors send 1 << (ch-1)
    char rc[4]; std::snprintf(rc, sizeof rc, "%X%X", nb[5], nb[6]); r.pubId = rc;
    r.battOk = (nb[7] & 0x4) ? 0 : 1;
    auto digits = [&](int lsd, int count) {                    // BCD, LSD first; < 0 = not BCD
        int v = 0, m = 1;
        for (int i = 0; i < count; ++i) { v += bcd(nb[size_t(lsd + i)]) * m; m *= 10; }
        return v;
    };
    switch (s->kind) {
        case 'T': case 't': {
            const int t = digits(8, 3);
            if (t < 0) return false;
            r.tempC = (nb[11] ? -1 : 1) * t / 10.0;
            if (s->kind == 'T') { const int h = digits(12, 2); if (h < 0 || h > 100) return false; r.hum = h; }
            break;
        }
        case 'U': { const int u = digits(8, 2); if (u < 0) return false; r.uv = u; break; }
        case 'W': {
            r.windDir = nb[8] * 22.5;
            const int g = digits(11, 3), a = digits(14, 3);
            if (g < 0 || a < 0) return false;
            r.gustKmh = g / 10.0 * 3.6; r.windKmh = a / 10.0 * 3.6;
            break;
        }
        case 'R': {   // PCR800: rate 0.01 in/h (nibbles 8..11), total 0.001 in (12..17)
            const int rate = digits(8, 4), tot = digits(12, 6);
            if (rate < 0 || tot < 0) return false;
            r.rainRateMmH = rate * 0.01 * 25.4; r.rainMm = tot * 0.001 * 25.4;
            break;
        }
        case 'r': {   // RGR968: rate 0.1 mm/h (8..10), total 0.1 mm (11..15)
            const int rate = digits(8, 3), tot = digits(11, 5);
            if (rate < 0 || tot < 0) return false;
            r.rainRateMmH = rate / 10.0; r.rainMm = tot / 10.0;
            break;
        }
    }
    if (!(r.tempC != r.tempC) && (r.tempC < -50 || r.tempC > 70)) return false;
    return true;
}

/** Find preamble (≥ minOnes 1s) + sync 0101 in `bits`, then read nibbles LSB first. */
inline bool fromBits(const Bits& bits, int minOnes, bool v21, Reading& r) {
    for (size_t i = 0; i + 8 < bits.size(); ++i) {
        size_t run = 0;
        while (i + run < bits.size() && bits[i + run]) ++run;
        if (run >= size_t(minOnes)) {
            const size_t s = i + run;   // the first 0 after the ones = the sync's first bit
            if (s + 4 <= bits.size() && bits[s] == 0 && bits[s + 1] == 1 && bits[s + 2] == 0 && bits[s + 3] == 1) {
                std::vector<int> nb;
                for (size_t p = s + 4; p + 4 <= bits.size(); p += 4) nb.push_back(int(bitsAtLsb(bits, p, 4)));
                if (parse(nb, v21, r)) return true;
            }
        }
        i += run;
    }
    return false;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    const HalfLimits L{615, 1100, 850, 1400, 200, 300};
    bool any = false;
    for (const Bits& row : halfBits(b.ook, L)) {
        if (row.size() < 80) continue;
        for (const Bits& seg : segments(row, true)) {
            Reading r;
            if (fromBits(seg, 16, false, r)) { r.note = "v3"; out.push_back(r); any = true; break; }
            // v2.1: undo the doubling — pairs (inverted, original), either alignment
            for (int k = 0; k < 2 && !any; ++k) {
                Bits orig;
                for (size_t i = size_t(k); i + 1 < seg.size(); i += 2) { if (seg[i] == seg[i + 1]) break; orig.push_back(seg[i + 1]); }
                if (orig.size() >= 40 && fromBits(orig, 8, true, r)) { r.note = "v2.1"; out.push_back(r); any = true; }
            }
            if (any) break;
        }
        if (any) break;
    }
    return any;
}

} // namespace oregon

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  FIXED-CODE REMOTES, DOORBELLS AND SENSORS — EV1527 / PT2262 style (OOK PWM)
//  Sources: EV1527 datasheet (Silvan Chip) — 24-bit frame (20-bit code C0–C19 + 4 data bits D0–D3); preamble 1 clock
//  high, 31 low; bit 0 = 1 high 3 low, bit 1 = 3 high 1 low. PT2262 datasheet (Princeton Technology) — 12 tri-state
//  bits of 2 pulse cycles of 16α (0 = 4α H 12α L twice, 1 = 12α H 4α L twice, f = 4α H 12α L then 12α H 4α L), then a
//  sync bit 4α H + 124α L; each code word sent 4 times at least. The two are the same shape on the air (EV1527's
//  clock = 4α): 24 marks of period 4T, a 31T gap — one decoder.
//  ★ PRIVACY: a fixed code IS the key of a gate, a garage, an alarm. Shown only as "Doorbell / remote · #n · button A".
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace fixedcode {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    const Runs& r = b.ook;
    std::vector<uint32_t> words;
    for (size_t i = 1; i + 48 < r.size(); ++i) {
        // a sync: a short mark then a gap 20–45× it
        if (r[i] >= 0 || r[i - 1] <= 0) continue;
        const double m = r[i - 1], g = -r[i];
        if (g < 20 * m || g > 45 * m) continue;
        // 24 (mark, space) pairs; period ≈ 4T with T ≈ the sync mark
        std::vector<int> marks, periods;
        size_t j = i + 1;
        for (int k = 0; k < 24 && j + 1 < r.size(); ++k, j += 2) {
            if (r[j] <= 0 || r[j + 1] >= 0) break;
            marks.push_back(r[j]);
            periods.push_back(r[j] - r[j + 1]);
        }
        if (marks.size() != 24) continue;
        std::vector<int> ps(periods.begin(), periods.end() - 1);   // the 24th space may run into the next sync
        std::sort(ps.begin(), ps.end());
        const double per = ps[ps.size() / 2], T = per / 4.0;
        if (T < 80 || T > 1200) continue;
        if (g < 24 * T || g > 38 * T) continue;
        bool ok = true; uint32_t w = 0;
        for (int k = 0; k < 24 && ok; ++k) {
            if (k < 23 && !near(periods[size_t(k)], per, 0.25)) ok = false;
            const bool lng = marks[size_t(k)] > 2 * T;
            if (!near(marks[size_t(k)], lng ? 3 * T : T, 0.45)) ok = false;
            w = (w << 1) | (lng ? 1u : 0u);
        }
        if (!ok) continue;
        words.push_back(w);
        i = j - 1;
    }
    // ★ the chips repeat the word: two alike in one burst, or nothing
    for (size_t a = 0; a < words.size(); ++a)
        for (size_t c = a + 1; c < words.size(); ++c)
            if (words[a] == words[c]) {
                const uint32_t w = words[a];
                Reading rd;
                rd.kind = "doorbell"; rd.make = "Fixed-code"; rd.model = "Doorbell / remote";
                rd.secret = true; rd.secretId = (uint64_t(1) << 40) | (w >> 4);   // the 20-bit code — hashed, never shown
                const unsigned d = w & 0xF;
                if (d == 8) rd.button = "A"; else if (d == 4) rd.button = "B"; else if (d == 2) rd.button = "C"; else if (d == 1) rd.button = "D";
                else { char bb[8]; std::snprintf(bb, sizeof bb, "%u", d); rd.button = bb; }
                out.push_back(rd);
                return true;
            }
    return false;
}

} // namespace fixedcode

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  KEELOQ CODE-HOPPING REMOTES (car keys, gates, garages) — Microchip HCS301 datasheet DS21143B, §4 and Table 8-4.
//  PWM, basic element TE (400/200/100 µs by BSL, 65–660 µs over tolerance): preamble 23 TE at 50 % duty, header 10 TE
//  low, 66 data bits LSb first, each 3 TE — logic 1 = 1 TE high 2 TE low, logic 0 = 2 TE high 1 TE low — guard 39 TE.
//  66 bits = 32 encrypted (button, discrimination, sync counter) + 28-bit serial + button S3 S0 S1 S2 + VLOW + repeat.
//  ★ PRIVACY: only "Key fob · KeeLoq · button 2" (the button bits travel in clear). Never the serial, never the hop code.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace keeloq {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    const Runs& r = b.ook;
    for (size_t i = 0; i + 140 < r.size(); ++i) {
        // the preamble: ≥ 8 alternating mark/space of one TE
        if (r[i] <= 0) continue;
        const double te = r[i];
        if (te < 60 || te > 700) continue;
        size_t j = i; int pulses = 0;
        while (j + 1 < r.size() && r[j] > 0 && near(r[j], te, 0.35) && r[j + 1] < 0 && near(r[j + 1], te, 0.4)) { j += 2; ++pulses; }
        if (pulses < 8 || j + 1 >= r.size() || r[j] <= 0 || !near(r[j], te, 0.35)) continue;
        // the header: the space after the last preamble mark, ~10 TE
        if (r[j + 1] >= 0 || !near(r[j + 1], 10 * te, 0.3)) { i = j; continue; }
        size_t k = j + 2;
        uint64_t bits = 0; int nb = 0; bool ok = true;
        while (nb < 66 && k < r.size() && ok) {
            const int mk = r[k];
            if (mk <= 0) { ok = false; break; }
            const bool one = mk < 1.5 * te;
            if (!near(mk, one ? te : 2 * te, 0.4)) { ok = false; break; }
            if (nb < 65) {   // the last bit's space is the guard time
                if (k + 1 >= r.size() || r[k + 1] >= 0 || !near(mk - r[k + 1], 3 * te, 0.3)) { ok = false; break; }
            }
            if (one && nb < 64) bits |= uint64_t(1) << nb;
            ++nb; k += 2;
        }
        if (!ok || nb != 66) { i = j; continue; }
        // bits: 0..31 encrypted, 32..59 serial, 60 S3, 61 S0, 62 S1, 63 S2, 64 VLOW, 65 repeat. The 66th bit (RPT) is the
        // highest: a uint64_t holds 0..63, so the last two come from the run list directly.
        const uint32_t serial = uint32_t((bits >> 32) & 0x0FFFFFFF);
        const unsigned s3 = (bits >> 60) & 1, s0 = (bits >> 61) & 1, s1 = (bits >> 62) & 1, s2 = (bits >> 63) & 1;
        const int vlowMark = r[j + 2 + 64 * 2];
        const bool vlow = vlowMark < 1.5 * te;
        Reading rd;
        rd.kind = "keyfob"; rd.make = "KeeLoq"; rd.model = "Key fob / remote";
        rd.secret = true; rd.secretId = (uint64_t(2) << 40) | serial;
        std::string bt;
        const unsigned bv[4] = {s0, s1, s2, s3};
        for (int q = 0; q < 4; ++q) if (bv[q]) { if (!bt.empty()) bt += "+"; bt += char('1' + q); }
        if (s0 && s1 && s2 && s3) { bt.clear(); rd.note = "learn"; }
        rd.button = bt;
        rd.battOk = vlow ? 0 : 1;
        out.push_back(rd);
        return true;
    }
    return false;
}

} // namespace keeloq

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  TPMS — the two formats of Krstic, Savic, Kraemer & Junghans, "Applying Tire Pressure Monitoring Devices for Traffic
//  Management Purposes" (IHP / DLR; elib.dlr.de/81155). The paper does not name the manufacturers ("Sensor A/B/C").
//   • Sensor A: FSK, differential Manchester, a preamble of 8 half-bit pulses, start of frame "10", then 10 bytes:
//     ID (4), pressure (1, scale not given), temperature (1, °C + 40), 2 status bytes, CRC-16-CCITT (2).
//   • Sensor B: OOK, 240 µs symbols; 3 LEARN bits, 1 status byte, ID (4), pressure (1, × 2.5 = kPa), temperature
//     (1, °C + 50), CRC-8-CCITT (poly 0x07) over ID..temperature (LEARN and status not included).
//   (Sensor C's checksum is a single XOR byte — too weak to tell a frame from noise without a published preamble — and
//    its pressure scale is not given: not done.)
//  ★ PRIVACY: the ID goes to secretId only. "Tyre sensor #2 · 32 PSI · 18 °C".
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace tpms {

/** The shortest-run cluster's centre — a half-bit for Manchester signals whose rate the source does not give. */
inline double halfEstimate(const Runs& r) {
    std::vector<int> a; a.reserve(r.size());
    for (int d : r) a.push_back(std::abs(d));
    if (a.size() < 20) return 0;
    std::sort(a.begin(), a.end());
    const double p20 = a[a.size() / 5];
    double s = 0; int n = 0;
    for (int d : a) if (d < 1.5 * p20) { s += d; ++n; }
    return n ? s / n : 0;
}

inline bool decodeA(const Burst& b, std::vector<Reading>& out) {
    if (b.fsk.empty()) return false;
    for (int pol = 0; pol < 2; ++pol) {
        Runs rr = b.fsk;
        if (pol) for (auto& d : rr) d = -d;
        const double h = halfEstimate(rr);
        if (h < 15 || h > 400) continue;
        for (const Bits& row : halfBits(rr, halfLimits(h))) {
            for (int ph = 0; ph < 2; ++ph) {
                const Bits bits = diffManchester(row, ph, true);
                for (int inv = 0; inv < 2; ++inv) {
                    const Bits bb = inv ? invert(bits) : bits;
                    // start of frame "10" then 80 bits with a CRC-16-CCITT over the first 8 bytes
                    for (size_t s = 0; s + 2 + 80 <= bb.size() && s < 40; ++s) {
                        if (bb[s] != 1 || bb[s + 1] != 0) continue;
                        const auto by = bytesAt(bb, s + 2, 10);
                        const uint16_t want = uint16_t(by[8] << 8 | by[9]);
                        if (crc16(by.data(), 8, 0x1021, 0xFFFF) != want && crc16(by.data(), 8, 0x1021, 0x0000) != want) continue;
                        /* ★★ A FRAME OF NOTHING IS NOT A TYRE (2026-10-25, the Pi 500 on 868: "TPMS #1 · type A · −40 °C").
                         *  With the CRC's 0x0000 start an all-zero frame checks out (the CRC of zeros is zero), and a steady
                         *  tone slices to exactly that — temperature byte 0 → −40 °C. Neither does an ID of all ones, or eight
                         *  equal bytes. And 640 offset/polarity/phase guesses per row against a 16-bit check pass noise now
                         *  and then on a band of 2 000 unidentified bursts, so a type A reading must be heard twice alike. */
                        { bool same = true; for (int i = 1; i < 8; ++i) same = same && by[size_t(i)] == by[0]; if (same) continue; }
                        if ((by[0] | by[1] | by[2] | by[3]) == 0 || (by[0] & by[1] & by[2] & by[3]) == 0xFF) continue;
                        const int t = int(by[5]) - 40;
                        if (t < -40 || t > 125) continue;
                        Reading rd;
                        rd.confirm = true;
                        rd.kind = "tpms"; rd.make = "TPMS"; rd.model = "type A";
                        rd.secret = true;
                        rd.secretId = (uint64_t(3) << 40) | (uint64_t(by[0]) << 24 | uint64_t(by[1]) << 16 | uint64_t(by[2]) << 8 | by[3]);
                        rd.tempC = t;
                        rd.note = "pressure scale unknown";
                        out.push_back(rd);
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

inline bool tryB(const Bits& bb, std::vector<Reading>& out) {
    // 3 LEARN bits, status, ID ×4, P, T, CRC — CRC over ID..T. ★ ASSUMED (the paper gives no preamble): the frame
    // follows a run-in of at least 8 equal or alternating bits — without that, a CRC-8 searched over every offset of
    // four line-code guesses finds a false match about one burst in three (test-ism caught it).
    // ★★ AND THE LATEST such start wins: a frame read one bit early (inside the run-in) can pass the CRC too — every
    //    byte halved, the carries chaining (test-ism: 88 → 44 kPa raw, 68 → 34). The real frame starts where the run-in
    //    ENDS, never inside it.
    int best = -1;
    std::vector<uint8_t> bestBy;
    for (size_t s = 8; s + 67 <= bb.size() && s < 48; ++s) {
        const uint32_t pre = bitsAt(bb, s - 8, 8);
        if (pre != 0xFF && pre != 0x00 && pre != 0xAA && pre != 0x55) continue;
        const auto by = bytesAt(bb, s + 3, 8);   // status, ID0..3, P, T, CRC
        if (crc8(by.data() + 1, 6, 0x07, 0x00) != by[7]) continue;
        const int t = int(by[6]) - 50;
        if (t < -40 || t > 125) continue;
        if (by[1] == 0 && by[2] == 0 && by[3] == 0 && by[4] == 0) continue;   // an all-zero ID is noise's favourite
        if (by[5] * 2.5 > 600) continue;                                       // 6 bar: no road tyre
        best = int(s); bestBy = by;
    }
    if (best < 0) return false;
    const auto& by = bestBy;
    Reading rd;
    rd.kind = "tpms"; rd.make = "TPMS"; rd.model = "type B";
    rd.secret = true;
    rd.secretId = (uint64_t(4) << 40) | (uint64_t(by[1]) << 24 | uint64_t(by[2]) << 16 | uint64_t(by[3]) << 8 | by[4]);
    rd.kpa = by[5] * 2.5;
    rd.tempC = int(by[6]) - 50;
    rd.confirm = true;      // ★ CRC-8 over a searched offset: shown once heard twice alike
    out.push_back(rd);
    return true;
}

inline bool decodeB(const Burst& b, std::vector<Reading>& out) {
    // 240 µs symbols. The paper does not say the line code: try Manchester (120 µs halves, both conventions) and NRZ.
    for (const Bits& row : halfBits(b.ook, halfLimits(120))) {
        for (int ph = 0; ph < 2; ++ph)
            for (int conv = 0; conv < 2; ++conv)
                if (tryB(manchester(row, ph, conv == 0), out)) return true;
    }
    for (const Bits& row : sliceNrz(b.ook, 240, 16)) if (tryB(row, out)) return true;
    return false;
}

} // namespace tpms

/** Every decoder, in turn. A burst may carry one reading; the first decoder that accepts it wins. */
inline void decodeAll(const Burst& b, std::vector<Reading>& out);

} // namespace vibeism

#include "vibe_ism_protocols2.h"   // the weather/energy families sourced in the second pass (decodeAll lives there)
