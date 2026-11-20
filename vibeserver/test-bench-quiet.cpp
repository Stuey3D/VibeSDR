// ★★★ THE WHOLE MACHINE HOLDS STILL FOR A BENCHMARK (Stuart, 2026-11-20) — vibe_bench_quiet.h's pure parts: the lease, the
// reading of a radio process's answers, and the sentences the setup page shows. The end-to-end half (a front door holding
// a busy radio process still) is scripts/test-bench-quiet.mjs.
//   build: c++ -std=c++17 -I vibeserver -o /tmp/t vibeserver/test-bench-quiet.cpp && /tmp/t
#include "vibe_bench_quiet.h"
#include <cstdio>
#include <string>

using namespace vibe::benchquiet;
static int fails = 0, passes = 0;
static void ok(bool c, const char* what, const std::string& got = "") {
    if (c) ++passes; else { ++fails; std::printf("FAIL  %s%s%s\n", what, got.empty() ? "" : " — got: ", got.c_str()); }
}

int main() {
    // ── the lease: a coordinator that dies leaves no radio deaf for long ──
    {
        Lease l;
        ok(!l.on && !l.expired(1e9), "a lease never taken is not held and never expires");
        l.hold(100.0, 90.0);
        ok(l.on && !l.expired(100.0) && !l.expired(189.9), "held for its 90 s");
        ok(l.expired(190.0), "…and runs out at 90 s without a renewal");
        l.hold(150.0, 90.0);
        ok(!l.expired(190.0) && l.expired(240.0), "a renewal counts 90 s from the renewal");
        l.end();
        ok(!l.on && !l.expired(1e9), "ended: not held, nothing to expire");
        ok(kRenewSec * 3 < kLeaseSec, "the coordinator renews at least three times inside one lease (a missed renewal is not fatal)");
        ok(leaseSec() == kLeaseSec, "no test override: the real lease");
        ok(renewSec() == kRenewSec, "…renewed every 20 s");
    }
    // ── reading a radio process's answer (our own JSON) ──
    {
        const std::string chk = "{\"hold\":\"check\",\"listeners\":3,\"busy\":false,\"held\":false}";
        ok(jsonNum(chk, "listeners") == 3, "listeners read");
        ok(!jsonTrue(chk, "busy") && !jsonTrue(chk, "held"), "busy / held false read as false");
        ok(jsonTrue("{\"hold\":\"on\",\"held\":true,\"released\":true}", "held"), "held true read");
        ok(jsonNum("{\"v\":1}", "listeners") == -1, "an absent number is -1 (an older build's saved result)");
        ok(jsonStr("{\"error\":\"its radio could not be \\\"taken\\\" off\"}", "error") == "its radio could not be \"taken\" off",
           "an error's words, quotes unescaped", jsonStr("{\"error\":\"its radio could not be \\\"taken\\\" off\"}", "error"));
        ok(jsonStr("{}", "error").empty(), "no error: empty");
    }
    // ── the sentences ──
    {
        ok(listenersRefusal({}).empty() && listenersRefusal({{"RTL", 0}, {"HF+", 0}}).empty(), "nobody listening: no refusal");
        const std::string one = listenersRefusal({{"RTL-SDR", 0}, {"Airspy HF+", 1}});
        ok(one == "1 listener is connected (Airspy HF+: 1) \xe2\x80\x94 every radio on this machine goes off the air for the measurement",
           "one listener: singular, the radio named, the idle one not", one);
        const std::string many = listenersRefusal({{"RTL-SDR", 2}, {"Airspy HF+", 1}, {"RSP1A", 0}});
        ok(many.rfind("3 listeners are connected (RTL-SDR: 2, Airspy HF+: 1)", 0) == 0, "summed ACROSS the machine, each radio named", many);
        ok(many.find("listener") != std::string::npos, "keeps the word the page offers \"Run it anyway?\" on");
        const std::string cq = cannotQuietRefusal("RTL-SDR", noAnswerWhy());
        ok(cq.find("RTL-SDR did not answer") != std::string::npos && cq.find("would be wrong") != std::string::npos
           && cq.find("Nothing was changed") != std::string::npos, "a radio that would not hold still: named, why, and that nothing changed", cq);
        ok(cq.find("listener") == std::string::npos, "…and it is NOT a listener refusal (forcing it would not help)");
    }
    std::printf("%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
