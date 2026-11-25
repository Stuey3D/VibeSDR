// test-skyplot-cells — ★★★ beta94 THE SKY PLOT BY POSITION (vibe_skyplot.h namespace cells + Store's cell file): Stuart 11-25,
// "could keep it as tracks when less data then a heatmap when it gets more".
//
// §1 the grid: 2,292 cells, equal-area-ish (every band's cells within ±30 % of the mean area, the zenith not over-weighted),
//    index ↔ bounds round trip, the wrap at north.
// §2 aggregation: count, grey, mean / max SNR, last heard; this process's counts read before they are written; another
//    process sees them after the flush; the file built from an older dot log the first time.
// §3 expiry: a 3-day bucket past the 30-day window is gone from the reading and from the file at the next compaction.
// §4 storage bounds: 100 days of every cell heard every day by one radio, then by three — ≤ 400 KB; one radio keeps its
//    whole window; three keep the newest buckets.
// §5 AUTO: tracks under 30 % of the sky heard, the heatmap from it (with ≥ 3 dots a cell); the JSON says which.
// §6 the heatmap PNG: a heard cell's pixels in its mean SNR's colour, a grey cell grey, an unheard one the dome; TRACKS
//    draws dots instead; the title line ("HEATMAP - 30 DAYS - … CELLS HEARD - STRONGEST …").
// §7 the owner's reset: one radio's dots and cells gone, the other radio's kept.
// Silent. Run: scripts/run-tests.sh (VIBE_TESTS="test-skyplot-cells").
#include "vibe_skyplot.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) { g_pass++; } else { g_fail++; std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace vibe::skyplot;

static std::string tmpDir() {
    const char* base = std::getenv("TMPDIR");
    std::string p = std::string(base ? base : "/tmp") + "/vs-skycells-XXXXXX";
    std::vector<char> b(p.begin(), p.end()); b.push_back(0);
    return ::mkdtemp(b.data()) ? std::string(b.data()) : std::string();
}
static void rmDir(const std::string& d) { std::system(("rm -rf '" + d + "'").c_str()); }
static Dot dot(double az, double el, int snr, uint32_t t, const std::string& radio = "aaaa1111", uint32_t norad = 41189) {
    Dot d; d.azDeg = float(az); d.elDeg = float(el); d.snr = int8_t(snr); d.t = t; d.kind = kOrbcomm; d.count = 3; d.norad = norad; d.radio = radio; return d;
}
static const uint32_t T0 = 1795600000;   // 2026-11-25

