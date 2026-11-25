// test-community.cpp — V12 beta94 THE COMMUNITY MAP (vibe_community.h, docs/v12/COMMUNITY-MAP.md): what a server shares.
//   1. assemble(): the envelope, the layers in their order, an empty section skipped, a section that would pass the cap
//      DROPPED and named — so the answer is bounded by construction (NO OOM).
//   2. the aircraft: positioned only, no older than the age cap, newest first, at most kAircraftMax; their layers.
//   3. the aircraft JSON is /vibeserver/aircraft.json's own format, and 600 rows fit the cap.
//   4. NDB: the day card read back (vibe_ndb_gallery.h parseCard, the SAME reader the day log restores with), placed from
//      the published list; the community row carries NO distance or bearing (they would give the receiver away);
//      one per ident + frequency across two days' cards; age and row caps.
//   5. the cache: one build per TTL whoever asks; clear() forces the next.
//   6. the off answer.
// usage: scripts/run-tests.sh builds it with the NDB group's sources; writes the answer it built to $COMMUNITY_FIXTURE if
// set (the directory's map-page test reads the same shape).
#include "vibe_community.h"
#include "vibe_ndb_gallery.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace vibecommunity;

static size_t count(const std::string& s, const std::string& sub) { size_t n = 0; for (size_t p = s.find(sub); p != std::string::npos; p = s.find(sub, p + 1)) ++n; return n; }

static vibeavshare::Row plane(uint32_t icao, int64_t posMs, bool pos, int src = vibeavshare::kAdsb) {
    vibeavshare::Row w; w.used = true; w.icao = icao; w.srcMask = uint8_t(1u << src); w.srcMs[src] = posMs; w.lastMs = posMs; w.firstMs = posMs - 60000;
    w.flight = "BAW" + std::to_string(icao % 1000); w.count = 10;
    if (pos) { w.flags = 1 | 2 | 4 | 8; w.lat = 51.0 + (icao % 100) * 0.01; w.lon = -1.0 + (icao % 50) * 0.02; w.posMs = posMs; w.posSrc = src; w.altFt = 35000; w.trk = 90; w.gs = 440; }
    return w;
}

