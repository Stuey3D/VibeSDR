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
#include "vibe_gzip.h"

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
        CHECK(j.rfind("{\"v\":2,\"on\":true,\"now\":1764072000,\"layers\":[\"adsb\",\"hfdl\",\"ndb\"]", 0) == 0, "envelope/order: %s", j.c_str());
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
        // ★★ beta96: a DATALINK position is held 30 min (faded on the map by its age); an ADS-B one still 5 min
        {
            std::vector<vibeavshare::Row> dl;
            dl.push_back(plane(0x600001, nowMs - 20 * 60000, true, vibeavshare::kVdl2));    // VDL2, 20 min → kept
            dl.push_back(plane(0x600002, nowMs - 29 * 60000, true, vibeavshare::kHfdl));    // HFDL, 29 min → kept
            dl.push_back(plane(0x600003, nowMs - 25 * 60000, true, vibeavshare::kAdsc));    // ADS-C, 25 min → kept
            dl.push_back(plane(0x600004, nowMs - 15 * 60000, true, vibeavshare::kAcars));   // ACARS, 15 min → kept
            dl.push_back(plane(0x600005, nowMs - 31 * 60000, true, vibeavshare::kHfdl));    // HFDL, 31 min → out
            dl.push_back(plane(0x600006, nowMs - 6 * 60000, true, vibeavshare::kAdsb));     // ADS-B, 6 min → out
            auto heardDl = plane(0x600007, nowMs - 10 * 60000, true, vibeavshare::kAdsb);  // heard on VDL2 too, but the
            heardDl.srcMask |= 1u << vibeavshare::kVdl2;                                    // POSITION is ADS-B's → 5 min → out
            dl.push_back(heardDl);
            auto unknown = plane(0x600008, nowMs - 10 * 60000, true, vibeavshare::kVdl2); unknown.posSrc = -1;   // unknown → ADS-B's 5 min
            dl.push_back(unknown);
            const auto o = aircraftRows(dl, nowMs);
            CHECK(o.size() == 4 && o[0].icao == 0x600004 && o[1].icao == 0x600001 && o[2].icao == 0x600003 && o[3].icao == 0x600002,
                  "datalink positions held 30 min, ADS-B 5 min, by the POSITION's source (%zu)", o.size());
            const std::string c = aircraftCompact(o, nowMs);
            CHECK(c.find("[\"600002\",") != std::string::npos && c.find(",1740,") != std::string::npos,
                  "the row carries its position age (29 min = 1740 s) for the map's fade: %.120s", c.c_str());
        }
        // ★★ beta96 THE GLYPH's INPUTS (Stuart 11-26: "the correct icon for the aircraft type"): field 11 "wake|letter|engines|m"
        //    from the aircraft data (the Cache's answer, here a stub); the type filled where the row has none; "" when unknown
        {
            std::vector<vibeavshare::Row> w3;
            w3.push_back(plane(0x400001, nowMs - 1000, true));                        // a 747 the data knows, the row no type
            w3.push_back(plane(0x400002, nowMs - 2000, true)); w3.back().type = "EC35"; // a helicopter, the row's own type
            w3.push_back(plane(0x400003, nowMs - 3000, true));                        // nothing known
            w3.push_back(plane(0x400004, nowMs - 4000, true));                        // a hostile answer: the separator stays out
            const vibeavshare::ExtraFn stub = [](const vibeavshare::Row& r, vibeavshare::Extra& ex) {
                if (r.icao == 0x400001) { ex.type = "B744"; ex.wtc = "H"; ex.species = "L"; ex.eng = "4J"; }
                if (r.icao == 0x400002) { ex.type = "XXXX"; ex.wtc = "L"; ex.species = "H"; ex.eng = "2T"; ex.mil = true; }
                if (r.icao == 0x400004) { ex.wtc = "|"; ex.species = "\""; ex.eng = "2J|x"; }
            };
            const std::string c = "[" + aircraftCompact(w3, nowMs, stub) + "]";
            CHECK(c.find("\"B744\",\"H|L|4J|\"]") != std::string::npos, "the 747: the data's type and \"H|L|4J|\": %.200s", c.c_str());
            CHECK(c.find("\"EC35\",\"L|H|2T|m\"]") != std::string::npos, "the helicopter: the ROW's type wins, military marked");
            { const size_t at = c.find("[\"400003\","), end = c.find(']', at);
              CHECK(at != std::string::npos && c.compare(end - 5, 5, "\"\",\"\"") == 0, "nothing known: an empty type and an empty what: %.80s", c.c_str() + at); }
            CHECK(c.find("\"||2J|\"]") != std::string::npos, "a part never carries the separator or a quote");
            CHECK(aircraftCompact(w3, nowMs).find(",\"\"]") != std::string::npos, "no extra (an older caller): the field is there, empty");
        }
        std::vector<vibeavshare::Row> lots;
        for (uint32_t i = 0; i < 1000; i++) lots.push_back(plane(0x500000 + i, nowMs - i * 100, true));
        const auto capped = aircraftRows(lots, nowMs);
        CHECK(capped.size() == kAircraftMax && capped.front().icao == 0x500000, "1000 → the newest kAircraftMax");

        std::printf("== 3. the aircraft rows — compact, 600 within the cap\n");
        const std::string aj = "[" + aircraftCompact(capped, nowMs) + "]";
        CHECK(aj.rfind("[[\"500000\",", 0) == 0, "compact rows, the ICAO first: %.60s", aj.c_str());
        CHECK(count(aj, "],[") == 599, "600 rows");
        CHECK(aj.find("\"rx\"") == std::string::npos && aj.find("km") == std::string::npos, "nothing about the receiver");
        std::vector<std::string> layers;
        const std::string all = assemble(now, { { "a", aj, aircraftLayers(capped) } }, kMaxBytes, &layers);
        CHECK(all.find("\"dropped\"") == std::string::npos && all.size() < kMaxBytes, "600 aircraft fit (%zu bytes)", all.size());
        std::printf("   (600 aircraft = %zu bytes raw, %zu gzipped)\n", aj.size(), vibegz::gzip(aj).size());
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
        const std::string j = ndbRows(rows, now);
        CHECK(count(j, "[\"") == 1 && j.rfind("[\"WOD\",352000,51.4528,-0.8789,", 0) == 0, "one WOD row, no unplaced, no old: %s", j.c_str());
        CHECK(j.find("94") == std::string::npos && j.find("181") == std::string::npos, "no distance or bearing from the receiver");
        CHECK(j.find("\"Woodley\",\"GB\"]") != std::string::npos, "name and country");
        CHECK(ndbRows({}, now).empty(), "nothing → no section");
        std::vector<NdbRow> many;
        for (int i = 0; i < 1000; i++) { NdbRow r = rows[0]; r.ident = "B" + std::to_string(i); r.last = now - i; many.push_back(r); }
        CHECK(count(ndbRows(many, now), "[\"") == kNdbMax, "capped at kNdbMax");
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

    std::printf("== 7. MeshCore nodes from the ISM gallery card — MeshCore only, a person rounded, no receiver line\n");
    {
        const char* dot = " \xc2\xb7 ";
        auto row = [&](std::vector<std::string> f) { std::string o; for (size_t i = 0; i < f.size(); i++) o += (i ? dot : "") + f[i]; return o + "\n"; };
        const std::string card = std::string("ISM nodes heard \xc2\xb7 868 \xc2\xb7 2025-11-25\nReceiver:     52.3456, -1.2345\n\nEach node, most recent first (last heard \xc2\xb7 first heard \xc2\xb7 times heard \xc2\xb7 heard by \xc2\xb7 network \xc2\xb7 type \xc2\xb7 name \xc2\xb7 id \xc2\xb7 position \xc2\xb7 hops \xc2\xb7 SNR dB \xc2\xb7 MHz):\n")
            + row({ "11:50:00", "08:00:00", "12\xc3\x97", "background", "MeshCore", "repeater", "Hilltop RPT", "a1b2c3d4", "52.12345, -1.54321", "0", "7.5", "869.525" })
            + row({ "11:40:00", "09:00:00", "3\xc3\x97", "live", "MeshCore", "companion", "Bob's phone", "ffee0011", "52.98765, -1.11111", "1", "2.0", "869.525" })
            + row({ "11:30:00", "09:00:00", "5\xc3\x97", "live", "Meshtastic", "router", "MT node", "!abcd", "51.5, -0.1", "0", "3.0", "869.525" })
            + "Acurite 5n1 weather sensor at 52.3, -1.2\n";
        const int64_t day = now / 86400;
        const auto nodes = meshNodesOfCard(card, day);
        CHECK(nodes.size() == 2, "two MeshCore nodes, the Meshtastic one and the sensor left out (%zu)", nodes.size());
        CHECK(nodes.size() == 2 && std::fabs(nodes[1].lat - 52.99) < 1e-9 && std::fabs(nodes[1].lon + 1.11) < 1e-9 && nodes[1].approx, "a companion (a person) rounded to 0.01°");
        CHECK(nodes.size() == 2 && std::fabs(nodes[0].lat - 52.12345) < 1e-9, "a repeater exact");
        const std::string j = meshRows(nodes, now);
        CHECK(count(j, "[\"") == 2 && j.find("52.3456") == std::string::npos && j.find("Receiver") == std::string::npos, "rows: no receiver position: %s", j.c_str());
        CHECK(meshRows(nodes, now + 2 * 86400).empty(), "older than the 24 h window → nothing");
    }
    std::printf("== 8. NAVTEX messages: one per id, the cleanest copy, a title, bounded\n");
    {
        std::vector<NavMsg> cs;
        auto copy = [&](const char* f, int64_t t, const std::string& text) { NavMsg m; m.file = f; m.first = m.last = t; m.khz = 518; m.text = text; m.id = navMsgId(text); m.lost = navLost(text); cs.push_back(m); };
        copy("a.txt", now - 3000, "ZCZC GA98\n251200 UTC NOV\nNAVAREA I 1*3/25\nWIGHT. BUOY UNL*T\nNNNN\n");
        copy("b.txt", now - 1000, "ZCZC GA98\n251200 UTC NOV\nNAVAREA I 123/25\nWIGHT. BUOY UNLIT\nNNNN\n");
        copy("c.txt", now - 500, "ZCZC GA98\n251200 UTC NOV\nNAVAREA I 1**/25\nWIGHT. BUOY ****T\nNNNN\n");
        copy("d.txt", now - 200, "ZCZC GE45\nSHIPPING FORECAST\nNNNN\n");
        copy("e.txt", now - 100, "start lost and no header\n");
        const auto m = navFold(cs);
        CHECK(m.size() == 3, "GA98 three copies → one; GE45; the header-less one stands alone (%zu)", m.size());
        const NavMsg* ga = nullptr; for (const auto& x : m) if (x.id == "GA98") ga = &x;
        CHECK(ga && ga->file == "b.txt" && ga->n == 3 && ga->lost == 0 && ga->first == now - 3000 && ga->last == now - 500, "the cleanest copy (b, 0 lost) is the text; heard 3 times; first/last span all");
        CHECK(navTitle("ZCZC GA98\n251200 UTC NOV\nNAVAREA I 123/25\n") == "251200 UTC NOV", "the title: the first line after the header");
        const std::string rows = navMsgRows(m);
        CHECK(rows.find("\"GA98\",518,") != std::string::npos && rows.find("WIGHT") == std::string::npos, "rows carry the title, never the text: %s", rows.substr(0, 120).c_str());
        std::vector<NavMsg> lots;
        for (int i = 0; i < 1000; i++) { NavMsg x = m[0]; x.id = "GA" + std::to_string(i % 100); x.khz = i; x.last = now - i; lots.push_back(x); }
        CHECK(count(navMsgRows(lots), "],[") + 1 == (int)kNavMsgMax, "capped at kNavMsgMax");
        CHECK(dropStrField("{\"a\":1,\"txt\":\"x\\\"y\",\"b\":2}", "txt") == "{\"a\":1,\"b\":2}", "a forecast's text taken out of the poll");
        // ★★ beta96: the copies' decode quality (Stuart 11-26: "I hear it with a large red part in the middle, NickB hears it
        //    all green → use NickB's") — ranked weak % first, then clean %, then the lowest slice, then the mean SNR
        auto rec = [](const char* q) { NavMsg m; navQualityFrom(m, q, [](const std::string& l, const char* k) {
            const std::string kk = std::string("\"") + k + "\":"; const size_t p = l.find(kk); return p == std::string::npos ? 0.0 : std::strtod(l.c_str() + p + kk.size(), nullptr); }); return m; };
        const NavMsg redMid = rec("{\"nqc\":90,\"nqf\":8,\"nql\":2,\"nqd\":1,\"snr\":14.2,\"smin\":1,\"red\":22,\"tl\":[15,1]}");
        const NavMsg green  = rec("{\"nqc\":85,\"nqf\":14,\"nql\":1,\"nqd\":0,\"snr\":11.0,\"smin\":9,\"red\":0}");
        const NavMsg old    = rec("{\"f\":\"x.txt\",\"title\":\"Station G\"}");
        CHECK(redMid.hasQ && redMid.hasSnr && redMid.qClean == 90 && redMid.qFec == 8 && redMid.qLost == 2 && redMid.snr == 14 && redMid.snrMin == 1 && redMid.redPct == 22,
              "the quality read from an index line: 90/8/2 %%, 14 dB, lowest 1, 22 %% weak");
        CHECK(!old.hasQ && !old.hasSnr, "an older record: no quality");
        {
            std::vector<NavMsg> two;
            NavMsg a = redMid; a.file = "red.txt"; a.id = "GA99"; a.khz = 518; a.first = a.last = now - 100; a.text = "ZCZC GA99\nGALE\nNNNN\n"; two.push_back(a);
            NavMsg b = green;  b.file = "grn.txt"; b.id = "GA99"; b.khz = 518; b.first = b.last = now - 900; b.text = a.text; b.lost = 1; two.push_back(b);
            const auto f = navFold(two);
            CHECK(f.size() == 1 && f[0].file == "grn.txt", "★ the all-green copy beats the red-middle one, though its mean SNR is lower and it is older (%s)", f.empty() ? "" : f[0].file.c_str());
            const std::string r = navMsgRows(f);
            CHECK(r.find(",85,14,1,11,9,0]") != std::string::npos, "its row ends clean, FEC, lost %%, SNR, lowest, weak %%: %s", r.c_str());
            CHECK(r.find("\"tl\"") == std::string::npos && r.find("[15,1]") == std::string::npos, "never the slices");
            NavMsg c = a; c.hasQ = c.hasSnr = false; c.file = "o.txt"; c.id = "GB01";
            CHECK(navMsgRows({ c }).find("\"GA99\"") == std::string::npos && navMsgRows({ c }).back() == ']' && count(navMsgRows({ c }), ",") == 10,
                  "an older copy's row: the 11 members as before");
        }
    }
    std::printf("== 9. the remembered rows (sondes / APRS / VOR) beside the gallery, bounded by their windows\n");
    {
        char tmpl[] = "/tmp/vs-commlive-XXXXXX";
        const std::string dir = std::string(mkdtemp(tmpl)) + "/.community";
        CHECK(writeLive(dir, "p", "radio-1", "[[\"G4ABC\",\"G4ABC\",52.1,-1.2," + std::to_string(now - 60) + ",\"/>\",\"position\",\"car\"]]"), "radio 1 writes its APRS rows");
        CHECK(writeLive(dir, "p", "radio/2", "[[\"OLD\",\"OLD\",52.1,-1.2," + std::to_string(now - 30000) + ",\"/>\",\"position\",\"old\"],[\"M0XYZ\",\"M0XYZ\",51,-1," + std::to_string(now - 10) + ",\"/-\",\"position\",\"home\"]]"), "radio 2 writes its rows (a hostile tag is cleaned)");
        { std::ofstream(dir + "/live-p-torn.json") << "[[\"X\",1,"; }
        const std::string all = readLive(dir, "p", now);
        CHECK(count(all, "[\"") == 3, "both radios' rows joined, the torn file skipped (%s)", all.c_str());
        const std::string win = rowsSince(all, 4, now - kAprsSinceSec, 100);
        CHECK(count(win, "[\"") == 2 && win.find("OLD") == std::string::npos, "the 6 h window drops the old one: %s", win.c_str());
        CHECK(rowsSince("[\"a\",\"x,]\",9],[\"b\",\"y\",1]", 2, 5, 10) == "[\"a\",\"x,]\",9]", "the row scanner respects strings");
        CHECK(writeLive(dir, "p", "radio-1", ""), "an empty body removes the file");
        CHECK(count(readLive(dir, "p", now), "[\"") == 2, "…and its rows are gone");
    }
    std::printf("== 10. the owner's per-mode choices and the ?s= sections\n");
    {
        const auto off = offSet("vor, APRS,bogus,ndb");
        CHECK(off.size() == 3 && off.count("vor") && off.count("aprs") && off.count("ndb"), "communityOff read, unknown words dropped");
        const auto all = sectionsOfQuery("");
        CHECK(all.size() == 12, "no ?s= = every section (%zu)", all.size());
        const auto some = sectionsOfQuery("adsb,acars,navtex");
        CHECK(some.size() == 5 && some[0] == "a", "adsb+acars = one aircraft section; navtex = four");
        CHECK(sectionTtl("a") == 30 && sectionTtl("nx") == 120 && sectionTtl("z") == 120, "live 30 s, slow 120 s");
    }

    std::printf("== 6. off\n");
    CHECK(offJson("off") == "{\"v\":1,\"on\":false,\"why\":\"off\"}", "off");
    CHECK(offJson("pin\"x") == "{\"v\":1,\"on\":false,\"why\":\"pin\\\"x\"}", "escaped");

    if (const char* fx = std::getenv("COMMUNITY_FIXTURE")) {
        std::vector<vibeavshare::Row> rows;
        for (uint32_t i = 0; i < 3; i++) rows.push_back(plane(0x4CA000 + i, nowMs - i * 4000, true));
        // ★ beta96: a heavy, a helicopter and an unknown — the page test draws each class's glyph
        const vibeavshare::ExtraFn fxExtra = [](const vibeavshare::Row& r, vibeavshare::Extra& ex) {
            if (r.icao == 0x4CA000) { ex.type = "B77W"; ex.wtc = "H"; ex.species = "L"; ex.eng = "2J"; }
            if (r.icao == 0x4CA001) { ex.type = "EC35"; ex.wtc = "L"; ex.species = "H"; ex.eng = "2T"; }
        };
        std::ofstream(fx) << assemble(now, { { "a", "[" + aircraftCompact(rows, nowMs, fxExtra) + "]", aircraftLayers(rows) } });
        std::printf("   (fixture written to %s)\n", fx);
    }

    std::printf("== beta95 the door's ONE merged feed: every radio's layers, rows once\n");
    {
        // the Pi 500 shape: RSP1B (marine: aircraft modes blocked), V4L 1 (ADS-B), V4L 4 (VDL2; ADS-B blocked)
        const std::vector<std::string> radios = { "adsb,avdata,hfdl", "", "adsb" };
        CHECK(layerAllowedAnyRadio("adsb", radios) && layerAllowedAnyRadio("vdl2", radios) && layerAllowedAnyRadio("dsc", radios),
              "a layer ANY radio allows is shared");
        CHECK(!layerAllowedAnyRadio("adsb", { "adsb", "ADSB" }) && !layerAllowedAnyRadio("ais", { "aisstore", "ais" }), "blocked on every radio: not shared (case-insensitive)");
        CHECK(!layerAllowedAnyRadio("dsc", {}), "no radios: nothing");
        CHECK(modeCsvHas("wfm; adsb ,dab", "ADSB") && !modeCsvHas("adsbx", "adsb"), "the list's own tokens");
        // remembered rows from two radios: one per key, the newest, newest first
        const std::string rows = "[\"S1\",\"RS41\",50.1,0.1,1000,2,100],[\"S2\",\"RS41\",50.2,0.2,900,1,300],"
                                 "[\"S1\",\"RS41\",50.15,0.12,1100,2,200],[\"S\\\"3\",\"M10\",1,2,3,4,50]";
        const std::string d = rowsDedupe(rows, 0, 6, 32);
        CHECK(d == "[\"S2\",\"RS41\",50.2,0.2,900,1,300],[\"S1\",\"RS41\",50.15,0.12,1100,2,200],[\"S\\\"3\",\"M10\",1,2,3,4,50]",
              "one row per key, the newest, newest first: %s", d.c_str());
        CHECK(rowsDedupe(rows, 0, 6, 1) == "[\"S2\",\"RS41\",50.2,0.2,900,1,300]", "capped");
        CHECK(rowsDedupe("", 0, 6, 8).empty() && rowsDedupe("[torn", 0, 6, 8).empty(), "empty and torn: nothing");
        // bounded: 10 000 rows in, at most 4096 looked at
        std::string many;
        for (int i = 0; i < 10000; ++i) many += (i ? "," : "") + std::string("[\"K") + std::to_string(i) + "\"," + std::to_string(i) + "]";
        CHECK(rowsDedupe(many, 0, 1, 100000).size() < many.size() / 2, "bounded");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
