// vibe_ism_fobs.h — V12 ISM decoder, second pass (2026-11-12): CAR KEY FOBS and CAR ALARMS, GARAGE / GATE OPENERS, TV
// remotes and other REMOTES with a known frame. The first pass is vibe_ism_remotes.h (remoteReading, the same shapes).
//
// ★★★ SOURCES: frame FACTS only — from the protocol notes at the head of rtl_433's device files and conf/ descriptions
//     (GPL: read for the frame layout, timing and check, nothing copied — every line here is our own), datasheets and
//     the rtl_433_tests READMEs. Each family says where its facts came from.
// ★★★ PRIVACY (Stuart, 2026-10-25): a remote's code IS the key of a car, a gate, a garage — SECRET: Reading::secret +
//     secretId (hashed by the Tracker), never pubId. A rolling / hopping code is never decoded beyond what identifies the
//     family, and never shown; the fixed part (serial) only ever becomes the hashed secret id. Shown: make, button, battery.
// ★ Secret-id family tags here: 0xD0… (the remotes use 20–43, TPMS 0x40…, vibe_ism_security2.h 0xC0…).
#pragma once
#include "vibe_ism_remotes.h"
#include "vibe_ism_security2.h"
#include <cctype>

namespace vibeism {

/** DIFFERENTIAL MANCHESTER from run `i` on: the runs (marks and gaps alike) become half-bit chips while each is one or
 *  two halves long; then, at whichever chip phase gives the longer run of bits, every bit must start with a level change,
 *  and a bit whose two halves are equal = 1, unequal = 0. `end` = the run where the chips stopped. */
inline Bits dmcBits(const Runs& r, size_t i, double half, double tol, size_t* end = nullptr) {
    Bits c;
    for (; i < r.size(); ++i) {
        const int d = std::abs(r[i]);
        const int n = near(d, half, tol) ? 1 : near(d, 2 * half, tol) ? 2 : 0;
        if (!n) break;
        for (int k = 0; k < n; ++k) c.push_back(r[i] > 0);
    }
    if (end) *end = i;
    Bits best;
    for (size_t ph = 0; ph < 2; ++ph) {
        Bits o;
        for (size_t k = ph; k + 1 < c.size(); k += 2) {
            if (k > ph && c[k] == c[k - 1]) break;
            o.push_back(c[k] == c[k + 1]);
        }
        if (o.size() > best.size()) best = o;
    }
    return best;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  FORD car keys (433.92 MHz, the European fobs) — OOK differential Manchester, 250 µs half-bits. Facts: the notes in
//  rtl_433's fordremote.c (a short mark, a few 500 µs mark/gap pairs, a short mark, a ~3.5 ms sync gap, then ~78 bits;
//  the frame resent every ~100 ms). The frame carries NO check and its fields are not understood (its first 24 bits
//  change between presses — a rolling part), so: ★ no button and no code are shown — "Key fob #n · Ford" — and the
//  reading is confirmed only when the identical frame is heard again (the next resend, 100 ms on).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace ford {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    const Runs& r = b.ook;
    for (size_t i = 0; i + 80 < r.size(); ++i) {
        // the sync: a ~250 µs mark, a 3.0–4.2 ms gap, after ≥ 3 500/500 µs mark/gap pairs
        if (r[i] >= 0 || -r[i] < 3000 || -r[i] > 4200 || i < 7 || !near(r[i - 1], 250, 0.3)) continue;
        int pairs = 0;
        for (size_t k = i - 1; k >= 2 && near(r[k - 1], 500, 0.25) && near(r[k - 2], 500, 0.25); k -= 2) ++pairs;
        if (pairs < 3) continue;
        size_t end = 0;
        const Bits d = dmcBits(r, i + 1, 250, 0.3, &end);
        if (d.size() < 76 || d.size() > 82) continue;
        if (end < r.size() && -r[end] < 4000 && end + 1 < r.size()) continue;   // the frame must end in the packet gap
        uint64_t h = 0; for (uint8_t x : d) h = h * 3 + x;                      // ★ hashed by the Tracker, never shown
        Reading rd = remoteReading("keyfob", "Ford", "Key fob", 0xD0, h);
        rd.confirm = true;   // ★ no check in the frame: shown only once the identical frame is heard twice
        out.push_back(rd);
        return true;
    }
    return false;
}

} // namespace ford

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  CAR ALARM / REMOTE-START fobs (North America) — facts from the notes in rtl_433's code_alarm_car_remote.c,
//  astrostart_2000.c, compustar_1wg3r.c and audiovox_pro_oe3b.c (E. Halsall et al., with the FCC IDs they name):
//  · Code Alarm FRDPC2002 (433.92 OOK Manchester, 550 µs halves, an implied leading 0 half-bit): 60 bits — 16+ zero
//    preamble bits … button 4 at bit 19 · a 36-bit rolling code from bit 23. NO check: the frame resent ~90 ms on
//    confirms it. ★ The rolling code is NEVER shown; the row is keyed (hashed) by the XOR of its byte pairs, as the
//    notes derive an ID from it.
//  · Astroflex Astrostart 2000/3000 (372.4 OOK PPM: ~125 µs marks, ~280 µs gap = 0, ~480 µs = 1): 52 bits — button 8 ·
//    its complement · ID 32 · a 4-bit sum of the ID's nibbles. Fixed code.
//  · Compustar 1WG3R / 1WAMR (433.92 OOK PWM: 740 µs mark = 1, 1110 µs = 0, a 1480 µs mark between words): ID 16 · 111
//    · ¬button 8 · button 8 (· a 0). Fixed code; the word is sent several times — twice alike in the burst required.
//  · Audiovox PRO-OE3B / OE4B (303 OOK PWM: 450 µs mark = 1, 900 µs = 0): 25 bits — ID 16 · then 110b1b1b 1 (a raw 0), all
//    inverted; b = lock, unlock, option, trunk. Fixed code, almost no check: twice alike (in the burst, or by the
//    Tracker's confirm).
//  ★ PRIVACY: the fixed codes ARE the keys of these cars — make + button only; the ID only ever as the hashed secret.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace caralarm {

