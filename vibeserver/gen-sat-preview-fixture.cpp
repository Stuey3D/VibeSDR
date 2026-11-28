// gen-sat-preview-fixture — ★★★ beta97 PASS PREVIEW: writes the server's OWN answers for a few fixed passes, so the
// clients' pass-track sampler (src/services/satPreview.ts) is tested against the server's prediction, not a copy of it.
//
// For each case (the 2026-11-19 CelesTrak fixtures, the receivers and passes of testdata-sat/reference-passes.txt) it
// prints the engine's pass (nextPass: aos / tca / los / maxEl / azimuths) and the {"type":"sat_track"} message EXACTLY as
// the server sends it (trackJson + geoJson, the shim's wording), plus the engine's Doppler and elevation every 30 s.
//   g++ -std=c++17 -O2 -I android/app/src/main/cpp vibeserver/gen-sat-preview-fixture.cpp -o gen -lpthread
//   ./gen vibeserver/testdata-sat > test/fixtures/sat-preview/passes.json
#include "vibe_sat_engine.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace vibe::sat;
static std::string readFile(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "vibeserver/testdata-sat";
    struct Case { const char* name; long long norad; double lat, lon, from, hz; };
    const Case cases[] = {
        { "ISS Sao Paulo 56", 25544, -23.5, -46.6, 1795609000, 145.8e6 },
        { "ISS Sao Paulo 9",  25544, -23.5, -46.6, 1795600000, 145.8e6 },
        { "SO-50 Sao Paulo 36", 27607, -23.5, -46.6, 1795605000, 436.795e6 },
        { "AO-7 IO92 24", 7530, 52.270833, -0.875, 1795600000, 145.9775e6 },
    };
    std::printf("{\"note\":\"vibeserver/gen-sat-preview-fixture.cpp: the server's engine on the 2026-11-19 fixtures\",\"cases\":[\n");
    bool firstC = true;
    for (const auto& c : cases) {
        Engine e;
        e.loadOmm(readFile(dir + "/celestrak-amateur-2026-11-19.json"));
        e.loadOmm(readFile(dir + "/celestrak-weather-2026-11-19.json"));
        e.setObserver(c.lat, c.lon);
        Pass p;
        if (!e.nextPass(c.norad, c.from, 0, p)) { std::fprintf(stderr, "no pass for %s\n", c.name); continue; }
        double t0 = 0, t1 = 0, st = 0, tca = 0;
        const std::string geo = e.geoJson(c.norad, p.aos, p.los, t0, t1, st, tca);
        const std::string msg = "{\"type\":\"sat_track\",\"norad\":" + std::to_string(c.norad) + ",\"aos\":" + fmtNum(p.aos, 0)
            + ",\"los\":" + fmtNum(p.los, 0) + ",\"pts\":" + e.trackJson(c.norad, p.aos, p.los)
            + ",\"geo\":" + geo + ",\"geoT0\":" + fmtNum(t0, 0) + ",\"geoT1\":" + fmtNum(t1, 0) + ",\"geoStep\":" + fmtNum(st, 2)
            + (tca > 0 ? ",\"tca\":" + fmtNum(tca, 0) : std::string()) + "}";
        std::string dop = "[";
        for (double t = p.aos; t <= p.los; t += 30) {
            double d = 0, r = 0, el = 0;
            if (!e.dopplerFor(c.norad, c.hz, t, d, r, &el)) continue;
            if (dop.size() > 1) dop += ",";
            dop += "[" + fmtNum(t, 1) + "," + fmtNum(d, 1) + "," + fmtNum(el, 2) + "]";
        }
        dop += "]";
        std::printf("%s{\"name\":\"%s\",\"norad\":%lld,\"rx\":{\"lat\":%.6f,\"lon\":%.6f},\"hz\":%.0f,"
                    "\"pass\":{\"aos\":%.3f,\"tca\":%.3f,\"los\":%.3f,\"maxEl\":%.3f,\"aosAz\":%.2f,\"losAz\":%.2f},\"track\":%s,\"doppler\":%s}\n",
                    firstC ? "" : ",", c.name, c.norad, c.lat, c.lon, c.hz, p.aos, p.tca, p.los, p.maxElDeg, p.aosAzDeg, p.losAzDeg, msg.c_str(), dop.c_str());
        firstC = false;
    }
    std::printf("]}\n");
    return 0;
}
