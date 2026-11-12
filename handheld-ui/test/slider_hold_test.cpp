// slider_hold_test.cpp — HOLD LEFT / RIGHT ON A SLIDER TO SLIDE IT (Stuart, 2026-11-12), on a SIMULATED CLOCK, through
// key_repeat.h and the real Nav + slider() (ui.h) offscreen:
//   • the timing: one step on the press, the first repeat at 350 ms, 12 a second, faster after a second, ≤ 30 a second;
//   • release: not one step more; a press elsewhere, or the highlight moving, ends the hold;
//   • only a slider slides — a stepper (takesLR, not repeats) takes a step a press, and the highlight never moves on a hold;
//   • LatestSender: a held slider's sends at most every 100 ms, the latest value, the last always (on release at once).
// Silent. Run in the arm64 test image: handheld-ui/test/run_offscreen_tests.sh slider_hold_test
#include "../src/app.h"

#include <cstdio>
#include <string>

using namespace hh;

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } } while (0)

static void timing() {
    // the schedule itself
    CHECK(KeyRepeat::stepsBy(0.0) == 0 && KeyRepeat::stepsBy(0.349) == 0, "no repeat before 350 ms");
    CHECK(KeyRepeat::stepsBy(0.35) == 1, "the first repeat at 350 ms (%ld)", KeyRepeat::stepsBy(0.35));
    const long a = KeyRepeat::stepsBy(0.35 + 1.0) - KeyRepeat::stepsBy(0.35);
    CHECK(a == 12, "12 a second for the first second (%ld)", a);
    const long fast = KeyRepeat::stepsBy(0.35 + 3.0) - KeyRepeat::stepsBy(0.35 + 2.0);
    CHECK(fast == 30, "30 a second once up to speed (%ld)", fast);
    const long mid = KeyRepeat::stepsBy(0.35 + 2.0) - KeyRepeat::stepsBy(0.35 + 1.0);
    CHECK(mid > 12 && mid < 30, "speeding up between (%ld)", mid);
    long prev = 0;
    bool mono = true;
    for (double t = 0; t < 6; t += 0.001) { const long n = KeyRepeat::stepsBy(t); if (n < prev) mono = false; prev = n; }
    CHECK(mono, "the schedule never goes back");

    // press, hold, release (frames at 30 a second)
    KeyRepeat r;
    double t = 10;
    keyRepeatFrame(r, 1, 1, t, "audio|a.vol", true);
    int steps = 0, frames = 0;
    for (; t < 10 + 1.35; t += 1.0 / 30) { steps += keyRepeatFrame(r, 0, 1, t, "audio|a.vol", false); frames++; }
    CHECK(steps == 12 || steps == 13, "held 1.35 s: 12 repeats after the press (%d)", steps);
    r.up(1);
    int after = 0;
    for (double u = t; u < t + 1; u += 1.0 / 30) after += keyRepeatFrame(r, 0, 0, u, "audio|a.vol", false);
    CHECK(after == 0 && !r.active(), "release: not one step more (%d)", after);

    // left held: negative steps
    keyRepeatFrame(r, -1, -1, 20, "audio|a.vol", true);
    CHECK(keyRepeatFrame(r, 0, -1, 20.4, "audio|a.vol", false) < 0, "left held steps left");
    r.up(-1);

    // the highlight moved (another sheet, a code jump): the hold is over, and stays over when it comes back
    keyRepeatFrame(r, 1, 1, 30, "audio|a.vol", true);
    CHECK(keyRepeatFrame(r, 0, 1, 30.5, "menu|m.close", false) == 0 && !r.active(), "focus moved: no steps, hold ended");
    CHECK(keyRepeatFrame(r, 0, 1, 31, "audio|a.vol", false) == 0, "back on the slider: still no steps until a new press");

    // a press released before it was used (a quick tap): no hold
    keyRepeatFrame(r, 1, 0, 40, "audio|a.vol", true);
    CHECK(!r.active() && keyRepeatFrame(r, 0, 0, 41, "audio|a.vol", false) == 0, "a tap does not repeat");
    // a press on a control that does not slide: no hold
    keyRepeatFrame(r, 1, 1, 50, "dec|dec.p.cw", false);
    CHECK(!r.active() && keyRepeatFrame(r, 0, 1, 51, "dec|dec.p.cw", false) == 0, "a stepper / choice row does not repeat");
    // another button pressed during a hold ends it
    keyRepeatFrame(r, 1, 1, 60, "audio|a.vol", true);
    keyRepeatFrame(r, 2, 1, 60.2, "audio|a.vol", true);
    CHECK(!r.active(), "ENTER / UP during a hold ends it");
}

