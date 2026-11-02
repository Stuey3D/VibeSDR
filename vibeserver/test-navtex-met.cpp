// test-navtex-met.cpp — V12 THE NAVTEX MAP, stage 3 (vibe_navtex_met.h, 2026-11-01): the met labels. Headings (a line of
// known area names and nothing else; an inshore stretch by its headlands), the block (the first period only, never the
// outlook), wind direction and force, the gale words and their negations, the sea state (words or a wave height), the
// visibility and weather; the native gale words set only the flag; the index and the latest per area. Then every met
// message of a real set (default scripts/navtex-fixtures; Stuart's Lenovo with its path).
//   test-navtex-met [<index.jsonl> <files dir>]
// Build: g++ -std=c++17 -O2 -I android/app/src/main/cpp -o test-navtex-met vibeserver/test-navtex-met.cpp
#include "vibe_navtex_warn.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unistd.h>

using namespace vibe;
static int fails = 0, passes = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); (c ? passes : fails)++; }
static const navmet::Fc* of(const std::vector<navmet::Fc>& v, const char* id) { for (auto& f : v) if (f.area == id) return &f; return nullptr; }

int main(int argc, char** argv) {
    std::printf("test-navtex-met — the NAVTEX met labels\n");
    std::printf("── headings ──\n");
    {
        ok(navmet::headingAreas("DOVER WIGHT PORTLAND PLYMOUTH BISCAY").size() == 5, "DOVER WIGHT PORTLAND PLYMOUTH BISCAY: five areas");
        ok(navmet::headingAreas("HUMBER. GERMAN BIGHT.").size() == 2, "HUMBER. GERMAN BIGHT.: two");
        ok(navmet::headingAreas("  SOUTH UTSIRE").size() == 1 && navmet::headingAreas("  SOUTH UTSIRE")[0] == "sutsire", "  SOUTH UTSIRE (indented)");
        ok(navmet::headingAreas("LANDS END TO ST DAVID'S HEAD INCLUDING THE BRISTOL CHANNEL - (SWW)").size() == 1, "an inshore stretch with its INCLUDING tail and (SWW)");
        ok(navmet::headingAreas("THAMES AND DOVER ARE EXPECTED TO SEE GALES").empty(), "prose naming areas is not a heading");
        ok(navmet::headingAreas("HIGH 1043 OVER HEBRIDES").empty(), "a synopsis line is not a heading");
    }
    std::printf("── one block ──\n");
    {
        const auto v = navmet::parse("ISSUED BY THE MET OFFICE AT 0700 ON SUN 01 NOV\n\nGALE WARNINGS: NONE\n\n24-HR FORECASTS\n\nTHAMES\n"
                                     "VRB 2 TO 4, BECMG N OR NW 4 TO 6 BUT LTR IN THAMES, OCNL 7 LTR IN E. SLT OR MOD, BECMG MOD OR ROUGH LTR. RAIN FOR A TIME, THEN SHWRS. GOOD, OCNL POOR\n\n"
                                     "DOVER WIGHT\nE OR NE 3 TO 5, OCNL 6 LTR. SMOOTH OR SLT. FAIR. MOD OR GOOD\n\nOUTLOOK FLW 24 HOURS:\nSTRG WINDS EXP IN ALL AREAS. THAMES SW 9.\n");
        const auto* t = of(v, "thames");
        ok(t && t->dir == "VRB" && t->fmin == 2 && t->fmax == 7, "Thames: VRB 2…7 (the first direction, every force)");
        ok(t && t->sea == 5 && t->gale == 0, "Thames: ROUGH at worst, no gale ('GALE WARNINGS: NONE' is outside the block)");
        ok(t && t->vis == 2 && t->wx.find("rain") != std::string::npos && t->wx.find("showers") != std::string::npos, "Thames: visibility POOR at worst; rain, showers");
        const auto* d = of(v, "dover"); const auto* w = of(v, "wight");
        ok(d && w && d->dir == "E" && d->fmax == 6 && d->sea == 3, "Dover and Wight share the block: E 3–6, SLIGHT");
        ok(v.size() == 3, "the outlook's 'THAMES SW 9' is not read (" + std::to_string(v.size()) + " areas)");
    }
    {
        const auto v = navmet::parse("GIBRALTAR POINT TO N FORELAND - (SWW)\n24 HR FCST:\nVRB 3 OR LESS BECMG N OR NE 4 TO 6.\nSMOOTH OR SLT BECMG SLT OR MOD.\n"
                                     "MAINLY FAIR THEN RAIN, THEN SHWRS LTR.\nMOD OR POOR BECMG MOD OR GOOD.\n\nOUTLOOK FOR THE FLW 24 HR:\nN OR NE 5 OR 6, OCNL 7.\n");
        const auto* f = of(v, "i-gibraltar-nforeland");
        ok(f && f->dir == "VRB" && f->fmin == 3 && f->fmax == 6 && f->sea == 4 && f->vis == 2, "Met Office inshore: VRB 3–6, MODERATE, POOR (the outlook's 7 not read)");
    }
    {
        const auto v = navmet::parse("GALE WARNINGS.\nTHAMES. HUMBER. GERMAN BIGHT. DOGGER.\nNO WARNING.\n\nSYNOPSIS.\nRIDGE OVER THE SOUTHERN NORTH SEA.\n\n"
                                     "FORECAST VALID SUNDAY 15:00 TILL MONDAY 03:00 UTC.\n\nTHAMES.\nVARIABLE 1 - 3, INCREASING NORTHWESTERLY 2 - 4, LATER NORTH TO NORTHEAST 5 - 6.\n"
                                     "RISK LIGHT RAIN.\nGOOD, IN PRECIPITATION MODERATE.\nWAVEHEIGHT LESS THAN 0.5 METER, LATER IN THE NORTH INCREASING 1.5 - 2.0 METER.\n\n"
                                     "FORECAST VALID MONDAY 03:00 TILL MONDAY 15:00 UTC.\n\nTHAMES.\nNORTH 8.\n");
        const auto* f = of(v, "thames");
        ok(v.size() == 1 && f && f->dir == "VRB" && f->fmax == 6 && f->gale == 0, "Dutch layout: the gale-warning list is not a forecast; the first period only (NORTH 8 later not read)");
        ok(f && f->sea == 4, "WAVEHEIGHT … 1.5 - 2.0 METER → MODERATE (WMO code from the height)");
    }
    {
        const auto v = navmet::parse("  INNER SKAGERRAK\nVRB 5. SLT. DRY, GOOD, BECMG OCNL RAIN, MOD.\n  FISHER\nN 6. SLT TO ROUGH. OCNL RAIN, MOD.\n  EAST-TAMPEN\nVRB 4. MOD. RAIN, MOD.\n");
        const auto* fi = of(v, "fisher");
        ok(of(v, "n-innerskagerrak") && fi && fi->dir == "N" && fi->fmax == 6 && fi->sea == 5, "Norwegian layout: FISHER N 6, ROUGH");
        ok(fi && fi->block.find("TAMPEN") == std::string::npos, "an area we do not hold (EAST-TAMPEN) ends the block before it");
    }
    {
        const auto v = navmet::parse("THAMES: \n2 TO 3 FR ENE, GRAD VRB, INCR TONIGHT TO 4 TO 6 FM N TO NNW.\nDOVER: \n2 TO 3 FM E TO ENE.\nALL REGIONS:\nLOTS OF LOW CLOUDS.\n");
        ok(of(v, "thames") && of(v, "thames")->dir == "ENE" && of(v, "thames")->fmax == 6 && of(v, "dover"), "Belgian layout: THAMES: ENE 2–6; DOVER");
    }
    {
        const auto v = navmet::parse("WEATHERFORECAST FOR GERMAN BIGHT\n\nSUNDAY:\nFIRST LIGHT AND VARIABLE WINDS, OTHERWISE WESTERLY WINDS ABOUT 3,\nSHIFTING NORTH LATER 5 TO 6.\n"
                                     "SEA: NORTHERN PART LATER 2,5 METER.\n\nMONDAY:\nNORTHERLY WINDS 8.\n");
        const auto* f = of(v, "gbight");
        ok(f && f->fmax == 6 && f->sea == 4 && f->gale == 0, "German layout: 'FORECAST FOR GERMAN BIGHT', SUNDAY only, SEA: 2,5 METER → MODERATE");
    }
    std::printf("── gales ──\n");
    {
        auto one = [](const std::string& b) { const auto v = navmet::parse("FAIR ISLE\n" + b + "\n"); return v.empty() ? navmet::Fc{} : v[0]; };
        ok(one("SOUTHWEST GALE 8 TO STORM 10, OCCASIONALLY VIOLENT STORM 11 LATER. HIGH. RAIN. POOR.").gale == 11, "…VIOLENT STORM 11 → 11");
        ok(one("SW 7 TO SEVERE GALE 9. ROUGH OR VERY ROUGH. SHOWERS. GOOD.").gale == 9, "SEVERE GALE 9 → 9");
        ok(one("W 6 TO GALE 8. VERY ROUGH. SHOWERS. GOOD.").sea == 6, "VERY ROUGH is 6, not ROUGH");
        ok(one("NE 5 TO 7. MOD. NO GALES EXPECTED. GOOD.").gale == 0, "NO GALES … : not a gale");
        ok(one("N 4 TO 6, GALES NOT EXPECTED. SLT. FAIR. GOOD.").gale == 0, "GALES NOT EXPECTED: not a gale");
        ok(one("HURRICANE FORCE 12. PHENOMENAL.").gale == 12 && one("HURRICANE FORCE 12. PHENOMENAL.").sea == 9, "HURRICANE FORCE 12, PHENOMENAL");
        const auto nat = one("NW 5 TO 6, COUP DE VENT EN SOIREE. AGITEE.");
        ok(nat.gale == 8 && nat.galeNative && nat.fmax == 6, "COUP DE VENT: the GALE flag only (forces read as given: 5–6)");
        ok(one("NW 5, OLUJNI VJETAR. MOD.").galeNative && one("W 5 BURRASCA. MOD.").galeNative && one("SW 5 STURM. MOD.").galeNative && one("S 5 TEMPORAL. MOD.").galeNative, "OLUJA / BURRASCA / STURM / TEMPORAL: the flag");
        ok(one("HIGH 1043 MOV NE 5 TO 6. SLT.").fmax == 6, "a pressure (1043) is not a force");
    }
    std::printf("── the index: the latest per area ──\n");
    {
        char tmpl[256]; std::snprintf(tmpl, sizeof tmpl, "%s/navmet-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp");
        const std::string dir = mkdtemp(tmpl) ? std::string(tmpl) : std::string("/tmp");
        const std::string idx = dir + "/navtex-warn.idx", met = dir + "/navtex-met.idx";
        const int64_t T = 1793400000;
        struct M { std::string f; int64_t t; std::string text; };
        std::vector<M> ms = {
            {"m1", T,         "ZCZC GE01\nTHAMES\nSW 4 TO 6. SLT. RAIN. GOOD.\nNNNN"},
            {"m2", T + 14400, "ZCZC GE02\nTHAMES\nSW 6 TO GALE 8. ROUGH. RAIN. MOD.\nNNNN"},
            {"m3", T + 28800, "ZCZC GE02\nTHAMES\nSW 6 TO GALE 8. ROUGH. RAIN. MOD.\nNNNN"},   // the same forecast again
            {"m4", T + 3600,  "ZCZC GA03\nWZ 1/26\nWRECK 51-00N 001-00E\nNNNN"},
        };
        const navwarn::LiveFn each = [&](const std::function<void(const std::string&)>& fn) {
            for (auto& m : ms) fn("{\"f\":\"" + m.f + "\",\"k\":\"navtex\",\"t\":" + std::to_string((long long)m.t) + ",\"hz\":517500,\"title\":\"x\"}");
        };
        const navwarn::TextFn text = [&](const std::string& f) { for (auto& m : ms) if (m.f == f) return m.text; return std::string(); };
        std::unordered_set<uint64_t> live;
        navwarn::sync(idx, each, text, 100, &live, nullptr, met);
        const auto q = navmet::query(met, live, 0);
        ok(q.size() == 1 && q[0].hist.size() == 2 && q[0].hist[0].f.gale == 8 && q[0].hist[0].t == T + 28800, "Thames: the newest (GALE 8) first, the older one in its history, the repeat folded");
        ok(q.size() == 1 && q[0].repeats == 1, "…heard twice (counted)");
        const std::string j = navmet::toJson(q, T + 30000, nullptr);
        ok(j.find("\"name\":\"Thames\"") != std::string::npos && j.find("\"gale\":8") != std::string::npos, "the JSON names the area and its gale");
        std::unordered_set<uint64_t> fewer = live; fewer.erase(navmet::fh("m3")); fewer.erase(navmet::fh("m2"));
        const auto q2 = navmet::query(met, fewer, 0);
        ok(q2.size() == 1 && q2[0].hist[0].f.gale == 0, "the gallery evicted the newer two: the older forecast is the latest again");
    }
    std::printf("── wind arrows (the SHIPPING MAP, 2026-11-02) ──\n");
    {
        auto W = [](const std::string& block) { navmet::Fc f = navmet::read("thames", block); return navmet::windOf(f); };
        auto near = [](double a, double b) { return std::fabs(a - b) < 0.01; };
        auto w = W("SW 6 TO 8. ROUGH. RAIN. MODERATE.");
        ok(w.kind == 'a' && near(w.from, 225) && near(w.to(), 45) && w.bf == 8 && w.sev == 2, "SW 6 TO 8: from 225, the arrow points downwind to 45, force 8 (the upper bound), RED");
        w = W("E 4 TO 6. SLIGHT.");
        ok(w.kind == 'a' && near(w.from, 90) && near(w.to(), 270) && w.bf == 6 && w.sev == 0 && !w.kind2, "E 4 TO 6: to the west, force 6, normal ink, no change");
        { const auto f = navmet::read("thames", "E 4-6. SLIGHT."); const auto x = navmet::windOf(f);
          ok(f.fmin == 4 && f.fmax == 6 && x.bf == 6 && near(x.to(), 270), "E 4-6 (a dash range): the label reads 4–6 and the arrow force 6 (Stuart's example)"); }
        { const auto f = navmet::read("thames", "SW 5, OCNL NEAR GALE 7. ROUGH."); const auto x = navmet::windOf(f);
          ok(f.gale == 0 && x.bf == 7 && x.sev == 1, "OCNL NEAR GALE 7: force 7, amber — a near gale is not a gale"); }
        { const auto x = W("WEST GALE 8 FROM 26 OCTOBER 10:00 UTC TO 14:00 UTC.");
          ok(x.bf == 8, "a date and a time (26 OCTOBER 10:00 UTC) are never a force"); }
        w = W("NORTHERLY 5 TO 7. MODERATE.");
        ok(w.kind == 'a' && near(w.from, 0) && near(w.to(), 180) && w.bf == 7 && w.sev == 1, "NORTHERLY 5 TO 7: to the south, 7 is AMBER");
        w = W("SOUTHWESTERLY 4 OR 5, OCCASIONALLY 6.");
        ok(w.kind == 'a' && near(w.from, 225) && w.bf == 6, "SOUTHWESTERLY 4 OR 5, OCCASIONALLY 6: from 225, force 6");
        const char* pts[16] = { "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW" };
        bool all = true;
        for (int i = 0; i < 16; ++i) { const auto x = W(std::string(pts[i]) + " 5."); if (x.kind != 'a' || !near(x.from, i * 22.5)) all = false; }
        ok(all, "all sixteen compass points N … NNW, from 0 to 337.5");
        const char* words[8] = { "NORTHERLY", "NORTHEASTERLY", "EASTERLY", "SOUTHEASTERLY", "SOUTHERLY", "SOUTHWESTERLY", "WESTERLY", "NORTHWESTERLY" };
        all = true;
        for (int i = 0; i < 8; ++i) { const auto x = W(std::string(words[i]) + " 5."); if (x.kind != 'a' || !near(x.from, i * 45.0)) all = false; }
        ok(all, "NORTHERLY … NORTHWESTERLY, every eighth");
        w = W("N OR NW 4 TO 6.");
        ok(w.kind == 'a' && near(w.from, 337.5), "N OR NW: the point between (337.5)");
        w = W("NE OR E 3 OR 4.");
        ok(w.kind == 'a' && near(w.from, 67.5), "NE OR E: 67.5");
        w = W("VARIABLE 2 OR 3. SMOOTH.");
        ok(w.kind == 'v' && w.bf == 3 && w.sev == 0 && w.from < 0, "VARIABLE 2 OR 3: its own glyph, no direction, force 3");
        w = W("CYCLONIC 6 TO GALE 8. VERY ROUGH.");
        ok(w.kind == 'c' && w.bf == 8 && w.sev == 2, "CYCLONIC 6 TO GALE 8: the cyclonic glyph, RED");
        w = W("SOUTHWEST 5 TO 7, VEERING WEST 6 TO 8 LATER. ROUGH.");
        ok(w.kind == 'a' && near(w.from, 225) && w.bf == 7 && w.sev == 1, "…VEERING WEST 6 TO 8 LATER: the first wind SW 7 (amber)");
        ok(w.chg == 'v' && w.kind2 == 'a' && near(w.from2, 270) && near(w.to2(), 90) && w.bf2 == 8 && w.sev2 == 2, "…and the later wind: veering, from W, force 8, red");
        w = W("SW 4 OR 5, BACKING SE 5 TO 7.");
        ok(w.chg == 'b' && w.bf == 5 && w.kind2 == 'a' && near(w.from2, 135) && w.bf2 == 7 && w.sev2 == 1, "BACKING SE 5 TO 7: SW 5, then from SE 7");
        w = W("VRB 2 TO 4, BECMG N OR NW 4 TO 6 BUT LTR IN THAMES, OCNL 7 LTR IN E.");
        ok(w.kind == 'v' && w.bf == 4 && w.chg == 'c' && w.kind2 == 'a' && near(w.from2, 337.5) && w.bf2 == 7, "VRB 2 TO 4, BECMG N OR NW 4 TO 6 … OCNL 7: variable 4, becoming from NNW 7");
        w = W("WEST 6, VEERING LATER.");
        ok(w.kind == 'a' && w.chg == 'v' && !w.kind2, "VEERING with no direction: no second arrow (never guessed)");
        w = W("SOUTHWEST SEVERE GALE 9 TO VIOLENT STORM 11.");
        ok(w.kind == 'a' && w.bf == 11 && w.sev == 2, "SEVERE GALE 9 TO VIOLENT STORM 11: force 11, red");
        w = W("WEST HURRICANE FORCE.");
        ok(w.kind == 'a' && w.bf == 12 && w.sev == 2, "a gale word without a figure counts as its force (HURRICANE 12)");
        w = W("NW 3 TO 5. PRESSURE 1012 HPA.");
        ok(w.bf == 5, "a pressure figure is never a force");
        w = W("NO GALES. NW 3 OR 4. SLIGHT.");
        ok(w.kind == 'a' && w.bf == 4 && w.sev == 0, "NO GALES is not a gale: normal ink");
        w = W("SMOOTH. GOOD.");
        ok(w.kind == 0 && w.bf < 0 && navmet::windJson(w).empty(), "no wind sentence: nothing drawn, no wind key");
        // the JSON the clients draw
        navmet::Line l; l.file = "m1"; l.t = 1; l.f = navmet::read("thames", "SOUTHWEST 5 TO 7, VEERING WEST 6 TO 8 LATER. ROUGH.");
        const std::string j = navmet::fcJson(l, nullptr);
        ok(j.find("\"wind\":{\"k\":\"a\",\"bf\":7,\"sev\":1,\"from\":225.0,\"to\":45.0,\"chg\":\"veer\",\"k2\":\"a\",\"bf2\":8,\"sev2\":2,\"from2\":270.0,\"to2\":90.0}") != std::string::npos,
           "the met JSON carries the arrows: " + j.substr(j.find("\"wind\"") == std::string::npos ? 0 : j.find("\"wind\"")));
    }
    std::printf("── a real set ──\n");
    {
        const std::string index = argc > 2 ? argv[1] : "scripts/navtex-fixtures/index.jsonl";
        const std::string files = argc > 2 ? argv[2] : "scripts/navtex-fixtures/files";
        size_t msgs = 0, metMsgs = 0, withArea = 0, fcs = 0, wind = 0, sea = 0, vis = 0, gale = 0, nat = 0, uk = 0;
        size_t arrows = 0, vrbcyc = 0, changes = 0, arrowForceOff = 0;
        std::map<std::string, size_t> perArea;
        const auto t0 = std::chrono::steady_clock::now();
        navmap::fileLines(index)([&](const std::string& l) {
            std::string f; double t = 0, hz = 0;
            if (!navmap::jstr(l, "f", f) || !navmap::jnum(l, "t", t) || !navmap::jnum(l, "hz", hz)) return;
            std::ifstream in(files + "/" + f); if (!in) return;
            std::stringstream ss; ss << in.rdbuf();
            const std::string tx = ss.str();
            msgs++;
            const auto e = navwarn::fromMessage(f, (int64_t)t, hz, tx);
            if (e.cls != "met") return;
            metMsgs++;
            const auto v = navmet::parse(navwarn::body(tx));
            if (!v.empty()) withArea++;
            for (auto& x : v) {
                fcs++; perArea[x.area]++;
                wind += x.fmin >= 0; sea += x.sea >= 0; vis += x.vis >= 0; gale += x.gale >= 8; nat += x.galeNative;
                { const auto aw = navmet::windOf(x); arrows += aw.kind == 'a'; vrbcyc += aw.kind == 'v' || aw.kind == 'c'; changes += aw.kind2 != 0;
                  if (x.fmin >= 0 && aw.kind == 'a' && aw.bf != x.fmax && aw.chg == 0 && ++arrowForceOff <= 4) std::printf("    differs: arrow %d label %d: %s\n", aw.bf, x.fmax, x.block.substr(0, 200).c_str()); }
                const auto* a = navmet::areaById(x.area); uk += a && a->kind != 'n';
            }
        });
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("    %zu messages, %zu met; %zu with at least one area → %zu area forecasts (%zu UK) in %.0f ms\n", msgs, metMsgs, withArea, fcs, uk, ms);
        std::printf("    wind %zu (%.0f %%), sea state %zu (%.0f %%), visibility %zu (%.0f %%), gale or worse %zu (native word %zu)\n",
                    wind, fcs ? 100.0 * wind / fcs : 0, sea, fcs ? 100.0 * sea / fcs : 0, vis, fcs ? 100.0 * vis / fcs : 0, gale, nat);
        std::string top; size_t k = 0;
        std::vector<std::pair<size_t, std::string>> srt; for (auto& p : perArea) srt.push_back({p.second, p.first});
        std::sort(srt.rbegin(), srt.rend());
        for (auto& p : srt) { if (k++ >= 12) break; top += " " + p.second + " " + std::to_string(p.first); }
        std::printf("    most forecast:%s\n", top.c_str());
        ok(metMsgs == 0 || withArea > 0, "the set's forecasts give area labels");
        ok(fcs == 0 || wind * 10 >= fcs * 8, "a wind for at least 80 % of area forecasts");
        std::printf("    arrows %zu, variable/cyclonic %zu, with a later wind %zu\n", arrows, vrbcyc, changes);
        ok(fcs == 0 || (arrows + vrbcyc) * 10 >= wind * 9, "an arrow or a variable/cyclonic glyph for at least 90 % of the winds read");
        ok(arrowForceOff == 0, "with no change, the arrow's force is the label's upper force (" + std::to_string(arrowForceOff) + " differ)");
    }
    std::printf("%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