inline Reading carFob(const char* make, uint64_t fam, uint64_t id, int code, const std::string& word) {
    Reading r = remoteReading("keyfob", make, "Key fob", fam, id);
    r.button = word;
    r.fact("button_code", (long long)code);
    return r;
}

inline bool codeAlarm(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& c0 : kit::nrzRows(b.ook, 550, 4, false, 100)) {
        if (c0.empty() || !c0[0]) continue;
        Bits c(1, 0); c.insert(c.end(), c0.begin(), c0.end());   // the implied first half-bit
        size_t end = 0;
        const Bits d = kit::manchesterAt(c, 0, 61, false, &end);   // the bit = its first half: 01 = 0
        if (d.size() != 60 || end + 1 < c.size() - 1) continue;    // exactly one frame, gap to gap
        if (bitsAt(d, 0, 16) != 0) continue;
        const auto by = bytesAt(d, 19, 5);
        const unsigned sum = kit::sumBytes(by.data(), 5);
        if (sum == 0 || sum >= 0xFFu * 5) continue;
        const auto cd = bytesAt(d, 23, 4);
        const uint32_t id = uint32_t(cd[0] ^ cd[1]) << 16 | uint32_t(cd[1] ^ cd[2]) << 8 | uint32_t(cd[2] ^ cd[3]);
        const int bt = by[0] >> 4;
        std::string w;
        if (bt == 7) w = "several";
        else { if (bt == 6 || bt == 4) addWord(w, "lock"); if (bt == 1 || bt == 3) addWord(w, "panic"); if (bt == 0 || bt == 3) addWord(w, "start");
               if (bt == 5 || bt == 4) addWord(w, "unlock"); }
        if (w.empty()) continue;
        Reading r = carFob("Code Alarm", 0xD1, id, bt, w);
        r.confirm = true;   // ★ no check: shown once the identical frame is heard again
        out.push_back(r);
        return true;
    }
    return false;
}

inline bool astrostart(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& w : kit::ppmRows(b.ook, 280, 480, 1000, 60, 220, 0.25)) {
        if (w.size() != 52) continue;
        const auto by = bytesAt(w, 0, 7);
        if (by[0] != uint8_t(~by[1])) continue;
        int ck = 0; for (int i = 2; i < 6; ++i) ck = (ck + (by[size_t(i)] >> 4) + by[size_t(i)]) & 0xF;
        if (ck != by[6] >> 4) continue;
        const uint32_t id = uint32_t(by[2]) << 24 | uint32_t(by[3]) << 16 | uint32_t(by[4]) << 8 | by[5];
        if (id == 0 || id == 0xFFFFFFFF) continue;
        const int bt = by[0];
        Reading r = carFob("Astroflex", 0xD2, id, bt, "code " + std::to_string(bt));
        r.model = "Astrostart remote starter";
        r.confirm = true;   // a 12-bit check on a fixed code: the press sends three — shown from the second
        out.push_back(r);
        return true;
    }
    return false;
}

/** Whole PWM words (a run of marks, each `oneUs` or `zeroUs` ± `tol`), ended by a gap over `gapUs` or a mark of neither
 *  width; every word kept. */
inline std::vector<Bits> pwmWords(const Runs& r, double oneUs, double zeroUs, double tol, double gapUs) {
    std::vector<Bits> rows(1);
    for (int d : r) {
        if (d > 0) {
            if (near(d, oneUs, tol)) rows.back().push_back(1);
            else if (near(d, zeroUs, tol)) rows.back().push_back(0);
            else if (!rows.back().empty()) rows.emplace_back();
        } else if (-d > gapUs && !rows.back().empty()) rows.emplace_back();
    }
    if (rows.back().empty()) rows.pop_back();
    return rows;
}

inline bool compustar(const Burst& b, std::vector<Reading>& out) {
    const auto rows = pwmWords(b.ook, 740, 1110, 0.18, 1700);
    for (const Bits& w0 : rows) {
        if ((w0.size() != 35 && w0.size() != 36) || !kit::twice(rows, w0)) continue;
        Bits w = w0; w.resize(40, 0);
        if (bitsAt(w, 16, 3) != 7 || w[35]) continue;
        const uint32_t id = bitsAt(w, 0, 16);
        if (id == 0 || id == 0xFFFF) continue;
        const uint32_t inv = bitsAt(w, 19, 8), bt = bitsAt(w, 27, 8);
        if ((~inv & 0xFF) != bt) continue;
        out.push_back(carFob("Compustar", 0xD3, id, int(bt), "code " + std::to_string(bt)));
        return true;
    }
    return false;
}

