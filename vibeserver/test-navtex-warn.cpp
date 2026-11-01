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
        ok(c("GUNNERY FIRING EXERCISES") == "firing" && c("GUNFACTS WARNING") == "firing", "FIRING / GUNNERY / GUNFACTS → firing (SUBFACTS AND GUNFACTS is a submarine bulletin, below)");
        ok(c("WIND FARM CONSTRUCTION") == "structure", "WIND FARM → structure");
        const auto t = navwarn::classify('A', "WIND TURBINE T12 LIGHT INOPERATIVE");
        ok(t.id == "structure" && t.unlit, "a turbine with its light inoperative: structure + unlit");
        const auto lh = navwarn::classify('A', "LIGHTHOUSE OF S. CATALDO OF LECCE I.L. 3612 (E2192),\nIN POSITION:\nLAT 40 23.4'N\nIS NOT WORKING.");
        ok(lh.id == "navaid" && lh.unlit, "LIGHTHOUSE … IS NOT WORKING: navaid + unlit");
        ok(c("BUOY MISSING") == "buoy" && c("RACON INOPERATIVE") == "navaid" && c("FOGHORN OUT OF ORDER") == "navaid", "BUOY → buoy (the buoy icon); RACON / FOGHORN → navaid");
        ok(c("KUSTROUTE OOSTENDE\nLICHTBOEI BA-E 51-14.77N 002-55.42E\nLICHT GEDOOFD") == "buoy", "NL: LICHTBOEI (a compound) → buoy");
        ok(c("CONTAINERS LOST OVERBOARD") == "adrift" && c("DERELICT YACHT ADRIFT") == "adrift", "CONTAINERS LOST / DERELICT → adrift");
        ok(c("DIVED SUBMARINE EXERCISES IN PROGRESS") == "submarine" && c("SUBFACTS AND GUNFACTS") == "submarine" && c("SUBMARINE TORPEDO FIRING") == "submarine", "submarine operations → submarine, before torpedo/firing");
        ok(c("EXERCICES SOUS-MARINS") == "submarine" && c("U-BOOT UEBUNG") == "submarine" && c("ESERCITAZIONI SOMMERGIBILI") == "submarine", "FR/DE/IT submarines");
        ok(c("GUNNERY FIRING EXERCISES") == "firing", "gunnery without a submarine stays firing");
        ok(c("SUBMARINE CABLE REPAIR") == "cable" && c("POSE DE CABLES SOUS-MARINS") == "cable" && c("SUBMARINE PIPELINE") == "pipeline", "an undersea CABLE or PIPELINE is never a submarine");
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
        ok(k.id == "cable_power", "FR: PROJET CELTIC INTERCONNECTOR … POSE DE CABLES SOUS-MARINS → a POWER cable (v6: the interconnector says what it carries)");
        k = cl("FEU ETEINT\nBOUEE CARDINALE NORD");
        ok(k.id == "buoy" && k.unlit && k.lang == "fr" && k.hit.find("ETEINT") != std::string::npos, "FR: FEU ETEINT, BOUEE → buoy + unlit, from French: " + k.hit);
        ok(cl("BOYA APAGADA EN POSICION").unlit && cl("BOYA APAGADA EN POSICION").id == "buoy", "ES: BOYA APAGADA → buoy + unlit");
        ok(cl("FANALE SPENTO").id == "navaid" && cl("FANALE SPENTO").unlit, "IT: FANALE SPENTO → navaid + unlit");
        ok(cl("RELITTO PERICOLOSO").id == "wreck" && cl("PECIO EN POSICION").id == "wreck" && cl("EPAVE DANGEREUSE").id == "wreck", "IT/ES/FR wreck words");
        ok(cl("ESERCITAZIONI DI TIRO").id == "firing" && cl("SCHIESSUEBUNGEN").id == "firing", "IT/DE firing words");
        ok(cl("STATHMOS KERKYRA AR.MIN 227/25\n1. ASKISEIS PYRON AEROSKAFON APO 01 IAN 26 EOS 31 DEK 26").id == "firing", "EL: ASKISEIS PYRON → firing");
        ok(cl("ORDIGNO BELLICO RINVENUTO").id == "uxo" && cl("PARCO EOLICO OFFSHORE").id == "structure" && cl("BOA ALLA DERIVA").id == "buoy", "IT: uxo, wind farm; a buoy adrift is a buoy");
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
        ok(navwarn::seriesKey(navwarn::seriesOf("STATHMOS IRAKLEIOY AR.MIN 370/26\nKRITIKO PELAGOS")) != navwarn::seriesKey(navwarn::seriesOf("AVURNAV BREST 370/26")),
           "'STATHMOS IRAKLEIOY AR.MIN 370/26' (Greek, one line) does not link to Corsen's AVURNAV BREST 370/26");
        ok(navwarn::seriesKey(navwarn::seriesOf("AVURNAV BREST 370/26")) == navwarn::seriesKey(navwarn::seriesOf("AVUCNAV BREST 370/26")), "AVURNAV / AVUCNAV BREST 370/26: one key");
        const auto cx = navwarn::cancelsOf("1. SANDETTIE WEST BUOY MISSING.\n2. CANCEL WZ 476/26 (EA77)(VA08).", 1793500000);
        ok(cx.ids.size() == 2 && cx.ids[0] == "EA77" && cx.series.size() == 1 && navwarn::seriesKey(cx.series[0]) == "WZ|476/26", "CANCEL WZ 476/26 (EA77)(VA08): ids EA77 VA08, series WZ 476/26");
        const auto self = navwarn::cancelsOf("CANCEL THIS MESSAGE 302200 UTC NOV", 1793500000);
        ok(self.at > 1793500000 && self.at - 1793500000 < 40 * 86400, "CANCEL THIS MESSAGE 302200 UTC NOV: an end time");
        const auto ua = navwarn::fromMessage("ua30.txt", 1793488800, 517500, "ZCZC UA30\nMONDOLFO RADIO\nCOASTAL WARNING 510/26 OCT 24 (CANCELED)\nLIGHTHOUSE … IS NOT WORKING.\nLAT 40 23.4'N LONG 018 18.4'E\nCOSTAVURNAV 510/26 OCT 24 IS CANCELED\nNNNN");
        ok(ua.cls == "cancel" && ua.cSeries.find("510/26") != std::string::npos, "UA30 '510/26 … IS CANCELED': a cancellation of 510/26, not a hazard to plot");
    }
    std::printf("── the shape the text names, its date-time group ──\n");
    {
        ok(navwarn::shapeOf("CABLE LAYING OPERATIONS ALONG A LINE JOINING 51-00N 001-00E 51-10N 001-20E", 2) == 'r', "cable laying ALONG A LINE JOINING (2 points): a ROUTE");
        ok(navwarn::shapeOf("AREA BOUNDED BY 50-00N 001-00W 50-10N 001-00W 50-10N 000-50W", 3) == 'a', "AREA BOUNDED BY (3 points): an AREA");
        ok(navwarn::shapeOf("ZONE DE TIRS 47-00N 003-00W 47-10N 003-00W 47-10N 002-50W 47-00N 002-50W", 4) == 'a', "French ZONE (4 points): an AREA");
        ok(navwarn::shapeOf("1. WRECK 50-00N 001-00W\n2. BUOY 50-10N 001-05W", 2) == 0, "two numbered items, no area or route words: separate points");
        ok(navwarn::shapeOf("AREA BOUNDED BY 50-00N 001-00W", 1) == 0, "one point: never a shape");
        ok(navwarn::shapeOf("WITHIN AREA 50-00N 001-00W 50-10N 001-00W", 2) != 'a', "an area with only two points is not a polygon");
        const int64_t t = 1793488800;   // 2026-11-01 01:20 UTC
        ok(navwarn::issuedOf("301500 UTC OCT 26\nWZ 1/26", t) == 1793372400, "301500 UTC OCT 26 → 2026-10-30 15:00 UTC");
        ok(navwarn::issuedOf("010710 UTC OCT\nOOSTENDERADIO", t) > 0, "010710 UTC OCT (no year): read against the message year");
        ok(navwarn::issuedOf("311899 UTC OCT 26", t) == 0 && navwarn::issuedOf("WZ 649/26", t) == 0, "a garbled group or none: 0 (never shown)");
    }
    {
        const auto na = navwarn::fromMessage("na84.txt", 1793488800, 517500, "ZCZC NA84\nAREA BOUNDED BY POSITIONS:\n64-20.3N 011-15.8E\n64-22.1N 011-19.3E\n64-22.8N 011-20.4E\n64-23.7N 011-19.0E\n24-20.6N 011-14.6E\nNNNN");
        ok(na.pts.size() == 4 && na.shape == 'a', "NA84: '24-20.6N' among four at 64°N (a 6 read as a 2) is dropped; the area keeps its four corners");
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
        ok(ea45 && ea45->n == 3 && ea45->lost == 0 && ea45->best == "a2", "EA45 heard 3 times (a repeat, a garbled copy) = ONE marker; the newest CLEAN copy's text shown (v6), never the garbled newest");
        ok(find("EA45", T - 40 * 86400) != nullptr, "EA45 forty days earlier, another text: a different warning");
        const navwarn::Group* ea46 = find("EA46", -1);
        ok(ea46 && ea46->cancelledBy == "EA47", "EA46 cancelled by EA47 ('CANCEL WZ 650/26 (EA46)')");
        ok(find("EA49", -1) == nullptr, "an item the gallery evicted is not on the map");
        const navwarn::Group* ea48 = find("EA48", -1);
        ok(ea48 && ea48->pts.size() == 4 && ea48->cls == "firing", "a firing area: four points");
        ok(ea48 && ea48->shape == 'a', "…'FIRING EXERCISES IN AREA' + 4 points: drawn as an AREA (a polygon)");
        const std::string j = navwarn::toJson(q, T + 30000, s2);
        ok(j.find("\"id\":\"EA46\"") != std::string::npos && j.find("\"expired\":1") != std::string::npos, "the JSON marks the cancelled one expired");
        const auto q2 = navwarn::query(idx, names, 0, T + 30000 + 80 * 3600);
        const std::string j2 = navwarn::toJson(q2, T + 30000 + 80 * 3600, s2);
        ok(j2.find("\"expired\":0") == std::string::npos, "80 h later every warning is expired (72 h unheard)");
        ok(lines == ms.size() && navwarn::compact(idx, names), "compaction runs");
        size_t after = 0; navwarn::sync(idx, each, text, 3, nullptr, &after);
        ok(after == live.size(), "…and drops the evicted item's line (" + std::to_string(after) + " lines)");
    }
    std::printf("── v6: cables and pipelines in five, platforms ──\n");
    {
        auto c = [](const char* t, char b2 = 'A') { return navwarn::classify(b2, t).id; };
        ok(c("CABLE LAYING OPERATIONS BY VESSEL") == "cable" && c("SUBMARINE CABLE REPAIR") == "cable", "a cable whose kind is not said → cable");
        ok(c("AIRDRAFT FOR OVERHEAD POWER CABLE IN NORDFJORDEN") == "cable_power" && c("EXPORT CABLE INSTALLATION") == "cable_power"
           && c("INTER-ARRAY CABLE LAY") == "cable_power" && c("ACCESSORY WORKS RELATED TO THE HVDC CONNECTION PROJECT") == "cable_power", "POWER / EXPORT / INTER-ARRAY / HVDC → cable_power (before works)");
        ok(c("POSE DE CABLE ELECTRIQUE") == "cable_power" && c("POSA CAVO ELETTRICO") == "cable_power" && c("STROMKABEL VERLEGUNG") == "cable_power"
           && c("HOOGSPANNINGSKABEL") == "cable_power" && c("DEPOSITO DE SACOS PARA TENDIDO ELECTRICO") == "cable_power", "FR / IT / DE / NL / ES power-cable words → cable_power");
        ok(c("FIBRE OPTIC CABLE LAYING") == "cable_telecom" && c("TELECOMMUNICATIONS CABLE REPAIR") == "cable_telecom" && c("POSE DE FIBRE OPTIQUE") == "cable_telecom"
           && c("FIBRA OTTICA") == "cable_telecom" && c("GLASFASERKABEL") == "cable_telecom", "telecom / fibre-optic (EN FR IT DE) → cable_telecom");
        ok(c("ANCHORAGE PROHIBITED AREA GAS PIPELINE WARNING") == "pipeline_gas" && c("GAZODUC") == "pipeline_gas" && c("GASDOTTO") == "pipeline_gas"
           && c("GASODUCTO") == "pipeline_gas" && c("ERDGASLEITUNG") == "pipeline_gas" && c("AARDGASLEIDING") == "pipeline_gas", "gas pipeline (EN FR IT ES DE NL) → pipeline_gas");
        ok(c("UNDERWATERPIPELINE RAVERSIJDE ESTABLISHED") == "pipeline" && c("ZINKERLEIDING LOMBARDSIJDE") == "pipeline" && c("OLEODUC") == "pipeline"
           && c("CONDOTTA SOTTOMARINA") == "pipeline" && c("ROHRLEITUNG") == "pipeline" && c("PIJPLEIDING") == "pipeline", "pipeline (EN, the Belgian run-together word, NL ZINKERLEIDING, FR IT DE NL) → pipeline");
        ok(c("POSE DE CABLES SOUS-MARINS") == "cable" && c("SUBMARINE CABLES") == "cable" && c("SUBMARINE POWER CABLE") == "cable_power", "SUBMARINE CABLE stays a cable (never a submarine), and says its kind when it does");
        ok(c("NAVIGATION RESTRICTED. KEEP A SAFE DISTANCE OF 2 CABLES.") == "general" && c("MINIMUM 2 KABELS AFSTAND") == "general", "'a safe distance of 2 CABLES' is a distance, not a cable");
        const auto p1 = navwarn::classify('A', "HUMBER.\nINNER CROMER KNOLL.\nPLATFORM BLYTHE 53-14.5N 001-26.8E UNLIT AND ALL NAVAIDS INOPERATIVE.");
        ok(p1.id == "platform" && p1.unlit, "PLATFORM BLYTHE … UNLIT AND ALL NAVAIDS INOPERATIVE → platform + unlit");
        ok(c("PETRO GIANT DRILLING RIG TO BE TOWED FROM 55-27.34N 018-09.34E") == "platform", "a drilling rig under tow → platform (before towing)");
        ok(c("SUSPENDED WELLHEAD 57-00N 001-00E") == "platform" && c("JACK-UP RIG ON LOCATION") == "platform" && c("FPSO ON STATION") == "platform", "WELLHEAD / JACK-UP / FPSO → platform");
        ok(c("PLATEFORME PETROLIERE") == "platform" && c("PIATTAFORMA") == "platform" && c("PLATAFORMA") == "platform" && c("BOORPLATFORM") == "platform", "FR / IT / ES / NL platform words");
        ok(c("WIND FARM CONSTRUCTION") == "structure" && c("WIND TURBINE T12 LIGHT INOPERATIVE") == "structure", "a wind farm stays structure");
        ok(c("THE RIGHT TRIGONOMETRIC ORIGIN IS BRIGHT") == "general", "RIG is a whole word: never RIGHT, TRIG, ORIGIN, BRIGHT");
        ok(c("UNDERWATERPIPELINE RAVERSIJDE ESTABLISHED BETWEEN BEACH\nAND 51-13.294N 002-51.385E\nMARKED WITH NORTH CARDINAL BUOY 'JDN'") == "pipeline",
           "a pipeline MARKED WITH a buoy is the pipeline (the buoy only marks it)");
        ok(c("ANCHOR AND CHAIN LOST IN POS 51-19-22N 002-35-15E MARKED WITH A WHITE BUOY") == "obstruction" && c("NORTH CARDINAL BUOY MISSING") == "buoy", "…an anchor lost, marked with a buoy, is the obstruction; a buoy missing is still a buoy");
        ok(c("OPERACIONES DE TENDIDO DE CABLE SUBMARINO") == "cable" && c("EJERCICIOS CON SUBMARINOS") == "submarine", "ES: CABLE SUBMARINO is a cable, never a submarine; SUBMARINOS alone is");
        ok(c("IT IS REQUIRED TO CONSULT NEARBY OFFSHORE PLATFORMS TO VERIFY YOUR ANCHORAGE. DAMAGE TO SUBSEA GAS PIPELINES") == "pipeline_gas", "'CONSULT NEARBY OFFSHORE PLATFORMS' (an anchoring notice) is not a platform");
    }
    std::printf("── v6: a list of named platforms = a marker each ──\n");
    {
        const char* pa = "ZCZC PA11\n231717 UTC OCT 26\nMSI 269/26\nNORTH SEA DUTCH EEZ\nFOLLOWING PLATFORMS HAVE DEFECTS: \nF16-A 54-07.0N 004-00.1E NAV AIDS UNRELIABLE\n"
                         "K6-N 53-41.0N 003-44.0E FOGHORN INOPERATIVE\nL10-B 53-27.4N 004-13.9E BLACK OUT\nN7-FA-1 53-30N 006-14E FOGHORN INOPERATIVE\n"
                         "K15-FA1 53-14N 003-59E UNLIT\nK12-G 53-21.3N 003-59.0E NAV AIDS UNRELIABLE\nCANCEL MSI 267/26\nNNNN";
        const auto e = navwarn::fromMessage("pa11.txt", 1793500000, 517500, pa);
        ok(e.cls == "platform" && e.shape == 0 && e.pts.size() == 6 && e.items.size() == 6, "PA11's six platforms: platform, separate points (never an area), six named items");
        ok(e.items.size() == 6 && e.items[0].name == "F16-A" && e.items[0].defect == "NAV AIDS UNRELIABLE" && e.items[1].name == "K6-N" && e.items[1].defect == "FOGHORN INOPERATIVE",
           "…each its own name and defect (F16-A: NAV AIDS UNRELIABLE; K6-N: FOGHORN INOPERATIVE)");
        ok(e.items.size() == 6 && e.items[2].unlit && e.items[4].unlit && e.items[0].unlit && !e.items[1].unlit && !e.items[3].unlit,
           "…the unlit mark per item: BLACK OUT, UNLIT, NAV AIDS UNRELIABLE yes; FOGHORN INOPERATIVE no");
        navwarn::Entry back; ok(navwarn::parse(navwarn::format(e).substr(0, navwarn::format(e).size() - 1), back) && back.items.size() == 6 && back.items[5].name == "K12-G", "…the items survive the index line");
        const auto corners = navwarn::fromMessage("cx.txt", 1793500000, 517500, "ZCZC EA50\nWZ 700/26\nA) 51-00.0N 001-00.0E\nB) 51-10.0N 001-00.0E\nC) 51-10.0N 001-10.0E\nNNNN");
        ok(corners.items.empty(), "a corner list (A) B) C) and nothing else) is not a named list");
        const auto lettered = navwarn::fromMessage("lt.txt", 1793500000, 517500, "ZCZC VA28\nWZ 702/26\nWRECKS:\nA. C22 51-30.0N 002-00.0E\nB. F17 51-35.0N 002-10.0E\nNNNN");
        ok(lettered.items.size() == 2 && lettered.items[0].name == "C22" && lettered.items[1].name == "F17", "'A. C22' / 'B. F17': a lettered list of designations, the marks dropped");
        const auto ar = navwarn::fromMessage("ar.txt", 1793500000, 517500, "ZCZC EA51\nWZ 701/26\nFIRING IN AREA BOUNDED BY\nALPHA 51-00.0N 001-00.0E\nBRAVO 51-10.0N 001-00.0E\nCHARLIE 51-10.0N 001-10.0E\nNNNN");
        ok(ar.shape == 'a' && ar.items.empty(), "an AREA BOUNDED BY with named corners stays an area");
    }
    std::printf("── v6: latest wins (cancelled, replaced, re-issued) ──\n");
    {
        char tmpl[] = "/tmp/navwarn-v6-XXXXXX"; const std::string dir = mkdtemp(tmpl), idx = dir + "/navtex-warn.idx";
        const int64_t T = 1793400000;
        struct M { std::string f; int64_t t; std::string text; };
        std::vector<M> ms = {
            {"w1", T,             "ZCZC GA20\nWZ 621/26\nHUMBER.\nWRECK 53-30.0N 001-00.0E.\nNNNN"},
            {"w2", T + 86400,     "ZCZC GA31\nWZ 640/26\nHUMBER.\nCANCEL WZ 621/26 AND THIS MSG.\nNNNN"},                                     // cancels w1
            {"r1", T,             "ZCZC GA21\nWZ 622/26\nHUMBER.\nOBSTRUCTION 53-40.0N 001-00.0E.\nNNNN"},
            {"r2", T + 7200,      "ZCZC GA22\nWZ 641/26\nHUMBER.\nTHIS MESSAGE REPLACES WZ 622/26.\nOBSTRUCTION 53-41.0N 001-02.0E.\nNNNN"},  // replaces r1
            {"m1", T,             "ZCZC GA23\nWZ 623/26\nHUMBER.\nBUOY 53-50.0N 001-00.0E MISSING.\nNNNN"},
            {"m2", T + 3 * 3600,  "ZCZC GA24\nWZ 623/26\nHUMBER.\nBUOY 53-52.0N 001-05.0E MISSING.\nNNNN"},                                     // re-issued, moved
            {"k1", T + 86400,     "ZCZC GA25\nWZ 624/26\nHUMBER.\nCANCEL WZ 999/26 (GA99) AND THIS MSG. PLATFORM 49/26C 53-00.0N 001-00.0E NORMAL CONDITIONS RESTORED.\nNNNN"},
        };
        const navwarn::LiveFn each = [&](const std::function<void(const std::string&)>& fn) {
            for (auto& m : ms) fn("{\"f\":\"" + m.f + "\",\"k\":\"navtex\",\"t\":" + std::to_string((long long)m.t) + ",\"hz\":517500,\"title\":\"x\"}");
        };
        const navwarn::TextFn text = [&](const std::string& f) { for (auto& m : ms) if (m.f == f) return m.text; return std::string(); };
        std::unordered_set<uint64_t> names; navwarn::SyncResult s;
        do { s = navwarn::sync(idx, each, text, 100, &names); } while (s.pending);
        const auto q = navwarn::query(idx, names, 0, T + 86400 + 60);
        auto find = [&](const char* id) -> const navwarn::Group* { for (auto& g : q.shown) if (g.id == id) return &g; return nullptr; };
        const auto* w1 = find("GA20"); ok(w1 && !w1->cancelledBy.empty(), "WZ 621/26 cancelled by 'CANCEL WZ 621/26 AND THIS MSG.' (no bracketed id)");
        ok(!find("GA31"), "…and the cancelling message itself is no marker ('AND THIS MSG' with no date-time)");
        const auto* r1 = find("GA21"); ok(r1 && r1->supersededBy == "GA22", "'THIS MESSAGE REPLACES WZ 622/26' supersedes it");
        const auto* m1 = find("GA23"); const auto* m2 = find("GA24");
        ok(m1 && m2 && m1->supersededBy == "GA24" && m2->supersededBy.empty() && std::fabs(m2->pts[0].first - 53.8667) < 0.01, "WZ 623/26 re-issued with a moved position: only the newest (53-52.0N) stands");
        ok(!find("GA25"), "'CANCEL WZ … AND THIS MSG. PLATFORM … NORMAL CONDITIONS RESTORED': a notice, not a platform marker");
        const std::string js = navwarn::toJson(q, T + 86400 + 60, s);
        ok(js.find("\"supersededBy\":\"GA22\"") != std::string::npos && js.find("\"cancelledN\":1") != std::string::npos && js.find("\"supersededN\":2") != std::string::npos,
           "the JSON says superseded / cancelled (and counts them) — the clients draw neither");
        // ★ the newest copy's content is drawn: the same id and points, a newer copy with another class
        auto x1 = navwarn::fromMessage("n1", T, 517500, "ZCZC GA40\nWZ 650/26\nBUOY 53-00.0N 002-00.0E MISSING.\nNNNN");
        auto x2 = navwarn::fromMessage("n2", T + 3600, 517500, "ZCZC GA40\nWZ 650/26\nWRECK MARKED BY BUOY 53-00.0N 002-00.0E.\nNNNN");
        const std::string idx2 = dir + "/b.idx";
        if (FILE* f = std::fopen(idx2.c_str(), "w")) { const std::string a = navwarn::format(x1) + navwarn::format(x2); std::fwrite(a.data(), 1, a.size(), f); std::fclose(f); }
        std::unordered_set<uint64_t> nm = {navwarn::nameHash("n1"), navwarn::nameHash("n2")};
        const auto q2 = navwarn::query(idx2, nm, 0, T + 7200);
        ok(q2.shown.size() == 1 && q2.shown[0].cls == "wreck" && q2.shown[0].best == "n2", "one warning, two copies: the NEWEST copy's class (wreck) and text are drawn");
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
