// test-navtex-warn.cpp — V12 THE NAVTEX MAP, stage 2 (vibe_navtex_warn.h, 2026-11-01): the strict position parser (the
// garbled shapes refused), the hazard classes (whole words: "MINE" never in DETERMINED / EXAMINED / MINERAL), series and
// cancellations, one marker per warning (repeats folded, the best copy kept, a number reused after 30 days a new one),
// expiry, the index beside the gallery (sync in chunks, evicted items skipped, compaction), and the real fixtures.
//   test-navtex-warn [<index.jsonl> <files dir>]   (default: scripts/navtex-fixtures)
// Build: g++ -std=c++17 -O2 -I android/app/src/main/cpp -o test-navtex-warn vibeserver/test-navtex-warn.cpp
#include "vibe_navtex_warn.h"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <string>

using namespace vibe;
static int fails = 0, passes = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); (c ? passes : fails)++; }
static size_t npos(const std::string& s) { return navtex::positions(navwarn::posText(s)).size(); }

int main(int argc, char** argv) {
    std::printf("test-navtex-warn — the NAVTEX warnings on the map\n");
    std::printf("── positions: strict ──\n");
    {
        ok(npos("ANCHOR AND CHAIN LOST IN POS 51-24,32N 002-40,82E") == 1, "51-24,32N 002-40,82E (a comma decimal)");
        ok(npos("IN POS 51-23.730N 002-29.791E AND\n51-23.557N 002-30.286E") == 2, "two points across a line break");
        ok(npos("NARVA DGNSS STATION IN POSITION 5928.02N, 02802.36E") == 1, "5928.02N, 02802.36E (no dash)");
        ok(npos("LAT 40 23.4'N LONG 018 18.4'E IS NOT WORKING") == 1, "LAT 40 23.4'N LONG 018 18.4'E (LAT/LONG, the minutes mark)");
        const auto p = navtex::positions(navwarn::posText("LAT 40 23.4'N LONG 018 18.4'E"));
        ok(p.size() == 1 && std::fabs(p[0].first - 40.39) < 0.001 && std::fabs(p[0].second - 18.3067) < 0.001, "…at 40.390N 18.307E");
        // ★ the garbled shapes from a Spanish warning (MA88): each REFUSED — a false position is worse than none
        ok(npos("39-56.45NK000-00.90E") == 0, "39-56.45NK000-00.90E (a letter in the gap): refused");
        ok(npos("39-53:54N 000-0.(1E") == 0, "39-53:54N 000-0.(1E (a colon for the decimal, a bracket in the minutes): refused");
        ok(npos("39-5.89N 200-00.02E") == 0, "39-5.89N 200-00.02E (one minute digit, longitude 200): refused");
        ok(npos("WZ 649/26 FREQ 518 KHZ 2026 1234") == 0, "numbers that are not positions: nothing");
        ok(npos("51-15.1N 001-54.5E MISSING") == 1, "51-15.1N 001-54.5E (one decimal)");
        // ★ Greek hemisphere letters (B north, N south, A east, D west) — only in a message identified as Greek
        const auto gr = navtex::positions(navwarn::posText("STATHMOS IRAKLEIOY\nPERIOCHI 35-28.30B 024-14.10A"));
        ok(gr.size() == 1 && std::fabs(gr[0].first - 35.4717) < 0.001 && std::fabs(gr[0].second - 24.235) < 0.001, "Greek: 35-28.30B 024-14.10A → 35.472N 24.235E");
        const auto gs = navtex::positions(navwarn::posText("NISOS KRITI 34-50.00N 025-10.00D"));
        ok(gs.size() == 1 && gs[0].first < 0 && gs[0].second < 0, "Greek: N is SOUTH, D is WEST");
        ok(npos("35-28.30B 024-14.10A") == 0, "the same without Greek words: B / A are not hemispheres (refused)");
        ok(navtex::positions(navwarn::posText("51-14.5N 003-11.4W"))[0].first > 0, "an English N stays north");
    }
    {   // ★ NA79: "02-44.67N 006-21.03E" (62-44.67N with its first digit lost) — far from every N station on 518: not plotted
        const auto e = navwarn::fromMessage("na79.txt", 1793400000, 517500, "ZCZC NA79\nNORWEGIAN NAV. WARNING 393/2026\nCHART 31\nPOSITION 02-44.67N 006-21.03E\nNNNN");
        ok(e.pts.empty(), "NA79's garbled 02-44.67N (Gulf of Guinea for a Norwegian chart): not plotted");
        const auto ok2 = navwarn::fromMessage("na80.txt", 1793400000, 517500, "ZCZC NA80\nNORWEGIAN NAV. WARNING 394/2026\nPOSITION 62-44.67N 006-21.03E\nNNNN");
        ok(ok2.pts.size() == 1, "…its true 62-44.67N is");
    }
    std::printf("── the classes ──\n");
    {
        auto c = [](const char* t, char b2 = 'A') { return navwarn::classify(b2, t).id; };
        ok(c("DANGEROUS WRECK IN POSITION") == "wreck" && c("SUNKEN VESSEL") == "wreck", "WRECK / SUNKEN → wreck");
        ok(c("UNEXPLODED ORDNANCE FOUND") == "uxo" && c("MINE SIGHTED") == "uxo" && c("WWII MUNITIONS DUMPED") == "uxo", "UNEXPLODED ORDNANCE / MINE / MUNITIONS → uxo");
        ok(c("POSITION DETERMINED BY GPS") != "uxo" && c("CHART EXAMINED") != "uxo" && c("MINERAL EXTRACTION") != "uxo", "…never DETERMINED, EXAMINED, MINERAL (whole words)");
        ok(c("GUNNERY FIRING EXERCISES") == "firing" && c("SUBFACTS AND GUNFACTS WARNING") == "firing", "FIRING / GUNNERY / GUNFACTS → firing");
        ok(c("WIND FARM CONSTRUCTION") == "structure", "WIND FARM → structure");
        const auto t = navwarn::classify('A', "WIND TURBINE T12 LIGHT INOPERATIVE");
        ok(t.id == "structure" && t.unlit, "a turbine with its light inoperative: structure + unlit");
        const auto lh = navwarn::classify('A', "LIGHTHOUSE OF S. CATALDO OF LECCE I.L. 3612 (E2192),\nIN POSITION:\nLAT 40 23.4'N\nIS NOT WORKING.");
        ok(lh.id == "navaid" && lh.unlit, "LIGHTHOUSE … IS NOT WORKING: navaid + unlit");
        ok(c("BUOY MISSING") == "navaid" && c("RACON INOPERATIVE") == "navaid" && c("FOGHORN OUT OF ORDER") == "navaid", "BUOY / RACON / FOGHORN → navaid");
        ok(c("CONTAINERS LOST OVERBOARD") == "adrift" && c("DERELICT YACHT ADRIFT") == "adrift", "CONTAINERS LOST / DERELICT → adrift");
        ok(c("CABLE LAYING OPERATIONS") == "cable" && c("SEISMIC SURVEY") == "survey" && c("TOW IN PROGRESS") == "towing", "cable, survey, towing");
        ok(c("VESSEL OVERDUE", 'D') == "sar" && c("ANYTHING", 'D') == "sar", "B2 = D or OVERDUE → sar");
        ok(c("GALE WARNING", 'B') == "met" && c("ICE", 'C') == "ice", "B2 = B → met; C → ice");
        ok(c("NAVIGATION RESTRICTED") == "general", "nothing recognised → general (never a guess)");
    }
    std::printf("── the lexicon (real wording) ──\n");
    {
        auto cl = [](const char* t, char b2 = 'A') { return navwarn::classify(b2, t); };
        auto k = cl("OOSTENDERADIO MSI 327/26\nAPPROACH ZEEBRUGGE\nWESTERN PIER\nFOGHORN OUT OF ORDER");
        ok(k.id == "navaid", "EN: FOGHORN OUT OF ORDER → navaid");
        k = cl("AVURNAV BREST 370/26\nOUESSANT\nZONE D'EXCLUSION\nPROJET CELTIC INTERCONNECTOR\nOPERATIONS CABLIERES\nPOSE DE CABLES SOUS-MARINS");
        ok(k.id == "cable", "FR: POSE DE CABLES SOUS-MARINS → cable");
        k = cl("FEU ETEINT\nBOUEE CARDINALE NORD");
        ok(k.id == "navaid" && k.unlit && k.lang == "fr" && k.hit.find("ETEINT") != std::string::npos, "FR: FEU ETEINT, BOUEE → navaid + unlit, from French: " + k.hit);
        ok(cl("BOYA APAGADA EN POSICION").unlit && cl("BOYA APAGADA EN POSICION").id == "navaid", "ES: BOYA APAGADA → navaid + unlit");
        ok(cl("FANALE SPENTO").id == "navaid" && cl("FANALE SPENTO").unlit, "IT: FANALE SPENTO → navaid + unlit");
        ok(cl("RELITTO PERICOLOSO").id == "wreck" && cl("PECIO EN POSICION").id == "wreck" && cl("EPAVE DANGEREUSE").id == "wreck", "IT/ES/FR wreck words");
        ok(cl("ESERCITAZIONI DI TIRO").id == "firing" && cl("SCHIESSUEBUNGEN").id == "firing", "IT/DE firing words");
        ok(cl("STATHMOS KERKYRA AR.MIN 227/25\n1. ASKISEIS PYRON AEROSKAFON APO 01 IAN 26 EOS 31 DEK 26").id == "firing", "EL: ASKISEIS PYRON → firing");
        ok(cl("ORDIGNO BELLICO RINVENUTO").id == "uxo" && cl("PARCO EOLICO OFFSHORE").id == "structure" && cl("BOA ALLA DERIVA").id == "navaid", "IT: uxo, wind farm; a buoy adrift is a navaid");
        ok(cl("CONTAINER ALLA DERIVA").id == "adrift", "IT: something else adrift → adrift");
        const auto wd = navwarn::windowOf("1. ASKISEIS PYRON AEROSKAFON APO 01 IAN 26 EOS 31 DEK 26", 1793488800);
        ok(wd.to > 0 && wd.to - 1798761540 < 86400 && wd.to >= 1798675200, "EL window APO 01 IAN 26 EOS 31 DEK 26 → ends 31 Dec 2026");
        ok(navwarn::windowOf("FROM 01 NOV TO 15 NOV 2026", 1793488800).to > 1793488800, "EN window FROM 01 NOV TO 15 NOV 2026");
        ok(navwarn::windowOf("VALID UNTIL FURTHER NOTICE", 1793488800).ufn && navwarn::windowOf("JUSQU'A NOUVEL AVIS", 1793488800).ufn, "UNTIL FURTHER NOTICE (and French)");
        ok(navwarn::windowOf("MONDAY FROM SUNRISE TO SUNSET", 1793488800).to == 0, "a recurring day window is not read");
    }
    std::printf("── series and cancellations ──\n");
    {
        ok(navwarn::seriesOf("WZ 649/26\nLUNDY.") == "WZ 649/26" && navwarn::seriesKey("WZ 649/26") == "WZ|649/26", "WZ 649/26 → key WZ|649/26");
        // ★ a number is NOT unique across issuers: a Greek warning's 370/26 is not AVURNAV BREST 370/26
        ok(navwarn::seriesKey(navwarn::seriesOf("STATHMOS IRAKLEIOY\nNAVTEX 370/26\nPERIOCHI")) != navwarn::seriesKey(navwarn::seriesOf("AVURNAV BREST 370/26"))
           && navwarn::seriesKey("370/26").empty(), "the Greek 370/26 does not link to AVURNAV BREST 370/26; a bare number links nothing");
        ok(navwarn::seriesKey(navwarn::seriesOf("AVURNAV BREST 370/26")) == navwarn::seriesKey(navwarn::seriesOf("AVUCNAV BREST 370/26")), "AVURNAV / AVUCNAV BREST 370/26: one key");
        const auto cx = navwarn::cancelsOf("1. SANDETTIE WEST BUOY MISSING.\n2. CANCEL WZ 476/26 (EA77)(VA08).", 1793500000);
        ok(cx.ids.size() == 2 && cx.ids[0] == "EA77" && cx.series.size() == 1 && navwarn::seriesKey(cx.series[0]) == "WZ|476/26", "CANCEL WZ 476/26 (EA77)(VA08): ids EA77 VA08, series WZ 476/26");
        const auto self = navwarn::cancelsOf("CANCEL THIS MESSAGE 302200 UTC NOV", 1793500000);
        ok(self.at > 1793500000 && self.at - 1793500000 < 40 * 86400, "CANCEL THIS MESSAGE 302200 UTC NOV: an end time");
        const auto ua = navwarn::fromMessage("ua30.txt", 1793488800, 517500, "ZCZC UA30\nMONDOLFO RADIO\nCOASTAL WARNING 510/26 OCT 24 (CANCELED)\nLIGHTHOUSE … IS NOT WORKING.\nLAT 40 23.4'N LONG 018 18.4'E\nCOSTAVURNAV 510/26 OCT 24 IS CANCELED\nNNNN");
        ok(ua.cls == "cancel" && ua.cSeries.find("510/26") != std::string::npos, "UA30 '510/26 … IS CANCELED': a cancellation of 510/26, not a hazard to plot");
    }
    std::printf("── one marker per warning ──\n");
    {
        char tmpl[256]; std::snprintf(tmpl, sizeof tmpl, "%s/navwarn-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp");
        const std::string dir = mkdtemp(tmpl) ? std::string(tmpl) : std::string("/tmp");
        const std::string idx = dir + "/navtex-warn.idx";
        struct M { std::string f; int64_t t; std::string text; };
        const int64_t T = 1793400000;
        std::vector<M> ms = {
            {"a1", T,          "ZCZC EA45\nWZ 649/26\nLUNDY.\n1. NORTH CARDINAL BUOY 51-14.5N 003-11.4W TEMPORARILY REMOVED.\nNNNN"},
            {"a2", T + 14400,  "ZCZC EA45\nWZ 649/26\nLUNDY.\n1. NORTH CARDINAL BUOY 51-14.5N 003-11.4W TEMPORARILY REMOVED.\nNNNN"},             // a repeat
            {"a3", T + 28800,  "ZCZC EA45\nWZ 6*9/26\nLUNDY.\n1. NORTH CARD*NAL BUOY 51-14.5N 003-11.4W TEMPORARILY REM_VED.\nNNNN"},            // a garbled copy
            {"b1", T + 3600,   "ZCZC EA46\nWZ 650/26\nTHAMES.\n1. WRECK 51-30.0N 001-30.0E.\nNNNN"},
            {"c1", T + 7200,   "ZCZC EA47\nWZ 651/26\nTHAMES.\n1. CANCEL WZ 650/26 (EA46).\nNNNN"},                                           // cancels b1
            {"d1", T - 40 * 86400, "ZCZC EA45\nWZ 101/26\nDOVER.\n1. SURVEY VESSEL 51-00.0N 001-30.0E.\nNNNN"},                         // EA45 40 days earlier: another warning
            {"e1", T + 4000,   "ZCZC EA48\nWZ 652/26\nDOVER.\n1. FIRING EXERCISES IN AREA 50-50.0N 001-00.0E 50-55.0N 001-00.0E 50-55.0N 001-10.0E 50-50.0N 001-10.0E.\nNNNN"},
            {"x1", T + 5000,   "ZCZC EA49\nWZ 653/26\nDOVER.\n1. BUOY 51-01.0N 001-31.0E.\nNNNN"},                                           // will be evicted
        };
        std::vector<M> live = ms;
        const navwarn::LiveFn each = [&](const std::function<void(const std::string&)>& fn) {
            for (auto& m : live) fn("{\"f\":\"" + m.f + "\",\"k\":\"navtex\",\"t\":" + std::to_string((long long)m.t) + ",\"hz\":517500,\"title\":\"x\"}");
        };
        const navwarn::TextFn text = [&](const std::string& f) { for (auto& m : ms) if (m.f == f) return m.text; return std::string(); };
        auto s1 = navwarn::sync(idx, each, text, 3);
        ok(s1.added == 3 && s1.pending == 5, "the backfill in chunks: 3 parsed, 5 pending");
        navwarn::SyncResult s2; size_t rounds = 1; do { s2 = navwarn::sync(idx, each, text, 3); ++rounds; } while (s2.pending);
        ok(s2.indexed == ms.size() && rounds == 3, "…done in 3 calls (" + std::to_string(s2.indexed) + " indexed)");
        ok(navwarn::sync(idx, each, text, 3).added == 0, "an index up to date: nothing parsed again");
        live.pop_back();   // ★ x1 evicted by the gallery
        std::unordered_set<uint64_t> names; size_t lines = 0;
        navwarn::sync(idx, each, text, 3, &names, &lines);
        const auto q = navwarn::query(idx, names, 0, T + 30000);
        auto find = [&](const char* id, int64_t first) -> const navwarn::Group* { for (auto& g : q.shown) if (g.id == id && (first < 0 || g.first == first)) return &g; return nullptr; };
        const navwarn::Group* ea45 = find("EA45", T);
        ok(ea45 && ea45->n == 3 && ea45->lost == 0 && ea45->best == "a1", "EA45 heard 3 times (a repeat, a garbled copy) = ONE marker, the clean copy shown");
        ok(find("EA45", T - 40 * 86400) != nullptr, "EA45 forty days earlier, another text: a different warning");
        const navwarn::Group* ea46 = find("EA46", -1);
        ok(ea46 && ea46->cancelledBy == "EA47", "EA46 cancelled by EA47 ('CANCEL WZ 650/26 (EA46)')");
        ok(find("EA49", -1) == nullptr, "an item the gallery evicted is not on the map");
        const navwarn::Group* ea48 = find("EA48", -1);
        ok(ea48 && ea48->pts.size() == 4 && ea48->cls == "firing", "a firing area: four points");
        const std::string j = navwarn::toJson(q, T + 30000, s2);
        ok(j.find("\"id\":\"EA46\"") != std::string::npos && j.find("\"expired\":1") != std::string::npos, "the JSON marks the cancelled one expired");
        const auto q2 = navwarn::query(idx, names, 0, T + 30000 + 80 * 3600);
        const std::string j2 = navwarn::toJson(q2, T + 30000 + 80 * 3600, s2);
        ok(j2.find("\"expired\":0") == std::string::npos, "80 h later every warning is expired (72 h unheard)");
        ok(lines == ms.size() && navwarn::compact(idx, names), "compaction runs");
        size_t after = 0; navwarn::sync(idx, each, text, 3, nullptr, &after);
        ok(after == live.size(), "…and drops the evicted item's line (" + std::to_string(after) + " lines)");
    }
    std::printf("── the real fixtures (Stuart's Lenovo) ──\n");
    {
        const std::string idxIn = argc > 2 ? argv[1] : "scripts/navtex-fixtures/index.jsonl";
        const std::string dir = argc > 2 ? argv[2] : "scripts/navtex-fixtures/files";
        char tmpl[256]; std::snprintf(tmpl, sizeof tmpl, "%s/navwarn-fx-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp");
        const std::string out = std::string(mkdtemp(tmpl) ? tmpl : "/tmp") + "/navtex-warn.idx";
        const navwarn::LiveFn each = [&](const std::function<void(const std::string&)>& fn) { navmap::fileLines(idxIn)(fn); };
        const navwarn::TextFn text = [&](const std::string& f) { std::string o; FILE* fp = std::fopen((dir + "/" + f).c_str(), "r"); if (!fp) return o; char b[16384]; const size_t n = std::fread(b, 1, sizeof b, fp); o.assign(b, n); std::fclose(fp); return o; };
        std::unordered_set<uint64_t> names; navwarn::SyncResult s;
        do { s = navwarn::sync(out, each, text, 1000, &names); } while (s.pending);
        const auto q = navwarn::query(out, names, 0, 1793560000);
        std::printf("    %zu receptions → %zu warnings, %zu with a position\n", q.receptions, q.groups, q.withPos);
        ok(q.withPos >= 10, "warnings with positions from the fixtures");
        navwarn::Entry ua; bool haveUa = false, haveAa = false;
        navmap::fileLines(out)([&](const std::string& l) { navwarn::Entry e; if (navwarn::parse(l, e)) { if (e.id == "UA30") { ua = e; haveUa = true; } if (e.id == "AA70") haveAa = haveAa || e.series.find("370/26") != std::string::npos; } });
        ok(haveUa && ua.cls == "cancel", "UA30 (Mondolfo, '510/26 … IS CANCELED'): indexed as a cancellation, not a hazard");
        ok(haveAa, "AA70 (Corsen, 518, English) carries AVURNAV BREST 370/26 — the number EA81 shares on 490");
    }
    std::printf("%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