inline bool audiovox(const Burst& b, std::vector<Reading>& out) {
    const auto rows = pwmWords(b.ook, 450, 900, 0.25, 1790);
    for (const Bits& w : rows) {
        if (w.size() != 25) continue;
        const auto raw = bytesAt(w, 0, 4);
        if ((raw[2] & 0xAA) || raw[2] == 0x55 || w[24]) continue;
        const uint16_t id = uint16_t(~(raw[0] << 8 | raw[1]));
        if (id == 0 || id == 0xFFFF) continue;
        const uint8_t bt = uint8_t(~raw[2]);
        std::string s;
        static const char* const names[4] = {"lock", "unlock", "option", "trunk"};
        for (int i = 0; i < 4; ++i) if (bt & (1 << (2 * i))) addWord(s, names[i]);
        if (s.empty()) continue;
        Reading r = carFob("Audiovox", 0xD4, id, bt, s);
        r.facts.clear();
        std::string key; for (int i = 0; i < 4; ++i) if (bt & (1 << (2 * i))) { if (!key.empty()) key += "; "; key += char(std::toupper(names[i][0])); key += names[i] + 1; }
        r.fact("button_str", key.c_str());
        r.confirm = !kit::twice(rows, w);   // ★ one whole word in this burst (a weak signal breaks the rest): shown once heard again
        out.push_back(r);
        return true;
    }
    return false;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    return astrostart(b, out) || compustar(b, out) || audiovox(b, out) || codeAlarm(b, out);
}

} // namespace caralarm

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  HONDA car keys (433.92 MHz FSK, the older European fobs) — facts from the notes in rtl_433's hondaremote.c: FSK, the
//  tone held 250 µs = 1, 500 µs = 0 (PWM on the "mark" tone); 385–394 bits; byte 0 = FF, byte 38 = FF, byte 46 = AA
//  (boot) / AB (unlock) / AC (lock); bytes 44–45 a device number; a rolling code otherwise, not decoded. NO check: 26
//  fixed bits, the exact length and a repeat (confirm). ★ Only "Key fob · Honda · lock" — the device number only
//  ever hashed.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace honda {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    if (b.fsk.empty()) return false;
    Runs inv0 = b.fsk; for (auto& d : inv0) d = -d;
    const Runs& inv = inv0;
    for (const Runs* r : {&b.fsk, &inv})
        for (const Bits& w : caralarm::pwmWords(*r, 250, 500, 0.25, 2000)) {
            if (w.size() < 385 || w.size() > 394) continue;
            const auto by = bytesAt(w, 0, 48);
            if (by[0] != 0xFF || by[38] != 0xFF || by[46] < 0xAA || by[46] > 0xAC) continue;
            const int cmd = by[46] - 0xAA;
            Reading rd = remoteReading("keyfob", "Honda", "Key fob", 0xD5, uint64_t(by[44]) << 8 | by[45]);
            rd.button = cmd == 0 ? "boot" : cmd == 1 ? "unlock" : "lock";
            rd.fact("code", rd.button.c_str());
            rd.confirm = true;   // ★ no check: shown once heard twice alike
            out.push_back(rd);
            return true;
        }
    return false;
}

} // namespace honda

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  SECURITY+ 2.0 garage / gate remotes (Chamberlain, LiftMaster, Craftsman; 310 / 315 / 390 MHz) — OOK Manchester,
//  250 µs halves. Facts: the notes in rtl_433's secplus_v2.c: raw chips AA AA 95 6 (28), Manchester from the 27th; a
//  packet = 2 bits (00 / 01: which half) · 2 zero bits · the 8-bit order·invert byte · 30 data bits — a Security+ 2.0
//  half-word (vibe_ism_security2.h); the two halves' fixed parts make 40 bits: button 8 · remote ID 32. A press sends
//  both halves, repeated.
//  ★★ PRIVACY: a rolling-code garage key — only "Key fob #n · Security+ 2.0 · button 16"; the remote ID only hashed;
//     the rolling counter is never assembled. A burst holding only one half shows nothing (half an ID is not a sender).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace secplusv2 {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    int32_t half[2] = {-1, -1};
    for (const Bits& c : kit::nrzRows(b.ook, 250, 6, false, 100)) {
        for (size_t s : kit::findAll(c, 0xAAAA956, 28)) {
            for (int conv = 0; conv < 2; ++conv) {
                Bits d = kit::manchesterAt(c, s + 26, 42, conv == 0);
                if (d.size() < 42) continue;
                if (d[0] || d[2] || d[3]) continue;
                const int32_t f = secplus2::fixedOfHalf(uint8_t(bitsAt(d, 4, 8)), bitsAt(d, 12, 30));
                if (f < 0) continue;
                half[d[1]] = f;
                break;
            }
        }
    }
    if (half[0] < 0 || half[1] < 0) return false;
    const uint64_t fixed = uint64_t(half[0]) << 20 | uint64_t(half[1]);
    const int bt = int(fixed >> 32);
    const uint32_t id = uint32_t(fixed);
    if (id == 0) return false;
    Reading r = remoteReading("keyfob", "Security+ 2.0", "garage remote", 0xD6, id);
    r.button = std::to_string(bt);
    r.fact("button_id", (long long)bt);
    out.push_back(r);
    return true;
}

} // namespace secplusv2

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  CHAMBERLAIN / LIFTMASTER DIP-switch garage remotes (7, 8 or 9 switches; 300–390 MHz) — OOK in 1 ms slots. Facts: the
//  notes in rtl_433's chamberlain_code.c (from the Flipper Zero protocol): a word follows a ~36 ms gap; each symbol is 4
//  slots, low then high — 0 = 1 low 3 high, 1 = 2 low 2 high, stop = 3 low 1 high; the first stop's high slot opens the
//  word. After it: 9 switches = D×9 S · 8 = D×5 S D D S · 7 = D×6 S S D S. No check beyond the symbol and stop places:
//  the word must come twice alike (it is resent every ~80 ms).
//  ★★ PRIVACY: the switches ARE the key of the garage (512 codes at most) — never shown; "Key fob #n · Chamberlain
//     · garage remote", the code only hashed.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace chamberlaindip {

