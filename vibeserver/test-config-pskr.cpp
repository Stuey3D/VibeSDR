// ★★ V12 beta90 PSK REPORTER / WSPRnet / DMR ID LOOKUP settings (ServerConfig pskrOn, pskrCall, pskrAntenna, wsprnetOn,
// dmrIdLookup) — machine-level, OFF by default, round-trip through the config, an older file reads them OFF, clamped.
#include "vibeserver_config.h"

#include <cstdio>
#include <string>

static int failures = 0, passes = 0;
static void ok(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "\033[32mok\033[0m  " : "\033[31mFAIL\033[0m", what);
    if (cond) passes++; else failures++;
}

int main() {
    std::printf("config — PSK Reporter, WSPRnet, the DMR ID lookup\n");
    vsconfig::ServerConfig d;
    ok(!d.pskrOn && !d.wsprnetOn && !d.dmrIdLookup && d.pskrCall.empty(), "defaults: all OFF, no callsign");
    d.pskrOn = true; d.pskrCall = "G4ABC"; d.pskrAntenna = "Wideband loop"; d.wsprnetOn = true; d.wsprnetCall = "G4ABC"; d.dmrIdLookup = true;
    vsconfig::ServerConfig back; std::string err;
    ok(vsconfig::fromJson(vsconfig::toJson(d), back, err), "written and read back");
    ok(back.pskrOn && back.pskrCall == "G4ABC" && back.pskrAntenna == "Wideband loop" && back.wsprnetOn && back.wsprnetCall == "G4ABC" && back.dmrIdLookup, "every key survives (and WSPRnet's own callsign)");
    const std::string old = R"({"configured": true, "name": "Loft", "radios": [{"serial":"00000001","driver":"rtlsdr","configured":true}]})";
    vsconfig::ServerConfig o;
    ok(vsconfig::fromJson(old, o, err) && !o.pskrOn && !o.wsprnetOn && !o.dmrIdLookup, "an older file: all OFF");
    const std::string longer = R"({"configured": true, "pskrOn": true, "pskrCall": "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "radios": []})";
    vsconfig::ServerConfig l;
    ok(vsconfig::fromJson(longer, l, err) && l.pskrCall.size() == 20, "the receiver's name clamped to 20");
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
