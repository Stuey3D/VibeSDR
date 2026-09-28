// vibe_airspy_limit.h — the owner's per-band gain LIMIT for an Airspy R2 / Mini, as pure policy.
//
// ★★★ STUART'S SPEC, 2026-09-28: "if a per band limit is in place tie it to one of the gain curves
//     and limit that and then block any manual gain with the individual gain controls. That seems
//     the safest option when dealing with gain limits. As long as it doesnt effect non limited
//     radios and they get all the controls. So I could say go FM Broadcast band Sensitive gain
//     slider and limit to 10/20 ... so a dropdown to choose the gain mode and the slider after that
//     to then limit or lock."
//
// ★★ WHAT A RULE IS. Three per-band lists the server already has, plus one new one:
//      gainLimits  "fm:100"  — the position, 0-21, carried x10 (the slider's tenths contract;
//                              AirspySource::gainListTenthDb is 0, 10 … 210)
//      gainLocks   "fm:1"    — LOCK: the position is the SETTING, not a ceiling
//      gainCurves  "fm:1"    — which preset curve: 0 = Linearity, 1 = Sensitivity (the `curve`
//                              wire field's own meaning). NEW; absent = Linearity, see limitFrom.
//    A band with no gainLimits entry has no rule, whatever the other lists say.
//
// ★★ WHAT A RULE DOES, in a limited band:
//      - the radio is held on the band's CURVE (Free mode, the other curve, the three manual
//        stages and the LNA / mixer AGCs are all refused — none of them is a position on that
//        curve, so none of them can be bound by one);
//      - LIMIT: a requested position above the figure is clamped to it;
//      - LOCK:  the position is the owner's and every gain request is refused.
//    Outside a limited band nothing here applies and every control is exactly as it always was.
//
// ★ Pure and header-only so it can be tested without a radio (vibeserver/test-airspy-limit.cpp):
//   the shim asks these functions and does the hardware writes itself.
#pragma once
#include <string>

namespace vibe {
namespace asplimit {

constexpr int kMaxPos = 21;   ///< positions 0..21 on either curve

/** AirspySource::GainMode values, restated so this header needs nothing else. */
constexpr int kModeSensitive = 0;
constexpr int kModeLinear    = 1;
constexpr int kModeFree      = 2;

struct Limit {
    bool active      = false;   ///< a rule covers this frequency
    bool sensitivity = false;   ///< the band's curve: false = Linearity, true = Sensitivity
    int  maxPos      = kMaxPos; ///< 0..21 — the ceiling (LIMIT) or the setting (LOCK)
    bool locked      = false;
    /** The AirspySource::GainMode that IS the band's curve. */
    int  mode() const { return sensitivity ? kModeSensitive : kModeLinear; }
    const char* curveName() const { return sensitivity ? "Sensitivity" : "Linearity"; }
};

/**
 * Build the rule in force from the three per-band answers.
 * @param capTenths LocalSdrShim::gainCapAt — -1 = no rule here
 * @param curve     LocalSdrShim::gainCurveAt — 0 Linearity, 1 Sensitivity, -1 not written
 * @param locked    LocalSdrShim::gainLockedAt
 * ★★ A RULE WITH NO CURVE IS HELD ON LINEARITY. Such a rule was written before the curve existed
 *    (7c740045 stored the position alone) or by hand. Linearity is Airspy's own curve for strong
 *    signals and this radio's default mode, so it is the conservative reading of "a ceiling on
 *    this band" — the owner is protecting the front end, and the linearity curve takes less LNA
 *    gain at every position.
 */
inline Limit limitFrom(int capTenths, int curve, bool locked) {
    Limit l;
    if (capTenths < 0) return l;
    l.active      = true;
    l.sensitivity = curve == 1;
    int pos = (capTenths + 5) / 10;
    l.maxPos      = pos < 0 ? 0 : (pos > kMaxPos ? kMaxPos : pos);
    l.locked      = locked;
    return l;
}

/** The position a listener's request lands on. LIMIT clamps; LOCK is always the owner's figure.
 *  Positions, not tenths. */
inline int clampPos(const Limit& l, int wantPos) {
    if (wantPos < 0) wantPos = 0;
    if (wantPos > kMaxPos) wantPos = kMaxPos;
    if (!l.active) return wantPos;
    if (l.locked) return l.maxPos;
    return wantPos > l.maxPos ? l.maxPos : wantPos;
}

/** The fields of an `airspy_control` message that touch the gain. */
enum class Field { Mode, Curve, Stage, StageAgc, Auto, Position };

/**
 * Whether a field is REFUSED in this band.
 * ★ Mode / Curve naming the band's own curve are accepted in a LIMIT band (a harmless no-op that
 *   an old client sends on connect) and refused in a LOCK band only if they would change anything —
 *   so re-stating the curve is never an error, choosing another one always is.
 * ★ A Position is never refused in a LIMIT band (it is clamped — see clampPos); in a LOCK band it
 *   is refused, because the owner's figure is the setting.
 */
inline bool refuses(const Limit& l, Field f, int value = 0) {
    if (!l.active) return false;
    switch (f) {
        case Field::Mode:     return value != l.mode();
        case Field::Curve:    return (value != 0) != l.sensitivity;
        case Field::Position: return l.locked;
        case Field::Stage:
        case Field::StageAgc:
        case Field::Auto:     return true;
    }
    return true;
}

struct Target {
    bool change = false;   ///< the radio is not where the rule puts it
    int  mode   = kModeLinear;
    int  pos    = 0;
};

/**
 * Where the radio must be in this band, from where it is now.
 * @param curMode     AirspySource::gainMode()
 * @param curvePos    the position the radio REMEMBERS on the band's curve (presetTenth / 10), or -1
 *                    if nobody has ever set one there
 * ★ LOCK: exactly the owner's position. LIMIT: the radio's own remembered position on that curve
 *   if it is inside the ceiling, the ceiling if it is not (or if there is none remembered — the
 *   owner's figure is the only position anyone has chosen for this curve). Nothing is raised
 *   except to reach a remembered-less curve, where there is no lower figure to prefer.
 */
inline Target target(const Limit& l, int curMode, int curvePos) {
    Target t;
    if (!l.active) return t;
    t.mode = l.mode();
    if (l.locked)          t.pos = l.maxPos;
    else if (curvePos < 0) t.pos = l.maxPos;
    else                   t.pos = curvePos > l.maxPos ? l.maxPos : curvePos;
    t.change = curMode != t.mode || curvePos != t.pos;
    return t;
}

/** ★ The refusal a listener is shown — one sentence, in the words their own panel uses. */
inline std::string why(const Limit& l) {
    const std::string pos = std::to_string(l.maxPos);
    if (l.locked)
        return std::string("the server owner has locked the gain on this band at ")
             + l.curveName() + " " + pos;
    return std::string("the server owner limits the gain on this band to ") + l.curveName()
         + ", up to " + pos + " of 21 \u2014 Free mode and the manual stages are off here";
}

}  // namespace asplimit
}  // namespace vibe
