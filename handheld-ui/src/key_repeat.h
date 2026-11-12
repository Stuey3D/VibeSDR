// key_repeat.h — HOLD LEFT / RIGHT ON A SLIDER TO SLIDE IT, and the latest-value sender that keeps a held slider's
// messages to a sane rate. No SDL, no clock of its own (the caller passes the time): test/key_repeat_test.cpp drives it
// with a simulated clock.
//
// ★★ Stuart, 2026-11-12: "on the auto squelch or volume sliders holding the left or right should slide them for as long
//    as the direction is held, right now i have to rapidly press the buttons which is awkward."
//    • a press is one step (the press itself, as before — this adds only the repeats);
//    • held 350 ms: 12 steps a second; after a further second it speeds up, to 30 a second by 2 s more;
//    • it stops the instant the direction is let go, and never fires on anything but the slider the press began on
//      (the highlight moved, a sheet opened or closed: the hold is over until the next press). Only a slider repeats —
//      a stepper or a choice row still takes one press per step (ui.h Nav::item `repeats`).
// ★★ LatestSender: a held slider makes up to 30 changes a second, and each one would be a message (the squelch to the
//    engine, the codec volume and the backlight as an HTTP POST on a thread of its own each — they could even land out
//    of order). Per key: the first change goes at once; after that at most one every 100 ms, always the LATEST value;
//    the last one is never lost (it goes when its 100 ms are up, or at once when the direction is let go — flush).
#pragma once
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <string>

namespace hh {

class KeyRepeat {
public:
    static constexpr double kDelay = 0.35, kRate = 12, kFastAfter = 1.0, kFastRate = 30, kRamp = 1.0;
    /** The direction went down at `t` (−1 left, +1 right). `where` names the control it began on (layer + id); `repeats`
     *  says whether that control slides. The press itself is the first step — the caller handles it as before. */
    void down(int dir, double t, const std::string& where, bool repeats) {
        dir_ = repeats ? dir : 0; where_ = where; t0_ = t; done_ = 0;
    }
    /** The direction came up (or went the other way): no more steps. */
    void up(int dir) { if (dir == 0 || dir == dir_) dir_ = 0; }
    void cancel() { dir_ = 0; }
    bool active() const { return dir_ != 0; }
    /** Steps due at `t` (signed; 0 none) while the highlight is still on `where`. Anything else ends the hold. */
    int due(double t, const std::string& where) {
        if (!dir_) return 0;
        if (where != where_) { dir_ = 0; return 0; }
        const long want = stepsBy(t - t0_);
        const long n = want - done_;
        done_ = want;
        return n > 0 ? (int)n * dir_ : 0;
    }
    /** How many repeat steps are due `held` seconds after the press (the press itself not counted). */
    static long stepsBy(double held) {
        const double r = held - kDelay;
        if (r < 0) return 0;
        // 12 a second for kFastAfter s, then the rate ramps linearly to kFastRate over kRamp s, then stays there
        double n;
        if (r <= kFastAfter) n = r * kRate;
        else if (r <= kFastAfter + kRamp) { const double x = r - kFastAfter; n = kFastAfter * kRate + kRate * x + (kFastRate - kRate) * x * x / (2 * kRamp); }
        else n = kFastAfter * kRate + (kRate + kFastRate) / 2 * kRamp + (r - kFastAfter - kRamp) * kFastRate;
        return 1 + (long)std::floor(n + 1e-9);   // ★ the first repeat lands AT 350 ms
    }

private:
    int dir_ = 0;
    std::string where_;
    double t0_ = 0;
    long done_ = 0;
};

/** ONE FRAME of the hold (App::run, before Nav::begin): `pressed` is the press this frame uses (B_LEFT −1, B_RIGHT +1,
 *  any other button 2, none 0), `held` the direction down now, `where` the highlight (layer|id) and `slides` whether
 *  that control slides (Nav::focusRepeats). Returns the repeat steps for Input::repeat. */
inline int keyRepeatFrame(KeyRepeat& r, int pressed, int held, double t, const std::string& where, bool slides) {
    if (pressed == -1 || pressed == 1) { if (held == pressed) r.down(pressed, t, where, slides); else r.cancel(); return 0; }
    if (pressed) { r.cancel(); return 0; }
    return r.due(t, where);
}

class LatestSender {
public:
    static constexpr double kGap = 0.1;
    /** Send `f` for `key` now if its last send was ≥ 100 ms ago, else keep it (the latest wins) for pump / flush. */
    void send(const std::string& key, double t, std::function<void()> f) {
        Slot& s = slots_[key];
        if (t - s.last >= kGap) { s.pending = nullptr; s.last = t; f(); }
        else s.pending = std::move(f);
    }
    /** Drop anything waiting for `key` (a newer value went out by another path). */
    void drop(const std::string& key) { auto it = slots_.find(key); if (it != slots_.end()) it->second.pending = nullptr; }
    /** Once a frame: whatever is waiting and due goes. */
    void pump(double t) {
        for (auto& kv : slots_) if (kv.second.pending && t - kv.second.last >= kGap) fire(kv.second, t);
    }
    /** The direction was let go: everything waiting goes now. */
    void flush(double t) { for (auto& kv : slots_) if (kv.second.pending) fire(kv.second, t); }
    bool waiting(const std::string& key) const { auto it = slots_.find(key); return it != slots_.end() && it->second.pending; }

private:
    struct Slot { double last = -1e9; std::function<void()> pending; };
    std::map<std::string, Slot> slots_;
    static void fire(Slot& s, double t) { auto f = std::move(s.pending); s.pending = nullptr; s.last = t; f(); }
};

}  // namespace hh
