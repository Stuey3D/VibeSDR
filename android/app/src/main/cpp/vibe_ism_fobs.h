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

/** The second-pass remote / fob families, strongest checks first. */
inline bool decodeFobs(const Burst& b, std::vector<Reading>& out) {
    if (ford::decode(b, out)) return true;
    return false;
}

} // namespace vibeism
