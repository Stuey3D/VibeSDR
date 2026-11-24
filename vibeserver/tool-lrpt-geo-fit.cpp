// tool-lrpt-geo-fit.cpp — THE RESIDUAL STUDY behind the LRPT map outline (docs/v12/LRPT-MAP-OVERLAY.md §Measured). A
// developer tool, kept so the numbers in the doc can be re-made: it places a real pass's picture with vibe_lrpt_geo.h
// under trial parameters and either scores it against coastlines or writes the land mask the model predicts.
//
//   tool-lrpt-geo-fit sats.json NORAD CAPTURE_UNIX strips.txt chan.pgm coast.txt MODE HALF SIGN TOFF_MS ROLL PITCH YAW [out]
//     MODE score   mean |land − sea| either side of each projected coast vertex (3 px out), at no shift
//          shift   …and the (dx, dy) that maximises it, overall and per third of the swath   (env BOX=x0,y0,x1,y1)
//          mask    the predicted land mask (255 land, 0 sea, 128 off the Earth) → [out].pgm, for scripts/lrpt_geo_ncc.py
//          render  the coast points burnt in yellow → [out].png
//     env GEOC=1 points the frame at the GEOCENTRIC nadir (the product uses the geodetic one — the study's finding).
//   strips.txt / chan.pgm come from tool-lrpt-geo (…-strips.txt, …-apidNN.pgm). coast.txt: "n lon lat lon lat …" per
//   line, densified to 0.005° (made from Natural Earth's 10m coastline); the land mask reads land.pgm (Natural Earth 10m
//   land, rasterised at 0.01° over 40–72°N, 20°W–35°E) from the working directory — scripts/lrpt_geo_ncc.py --prep
//   writes both from .mapdata-cache.
// Build: c++ -std=c++17 -O2 -I android/app/src/main/cpp vibeserver/tool-lrpt-geo-fit.cpp -o tool-lrpt-geo-fit
#include "vibe_lrpt_geo.h"
#include "vibe_png.h"
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
using namespace vibelrpt::geo;
using namespace vibe::sat;

struct Img { int w = 0, h = 0; std::vector<uint8_t> px; int at(int x, int y) const { return (x < 0 || y < 0 || x >= w || y >= h) ? -1 : px[size_t(y) * w + x]; } };
Img readPgm(const char* p) { Img im; FILE* f = fopen(p, "rb"); int mx; fscanf(f, "P5 %d %d %d", &im.w, &im.h, &mx); fgetc(f); im.px.resize(size_t(im.w) * im.h); fread(im.px.data(), 1, im.px.size(), f); fclose(f); return im; }

std::vector<std::vector<std::pair<double,double>>> coast;
Elements el;
std::vector<std::pair<int,int>> strips;   // row0, ms
double dayStart = 0;

struct P { double half = 54.4, toff = 0, roll = 0, pitch = 0, yaw = 0, period = -1; int sign = -1; };

bool makeGeo(const P& p, int h, Georef& g) {
    // fit stamp = a + b * strip
    double sx=0, sy=0, sxx=0, sxy=0; int n=0;
    for (auto& s : strips) if (s.second >= 0) { double x = s.first / 8.0, y = s.second; sx+=x; sy+=y; sxx+=x*x; sxy+=x*y; n++; }
    double b = (n*sxy - sx*sy)/(n*sxx - sx*sx), a = (sy - b*sx)/n;
    if (p.period > 0) { b = p.period; a = (sy - b*sx)/n; }
    std::vector<double> t(h);
    for (int r = 0; r < h; r++) t[r] = dayStart + (a + b * (r / 8.0) + p.toff) / 1000.0;
    g.ins.halfScanDeg = p.half; g.ins.scanSign = p.sign; g.rollDeg = p.roll; g.pitchDeg = p.pitch; g.yawDeg = p.yaw; g.geodeticNadir = getenv("GEOC") == nullptr;
    std::string why; return g.build(el, t, why);
}

// project all coast points; return list of (x,y,nx,ny)
struct Q { double x, y, nx, ny; };
std::vector<Q> project(const Georef& g, int h) {
    std::vector<Q> out;
    for (auto& l : coast) {
        std::vector<std::pair<double,double>> pr; std::vector<bool> ok;
        for (auto& v : l) { double c, r; bool k = g.toImage(v.second, v.first, c, r); pr.push_back({c, r}); ok.push_back(k && r >= 0 && r < h); }
        for (size_t i = 1; i + 1 < l.size(); i++) {
            if (!ok[i] || !ok[i-1] || !ok[i+1]) continue;
            double dx = pr[i+1].first - pr[i-1].first, dy = pr[i+1].second - pr[i-1].second, L = std::sqrt(dx*dx+dy*dy);
            if (L < 0.3 || L > 6) continue;
            out.push_back({pr[i].first, pr[i].second, -dy/L, dx/L});
        }
    }
    return out;
}
double score(const std::vector<Q>& q, const Img& im, double ox, double oy, int k, int* used = nullptr) {
    double s = 0; int n = 0;
    for (auto& e : q) {
        double x = e.x + ox, y = e.y + oy;
        int a = im.at(int(std::lround(x + k*e.nx)), int(std::lround(y + k*e.ny))), b = im.at(int(std::lround(x - k*e.nx)), int(std::lround(y - k*e.ny)));
        if (a <= 0 || b <= 0) continue;   // missing data (black)
        s += std::fabs(double(a - b)); n++;
    }
    if (used) *used = n;
    return n ? s / n : 0;
}

