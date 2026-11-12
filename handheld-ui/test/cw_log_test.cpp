// cw_log_test.cpp — THE CW TEXT BOX'S READING (Stuart, 2026-11-12: "the messages should auto scroll like RTTY does, and
// then selected the user should be able to scroll back to see the decoded history"), through the box's own code
// (dec_box.h decDrawLog over dec_state.h DecLog) offscreen, with the real fonts and the real D-pad Nav:
//   • following: CW text (a stream with no line ends, as vibe_cw_run.h sends it) keeps the newest row on the foot;
//   • holding: scrolled back with UP, the rows on screen do not move while text keeps arriving — RAW and EXPLAIN;
//   • EXPLAIN ↔ RAW while scrolled back stays on the same decoded line; ENTER on the text follows again; CLR resets;
//   • a SIGNAL's history joined from the server's rolling 120-character windows (decHistMerge) reads back whole.
// Silent (no audio). Run in the arm64 test image: handheld-ui/test/run_cw_log_test.sh
#include "../src/app.h"

#include <cstdio>
#include <string>

using namespace hh;

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } } while (0)

static App* gA;
static const Rect kBody{10, 190, 610, 112};   // the box's body (5 rows of the 19 px font)

/** One frame of the box's text pane: `in` is that frame's D-pad. */
static void frame(DecLog* L = nullptr, Input in = {}) {
    App& a = *gA;
    a.nav_.begin("dec", in);
    Ctx c{a.g_, a.tc_, a.f_, a.nav_, nowSec()};
    c.nav.layer("dec");
    decDrawLog(a, c, kBody, "idle", L);
    a.nav_.end();
    a.g_.flush();
}
static int viewRows() { return std::max(1, (int)std::floor(kBody.h / gA->f_.body.lineHeight())); }
/** The rows on screen, top to bottom (decDrawLog's own sum). */
static std::vector<std::string> shown(DecLog& L) {
    const int n = viewRows(), rows = (int)L.rows();
    const int first = std::max(0, rows - n - L.back);
    std::vector<std::string> v;
    for (int k = first; k < std::min(rows, first + n); k++) v.push_back(L.row((size_t)k));
    return v;
}
static std::string join(const std::vector<std::string>& v) { std::string s; for (auto& x : v) s += x + " | "; return s; }

/** CW as it arrives: line n of a QSO, no line ends, a BT (=) between overs, a marker (G4A<n>) in each. */
static std::string qso(int n) {
    return "CQ CQ CQ DE G4A" + std::to_string(n) + " G4A" + std::to_string(n) + " PSE K = R R TNX FER CALL UR RST 599 599 NAME IS "
           "STU STU QTH NR LONDON HW CPY? BK = ";
}
/** Feed `s` in 7-character frames (a few characters every 250 ms on the air), one box frame each. */
static void feed(DecLog& L, const std::string& s, DecLog* box) {
    for (size_t i = 0; i < s.size(); i += 7) { L.push(s.substr(i, 7), false); frame(box); }
}

static void testMode(bool explain) {
    App& a = *gA;
    DecUi& u = decUi();
    const char* tag = explain ? "EXPLAIN" : "RAW";
    a.set_.cwPlain = explain ? 1 : 0;
    u.active = "cw";
    u.log.clear();
    a.nav_.forget("dec");
    frame();
    // ── following ──
    for (int n = 0; n < 30; n++) {
        feed(u.log, qso(n), nullptr);
        const auto v = shown(u.log);
        CHECK(u.log.back == 0, "%s follow: back %d after line %d", tag, u.log.back, n);
        CHECK(!v.empty() && v.back() == u.log.row(u.log.rows() - 1), "%s follow: the newest row is not on the foot (%d)", tag, n);
    }
    CHECK(u.log.rows() > 40, "%s: CW text was cut into lines and wrapped (%zu rows)", tag, u.log.rows());
    // ── scrolled back with the D-pad: UP twice from the text ──
    a.nav_.setFocus("dec", "dec.log");
    frame();
    for (int k = 0; k < 4; k++) { Input in; in.nav = D_UP; frame(nullptr, in); frame(); }
    CHECK(u.log.back >= 4, "%s: UP scrolled back (back %d)", tag, u.log.back);
    const auto held = shown(u.log);
    // ── text keeps arriving: what is on screen holds still ──
    for (int n = 30; n < 45; n++) {
        feed(u.log, qso(n), nullptr);
        CHECK(shown(u.log) == held, "%s hold: the view moved at line %d\n  was %s\n  now %s", tag, n, join(held).c_str(), join(shown(u.log)).c_str());
    }
    // ── a DOWN press comes forward ──
    { const int b = u.log.back; Input in; in.nav = D_DOWN; frame(nullptr, in); frame(); CHECK(u.log.back == b - 2, "%s: DOWN came forward 2 (%d → %d)", tag, b, u.log.back); }
    // ── ENTER on the text: back to the newest, following again ──
    { Input in; in.enter = true; frame(nullptr, in); }
    CHECK(u.log.back == 0, "%s: ENTER returned to the newest (back %d)", tag, u.log.back);
    feed(u.log, qso(45), nullptr);
    CHECK(u.log.back == 0 && shown(u.log).back() == u.log.row(u.log.rows() - 1), "%s: following again after ENTER", tag);
}

