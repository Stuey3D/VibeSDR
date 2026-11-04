// test-dab-tii-air.cpp — TII off the air: real captures through the real DabService, with the UK directory.
//
// ★★★ WHY (2026-11-04). 11D (D1 National) and 10D (Beds/Bucks/Herts) named no transmitter at Stuart's,
//     though both carry TII: their sites radiate each pair's second carrier with its own PRS phase, and
//     the noise reference was the TII itself (vibe_dab_tii.h, the top). This holds the fix to the air:
//       · 11D: Daventry 23/2 and Bow Brickhill 22/9;  10D: Bow Brickhill 22/9 and Sandy Heath 22/3 —
//         each in nearly every verdict, named from the Ofcom record, and NOTHING else named;
//       · 12B (BBC National, where TII always worked): Daventry 23/2 and Northampton 23/15 as before (and,
//         now, a weak Bow Brickhill 22/9 in some verdicts — allowed, not required).
//     Captures: ~/VibeSDR-test-audio/dab/tii (README there) and dab/12B-bbc-2026-10-26.vbu8 — other people's
//     broadcasts, so never in git. Exit 3 = NOT RUN when they are absent.
#include "vibe_dab_service.h"
#include "vibe_dab_channels.h"
#include "vibe_dab_txdb_e1.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace vibedab;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); ++fails; } else printf("  ok: %s\n", msg); } while (0)

struct Run { int verdicts = 0; std::map<std::string, int> seen; };   // "main/sub site" -> verdicts named in

static bool replay(const std::string& path, double secs, const DabTxDb& db, Run& out) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    char magic[8]; double rate = 0, centre = 0; uint32_t sid = 0;
    if (std::fread(magic, 1, 8, fp) != 8 || std::memcmp(magic, "VIBEBU81", 8) || std::fread(&rate, 8, 1, fp) != 1
        || std::fread(&centre, 8, 1, fp) != 1 || std::fread(&sid, 4, 1, fp) != 1) { std::fclose(fp); return false; }
    DabService svc;
    svc.setRfRate(rate); svc.setRfCentre(centre);
    for (int i = 0; i < int(kBandIIICount); ++i) if (std::fabs(double(kBandIII[i].centreHz) - centre) < 1000.0) svc.setChannel(i);
    svc.setTxDb(&db); svc.setReceiverPosition(52.29, -0.85);   // Stuart's, where the captures were taken
    const size_t blk = size_t(rate / 20);
    std::vector<uint8_t> u(2 * blk); std::vector<float> f(2 * blk);
    std::string lastDiag; double fed = 0, nextPoll = 0;
    while (fed < secs && std::fread(u.data(), 1, u.size(), fp) == u.size()) {
        for (size_t i = 0; i < u.size(); ++i) f[i] = (float(u[i]) - 127.5f) / 127.5f;
        svc.feed(f.data(), blk);
        fed += 0.05;
        for (int w = 0; w < 400000 && svc.backlogForTest() > 0; ++w) std::this_thread::sleep_for(std::chrono::microseconds(100));
        if (fed < nextPoll) continue;
        nextPoll = fed + 0.1;
        std::string j; if (!svc.jsonTry(j)) continue;
        const size_t d = j.find("\"tiiDiag\":"), t = j.find("\"tii\":[");
        if (d == std::string::npos || t == std::string::npos) continue;
        const std::string diag = j.substr(d, j.find('}', d) - d);
        if (diag == lastDiag || diag.find("\"frames\":0") != std::string::npos) continue;
        lastDiag = diag; ++out.verdicts;
        // each hit: "main":M,"sub":S … "site":"X"
        const size_t end = j.find(']', t);
        for (size_t p = j.find("{\"main\":", t); p != std::string::npos && p < end; p = j.find("{\"main\":", p + 1)) {
            const int m = std::atoi(j.c_str() + p + 8);
            const size_t sp = j.find("\"sub\":", p); const int s = std::atoi(j.c_str() + sp + 6);
            const size_t close = j.find('}', p), ss = j.find("\"site\":\"", p);
            std::string site = "?";
            if (ss != std::string::npos && ss < close) { const size_t a = ss + 8; site = j.substr(a, j.find('"', a) - a); }
            out.seen[std::to_string(m) + "/" + std::to_string(s) + " " + site]++;
        }
    }
    svc.stopWorker();
    std::fclose(fp);
    return true;
}

int main() {
    printf("test-dab-tii-air\n");
    const char* home = std::getenv("HOME");
    const std::string dir = std::string(home ? home : "") + "/VibeSDR-test-audio/dab/";
    DabTxDb db; db.addBuiltin(0xE1, kDabTx_E1, kDabTx_E1_n);
    struct Case { const char* file; std::vector<std::string> want, allow; };   // allow: real but weak, comes and goes
    const Case cases[] = {
        { "tii/11D-v4l48004-2026-11-04.vbu8", { "23/2 Daventry", "22/9 Bow Brickhill" } },
        { "tii/10D-v4l48004-2026-11-04.vbu8", { "22/9 Bow Brickhill", "22/3 Sandy Heath" } },
        { "12B-bbc-2026-10-26.vbu8",          { "23/2 Daventry", "23/15 Northampton" }, { "22/9 Bow Brickhill" } },
    };
    int ran = 0;
    for (const auto& c : cases) {
        Run r;
        if (!replay(dir + c.file, 12.0, db, r)) { printf("  (no %s — skipped)\n", c.file); continue; }
        ++ran;
        printf("── %s: %d verdicts\n", c.file, r.verdicts);
        for (const auto& kv : r.seen) printf("     %-28s in %d\n", kv.first.c_str(), kv.second);
        CHECK(r.verdicts >= 10, "a verdict about every 0.8 s");
        for (const auto& w : c.want) {
            const auto it = r.seen.find(w);
            const std::string msg = w + " named in at least 80 % of verdicts";
            CHECK(it != r.seen.end() && it->second * 5 >= r.verdicts * 4, msg.c_str());
        }
        // nothing else NAMED steadily: a site in more than a fifth of the verdicts must be one we expect
        for (const auto& kv : r.seen) {
            bool expected = false;
            for (const auto& w : c.want) if (kv.first == w) expected = true;
            for (const auto& w : c.allow) if (kv.first == w) expected = true;
            if (!expected && kv.second * 5 > r.verdicts) { const std::string msg = "no steady unexpected site: " + kv.first; CHECK(false, msg.c_str()); }
        }
    }
    if (!ran) { printf("  NOT RUN — no captures in %s\n", dir.c_str()); return 3; }
    printf(fails ? "  %d FAILED\n" : "  all passed\n", fails);
    return fails ? 1 : 0;
}
