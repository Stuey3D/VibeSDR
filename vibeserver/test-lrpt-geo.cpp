// test-lrpt-geo.cpp — METEOR LRPT's MAP OUTLINE (vibe_lrpt_geo.h, vibe_lrpt_overlay.h, docs/v12/LRPT-MAP-OVERLAY.md):
// the geometry on a real orbit (METEOR-M2-4's element set from vibeserver/testdata-sat), the fail-closed checks, the
// outline JSON, and the gallery's filing (the outline item, "geo" / "gw" on the pictures, the PNG untouched).
#include "vibe_lrpt_overlay.h"
#include "vibe_bgdecode.h"
#include "vibe_lrpt_run.h"
#include "vibe_lrpt_tx.h"
#include "vibe_lrpt_burn.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace vibelrpt;
using namespace vibelrpt::geo;
using namespace vibe;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::vector<Candidate> loadMeteors(const char* path) {
    std::vector<Candidate> out;
    std::ifstream f(path); std::stringstream ss; ss << f.rdbuf();
    vibe::sat::json::Value doc;
    if (!vibe::sat::json::parseValue(ss.str(), doc) || !doc.get("satellites")) return out;
    for (const auto& x : doc.get("satellites")->a) {
        const std::string id = x.str("id");
        if (id != "METEOR-M2-3" && id != "METEOR-M2-4") continue;
        Candidate c; std::string err;
        if (!x.get("omm") || !vibe::sat::fromOmm(x.get("omm")->flat(), c.el, err)) continue;
        c.name = x.str("name"); c.norad = c.el.norad; out.push_back(c);
    }
    return out;
}

static double dist(double la1, double lo1, double la2, double lo2) {   // km, great circle
    const double r = 6371.0, a = la1 * kDeg, b = la2 * kDeg, d = (lo2 - lo1) * kDeg;
    return r * std::acos(std::max(-1.0, std::min(1.0, std::sin(a) * std::sin(b) + std::cos(a) * std::cos(b) * std::cos(d))));
}

