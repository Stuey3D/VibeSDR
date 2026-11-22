// test-sat-labels — beta91 SATELLITE LABELS ON THE WATERFALL (vibe_sat_engine.h skyLabels / skyLabelsJson,
// docs/v12/SAT-LABELS.md). Synthetic only: the curated birds' element sets are the 2026-11-19 CelesTrak fixtures (through
// the mirror), the ORBCOMM birds are a SYNTHETIC fixture (plausible orbits — testdata-sat/celestrak-orbcomm-SYNTHETIC-*),
// built into sats.json by the directory mirror's own code (sats-orbcomm-SYNTHETIC-2026-11-22.json).
//
// §1 sats.json with label-only birds: they parse into their own set; never in the list, the picker, the reach gate or
//    a Doppler lock; a validator (no label set) accepts the file; an all-label file is refused.
// §2 the ISS overhead (São Paulo, the reference pass): its downlinks named at the Doppler-shifted frequency, equal to the
//    lock's model; only inside the window; the rate's sign; gone after LOS; the 4-s cache; no location / no time = none.
// §3 ORBCOMM: a label-only bird rises — named, flagged, NFM, its own SatNOGS channel or the shifted band.
// §4 the wire message: parses back, the flags, bounded size, the cap (highest first).
// §5 cost: a refresh of every bird.
#include "vibe_sat_engine.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace vibe::sat;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) { ++g_pass; } else { ++g_fail; std::printf("   FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::string readFile(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

static const double kSaoLat = -23.5, kSaoLon = -46.6, kIoLat = 52.270833, kIoLon = -0.875;
static const double kIssAos = 1795609400.724, kIssTca = 1795609729.166, kIssLos = 1795610051.907;   // reference-passes.txt
static const char* kFix = "/sats-orbcomm-SYNTHETIC-2026-11-22.json";

static std::unique_ptr<Engine> mk(const std::string& dir, double lat, double lon) {
    std::unique_ptr<Engine> e(new Engine());
    std::string err;
    const int n = e->loadSats(readFile(dir + kFix), &err);
    if (n < 0) std::printf("   loadSats refused: %s\n", err.c_str());
    e->setObserver(lat, lon);
    return e;
}
static const SkyLabel* find(const std::vector<SkyLabel>& v, long long norad, const char* tx) {
    for (auto& L : v) if (L.norad == norad && L.tx == tx) return &L;
    return nullptr;
}