int main(int argc, char** argv) {
    { std::ifstream f(argv[1]); std::stringstream ss; ss << f.rdbuf(); json::Value doc; json::parseValue(ss.str(), doc);
      long long want = atoll(argv[2]);
      for (auto& x : doc.get("satellites")->a) { const json::Value* o = x.get("omm"); if (!o) continue; Elements e; std::string err; if (fromOmm(o->flat(), e, err) && e.norad == want) el = e; } }
    double cap = atof(argv[3]); dayStart = std::floor(cap / 86400.0) * 86400.0;
    { FILE* f = fopen(argv[4], "r"); int r, ms; while (fscanf(f, "%d %d", &r, &ms) == 2) strips.push_back({r, ms}); fclose(f); }
    Img im = readPgm(argv[5]);
    { FILE* f = fopen(argv[6], "r"); int n; while (fscanf(f, "%d", &n) == 1) { std::vector<std::pair<double,double>> l(n); for (int i = 0; i < n; i++) fscanf(f, "%lf %lf", &l[i].first, &l[i].second); coast.push_back(l); } fclose(f); }
    std::string mode = argc > 7 ? argv[7] : "score";
    P p; int ai = 8;
    auto num = [&](double d) { return ai < argc ? atof(argv[ai++]) : d; };
    p.half = num(p.half); p.sign = int(num(p.sign)); p.toff = num(p.toff); p.roll = num(0); p.pitch = num(0); p.yaw = num(0);
    Georef g;
    if (mode == "score" || mode == "shift") {
        makeGeo(p, im.h, g); auto q = project(g, im.h);
        if (const char* b = getenv("BOX")) { double x0,y0,x1,y1; sscanf(b, "%lf,%lf,%lf,%lf", &x0,&y0,&x1,&y1); std::vector<Q> k; for (auto& e : q) if (e.x>=x0&&e.x<x1&&e.y>=y0&&e.y<y1) k.push_back(e); q = k; }
        int used; double s0 = score(q, im, 0, 0, 3, &used);
        printf("half %.2f sign %d toff %.0f roll %.2f pitch %.2f yaw %.2f: %zu pts, %d used, score %.2f\n", p.half, p.sign, p.toff, p.roll, p.pitch, p.yaw, q.size(), used, s0);
        if (mode == "shift") {
            double best = -1; int bx = 0, by = 0;
            for (int dy = -40; dy <= 40; dy++) for (int dx = -40; dx <= 40; dx++) { double s = score(q, im, dx, dy, 3); if (s > best) { best = s; bx = dx; by = dy; } }
            if (getenv("MAP")) for (int dy = -8; dy <= 8; dy+=2) { for (int dx = -8; dx <= 8; dx+=2) printf("%6.2f", score(q, im, dx, dy, 3)); printf("\n"); }
            printf("best shift dx %d dy %d score %.2f\n", bx, by, best);
            // residual per column band: left / centre / right thirds
            for (int band = 0; band < 3; band++) {
                std::vector<Q> sub; for (auto& e : q) if (int(e.x * 3 / im.w) == band) sub.push_back(e);
                double bb = -1; int x0 = 0, y0 = 0;
                for (int dy = -25; dy <= 25; dy++) for (int dx = -25; dx <= 25; dx++) { double s = score(sub, im, dx, dy, 3); if (s > bb) { bb = s; x0 = dx; y0 = dy; } }
                int u; score(sub, im, 0, 0, 3, &u);
                printf("  band %d: %zu pts (%d with data), best dx %d dy %d (score %.2f, at 0: %.2f)\n", band, sub.size(), u, x0, y0, bb, score(sub, im, 0, 0, 3));
            }
        }
        return 0;
    }
    if (mode == "mask") {
        makeGeo(p, im.h, g);
        Img land = readPgm("land.pgm");
        std::vector<uint8_t> m(size_t(im.w) * im.h, 128);
        for (int y = 0; y < im.h; y++) for (int x = 0; x < im.w; x++) {
            double la, lo; if (!g.toGround(x, y, la, lo)) continue;
            int lx = int((lo + 20.0) / 0.01), ly = int((72.0 - la) / 0.01);
            int v = land.at(lx, ly); if (v >= 0) m[size_t(y) * im.w + x] = v;
        }
        FILE* f = fopen(argv[ai], "wb"); fprintf(f, "P5\n%d %d\n255\n", im.w, im.h); fwrite(m.data(), 1, m.size(), f); fclose(f);
        return 0;
    }
    if (mode == "render") {
        makeGeo(p, im.h, g); auto q = project(g, im.h);
        std::vector<uint8_t> rgb(size_t(im.w) * im.h * 3);
        for (size_t i = 0; i < im.px.size(); i++) rgb[i*3] = rgb[i*3+1] = rgb[i*3+2] = im.px[i];
        for (auto& e : q) { int x = int(std::lround(e.x)), y = int(std::lround(e.y)); if (x < 0 || y < 0 || x >= im.w || y >= im.h) continue; size_t i = size_t(y) * im.w + x; rgb[i*3] = 255; rgb[i*3+1] = 220; rgb[i*3+2] = 0; }
        std::string png = vibe::png::encode(im.w, im.h, 3, rgb.data());
        FILE* f = fopen(argv[ai], "wb"); fwrite(png.data(), 1, png.size(), f); fclose(f);
        return 0;
    }
}