static void testSwitch() {
    App& a = *gA;
    DecUi& u = decUi();
    a.set_.cwPlain = 0; u.active = "cw"; u.log.clear(); a.nav_.forget("dec"); frame();
    for (int n = 0; n < 40; n++) feed(u.log, qso(n), nullptr);
    u.log.back = 30; frame();
    auto marker = [&](const std::vector<std::string>& v) {
        for (auto& r : v) { const size_t p = r.find("G4A"); if (p != std::string::npos) return std::atoi(r.c_str() + p + 3); }
        return -1;
    };
    // the line at the top of the view, by its marker (the first row with one)
    const int before = marker(shown(u.log));
    CHECK(before >= 0, "switch: a marker on screen before (%s)", join(shown(u.log)).c_str());
    a.set_.cwPlain = 1; frame();
    const int mid = marker(shown(u.log));
    CHECK(u.log.back > 0, "switch RAW → EXPLAIN: still scrolled back (back %d)", u.log.back);
    CHECK(mid >= 0 && std::abs(mid - before) <= 1, "switch RAW → EXPLAIN: line G4A%d → G4A%d (%s)", before, mid, join(shown(u.log)).c_str());
    a.set_.cwPlain = 0; frame();
    const int after = marker(shown(u.log));
    CHECK(after >= 0 && std::abs(after - before) <= 1, "switch back to RAW: line G4A%d → G4A%d", before, after);
    // following stays following across a switch
    u.log.back = 0; frame(); a.set_.cwPlain = 1; frame();
    CHECK(u.log.back == 0, "switch while following: still following (back %d)", u.log.back);
    // ── CLR: empty, and following ──
    u.log.back = 5; frame();
    u.log.clear(); frame();
    CHECK(u.log.back == 0 && u.log.rows() == 0, "CLR: empty and following (back %d rows %zu)", u.log.back, u.log.rows());
    feed(u.log, qso(99), nullptr);
    CHECK(u.log.back == 0 && u.log.rows() > 0, "after CLR: following new text");
}

static void testHistory() {
    // the server's rolling window (vibe_cw_run.h: the last 120 characters of each signal), sent as it grows
    std::string full;
    for (int n = 0; n < 12; n++) full += qso(n);
    std::string hist, joined;
    for (size_t end = 3; end <= full.size(); end += 5) {
        const std::string win = full.substr(end > 120 ? end - 120 : 0, std::min<size_t>(end, 120));
        joined += decHistMerge(hist, win);
    }
    if (full.size() % 5) joined += decHistMerge(hist, full.substr(full.size() - 120));
    CHECK(hist == full, "history: joined windows read back whole (%zu of %zu)", hist.size(), full.size());
    CHECK(joined == full, "history: the deltas add up to the whole");
    // a window that leapt further than it holds: a gap, never repeated text
    std::string h2 = "ABC DEF GHI JKL MNO";
    decHistMerge(h2, "XYZ XYZ XYZ XYZ QRS");
    CHECK(h2 == "ABC DEF GHI JKL MNO … XYZ XYZ XYZ XYZ QRS", "history: a gap is marked (%s)", h2.c_str());
    // the same window again adds nothing
    std::string h3 = full; CHECK(decHistMerge(h3, full.substr(full.size() - 120)).empty() && h3 == full, "history: an unchanged window adds nothing");

    // ── the reader: one signal's history, following, then held while its windows keep coming ──
    App& a = *gA;
    DecUi& u = decUi();
    a.set_.cwPlain = 0; u.active = "cw"; u.view = "cw";
    u.sigHist.clear(); u.sigs.clear();
    DecSig g; g.id = 7; g.hz = 7012300; g.q = "high"; g.active = true;
    auto list = [&](size_t end) {
        g.text = full.substr(end > 120 ? end - 120 : 0, std::min<size_t>(end, 120));
        u.sigs = {g};
        decCwHistories();
    };
    size_t end = 0;
    for (; end < full.size() / 2; end += 9) list(end);
    decReadSig(7);
    a.nav_.forget("dec"); frame(&u.sigLog);
    CHECK(u.sigLog.rows() > 6, "reader: opens on the history so far (%zu rows)", u.sigLog.rows());
    for (; end < full.size() * 3 / 4; end += 9) { list(end); frame(&u.sigLog); CHECK(u.sigLog.back == 0, "reader: follows"); }
    a.nav_.setFocus("dec", "dec.log"); frame(&u.sigLog);
    for (int k = 0; k < 3; k++) { Input in; in.nav = D_UP; frame(&u.sigLog, in); frame(&u.sigLog); }
    const auto held = shown(u.sigLog);
    CHECK(u.sigLog.back > 0, "reader: UP scrolled back (back %d)", u.sigLog.back);
    for (; end <= full.size(); end += 9) { list(end); frame(&u.sigLog); }
    CHECK(shown(u.sigLog) == held, "reader: held while text arrived\n  was %s\n  now %s", join(held).c_str(), join(shown(u.sigLog)).c_str());
    decReadSig(0);
}

int main() {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0 || TTF_Init() != 0) { std::fprintf(stderr, "init failed\n"); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    static App a;
    gA = &a;
    a.win_ = SDL_CreateWindow("cw", 0, 0, a.W, a.H, SDL_WINDOW_OPENGL);
    SDL_GLContext ctx = a.win_ ? SDL_GL_CreateContext(a.win_) : nullptr;
    if (!ctx) { std::fprintf(stderr, "GL: %s\n", SDL_GetError()); return 1; }
    a.g_.init(a.W, a.H);
    const std::string R = fontPath("AtkinsonHyperlegible-Regular.ttf"), B = fontPath("AtkinsonHyperlegible-Bold.ttf");
    a.f_.tiny.load(B, 13); a.f_.small.load(R, 16); a.f_.body.load(R, 19); a.f_.bodyB.load(B, 19);
    testMode(false);
    testMode(true);
    testSwitch();
    testHistory();
    std::printf("cw_log_test: %d / %d checks pass\n", checks - fails, checks);
    return fails ? 1 : 0;
}
