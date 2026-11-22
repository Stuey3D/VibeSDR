// test-ft8-deep-auto — WHETHER FT8 / FT4 "DIG DEEPER" IS ON (decoders/ft8_deep_auto.h, V12 beta91).
//
// ★★★ Stuart, 2026-11-22: "should be automatic and simply enable when the cpu has headroom with a benchmark driven toggle
//     in the settings with the rest of the digital modes, no user facing toggles". What must hold:
//   1. a FRESH server (no benchmark, no choice) is off and asks to measure itself once; its own figure then decides;
//   2. an UPGRADED server (a beta90-or-older benchmark: a Band row, no dig-deeper row) decides at once from the Band row;
//   3. a current benchmark's dig-deeper row decides (25 % of a core); a row listed as not run (no radio makes spots) = off;
//   4. the OWNER's word ("ft8+deep" / "ft8-deep") wins over all of it, and survives the server's own mode-list handling
//      (vibereach::effectiveBlocked keeps both tokens), so it persists in the radio's saved list as any switch does.
// (No listener-facing control: scripts/test-ft8-deep-owner-only.mjs.)
#include "decoders/ft8_deep_auto.h"
#include "vibe_decoder_reach.h"
#include <cstdio>
#include <string>

using namespace vibe::ft8deep;
static int fails = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (!c) fails++; }

static std::string bench(const std::string& rows, const std::string& notRun = "") {
    return "{\"v\":1,\"at\":1795370000,\"rows\":[" + rows + "]" + (notRun.empty() ? "" : ",\"notRun\":[" + notRun + "]") + "}";
}
static std::string row(const char* id, double pct) {
    char b[200];
    std::snprintf(b, sizeof b, "{\"id\":\"%s\",\"label\":\"x\",\"rate\":48000,\"pct\":%.1f,\"grade\":\"green\",\"thread\":\"decoder\",\"threads\":{}}", id, pct);
    return b;
}
/** The owner's choice as the shim reads it: the radio's saved list, through the server's own handling of it. */
static Choice fromCsv(const std::string& csv) {
    const std::string eff = vibereach::effectiveBlocked(csv, 1);
    auto has = [&](const std::string& t) { return ("," + eff + ",").find("," + t + ",") != std::string::npos; };
    return ownerChoice(has("ft8+deep"), has("ft8-deep"));
}

int main() {
    bool need = false;
    std::printf("1. A fresh server: no benchmark, no choice\n");
    ok(!decide(Choice::Auto, "", -1, need) && need, "off, and it asks to measure itself once (the shim's idle probe)");
    ok(decide(Choice::Auto, "", 0.9, need) && !need, "its own figure (0.9 % of a core, a Lenovo / Pi 500 class box) turns it on");
    ok(!decide(Choice::Auto, "", 48.0, need) && !need, "...and a Pi 2's (~48 %) leaves it off, without measuring again");
    ok(!decide(Choice::Auto, "", 25.0, need), "the line is 25 % of a core: exactly 25 is off");
    ok(!decide(Choice::Auto, bench(row("dec_rtty", 2.0)), -1, need) && need,
       "a benchmark with no FT8 rows at all says nothing: measure");

    std::printf("2. A server upgrading from beta90 or earlier (a Band row, no dig-deeper row)\n");
    ok(decide(Choice::Auto, bench(row("dec_ft8", 0.1) + "," + row("dec_ft8band", 0.8)), -1, need) && !need,
       "Band at one pass 0.8 % (an M5): on at once, no measuring");
    ok(decide(Choice::Auto, bench(row("dec_ft8band", 15.0)), -1, need) && !need, "Band 15 % (x 1.25 = 18.8 %): on");
    ok(!decide(Choice::Auto, bench(row("dec_ft8band", 21.0)), -1, need) && !need, "Band 21 % (x 1.25 = 26 %): off");
    ok(!decide(Choice::Auto, bench(row("dec_ft8band", 55.0)), -1, need) && !need, "a Pi 2's Band row: off");
    ok(!decide(Choice::Auto, bench(row("dec_rtty", 2.0), "{\"id\":\"dec_ft8band\",\"why\":\"no radio here makes spots\"}"), 0.5, need) && !need,
       "Band listed as not run (no radio here makes spots): off, and its own figure is not wanted");
    ok(decide(Choice::Auto, bench(row("dec_ft8band", 0.8)), 99.0, need), "a benchmark outranks the server's own figure");

    std::printf("3. A current benchmark (its dig-deeper row)\n");
    ok(decide(Choice::Auto, bench(row("dec_ft8band", 0.8) + "," + row("dec_ft8deep", 0.97)), -1, need), "dig deeper 0.97 %: on");
    ok(!decide(Choice::Auto, bench(row("dec_ft8band", 10.0) + "," + row("dec_ft8deep", 30.0)), -1, need),
       "dig deeper 30 %: off (the row decides, not the Band row's estimate)");
    ok(!decide(Choice::Auto, bench(row("dec_ft8band", 60.0) + "," + row("dec_ft8deep", -1.0)), -1, need) && !need,
       "dig deeper not run because Band was not green: off");

    std::printf("4. The owner's word wins, and persists\n");
    const std::string green = bench(row("dec_ft8deep", 0.5)), red = bench(row("dec_ft8deep", 60.0));
    ok(!decide(fromCsv("wefax,ft8-deep"), green, -1, need) && !need, "\"ft8-deep\" on a box that measured green: off");
    ok(decide(fromCsv("ft8+deep,dab"), red, -1, need) && !need, "\"ft8+deep\" on a box that measured red: on");
    ok(decide(fromCsv("ft8+deep"), "", -1, need) && !need, "\"ft8+deep\" with no benchmark: on, and nothing is measured");
    ok(fromCsv("wefax,dab") == Choice::Auto, "no word = automatic");
    ok(fromCsv("ft8+deep,ft8-deep") == Choice::Auto, "both words (never written by the page) = automatic, not a guess");
    const std::string eff = vibereach::effectiveBlocked("ft8+deep,ft8-deep,wefax", 1);
    ok(eff.find("ft8+deep") != std::string::npos && eff.find("ft8-deep") != std::string::npos,
       "the server's mode-list handling keeps both tokens (" + eff + ")");

    std::printf(fails ? "\n%d FAILURE(S)\n" : "\nALL PASS (0 failures)\n", fails);
    return fails ? 1 : 0;
}