/** The Nav side: a slider and a stepper on one layer, the repeat as App::run feeds it. */
static void nav(App& a) {
    double vol = 50, step = 3;
    auto frame = [&](Input in) {
        a.nav_.begin("audio", in);
        Ctx c{a.g_, a.tc_, a.f_, a.nav_, 0};
        c.nav.layer("audio");
        slider(c, "a.vol", {100, 100, 300, 40}, vol, 0, 100, 1);
        const Nav::Hit h = c.nav.item("pad.feel", {100, 200, 300, 40}, true);   // a stepper (takesLR, no repeats)
        if (h.adjust) step += h.adjust;
        c.nav.item("below", {100, 300, 300, 40});
        a.nav_.end();
        a.g_.flush();
    };
    a.nav_.setFocus("audio", "a.vol");
    frame({});
    // press RIGHT, hold 2 s at 30 fps (the App's loop: the press, then repeats), release
    KeyRepeat r;
    double t = 100;
    { Input in; in.nav = D_RIGHT; keyRepeatFrame(r, 1, 1, t, "audio|" + a.nav_.focus("audio"), a.nav_.focusRepeats("audio")); frame(in); }
    CHECK(vol == 51, "the press: one step (%g)", vol);
    for (int k = 1; k <= 60; k++) { Input in; in.repeat = keyRepeatFrame(r, 0, 1, t + k / 30.0, "audio|" + a.nav_.focus("audio"), false); frame(in); }
    const double expect = 51 + KeyRepeat::stepsBy(2.0);
    CHECK(vol == expect, "held 2 s: %g (expected %g)", vol, expect);
    CHECK(a.nav_.focus("audio") == "a.vol", "the highlight stayed on the slider (%s)", a.nav_.focus("audio").c_str());
    r.up(1);
    const double at = vol;
    for (int k = 61; k <= 90; k++) { Input in; in.repeat = keyRepeatFrame(r, 0, 0, t + k / 30.0, "audio|" + a.nav_.focus("audio"), false); frame(in); }
    CHECK(vol == at, "released: it stops at once (%g → %g)", at, vol);
    // to the end of the track: clamped, no wrap
    { Input in; in.nav = D_RIGHT; keyRepeatFrame(r, 1, 1, 200, "audio|a.vol", a.nav_.focusRepeats("audio")); frame(in); }
    for (int k = 1; k <= 200; k++) { Input in; in.repeat = keyRepeatFrame(r, 0, 1, 200 + k / 30.0, "audio|" + a.nav_.focus("audio"), false); frame(in); }
    CHECK(vol == 100, "held to the end: 100 (%g)", vol);
    r.up(1);
    // DOWN to the stepper: a hold there is one step only, and a stray repeat never moves the highlight
    { Input in; in.nav = D_DOWN; frame(in); }
    CHECK(a.nav_.focus("audio") == "pad.feel", "on the stepper");
    CHECK(!a.nav_.focusRepeats("audio"), "the stepper does not slide");
    { Input in; in.nav = D_RIGHT; keyRepeatFrame(r, 1, 1, 300, "audio|pad.feel", a.nav_.focusRepeats("audio")); frame(in); }
    for (int k = 1; k <= 30; k++) { Input in; in.repeat = keyRepeatFrame(r, 0, 1, 300 + k / 30.0, "audio|pad.feel", false); frame(in); }
    CHECK(step == 4, "a held stepper: the press's one step only (%g)", step);
    { Input in; in.repeat = 3; frame(in); }   // (as if one leaked)
    CHECK(step == 4 && a.nav_.focus("audio") == "pad.feel", "a repeat on a stepper: nothing, and the highlight stays");
}

static void sender() {
    LatestSender ls;
    std::vector<std::pair<double, int>> sent;
    double t = 0;
    int v = 0;
    // a hold: a new value every 1/30 s for 1 s
    for (int k = 0; k < 30; k++, t += 1.0 / 30) { v++; const int x = v; const double at = t; ls.send("squelch", t, [&sent, at, x] { sent.push_back({at, x}); }); ls.pump(t); }
    CHECK(sent.size() >= 9 && sent.size() <= 11, "≈ 10 a second (%zu)", sent.size());
    bool spaced = true;
    for (size_t i = 1; i < sent.size(); i++) if (sent[i].first - sent[i - 1].first < 0.1 - 1e-9) spaced = false;
    CHECK(spaced, "never two within 100 ms");
    bool rising = true;
    for (size_t i = 1; i < sent.size(); i++) if (sent[i].second <= sent[i - 1].second) rising = false;
    CHECK(rising, "in order, each the latest");
    CHECK(sent.front().second == 1, "the first goes at once");
    // release: the last value goes at once
    ls.flush(t);
    CHECK(sent.back().second == v, "the final value is sent (%d of %d)", sent.back().second, v);
    CHECK(!ls.waiting("squelch"), "nothing left waiting");
    // with no flush, it goes by itself once its 100 ms are up
    ls.send("speaker", 10, [&sent] { sent.push_back({10, 1}); });
    ls.send("speaker", 10.03, [&sent] { sent.push_back({10.03, 2}); });
    ls.pump(10.05);
    CHECK(sent.back().second == 1, "not yet");
    ls.pump(10.11);
    CHECK(sent.back().second == 2, "the last one went by itself after 100 ms");
    // a newer value by another path drops the waiting one
    ls.send("squelch", 20, [&sent] { sent.push_back({20, 7}); });
    ls.send("squelch", 20.02, [&sent] { sent.push_back({20.02, 8}); });
    ls.drop("squelch"); ls.flush(21);
    CHECK(sent.back().second == 7, "dropped: the stale value never goes");
    // a tap (no hold): each one at once when they are ≥ 100 ms apart
    const size_t n0 = sent.size();
    for (int k = 0; k < 5; k++) ls.send("hw.vol", 30 + k * 0.3, [&sent] { sent.push_back({0, 0}); });
    CHECK(sent.size() == n0 + 5, "taps go at once");
}

int main() {
    timing();
    sender();
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0 || TTF_Init() != 0) { std::fprintf(stderr, "init failed\n"); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    static App a;
    a.win_ = SDL_CreateWindow("hold", 0, 0, a.W, a.H, SDL_WINDOW_OPENGL);
    SDL_GLContext ctx = a.win_ ? SDL_GL_CreateContext(a.win_) : nullptr;
    if (!ctx) { std::fprintf(stderr, "GL: %s\n", SDL_GetError()); return 1; }
    a.g_.init(a.W, a.H);
    nav(a);
    std::printf("slider_hold_test: %d / %d checks pass\n", checks - fails, checks);
    return fails ? 1 : 0;
}
