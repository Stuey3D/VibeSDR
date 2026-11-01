// test-navtex-map.cpp — V12 THE NAVTEX MAP, stage 1 (vibe_navtex_map.h, 2026-11-01): the gallery titles read back, the
// station each is placed at (the label's own, one resolver), what the label could not decide (the time of day, the
// station's other messages, the message's own text), the fold per station, bounded, and the real fixture set
// (scripts/navtex-fixtures — real messages from Stuart's Lenovo).
//   test-navtex-map [<index.jsonl> <files dir>]   (default: scripts/navtex-fixtures)
// Build: g++ -std=c++17 -O2 -I android/app/src/main/cpp -o test-navtex-map vibeserver/test-navtex-map.cpp
#include "vibe_navtex_map.h"

#include <cstdio>
#include <string>

using namespace vibe;
static int fails = 0, passes = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); (c ? passes : fails)++; }
static const double LA = 52.3020, LO = -0.8528;   // IO92nh (Moulton, Northamptonshire)

static std::string line(const std::string& title, double hz, int64_t t, const std::string& f = "x.txt") {
    return "{\"f\":\"" + f + "\",\"k\":\"navtex\",\"o\":\"background\",\"t\":" + std::to_string((long long)t) + ",\"hz\":" + std::to_string((long long)hz)
         + ",\"m\":\"usb\",\"d\":\"navtex\",\"p\":\"Profile 1\",\"r\":\"\",\"title\":\"" + navmap::jesc(title) + "\"}";
}

