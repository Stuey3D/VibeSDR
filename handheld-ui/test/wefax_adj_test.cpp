// wefax_adj_test.cpp — WEFAX's ADJ ROW IS PRESSED INTO (Stuart, 2026-11-15: MARGIN and SLANT took LEFT / RIGHT the moment
// they had the highlight, so nothing could get from MARGIN to SLANT, AUTO or RAW). Through the R36S's own code
// (dec_pic_box.h picAdjRow) offscreen, with the real fonts and the real D-pad Nav:
//   • the row opens with the highlight on MARGIN, NOT editing;
//   • highlighted, LEFT / RIGHT move along MARGIN · SLANT · AUTO · RAW and change nothing;
//   • ENTER presses into an adjuster: LEFT / RIGHT then nudge it and the highlight stays;
//   • ENTER again finishes; BACK finishes too and is CONSUMED (the box is not released by the same press);
//   • UP / DOWN off an adjuster being edited ends the edit; closing ADJ ends it.
// The keyboard drives the same Nav (← → Enter Esc → B_LEFT / B_RIGHT / B_ENTER / B_BACK in app_impl.h pollInput), so
// these presses are the keyboard's too. Silent (no audio). Run: handheld-ui/test/run_offscreen_tests.sh wefax_adj_test
#include "../src/app.h"

#include <cstdio>
#include <string>

using namespace hh;

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } } while (0)

static App* gA;
static bool gBackLeft = false;   // the frame's BACK was still unconsumed after the row (the run loop would close the box)

static void frame(Input in = {}) {
    App& a = *gA;
    a.nav_.begin("dec", in);
    Ctx c{a.g_, a.tc_, a.f_, a.nav_, nowSec()};
    c.nav.layer("dec");
    picAdjRow(a, c, {10, 160, 620, 28});
    // something under the row, as the picture is in the box (so DOWN has somewhere to go)
    c.nav.item("dec.pic", {10, 200, 620, 100});
    gBackLeft = a.nav_.backPressed();
    a.nav_.end();
    a.g_.flush();
}
static void press(Dir d) { Input in; in.nav = d; frame(in); frame(); }
static void enter() { Input in; in.enter = true; frame(in); frame(); }
static void back() { Input in; in.back = true; frame(in); }
static std::string focus() { return gA->nav_.focus("dec"); }
static double shift() { return picCorr(*picUi().live).shift; }

int main() {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0 || TTF_Init() != 0) { std::fprintf(stderr, "init failed\n"); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    static App a;
    gA = &a;
    a.win_ = SDL_CreateWindow("adj", 0, 0, a.W, a.H, SDL_WINDOW_OPENGL);
    SDL_GLContext ctx = a.win_ ? SDL_GL_CreateContext(a.win_) : nullptr;
    if (!ctx) { std::fprintf(stderr, "GL: %s\n", SDL_GetError()); return 1; }
    a.g_.init(a.W, a.H);
    const std::string R = fontPath("AtkinsonHyperlegible-Regular.ttf"), B = fontPath("AtkinsonHyperlegible-Bold.ttf");
    a.f_.tiny.load(B, 13); a.f_.small.load(R, 16); a.f_.body.load(R, 19); a.f_.bodyB.load(B, 19); a.f_.btn.load(B, 21);

    PicUi& u = picUi();
    picStart(PK_WEFAX, 1809, 1);
    CHECK(u.live && u.live->kind == PK_WEFAX, "a live WEFAX chart");

    // ── ADJ opens on MARGIN, not editing ──
    decPicKeyPress(a, 61, "wefax");
    CHECK(u.adjOpen, "ADJ open");
    frame(); frame();
    CHECK(focus() == "dec.adj.m", "the highlight lands on MARGIN (%s)", focus().c_str());
    CHECK(u.adjEdit.empty(), "not editing as the row opens");

    // ── highlighted: RIGHT walks the row, nothing moves ──
    const double s0 = shift();
    press(D_RIGHT);
    CHECK(focus() == "dec.adj.s", "RIGHT from MARGIN reaches SLANT (%s)", focus().c_str());
    CHECK(shift() == s0 && !u.live->hasManual, "the margin did not move (%.1f)", shift());
    CHECK(!u.live->slantTouched, "the slant was not touched by passing through");
    press(D_RIGHT);
    CHECK(focus() == "dec.adj.auto", "RIGHT again reaches AUTO (%s)", focus().c_str());
    press(D_RIGHT);
    CHECK(focus() == "dec.adj.raw", "and RAW (%s)", focus().c_str());
    press(D_LEFT); press(D_LEFT); press(D_LEFT);
    CHECK(focus() == "dec.adj.m", "LEFT ×3 back to MARGIN (%s)", focus().c_str());
    CHECK(!u.live->hasManual && !u.live->slantTouched, "still nothing adjusted");

    // ── ENTER presses in: LEFT / RIGHT nudge, the highlight stays ──
    enter();
    CHECK(u.adjEdit == "dec.adj.m", "ENTER on MARGIN edits it (%s)", u.adjEdit.c_str());
    press(D_LEFT);
    CHECK(focus() == "dec.adj.m", "editing: LEFT stays on MARGIN (%s)", focus().c_str());
    CHECK(u.live->hasManual && shift() == s0 + 1, "editing: LEFT moved the chart 1 px (%.1f → %.1f)", s0, shift());
    press(D_RIGHT);
    CHECK(focus() == "dec.adj.m" && shift() == s0, "editing: RIGHT moved it back (%.1f)", shift());

    // ── ENTER finishes ──
    enter();
    CHECK(u.adjEdit.empty(), "ENTER again finishes");
    press(D_RIGHT);
    CHECK(focus() == "dec.adj.s" && shift() == s0, "after finishing RIGHT moves to SLANT again (%s)", focus().c_str());

    // ── BACK finishes and is consumed ──
    enter();
    CHECK(u.adjEdit == "dec.adj.s", "ENTER on SLANT edits it");
    back();
    CHECK(u.adjEdit.empty(), "BACK finishes the edit");
    CHECK(!gBackLeft, "BACK was taken by the edit — the box is not released by the same press");
    frame();
    back();
    CHECK(gBackLeft, "BACK with nothing being edited is left for the box (release)");

    // ── DOWN off an adjuster being edited ends the edit ──
    frame();
    a.nav_.setFocus("dec", "dec.adj.m"); frame();
    enter();
    CHECK(u.adjEdit == "dec.adj.m", "editing MARGIN");
    press(D_DOWN);
    CHECK(focus() == "dec.pic", "DOWN leaves the row (%s)", focus().c_str());
    frame();
    CHECK(u.adjEdit.empty(), "…and the edit is over");

    // ── closing ADJ ends it ──
    a.nav_.setFocus("dec", "dec.adj.m"); frame();
    enter();
    CHECK(u.adjEdit == "dec.adj.m", "editing MARGIN again");
    decPicKeyPress(a, 61, "wefax");
    CHECK(!u.adjOpen && u.adjEdit.empty(), "ADJ closed: no edit left behind");

    std::printf("wefax_adj_test: %d / %d checks pass\n", checks - fails, checks);
    return fails ? 1 : 0;
}