/** Each word: the symbols after the opening slot, 0 / 1 / 2 (= stop); empty when the runs are not such a word. */
inline std::vector<std::vector<int>> words(const Runs& r) {
    std::vector<std::vector<int>> out;
    for (size_t i = 0; i + 1 < r.size(); ++i) {
        if (r[i] <= 0 || !near(r[i], 1000, 0.25) || (i > 0 && -r[i - 1] < 20000)) continue;
        std::vector<int> w;
        size_t k = i + 1;
        while (k + 1 < r.size() && r[k] < 0 && r[k + 1] > 0) {
            const int lo = -r[k], hi = r[k + 1];
            if (!near(lo + hi, 4000, 0.12)) break;
            const int s = near(lo, 1000, 0.3) ? 0 : near(lo, 2000, 0.2) ? 1 : near(lo, 3000, 0.15) ? 2 : -1;
            if (s < 0) break;
            w.push_back(s); k += 2;
        }
        if (k < r.size() && r[k] < 0 && -r[k] < 20000) continue;   // the word must end in the long gap
        if (w.size() == 9 || w.size() == 10) out.push_back(w);
        i = k;
    }
    return out;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    const auto ws = words(b.ook);
    for (size_t a = 0; a < ws.size(); ++a) {
        const auto& w = ws[a];
        auto at = [&](std::initializer_list<int> stops, size_t n) {
            if (w.size() != n) return false;
            for (size_t k = 0; k < n; ++k) {
                const bool stop = std::find(stops.begin(), stops.end(), int(k)) != stops.end();
                if ((w[k] == 2) != stop) return false;
            }
            return true;
        };
        int nsw = at({9}, 10) ? 9 : at({5, 8}, 9) ? 8 : at({6, 7, 9}, 10) ? 7 : 0;
        if (!nsw) continue;
        bool twice = false;
        for (size_t c = 0; c < ws.size(); ++c) if (c != a && ws[c] == w) twice = true;
        uint64_t code = uint64_t(nsw) << 16;
        for (int s : w) code = code * 3 + uint64_t(s);
        Reading r = remoteReading("keyfob", "Chamberlain", "DIP-switch garage remote", 0xD7, code);
        r.fact("bits", (long long)nsw);
        r.confirm = !twice;   // ★ no check: twice alike — in this burst, or by the Tracker
        out.push_back(r);
        return true;
    }
    return false;
}

} // namespace chamberlaindip

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  MICROCHIP HCS362 KeeLoq encoders (gate / garage / car remotes; 315 / 433 / 868 MHz) — Microchip datasheet DS40189E
//  and the notes in rtl_433's hcs362.c: a preamble of 12 TE pulses (TE ≈ 200 µs), then EITHER (PWM mode) a ~3 TE gap and
//  69 PWM bits of 3 TE (a 1 TE mark = 1, 2 TE = 0), OR (Manchester mode) a ~4 TE gap, a start bit 1, 69 Manchester bits
//  (the first half-bit high = 1) and a stop. The 69 bits, LSB first: 32 encrypted (the hop code) · 28 serial · buttons
//  S3 S0 S1 S2 · VLOW · a 2-bit CRC over the first 65 (datasheet Eq. 3-1, as the HCS361's) · 2 queue bits.
//  ★★ PRIVACY: only "Key fob · Microchip HCS362 · button 3" — the serial only hashed, the hop code never read. The 2-bit
//     CRC is weak: confirm.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace hcs362 {

inline bool crcOk(const Bits& d) {
    int crc = 0;   // CRC0 << 1 | CRC1
    for (int j = 0; j < 65; ++j) { const int c = ((crc >> 1) ^ d[size_t(j)]) & 1; crc = c | (((c ^ crc) << 1) & 2); }
    return (d[65] << 1 | d[66]) == crc;
}

