// tool-skyplot-fixtures.cpp — SYNTHETIC sky-plot data (beta94; not real data): a heatmap's worth (≈ 55 % of the sky, six
// dots a cell over 20 days, strongest in the east, blocked low in the west) and a tracks' worth (three passes).
//   tool-skyplot-fixtures heat.json tracks.json                — the clients' fixtures (test/fixtures/skyplot/, T fixed)
//   tool-skyplot-fixtures --dirs <heatGalleryDir> <tracksGalleryDir> — seeded gallery folders (skyplot.dat + skycells.dat)
//                                                                for scripts/shot-web-skyplot.mjs, timed from now
// c++ -std=c++17 -I android/app/src/main/cpp vibeserver/tool-skyplot-fixtures.cpp -o tool-skyplot-fixtures
#include "vibe_skyplot.h"
#include <cstdio>
#include <cstring>
using namespace vibe::skyplot;

static Dot mk(double az, double el, int snr, uint32_t t, uint32_t norad) {
    Dot d; d.azDeg = float(az); d.elDeg = float(el); d.snr = int8_t(snr); d.t = t; d.kind = kOrbcomm; d.count = 4; d.norad = norad; d.radio = "aaaa1111"; return d;
}
static std::vector<Dot> passes(uint32_t T) {
    std::vector<Dot> v;
    for (int p = 0; p < 3; p++) for (int k = 0; k <= 120; k++) {
        const double f = k / 120.0, az = std::fmod(200 + p * 40 + f * 150, 360), el = 70 * std::sin(f * M_PI) * (0.6 + 0.2 * p);
        const double s = 4 + 10 * std::max(0.0, std::cos((az - 90) * M_PI / 180)) + el / 10;
        v.push_back(mk(az, std::max(0.5, el), (az > 250 && az < 300 && el < 12) ? kNothing : int(std::lround(s)), T - 7200 + p * 1800 + k * 5, 41185 + p));
    }
    return v;
}
static std::vector<Dot> sky(uint32_t T) {
    std::vector<Dot> v;
    for (int i = 0; i < cells::total(); i++) {
        double a0, a1, e0, e1; cells::bounds(i, a0, a1, e0, e1);
        const double az = 0.5 * (a0 + a1), el = 0.5 * (e0 + e1);
        if (el > 75 || (el < 15 && (az > 320 || az < 40))) continue;
        for (int k = 0; k < 6; k++) {
            const bool blocked = az > 250 && az < 300 && el < 12;
            const int s = int(std::lround(4 + 10 * std::max(0.0, std::cos((az - 90) * M_PI / 180)) + el / 10 + (k % 3) - 1));
            v.push_back(mk(az, el, blocked ? kNothing : s, T - 86400 * uint32_t(k % 20) - 9000, 41185 + uint32_t(k % 5)));
        }
    }
    std::sort(v.begin(), v.end(), [](const Dot& a, const Dot& b) { return a.t < b.t; });
    return v;
}
int main(int argc, char** argv) {
    if (argc == 4 && std::strcmp(argv[1], "--dirs") == 0) {
        const uint32_t T = uint32_t(std::time(nullptr));
        Store h; h.setDir(argv[2]); auto s = sky(T); auto p = passes(T); s.insert(s.end(), p.begin(), p.end()); h.add(s); h.flushCells();
        Store t; t.setDir(argv[3]); t.add(passes(T)); t.flushCells();
        std::printf("seeded %zu + %zu dots\n", s.size(), passes(T).size());
        return 0;
    }
    if (argc != 3) { std::fprintf(stderr, "usage: see the header\n"); return 2; }
    const uint32_t T = 1795600000;
    const auto ps = passes(T), sk = sky(T);
    std::vector<Dot> newest(ps.rbegin(), ps.rend());
    std::map<std::string, uint64_t> radios{{"aaaa1111", ps.size()}};
    cells::Map few, many;
    for (auto& d : ps) cells::addDot(few[cells::index(d.azDeg, d.elDeg)], d.snr, d.t);
    for (auto& d : sk) cells::addDot(many[cells::index(d.azDeg, d.elDeg)], d.snr, d.t);
    int a0, a1; std::string nm; cells::strongest(many, a0, a1, nm);
    std::FILE* f = std::fopen(argv[1], "w"); std::fputs(json(newest, radios, "orbcomm", false, many, 30).c_str(), f); std::fclose(f);
    f = std::fopen(argv[2], "w"); std::fputs(json(newest, radios, "orbcomm", false, few, 30).c_str(), f); std::fclose(f);
    std::printf("strongest %s %d-%d; heatmap due: many %d, few %d\n", nm.c_str(), a0, a1, cells::heatmapDue(many), cells::heatmapDue(few));
    return 0;
}