static void s1(const std::string& dir) {
    std::printf("§1 sats.json with label-only birds\n");
    const std::string js = readFile(dir + kFix);
    std::vector<SatDef> d; std::vector<Elements> el; std::vector<bool> have; SatsMeta meta; std::string err; LabelSet ls;
    CHECK(parseSats(js, d, el, have, meta, err, &ls), "the fixture parses (%s)", err.c_str());
    CHECK(d.size() == curatedCatalogue().size(), "the list is the curated birds alone (%zu)", d.size());
    CHECK(ls.defs.size() == 8, "8 ORBCOMM birds in the label set (%zu)", ls.defs.size());
    bool allHave = true, allLabel = true;
    for (size_t i = 0; i < ls.defs.size(); ++i) {
        allHave = allHave && ls.have[i];
        for (auto& t : ls.defs[i].tx) allLabel = allLabel && t.labelOnly() && t.loop == "none" && t.consumer.empty();
    }
    CHECK(allHave && allLabel, "each has elements and only 'label' downlinks (no loop, no consumer)");
    bool fm108 = false;
    for (auto& s : ls.defs) if (s.norad == 41179 && s.tx.size() == 1 && s.tx[0].hz == 137713000 && !s.tx[0].passband()) fm108 = true;
    CHECK(fm108, "FM108 on its own SatNOGS channel, 137.713 MHz");
    std::vector<SatDef> d2; std::vector<Elements> e2; std::vector<bool> h2; SatsMeta m2; std::string err2;
    CHECK(parseSats(js, d2, e2, h2, m2, err2) && d2.size() == d.size(), "a validator (no label set) accepts the same file");
    // a label downlink on an ordinary bird is dropped; a file of label birds only is refused (nothing to offer)
    const std::string mixed = R"({"format":1,"satellites":[{"id":"X-1","name":"X","norad":99001,"transmitters":[
        {"label":"FM","downlink":145900000,"mode":"nfm","loop":"fll"},{"label":"L","downlink":137500000,"mode":"label","loop":"none"}]}]})";
    CHECK(parseSats(mixed, d2, e2, h2, m2, err2, &ls) && d2.size() == 1 && d2[0].tx.size() == 1 && ls.defs.empty(), "a stray 'label' downlink on an ordinary bird is dropped");
    const std::string only = R"({"format":1,"satellites":[{"id":"O-1","name":"O","norad":99002,"labelOnly":true,"transmitters":[
        {"label":"L","downlink":137500000,"mode":"label","loop":"none"}]}]})";
    CHECK(!parseSats(only, d2, e2, h2, m2, err2, &ls), "a file of label-only birds is refused (%s)", err2.c_str());
    const std::string flagged = R"({"format":1,"satellites":[{"id":"A-1","name":"A","norad":99003,"transmitters":[{"label":"FM","downlink":145900000,"mode":"nfm","loop":"fll"}]},
        {"id":"F-1","name":"F","norad":99004,"labelOnly":true,"transmitters":[{"label":"FM","downlink":137500000,"mode":"nfm","loop":"fll"},{"label":"L","downlink":137600000,"mode":"label","loop":"none"}]}]})";
    CHECK(parseSats(flagged, d2, e2, h2, m2, err2, &ls) && d2.size() == 1 && ls.defs.size() == 1 && ls.defs[0].tx.size() == 1 && ls.defs[0].tx[0].hz == 137600000,
          "\"labelOnly\":true keeps only its label downlinks");

    auto e = mk(dir, kIoLat, kIoLon);
    int withEl = 0;
    CHECK(e->labelCount(&withEl) == 8 && withEl == 8, "the engine holds them apart (%d, %d with elements)", e->labelCount(), withEl);
    const std::string list = e->listJson(Clock{kIssTca, true}, 24e6, 1766e6);
    CHECK(list.find("ORBCOMM") == std::string::npos, "never in the SATELLITE picker's sat_list");
    CHECK(!e->reaches(137.15e6, 137.85e6), "the reach gate ignores them (137.15–137.85 MHz reaches nothing listed)");
    const Track t = e->start("k", "o", 137.5e6, "nfm", Clock{kIssTca, true});
    CHECK(t.state == LockState::Off, "Doppler at 137.5 MHz locks nothing (%s)", t.why.c_str());
    for (auto& sd : e->defs()) CHECK(sd.group != "orbcomm", "defs() never carries %s", sd.id.c_str());
}

