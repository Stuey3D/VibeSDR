// bench-gallery.cpp — what the gallery's store costs at N items (vibe_gallery.h, 2026-11-01). Not a test: a measurement.
//   c++ -std=c++17 -O2 -I android/app/src/main/cpp vibeserver/bench-gallery.cpp -o /tmp/bench-gallery -lpthread
//   /tmp/bench-gallery 50000            (one N per run, so the peak RSS is that N's alone)
// Writes N small NAVTEX-shaped items through Store::add (as the decoders do), then, as a FRESH process would:
// the first list (the whole index read into the table), a cached page, a deep filtered page, the steady state when
// full (each add evicts one), and a mass eviction with its compaction. RSS = peak resident (getrusage).
#include "vibe_gallery.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>

using namespace vibe;
using clk = std::chrono::steady_clock;
static double ms(clk::time_point a) { return std::chrono::duration<double, std::milli>(clk::now() - a).count(); }
static double rssMB() {
    rusage u{}; getrusage(RUSAGE_SELF, &u);
#if defined(__APPLE__)
    return u.ru_maxrss / 1048576.0;
#else
    return u.ru_maxrss / 1024.0;
#endif
}
static uint64_t fileSize(const std::string& p) { struct stat st{}; return ::stat(p.c_str(), &st) == 0 ? (uint64_t)st.st_size : 0; }

int main(int argc, char** argv) {
    const size_t N = argc > 1 ? (size_t)std::atol(argv[1]) : 5000;
    char t[256]; std::snprintf(t, sizeof t, "%s/bench-gallery-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp");
    const std::string dir = mkdtemp(t);
    const double rss0 = rssMB();
    const std::string body(200, 'N');                              // ★ the item's size does not change the index's cost
    {
        gallery::Store w; w.setDir(dir); w.setLimitBytes(~0ull >> 1);
        w.setRadioLabel("SDRplay RSP1A");
        gallery::Meta m; m.origin = "background"; m.freqHz = 518000; m.mode = "usb"; m.decoder = "navtex"; m.profile = "Profile 1";
        const auto a = clk::now();
        for (size_t i = 0; i < N; i++) {
            m.kind = i % 50 == 0 ? "wefax" : "navtex";
            m.utc = 1792700000 + (int64_t)i; m.title = "Station G \xc2\xb7 Met warning \xc2\xb7 #" + std::to_string(i % 100) + " \xc2\xb7 no errors";
            w.add(m, "txt", body);
        }
        const double addMs = ms(a);
        std::printf("N=%zu  write: %.1f s total, %.0f us an add (append + table tail + size check)\n", N, addMs / 1000, addMs * 1000 / N);
    }
    const uint64_t idx = fileSize(dir + "/index.jsonl");
    std::printf("  index on disk: %.1f MB (%.0f bytes an item)\n", idx / 1048576.0, (double)idx / N);
    const double rssW = rssMB();
    gallery::Store s; s.setDir(dir); s.setLimitBytes(~0ull >> 1);   // ★ a fresh process: nothing cached
    auto a = clk::now();
    std::string j = s.listJson("", 25, 0);
    const double first = ms(a);
    a = clk::now(); for (int k = 0; k < 20; k++) j = s.listJson("", 25, 0); const double page = ms(a) / 20;
    a = clk::now(); for (int k = 0; k < 20; k++) j = s.listJson("wefax", 25, (long)(N / 50 / 2)); const double deep = ms(a) / 20;
    std::printf("  list: first (whole index read) %.1f ms · page 1 %.2f ms · a deep WEFAX page %.2f ms · reply %zu bytes\n", first, page, deep, j.size());
    // ★ full: each add evicts the oldest (one {"x":…} line); averaged over 1000 adds
    const uint64_t used = s.usedBytes();
    s.setLimitBytes(used);
    gallery::Meta m; m.kind = "navtex"; m.origin = "background"; m.freqHz = 518000; m.mode = "usb"; m.decoder = "navtex"; m.title = "x";
    a = clk::now(); for (int k = 0; k < 1000; k++) { m.utc = 1800000000 + k; s.add(m, "txt", body); }
    const double steady = ms(a) / 1000;
    // ★ the owner lowers the size by 30%: one add evicts 30% and compacts the index
    s.setLimitBytes(used * 7 / 10);
    const unsigned long c0 = s.compactions();
    a = clk::now(); s.add(m, "txt", body); const double mass = ms(a);
    std::printf("  evict: full, an add that evicts one %.2f ms · the size cut 30%% (%zu items left, %lu compaction) %.0f ms\n",
                steady, s.itemCount(), s.compactions() - c0, mass);
    std::printf("  RSS peak: %.1f MB at start, %.1f MB after writing, %.1f MB after listing + evicting\n", rss0, rssW, rssMB());
    std::string rm = "rm -rf '" + dir + "'"; if (std::system(rm.c_str())) {}
    return 0;
}