inline bool emit(const Bits& d, std::vector<Reading>& out) {
    if (d.size() < 69 || !crcOk(d)) return false;
    uint32_t serial = 0; for (int j = 0; j < 28; ++j) serial |= uint32_t(d[size_t(32 + j)]) << j;
    if (serial == 0 || serial == 0x0FFFFFFF) return false;
    const int s3 = d[60], s0 = d[61], s1 = d[62], s2 = d[63];
    const int bt = s0 | s1 << 1 | s2 << 2 | s3 << 3;
    if (bt == 0) return false;
    Reading r = remoteReading("keyfob", "Microchip HCS362", "Key fob / remote", 0xD8, serial);
    r.button = std::to_string(bt);
    r.battOk = d[64] ? 0 : 1;
    r.fact("button", (long long)bt);
    r.confirm = true;   // ★ a 2-bit CRC: shown once heard twice alike
    out.push_back(r);
    return true;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    const Runs& r = b.ook;
    for (size_t i = 0; i + 30 < r.size(); ++i) {
        // 12 preamble pulses of one TE
        if (r[i] <= 0) continue;
        const double te = r[i];
        if (te < 120 || te > 480) continue;
        size_t j = i; int n = 0; double sum = 0;
        while (j + 1 < r.size() && r[j] > 0 && near(r[j], te, 0.3) && r[j + 1] < 0 && near(r[j + 1], te, 0.35)) { sum += r[j] - r[j + 1]; j += 2; ++n; }
        if (n < 10 || n > 12 || j + 1 >= r.size() || r[j] <= 0 || !near(r[j], te, 0.3)) { if (n) i = j; continue; }
        const double T = (sum + r[j]) / (2 * n + 1);
        const double g = -r[j + 1];
        if (near(int(g), 3 * T, 0.2)) {                                         // PWM mode
            Bits d; size_t k = j + 2;
            while (d.size() < 69 && k < r.size() && r[k] > 0) {
                const bool one = r[k] < 1.5 * T;
                if (!near(r[k], one ? T : 2 * T, 0.35)) break;
                if (d.size() < 68 && (k + 1 >= r.size() || r[k + 1] >= 0 || !near(r[k] - r[k + 1], 3 * T, 0.25))) break;
                d.push_back(one); k += 2;
            }
            if (emit(d, out)) return true;
        } else if (near(int(g), 4 * T, 0.2)) {                                  // Manchester mode
            Bits c; size_t k = j + 2;
            for (; k < r.size() && c.size() < 2 * 72; ++k) {
                const int m = near(r[k], T, 0.3) ? 1 : near(r[k], 2 * T, 0.25) ? 2 : 0;
                if (!m) { if (r[k] > 2 * T) c.push_back(1); break; }   // the stop: a long high — its first chip is the last bit's
                for (int q = 0; q < m; ++q) c.push_back(r[k] > 0);
            }
            if (c.size() >= 2 + 2 * 69 && c[0] == 1 && c[1] == 0) {
                const Bits d = kit::manchesterAt(c, 2, 69, false);
                if (emit(d, out)) return true;
            }
        }
        i = j;
    }
    return false;
}

} // namespace hcs362

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  2GIG KEY2E-345 encrypted key fobs (345 MHz) — the notes in rtl_433's twogig_key2e.c (issue #2584): the Honeywell /
//  2GIG raw preamble 55 55 56 and OOK Manchester (~136 µs halves), then 72 bits: 32 encrypted · 0x25 · 16 encrypted ·
//  CRC-16 poly 0x8005, init 0x4C57, over the first 7 bytes. Everything but the constant is enciphered ("eSeries
//  Encrypted") — no ID and no button can be read. ★ So only "Key fob #n · 2GIG KEY2E (encrypted)"; the row is keyed by
//  a hash of the enciphered word (a new row per press, as the 6SC2's).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace twogigkey {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& row : kit::nrzRowsAny(b, 136, 12, 96)) {
        for (size_t s : kit::findAll(row, 0x555556, 24)) {
            const Bits d = kit::manchesterAt(row, s + 24, 72);
            if (d.size() < 72) continue;
            const auto by = bytesAt(d, 0, 9);
            if (crc16(by.data(), 7, 0x8005, 0x4C57) != uint16_t(by[7] << 8 | by[8])) continue;
            const uint32_t enc = uint32_t(by[0]) << 24 | uint32_t(by[1]) << 16 | uint32_t(by[2]) << 8 | by[3];
            Reading r = remoteReading("keyfob", "2GIG", "KEY2E (encrypted)", 0xD9, enc);
            r.note = "encrypted";
            out.push_back(r);
            return true;
        }
    }
    return false;
}

} // namespace twogigkey

/** "ZERO-BIT" MANCHESTER frames (a frame's first half-bit is the idle level, so the slice starts at its second): every
 *  NRZ row of `r` at `half` µs, both polarities, given back that first half-bit and read with a bit = its FIRST half;
 *  each decoded frame of at least `minBits`. */