static void s2(const std::string& dir) {
    std::printf("§2 the ISS overhead (São Paulo, 56.5°)\n");
    auto e = mk(dir, kSaoLat, kSaoLon);
    const double t = kIssTca - 60;
    auto v = e->skyLabels(Clock{t, true}, 437.0e6, 438.5e6);
    const SkyLabel* fm = find(v, 25544, "FM repeater");
    double dop = 0, rate = 0;
    e->dopplerFor(25544, 437.8e6, t, dop, rate);
    CHECK(fm && std::fabs(fm->hz - (437.8e6 + dop)) < 1.0, "the FM repeater at 437.8 MHz + Doppler (%.0f Hz, the lock's model %.0f)", fm ? fm->hz - 437.8e6 : 0.0, dop);
    CHECK(fm && std::fabs(fm->hz - 437.8e6) > 1000 && std::fabs(fm->hz - 437.8e6) < 11000, "a real shift, within the 70 cm range (%.0f Hz)", fm ? fm->hz - 437.8e6 : 0.0);
    CHECK(fm && fm->rateHzS < 0 && std::fabs(fm->rateHzS - rate) < 5.0, "falling near TCA, the lock's rate (%.1f vs %.1f Hz/s)", fm ? fm->rateHzS : 0.0, rate);
    CHECK(fm && fm->mode == "nfm" && !fm->labelOnly && fm->name == "ISS" && fm->id == "ISS" && fm->elDeg > 20, "named ISS, NFM, not label-only, well up");
    CHECK(find(v, 25544, "SSTV events") && !find(v, 25544, "Voice / SSTV"), "437.55 named too; 145.8 is outside this window");
    auto v2 = e->skyLabels(Clock{t, true}, 145.7e6, 145.9e6);
    CHECK(find(v2, 25544, "Voice / SSTV") && find(v2, 25544, "APRS") && !find(v2, 25544, "FM repeater"), "a 2 m window: voice and APRS, not the 70 cm repeater");
    const SkyLabel* aprs = find(v2, 25544, "APRS");
    CHECK(aprs && std::fabs((aprs->hz - 145.825e6) / (fm->hz - 437.8e6) - 145.825 / 437.8) < 1e-3, "Doppler scales with frequency (2 m shift = 70 cm × 145.825/437.8)");
    // the cache: within 4 s a call only filters; after it, the sky moves
    auto c1 = e->skyLabels(Clock{t + 2, true}, 437.0e6, 438.5e6);
    CHECK(find(c1, 25544, "FM repeater") && find(c1, 25544, "FM repeater")->hz == fm->hz, "2 s later: the same look (the 4 s cache)");
    auto c2 = e->skyLabels(Clock{t + 4.5, true}, 437.0e6, 438.5e6);
    const SkyLabel* f2 = find(c2, 25544, "FM repeater");
    CHECK(f2 && f2->hz < fm->hz && std::fabs((f2->hz - fm->hz) - fm->rateHzS * 4.5) < 30, "4.5 s later: moved by the rate (%.0f Hz, rate × 4.5 = %.0f)", f2 ? f2->hz - fm->hz : 0.0, fm->rateHzS * 4.5);
    CHECK(e->skyLabels(Clock{kIssLos + 60, true}, 437.0e6, 438.5e6).empty() || !find(e->skyLabels(Clock{kIssLos + 60, true}, 437.0e6, 438.5e6), 25544, "FM repeater"),
          "a minute after LOS: gone");
    CHECK(!find(e->skyLabels(Clock{kIssAos - 120, true}, 437.0e6, 438.5e6), 25544, "FM repeater"), "two minutes before AOS: not yet");
    Engine none;
    none.loadSats(readFile(dir + kFix));
    CHECK(none.skyLabels(Clock{t, true}, 437e6, 438.5e6).empty(), "no location: nothing");
    CHECK(e->skyLabels(Clock{t + 400 * 86400.0, false}, 437e6, 438.5e6).empty(), "NO TIME (an unmeasured clock far from the elements): nothing");
    CHECK(e->skyLabels(Clock{t, true}, 438e6, 437e6).empty(), "an empty window: nothing");
}

static void s3(const std::string& dir) {
    std::printf("§3 ORBCOMM rises (label-only)\n");
    auto e = mk(dir, kIoLat, kIoLon);
    const SkyLabel* seen = nullptr; std::vector<SkyLabel> v; double at = 0; int maxUp = 0;
    bool fm108 = false, band = false, fm108ok = false, bandok = false;
    for (double t = kIssTca - 6 * 3600; t < kIssTca + 30 * 3600; t += 30) {
        v = e->skyLabels(Clock{t, true}, 137.0e6, 138.0e6);
        int up = 0;
        for (auto& L : v) {
            if (!L.labelOnly) continue;
            ++up;
            if (!seen) { at = t; }
            seen = &L;
            if (L.norad == 41179) { fm108 = true; fm108ok = L.loHz == 0 && std::fabs(L.hz - 137.713e6) < 4000 && L.mode == "nfm" && L.tx == "ORBCOMM"; }
            else if (L.hiHz > L.loHz) {
                band = true;
                bandok = std::fabs(L.loHz - 137.2e6) < 4000 && std::fabs(L.hiHz - 137.8e6) < 4000 && std::fabs(L.hz - 137.5e6) < 4000
                         && L.name.rfind("ORBCOMM FM", 0) == 0;
            }
        }
        maxUp = std::max(maxUp, up);
    }
    CHECK(at > 0, "an ORBCOMM bird rises over IO92 in 36 h of the synthetic set");
    CHECK(fm108 && fm108ok, "FM108: on its own channel 137.713 MHz ± Doppler, named 'ORBCOMM', NFM");
    CHECK(band && bandok, "the others over the whole band, shifted (137.2–137.8 MHz ± Doppler)");
    std::printf("   up to %d ORBCOMM birds at once\n", maxUp);
    bool none = true;
    for (double t = kIssTca - 6 * 3600; t < kIssTca + 6 * 3600; t += 300)
        for (auto& L : e->skyLabels(Clock{t, true}, 144e6, 146e6)) if (L.labelOnly) none = false;
    CHECK(none, "never named outside 137 MHz");
}

