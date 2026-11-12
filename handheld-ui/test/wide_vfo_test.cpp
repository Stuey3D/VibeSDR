// wide_vfo_test.cpp — the R36S's wide VFO (src/wide_vfo.h) replays the app's / web's own answers (gen_wide_vfo_golden.ts):
// the table, every passband, label and fill decision must match exactly. run_wide_vfo_test.sh.
#include "../src/json.h"
#include "../src/wide_vfo.h"

#include <cmath>
#include <fstream>
#include <iostream>

using namespace hh;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s golden.jsonl\n", argv[0]); return 2; }
    std::ifstream in(argv[1]);
    if (!in) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    std::string ln;
    int n = 0, bad = 0, nTable = 0;
    auto fail = [&](const std::string& what, const std::string& got, const std::string& want) {
        ++bad; std::printf("FAIL %s\n   got  %s\n   want %s\n", what.c_str(), got.c_str(), want.c_str());
    };
    while (std::getline(in, ln)) {
        if (ln.empty()) continue;
        const Json g = Json::parse(ln);
        const std::string k = g["k"].str();
        ++n;
        if (k == "max") {
            if (kWideVfoFillMax != g["v"].num()) fail("fill max", std::to_string(kWideVfoFillMax), std::to_string(g["v"].num()));
        } else if (k == "table") {
            ++nTable;
            const WideVfoMode m = wideVfoMode(g["m"].str());
            if (m == WIDE_NONE) { fail("table has " + g["m"].str(), "missing", "present"); continue; }
            if (kWideVfo[m].half != g["half"].num()) fail(g["m"].str() + " half", std::to_string(kWideVfo[m].half), std::to_string(g["half"].num()));
            if (g["name"].str() != kWideVfo[m].name) fail(g["m"].str() + " name", kWideVfo[m].name, g["name"].str());
        } else if (k == "pb") {
            const WideVfoMode m = wideVfoMode(g["m"].str());
            double lo, hi;
            wideVfoPassband(m, g["dial"].num(), g["centre"].num(), lo, hi);
            const std::string tag = g["m"].str() + " @" + std::to_string((long long)g["dial"].num());
            if (lo != g["lo"].num() || hi != g["hi"].num())
                fail(tag + " passband", std::to_string(lo) + "," + std::to_string(hi), std::to_string(g["lo"].num()) + "," + std::to_string(g["hi"].num()));
            const double c = g["centre"].num() > 0 ? g["centre"].num() : g["dial"].num();
            const std::string lab = wideVfoLabel(m, c, g["blk"].str());
            if (lab != g["label"].str()) fail(tag + " label", lab, g["label"].str());
        } else if (k == "fill") {
            const bool f = wideVfoFill(g["lo"].num(), g["hi"].num(), g["w"].num());
            if (f != g["fill"].boolean())
                fail("fill " + std::to_string(g["lo"].num()) + ".." + std::to_string(g["hi"].num()) + " / " + std::to_string(g["w"].num()),
                     f ? "true" : "false", g["fill"].boolean() ? "true" : "false");
        }
    }
    if (nTable != 3) fail("table size", std::to_string(nTable), "3");
    std::printf("wide VFO port: %d cases, %d failed\n", n, bad);
    return bad || n == 0 ? 1 : 0;
}