inline std::vector<Bits> zerobitFrames(const Runs& r, double half, size_t minBits, int maxChips = 3) {
    std::vector<Bits> out;
    for (const Bits& c0 : kit::nrzRows(r, half, maxChips, true, 2 * minBits - 1)) {
        if (c0.empty() || !c0[0]) continue;
        Bits c(1, 0); c.insert(c.end(), c0.begin(), c0.end());
        Bits d = kit::manchesterAt(c, 0, 100000, false);
        if (d.size() >= minBits) out.push_back(std::move(d));
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  ALPS FWB1U545 car keys (Honda OEM, 433.92 MHz) — facts from the notes in rtl_433's alps_fwb1u545.c: FSK "zero-bit"
//  Manchester, 500 µs halves; exactly 76 bits: 55 · ID 32 · button byte · its copy · sequence 16 · 4 unknown; button
//  (high nibble) E lock, C panic, 5 panic held, 1 unlock. A fixed ID — ★ only hashed; the sequence is never shown.
//  The check is weak (the 55, the copied byte, the exact length): confirm.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace alps {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    if (b.fsk.empty()) return false;
    for (const Bits& d : zerobitFrames(b.fsk, 500, 76)) {
        if (d.size() != 76) continue;
        const auto by = bytesAt(d, 0, 10);
        if (by[0] != 0x55 || by[5] != by[6]) continue;
        const uint32_t id = uint32_t(by[1]) << 24 | uint32_t(by[2]) << 16 | uint32_t(by[3]) << 8 | by[4];
        if (id == 0 || id == 0xFFFFFFFF) continue;
        const int bt = by[5] >> 4;
        const char* w = bt == 0xE ? "lock" : bt == 0xC ? "panic" : bt == 0x5 ? "panic (held)" : bt == 0x1 ? "unlock" : nullptr;
        if (!w) continue;
        Reading r = caralarm::carFob("Alps", 0xDA, id, bt, w);
        r.confirm = true;
        out.push_back(r);
        return true;
    }
    return false;
}

} // namespace alps

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  TV / SATELLITE BOX RF REMOTES — kind "doorbell" (the list's "Doorbell / remote"), the remote's address secret as for
//  every remote.
//  · DirecTV RC66RX (433.92 FSK, 600 µs symbols) — facts from K. Lohner's notes in rtl_433's directv.c: a row = a sync
//    (3 low, then 10 high — the first time — or 5 high — a repeat —, then 2 low) and a message read by the symbol count
//    between transitions (1 = 0, 2 = 1): 40 bits — 0x10 · device ID 20 · button 8 · a nibble = the low nibble of the sum
//    of the nine before it.
//  · Dish Network UHF remote 6.3 (384 MHz OOK PPM) — facts from the notes in rtl_433's dish_remote_6_3.c: 400 µs marks, a
//    1692 µs gap = 0, 2812 µs = 1; a 400 µs start mark and a ~6 ms gap, then 16 bits BBBBBB10 101x1xxx (B = the button);
//    seven copies per press — only six fixed bits, so the word must come at least twice alike in the burst.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace tvremote {

inline bool directv(const Burst& b, std::vector<Reading>& out) {
    if (b.fsk.empty()) return false;
    Runs inv0 = b.fsk; for (auto& d : inv0) d = -d;
    const Runs& inv = inv0;
    for (const Runs* rp : {&b.fsk, &inv}) {
        const Runs& r = *rp;
        std::vector<int> n(r.size());
        for (size_t i = 0; i < r.size(); ++i) n[i] = int(std::lround(std::abs(r[i]) / 600.0));
        for (size_t i = 1; i + 2 < r.size(); ++i) {
            if (r[i] <= 0 || (n[i] != 10 && n[i] != 5) || n[i - 1] != 3 || n[i + 1] != 2) continue;
            if (!near(r[i], n[i] * 600.0, 0.12)) continue;
            Bits d; size_t k = i + 2;
            for (; k < r.size() && d.size() < 42; ++k) {
                if (n[k] == 1 && near(r[k], 600, 0.3)) d.push_back(0);
                else if (n[k] == 2 && near(r[k], 1200, 0.2)) d.push_back(1);
                else break;
            }
            if (d.size() != 40 && d.size() != 41) continue;   // (41: the row's last symbol, before the next sync's 3 low)
            const auto by = bytesAt(d, 0, 5);
            if (by[0] != 0x10) continue;
            int sum = 0; for (int q = 0; q < 9; ++q) sum += (by[size_t(q / 2)] >> (q & 1 ? 0 : 4)) & 0xF;
            if ((sum & 0xF) != (by[4] & 0xF)) continue;
            const uint32_t id = uint32_t(by[1]) << 12 | uint32_t(by[2]) << 4 | uint32_t(by[3] >> 4);
            const int bt = (by[3] & 0xF) << 4 | by[4] >> 4;
            Reading rd = remoteReading("doorbell", "DirecTV", "RC66RX TV remote", 0xDB, id);
            rd.button = std::to_string(bt);
            rd.fact("button_id", (long long)bt);
            out.push_back(rd);
            return true;
        }
    }
    return false;
}

inline bool dish(const Burst& b, std::vector<Reading>& out) {
    const auto rows = kit::ppmRows(b.ook, 1692, 2812, 4500, 250, 600, 0.2);
    for (const Bits& w : rows) {
        if (w.size() != 16 || !kit::twice(rows, w)) continue;
        if (w[6] != 1 || w[7] != 0 || w[8] != 1 || w[9] != 0 || w[10] != 1 || w[12] != 1) continue;
        const uint32_t v = bitsAt(w, 0, 16);
        const int bt = int(v >> 10);
        Reading rd = remoteReading("doorbell", "Dish Network", "UHF remote 6.3", 0xDC, v & 0x3FF);
        rd.button = std::to_string(bt);
        out.push_back(rd);
        return true;
    }
    return false;
}

} // namespace tvremote

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  ALARM-SYSTEM REMOTES (arm / disarm / panic / ring) and a KeeLoq gate remote — facts from the notes in rtl_433's
//  gangqi.c, hollarm.c and kinggates.c (each from the Flipper Zero protocol of the same name):
//  · GangQi (433.92 OOK PWM: a 1200 µs mark = 1, 500 µs = 0; 34 bits, resent after 2.2 ms): serial 16 · 0xD · button 4
//    · a checksum = 0xC8 − the three bytes before (or 0x02 + them) · 00.
//  · Hollarm (433.92 OOK PPM: 200 µs marks, a 1000 µs gap = 0, 1600 µs = 1; 42 bits, the last one lost in the row's
//    ending gap, so 41): serial 24 · 4 · button 4 · the sum of the 4 bytes before · 00.
//  · King Gates Stylo 4k (433.92: 12 × 400/400 µs, a 2.2 ms gap, an 800 µs start mark, then 89 bits, each a gap and a
//    mark: 1 = 400 µs gap + 1100 µs mark, 0 = 1100 + 400), LSB first: serial low 16 · 1 · button 4 · serial high 16 ·
//    16 zeros · a 32-bit KeeLoq hop code · 4 zeros. NO check: the constant bits and two identical messages.
//  ★★ PRIVACY: the serials are the keys of the alarm / the gate — only hashed; the hop code is never read.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace alarmremote {

