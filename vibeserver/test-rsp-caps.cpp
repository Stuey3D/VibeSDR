// ★★★ WHAT EACH RSP MODEL HAS (vibe_rsp_caps.h, 2026-10-22) — the table every panel's controls come from, by way of
// hwinfo. One row per model, from SDRplay's API 3.15 headers: if a row here changes, a control appears or vanishes
// on somebody's radio, so the test spells the whole table out rather than spot-checking it.
//   build: c++ -std=c++17 -I android/app/src/main/cpp -o /tmp/trc vibeserver/test-rsp-caps.cpp
#include "vibe_rsp_caps.h"
#include <cstdio>
#include <string>
#include <vector>

static int fails = 0, passes = 0;
static void ok(bool cond, const std::string& what) {
    if (cond) { ++passes; printf("   ok   %s\n", what.c_str()); }
    else      { ++fails;  printf("   FAIL %s\n", what.c_str()); }
}

int main() {
    using namespace vibe::rspcaps;
    struct Want { int hw; const char* model; bool rf, dab, bt, hdr, am, ext; std::vector<std::string> ports; };
    const std::vector<Want> table = {
        { kRsp1,    "RSP1",     false, false, false, false, false, false, {} },
        { kRsp1A,   "RSP1A",    true,  true,  true,  false, false, false, {} },
        { kRsp1B,   "RSP1B",    true,  true,  true,  false, false, false, {} },
        { kRsp2,    "RSP2",     true,  false, true,  false, false, true,  {"A", "B", "Hi-Z"} },
        { kRspDuo,  "RSPduo",   true,  true,  true,  false, true,  true,  {"Tuner 1 50Ω", "Tuner 1 Hi-Z", "Tuner 2 50Ω"} },
        { kRspDx,   "RSPdx",    true,  true,  true,  true,  false, false, {"A", "B", "C"} },
        { kRspDxR2, "RSPdx-R2", true,  true,  true,  true,  false, false, {"A", "B", "C"} },
    };
    printf("\nPer model: which controls the panels may draw\n");
    for (const auto& w : table) {
        const Caps c = forHwVer(w.hw);
        const std::string m = w.model;
        ok(m == c.model, m + ": model name");
        ok(c.rfNotch == w.rf && c.dabNotch == w.dab, m + ": FM/MW notch " + (w.rf ? "yes" : "no") + ", DAB notch " + (w.dab ? "yes" : "no"));
        ok(c.biasT == w.bt, m + ": bias-T " + (w.bt ? "yes" : "no"));
        ok(c.hdr == w.hdr && c.amNotch == w.am && c.extRef == w.ext,
           m + ": HDR " + (w.hdr ? "yes" : "no") + ", AM notch " + (w.am ? "yes" : "no") + ", 24 MHz ref " + (w.ext ? "yes" : "no"));
        ok(c.ports == w.ports, m + ": " + (w.ports.empty() ? "one socket, so no selector" : std::to_string(w.ports.size()) + " antenna choices"));
    }
    printf("\nThe rules the table must keep\n");
    for (const auto& w : table) {
        const Caps c = forHwVer(w.hw);
        ok(c.ports.empty() || c.ports.size() >= 2, std::string(w.model) + ": never a one-item selector");
        ok(!c.hdr || std::string(c.model).rfind("RSPdx", 0) == 0, std::string(w.model) + ": HDR only on the dx family");
    }
    const Caps unknown = forHwVer(99);
    ok(!unknown.rfNotch && !unknown.dabNotch && !unknown.biasT && !unknown.hdr && !unknown.amNotch && !unknown.extRef
       && unknown.ports.empty(), "an unknown model: no controls at all — never a guess");
    printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