int main(int argc, char** argv) {
    std::printf("test-navtex-map — the NAVTEX stations received\n");
    std::printf("── the titles ──\n");
    {
        auto t = navmap::parseTitle("G \xc2\xb7 Cullercoats, United Kingdom \xf0\x9f\x87\xac\xf0\x9f\x87\xa7 \xc2\xb7 Nav warning \xc2\xb7 #98 \xc2\xb7 no errors");
        ok(t.b1 == 'G' && t.name == "Cullercoats" && t.b2 == 'A' && t.num == "98" && t.lost == 0 && !t.pair && !t.generic, "one station named: G, Cullercoats, A, 98, no errors");
        t = navmap::parseTitle("G \xc2\xb7 Halifax, Canada \xf0\x9f\x87\xa8\xf0\x9f\x87\xa6 \xc2\xb7 DX 4,700 km \xc2\xb7 Met warning \xc2\xb7 #02");
        ok(t.b1 == 'G' && t.name == "Halifax" && t.b2 == 'B' && t.num == "02", "…a DX one (the DX field skipped)");
        t = navmap::parseTitle("W \xc2\xb7 Corsen \xf0\x9f\x87\xab\xf0\x9f\x87\xb7 or Valentia \xf0\x9f\x87\xae\xf0\x9f\x87\xaa? \xc2\xb7 Nav warning (more) \xc2\xb7 #12");
        ok(t.pair && t.name == "Corsen" && t.alt == "Valentia" && t.b2 == 'L', "the label's pair: Corsen or Valentia, L");
        t = navmap::parseTitle("Station F \xc2\xb7 Weather forecast \xc2\xb7 #40 \xc2\xb7 no errors");
        ok(t.generic && t.b1 == 'F' && t.b2 == 'E' && t.num == "40", "\"Station F\": undecided, E, 40");
        t = navmap::parseTitle("Start lost \xc2\xb7 #00");
        ok(!t.b1, "a message joined part way: no letter, not placed");
    }
    std::printf("── the night ──\n");
    {
        // 2026-11-01 00:00 UTC and 12:00 UTC at a mid-Atlantic midpoint
        const int64_t mid = 1793491200, noon = mid + 12 * 3600;
        ok(navmap::sunElev(48, -30, mid) < -6 && navmap::sunElev(48, -30, noon) > 0, "the sun: down at 00 UTC, up at 12 UTC mid-Atlantic");
        size_t n = 0; const navtex::Station* t = navtex::stations(n);
        const navtex::Station* boston = nullptr; for (size_t i = 0; i < n; ++i) if (std::string(t[i].name) == "Boston" && t[i].khz == 518) boston = &t[i];
        ok(boston && navmap::plausible(boston, LA, LO, mid) && !navmap::plausible(boston, LA, LO, noon), "Boston 518 (5 300 km) from England: plausible at night, not at noon");
    }
    std::printf("── the fold ──\n");
    {
        const int64_t T = 1793500000;
        std::map<std::string, navmap::Agg> by; navmap::Result r;
        const std::string g = "G \xc2\xb7 Cullercoats, United Kingdom \xf0\x9f\x87\xac\xf0\x9f\x87\xa7 \xc2\xb7 Nav warning \xc2\xb7 #";
        for (int i = 0; i < 30; ++i) navmap::take(by, r, line(g + (i % 3 == 0 ? "98" : "02"), 517500, T + i * 600), LA, LO, 0);
        ok(by.size() == 1 && by.begin()->second.n == 30 && by.begin()->second.ids.size() == 2, "30 Cullercoats messages, 2 different: one station, ×30, 2 different (the map's dedupe)");
        ok(by.begin()->second.conf == "confident" && by.begin()->second.recent.size() == navmap::kRecent && by.begin()->second.recent[0].t == T + 29 * 600, "confident, the 12 newest ids, newest first");
        navmap::take(by, r, line("Start lost \xc2\xb7 #00", 517500, T), LA, LO, 0);
        ok(r.unplaced == 1, "a start-lost message is counted as not placed");
        navmap::take(by, r, line(g + "11", 517500, T - 999999), LA, LO, T - 1000);
        ok(by.begin()->second.n == 30, "`since`: an older message is left out");
    }
    std::printf("── the real fixture set (Stuart's Lenovo, IO92nh) ──\n");
    {
        const std::string idx = argc > 2 ? argv[1] : "scripts/navtex-fixtures/index.jsonl";
        const std::string dir = argc > 2 ? argv[2] : "scripts/navtex-fixtures/files/";
        auto read = [&](const std::string& f) { std::string o; FILE* fp = std::fopen((dir + "/" + f).c_str(), "r"); if (!fp) return o; char b[16384]; const size_t n = std::fread(b, 1, sizeof b, fp); o.assign(b, n); std::fclose(fp); return o; };
        const auto res = navmap::scanIndex(idx, LA, LO, 0, read);
        auto find = [&](char b1, const char* name) -> const navmap::Agg* { for (auto& a : res.stations) if (a.b1 == b1 && a.name == name) return &a; return nullptr; };
        std::printf("    %zu stations, %llu messages, %llu not placed\n", res.stations.size(), (unsigned long long)res.messages, (unsigned long long)res.unplaced);
        ok(res.stations.size() >= 20, "at least 20 stations from the fixtures");
        ok(find('T', "Oostende") && find('E', "Niton") && find('G', "Cullercoats") && find('O', "Portpatrick"), "the near ones: T Oostende, E Niton, G Cullercoats, O Portpatrick");
        const navmap::Agg* tl = find('F', "Tallinn");
        ok(tl && tl->conf == "likely", "\"Station F\" on 518 (Estonian warnings, Gulf of Finland positions) → Tallinn, likely");
        ok(tl && std::fabs(tl->st->lat - 59.5) < 0.01, "…at Pub. 117's Tallinn position");
        bool sorted = true; for (size_t i = 1; i < res.stations.size(); ++i) if (res.stations[i - 1].last < res.stations[i].last) sorted = false;
        ok(sorted, "most recently heard first");
        const std::string j = navmap::toJson(res, LA, LO, 1793560000);
        ok(j.find("\"stations\":[") != std::string::npos && j.find("\"rx\":{\"lat\":52.3020") != std::string::npos && j.size() < 200000, "the JSON: stations, the receiver, bounded (" + std::to_string(j.size()) + " B)");
    }
    std::printf("── bounded ──\n");
    {
        std::map<std::string, navmap::Agg> by; navmap::Result r;
        size_t n = 0; const navtex::Station* t = navtex::stations(n);
        for (int rep = 0; rep < 3; ++rep)
            for (size_t i = 0; i < n; ++i) {
                const std::string title = std::string(1, t[i].b1) + " \xc2\xb7 " + t[i].name + ", " + t[i].country + " \xc2\xb7 Nav warning \xc2\xb7 #01";
                navmap::take(by, r, line(title, t[i].khz * 1000, 1793500000 + (int64_t)i), LA, LO, 0);
            }
        ok(by.size() <= navmap::kMaxStations && by.size() >= 200, "every station in the world table: " + std::to_string(by.size()) + " ≤ kMaxStations");
    }
    std::printf("%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