inline bool gangqi(const Burst& b, std::vector<Reading>& out) {
    const auto rows = caralarm::pwmWords(b.ook, 1200, 500, 0.3, 2000);
    for (const Bits& w : rows) {
        if (w.size() != 34) continue;
        const auto by = bytesAt(w, 0, 5);
        if ((by[2] >> 4) != 0xD || w[32] || w[33]) continue;
        const unsigned s3 = unsigned(by[0]) + by[1] + by[2];
        if (by[3] != uint8_t(0xC8 - s3) && by[3] != uint8_t(0x02 + s3)) continue;
        const uint32_t id = uint32_t(by[0]) << 8 | by[1];
        if (id == 0 || id == 0xFFFF) continue;
        const int bt = by[2] & 0xF;
        const char* wd = bt == 0xD ? "arm" : bt == 0xE ? "disarm" : bt == 0xB ? "alarm" : bt == 0x7 ? "ring" : nullptr;
        Reading r = remoteReading("keyfob", "GangQi", "alarm remote", 0xDD, id);
        r.button = wd ? wd : std::to_string(bt);
        r.fact("button", (long long)bt);
        r.confirm = !kit::twice(rows, w);   // an 8-bit sum: twice alike (in the burst, or by the Tracker)
        out.push_back(r);
        return true;
    }
    return false;
}

inline bool hollarm(const Burst& b, std::vector<Reading>& out) {
    const auto rows = kit::ppmRows(b.ook, 1000, 1600, 2000, 120, 320, 0.2);
    for (const Bits& w : rows) {
        if (w.size() != 41) continue;
        const auto by = bytesAt(w, 0, 6);
        if (w[40]) continue;
        if (uint8_t(by[0] + by[1] + by[2] + by[3]) != by[4]) continue;
        const uint32_t id = uint32_t(by[0]) << 16 | uint32_t(by[1]) << 8 | by[2];
        if (id == 0 || id == 0xFFFFFF) continue;
        const int bt = by[3] & 0xF;
        const char* wd = bt == 0x2 ? "arm" : bt == 0x1 ? "disarm" : bt == 0x4 ? "alarm" : bt == 0x8 ? "ring" : nullptr;
        Reading r = remoteReading("keyfob", "Hollarm", "alarm remote", 0xDE, id);
        r.button = wd ? wd : std::to_string(bt);
        r.fact("button", (long long)bt);
        r.confirm = !kit::twice(rows, w);
        out.push_back(r);
        return true;
    }
    return false;
}

inline bool kinggates(const Burst& b, std::vector<Reading>& out) {
    const Runs& r = b.ook;
    std::vector<Bits> msgs;
    for (size_t i = 1; i + 2 * 89 < r.size(); ++i) {
        if (r[i] <= 0 || !near(r[i], 800, 0.2) || !near(r[i - 1], 2200, 0.2)) continue;
        Bits d; size_t k = i + 1;
        for (; d.size() < 89 && k + 1 < r.size(); k += 2) {
            const int g = -r[k], m = r[k + 1];
            if (r[k] >= 0 || m <= 0) break;
            if (near(g, 400, 0.35) && near(m, 1100, 0.2)) d.push_back(1);
            else if (near(g, 1100, 0.2) && near(m, 400, 0.35)) d.push_back(0);
            else break;
        }
        if (d.size() == 89) msgs.push_back(d);
        i = k;
    }
    for (size_t a = 0; a < msgs.size(); ++a) {
        const Bits& d = msgs[a];
        if (!d[16]) continue;
        bool zero = true;
        for (int j = 37; j < 53; ++j) if (d[size_t(j)]) zero = false;
        for (int j = 85; j < 89; ++j) if (d[size_t(j)]) zero = false;
        if (!zero || std::count(msgs.begin(), msgs.end(), d) < 2) continue;   // ★ no check: two identical messages
        uint32_t lo = 0, hi = 0; int bt = 0;
        for (int j = 0; j < 16; ++j) lo |= uint32_t(d[size_t(j)]) << j;
        for (int j = 0; j < 4; ++j) bt |= d[size_t(17 + j)] << j;
        for (int j = 0; j < 16; ++j) hi |= uint32_t(d[size_t(21 + j)]) << j;
        const uint32_t serial = hi << 16 | lo;
        if (serial == 0 || serial == 0xFFFFFFFF) continue;
        Reading rd = remoteReading("keyfob", "King Gates", "Stylo 4k gate remote", 0xDF, serial);
        rd.button = std::to_string(bt);
        rd.fact("button", (long long)bt);
        out.push_back(rd);
        return true;
    }
    return false;
}

} // namespace alarmremote

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  iDo 111 / 117 gate remotes (433.92 OOK PPM) — facts from the notes in rtl_433's ido.c (from the Flipper Zero
//  protocol): a 4.5 ms mark and 4.5 ms gap, then 48 bits on ~450 µs marks, a 450 µs gap = 1, 1450 µs = 0; LSB first:
//  serial 20 · button 4 · a 24-bit hop code (new in every message; ~20 messages a press). NO check: two messages with
//  the same fixed part. ★ The hop code is never read; the serial only hashed.
//  HT680-based remotes (433.92 OOK PWM: ~260 µs mark = 1, ~660 µs = 0) — facts from rtl_433's ht680.c notes: 41 bits,
//  a 10101 sync, then 18 tri-state address/data pins in pairs (00 = 0, 11 = 1, 10 = open) with the HT680's fixed open
//  pins (A4, A5, AD10, AD13, AD16, AD17); buttons = the pairs at D1–D4. Twice alike (burst or Tracker).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace gateremote {

