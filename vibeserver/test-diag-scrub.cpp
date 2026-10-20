// ★★★ The daemon's own scrub of the diagnostics file (diag_scrub.h, 2026-10-20): planted secrets never survive, a
//     secret inside another goes whole, short values (which would eat ordinary words) are left, the file name is safe.
#include "diag_scrub.h"
#include <cstdio>
#include <string>

static int fails = 0;
static void ok(bool c, const std::string& w) {
    std::printf("  %s %s\n", c ? "\033[32mok\033[0m  " : "\033[31mFAIL\033[0m", w.c_str()); if (!c) fails++;
}

int main() {
    std::printf("diagnostics — the daemon's scrub\n");
    const std::string admin = "Adm1n-Secret-77", pin = "482913", rpin = "771200", dkey = "dk_live_ABCDEF123456";
    const std::string in = "config {\"adminPass\":\"" + admin + "\",\"pin\":\"" + pin + "\"}\n"
                           "log: radio pin " + rpin + " twice " + rpin + "\nkey=" + dkey + " and " + dkey + "X\n"
                           "ordinary words: pin abc\n";
    const std::string out = vsdiag::scrubSecrets(in, {pin, admin, rpin, dkey, "abc", ""});
    ok(out.find(admin) == std::string::npos, "the admin password is gone");
    ok(out.find(pin) == std::string::npos && out.find(rpin) == std::string::npos, "the listener PIN and a radio's PIN are gone (every time)");
    ok(out.find(dkey) == std::string::npos, "the directory key is gone, even with text glued to it");
    ok(out.find("pin abc") != std::string::npos, "a value under 4 characters is left alone (it would eat ordinary words)");
    ok(vsdiag::scrubSecrets("x Secret-77-long y", {"Secret-77", "Secret-77-long"}) == "x [redacted] y",
       "a secret containing another is removed whole (longest first)");
    ok(vsdiag::fileName("vibe pocket/../x", "20261020-120000") == "vibeserver-diagnostics-vibe-pocket----x-20261020-120000.txt",
       "the file name keeps only letters, digits and '-' from the host");
    ok(vsdiag::fileName("", "s") == "vibeserver-diagnostics-vibeserver-s.txt", "…and never an empty host");
    std::printf(fails ? "\033[31m%d failed\033[0m\n" : "\033[32mall passed\033[0m\n", fails);
    return fails ? 1 : 0;
}
