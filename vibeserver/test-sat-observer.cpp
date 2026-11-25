// test-sat-observer — ★★ beta94 LOCAL HARDWARE's SATELLITE POSITION (tester Mac Ha, Stuart 11-25): the observer's two
// sources (vibe_sat_observer.h) applied to the real engine (vibe_sat_engine.h), as LocalSdrShim does.
//
// §1 precedence: the phone's position fills in with no owner location; the owner's wins and is never replaced by it;
//    CLEAR clears only the phone's own; the owner turning location off hands back to the phone's.
// §2 the engine: no observer → no sky ("located":false, no pass); the phone's position + orbit data → "located":true and
//    the ISS pass Skyfield found for São Paulo; CLEAR → "no location" again.
// §3 nothing published: the list carries `located` only — never the observer's latitude / longitude.
#include "vibe_sat_engine.h"
#include "vibe_sat_observer.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace vibe::sat;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) { ++g_pass; } else { ++g_fail; std::printf("   FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
static std::string readFile(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

static const double kSaoLat = -23.5, kSaoLon = -46.6, kIoLat = 52.270833, kIoLon = -0.875;
static const double kIssAos = 1795609400.724, kIssTca = 1795609729.166;   // São Paulo, reference-passes.txt

static void apply(Engine& e, const ObserverSource::Apply& a) {
    if (a.kind == ObserverSource::Apply::Set) e.setObserver(a.lat, a.lon);
    else if (a.kind == ObserverSource::Apply::Clear) e.clearObserver();
}
static bool near(const Observer& o, double la, double lo) { return std::fabs(o.latDeg - la) < 1e-9 && std::fabs(o.lonDeg - lo) < 1e-9; }

static void s1() {
    std::printf("§1 precedence\n");
    Engine e; ObserverSource src; Observer o;
    CHECK(!e.haveObserver(), "a new engine has no observer");
    apply(e, src.device(true, kSaoLat, kSaoLon));
    CHECK(e.observer(o) && near(o, kSaoLat, kSaoLon) && src.deviceInUse(), "no owner location: the phone's position is the observer");
    apply(e, src.owner(true, kIoLat, kIoLon));
    CHECK(e.observer(o) && near(o, kIoLat, kIoLon) && !src.deviceInUse(), "the owner's location arrives: it wins");
    apply(e, src.device(true, 10.0, 10.0));
    CHECK(e.observer(o) && near(o, kIoLat, kIoLon), "a new phone position never replaces the owner's");
    apply(e, src.device(false, 0, 0));
    CHECK(e.observer(o) && near(o, kIoLat, kIoLon), "CLEAR on the phone leaves the owner's location alone");
    apply(e, src.device(true, kSaoLat, kSaoLon));
    apply(e, src.owner(false, 0, 0));
    CHECK(e.observer(o) && near(o, kSaoLat, kSaoLon) && src.deviceInUse(), "the owner turns location off: the phone's fills in");
    apply(e, src.device(false, 0, 0));
    CHECK(!e.haveObserver(), "CLEAR with no owner location: no observer (\"no location\" again)");

    Engine e2; ObserverSource s2;
    apply(e2, s2.owner(true, kIoLat, kIoLon));
    apply(e2, s2.owner(false, 0, 0));
    CHECK(e2.observer(o) && near(o, kIoLat, kIoLon), "an owner turning location off with no phone position keeps the sky (as before beta94)");
    apply(e2, s2.device(false, 0, 0));
    CHECK(e2.haveObserver(), "a phone CLEAR never clears a position the phone did not give");
}

static void s2(const std::string& dir) {
    std::printf("§2 the engine with the phone's position\n");
    Engine e; ObserverSource src;
    e.loadOmm(readFile(dir + "/celestrak-amateur-2026-11-19.json"));
    e.loadOmm(readFile(dir + "/celestrak-weather-2026-11-19.json"));
    CHECK(e.elementCount() > 0, "orbit data loaded (%d birds)", e.elementCount());
    const Clock clk{kIssTca, true};
    Pass p;
    CHECK(!e.passAt(25544, kIssTca, p), "no observer: no pass (the server says \"no location\")");
    CHECK(e.listJson(clk, 24e6, 1766e6).find("\"located\":false") != std::string::npos, "no observer: the list says located:false");
    apply(e, src.device(true, kSaoLat, kSaoLon));
    CHECK(e.passAt(25544, kIssTca, p) && std::fabs(p.aos - kIssAos) < 5.0, "the phone's position: the ISS pass Skyfield found (AOS %.1f s off)", std::fabs(p.aos - kIssAos));
    CHECK(e.listJson(clk, 24e6, 1766e6).find("\"located\":true") != std::string::npos, "the phone's position: the list says located:true");
    apply(e, src.device(false, 0, 0));
    CHECK(!e.passAt(25544, kIssTca, p) && !e.haveObserver(), "CLEAR: no pass, no observer");
}

static void s3(const std::string& dir) {
    std::printf("§3 the position is not published by the engine\n");
    Engine e; ObserverSource src;
    e.loadOmm(readFile(dir + "/celestrak-amateur-2026-11-19.json"));
    const double la = -23.4791667, lo = -46.625;   // a grid-square centre (GG66qm)
    apply(e, src.device(true, la, lo));
    const std::string j = e.listJson(Clock{kIssTca, true}, 24e6, 1766e6);
    CHECK(j.find("-23.479") == std::string::npos && j.find("-46.625") == std::string::npos && j.find("\"obs") == std::string::npos,
          "sat_list carries no observer latitude / longitude");
    CHECK(j.find("\"located\":true") != std::string::npos, "…only that a position is set");
}

int main(int argc, char** argv) {
    std::string dir = "vibeserver/testdata-sat";
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--data" && i + 1 < argc) dir = argv[++i];
    if (readFile(dir + "/celestrak-amateur-2026-11-19.json").empty()) { std::printf("data missing\n"); return 3; }
    s1(); s2(dir); s3(dir);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