inline bool ido(const Burst& b, std::vector<Reading>& out) {
    const Runs& r = b.ook;
    std::vector<uint32_t> fix;
    for (size_t i = 0; i + 2 + 96 <= r.size(); ++i) {
        if (r[i] <= 0 || !near(r[i], 4500, 0.15) || !near(r[i + 1], 4500, 0.15)) continue;
        uint32_t f = 0; int n = 0; size_t k = i + 2;
        for (; n < 48 && k + 1 < r.size(); k += 2, ++n) {
            if (!near(r[k], 450, 0.35) || r[k + 1] >= 0) break;
            const int g = -r[k + 1];
            const int bit = near(g, 450, 0.35) ? 1 : near(g, 1450, 0.2) ? 0 : (n == 47 && g > 3000) ? 2 : -1;
            if (bit < 0) break;
            if (n < 24 && bit == 1) f |= uint32_t(1) << n;
        }
        if (n == 48) fix.push_back(f);
        i = k - 1;
    }
    for (size_t a = 0; a < fix.size(); ++a) {
        if (std::count(fix.begin(), fix.end(), fix[a]) < 2) continue;
        const uint32_t serial = fix[a] & 0xFFFFF;
        const int bt = int(fix[a] >> 20);
        if (serial == 0 || serial == 0xFFFFF) continue;
        Reading rd = remoteReading("keyfob", "iDo", "gate remote", 0xE0, serial);
        rd.button = std::to_string(bt);
        rd.fact("button", (long long)bt);
        out.push_back(rd);
        return true;
    }
    return false;
}

inline bool ht680(const Burst& b, std::vector<Reading>& out) {
    const auto rows = caralarm::pwmWords(b.ook, 260, 660, 0.35, 1200);
    for (const Bits& w : rows) {
        if (w.size() != 41 || bitsAt(w, 0, 5) != 0x15) continue;
        const auto by = bytesAt(w, 5, 5);
        if ((by[1] & 0xF0) != 0xA0 || (by[2] & 0x0C) != 0x08 || (by[3] & 0x30) != 0x20 || (by[4] & 0xF0) != 0xA0) continue;
        const uint32_t addr = uint32_t(by[0]) << 12 | uint32_t(by[1]) << 4 | uint32_t(by[2] >> 4);
        const int pins[4] = {by[3] & 3, (by[3] >> 2) & 3, (by[3] >> 6) & 3, by[2] & 3};
        std::string bt;
        for (int q = 0; q < 4; ++q) if (pins[q] == 3) { if (!bt.empty()) bt += "+"; bt += char('1' + q); }
        Reading rd = remoteReading("doorbell", "Generic", "HT680 remote", 0xE1, addr);
        rd.button = bt.empty() ? "none" : bt;
        for (int q = 0; q < 4; ++q) { const std::string k = "button" + std::to_string(q + 1); rd.facts.emplace_back(k, pins[q] == 3 ? "\"PRESSED\"" : "\"\""); }
        rd.confirm = !kit::twice(rows, w);   // 17 fixed bits: twice alike (in the burst, or by the Tracker)
        out.push_back(rd);
        return true;
    }
    return false;
}

} // namespace gateremote

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  REVERS RB2 remotes (433.92 OOK "zero-bit" Manchester, 250 µs halves) — facts from rtl_433's conf/Revers-RB2.conf and
//  its test notes: after a long preamble, the 32-bit word (ID 22 · 10 more) and 32 one-bits, again and again (seven times
//  in the corpus's frames). No check: the word must repeat at least three times, each followed by its 32 ones.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace revers {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& d0 : zerobitFrames(b.ook, 250, 160, 4)) {
        for (int inv = 0; inv < 2; ++inv) {
            const Bits d = inv ? invert(d0) : d0;
            for (size_t s = 0; s + 64 * 3 <= d.size(); ++s) {
                const uint32_t w = bitsAt(d, s, 32);
                if (w == 0xFFFFFFFF || w == 0 || (s && d[s - 1] == 0)) continue;
                int n = 0;
                for (size_t k = s; k + 64 <= d.size() && bitsAt(d, k, 32) == w && bitsAt(d, k + 32, 32) == 0xFFFFFFFF; k += 64) ++n;
                if (n < 3) continue;
                Reading r = remoteReading("doorbell", "Revers", "RB2 remote", 0xE3, w >> 10);
                out.push_back(r);
                return true;
            }
        }
    }
    return false;
}

} // namespace revers

/** The second-pass remote / fob families, strongest checks first. */
inline bool decodeFobs(const Burst& b, std::vector<Reading>& out) {
    if (twogigkey::decode(b, out)) return true;
    if (secplusv2::decode(b, out)) return true;
    if (hcs362::decode(b, out)) return true;
    if (caralarm::decode(b, out)) return true;
    if (ford::decode(b, out)) return true;
    if (chamberlaindip::decode(b, out)) return true;
    if (honda::decode(b, out)) return true;
    if (alps::decode(b, out)) return true;
    if (tvremote::directv(b, out)) return true;
    if (alarmremote::gangqi(b, out)) return true;
    if (alarmremote::hollarm(b, out)) return true;
    if (alarmremote::kinggates(b, out)) return true;
    if (gateremote::ido(b, out)) return true;
    if (gateremote::ht680(b, out)) return true;
    if (revers::decode(b, out)) return true;
    if (tvremote::dish(b, out)) return true;
    return false;
}

} // namespace vibeism
