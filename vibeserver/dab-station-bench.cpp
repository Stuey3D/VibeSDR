// dab-station-bench.cpp — V12 DAB OWN STATION (docs/v12/DAB-OWN-STATION.md): what each extra station costs ON THIS BOX,
// standalone — the benchmark's dec_dab_station row without the rest of the benchmark, for a machine (a Pi 2) where a test
// build of the whole server is not wanted. The same method: a VIBEBU81 multiplex looped through a DabService IN REAL TIME,
// first alone, then with K stations on other services fed by its tap; every thread's CPU over the measured window
// (/proc/self/task), memory (RSS) before and after the stations opened, each station's super frames and dropped frames.
//
//   dab-station-bench <clip.vbu8> [K=3] [secs=20] [warm=6] [sid of the radio's own=the clip's]
// Prints one JSON line: {"cores":…,"alonePct":{thread:%},"withPct":{…},"perStationPct":…,"memKBPerStation":…,
//   "stations":[{"sid":…,"sfOk":…,"sfTried":…,"mp2Out":…,"dropped":…}],"n":N (vibe_dab_own.h nFromBench)}
#include "vibe_dab_service.h"
#include "vibe_dab_own.h"
#include "opus_audio_encoder.h"
#include "vibe_thread.h"
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
struct Clip { double rate = 0, centre = 0; uint32_t sid = 0; std::vector<float> iq; };
bool load(const char* path, Clip& c) {
    FILE* fp = std::fopen(path, "rb");
    if (!fp) return false;
    char magic[8] = {0};
    bool ok = std::fread(magic, 1, 8, fp) == 8 && std::memcmp(magic, "VIBEBU81", 8) == 0
           && std::fread(&c.rate, 8, 1, fp) == 1 && std::fread(&c.centre, 8, 1, fp) == 1 && std::fread(&c.sid, 4, 1, fp) == 1;
    std::vector<uint8_t> u; uint8_t b[65536]; size_t n;
    while (ok && (n = std::fread(b, 1, sizeof b, fp)) > 0) u.insert(u.end(), b, b + n);
    std::fclose(fp);
    c.iq.resize(u.size());
    for (size_t i = 0; i < u.size(); ++i) c.iq[i] = (float(u[i]) - 127.5f) / 127.5f;
    return ok && !c.iq.empty();
}
/** Every thread's CPU seconds and name, from /proc (Linux). */
std::map<long, std::pair<std::string, double>> threads() {
    std::map<long, std::pair<std::string, double>> out;
    DIR* d = opendir("/proc/self/task");
    if (!d) return out;
    const double tck = double(sysconf(_SC_CLK_TCK));
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        const long tid = std::atol(e->d_name);
        char p[96]; std::snprintf(p, sizeof p, "/proc/self/task/%ld/stat", tid);
        FILE* f = std::fopen(p, "r"); if (!f) continue;
        char buf[1024]; const size_t n = std::fread(buf, 1, sizeof buf - 1, f); std::fclose(f); buf[n] = 0;
        const char* a = std::strchr(buf, '('); const char* z = std::strrchr(buf, ')');
        if (!a || !z) continue;
        std::string name(a + 1, z);
        unsigned long ut = 0, st = 0;
        // fields after ')': state(3) … utime(14) stime(15)
        const char* q = z + 2; int field = 3;
        while (*q && field < 14) { if (*q == ' ') ++field; ++q; }
        std::sscanf(q, "%lu %lu", &ut, &st);
        out[tid] = { name, double(ut + st) / tck };
    }
    closedir(d);
    return out;
}
long rssKB() {
    long pages = 0, rss = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) { if (std::fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0; std::fclose(f); }
    return rss * (sysconf(_SC_PAGESIZE) / 1024);
}
double num(const std::string& j, const char* k) {
    const std::string key = std::string("\"") + k + "\":";
    const size_t p = j.find(key); return p == std::string::npos ? -1 : std::atof(j.c_str() + p + key.size());
}