int main() {
    const int64_t now = 1764072000;   // 2025-11-25 12:00:00 UTC
    const int64_t nowMs = now * 1000;

    std::printf("== 1. assemble: the envelope, the layer order, the cap\n");
    {
        std::vector<std::string> layers;
        const std::string j = assemble(now, { { "ndb", "{\"log\":[]}", { "ndb" } }, { "aircraft", "{\"aircraft\":[]}", { "hfdl", "adsb" } },
                                              { "empty", "", { "dsc" } } }, kMaxBytes, &layers);
        CHECK(j.rfind("{\"v\":1,\"on\":true,\"now\":1764072000,\"layers\":[\"adsb\",\"hfdl\",\"ndb\"]", 0) == 0, "envelope/order: %s", j.c_str());
        CHECK(layers.size() == 3 && layers[0] == "adsb" && layers[2] == "ndb", "layersOut in layerOrder");
        CHECK(j.find("\"empty\"") == std::string::npos && j.find("\"dsc\"") == std::string::npos, "an empty section is skipped, its layer not named");
        CHECK(j.find("\"dropped\"") == std::string::npos, "nothing dropped under the cap");
        const std::string big(5000, 'x');
        const std::string j2 = assemble(now, { { "aircraft", "{\"a\":1}", { "adsb" } }, { "ships", "\"" + big + "\"", { "ais" } },
                                               { "ndb", "{\"b\":2}", { "ndb" } } }, 2000, &layers);
        CHECK(j2.find("\"ships\":") == std::string::npos && j2.find("\"dropped\":[\"ships\"]") != std::string::npos, "an oversized section is dropped and named: %s", j2.c_str());
        CHECK(j2.find("\"ais\"") == std::string::npos && j2.find("\"ndb\":{\"b\":2}") != std::string::npos, "…its layer with it; the next still fits");
        CHECK(j2.size() <= 2000, "the answer is within the cap (%zu)", j2.size());
        // ★ many sections each near the cap: the total never passes it
        std::vector<Section> many;
        for (int i = 0; i < 20; i++) many.push_back({ "s" + std::to_string(i), "\"" + std::string(60000, 'y') + "\"", { "adsb" } });
        CHECK(assemble(now, many).size() <= kMaxBytes, "20 × 60 KB sections stay within kMaxBytes");
    }

    std::printf("== 2. the aircraft to share\n");
    {
        std::vector<vibeavshare::Row> rows;
        rows.push_back(plane(0x400001, nowMs - 5000, true));                                    // now
        rows.push_back(plane(0x400002, nowMs - 2000, false));                                   // no position → out
        rows.push_back(plane(0x400003, nowMs - (kAircraftAgeSec + 10) * 1000, true));           // too old → out
        rows.push_back(plane(0x400004, nowMs - 1000, true, vibeavshare::kHfdl));                 // newest, HFDL
        rows.push_back(plane(0x400005, nowMs - 60000, true, vibeavshare::kVdl2));
        const auto out = aircraftRows(rows, nowMs);
        CHECK(out.size() == 3, "positioned and recent only (%zu)", out.size());
        CHECK(out.size() == 3 && out[0].icao == 0x400004 && out[1].icao == 0x400001 && out[2].icao == 0x400005, "newest position first");
        const auto l = aircraftLayers(out);
        CHECK(l.size() == 3 && l[0] == "adsb" && l[1] == "vdl2" && l[2] == "hfdl", "layers adsb vdl2 hfdl (no acars: none heard)");
        std::vector<vibeavshare::Row> lots;
        for (uint32_t i = 0; i < 1000; i++) lots.push_back(plane(0x500000 + i, nowMs - i * 100, true));
        const auto capped = aircraftRows(lots, nowMs);
        CHECK(capped.size() == kAircraftMax && capped.front().icao == 0x500000, "1000 → the newest kAircraftMax");

        std::printf("== 3. the aircraft JSON — aircraft.json's own format, 600 rows within the cap\n");
        const std::string aj = vibeavshare::toJson(capped, {}, nowMs);
        CHECK(aj.rfind("{\"now\":1764072000,\"count\":600,\"aircraft\":[{\"key\":\"500000\",\"icao\":\"500000\"", 0) == 0, "aircraft.json's shape");
        CHECK(count(aj, "\"lat\":") == 600, "every row placed");
        CHECK(aj.find("\"rx\"") == std::string::npos && aj.find("\"km\"") == std::string::npos, "nothing about the receiver");
        std::vector<std::string> layers;
        const std::string all = assemble(now, { { "aircraft", aj, aircraftLayers(capped) } }, kMaxBytes, &layers);
        CHECK(all.find("\"dropped\"") == std::string::npos && all.size() < kMaxBytes, "600 aircraft fit (%zu bytes)", all.size());
        std::printf("   (600 aircraft = %zu bytes)\n", aj.size());
    }

    std::printf("== 4. NDB — the day card, placed from the published list, no distance or bearing\n");
    {
        const char* dot = " \xc2\xb7 ";
        auto line = [&](const char* last, const char* first, const char* id, const char* khz, const char* snr, const char* km, const char* brg,
                        const char* cc, const char* st) {
            return std::string(last) + dot + first + dot + id + dot + khz + " kHz" + dot + snr + " dB" + dot + km + dot + brg + dot + cc + dot + st + "\n";
        };
        const std::string head = "NDB heard log \xc2\xb7 2025-11-25 (UTC)\nHeard by every radio on this server.\n\n3 beacons identified\n\n"
                                 "Each beacon, most recently heard first (last heard \xc2\xb7 first heard \xc2\xb7 ident \xc2\xb7 kHz \xc2\xb7 best SNR \xc2\xb7 distance \xc2\xb7 bearing \xc2\xb7 country \xc2\xb7 station):\n";
        const std::string today = head
            + line("11:40:12", "07:30:05", "WOD", "352.000", "26", "94 km", "181\xc2\xb0 S", "GB", "Woodley")
            + line("11:30:00", "08:00:00", "QQX", "399.000", "12", "-", "-", "-", "(not in the published list)")
            + "this line is not a beacon\n";
        const int64_t day = now / 86400;
        const auto c = vibe::ndbgal::parseCard(today, day);
        CHECK(c.size() == 2, "two beacon lines read (%zu)", c.size());
        CHECK(c.size() == 2 && c[0].ident == "WOD" && std::fabs(c[0].hz - 352000) < 1 && c[0].last == day * 86400 + 11 * 3600 + 40 * 60 + 12, "WOD 352 kHz at 11:40:12");
        CHECK(c.size() == 2 && std::fabs(c[0].lat - 51.4528) < 1e-3 && std::fabs(c[0].lon + 0.87889) < 1e-3, "WOD placed from the published list");
        CHECK(c.size() == 2 && !std::isfinite(c[1].lat), "an unlisted ident has no position");
        std::vector<NdbRow> rows;
        for (const auto& b : c) { NdbRow r; r.ident = b.ident; r.name = b.name; r.cc = b.cc; r.hz = b.hz; r.lat = b.lat; r.lon = b.lon; r.best = b.best; r.first = b.first; r.last = b.last; rows.push_back(r); }
        // yesterday's card names WOD again (older) and an old beacon past the day
        NdbRow y = rows[0]; y.last -= 86400; y.first -= 86400; rows.push_back(y);
        NdbRow old = rows[0]; old.ident = "OLD"; old.last = now - kNdbSinceSec - 60; rows.push_back(old);
        const std::string j = ndbJson(rows, now);
        CHECK(count(j, "\"ident\":") == 1 && j.find("\"ident\":\"WOD\"") != std::string::npos, "one WOD, no unplaced, no old: %s", j.c_str());
        CHECK(j.find("\"km\"") == std::string::npos && j.find("\"brg\"") == std::string::npos && j.find("\"deg\"") == std::string::npos,
              "no distance or bearing from the receiver");
        CHECK(j.find("\"name\":\"Woodley\"") != std::string::npos && j.find("\"cc\":\"GB\"") != std::string::npos && j.find("\"hz\":352000") != std::string::npos, "the NdbLog fields");
        CHECK(ndbJson({}, now).empty(), "nothing → no section");
        std::vector<NdbRow> many;
        for (int i = 0; i < 1000; i++) { NdbRow r = rows[0]; r.ident = "B" + std::to_string(i); r.last = now - i; many.push_back(r); }
        CHECK(count(ndbJson(many, now), "\"ident\":") == kNdbMax, "capped at kNdbMax");
    }

    std::printf("== 5. the cache: one build per TTL\n");
    {
        Cache c;
        int n = 0;
        auto build = [&](std::vector<std::string>& l) { ++n; l = { "adsb" }; return std::string("{\"n\":") + std::to_string(n) + "}"; };
        const auto a = c.get(now, kCacheSec, build);
        const auto b = c.get(now + 10, kCacheSec, build);
        const auto d = c.get(now + 29, kCacheSec, build);
        CHECK(n == 1 && a.body == b.body && b.body == d.body && a.tag == d.tag, "three asks inside 30 s = one build");
        const auto e = c.get(now + 30, kCacheSec, build);
        CHECK(n == 2 && e.body == "{\"n\":2}" && e.tag != a.tag, "at 30 s, a fresh build with a fresh ETag");
        CHECK(c.get(now + 100, 600, build).body == e.body && n == 2, "a status read accepts 10 minutes");
        CHECK(e.layers.size() == 1 && e.layers[0] == "adsb", "the layers ride along");
        c.clear();
        c.get(now + 101, kCacheSec, build);
        CHECK(n == 3, "clear() → the next ask builds");
        CHECK(c.get(now - 1000, kCacheSec, build).body == "{\"n\":4}", "a clock that went back rebuilds rather than serving for ever");
    }

    std::printf("== 6. off\n");
    CHECK(offJson("off") == "{\"v\":1,\"on\":false,\"why\":\"off\"}", "off");
    CHECK(offJson("pin\"x") == "{\"v\":1,\"on\":false,\"why\":\"pin\\\"x\"}", "escaped");

    if (const char* fx = std::getenv("COMMUNITY_FIXTURE")) {
        std::vector<vibeavshare::Row> rows;
        for (uint32_t i = 0; i < 3; i++) rows.push_back(plane(0x4CA000 + i, nowMs - i * 4000, true));
        const std::string aj = vibeavshare::toJson(rows, {}, nowMs);
        std::ofstream(fx) << assemble(now, { { "aircraft", aj, aircraftLayers(rows) } });
        std::printf("   (fixture written to %s)\n", fx);
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
