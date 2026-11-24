// tool-lrpt-geo.cpp — the METEOR LRPT MAP OUTLINE on a developer capture (docs/v12/LRPT-MAP-OVERLAY.md).
//
// Replays a capture (VIBE_LRPT_IQ_DUMP, as tool-lrpt-replay) through the server's own Run with the satellite engine's
// answers (72k, "in range" while the logged feed-forward is on), with the outline switched on exactly as the server does
// it: candidates = every METEOR with LRPT in a sats.json, this server's place, OUR clock = the capture's start (the Unix
// time in its name) + the seconds fed. For each saved pass it writes:
//   <prefix>-N-plain.png     the picture the gallery keeps (RGB 221, or the first channel)
//   <prefix>-N-outline.png   the same with the outline burnt in (what DOWNLOAD gives with the toggle on)
//   <prefix>-N-geo.json      the outline JSON the gallery keeps beside it (or the reason there is none)
//   <prefix>-N-apidNN.pgm    each channel, levelled (for the residual study: scripts/lrpt_geo_residual.py)
//   <prefix>-N-strips.txt    row of each strip and its stamp (ms of day, −1 none)
// Options: --sats FILE (sats.json; default vibeserver/testdata-sat/sats-2026-11-19.json) --lat/--lon (default IO92NH,
// the Pi 500's square) --half DEG (the scan's half-angle, for the study) --geocentric --roll/--pitch/--yaw DEG
// (diagnostics only — never in the product).
//
// Build: c++ -std=c++17 -O2 -I android/app/src/main/cpp vibeserver/tool-lrpt-geo.cpp -o tool-lrpt-geo
#include "vibe_lrpt_run.h"
#include "vibe_lrpt_burn.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace vibelrpt;

