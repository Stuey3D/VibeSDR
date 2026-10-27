// vibe_vfo_dc.h — KEEP THE DC SPIKE OFF THE LISTENING VFO WHEN THE VFO IS FREE (V12, 2026-10-27, branch v12-dc-nudge).
//
// ★★★ Stuart, 2026-10-27: "we know where a user is tuning and we know where the rf centre is so as the user approaches the
//     DC spike in Free Tune mode just nudge the RF centre so the spike moves away from the listen VFO".
//
// A zero-IF radio (an RTL dongle, its rtl_tcp cousin, a HackRF, an RSP) carries a spike at its capture's PHYSICAL centre,
// rtlCenter + hwOffsetHz(). With the VFO Centre LOCKED the VFO is the centre, so the spike always sits hwOffsetHz() away
// (15 kHz on a dongle — chosen to clear AM, SSB and NFM). With the VFO Centre FREE the centre stays put while the VFO roams
// the capture, and tuning to centre + 15 kHz puts the listener ON the spike (Gesiima, GitHub #57). This is the rule that
// moves the centre out of the way. Pure, so test-vfo-dc checks it; the shim's vfoDcPlace() decides when it applies.
#pragma once

#include <algorithm>
#include <cmath>

namespace vibe {

/** ★★ THE GUARD BEYOND THE PASSBAND: 2.5 kHz, whatever the sample rate. The spike is a LINE (LO leakage plus the ADC's DC
 *  offset), so its width is set by the tuner and not by the capture: Stuart's V4 draws it as one FFT bin (the shim's bins
 *  are ≤ 75 Hz at every rate — fftSizeForRate), and the dirtiest R820T we model (scripts/test-server-band-dc.mjs, the
 *  stress case) has a ±1.5 kHz skirt of drifting DC round it. 2.5 kHz clears that skirt with the demodulator's own filter
 *  transition (~1 kHz) on top — and it is small enough that the move it buys stays a few kHz on HF. */
constexpr double kVfoDcGuardHz = 2500.0;
/** ★★ THE HYSTERESIS: the DC is placed this far BEYOND the edge of the zone it was cleared from, so a VFO hovering at the
 *  edge (a slow drag, a fine-tuning knob going back and forth by 10 Hz) has to travel this much further before it can ask
 *  for another retune. 5 kHz or half the zone, whichever is more — a few SSB channels, a third of a WFM one. */
constexpr double kVfoDcHoldMinHz = 5000.0;

/** Where the DC must NOT be, relative to the dial: the passband (`pbLo`..`pbHi`, Hz from the dial — USB 0..+bw, LSB −bw..0,
 *  CW and the rest ±bw/2) widened by the guard.
 *  ★★★ NEVER STRICTER THAN THE LOCKED VFO. Where the radio's own offset (`dcOffHz`) would itself sit inside that zone —
 *  broadcast FM on a dongle: 200 kHz wide, 15 kHz offset — the zone shrinks to ±dcOffHz. That is exactly how far the
 *  LOCKED VFO keeps the spike on every tune, so a FREE VFO is held to the same standard and no further: otherwise a WFM
 *  listener roaming the band would be followed by a retune for every station, for a spike the locked dial has always
 *  lived with. */
struct VfoDcZone { double lo = 0, hi = 0; };   // absolute Hz; empty when lo >= hi
inline VfoDcZone vfoDcZone(double dialHz, double pbLoHz, double pbHiHz, double dcOffHz) {
    VfoDcZone z{ dialHz + pbLoHz - kVfoDcGuardHz, dialHz + pbHiHz + kVfoDcGuardHz };
    if (dcOffHz > 0.0 && dialHz + dcOffHz > z.lo && dialHz + dcOffHz < z.hi) { z.lo = dialHz - dcOffHz; z.hi = dialHz + dcOffHz; }
    return z;
}

struct VfoDcPlace { bool moved = false; double centre = 0; };
/** The logical centre to use instead of `cur`, so the DC (`cur + dcOffHz`) is outside the listening VFO's `zone`.
 *  · DC already outside the zone → `cur`, untouched: no retune, no PLL relock (by far the commonest case).
 *  · Inside → the DC goes `hold` beyond one edge of the zone: BEHIND THE VFO'S TRAVEL when it is moving (`prevDialHz` →
 *    `dialHz`), so the next step of the same sweep walks AWAY from it — one retune per crossing. Pushing the spike ahead
 *    of a VFO that is still coming is the "minimum move" trap dongleForView warns about: every few steps another PLL
 *    relock, a relock storm. With no travel to go by (a pan, a jump), the edge nearer the DC — the smallest move.
 *  · `lo`..`hi`: where the centre MAY go (the VFO — and a personal receiver's VFO B — still captured); a candidate outside
 *    is never taken. `softLo`..`softHi`: where it should go (the whole view inside the capture); preferred, not required —
 *    a sliver of "no data" at the edge of a full-span view is a far smaller harm than the listener sitting on the spike.
 *  · `avoid` (VFO B's zone, absolute; empty for none): a candidate that would put the DC on B is not taken either.
 *  · Neither side allowed → `cur` (nothing better is legal; the caller's own limits win). */
inline VfoDcPlace vfoDcCentre(double cur, double dcOffHz, double dialHz, double prevDialHz, VfoDcZone zone,
                              double lo, double hi, double softLo, double softHi, VfoDcZone avoid = {}) {
    VfoDcPlace r; r.centre = cur;
    if (!(zone.lo < zone.hi)) return r;
    const double dc = cur + dcOffHz;
    if (dc <= zone.lo || dc >= zone.hi) return r;
    const double hold = std::max(kVfoDcHoldMinHz, 0.5 * (zone.hi - zone.lo));
    const double cand[2] = { zone.lo - hold - dcOffHz, zone.hi + hold - dcOffHz };   // [0]: DC below the VFO, [1]: above
    auto legal = [&](double c) {
        if (c < lo || c > hi) return false;
        const double d = c + dcOffHz;
        return !(avoid.lo < avoid.hi && d > avoid.lo && d < avoid.hi);
    };
    auto soft = [&](double c) { return c >= softLo && c <= softHi; };
    // ★ the preferred side: behind the travel, else the nearer
    int pref;
    if (dialHz > prevDialHz + 1.0) pref = 0;          // moving up: leave the spike below
    else if (dialHz < prevDialHz - 1.0) pref = 1;     // moving down: leave it above
    else pref = std::fabs(cand[0] - cur) <= std::fabs(cand[1] - cur) ? 0 : 1;
    const int order[2] = { pref, 1 - pref };
    for (int pass = 0; pass < 2; ++pass)               // ★ pass 0: inside the view's room too; pass 1: the hard limits alone
        for (int k : order) {
            const double c = cand[k];
            if (!legal(c) || (pass == 0 && !soft(c))) continue;
            r.moved = true; r.centre = c;
            return r;
        }
    return r;
}

} // namespace vibe
