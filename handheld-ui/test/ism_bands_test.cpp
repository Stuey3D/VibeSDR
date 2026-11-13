// ism_bands_test.cpp — THE ISM BOX'S BANDS LIST (Stuart, 2026-11-13: one BANDS key instead of the box's band arrows, "pops
// open the full list of ISM frequencies with their title and region … scrollable on smaller screens"), through the
// R36S's own code (map_modes.h mapIsmParseBands / mapIsmBandsPop / mapIsmPick) offscreen, with the real fonts and the
// real D-pad Nav:
//   • vibeserver.json's ismBands parse with their region (an older server's entry without one still parses), ≤ 64;
//   • the list opens on the band the server is on, which is lit and marked ON NOW;
//   • the D-pad walks it, the view scrolls to keep the highlight in sight (17 bands: more than fit);
//   • ENTER on a band queues exactly that band's id for the stepper's send (ismHop) and closes the list;
//   • ENTER on the band already playing sends nothing (and cancels a burst on its way); BACK leaves it alone.
// Silent (no audio). Run in the arm64 test image: handheld-ui/test/run_offscreen_tests.sh ism_bands_test
#include "../src/app.h"

#include <cstdio>
#include <string>

using namespace hh;

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } } while (0)

static App* gA;
static RadioState gS;

/** One frame of the pop-up: `in` is that frame's D-pad. Returns the pop-up's answer (false = it closed itself). */
static bool frame(Input in = {}) {
    App& a = *gA;
    a.nav_.begin("map", in);
    Ctx c{a.g_, a.tc_, a.f_, a.nav_, nowSec()};
    c.nav.layer("map");
    const bool open = mapIsmBandsPop(a, c, gS);
    a.nav_.end();
    a.g_.flush();
    return open;
}
static bool press(Dir d) { Input in; in.nav = d; const bool o = frame(in); frame(); return o; }
static bool enter() { Input in; in.enter = true; return frame(in); }

static const char* kJson = R"J({"ism":true,"ismBands":[
 {"id":"315","name":"315 MHz ISM","region":"USA, Canada, Japan","hz":315000000},
 {"id":"433","name":"433.92 MHz ISM","region":"Europe, UK, Africa, Australia, much of Asia","hz":433920000},
 {"id":"479","name":"479 MHz LoRa (China CN470)","region":"China","hz":479300000},
 {"id":"865","name":"865 MHz LoRa (India, NZ)","region":"India, New Zealand","hz":865600000},
 {"id":"867","name":"867 MHz LoRa (EU 867, KZ, NP)","region":"Europe, Kazakhstan, Nepal","hz":867200000},
 {"id":"868","name":"868 MHz SRD","region":"Europe, UK","hz":868600000},
 {"id":"904","name":"904.6 MHz LoRa (US FSB2, BR)","region":"USA, Brazil","hz":904600000},
 {"id":"907","name":"907 MHz ISM (Meshtastic US)","region":"USA, Canada, Mexico","hz":906600000},
 {"id":"910","name":"910 MHz ISM (MeshCore US)","region":"USA, Canada, Costa Rica","hz":910400000},
 {"id":"913","name":"913 MHz LoRa (Meshtastic US MF)","region":"USA","hz":913400000},
 {"id":"915","name":"915 MHz ISM","region":"The Americas, Australia","hz":915000000},
 {"id":"917","name":"917.4 MHz LoRa (AU915 FSB2, NZ)","region":"Australia, New Zealand, Singapore, Philippines","hz":917450000},
 {"id":"920","name":"920 MHz ISM (Meshtastic ANZ)","region":"Australia, New Zealand, Vietnam, Thailand","hz":920650000},
 {"id":"922","name":"921.7 MHz LoRa (AS923-2, KR)","region":"Korea, Indonesia, Vietnam, Thailand","hz":921700000},
 {"id":"923","name":"923 MHz ISM (MeshCore BR/AU, AS923)","region":"Brazil, Australia, Japan, Korea, Taiwan, SE Asia","hz":923300000},
 {"id":"925","name":"925 MHz LoRa (US/AU downlinks)","region":"USA, Australia","hz":925400000},
 {"id":"927","name":"927 MHz LoRa (MeshCore SoCal)","region":"USA (Southern California), Uzbekistan","hz":927050000}]})J";

static void openList(const char* serverBand) {
    MapModes& x = mapModes();
    x.canned = "ism";
    mapMode("ism")->canned = Json::parse(std::string("{\"band\":\"") + serverBand + "\",\"devices\":[]}");
    x.ismHop = -1; x.ismSendAt = 0;
    mapUi().popScroll = Scroller{};
    gA->nav_.forget("map");
    frame();   // first frame: focus on the current band
}

