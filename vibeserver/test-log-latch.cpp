// test-log-latch.cpp — the audio audit and the ADC/spec-rate reports speak on CHANGE, not every second.
//
// ★★ The bug (B6, 2026-09-30): "AUDIO AUDIT: quiet 99 %" once a second on every squelched or muted
//    listen filled Android's 1 MB logcat ring in about five minutes, so a real fault's evidence was
//    always gone before anyone looked. The rule now: say it when it starts, once a minute while it
//    lasts (with how long), when it stops — and a NaN every single time.
#include "vibe_log_latch.h"
#include <cstdio>
#include <string>

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (!c) fails++; }

/** One simulated second of audio at one level, through the real audit. Returns the line, or "". */
static std::string second(AudioAudit& a, double& tMs, float level, bool nan = false) {
    for (int i = 0; i < 4800; ++i) a.note(nan && i == 7 ? NAN : level);
    tMs += 1000.0;
    char b[256];
    return a.tick(tMs, b, sizeof b, nullptr) ? std::string(b) : std::string();
}

int main() {
    std::printf("── LogLatch ──\n");
    LogLatch l;
    ok(l.step(false, 0) == 0, "nothing to say while all is well");
    ok(l.step(true, 1) == 1, "says it the moment it starts");
    int said = 0; for (int t = 2; t < 61; ++t) said += l.step(true, t) != 0;
    ok(said == 0, "silent for the rest of the minute");
    ok(l.step(true, 61) == 2 && l.forSec(61) == 60.0, "a summary a minute in, with how long");
    ok(l.step(false, 70) == 3 && l.forSec(70) == 69.0, "says when it stops, and after how long");

    std::printf("── the audio audit ──\n");
    AudioAudit a; double t = 1;
    char b[64]; a.tick(t, b, sizeof b, nullptr);   // arms the clock
    ok(second(a, t, 0.3f).empty(), "normal audio: nothing");
    const std::string q1 = second(a, t, 0.0f);
    ok(q1.find("AUDIO AUDIT: quiet") == 0, "squelch closes: said at once (" + q1 + ")");
    int lines = 0; for (int s = 0; s < 59; ++s) lines += !second(a, t, 0.0f).empty();
    ok(lines == 0, "59 more quiet seconds: silent (was 59 lines)");
    const std::string q2 = second(a, t, 0.0f);
    ok(q2.find("quiet for 60 s") != std::string::npos, "a minute in: one summary with the duration (" + q2 + ")");
    const std::string q3 = second(a, t, 0.3f);
    ok(q3.find("quiet ended after") != std::string::npos, "squelch opens: said at once (" + q3 + ")");
    const std::string r1 = second(a, t, 1.0f);
    ok(r1.find("AUDIO AUDIT: railed") == 0, "railed: said at once");
    const std::string r2 = second(a, t, 1.0f, true);
    ok(r2.find("nonfinite") != std::string::npos, "a NaN is said every time it happens");
    const std::string r3 = second(a, t, 1.0f, true);
    ok(r3.find("nonfinite") != std::string::npos, "...every single second, never latched");
    int perHour = 0; AudioAudit h; double th = 1; h.tick(th, b, sizeof b, nullptr);
    for (int s = 0; s < 3600; ++s) perHour += !second(h, th, 0.0f).empty();
    ok(perHour == 60, "an hour of squelched listening: 60 lines, not 3600 (" + std::to_string(perHour) + ")");
    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
