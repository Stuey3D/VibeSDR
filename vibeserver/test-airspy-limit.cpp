// ★★★ THE AIRSPY R2 / MINI's PER-BAND LIMIT, TESTED WITHOUT A RADIO.
//
// Stuart's spec (2026-09-28): a limited band is tied to ONE gain curve, the position on it is
// limited (or locked), and every other way of setting the gain — Free mode, the other curve, the
// three manual stages, the stage AGCs, "auto" — is blocked there. Unlimited bands keep everything.
//
// The shim does the hardware writes; every DECISION it makes comes from vibe_airspy_limit.h, fed
// by the same band parser the server uses (vibe_bands.h). So this drives the config strings an
// owner writes through that parser and asks the policy what it would do — which is everything
// short of a device. No R2 / Mini exists on the build Mac; what this cannot see is libairspy.
#include "vibe_airspy_limit.h"
#include "vibe_bands.h"
#include <cstdio>
#include <string>

static int failures = 0, checks = 0;
static void ok(bool cond, const char* what, const std::string& extra = "") {
    checks++;
    if (cond) { std::printf("   ok   %s\n", what); return; }
    failures++;
    std::printf("   FAIL %s %s\n", what, extra.c_str());
}

using namespace vibe::asplimit;

/** The shim's three readers, over the owner's strings — the same calls, minus the mutex and the
 *  "only while serving" gate (which is a bool in front of each). */
struct Cfg {
    vibebands::GainRules limits, locks, curves;
    Cfg(const std::string& l, const std::string& k, const std::string& c)
        : limits(vibebands::parseGainList(l)), locks(vibebands::parseGainList(k)),
          curves(vibebands::parseGainList(c)) {}
    Limit at(double hz) const {
        const int cap = limits.empty() ? -1 : vibebands::gainCapAt(limits, hz);
        const bool lk = !locks.empty() && vibebands::valueAt(locks, hz) > 0;
        int cv = curves.empty() ? -1 : vibebands::valueAt(curves, hz);
        cv = cv < 0 ? -1 : (cv > 0 ? 1 : 0);
        return limitFrom(cap, cv, lk);
    }
};