static void s4(const std::string& dir) {
    std::printf("§4 the wire message\n");
    auto e = mk(dir, kSaoLat, kSaoLon);
    auto v = e->skyLabels(Clock{kIssTca, true}, 24e6, 1766e6);
    const std::string j = Engine::skyLabelsJson(v, kIssTca);
    json::Value doc;
    CHECK(json::parseValue(j, doc) && doc.str("type") == "sat_labels" && doc.num("v") == 1 && doc.num("t") == std::round(kIssTca), "parses: type, v, t");
    const json::Value* sats = doc.get("sats");
    CHECK(sats && sats->t == json::Value::Arr && sats->a.size() == v.size() && !v.empty(), "one entry per label (%zu)", v.size());
    bool shape = sats && !sats->a.empty();
    if (shape) for (auto& x : sats->a) shape = shape && !x.str("id").empty() && !x.str("n").empty() && x.num("hz") > 1e6 && !x.str("m").empty() && x.num("norad") > 0;
    CHECK(shape, "each: id, name, catalogue number, hz, mode");
    CHECK(j.size() < 6000, "small (%zu bytes for %zu labels)", j.size(), v.size());
    CHECK(Engine::skyLabelsJson({}, 5) == "{\"type\":\"sat_labels\",\"v\":1,\"t\":5,\"sats\":[]}", "the empty message");
    auto few = e->skyLabels(Clock{kIssTca, true}, 24e6, 1766e6, 2);
    CHECK(few.size() <= 2 && (few.size() < 2 || few[0].elDeg >= few[1].elDeg), "the cap keeps the highest birds");
    SkyLabel b; b.id = "ORBCOMM-FM108"; b.name = "ORBCOMM FM108"; b.tx = "ORBCOMM"; b.norad = 41179; b.hz = 137.7e6; b.loHz = 137.2e6; b.hiHz = 137.8e6; b.mode = "nfm"; b.labelOnly = true;
    const std::string jb = Engine::skyLabelsJson({b}, 1);
    CHECK(jb.find("\"b\":true") != std::string::npos && jb.find("\"lo\":137200000") != std::string::npos && jb.find("\"hi\":137800000") != std::string::npos, "label-only flag and a band's edges");
}

static void s5(const std::string& dir) {
    std::printf("§5 cost\n");
    auto e = mk(dir, kIoLat, kIoLon);
    const auto t0 = std::chrono::steady_clock::now();
    const int n = 500;
    size_t total = 0;
    for (int i = 0; i < n; ++i) total += e->skyLabels(Clock{kIssTca + i * 5.0, true}, 24e6, 1766e6).size();   // ★ every call refreshes
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / n;
    std::printf("   %.1f µs a refresh for %zu birds (%zu labels over %d refreshes)\n", us, e->defs().size() + (size_t)e->labelCount(), total, n);
    CHECK(us < 2000, "a refresh of every bird well under 2 ms here (%.1f µs) — once in 4 s per radio", us);
}

int main(int argc, char** argv) {
    std::string dir = "vibeserver/testdata-sat";
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--data" && i + 1 < argc) dir = argv[++i];
    if (readFile(dir + kFix).empty()) { std::printf("data missing\n"); return 3; }
    s1(dir); s2(dir); s3(dir); s4(dir); s5(dir);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
