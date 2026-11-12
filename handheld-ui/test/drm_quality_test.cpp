// drm_quality_test.cpp — the R36S's DRM meter (src/drm_quality.h) replays the app's own outputs (gen_drm_quality_golden.ts):
// every push must give the same level, words, detail, box reading and line. run_drm_quality_test.sh.
#include "../src/drm_quality.h"
#include <fstream>
#include <iostream>
#include <map>

using namespace hh;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s golden.jsonl\n", argv[0]); return 2; }
    std::ifstream in(argv[1]);
    std::string ln, cur;
    DrmQualityMeter m;
    int n = 0, bad = 0;
    while (std::getline(in, ln)) {
        if (ln.empty()) continue;
        const Json g = Json::parse(ln);
        if (g["name"].str() != cur) { cur = g["name"].str(); m.reset(); }
        const DrmQuality& q = m.push(g["r"], g["t"].num());
        ++n;
        auto chk = [&](const char* what, const std::string& got, const std::string& want) {
            if (got == want) return;
            ++bad; std::printf("FAIL %s @%g %s\n   got  %s\n   want %s\n", cur.c_str(), g["t"].num(), what, got.c_str(), want.c_str());
        };
        chk("level", std::to_string(q.level), std::to_string((int)g["level"].num()));
        chk("label", q.label, g["label"].str());
        chk("short", q.shortL, g["short"].str());
        chk("advice", q.advice, g["advice"].str());
        chk("detail", q.detail, g["detail"].str());
        chk("box", q.boxReading(), g["box"].str());
        chk("line", q.line(), g["line"].str());
    }
    std::printf("drm_quality_test: %d pushes, %d differences\n", n, bad);
    return bad || !n ? 1 : 0;
}
