// The handheld TUNED STATION LABEL port's golden test: replays tuned_label_inputs.json through src/tuned_label.h and
// requires gen_tuned_label_golden.ts's lines (the app's src/utils/tunedStation.ts + wfStamps.ts stampColour) exactly —
// precedence, tolerance, EiBi on air now, the live-name / VTS / moving-dial suppression, the text, the placement and the
// colour. Pure: no SDL, no GL, no audio.   handheld-ui/test/run_tuned_label.sh
#include "../src/json.h"
#include "../src/tuned_label.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace hh;

static std::string jsNum(double v) {
    char b[48];
    if (v == std::floor(v) && std::fabs(v) < 1e15) std::snprintf(b, sizeof b, "%.0f", v);
    else std::snprintf(b, sizeof b, "%.17g", v);
    return b;
}
static std::string readAll(const std::string& p) { std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: tuned_label_test <inputs.json> <golden.txt>\n"); return 2; }
    const Json in = Json::parse(readAll(argv[1]));
    if (in.isNull()) { std::fprintf(stderr, "inputs do not parse\n"); return 2; }
    std::vector<tl::Row> rows;
    for (auto& r : in["rows"].a) rows.push_back({r["frequency"].num(), r["name"].str(), r["source"].str(), r["comment"].str(), r["mode"].str()});
    std::vector<std::string> got;
    for (auto& q : in["queries"].a) {
        const double dial = q["dial"].num(); const int now = (int)q["now"].num();
        const tl::Station st = tl::tunedStation(rows, dial, now);
        const std::string lb = tl::tunedLabel(rows, dial, now, q["live"].str(), q["vts"].boolean(), q["settled"].boolean());
        got.push_back("Q " + jsNum(dial) + " " + jsNum(now) + " " + (st.ok ? st.source + " " + jsNum(st.hz) : "-") + " |" + lb);
    }
    for (auto& p : in["place"].a) {
        char b[64]; std::snprintf(b, sizeof b, "X %.4f", tl::labelX(p["nx"].num(), p["w"].num(), p["width"].num()));
        got.push_back(b);
    }
    for (auto& c : in["colours"].a) {
        const std::string nh = c["needle"].str();
        const unsigned needle = (unsigned)std::strtoul(nh.c_str() + 1, nullptr, 16);
        unsigned char lut[1024] = {0};
        const bool has = c["lut"].kind == Json::Arr;
        if (has) { int k = 0; for (int i : {96, 160, 224}) { const unsigned n = (unsigned)std::strtoul(c["lut"].a[k++].str().c_str() + 1, nullptr, 16);
            lut[i * 4] = (n >> 16) & 255; lut[i * 4 + 1] = (n >> 8) & 255; lut[i * 4 + 2] = n & 255; lut[i * 4 + 3] = 255; } }
        char b[64]; std::snprintf(b, sizeof b, "C %s #%06x", nh.c_str(), tl::stampColour(needle, has ? lut : nullptr));
        got.push_back(b);
    }
    std::vector<std::string> want;
    { std::stringstream ss(readAll(argv[2])); std::string l; while (std::getline(ss, l)) want.push_back(l); }
    int fail = 0;
    const size_t n = std::max(got.size(), want.size());
    for (size_t i = 0; i < n; i++) {
        const std::string g = i < got.size() ? got[i] : "(missing)", w = i < want.size() ? want[i] : "(missing)";
        if (g != w) { fail++; std::printf("   FAIL line %zu\n     want %s\n     got  %s\n", i + 1, w.c_str(), g.c_str()); }
    }
    std::printf("   %s %zu golden lines (tunedStation.ts / stampColour, line for line)\n", fail ? "FAIL" : "ok  ", want.size());
    return fail ? 1 : 0;
}
