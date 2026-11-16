// paging_box_test.cpp — V12 PAGE on the R36S (paging.h, dec_state.h, dec_box.h), offscreen through the handheld's own code
// with the real fonts and the real D-pad Nav (2026-11-16):
//   • OFFERED ONLY WHERE THE ENGINE SAYS: a vibeserver.json without "paging" (every server) draws no PAGE key; with it and
//     its pagingModes, PAGE sits in VHF's PAGING group;
//   • THE SPEAKER: PAGE open → the deck / AUDIO glyph shows muted and the VTS line says so, while the person's own mute is
//     NOT touched (the engine plays the silence — vibe_paging_gate.h pagingSpeakerLevel); PAGE closed → glyph and line back
//     as they were, mute unchanged — from unmuted, from muted, and from APRS (which mutes for itself) straight to PAGE;
//   • THE LIST: the engine's lines (made up here) become rows — time, "FLEX 3200/4", the capcode, the function / phase, the
//     text, "cut short" — newest first, bounded at kPagingMax; ENTER on a row reads it whole, ENTER again is the list;
//     CLR empties it; a restart of PAGE starts empty;
//   • NOTHING ON DISK: the whole run, with HOME pointed at an empty folder, leaves no file but the UI's own device id.
// Silent (no audio). Run in the arm64 test image: handheld-ui/test/run_offscreen_tests.sh paging_box_test
#include "../src/app.h"

#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace hh;

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } } while (0)

static App* gA;
static RadioState gS;
static const Rect kBody{10, 190, 610, 112};

static void pane(Input in = {}) {
    App& a = *gA;
    a.nav_.begin("dec", in);
    Ctx c{a.g_, a.tc_, a.f_, a.nav_, nowSec()};
    c.nav.layer("dec");
    decPanePaging(a, c, gS, kBody);
    a.nav_.end();
    a.g_.flush();
}
static void enter() { Input in; in.enter = true; pane(in); pane(); }
static void down() { Input in; in.nav = D_DOWN; pane(in); pane(); }

static std::vector<std::string> listDir(const std::string& d) {
    std::vector<std::string> v;
    if (DIR* p = opendir(d.c_str())) { while (dirent* e = readdir(p)) { const std::string n = e->d_name; if (n != "." && n != "..") v.push_back(n); } closedir(p); }
    return v;
}
static std::string frame01(const std::string& t) { return decTextFrame(t); }
static void offer(bool paging, bool modes) {
    DecUi& u = decUi();
    std::lock_guard<std::mutex> lk(u.capMtx);
    u.caps["paging"] = paging;
    u.pagingModes.clear();
    if (modes) u.pagingModes = {{"auto", "AUTO", ""}, {"pocsag", "ANY", "POCSAG"}, {"pocsag512", "512", "POCSAG"}, {"pocsag1200", "1200", "POCSAG"},
                               {"pocsag2400", "2400", "POCSAG"}, {"flex", "ANY", "FLEX"}};
    u.capFetched = true; u.capAt = nowSec() + 1e6;
}
/** The deck glyph's fact, as audioTick sets it (sec_audio.h). */
static bool glyphMuted() { const aud::State& S = aud::st(); return S.mute || S.pagingSilent; }