int main() {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0 || TTF_Init() != 0) { std::fprintf(stderr, "init failed\n"); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    static App a;
    gA = &a;
    a.win_ = SDL_CreateWindow("ism", 0, 0, a.W, a.H, SDL_WINDOW_OPENGL);
    SDL_GLContext ctx = a.win_ ? SDL_GL_CreateContext(a.win_) : nullptr;
    if (!ctx) { std::fprintf(stderr, "GL: %s\n", SDL_GetError()); return 1; }
    a.g_.init(a.W, a.H);
    const std::string R = fontPath("AtkinsonHyperlegible-Regular.ttf"), B = fontPath("AtkinsonHyperlegible-Bold.ttf");
    a.f_.tiny.load(B, 13); a.f_.small.load(R, 16); a.f_.body.load(R, 19); a.f_.bodyB.load(B, 19); a.f_.btn.load(B, 21);

    MapModes& x = mapModes();
    // ── the parse ──
    mapIsmParseBands(Json::parse(kJson), x.ismBands);
    CHECK(x.ismBands.size() == 17, "17 bands parsed (%zu)", x.ismBands.size());
    CHECK(x.ismBands[1].id == "433" && x.ismBands[1].name == "433.92 MHz ISM" && x.ismBands[1].region == "Europe, UK, Africa, Australia, much of Asia",
          "433: id, name, region (%s / %s)", x.ismBands[1].name.c_str(), x.ismBands[1].region.c_str());
    {   // an older server: no region — still a band; and the 64 bound
        std::vector<MapModes::IsmBand> v;
        mapIsmParseBands(Json::parse(R"({"ismBands":[{"id":"433","name":"433.92 MHz ISM","hz":433920000}]})"), v);
        CHECK(v.size() == 1 && v[0].region.empty(), "a band without a region parses, region empty");
        std::string big = "{\"ismBands\":[";
        for (int i = 0; i < 100; i++) big += std::string(i ? "," : "") + "{\"id\":\"b" + std::to_string(i) + "\",\"name\":\"B\",\"hz\":1}";
        mapIsmParseBands(Json::parse(big + "]}"), v);
        CHECK(v.size() == 64, "a runaway list stops at 64 (%zu)", v.size());
    }

    // ── opens on the server's band, in view ──
    openList("868");
    CHECK(a.nav_.focus("map") == "map.b.868", "opens on the band the server is on (%s)", a.nav_.focus("map").c_str());
    const float y0 = mapUi().popScroll.y;
    CHECK(y0 > 0, "868 is 6th of 17: the list opened scrolled to it (%.0f)", y0);

    // ── D-pad down to the end: the highlight walks, the view follows ──
    for (int k = 0; k < 11; k++) press(D_DOWN);
    CHECK(a.nav_.focus("map") == "map.b.927", "DOWN ×11 from 868 reaches 927 (%s)", a.nav_.focus("map").c_str());
    for (int k = 0; k < 8; k++) frame();   // (the scroll eases)
    CHECK(mapUi().popScroll.y > y0, "the view scrolled down with the highlight (%.0f > %.0f)", mapUi().popScroll.y, y0);
    for (int k = 0; k < 16; k++) press(D_UP);
    CHECK(a.nav_.focus("map") == "map.b.315", "UP to the top: 315 (%s)", a.nav_.focus("map").c_str());
    for (int k = 0; k < 8; k++) frame();
    CHECK(mapUi().popScroll.y < 1, "the view back at the top (%.0f)", mapUi().popScroll.y);

    // ── ENTER picks: exactly that band's id, queued for the stepper's send, and the list closes ──
    press(D_DOWN); press(D_DOWN);   // 479
    CHECK(a.nav_.focus("map") == "map.b.479", "on 479 (%s)", a.nav_.focus("map").c_str());
    const bool stillOpen = enter();
    CHECK(!stillOpen, "ENTER closes the list");
    CHECK(x.ismHop == mapIsmIndex(x, "479") && x.ismSendAt > 0 && x.ismBands[(size_t)x.ismHop].id == "479",
          "ENTER on 479 queues band 479 for the send (hop %d)", x.ismHop);

    // ── reopened during that burst: it opens on where the band is GOING ──
    a.nav_.forget("map"); frame();
    CHECK(a.nav_.focus("map") == "map.b.479", "reopened mid-hop: on 479, the band on its way (%s)", a.nav_.focus("map").c_str());

    // ── ENTER on the band already playing: nothing sent, the burst cancelled ──
    openList("868");
    x.ismHop = mapIsmIndex(x, "923"); x.ismSendAt = nowSec() + 10;   // a burst heading for 923
    a.nav_.setFocus("map", "map.b.868"); frame();
    enter();
    CHECK(x.ismHop == -1 && x.ismSendAt == 0, "ENTER on the band already on: nothing to send (hop %d)", x.ismHop);

    // ── a short screen: the list never runs off it ──
    {
        openList("433");
        const size_t shown = std::min<size_t>(x.ismBands.size(), (size_t)std::max(3.f, std::floor((480 - mapl::headH - 8 - 30 - 12 - 40) / 50.f)));
        CHECK(shown < x.ismBands.size(), "17 bands do not fit on the R36S's 480 lines: the list scrolls (%zu shown)", shown);
        CHECK(mapl::headH + 4 + 30 + shown * 50 + 12 <= 480 - 30, "the pop-up stays above the hint line");
    }

    std::printf("ism_bands_test: %d / %d checks pass\n", checks - fails, checks);
    return fails ? 1 : 0;
}
