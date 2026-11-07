// wefax_golden_test.cpp — the handheld's WEFAX port (src/dec_pic_align.h) against the app's own wefaxAlign.ts /
// wefaxCrisp.ts (gen_wefax_golden.ts): every scenario's received rows go through chartAlignStep line by line, exactly as
// the TS did, and every state change must match; then the crisp rendering's bytes. Silent: no GL, no audio.
//   wefax_golden_test <prefix>     (reads <prefix>.rows.gz and <prefix>.txt)
#include "../src/dec_pic_align.h"

#include <zlib.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace hh::wfx;

static std::string f6(double x) { if (std::fabs(x) < 5e-7) x = 0; char b[64]; std::snprintf(b, sizeof b, "%.6f", x); return b; }
static const char* viaName(Via v) { return v == V_BORDER ? "border" : v == V_STRIP ? "strip" : v == V_MARGIN ? "margin" : "-"; }
static std::string sig(const AlignState& st) {
    std::string al = !st.lookedAt ? "u" : !st.hasAl ? "null" : f6(st.al.shift) + "," + f6(st.al.slant);
    return al + "|" + viaName(st.via) + "|" + (st.atEdge ? "1" : "0") + "|" + (st.hasSlant ? f6(st.slant) : "u") + "|" + std::to_string(st.n)
         + "|" + (st.refined ? "1" : "0") + "|" + (st.retried ? "1" : "0") + "|" + std::to_string(st.flat);
}
static std::string core(const std::string& s) {
    std::vector<std::string> p; std::string t; std::stringstream ss(s);
    while (std::getline(ss, t, '|')) p.push_back(t);
    std::string o;
    for (size_t i = 0; i < p.size(); i++) if (i != 4 && i != 7) o += p[i] + "|";
    return o;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: wefax_golden_test <prefix>\n"); return 2; }
    const std::string pre = argv[1];
    gzFile gz = gzopen((pre + ".rows.gz").c_str(), "rb");
    if (!gz) { std::fprintf(stderr, "cannot open %s.rows.gz\n", pre.c_str()); return 2; }
    std::ifstream tf(pre + ".txt");
    std::vector<std::string> lines; std::string l;
    while (std::getline(tf, l)) lines.push_back(l);
    size_t li = 0;
    int scen = 0, bad = 0, events = 0;
    auto rd = [&](void* p, unsigned n) { return gzread(gz, p, n) == (int)n; };
    for (;;) {
        uint32_t nl;
        if (!rd(&nl, 4)) break;
        std::string name(nl, '\0'); rd(&name[0], nl);
        double dial; uint32_t W, n;
        rd(&dial, 8); rd(&W, 4); rd(&n, 4);
        std::vector<std::vector<uint8_t>> rows(n, std::vector<uint8_t>(W));
        for (uint32_t y = 0; y < n; y++) rd(rows[y].data(), W);
        if (li >= lines.size() || lines[li] != "S|" + name) { std::fprintf(stderr, "out of step at %s\n", name.c_str()); return 1; }
        li++;
        std::vector<std::string> want;
        while (li < lines.size() && lines[li].compare(0, 2, "E|") == 0) want.push_back(lines[li++]);
        const std::string wantC = li < lines.size() ? lines[li++] : "";
        // replay
        const double station = preset(dial).slant;
        const Format fmt = format(dial);
        AlignState st;
        Rows so;
        std::vector<std::string> got;
        std::string last;
        for (uint32_t y = 0; y < n; y++) {
            so.push_back(rows[y].data());
            const bool ret = chartAlignStep(st, so, (int)W, station, rows[y].data(), fmt);
            const std::string s = sig(st), c = core(s);
            if (ret || c != last) got.push_back("E|" + std::to_string(y) + "|" + (ret ? "1" : "0") + "|" + s);
            last = c;
        }
        bool ok = got == want;
        events += (int)want.size();
        if (!ok) {
            bad++;
            std::printf("FAIL %s\n", name.c_str());
            for (size_t i = 0; i < std::max(got.size(), want.size()) && i < 12; i++)
                std::printf("   ts  %s\n   c++ %s\n", i < want.size() ? want[i].c_str() : "-", i < got.size() ? got[i].c_str() : "-");
        }
        // crisp
        Hist h;
        const uint32_t top = std::min<uint32_t>(80, n);
        for (uint32_t y = 0; y < top; y++) addToHist(h, rows[y].data(), (int)W);
        const Levels lv = crispLevels(h);
        uint32_t hsh = 2166136261u;
        if (lv.ok) {
            std::vector<uint8_t> o(W); std::vector<float> v;
            auto rowF = [&](int j) -> const uint8_t* { return j >= 0 && j < (int)top ? rows[(size_t)j].data() : nullptr; };
            for (uint32_t y = 0; y < top; y++) { crispLine(rowF, (int)y, (int)W, lv, o.data(), v); for (uint32_t x = 0; x < W; x++) hsh = (hsh ^ o[x]) * 16777619u; }
        }
        const std::string gotC = "C|" + (lv.ok ? f6(lv.lo) + "," + f6(lv.hi) : std::string("none")) + "|" + std::to_string(hsh);
        if (gotC != wantC) { if (ok) bad++; ok = false; std::printf("FAIL %s crisp: ts %s c++ %s\n", name.c_str(), wantC.c_str(), gotC.c_str()); }
        scen++;
    }
    gzclose(gz);
    std::printf("wefax golden: %d scenarios, %d state changes compared, %d FAIL\n", scen, events, bad);
    return bad || !scen ? 1 : 0;
}