int main() {
    // ★ NOTHING ON DISK: HOME and the working directory are empty folders for the whole run
    char tmpl[] = "/tmp/pgbox-XXXXXX";
    const std::string home = mkdtemp(tmpl);
    setenv("HOME", home.c_str(), 1);
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);   // ★ the GL driver's own cache is not ours to count
    if (chdir(home.c_str()) != 0) { std::fprintf(stderr, "chdir failed\n"); return 1; }

    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0 || TTF_Init() != 0) { std::fprintf(stderr, "init failed\n"); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    static App a;
    gA = &a;
    a.win_ = SDL_CreateWindow("paging", 0, 0, a.W, a.H, SDL_WINDOW_OPENGL);
    SDL_GLContext ctx = a.win_ ? SDL_GL_CreateContext(a.win_) : nullptr;
    if (!ctx) { std::fprintf(stderr, "GL: %s\n", SDL_GetError()); return 1; }
    a.g_.init(a.W, a.H);
    const std::string R = fontPath("AtkinsonHyperlegible-Regular.ttf"), B = fontPath("AtkinsonHyperlegible-Bold.ttf");
    a.f_.tiny.load(B, 13); a.f_.small.load(R, 16); a.f_.body.load(R, 19); a.f_.bodyB.load(B, 19); a.f_.btn.load(B, 21);
    static Engine eng(1);   // ★ never started: no socket is opened (port 1 answers nothing)
    a.eng_ = &eng;
    gS.session = "running"; gS.mode = "nfm"; gS.vfoHz = 153350000;
    DecUi& u = decUi();
    u.canned = true;        // ★ the decoder socket is never opened: frames go through the same parse path
    decTick(a, gS);         // the session's first tick (the offer's clock)

    // ── offered only where the engine says ──
    const DecDef* pd = decDef("paging");
    CHECK(pd && std::string(pd->label) == "PAGE" && pd->kind == DK_PAGING, "PAGE is in the table");
    offer(false, false);
    CHECK(!decOffered(gS, *pd), "a server's vibeserver.json (no \"paging\"): no PAGE key");
    offer(true, false);
    CHECK(!decOffered(gS, *pd), "\"paging\":true with no pagingModes: still no key (nothing to choose AUTO from)");
    offer(true, true);
    CHECK(decOffered(gS, *pd), "\"paging\":true + pagingModes (the handheld's own radio): PAGE offered");
    bool inGroup = false;
    for (auto& g : decPlaces(1)) if (std::string(g.group) == "PAGING") for (auto* id : g.ids) inGroup = inGroup || std::string(id) == "paging";
    CHECK(inGroup, "PAGE sits in VHF's PAGING group");
    for (auto& g : decPlaces(0)) for (auto* id : g.ids) CHECK(std::string(id) != "paging", "not on the HF tab");
    { gS.blockedModes = {"paging"}; CHECK(!decOffered(gS, *pd), "the owner's block hides it"); gS.blockedModes.clear(); }

    // ── the attach asks for the person's choice ──
    u.pagingMode = "auto";
    CHECK(decAttachJson(gS, "paging").find("\"extension_name\":\"paging\",\"mode\":\"auto\"") != std::string::npos, "attach: paging, AUTO");
    u.pagingMode = "pocsag1200";
    CHECK(decAttachJson(gS, "paging").find("\"mode\":\"pocsag1200\"") != std::string::npos, "attach: a manual rate");
    u.pagingMode = "auto";

    // ── the speaker, from unmuted ──
    aud::State& A = aud::st();
    A.mute = false; A.vol = 80;
    decStart(a, gS, "paging"); decTick(a, gS);
    CHECK(A.pagingSilent && glyphMuted(), "PAGE open: the glyph shows the speaker muted");
    CHECK(!A.mute && A.vol == 80, "…and the person's own mute and volume are untouched (the engine plays the silence)");
    CHECK(pagingVtsLine().find("PAGE \xc2\xb7 ") == 0 && pagingVtsLine().find("speaker muted") != std::string::npos, "the VTS line: %s", pagingVtsLine().c_str());
    decStop(a, "paging"); decTick(a, gS);
    CHECK(!A.pagingSilent && !glyphMuted() && !A.mute, "PAGE closed: the speaker as it was (on)");
    CHECK(pagingVtsLine().empty(), "…and the VTS line gone");
    // ── from muted ──
    A.mute = true;
    decStart(a, gS, "paging"); decTick(a, gS);
    CHECK(glyphMuted() && A.mute, "muted before PAGE: muted while it runs");
    decStop(a, "paging"); decTick(a, gS);
    CHECK(A.mute && !A.pagingSilent, "…and still muted after (it was the person's)");
    A.mute = false;
    // ── APRS (mutes for itself) straight to PAGE, then PAGE closed ──
    decStart(a, gS, "aprs"); decTick(a, gS);
    CHECK(A.mute, "APRS open: muted (its own rule)");
    decStart(a, gS, "paging"); decTick(a, gS);
    CHECK(!A.mute && A.pagingSilent && glyphMuted(), "APRS → PAGE: APRS's mute given back, PAGE's silence on");
    decStop(a, "paging"); decTick(a, gS);
    CHECK(!A.mute && !glyphMuted(), "PAGE closed: the speaker on again, as before APRS");

    // ── the list ──
    decStart(a, gS, "paging"); decTick(a, gS);
    CHECK(u.pages.empty(), "a fresh PAGE starts with an empty list");
    for (int i = 0; i < 30; i++) { u.cannedAt = 0; decCannedTick(a, gS); }
    CHECK(u.pages.size() == 6, "the six made-up pages: %zu", u.pages.size());
    if (u.pages.size() == 6) {
        const PageRow& n = u.pages.front();
        CHECK(n.proto == "FLEX 6400/4" && n.addr == "0345678" && n.fn == "B" && n.cut, "newest first, read back: %s · %s · %s", n.proto.c_str(), n.addr.c_str(), n.fn.c_str());
        CHECK(n.text == "A LONG MESSAGE THAT LOST ITS LOCK HALF WAY", "…its text without the cut-short tag: %s", n.text.c_str());
        const PageRow& o = u.pages.back();
        CHECK(o.proto == "FLEX 3200/4" && o.addr == "0123456" && o.fn == "A" && !o.cut, "the oldest last: %s", o.proto.c_str());
        CHECK(u.pages[1].text == "(tone only)" && u.pages[1].fn == "F1" && u.pages[1].proto == "POCSAG 2400", "a tone-only POCSAG page");
    }
    CHECK(u.status.find("FLEX 6400/4") == 0 && u.status.find("6 pages") != std::string::npos, "the status line: %s", u.status.c_str());
    // two lines in one frame are two rows
    decOnBinary(a, gS, frame01("[POCSAG 1200 \xc2\xb7 0000001 \xc2\xb7 F0] ONE\n[POCSAG 1200 \xc2\xb7 0000002 \xc2\xb7 F0] TWO\n"));
    CHECK(u.pages.size() == 8 && u.pages[0].text == "TWO" && u.pages[1].text == "ONE", "two lines in one frame: two rows, in order");
    // drawn; ENTER reads one whole, ENTER again is back to the list
    a.nav_.forget("dec");
    pane(); pane();
    CHECK(a.nav_.focus("dec").rfind("dec.pg.", 0) == 0, "the D-pad lands on a page row: %s", a.nav_.focus("dec").c_str());
    down();
    const std::string at = a.nav_.focus("dec");
    enter();
    CHECK(u.pageRead != 0 && a.nav_.focus("dec") == "dec.page.read", "ENTER on a row: that page, whole (%llu)", (unsigned long long)u.pageRead);
    enter();
    CHECK(u.pageRead == 0, "ENTER again: back to the list");
    CHECK(a.nav_.focus("dec") == at, "…on the row that was read (%s, was %s)", a.nav_.focus("dec").c_str(), at.c_str());
    // bounded
    for (int i = 0; i < 500; i++) decOnBinary(a, gS, frame01("[POCSAG 512 \xc2\xb7 0000003 \xc2\xb7 F0] FLOOD " + std::to_string(i) + "\n"));
    CHECK(u.pages.size() == kPagingMax, "bounded: %zu rows after 508 pages", u.pages.size());
    CHECK(u.pages.front().text == "FLOOD 499", "…the newest kept");
    // a line that is not the receiver's shape is still a row (nothing it says is lost), control characters made spaces
    decOnBinary(a, gS, frame01("odd \x07line\n"));
    CHECK(u.pages.front().proto.empty() && u.pages.front().text == "odd  line", "an odd line: a row of its own text: '%s'", u.pages.front().text.c_str());
    // ── the picker: PAGE's options under it — PROTOCOL (AUTO, POCSAG, FLEX), RATE only where POCSAG has a choice ──
    {
        u.pickBand = 1;
        auto rows = [&] { std::vector<DecPRow> v; decPickRows(a, gS, v); return v; };
        auto find = [](std::vector<DecPRow>& v, const std::string& id) -> DecPRow* { for (auto& r : v) if (r.id == id) return &r; return nullptr; };
        u.pagingMode = "auto";
        auto v = rows();
        DecPRow* t = find(v, "paging");
        DecPRow* pr = find(v, "paging.proto");
        CHECK(t && t->kind == DecPRow::TOGGLE && t->on && t->label == "PAGE", "the picker: PAGE, on");
        CHECK(pr && pr->opts.size() == 3 && pr->opts[0] == "AUTO" && pr->opts[1] == "POCSAG" && pr->opts[2] == "FLEX" && pr->sel == 0, "PROTOCOL: AUTO · POCSAG · FLEX, AUTO chosen");
        CHECK(!find(v, "paging.rate"), "AUTO: no RATE row");
        if (pr) pr->pick(1);
        CHECK(u.pagingMode == "pocsag", "PROTOCOL POCSAG: its own ANY (%s)", u.pagingMode.c_str());
        v = rows();
        DecPRow* rr = find(v, "paging.rate");
        CHECK(rr && rr->opts.size() == 4 && rr->opts[2] == "1200", "POCSAG: RATE ANY · 512 · 1200 · 2400");
        if (rr) rr->pick(2);
        CHECK(u.pagingMode == "pocsag1200" && u.reattachAt > 0, "RATE 1200: re-attached with pocsag1200");
        v = rows(); pr = find(v, "paging.proto");
        if (pr) pr->pick(2);
        v = rows();
        CHECK(u.pagingMode == "flex" && !find(v, "paging.rate"), "FLEX: no RATE row (a FLEX frame names its own speed)");
        bool hint = false; for (auto& r : v) if (r.kind == DecPRow::HINT && r.label.find("speaker is muted while PAGE runs") != std::string::npos) hint = true;
        CHECK(hint, "the hint says the speaker is muted and nothing is kept");
        u.pagingMode = "auto";
    }
    // ★ pictures for a person to look at (VIBE_HHUI_SHOTS=<dir>): the list, a page read whole
    if (const char* dir = std::getenv("VIBE_HHUI_SHOTS")) {
        auto shot = [&](const std::string& name) {
            std::vector<uint8_t> px((size_t)a.W * a.H * 4);
            glReadPixels(0, 0, a.W, a.H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            if (FILE* f = std::fopen((std::string(dir) + "/" + name).c_str(), "wb")) {
                std::fprintf(f, "P6\n%d %d\n255\n", a.W, a.H);
                for (int y = a.H - 1; y >= 0; y--) for (int x = 0; x < a.W; x++) std::fwrite(&px[((size_t)y * a.W + x) * 4], 1, 3, f);
                std::fclose(f);
            }
        };
        u.pages.clear();
        for (int i = 0; i < 30; i++) { u.cannedAt = 0; decCannedTick(a, gS); }
        u.cannedN = 4;
        for (int i = 0; i < 20; i++) { u.cannedAt = 0; decCannedTick(a, gS); }
        glClearColor(0.05f, 0.05f, 0.06f, 1); glClear(GL_COLOR_BUFFER_BIT);
        a.nav_.forget("dec"); pane(); pane(); shot("page-list.ppm");
        enter(); glClear(GL_COLOR_BUFFER_BIT); pane(); shot("page-read.ppm");
        enter();
    }
    // STOP and start again: empty
    decStop(a, "paging"); decTick(a, gS);
    decStart(a, gS, "paging"); decTick(a, gS);
    CHECK(u.pages.empty(), "PAGE started again: the list is empty (nothing was kept)");
    decStop(a, "paging"); decTick(a, gS);

    // ── nothing on disk ──
    const auto files = listDir(home);
    bool onlyBid = true;
    for (auto& f : files) if (f != ".vibe-hhui-bid") { onlyBid = false; std::fprintf(stderr, "  in HOME: %s\n", f.c_str()); }
    CHECK(onlyBid, "no file written but the UI's device id (%zu entries in HOME)", files.size());

    std::printf("paging_box_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