struct Run { std::map<std::string, double> pct; double total = 0; long rss0 = 0, rss1 = 0; std::vector<std::string> st; std::vector<uint32_t> sids; std::vector<uint32_t> drops; };
Run run(const Clip& c, uint32_t sid, int K, double secs, double warm) {
    Run r;
    auto svc = std::make_unique<vibedab::DabService>();
    svc->setRfRate(c.rate); svc->setRfCentre(c.centre);
    for (int i = 0; i < int(vibedab::kBandIIICount); ++i)
        if (std::fabs(double(vibedab::kBandIII[i].centreHz) - c.centre) < 1000.0) { svc->setChannel(i); break; }
    svc->setService(sid);
    const auto before = threads();
    r.rss0 = rssKB();
    std::vector<std::shared_ptr<vibedab::DabService>> st;
    // ★ the stations: services after the radio's own in SId order (the bench clip: C002…, all DAB+)
    for (uint32_t s = 0xC001; s <= 0xC0FF && int(st.size()) < K; ++s) if (s != sid && (s <= 0xC00B || s == 0xC0FF)) { st.push_back(svc->openStation(s)); r.sids.push_back(s); }
    const size_t blk = size_t(c.rate / 20) * 2;
    size_t pos = 0; std::vector<float> pcm(48000);
    std::map<long, std::pair<std::string, double>> mid; bool measuring = false;
    /* ★★ EACH STATION'S ONE OPUS ENCODE (2026-11-17 — the Pi 2 live: ~12 % of an A7 core per stereo stream, left out
     *  before, so N read ~2× too high): a vibe-dab-enc thread per station, 20 ms of its PCM at the server's 96 kb/s
     *  stereo, as the server's per-station encoder does. VIBE_BENCH_NO_ENCODE=1 measures without it (the old figure). */
    std::atomic<bool> encRun{true};
    std::vector<std::thread> enc;
#ifdef VIBE_HAVE_OPUS
    if (!std::getenv("VIBE_BENCH_NO_ENCODE"))
        for (auto& s : st) if (s) enc.emplace_back([&encRun, sp = s] {
            vibeThreadName("vibe-dab-enc");
            vibe::OpusAudioEncoder opus; opus.setBitrate(96000);
            std::vector<float> f(960 * 2); std::vector<int16_t> q(960 * 2); std::vector<std::vector<uint8_t>> pk;
            auto next = std::chrono::steady_clock::now();
            while (encRun.load()) {
                next += std::chrono::milliseconds(20);
                std::this_thread::sleep_until(next);
                const size_t got = sp->takePcm(f.data(), 960);
                for (size_t i = 0; i < 960 * 2; ++i) { const float v = i < got * 2 ? std::max(-1.0f, std::min(1.0f, f[i])) : 0.0f; q[i] = int16_t(std::lround(v * 32767.0f)); }
                pk.clear(); opus.encode(q.data(), 960, 2, pk);
            }
        });
#endif
    const auto t0 = std::chrono::steady_clock::now(); double fed = 0;
    for (;;) {
        if (!measuring && fed >= warm) { mid = threads(); measuring = true; r.rss1 = rssKB(); }
        if (fed >= warm + secs) break;
        const size_t n = std::min(blk, c.iq.size() - pos);
        svc->feed(c.iq.data() + pos, n / 2);
        pos = (pos + n) % c.iq.size();
        fed += double(n / 2) / c.rate;
        while (svc->takePcm(pcm.data(), pcm.size() / 2) > 0) {}
        if (enc.empty()) for (auto& s : st) if (s) while (s->takePcm(pcm.data(), pcm.size() / 2) > 0) {}
        std::this_thread::sleep_until(t0 + std::chrono::microseconds((long long)(fed * 1e6)));
    }
    const auto after = threads();
    encRun.store(false);
    for (auto& t : enc) t.join();
    for (const auto& kv : after) {
        if (before.count(kv.first)) continue;   // ★ new threads only — the decoder's, not this feeder
        auto m = mid.find(kv.first);
        const double used = 100.0 * (kv.second.second - (m != mid.end() ? m->second.second : 0.0)) / secs;
        r.pct[kv.second.first] += used; r.total += used;
    }
    for (auto& s : st) { r.st.push_back(s ? s->json() : std::string()); r.drops.push_back(s ? s->stationDropped() : 0); }
    for (auto& s : st) if (s) svc->closeStation(s);
    st.clear(); svc.reset();
    return r;
}
std::string pctJson(const std::map<std::string, double>& m) {
    std::string j = "{"; bool f = true; char b[96];
    for (const auto& kv : m) { std::snprintf(b, sizeof b, "%s\"%s\":%.1f", f ? "" : ",", kv.first.c_str(), kv.second); j += b; f = false; }
    return j + "}";
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: dab-station-bench <clip.vbu8> [K] [secs] [warm] [sid hex]\n"); return 2; }
    Clip c; if (!load(argv[1], c)) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    const int K = argc > 2 ? std::atoi(argv[2]) : 3;
    const double secs = argc > 3 ? std::atof(argv[3]) : 20, warm = argc > 4 ? std::atof(argv[4]) : 6;
    const uint32_t sid = argc > 5 ? uint32_t(std::strtoul(argv[5], nullptr, 16)) : (c.sid ? c.sid : 0xC001);
    vibedab::dabScanLabels().store(0);   // ★ as the benchmark: the PAD label scan is its own row
    const Run alone = run(c, sid, 0, secs, warm);
    const Run with = run(c, sid, K, secs, warm);
    const long cores = sysconf(_SC_NPROCESSORS_ONLN);
    const double per = K > 0 ? std::max(0.0, with.total - alone.total) / K : 0;
    std::printf("{\"cores\":%ld,\"K\":%d,\"secs\":%.0f,\"aloneTotal\":%.1f,\"withTotal\":%.1f,\"perStationPct\":%.2f,\"memKBPerStation\":%ld,"
                "\"n\":%d,\"alonePct\":%s,\"withPct\":%s,\"stations\":[",
                cores, K, secs, alone.total, with.total, per, K > 0 ? (with.rss1 - with.rss0) / K : 0L,
                vibedab::own::nFromBench(per, alone.total, int(cores)), pctJson(alone.pct).c_str(), pctJson(with.pct).c_str());
    for (size_t k = 0; k < with.st.size(); ++k)
        std::printf("%s{\"sid\":\"%04X\",\"sfOk\":%.0f,\"sfTried\":%.0f,\"mp2Out\":%.0f,\"pcmPushed\":%.0f,\"dropped\":%u}", k ? "," : "",
                    with.sids[k], num(with.st[k], "sfOk"), num(with.st[k], "sfTried"), num(with.st[k], "mp2Out"), num(with.st[k], "pcmPushed"), with.drops[k]);
    std::printf("]}\n");
    return 0;
}