int main() {
    const double FM = 98.5e6, AIR = 125e6, UHF = 446e6;

    std::printf("\nStuart's example: FM broadcast, Sensitivity, limited to 10\n");
    {
        Cfg c("fm:100", "", "fm:1");
        const Limit l = c.at(FM);
        ok(l.active, "FM is limited");
        ok(l.sensitivity, "★ on the SENSITIVITY curve, as chosen");
        ok(l.maxPos == 10, "at position 10", std::to_string(l.maxPos));
        ok(!l.locked, "as a LIMIT, not a lock");
        ok(l.mode() == kModeSensitive, "its mode is AirspySource::GainSensitive");
        ok(clampPos(l, 15) == 10, "★ a request for 15 is clamped to 10");
        ok(clampPos(l, 7) == 7, "a request for 7 is left at 7 — a limit, not a setting");
        ok(!refuses(l, Field::Position), "a position is clamped, never refused, on a LIMIT");
        ok(refuses(l, Field::Mode, kModeFree), "★★ FREE MODE is refused");
        ok(refuses(l, Field::Mode, kModeLinear), "★★ the OTHER curve (as a mode) is refused");
        ok(!refuses(l, Field::Mode, kModeSensitive), "naming the band's own curve is accepted");
        ok(refuses(l, Field::Curve, 0), "the other curve (legacy `curve` field) is refused");
        ok(!refuses(l, Field::Curve, 1), "the band's own curve (legacy field) is accepted");
        ok(refuses(l, Field::Stage), "★★ the manual LNA / mixer / VGA are refused");
        ok(refuses(l, Field::StageAgc), "★★ the LNA and mixer AGCs are refused");
        ok(refuses(l, Field::Auto), "★★ 'auto' (Free with both AGCs) is refused");
        ok(why(l).find("Sensitivity, up to 10 of 21") != std::string::npos,
           "the notice names the curve and the figure", why(l));
    }

    std::printf("\nThe same band LOCKED\n");
    {
        Cfg c("fm:100", "fm:1", "fm:1");
        const Limit l = c.at(FM);
        ok(l.locked, "FM is locked");
        ok(refuses(l, Field::Position), "★ every position is refused on a LOCK");
        ok(clampPos(l, 3) == 10, "and the band's position is the owner's, from any request");
        ok(why(l) == "the server owner has locked the gain on this band at Sensitivity 10",
           "the notice", why(l));
    }

    std::printf("\nUnlimited bands, and radios with no rules, keep EVERY control\n");
    {
        Cfg c("fm:100", "fm:1", "fm:1");
        const Limit l = c.at(AIR);
        ok(!l.active, "airband has no rule");
        ok(!refuses(l, Field::Mode, kModeFree), "★★ Free mode allowed off the limited band");
        ok(!refuses(l, Field::Stage) && !refuses(l, Field::StageAgc) && !refuses(l, Field::Auto),
           "★★ stages, AGCs and auto all allowed");
        ok(clampPos(l, 21) == 21, "the whole 0-21 range");
        ok(!target(l, kModeFree, -1).change, "★ and nothing is moved on the radio");
        Cfg none("", "", "");
        ok(!none.at(FM).active, "a radio with no rules at all has none anywhere");
        // ★ A curve with NO position is not a rule — the server binds only where gainLimits has it.
        Cfg orphan("", "", "fm:1");
        ok(!orphan.at(FM).active, "★ a curve alone, with no position, limits nothing");
        // ★ A lock with no position is the same: gainLockedAt only bites where a cap exists.
        Cfg lockOnly("", "fm:1", "");
        ok(!lockOnly.at(FM).active, "★ a lock alone, with no position, limits nothing");
    }

    std::printf("\nEntering a limited band moves the radio onto the band's curve, under its figure\n");
    {
        Cfg c("fm:100", "", "fm:1");
        const Limit l = c.at(FM);
        Target t = target(l, kModeFree, -1);
        ok(t.change && t.mode == kModeSensitive && t.pos == 10,
           "★★ from FREE with nothing remembered on the curve -> Sensitivity 10");
        t = target(l, kModeLinear, 6);
        ok(t.change && t.mode == kModeSensitive && t.pos == 6,
           "from the OTHER curve, remembered 6 on this one -> Sensitivity 6 (within the ceiling)");
        t = target(l, kModeSensitive, 18);
        ok(t.change && t.pos == 10, "already on the curve at 18 -> brought down to 10");
        t = target(l, kModeSensitive, 4);
        ok(!t.change, "★ already on the curve at 4 -> left alone (nothing is raised)");
        Cfg lk("fm:100", "fm:1", "fm:1");
        t = target(lk.at(FM), kModeSensitive, 4);
        ok(t.change && t.pos == 10, "★ a LOCKED band puts it exactly at 10, from below as well");
    }

    std::printf("\nRules written before the curve existed (7c740045) and by hand\n");
    {
        Cfg c("fm:150", "", "");
        const Limit l = c.at(FM);
        ok(l.active && !l.sensitivity, "★★ a position with no curve is held on LINEARITY");
        ok(l.maxPos == 15, "at the stored position", std::to_string(l.maxPos));
        Cfg over("fm:25dB", "", "fm:0");   // an owner typing the dongle's spelling on this radio
        ok(over.at(FM).maxPos == 21, "a figure past the top is held at 21, never beyond",
           std::to_string(over.at(FM).maxPos));
        Cfg zero("fm:0", "", "fm:0");
        ok(zero.at(FM).active && zero.at(FM).maxPos == 0, "★ position 0 is a real rule, not 'none'");
    }

    std::printf("\nOverlapping and 'all' rules\n");
    {
        // ★ The TIGHTER ceiling wins (gainCapAt); the curve is a choice, so the owner's own order wins.
        Cfg c("all:180, fm:80", "", "fm:1, all:0");
        ok(c.at(FM).maxPos == 8, "FM: the tighter of all:18 and fm:8");
        ok(c.at(FM).sensitivity, "FM: its own curve (first match) — Sensitivity");
        ok(c.at(UHF).maxPos == 18 && !c.at(UHF).sensitivity, "elsewhere: all:18 on Linearity");
    }

    std::printf("\n%s%d checks\n", failures ? "FAILURES — " : "", checks);
    if (failures) std::printf("%d FAILED\n", failures);
    return failures ? 1 : 0;
}
