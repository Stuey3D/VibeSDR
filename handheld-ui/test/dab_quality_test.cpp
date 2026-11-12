// dab_quality_test.cpp — the R36S's DAB meter (src/dab_quality.h) replays the app's own outputs (gen_dab_quality_golden.ts):
// every push must give the same level, words, detail, starting flag, MER and line. run_dab_quality_test.sh.
#include "../src/dab_quality.h"
#include <cmath>
#include <fstream>
#include <iostream>

using namespace hh;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s golden.jsonl\n", argv[0]); return 2; }
    std::ifstream in(argv[1]);
    std::string ln, cur;
    DabQualityMeter m;
    int n = 0, bad = 0;
    while (std::getline(in, ln)) {
        if (ln.empty()) continue;
        const Json g = Json::parse(ln);
        if (g["name"].str() != cur) { cur = g["name"].str(); m.reset(); }
        const DabQuality& q = m.push(g["r"], g["t"].num());
        ++n;
        auto chk = [&](const char* what, const std::string& got, const std::string& want) {
            if (got == want) return;
            ++bad; std::printf("FAIL %s @%g %s\n   got  %s\n   want %s\n", cur.c_str(), g["t"].num(), what, got.c_str(), want.c_str());
        };
        const std::string line = q.label + " · " + q.advice + (q.detail.empty() ? "" : " · " + q.detail);
        char mg[32], mw[32];
        std::snprintf(mg, sizeof mg, "%.6f", q.hasMer ? q.merDb : -1.0);
        std::snprintf(mw, sizeof mw, "%.6f", g["mer"].kind == Json::Num ? g["mer"].num() : -1.0);
        chk("level", std::to_string(q.level), std::to_string((int)g["level"].num()));
        chk("label", q.label, g["label"].str());
        chk("short", q.shortL, g["short"].str());
        chk("advice", q.advice, g["advice"].str());
        chk("detail", q.detail, g["detail"].str());
        chk("starting", q.starting ? "1" : "0", g["starting"].boolean() ? "1" : "0");
        chk("mer", mg, mw);
        chk("line", line, g["line"].str());
    }
    std::printf("dab_quality_test: %d pushes, %d differences\n", n, bad);
    return bad || !n ? 1 : 0;
}
