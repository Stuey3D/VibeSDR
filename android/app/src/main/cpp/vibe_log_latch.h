// vibe_log_latch.h — log a condition when it CHANGES, and once a minute while it lasts.
//
// ★★★ WHY (B6, 2026-09-30). The audio audit said "AUDIO AUDIT: quiet 99 %" once a second on every
//     squelched NFM, airband or muted listen — which is most listening — and Android keeps a 1 MB
//     logcat ring, so the log held about FIVE MINUTES and the evidence of a real fault had always
//     scrolled away before anybody looked. ADC OVERLOAD (once a second while overloaded) and the
//     SPEC RATE audit (every 5 s per slow listener, for hours) had the same shape.
//  ★ Only what is LOGGED changes. Nothing measured, and nothing any DSP decision reads, moves.
#pragma once
#include <cmath>
#include <cstdio>
#include <string>

namespace vibe {

/** ★★★ SAY IT WHEN IT CHANGES, AND ONCE A MINUTE WHILE IT LASTS (B6, 2026-09-30).
 *  A condition that persists is not news every second. The audio audit said "quiet 99 %" once
 *  a second on every squelched NFM, airband or muted listen — i.e. most of the time — and
 *  Android's 1 MB logcat ring then held about five minutes, so the evidence of a REAL fault had
 *  scrolled away before anybody looked. This keeps the three things a reader needs: when it
 *  started (at once), that it is still going and for how long (every `every` s), and when it
 *  stopped (at once). Only what is LOGGED changes; nothing measured or decided does.
 *  step() → 0 say nothing · 1 it started · 2 still going (a summary is due) · 3 it stopped. */
struct LogLatch {
    bool on = false; double since = 0, said = 0;
    int step(bool bad, double nowSec, double every = 60.0) {
        if (bad && !on)  { on = true;  since = said = nowSec; return 1; }
        if (!bad && on)  { on = false; return 3; }
        if (bad && nowSec - said >= every) { said = nowSec; return 2; }
        return 0;
    }
    double forSec(double nowSec) const { return nowSec - since; }
};

struct AudioAudit {
    long long n = 0, nonFinite = 0, railed = 0, quiet = 0;
    double tMs = 0.0;
    void note(float v) {
        ++n;
        if (!std::isfinite(v)) { ++nonFinite; return; }
        const float a = std::fabs(v);
        if (a > 0.995f)      ++railed;
        else if (a < 1e-4f)  ++quiet;
    }
    // ★★ WHAT IT IS SAYING — see LogLatch. "quiet" is the normal state of a squelched or
    //    muted listen, so it is reported when it starts, once a minute while it lasts, and when
    //    it ends; "railed" the same (at once — no debounce); NON-FINITE every second it happens,
    //    always: a NaN latched into the DSP is the one fault this exists to catch, and it is rare.
    std::string state_;           // "", "quiet" or "railed"
    LogLatch latch_;
    // Returns true and fills `out` when there is something to say (at most once a second).
    bool tick(double nowMs, char* out, size_t cap, const char* stage) {
        if (tMs == 0.0) { tMs = nowMs; return false; }
        if (nowMs - tMs < 1000.0 || n == 0) return false;
        const double nf = 100.0 * (double)nonFinite / (double)n;
        const double rl = 100.0 * (double)railed    / (double)n;
        const double qt = 100.0 * (double)quiet     / (double)n;
        const double now = nowMs / 1000.0;
        bool say = false;
        if (nonFinite > 0) {
            snprintf(out, cap, "AUDIO AUDIT: nonfinite %.1f%% railed %.1f%% quiet %.1f%% (n=%lld)%s%s",
                     nf, rl, qt, n, stage ? " ORIGIN=" : "", stage ? stage : "");
            say = true;
        } else {
            const std::string st = rl > 20.0 ? "railed" : qt > 95.0 ? "quiet" : "";
            if (st != state_ && !state_.empty() && !st.empty()) latch_.on = false;   // quiet <-> railed: a new condition
            const int k = latch_.step(!st.empty(), now);
            if (k == 3) {
                snprintf(out, cap, "AUDIO AUDIT: %s ended after %.0f s", state_.c_str(), latch_.forSec(now));
                say = true;
            } else if (k == 1 || k == 2) {
                snprintf(out, cap, "AUDIO AUDIT: %s%s — railed %.1f%% quiet %.1f%% (n=%lld)%s%s",
                         st.c_str(), k == 2 ? (" for " + std::to_string((int)latch_.forSec(now)) + " s").c_str() : "",
                         rl, qt, n, stage ? " ORIGIN=" : "", stage ? stage : "");
                say = true;
            }
            state_ = st;
        }
        n = nonFinite = railed = quiet = 0; tMs = nowMs;
        return say;
    }
};

} // namespace vibe
