// test-dab-scan-store.cpp — the last DAB band scan's on-disk store (vibe_dab_scan_store.h), on its own.
//   Round trip (labels with spaces, block order, quality), the bounds (64 services, 64-byte labels, 41 blocks, a file
//   too big to be ours), damage (wrong header, unknown and repeated blocks, newlines in a label, a bad SId), and the
//   atomic write (no .tmp left, a missing directory refused without throwing).
#include "vibe_dab_scan_store.h"

#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;
static void ok(bool c, const char* what) { std::printf("   %s %s\n", c ? "ok  " : "FAIL", what); c ? ++g_pass : ++g_fail; }

int main() {
    using namespace vibedabscan;
    std::printf("A. round trip\n");
    Saved s;
    s.at = 1789000000; s.secs = 97; s.cancelled = false; s.by = "listener";
    Hit h1; h1.idx = blockIndex("11D"); h1.ensemble = "D1 National"; h1.fibRate = 0.98f; h1.merDb = 14.2f; h1.complete = true;
    h1.services = {{0xC0DE, "Radio X 90s"}, {0xC0DF, "Absolute 80s"}};
    Hit h2; h2.idx = blockIndex("7D"); h2.ensemble = "NNDAB"; h2.fibRate = 0.91f; h2.merDb = 8.0f;
    h2.services = {{0xC1A1, "BBC Radio 4"}};
    s.hits = {h1, h2};   // ★ out of block order on purpose
    Saved r;
    ok(parse(serialise(s), r), "parses what it wrote");
    ok(r.at == s.at && r.secs == 97 && !r.cancelled && r.by == "listener", "the scan's own line survives");
    ok(r.hits.size() == 2 && r.hits[0].idx == blockIndex("7D") && r.hits[1].idx == blockIndex("11D"), "block order, as the band is walked");
    ok(r.hits[1].ensemble == "D1 National" && r.hits[1].complete && r.hits[1].services.size() == 2
       && r.hits[1].services[0].first == 0xC0DE && r.hits[1].services[0].second == "Radio X 90s", "labels with spaces, SIds, complete");
    ok(r.hits[1].fibRate > 0.975f && r.hits[1].fibRate < 0.985f && r.hits[1].merDb > 14.1f && r.hits[1].merDb < 14.3f, "quality figures");

    std::printf("B. bounds\n");
    Saved big;
    for (size_t i = 0; i < vibedab::kBandIIICount; ++i) {
        Hit h; h.idx = int(i); h.ensemble = std::string(200, 'E');
        for (uint32_t k = 1; k <= 200; ++k) h.services.push_back({k, std::string(300, 'S')});
        big.hits.push_back(h);
    }
    const std::string bt = serialise(big);
    ok(parse(bt, r) && r.hits.size() == vibedab::kBandIIICount, "41 blocks at most");
    ok(r.hits[0].services.size() == kMaxServices && r.hits[0].ensemble.size() == kMaxLabel && r.hits[0].services[0].second.size() == kMaxLabel,
       "64 services, 64-byte labels");
    ok(bt.size() < kMaxFileBytes, "the biggest list fits the read cap");
    ok(!parse(std::string("VIBE-DAB-SCAN 1\n") + std::string(kMaxFileBytes, 'x'), r), "a file past the cap is damage, not read");

    std::printf("C. damage\n");
    ok(!parse("VIBE-DAB-SCAN 2\nat 1 secs 1 cancelled 0 by x\n", r), "another version is not ours");
    ok(!parse("", r) && r.hits.empty(), "an empty file is no list");
    ok(parse("VIBE-DAB-SCAN 1\nH 99Z 1 0.9 9 Nowhere\nS 5 x\nH 11D 1 0.9 9 Real\nS 0 zero\nS abc bad\nS 77 ok\nH 11D 1 0.9 9 Again\nS 78 the repeat\'s\n", r)
       && r.hits.size() == 1 && r.hits[0].ensemble == "Real" && r.hits[0].services.size() == 1 && r.hits[0].services[0].first == 77,
       "unknown and repeated blocks skipped (and their services), SId 0 / not a number skipped");
    Saved nl; Hit hn; hn.idx = 0; hn.ensemble = "Two\nlines\r"; hn.services = {{1, "a\nH 5B 1 1 1 injected"}}; nl.hits = {hn};
    ok(parse(serialise(nl), r) && r.hits.size() == 1 && r.hits[0].ensemble == "Twolines" && r.hits[0].services.size() == 1,
       "a newline in a label cannot make a record");

    std::printf("D. the atomic write\n");
    char tmpl[] = "/tmp/vs-dabscan-XXXXXX";
    const char* d = mkdtemp(tmpl);
    ok(d != nullptr, "a scratch directory");
    if (d) {
        const std::string f = std::string(d) + "/dab-scan-x.txt";
        std::string err;
        ok(writeAtomic(f, serialise(s), &err), "written");
        ok(::access((f + ".tmp").c_str(), F_OK) != 0, "no .tmp left behind");
        ok(load(f, r) && r.hits.size() == 2, "read back");
        ok(writeAtomic(f, serialise(Saved{}), &err) && load(f, r) && r.hits.empty(), "a rewrite replaces it whole");
        ok(!writeAtomic(std::string(d) + "/no/such/dir/f.txt", "x", &err) && !err.empty(), "a missing directory is refused, in words");
        ok(!load(std::string(d) + "/absent.txt", r) && r.hits.empty(), "no file: no list");
        ::unlink(f.c_str());
        ::rmdir(d);
    }
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
