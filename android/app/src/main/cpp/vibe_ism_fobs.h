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

/** The second-pass remote / fob families, strongest checks first. */
inline bool decodeFobs(const Burst& b, std::vector<Reading>& out) {
    if (secplusv2::decode(b, out)) return true;
    if (caralarm::decode(b, out)) return true;
    if (ford::decode(b, out)) return true;
    if (honda::decode(b, out)) return true;
    return false;
}

} // namespace vibeism