int main() {
    std::printf("§1 the grid\n");
    {
        const int n = cells::total();
        CHECK(n == 2292, "2,292 cells (%d)", n);
        double amin = 1e9, amax = 0, asum = 0;
        for (int i = 0; i < n; i++) {
            double a0, a1, e0, e1; cells::bounds(i, a0, a1, e0, e1);
            const double area = (a1 - a0) * M_PI / 180 * (std::sin(e1 * M_PI / 180) - std::sin(e0 * M_PI / 180));
            if (e0 < 87) { amin = std::min(amin, area); amax = std::max(amax, area); }
            asum += area;
        }
        const double mean = asum / n;
        CHECK(amin > 0.7 * mean && amax < 1.3 * mean, "equal-area: every cell below the top band within ±30 %% of the mean (%.2f … %.2f of it)", amin / mean, amax / mean);
        CHECK(std::fabs(asum - 2 * M_PI) < 1e-6, "the cells tile the hemisphere (%.6f sr)", asum);
        bool rt = true;
        for (double el = 0.5; el < 90; el += 2.9) for (double az = 0.2; az < 360; az += 7.31) {   // (off the cells' edges)
            const int i = cells::index(az, el); double a0, a1, e0, e1; cells::bounds(i, a0, a1, e0, e1);
            if (!(az >= a0 && az < a1 && el >= e0 && el < e1)) rt = false;
        }
        CHECK(rt, "index ↔ bounds: every az/el lands in its own cell");
        CHECK(cells::index(359.99, 0.1) == cells::index(-0.01, 0.1) && cells::index(0, 0) == 0 && cells::index(10, 90) == n - 3 + 0, "north wraps; the top band is three cells");
    }

    std::printf("§2 aggregation\n");
    const std::string dir = tmpDir();
    {
        Store st; st.setDir(dir); st.setNowForTest(T0);
        st.add({ dot(100.1, 30.2, 10, T0 - 60), dot(100.2, 30.4, 14, T0 - 30), dot(100.3, 30.6, kNothing, T0 - 10) });
        struct stat sb{};
        CHECK(::stat((dir + "/skycells.dat").c_str(), &sb) == 0, "the first add builds the cell file");
        auto m = st.readCells(kOrbcomm, "aaaa1111");
        const auto& a = m[cells::index(100.2, 30.4)];
        CHECK(m.size() == 1 && a.n == 2 && a.grey == 1 && a.sum == 24 && a.maxSnr == 14 && cells::colourSnr(a) == 12, "n 2, grey 1, mean 12, max 14 (%u %u %lld %d)", a.n, a.grey, (long long)a.sum, a.maxSnr);
        CHECK(a.lastT / 60 == (T0 - 10) / 60, "last heard (to the minute)");
        // more, within the flush interval: in RAM, read here; not yet in the file for another process
        st.setNowForTest(T0 + 120);
        st.add({ dot(200, 50, 8, T0 + 100) });
        CHECK(st.readCells(kOrbcomm, "").size() == 2, "this process reads its unwritten counts");
        Store other; other.setDir(dir); other.setNowForTest(T0 + 120);
        CHECK(other.readCells(kOrbcomm, "").size() == 1, "another process does not see them yet (written every %u s)", Store::kCellFlushSec);
        st.flushCells();
        CHECK(other.readCells(kOrbcomm, "").size() == 2, "…and does after the flush");
        st.setNowForTest(T0 + 120 + Store::kCellFlushSec);
        st.add({ dot(200, 50, 12, T0 + 700) });
        CHECK(other.readCells(kOrbcomm, "")[cells::index(200, 50)].n == 2, "the interval past: the next add writes them");
        CHECK(st.readCells(kOrbcomm, "zzzz9999").size() == 0 && st.readCells(kMeteor, "").size() == 0, "filtered by radio and kind");
    }
    rmDir(dir);
    {   // an older server's dot log, no cell file yet
        const std::string d2 = tmpDir();
        Store st; st.setDir(d2); st.setNowForTest(T0);
        st.add({ dot(10, 10, 5, T0) });
        ::unlink((d2 + "/skycells.dat").c_str());
        auto m = st.readCells(kOrbcomm, "");
        CHECK(m.size() == 1 && m.begin()->second.n == 1, "the cell file is rebuilt from the dot log when missing");
        rmDir(d2);
    }

    std::printf("§3 expiry\n");
    {
        const std::string d3 = tmpDir();
        Store st; st.setDir(d3); st.setNowForTest(T0 - 40 * 86400);
        st.add({ dot(50, 20, 9, T0 - 40 * 86400) });
        st.flushCells();
        st.setNowForTest(T0 - 31 * 86400 + 3600);
        st.add({ dot(60, 20, 9, T0 - 31 * 86400) });   // in the bucket just past the window at T0 (rounding to 3 days)
        st.setNowForTest(T0);
        st.add({ dot(70, 20, 9, T0) });
        st.flushCells();
        auto m = st.readCells(kOrbcomm, "");
        CHECK(!m.count(cells::index(50, 20)) && m.count(cells::index(70, 20)), "a dot 40 days old is out of the reading, today's in");
        CHECK(cells::firstBucket(T0) == cells::bucketOf(T0) - 9, "the window: this bucket and the 9 before (30 days)");
        // the compaction drops it from the file too
        std::vector<Dot> many;
        for (int i = 0; i < 2292; i++) { double a0, a1, e0, e1; cells::bounds(i, a0, a1, e0, e1); many.push_back(dot(0.5 * (a0 + a1), 0.5 * (e0 + e1), 6, T0)); }
        for (int k = 0; k < 12; k++) { st.setNowForTest(T0 + k * 700); for (auto& x : many) x.radio = "r" + std::to_string(k) + "abcdef"; st.add(many); }
        st.flushCells();
        CHECK(st.oldestCellBucket() >= cells::firstBucket(T0), "after a compaction no expired bucket is left in the file (oldest %d, window from %d)",
              st.oldestCellBucket(), cells::firstBucket(T0));
        rmDir(d3);
    }

    std::printf("§4 storage bounds\n");
    {
        const std::string d4 = tmpDir();
        Store st; st.setDir(d4);
        std::vector<Dot> sky;
        for (int i = 0; i < 2292; i++) { double a0, a1, e0, e1; cells::bounds(i, a0, a1, e0, e1); sky.push_back(dot(0.5 * (a0 + a1), 0.5 * (e0 + e1), 8, 0)); }
        size_t worst = 0;
        for (int day = 0; day < 100; day++) {
            const uint32_t t = T0 + uint32_t(day) * 86400;
            st.setNowForTest(t);
            for (auto& x : sky) x.t = t;
            st.add(sky);
            st.flushCells();
            worst = std::max(worst, st.cellFileBytes());
        }
        const size_t cap = Store::kCellHeader + Store::kMaxCellRecs * Store::kCellRec;
        auto m = st.readCells(kOrbcomm, "aaaa1111");
        uint32_t oldest = UINT32_MAX; for (auto& kv : m) oldest = std::min(oldest, kv.second.n);
        CHECK(worst <= cap, "one radio, every cell every day for 100 days: ≤ %zu bytes (worst %zu)", cap, worst);
        CHECK(m.size() == 2292 && oldest >= 28, "…and its whole 30-day window is kept (every cell ≥ 28 of its 30 daily dots: %u)", oldest);
        // three radios
        for (int day = 100; day < 130; day++) {
            const uint32_t t = T0 + uint32_t(day) * 86400;
            st.setNowForTest(t);
            for (const char* r : { "aaaa1111", "bbbb2222", "cccc3333" }) { for (auto& x : sky) { x.t = t; x.radio = r; } st.add(sky); }
            st.flushCells();
            worst = std::max(worst, st.cellFileBytes());
        }
        auto m3 = st.readCells(kOrbcomm, "cccc3333");
        CHECK(worst <= cap, "three radios, the same: still ≤ %zu bytes (worst %zu, %.0f KB)", cap, worst, worst / 1024.0);
        CHECK(m3.size() == 2292 && m3.begin()->second.n >= 3, "…the newest buckets kept for each (%u dots a cell)", m3.begin()->second.n);
        std::printf("   one radio's whole sky for 30 days: %zu records; the cap %zu (%.0f KB)\n", size_t(2292 * 10), Store::kMaxCellRecs, cap / 1024.0);
        rmDir(d4);
    }

    std::printf("§5 AUTO\n");
    {
        cells::Map m;
        const int n = cells::total();
        for (int i = 0; i < int(0.29 * n); i++) for (int k = 0; k < 4; k++) cells::addDot(m[i], 9, T0);
        CHECK(!cells::heatmapDue(m), "29 %% of the sky heard: tracks");
        for (int i = int(0.29 * n); i < int(0.31 * n); i++) for (int k = 0; k < 4; k++) cells::addDot(m[i], 9, T0);
        CHECK(cells::heatmapDue(m), "31 %% heard, 4 dots a cell: the heatmap");
        cells::Map thin;
        for (int i = 0; i < int(0.5 * n); i++) cells::addDot(thin[i], 9, T0);
        CHECK(!cells::heatmapDue(thin), "half the sky but one dot a cell: still tracks");
        CHECK(cellsJson(m, 30).find("\"auto\":\"heatmap\"") != std::string::npos && cellsJson(thin, 30).find("\"auto\":\"tracks\"") != std::string::npos,
              "the JSON says which");
        const std::string j = cellsJson(m, 30);
        CHECK(j.find("\"elStep\":3,\"az\":[120,") == 0 + 15 || j.find("\"az\":[120,") != std::string::npos, "the grid travels: elStep + cells per band");
        CHECK(j.find("\"heatCoverage\":0.30,\"heatDotsPerCell\":3.0") != std::string::npos, "…and AUTO's rule");
    }

    std::printf("§6 the heatmap PNG\n");
    {
        cells::Map m;
        const int n = cells::total();
        for (int i = 0; i < n * 4 / 10; i++) for (int k = 0; k < 4; k++) cells::addDot(m[i], i % 2 ? 4 : 16, T0);   // 40 % of the sky, from the horizon up
        // an east cell at 30° el: 14 dB; a grey cell; one never heard (high in the north-west)
        const int east = cells::index(95, 31.5), grey = cells::index(180, 1.5), none = cells::index(315, 70);
        m[east] = cells::Agg{}; for (int k = 0; k < 5; k++) cells::addDot(m[east], 14, T0);
        m[grey] = cells::Agg{}; for (int k = 0; k < 5; k++) cells::addDot(m[grey], kNothing, T0);
        m.erase(none);
        std::vector<Dot> dots{ dot(10, 80, 18, T0) };
        const int W = 520, cx = W / 2, cy = 36 + (W - 72) / 2, R = (W - 72) / 2;
        auto px = [&](const vibe::canvas::Canvas& c, double az, double el, uint8_t* o) {
            const double rr = R * (90 - el) / 90, a = az * M_PI / 180;
            const int x = int(std::lround(cx + rr * std::sin(a))), y = int(std::lround(cy - rr * std::cos(a)));
            const uint8_t* p = &c.px[(size_t(y) * size_t(c.w) + size_t(x)) * 3]; o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
        };
        vibe::canvas::Canvas h = render(dots, "ORBCOMM SKY PLOT", W, &m, kAuto);
        uint8_t o[3], e[3];
        px(h, 95, 31.5, o); snrColour(14, e[0], e[1], e[2]);
        CHECK(o[0] == e[0] && o[1] == e[1] && o[2] == e[2], "AUTO (40 %% heard): the east cell in 14 dB's colour (%d,%d,%d)", o[0], o[1], o[2]);
        px(h, 180, 1.5, o);
        CHECK(o[0] == 110 && o[1] == 110 && o[2] == 110, "a tracked-but-nothing cell grey");
        px(h, 315, 70, o);
        CHECK(o[0] == 22 && o[1] == 28 && o[2] == 40, "an unheard cell is the dome");
        px(h, 10, 80, o); snrColour(18, e[0], e[1], e[2]);
        CHECK(!(o[0] == e[0] && o[1] == e[1] && o[2] == e[2]), "no track dots on the heatmap picture");
        vibe::canvas::Canvas t = render(dots, "ORBCOMM SKY PLOT", W, &m, kTracks);
        px(t, 95, 31.5, o);
        CHECK(o[0] == 22 && o[1] == 28 && o[2] == 40, "TRACKS: no cells");
        px(t, 10, 80, o);
        CHECK(o[0] == e[0] && o[1] == e[1] && o[2] == e[2], "TRACKS: the dot is drawn");
        const std::string line = heatLine(m, 30);
        CHECK(line.rfind("HEATMAP - 30 DAYS - ", 0) == 0 && line.find(" CELLS HEARD - STRONGEST ") != std::string::npos, "the title line: %s", line.c_str());
        const std::string png1 = png(dots, "ORBCOMM SKY PLOT", W, &m, kHeatmap);
        CHECK(png1.size() > 1000 && png1.compare(1, 3, "PNG") == 0, "a PNG (%zu bytes)", png1.size());
        if (const char* td = std::getenv("TMPDIR")) {
            if (std::FILE* f = std::fopen((std::string(td) + "/skyplot-heatmap-test.png").c_str(), "wb")) { std::fwrite(png1.data(), 1, png1.size(), f); std::fclose(f); }
        }
    }

    std::printf("§7 the owner's reset\n");
    {
        const std::string d7 = tmpDir();
        Store st; st.setDir(d7); st.setNowForTest(T0);
        st.add({ dot(10, 10, 5, T0, "aaaa1111"), dot(20, 20, 6, T0, "aaaa1111"), dot(30, 30, 7, T0, "bbbb2222") });
        st.flushCells();
        const size_t gone = st.resetRadio("aaaa1111");
        Store::Filter f; f.kind = kOrbcomm;
        const auto left = st.read(f);
        CHECK(gone == 2 && left.size() == 1 && left[0].radio == "bbbb2222", "radio aaaa1111's two dots gone, bbbb2222's kept (%zu removed, %zu left)", gone, left.size());
        CHECK(st.readCells(kOrbcomm, "aaaa1111").empty() || st.readCells(kOrbcomm, "aaaa1111").size() == 0, "its cells gone");
        CHECK(st.readCells(kOrbcomm, "bbbb2222").size() == 1, "the other radio's cells kept");
        rmDir(d7);
    }
    std::printf("test-skyplot-cells: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
