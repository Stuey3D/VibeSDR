// test-navtex-map.cpp — V12 THE NAVTEX MAP, stage 1 (vibe_navtex_map.h, 2026-11-01): the gallery titles read back, the
// station each is placed at (the label's own, one resolver), what the label could not decide (the time of day, the
// station's other messages, the message's own text), the fold per station, bounded, and the real fixture set
// (scripts/navtex-fixtures — real messages from Stuart's Lenovo).
//   test-navtex-map [<index.jsonl> <files dir>]   (default: scripts/navtex-fixtures)
// Build: g++ -std=c++17 -O2 -I android/app/src/main/cpp -o test-navtex-map vibeserver/test-navtex-map.cpp
#include "vibe_navtex_map.h"
#include "vibe_gallery.h"
#include <cstdlib>

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
    std::printf("── the schedule slot ──\n");
    {
        const int64_t day = 1793491200;   // 2026-11-01 00:00 UTC
        ok(navmap::slotFits('A', day + 300) && navmap::slotFits('T', day + 3 * 3600 + 14 * 60 + 48) && navmap::slotFits('E', day + 4 * 3600 + 40 * 60 + 600),
           "A ending 00:05, T ending 03:14:48 (slot 03:10), E ending 04:50 (slot 04:40, every 4 h): in their slots");
        ok(!navmap::slotFits('A', day + 1800) && !navmap::slotFits('Z', day), "A ending 00:30, and Z (no slot): not");
        ok(navmap::sharing('E', 518) >= 2, "several stations share E on 518 kHz worldwide");
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
    std::printf("── transmission runs ──\n");
    {
        // a run in E's slot (00:40, 04:40 …): one message named Niton outright, then two whose label could not decide
        const int64_t c = 1793491200 + 4 * 3600 + 40 * 60;   // 04:40 UTC
        std::vector<std::string> lines = {
            line("E \xc2\xb7 Niton, United Kingdom \xc2\xb7 Nav warning \xc2\xb7 #11", 517500, c + 120),
            line("Station E \xc2\xb7 Nav warning \xc2\xb7 #12", 517500, c + 300),
            line("E \xc2\xb7 Niton \xf0\x9f\x87\xac\xf0\x9f\x87\xa7 or Corsen \xf0\x9f\x87\xab\xf0\x9f\x87\xb7? \xc2\xb7 Nav warning \xc2\xb7 #13", 517500, c + 480),
            line("Station E \xc2\xb7 Nav warning \xc2\xb7 #14", 517500, c + 4 * 3600 + 300),   // the NEXT cycle: no run with a name
        };
        const navmap::EachLine each = [&](const std::function<void(const std::string&)>& fn) { for (auto& l : lines) fn(l); };
        const auto r = navmap::scan(each, LA, LO, 0, [](const std::string&) { return std::string(); });
        uint64_t niton = 0; for (auto& a : r.stations) if (a.name == "Niton") niton += a.n;
        ok(r.byRun == 2, "the two undecided messages in Niton's run inherit it (" + std::to_string((unsigned long long)r.byRun) + ")");
        ok(niton >= 3, "…Niton holds the run's three messages");
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
        // ★ EA14 on 490 kHz, 2026-11-01 00:40 UTC — "AVURNAV BREST … IROISE - OUESSANT …": a French warning, Corsen's. The
        //   label could not decide between the two E stations on 490 (Corsen 520 km, Mondolfo 1,406 km: no positions, too
        //   few French words to count as the language), and the map does not guess either: a "?" with Corsen, the nearer,
        //   first — the issuing authority's series name is not used, because stations relay each other's series (measured:
        //   264 of 2 290 named messages carry another country's series, e.g. Oostende sending WZ).
        {
            const std::string ea14 = read("20261101-004012-navtex-489k5-cc32.txt");
            const auto r14 = navtex::resolveAll('E', 489500, LA, LO, ea14);
            ok(r14.stations.size() == 2 && std::string(r14.stations[0]->name) == "Corsen" && std::string(r14.stations[1]->name) == "Mondolfo",
               "EA14 (AVURNAV BREST): the label's pair, Corsen (520 km) or Mondolfo (1,406 km) — not one station named");
        }
        const std::string j = navmap::toJson(res, LA, LO, 1793560000);
        ok(j.find("\"stations\":[") != std::string::npos && j.find("\"rx\":{\"lat\":52.3020") != std::string::npos && j.size() < 200000, "the JSON: stations, the receiver, bounded (" + std::to_string(j.size()) + " B)");
    }
    std::printf("── through the Store's live view (append-only index: replacements and removals) ──\n");
    {
        char tmpl[256]; std::snprintf(tmpl, sizeof tmpl, "%s/navmap-store-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp");
        const std::string dir = mkdtemp(tmpl) ? std::string(tmpl) : std::string("/tmp/navmap-store");
        gallery::Store st; st.setDir(dir); st.setLimitBytes(64ull << 10);   // ★ 64 KB: older items must go
        const std::string body(1500, 'x');
        std::vector<std::string> names;
        for (int i = 0; i < 80; ++i) {
            gallery::Meta m; m.kind = "navtex"; m.origin = "background"; m.freqHz = 517500; m.mode = "usb"; m.decoder = "navtex";
            m.utc = 1793500000 + i * 60;
            m.title = std::string("G \xc2\xb7 Cullercoats, United Kingdom \xc2\xb7 Nav warning \xc2\xb7 #") + (i < 10 ? "0" : "") + std::to_string(i % 100);
            const std::string n = st.create(m, "txt", body);
            st.close(n);
            names.push_back(n);
        }
        // one item replaced in place: it becomes Niton's — counted once, under its new title
        gallery::Meta m; m.kind = "navtex"; m.origin = "background"; m.freqHz = 517500; m.mode = "usb"; m.decoder = "navtex"; m.utc = 1793500000 + 79 * 60;
        m.title = "E \xc2\xb7 Niton, United Kingdom \xc2\xb7 Nav warning \xc2\xb7 #79";
        const bool rep = st.replace(names.back(), m, body);
        const size_t live = st.itemCount();
        const std::string rawIdx = [&] { std::string o; FILE* f = std::fopen((dir + "/index.jsonl").c_str(), "r"); char b[4096]; size_t n; while (f && (n = std::fread(b, 1, sizeof b, f)) > 0) o.append(b, n); if (f) std::fclose(f); return o; }();
        size_t rawLines = 0; for (char c : rawIdx) rawLines += c == '\n';
        const navmap::EachLine each = [&st](const std::function<void(const std::string&)>& fn) { st.forEachLive("navtex", fn); };
        const auto r = navmap::scan(each, LA, LO, 0);
        uint64_t total = 0; for (auto& a : r.stations) total += a.n;
        auto n = [&](const char* name) { for (auto& a : r.stations) if (a.name == name) return a.n; return (uint64_t)0; };
        std::printf("    %zu live items, %zu raw index lines; the map counted %llu\n", live, rawLines, (unsigned long long)total);
        ok(rep && live < 80 && rawLines > live, "the store evicted and replaced (raw index " + std::to_string(rawLines) + " lines, " + std::to_string(live) + " live)");
        ok(total == live, "the map counts exactly the live items — never a removed or a replaced line");
        ok(n("Niton") == 1 && n("Cullercoats") == live - 1, "the replaced item counts once, under its new title");
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
