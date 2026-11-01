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
        // ★★ EA14 on 490 kHz, 2026-11-01 00:40 UTC — "AVURNAVKBREST _02/26 … IROISE - OESSANT - PAZENN …": the label's own
        //   pair, Corsen (520 km) or Mondolfo (1,406 km) — no positions, too few French words for the language. The map's
        //   ADDITIVE clue, the issuing authority's series among those two candidates, picks Corsen (likely).
        //   EA81, 40 s later on the same band, its header damaged ("QPPWZCZC EA"), "AVUCNAV BREST 370/26": a Start-lost
        //   item, placed by its neighbour in E's slot — Corsen (likely).
        {
            const std::string ea14 = read("20261101-004012-navtex-489k5-cc32.txt");
            navtex::Base bf;
            const auto r14 = navtex::baseResolve('E', 489500, LA, LO, ea14, nullptr, bf);
            ok(r14.stations.size() == 2 && std::string(r14.stations[0]->name) == "Corsen" && std::string(r14.stations[1]->name) == "Mondolfo",
               "EA14 (AVURNAV BREST): the proven rule's pair, Corsen or Mondolfo");
            const auto w14 = navtex::resolveAll('E', 489500, LA, LO, ea14);
            ok(w14.stations.size() == 1 && std::string(w14.stations[0]->name) == "Corsen" && w14.conf == "likely",
               "…the label (2026-11-01) now takes the series among those two: Suspected Corsen");
            ok(navmap::seriesPick(ea14, r14.stations) && std::string(navmap::seriesPick(ea14, r14.stations)->name) == "Corsen",
               "…the series AVURNAV BREST ('AVURNAVKBREST', one letter off) picks Corsen among them");
            ok(!navmap::seriesPick("WZ 649/26\nLUNDY.", r14.stations), "…and WZ (the UK's) picks neither French nor Italian candidate");
            const navmap::Agg* co = find('E', "Corsen");
            bool has14 = false, has81 = false;
            if (co) for (auto& rc : co->recent) { has14 |= rc.id == "EA14"; has81 |= rc.file == "20261101-004052-navtex-489k5-783f.txt"; }
            ok(co && has14, "EA14 is on the map at Corsen");
            ok(co && has81 && res.startLostPlaced >= 1, "EA81 (start lost, 'AVUCNAV BREST') inherits Corsen from EA14 beside it");
        }
        // ★ NORWEGIAN NAV. WARNING, three undecided titles whose positions were garbled ("02-44.67N" for 62-44.67N; a
        //   longitude "809-36,32_"): placed at the Norwegian station on that letter and band, never left as "Station N/M"
        {
            auto has = [&](const char* name, const char* file) { const navmap::Agg* a = nullptr; for (auto& x : res.stations) if (x.name == name) { for (auto& rc : x.recent) if (rc.file == file) a = &x; } return a != nullptr; };
            ok(has("Orlandet", "20261024-221012-navtex-517k5-7e1e.txt"), "NA79 (NORWEGIAN NAV. WARNING, a garbled position) → Ørlandet");
            ok(has("Jeløy", "20261027-220142-navtex-517k5-278a.txt") || has("Jeloy", "20261027-220142-navtex-517k5-278a.txt"), "MA05 (a garbled longitude) → Jeløy");
        }
        {   // ★ BA43, 518 kHz, a clean NORWEGIAN NAV. WARNING with a position 30 km from Bodø: Bodø, never Algiers
            const std::string t = "ZCZC BA43\nNORWEGIAN NAV. WARNING 356/2026\nCHART 67\nAREA ANDHOLMFJORDEN\nMAALOEY-SKARHOLMEN LIGHTHOUSE 67-46.08N 014-24.08E IS UNLIT.\nNNNN";
            const auto r = navtex::resolveAll('B', 517500, LA, LO, t);
            std::vector<const navtex::Station*> pool; size_t n = 0; const navtex::Station* tb = navtex::stations(n);
            for (size_t i = 0; i < n; ++i) if (tb[i].b1 == 'B' && tb[i].khz == 518) pool.push_back(&tb[i]);
            const navtex::Station* sp = navmap::seriesPick(t, pool);
            ok((r.stations.size() == 1 && std::string(r.stations[0]->name) == "Bodo") || (sp && std::string(sp->name) == "Bodo"), "BA43 (NORWEGIAN NAV. WARNING, 67-46N 014-24E) → Bodø");
        }
        // ★★★ 2026-11-01 — THE DECODER'S OWN LABEL (navtex::resolveAll + labelOf, as vibe_bgdecode.h titles a message), with
        //     its start time and header: the fuzzy self-ID (known names only), a corrupted header letter, the issuer
        //     series, garbled positions at no weight. scripts/test_navtex.ts runs the same cases through the clients' copy.
        {
            struct Msg { std::string id, text; double utc = NAN, hz = 0; };
            auto msg = [&](const std::string& f) {
                Msg m; const std::string raw = read(f);
                const size_t k = raw.find(" kHz"); const size_t ls = raw.rfind('\n', k);
                if (k != std::string::npos) m.hz = std::atof(raw.substr(ls + 1, k - ls - 1).c_str()) * 1000.0;
                std::tm t{}; t.tm_year = std::atoi(f.substr(0, 4).c_str()) - 1900; t.tm_mon = std::atoi(f.substr(4, 2).c_str()) - 1; t.tm_mday = std::atoi(f.substr(6, 2).c_str());
                t.tm_hour = std::atoi(f.substr(9, 2).c_str()); t.tm_min = std::atoi(f.substr(11, 2).c_str()); t.tm_sec = std::atoi(f.substr(13, 2).c_str());
                m.utc = (double)timegm(&t);
                const size_t z = raw.find("\n\nZCZC "); if (z == std::string::npos) return m;
                const size_t e1 = raw.find('\n', z + 2); m.id = raw.substr(z + 7, e1 - z - 7);
                size_t e2 = raw.find("\nNNNN", e1); if (e2 == std::string::npos) e2 = raw.find("\n[end lost]", e1);
                m.text = raw.substr(e1 + 1, (e2 == std::string::npos ? raw.size() : e2) - e1 - 1);
                return m;
            };
            auto lab = [&](const Msg& m) { return navtex::labelOf(m.id[0], navtex::resolveAll(m.id[0], m.hz, LA, LO, m.text, m.utc, m.id)); };
            auto has = [](const std::string& s, const char* x) { return s.find(x) != std::string::npos; };
            const std::string IT = "Italy \xf0\x9f\x87\xae\xf0\x9f\x87\xb9";
            { const Msg m = msg("20261025-164259-navtex-489k5-7503.txt"); const auto r = navtex::resolveAll(m.id[0], m.hz, LA, LO, m.text, m.utc, m.id);
              const std::string l = navtex::labelOf(m.id[0], r);
              ok(l == "E \xc2\xb7 Suspected Mondolfo, " + IT, "7503 (BD12, 'CONDOGNO RADIO', 16:42 — outside B's slots, inside E's) \xe2\x86\x92 Suspected Mondolfo: " + l);
              ok(!has(l, "Condogno") && r.conf == "likely", "\xe2\x80\xa6never the station that does not exist ('Condogno Radio')");
              ok(!r.why.empty() && r.why[0] == "Header letter looks corrupted: started outside B\xe2\x80\x99s slots; Mondolfo identifies itself", "\xe2\x80\xa6the why: " + (r.why.empty() ? std::string() : r.why[0])); }
            { const Msg m = msg("20261025-004258-navtex-489k5-f890.txt"); const auto r = navtex::resolveAll(m.id[0], m.hz, LA, LO, m.text, m.utc, m.id);
              ok(navtex::labelOf('E', r) == "E \xc2\xb7 Mondolfo, " + IT && r.conf == "confident", "f890 (EA12, 'MONDOLFO RADIO') stays confident Mondolfo: " + navtex::labelOf('E', r)); }
            { const Msg m = msg("20261024-221012-navtex-517k5-7e1e.txt"); const std::string l = lab(m);
              ok(has(l, "N \xc2\xb7 ") && has(l, "Orlandet, Norway"), "7e1e (NA79, NORWEGIAN NAV. WARNING, '02-44.67N' garbled) \xe2\x86\x92 \xc3\x98rlandet: " + l); }
            for (const char* f : { "20261027-220142-navtex-517k5-278a.txt", "20261028-220041-navtex-517k5-f67c.txt" }) {
                const Msg m = msg(f); const std::string l = lab(m);
                ok(l == "M \xc2\xb7 Suspected Jel\xc3\xb8y, Norway \xf0\x9f\x87\xb3\xf0\x9f\x87\xb4", std::string(f).substr(28, 4) + " (MA05, NORWEGIAN NAV. WARNING, a garbled position) \xe2\x86\x92 Suspected Jel\xc3\xb8y: " + l);
            }
            { const Msg m = msg("20261101-004012-navtex-489k5-cc32.txt"); const std::string l = lab(m);
              ok(l == "E \xc2\xb7 Suspected Corsen, France \xf0\x9f\x87\xab\xf0\x9f\x87\xb7", "cc32 (EA14, AVURNAV BREST): the label's pair settled by the series \xe2\x86\x92 Suspected Corsen: " + l); }
            auto at = [](int y, int mo, int d, int h, int mi, int sec) { std::tm t{}; t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_sec = sec; return (double)timegm(&t); };
            { const std::string t = "240040 UTC OCT 26\nMTNDOOFO AADIO\n\nCOSTAVURNAV 396/26 AGO 30\nMAR ADRIATICO CENTRALE - TERMOLI.\nIL FARO RISULTA NON FUNZIONANTE.";
              const std::string l = navtex::labelOf('J', navtex::resolveAll('J', 489500, LA, LO, t, at(2026, 10, 24, 0, 43, 0), "JA12"));
              ok(l == "E \xc2\xb7 Suspected Mondolfo, " + IT, "synthetic JA12 'MTNDOOFO AADIO', 490, 00:43 \xe2\x86\x92 Suspected Mondolfo (E): " + l);
              const std::string l2 = navtex::label('J', 489500, LA, LO, t);
              ok(!has(l2, "Talcahuano"), "\xe2\x80\xa6" "and with no time, never Talcahuano, Chile (12,000 km): " + l2); }
            { const std::string t = "NCC-AMBURG\nNAV. WARN. NO. 563\nGERMAN BIGHT\nWINDPARK BUTENDIEK LIGHTS UNRELIABLE";
              const auto r = navtex::resolveAll('C', 517500, LA, LO, t, at(2026, 10, 5, 23, 1, 15), "CS");
              const std::string l = navtex::labelOf('C', r);
              ok(l == "S \xc2\xb7 Suspected Hamburg, Germany \xf0\x9f\x87\xa9\xf0\x9f\x87\xaa", "synthetic 'ZCZC CS' NCC-AMBURG, 518, 23:01 \xe2\x86\x92 Suspected Hamburg (S): " + l);
              ok(!has(l, "Odes") && r.why.size() > 1 && has(r.why[1], "shifted"), "\xe2\x80\xa6never Odesa; the letters look shifted (CS)"); }
            { const std::string t = "180550 NAVTEXJAHAMBURG (NCC)\nSEEWETTERVORHERSAGE\nVORHERSAGE FUER SONNTAG:\nDEUTSCHE BUCHT\nNE 4 BIS 5, SEE ZTW. 3M.\nWESTLICHE OSTSEE\nOST 4, SPAETER 5 UND DIE SEE";
              const std::string l = navtex::labelOf('V', navtex::resolveAll('V', 489500, LA, LO, t, at(2026, 10, 18, 5, 50, 51), "VL"));
              ok(l == "L \xc2\xb7 Suspected Hamburg, Germany \xf0\x9f\x87\xa9\xf0\x9f\x87\xaa", "synthetic 'ZCZC VL' NAVTEXJAHAMBURG, 490, 05:50 \xe2\x86\x92 Suspected Hamburg (L), never Yarmouth: " + l);
              const std::string sl = navtex::startLostLabel(489500, LA, LO, "ZCZC VL\n" + t, at(2026, 10, 18, 5, 50, 51));
              ok(sl == l, "\xe2\x80\xa6the same as a start-lost message whose text begins 'ZCZC VL': " + sl); }
            { const std::string t = "NORWEGIAN NAV. WARNING 356/2026\nCHART 67\nAREA ANDHOLMFJORDEN\nMAALOEY-SKARHOLMEN LIGHTHOUSE 67-46.08N 014-24.08E IS UNLIT.";
              const std::string l = navtex::labelOf('B', navtex::resolveAll('B', 517500, LA, LO, t, at(2026, 10, 8, 4, 10, 41), "BA43"));
              ok(has(l, "B \xc2\xb7 Bodo, Norway") && !has(l, "Algiers"), "synthetic BA43 (Norwegian, 67-46N 014-24E) \xe2\x86\x92 Bod\xc3\xb8, never Algiers: " + l); }
            { const std::string t = "281446 UTC SEP 26\nNORWEGIAN NAV. WARNING 347/2026\nCHART 84\nAREA GIBOSTAD\nNORDGRUNNEN LIGHT 69-21.40N 018-05.84E IS UNLIT.";
              const auto r = navtex::resolveAll('B', 517500, LA, LO, t, at(2026, 10, 7, 0, 15, 47), "BA41");
              ok(has(navtex::labelOf('B', r), "B \xc2\xb7 Bodo, Norway") && r.conf == "confident", "UberSDR's BA41 (clean, Gibostad 69-21N 018-05E) \xe2\x86\x92 Bod\xc3\xb8, confident, never Algiers: " + navtex::labelOf('B', r)); }
            { const std::string g1 = "STATHMOS IRAKLEIOY AR.MIN 370/26\nKRITIKO PELAGOS", g2 = "STATHMOS KERKYRA AR.MIN 227/25\nIONIO PELAGOS";
              ok(has(navtex::label('Q', 489500, LA, LO, g1), "Q \xc2\xb7 Irakleio, Greece"), "'STATHMOS IRAKLEIOY' (Greek genitive) \xe2\x86\x92 Irakleio");
              ok(has(navtex::label('P', 489500, LA, LO, g2), "P \xc2\xb7 Kerkyra, Greece"), "'STATHMOS KERKYRA' \xe2\x86\x92 Kerkyra");
              ok(navtex::selfId("STATHMOS I_AKLEIO AR.MUN. 223/26", {}, {}).L == "IRAKLEIO", "'STATHMOS I_AKLEIO' (a lost letter) \xe2\x86\x92 Irakleio"); }
            { const std::string none = navtex::label('T', 489500, LA, LO, "ZCZC TA17\nWIDGET RADIO\nMAR ADRIATICO 44-31N 012-30E\nI SISTEMI DI AUSILIO NON SONO FUNZIONANTI SULLE PIATTAFORME");
              ok(none == "Station T", "an unknown '<NAME> RADIO' is never shown \xe2\x80\x94 the letter only: " + none);
              ok(navtex::selfId("MONDOLFO RADIO", {}, {}).L == "MONDOLFO" && navtex::selfId("PHIT RADIO", {}, {}).L.empty(), "'PHIT RADIO' (2 off SPLIT, nothing backing it) is ignored"); }
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