int main() {
    const auto cands = loadMeteors("vibeserver/testdata-sat/sats-2026-11-19.json");
    CHECK(cands.size() == 2, "two METEORs in the test sats.json (%zu)", cands.size());
    if (cands.size() != 2) { std::printf("%d passed, %d failed\n", g_pass, g_fail); return 1; }
    const Candidate* m24 = cands[0].norad == 59051 ? &cands[0] : &cands[1];
    const Candidate* m23 = m24 == &cands[0] ? &cands[1] : &cands[0];
    // ★ Stuart's 23 Nov 14:06 UTC pass (METEOR-M2-4 northbound over the UK); the 11-19 elements are 4 days old — fine
    //   for geometry, and the test's own age limit is checked separately below
    const double t0 = 1795442805.129;   // 23 Nov 2026 14:06:45.129 UTC
    const int rows = 800;
    std::vector<double> t(rows);
    for (int r = 0; r < rows; r++) t[r] = t0 + r * 1.23114 / 8.0;

    std::printf("── the geometry on METEOR-M2-4's orbit ──\n");
    Georef g;
    std::string why;
    CHECK(g.build(m24->el, t, why), "row table: %s", why.c_str());
    // the nadir column (between 783 and 784: angle 0) lands on the sub-satellite point (the engine's look())
    {
        vibe::sat::Sgp4 s(m24->el); vibe::sat::Observer ob(52.3125, -0.875, 0);
        double worst = 0;
        for (int r : { 0, 400, 799 }) {
            const vibe::sat::Look L = vibe::sat::look(s, ob, t[r]);
            double la, lo; g.toGround(g.ins.width / 2.0 - 0.5, r, la, lo);
            worst = std::max(worst, dist(la, lo, L.subLatDeg, L.subLonDeg));
        }
        CHECK(worst < 0.5, "the nadir column is the sub-satellite point (worst %.3f km)", worst);
    }
    // the swath: half-scan 55.2° from ~830 km → ~1440 km each side
    {
        double la0, lo0, la1, lo1, lac, loc;
        g.toGround(0, 400, la0, lo0); g.toGround(1567, 400, la1, lo1); g.toGround(783.5, 400, lac, loc);
        const double w = dist(la0, lo0, la1, lo1), e0 = dist(la0, lo0, lac, loc), e1 = dist(la1, lo1, lac, loc);
        CHECK(w > 2750 && w < 3000, "swath %.0f km", w);
        CHECK(std::fabs(e0 - e1) < 10, "symmetric about nadir (%.0f / %.0f km)", e0, e1);
        // ★ the scan direction: northbound, column 0 is EAST of the track (the picture is south-up)
        CHECK(lo0 > loc && lo1 < loc, "column 0 east (%.2f), column 1567 west (%.2f) of nadir %.2f", lo0, lo1, loc);
        // pixel size at nadir ≈ 1.05–1.15 km across, ~1.05 km along
        double lb, lob; g.toGround(784.5, 400, lb, lob);
        const double px = dist(lac, loc, lb, lob);
        double lr, lor; g.toGround(783.5, 401, lr, lor);
        const double py = dist(lac, loc, lr, lor);
        CHECK(px > 0.95 && px < 1.25, "pixel at nadir %.3f km across", px);
        CHECK(py > 0.95 && py < 1.15, "line spacing %.3f km", py);
    }
    // ★★ ROUND TRIP: pixel → ground → pixel, across the picture to its edges
    {
        double worst = 0; int n = 0;
        for (int r = 4; r < rows - 4; r += 97) for (double c = 2; c < 1566; c += 111) {
            double la, lo, c2, r2;
            if (!g.toGround(c, r, la, lo) || !g.toImage(la, lo, c2, r2)) { worst = 1e9; continue; }
            worst = std::max(worst, std::hypot(c2 - c, r2 - r)); n++;
        }
        CHECK(n > 100 && worst < 0.05, "round trip over %d pixels: worst %.4f px", n, worst);
    }
    // places that are NOT in the picture are refused: the far side of the Earth, beyond the swath, before the first row
    {
        double c, r;
        CHECK(!g.toImage(-56.0, 177.0, c, r), "the antipode is not in the picture");
        CHECK(!g.toImage(56.5, 60.0, c, r), "the Urals (beyond the swath) are not in the picture");
        CHECK(!g.toImage(40.0, -3.0, c, r), "Madrid (south of the first line) is not in the picture");
        CHECK(g.toImage(57.48, -4.22, c, r) && c > 600 && c < 900, "Inverness is near nadir (col %.0f row %.0f)", c, r);
    }

    std::printf("── the clock checks (fail closed) ──\n");
    auto strips = [&](double periodMs, int n, int32_t first) { std::vector<int32_t> v; for (int i = 0; i < n; i++) v.push_back(int32_t(std::lround(first + i * periodMs))); return v; };
    const int32_t ms0 = 50805129;              // 14:06:45.129
    const double day = 1795392000.0;           // 23 Nov 2026 00:00 UTC
    Clock c;
    {
        auto v = strips(1231.14, 100, ms0); for (int i = 3; i < 100; i += 7) v[i] = -1;   // gaps are normal
        CHECK(checkClock(v, day + ms0 / 1000.0 + 90 + 1.4, int32_t(ms0 + 90000), c, why), "a good pass: %s", why.c_str());
        CHECK(std::fabs(c.bMs - 1231.14) < 0.05 && std::fabs(c.skewSec + 1.4) < 0.01 && c.dayStartUnix == day, "period %.2f skew %.2f day %.0f", c.bMs, c.skewSec, c.dayStartUnix);
    }
    {
        auto v = strips(1231.14, 50, ms0); v[20] += 400;
        why.clear(); CHECK(!checkClock(v, day + ms0 / 1000.0 + 30, ms0 + 30000, c, why) && why.find("jump") != std::string::npos, "a stamp 400 ms out: %s", why.c_str());
    }
    {
        auto v = strips(1000.0, 50, ms0);
        why.clear(); CHECK(!checkClock(v, day + ms0 / 1000.0 + 30, ms0 + 30000, c, why) && why.find("1000 ms") != std::string::npos, "a 1000 ms period: %s", why.c_str());
    }
    {
        auto v = strips(1231.14, 50, ms0);   // ★ METEOR-M2 3, Oct 2023 shape: the satellite's clock hours out
        why.clear(); CHECK(!checkClock(v, day + ms0 / 1000.0 - 3 * 3600.0, ms0, c, why) && why == "the satellite's clock reads 3 h 0 min ahead of ours", "3 h out: %s", why.c_str());
        why.clear(); CHECK(!checkClock(v, day + ms0 / 1000.0 + 41, ms0, c, why) && why.find("41.0 s behind") != std::string::npos, "41 s out: %s", why.c_str());
    }
    {
        auto v = strips(1231.14, 40, 86400000 - 20000);   // across midnight
        for (auto& x : v) if (x >= 86400000) x -= 86400000;
        CHECK(checkClock(v, 1795392000.0 + 86400 + 5, 3000, c, why), "across midnight: %s", why.c_str());
        CHECK(std::fabs(c.unixAt(0) - (1795392000.0 + 86400 - 20)) < 0.01, "the first strip is before midnight (%.3f)", c.unixAt(0) - 1795392000.0 - 86400);
    }
    {
        std::vector<int32_t> v = { ms0, -1, -1 };
        why.clear(); CHECK(!checkClock(v, day + ms0 / 1000.0, ms0, c, why) && why.find("too little") != std::string::npos, "one strip: %s", why.c_str());
    }

    std::printf("── which satellite ──\n");
    vibe::sat::Observer obs(52.3125, -0.875, 0);
    {
        why.clear();
        int i = chooseSatellite(cands, &obs, t0 + 60, why);
        CHECK(i >= 0 && cands[size_t(i)].norad == 59051, "the one above the horizon is METEOR-M2-4 (%d %s)", i, why.c_str());
        // the pick wins — when it is up
        std::vector<Candidate> cs = cands; for (auto& x : cs) x.picked = x.norad == 57166;
        why.clear(); i = chooseSatellite(cs, &obs, t0 + 60, why);
        CHECK(i >= 0 && cs[size_t(i)].norad == 59051, "a pick that is below the horizon does not win (%d)", i);
        // nobody up (the far side of the world from both)
        vibe::sat::Observer far(-52.0, 179.0, 0);
        why.clear(); i = chooseSatellite(cands, &far, t0 + 60, why);
        CHECK(i < 0 && why.find("horizon") != std::string::npos, "neither up: %s", why.c_str());
        // no location, two candidates, no pick: refused
        why.clear(); i = chooseSatellite(cands, nullptr, t0 + 60, why);
        CHECK(i < 0 && why.find("location") != std::string::npos, "no location: %s", why.c_str());
        // old elements
        why.clear(); i = chooseSatellite(cands, &obs, t0 + 20 * 86400.0, why);
        CHECK(i < 0 && why.find("days old") != std::string::npos, "elements 24 days old: %s", why.c_str());
        (void)m23;
    }

    std::printf("── the outline JSON ──\n");
    {
        // ★ the 11-19 elements are 4.0 days before this pass: inside kMaxElementDays
        auto v = strips(1231.14, 100, ms0);
        const Overlay ov = makeOverlay(v, 800, day + ms0 / 1000.0 + 100, int32_t(ms0 + 98500), cands, &obs);
        CHECK(ov.ok, "outline made: %s", ov.why.c_str());
        const std::string& j = ov.json;
        CHECK(j.rfind("{\"v\":1,\"w\":1568,\"h\":800,", 0) == 0, "header: %s", j.substr(0, 60).c_str());
        CHECK(j.find("\"sat\":\"Meteor-M2-4\",\"norad\":59051") != std::string::npos, "names the satellite");
        CHECK(j.find("\"coast\":[[") != std::string::npos && j.find("\"border\":[[") != std::string::npos && j.find("\"grid\":[[") != std::string::npos, "coast, border and grid lines");
        CHECK(j.find("\"55\xc2\xb0N\"") != std::string::npos && j.find("\"5\xc2\xb0W\"") != std::string::npos, "grid labels 55°N and 5°W");
        CHECK(std::fabs(ov.latC - 56.4) < 1.0 && std::fabs(ov.lonC + 2.7) < 1.5, "centre %.2f %.2f (Scotland)", ov.latC, ov.lonC);
        CHECK(j.size() < 200000 && ov.points > 1000, "%zu bytes, %d points", j.size(), ov.points);
        // every coordinate inside the picture (±2 px)
        bool inside = true; size_t p = j.find("\"coast\":[");
        for (const char* q = j.c_str() + p + 9; *q && *q != 'l'; ) {   // up to "lake"
            if ((*q >= '0' && *q <= '9') || *q == '-') { char* e; const long x = std::strtol(q, &e, 10); q = e; if (*q == ',') q++; const long y = std::strtol(q, &e, 10); q = e;
                if (x < -2 || x > 1570 || y < -2 || y > 802) inside = false; }
            else q++;
        }
        CHECK(inside, "every coast point is inside the picture");
        // the live preview's quarter size
        const Overlay q = makeOverlay(v, 800, day + ms0 / 1000.0 + 100, int32_t(ms0 + 98500), cands, &obs, 0.25);
        CHECK(q.ok && q.json.rfind("{\"v\":1,\"w\":392,\"h\":200,", 0) == 0, "quarter size: %s", q.json.substr(0, 40).c_str());
        // ★★ fail closed: the same pass with the clock 3 h out draws nothing
        const Overlay bad = makeOverlay(v, 800, day + ms0 / 1000.0 + 100 - 10800, int32_t(ms0 + 98500), cands, &obs);
        CHECK(!bad.ok && bad.json.empty() && bad.why.find("3 h") != std::string::npos, "3 h out: no outline (%s)", bad.why.c_str());
        // the cost: a whole pass's outline
        const auto a = std::chrono::steady_clock::now();
        for (int k = 0; k < 5; k++) (void)makeOverlay(v, 800, day + ms0 / 1000.0 + 100, int32_t(ms0 + 98500), cands, &obs);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count() / 5;
        std::printf("   an outline for 800 rows: %.1f ms (M-series Mac)\n", ms);
        CHECK(ms < 500, "under half a second here (%.1f ms)", ms);
    }

    std::printf("── through Run: a synthetic pass gets its outline at the end, and a live one while it grows ──\n");
    for (int bad = 0; bad < 2; bad++) {
        tx::TxConfig tc; tc.esn0Db = 25; tc.startMs = uint32_t(ms0);
        const auto iq = tx::modulate(tc, tx::codeBits(tc, tx::buildVcdus(tc, 8)));
        Run::Opts o; o.symbolRate = 72000;
        double fed = 0;
        o.geoCandidates = [&] { return cands; };
        o.geoObserver = [&](vibe::sat::Observer& ob) { ob = obs; return true; };
        // our clock: the stamps' time + 1.5 s of delay (or 3 h out for the second run)
        o.nowUnix = [&] { return day + ms0 / 1000.0 + fed + 1.5 + (bad ? 10800.0 : 0.0); };
        std::vector<Picture> got; int live = 0; std::string lastLive;
        o.pictures = [&](const std::vector<Picture>& ps, bool) { got = ps; };
        o.json = [&](const std::string& j) { if (j.find("\"type\":\"lrpt_geo\"") != std::string::npos) { live++; lastLive = j; } };
        o.frame = [](const uint8_t*, size_t) {};
        {
            Run run(o);
            const size_t blk = size_t(tc.sampleRate / 20);
            for (size_t at = 0; at < iq.size() / 2; at += blk) {
                const size_t n = std::min(blk, iq.size() / 2 - at);
                run.feedIq(reinterpret_cast<const std::complex<float>*>(&iq[at * 2]), int(n), tc.sampleRate, 0);
                fed += n / tc.sampleRate;
            }
            run.finish(false);
        }
        CHECK(!got.empty(), "pictures at the end (%zu)", got.size());
        if (!bad) {
            bool all = !got.empty(); for (auto& p : got) all = all && !p.geoJson.empty() && p.geoWhy.empty();
            CHECK(all, "every picture carries the outline (%s)", got.empty() ? "-" : got[0].geoWhy.c_str());
            CHECK(live >= 1 && lastLive.find("\"w\":392") != std::string::npos, "the live outline, quarter size (%d): %s", live, lastLive.substr(0, 80).c_str());
        } else {
            bool all = !got.empty(); for (auto& p : got) all = all && p.geoJson.empty() && p.geoWhy.find("3 h 0 min") != std::string::npos;
            CHECK(all, "3 h out: no outline, the reason on every picture (%s)", got.empty() ? "-" : got[0].geoWhy.c_str());
            CHECK(live >= 1 && lastLive.find("\"why\":\"the satellite's clock") != std::string::npos, "the live message says why: %s", lastLive.c_str());
        }
    }

    std::printf("── DOWNLOAD with the outline: burnt into a copy (vibe_lrpt_burn.h, ?outline=1) ──\n");
    {
        const int w = 64, h = 40;
        std::vector<uint8_t> grey(size_t(w) * h);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) grey[size_t(y) * w + x] = uint8_t((x * 3 + y * 5) & 0xFF);
        const std::string png = vibe::png::encode(w, h, 1, grey.data());
        const std::string js = "{\"v\":1,\"w\":64,\"h\":40,\"coast\":[[2,10,60,10]],\"lake\":[],\"border\":[[5,30,5,35]],\"grid\":[],\"labels\":[[20,20,\"5\xc2\xb0N\"]]}";
        std::string err;
        const std::string out = burn::withOutline(png, js, err);
        int ow = 0, oh = 0; std::vector<uint8_t> rgb;
        CHECK(!out.empty() && burn::decodeRgb(out, ow, oh, rgb).empty() && ow == w && oh == h, "a burnt copy decodes (%s)", err.c_str());
        if (rgb.size() == size_t(w) * h * 3) {
            auto at = [&](int x, int y) { const uint8_t* p = &rgb[(size_t(y) * w + x) * 3]; return (int(p[0]) << 16) | (int(p[1]) << 8) | p[2]; };
            CHECK(at(30, 10) == 0xFFD600, "the coast line is drawn (%06x)", at(30, 10));
            CHECK(at(5, 32) == 0xFF8C3C, "the border line is drawn (%06x)", at(5, 32));
            const int g = grey[size_t(2) * w + 50];
            CHECK(at(50, 2) == ((g << 16) | (g << 8) | g), "the rest of the picture is untouched");
            bool lab = false; for (int y = 24; y < 40; y++) for (int x = 24; x < 40; x++) if (at(x, y) == 0x96CDFF) lab = true;
            CHECK(lab, "the grid label is drawn");
        }
        // ★ every PNG row filter decodes (our writer uses Sub only; a decoder that skipped the others would be a trap)
        std::string rt; int rw, rh; std::vector<uint8_t> back;
        CHECK(burn::decodeRgb(png, rw, rh, back).empty() && back[(size_t(7) * w + 9) * 3] == grey[size_t(7) * w + 9], "a grey PNG reads back");
        err.clear(); CHECK(burn::withOutline("not a png", js, err).empty() && !err.empty(), "nonsense refused: %s", err.c_str());
    }

    std::printf("── the gallery: the outline item, geo / gw, the PNG untouched ──\n");
    {
        char tmpl[] = "/tmp/vibe-lrpt-geo-XXXXXX";
        const char* dir = mkdtemp(tmpl);
        CHECK(dir != nullptr, "temp dir");
        gallery::Store store; store.setDir(dir); store.setLimitBytes(64ull << 20);
        gallery::Meta meta; meta.kind = "lrpt"; meta.origin = "live"; meta.freqHz = 137.9e6; meta.mode = "iq"; meta.decoder = "lrpt";
        const std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + "pixels";
        {
            bg::LrptGalleryTap tap(store, meta);
            tap.outline("{\"v\":1,\"w\":1568,\"h\":800,\"coast\":[[1,2,3,4]]}", "");
            tap.picture("RGB 221 false colour", png, 1568, 800, false);
            tap.picture("channel 1 visible", png, 1568, 800, false);
        }
        const std::string list = store.listJson("", 50);
        size_t geoAt = list.find("\"k\":\"lrptgeo\"");
        CHECK(geoAt != std::string::npos, "the outline is its own item (kind lrptgeo): %s", list.substr(0, 400).c_str());
        // its file name, from that item's line
        std::string geoName;
        { size_t a = list.rfind("{\"f\":\"", geoAt); if (a != std::string::npos) { a += 6; geoName = list.substr(a, list.find('"', a) - a); } }
        CHECK(!geoName.empty() && geoName.size() > 5 && geoName.compare(geoName.size() - 5, 5, ".json") == 0, "a .json item: %s", geoName.c_str());
        int named = 0; size_t p = 0; while ((p = list.find("\"geo\":\"" + geoName + "\"", p)) != std::string::npos) { named++; p++; }
        CHECK(named == 2, "both pictures name it (%d)", named);
        std::string body; CHECK(store.readItem(geoName, body) && body.find("\"coast\":[[1,2,3,4]]") != std::string::npos, "the outline reads back");
        // a PNG is stored byte for byte (never altered by the outline)
        std::string pn; { size_t a = list.find(".png\""); if (a != std::string::npos) { size_t b = list.rfind('"', a - 1); pn = list.substr(b + 1, a + 4 - b - 1); } }
        std::string pb; CHECK(store.readItem(pn, pb) && pb == png, "the PNG %s is the plain picture, unaltered", pn.c_str());
        // a pass that failed its checks: no outline item, the reason on each picture
        {
            bg::LrptGalleryTap tap(store, meta);
            tap.outline("", "the satellite's clock reads 3 h 0 min ahead of ours");
            tap.picture("channel 4 MWIR", png, 1568, 400, false);
        }
        const std::string l2 = store.listJson("lrptgeo", 50);
        int geoItems = 0; p = 0; while ((p = l2.find("\"k\":\"lrptgeo\"", p)) != std::string::npos) { geoItems++; p++; }
        CHECK(geoItems == 1, "no second outline item (%d)", geoItems);
        const std::string l3 = store.listJson("lrpt", 1);
        CHECK(l3.find("\"gw\":\"the satellite's clock reads 3 h 0 min ahead of ours\"") != std::string::npos && l3.find("\"geo\":") == std::string::npos,
              "the reason rides on the picture: %s", l3.substr(0, 300).c_str());
        std::string cmd = std::string("rm -rf ") + dir; (void)!std::system(cmd.c_str());
    }

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