namespace {

std::vector<geo::Candidate> loadCandidates(const std::string& path) {
    std::vector<geo::Candidate> out;
    std::ifstream f(path); std::stringstream ss; ss << f.rdbuf();
    vibe::sat::json::Value doc;
    if (!vibe::sat::json::parseValue(ss.str(), doc)) return out;
    const vibe::sat::json::Value* sats = doc.get("satellites");
    if (!sats) return out;
    for (const auto& x : sats->a) {
        bool lrpt = false;
        if (const auto* tx = x.get("transmitters")) for (const auto& t : tx->a) if (t.str("consumer") == "lrpt" || t.str("mode") == "lrpt") lrpt = true;
        const auto* omm = x.get("omm");
        if (!lrpt || !omm) continue;
        geo::Candidate c; std::string err;
        if (!vibe::sat::fromOmm(omm->flat(), c.el, err)) continue;
        c.name = x.str("name"); c.norad = c.el.norad;
        out.push_back(c);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::string sats = "vibeserver/testdata-sat/sats-2026-11-19.json", prefix = "lrpt-geo";
    double lat = 52.3125, lon = -0.875;
    std::vector<std::string> pos;
    geo::Instrument ins;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto val = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--sats") sats = val();
        else if (a == "--lat") lat = std::atof(val().c_str());
        else if (a == "--lon") lon = std::atof(val().c_str());
        else if (a == "--out") prefix = val();
        else if (a == "--half") ins.halfScanDeg = std::atof(val().c_str());
        else pos.push_back(a);
    }
    if (pos.empty()) { std::fprintf(stderr, "usage: %s <capture.cf32> [--sats sats.json] [--lat L --lon L] [--out prefix] [--half deg]\n", argv[0]); return 2; }
    const std::string cf32 = pos[0], txt = cf32.substr(0, cf32.size() - 5) + ".txt";
    // the capture's start: lrpt-<unix>-<rate>.cf32
    const size_t sl = cf32.rfind("lrpt-");
    const double startUnix = sl == std::string::npos ? 0 : std::atof(cf32.c_str() + sl + 5);
    double rate = 300000;
    { const size_t dash = cf32.rfind('-'), dot = cf32.rfind('.'); if (dash != std::string::npos && dot > dash) rate = std::atof(cf32.substr(dash + 1, dot - dash - 1).c_str()); }
    std::map<long, double> dop;
    if (FILE* f = std::fopen(txt.c_str(), "r")) { char l[512]; while (std::fgets(l, sizeof l, f)) { if (l[0] == '#') continue; double t, d; if (std::sscanf(l, "%lf %lf", &t, &d) == 2) dop[long(t)] = d; } std::fclose(f); }
    const auto cands = loadCandidates(sats);
    std::printf("capture from %.0f (Unix), %zu METEOR candidates in %s, observer %.4f %.4f\n", startUnix, cands.size(), sats.c_str(), lat, lon);
    FILE* f = std::fopen(cf32.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", cf32.c_str()); return 1; }
    double tNow = 0;
    Run::Opts o; o.rateAuto = true;
    auto at = [&] { auto it = dop.upper_bound(long(tNow)); if (it == dop.begin()) return dop.empty() ? 0.0 : dop.begin()->second; --it; return it->second; };
    o.dopplerHz = at;
    o.satSymbolRate = [] { return 72000.0; };
    o.satInRange = [&] { return at() != 0.0 ? 1 : 0; };
    o.geoCandidates = [&] { return cands; };
    o.geoObserver = [&](vibe::sat::Observer& ob) { ob.set(lat, lon, 0); return true; };
    o.nowUnix = [&] { return startUnix + tNow; };
    o.log = [&](const std::string& l) { if (l.find("outline") != std::string::npos) std::printf("%7.1f s  %s\n", tNow, l.c_str()); };
    Receiver* cur = nullptr;
    o.made = [&](Receiver& r) { cur = &r; };
    int k = 0;
    o.pictures = [&](const std::vector<Picture>& ps, bool partial) {
        if (ps.empty() || !cur) return;
        const std::string base = prefix + "-" + std::to_string(k++);
        int s0, s1; cur->stripRange(s0, s1);
        if (FILE* st = std::fopen((base + "-strips.txt").c_str(), "w")) { for (int s = s0; s < s1; s++) std::fprintf(st, "%d %d\n", (s - s0) * 8, cur->stripTimeMs(s)); std::fclose(st); }
        for (int a : cur->liveApids()) {
            int w, h; auto g = channelImage(cur->channels().at(a), s0, s1, false, w, h);
            if (FILE* p = std::fopen((base + "-apid" + std::to_string(a) + ".pgm").c_str(), "wb")) { std::fprintf(p, "P5\n%d %d\n255\n", w, h); std::fwrite(g.data(), 1, g.size(), p); std::fclose(p); }
        }
        const Picture& p0 = ps.front();
        if (FILE* q = std::fopen((base + "-plain.png").c_str(), "wb")) { std::fwrite(p0.png.data(), 1, p0.png.size(), q); std::fclose(q); }
        if (FILE* q = std::fopen((base + "-geo.json").c_str(), "wb")) { const std::string& j = p0.geoJson.empty() ? "{\"why\":\"" + p0.geoWhy + "\"}" : p0.geoJson; std::fwrite(j.data(), 1, j.size(), q); std::fclose(q); }
        std::printf("%7.1f s  pass %s%s: %dx%d, %s\n", tNow, base.c_str(), partial ? " (partial)" : "", p0.w, p0.h,
                    p0.geoJson.empty() ? ("NO OUTLINE: " + p0.geoWhy).c_str() : ("outline " + std::to_string(p0.geoJson.size()) + " bytes").c_str());
        if (!p0.geoJson.empty()) {
            // the composite's pixels again (renderPictures made PNG bytes; rebuild the RGB here for the burnt-in copy)
            std::vector<uint8_t> rgb(size_t(p0.w) * p0.h * 3, 0);
            const bool comp = p0.apid == 0;
            int w, h;
            auto b = channelImage(cur->channels().at(comp ? 64 : p0.apid), s0, s1, !comp && p0.apid >= 67, w, h);
            std::vector<uint8_t> r = comp ? channelImage(cur->channels().at(65), s0, s1, false, w, h) : b;
            for (size_t i = 0; i < size_t(w) * h; i++) { rgb[i * 3] = r[i]; rgb[i * 3 + 1] = r[i]; rgb[i * 3 + 2] = b[i]; }
            burn::draw(rgb, w, h, p0.geoJson);   // ★ the server's own ?outline=1 drawing (vibe_lrpt_burn.h)
            const std::string png = vibe::png::encode(w, h, 3, rgb.data());
            if (FILE* q = std::fopen((base + "-outline.png").c_str(), "wb")) { std::fwrite(png.data(), 1, png.size(), q); std::fclose(q); }
        }
    };
    {
        Run run(o);
        std::vector<std::complex<float>> buf(size_t(rate / 20)); size_t got;
        while ((got = std::fread(buf.data(), sizeof(std::complex<float>), buf.size(), f)) > 0) { run.feedIq(buf.data(), int(got), rate, 0); tNow += got / rate; }
        run.finish(false);
    }
    std::fclose(f);
    std::printf("replayed %.1f s, %d pass(es)\n", tNow, k);
    return 0;
}
