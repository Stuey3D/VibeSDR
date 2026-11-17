// dab-wide-bench.cpp — V12 DAB PHASE 2 (docs/v12/DAB-OWN-STATION.md §11): SEVERAL MULTIPLEXES FROM ONE WIDE CAPTURE, and every
// station on them at once — offline, from a recorded capture (the RSP1A's 8 MS/s 10C/10D/11A file), in real time.
//
// One DabService per multiplex, each fed the SAME wide stream and cutting its own block out with its DDC (vibe_dab_ddc.h —
// the small-N front end of §11.1); once each ensemble's list is read, every audio service on it is opened as a STATION on
// that multiplex (Phase 1's station mode: its own sub-channel decode and audio chain). Measured over the window after the
// warm-up: every thread's CPU by name (/proc), RSS, and per multiplex MER / FIB / MSC BER, per station super frames (or
// MP2 frames), dropped frames and audio delivered against real time (continuity).
//
//   dab-wide-bench <capture.cu8> <rate> <centreHz> <block,block,…> [secs=40] [warm=15] [stations=all|N per mux] [skip=0]
//     capture: unsigned 8-bit I/Q (rtl_tcp style, 127.5 = zero), no header. Streamed — never whole in memory.
// Prints one JSON line per multiplex, one per station, and a summary line.
#include "vibe_dab_service.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
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
        unsigned long ut = 0, st = 0; const char* q = z + 2; int field = 3;
        while (*q && field < 14) { if (*q == ' ') ++field; ++q; }
        std::sscanf(q, "%lu %lu", &ut, &st);
        out[tid] = { std::string(a + 1, z), double(ut + st) / tck };
    }
    closedir(d);
    return out;
}
long rssKB() {
    long pages = 0, rss = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) { if (std::fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0; std::fclose(f); }
    return rss * (sysconf(_SC_PAGESIZE) / 1024);
}
double num(const std::string& j, const char* k, double dflt = -1) {
    const std::string key = std::string("\"") + k + "\":";
    const size_t p = j.find(key); return p == std::string::npos ? dflt : std::atof(j.c_str() + p + key.size());
}
std::string str(const std::string& j, const char* k) {
    const std::string key = std::string("\"") + k + "\":\"";
    const size_t p = j.find(key); if (p == std::string::npos) return "";
    const size_t e = j.find('"', p + key.size()); return j.substr(p + key.size(), e - p - key.size());
}
/** The audio services the report lists (sid + codec), top-level "services" array. */
std::vector<std::pair<uint32_t, std::string>> services(const std::string& j) {
    std::vector<std::pair<uint32_t, std::string>> out;
    const size_t a = j.find("\"services\":[");
    if (a == std::string::npos) return out;
    const size_t b = j.find("]}", a);
    size_t p = a;
    while ((p = j.find("{\"sid\":", p)) != std::string::npos && p < b) {
        const uint32_t sid = uint32_t(std::atol(j.c_str() + p + 7));
        const size_t q = j.find("\"codec\":\"", p), next = j.find("{\"sid\":", p + 7);
        std::string codec;
        if (q != std::string::npos && (next == std::string::npos || q < next)) codec = j.substr(q + 9, j.find('"', q + 9) - q - 9);
        out.push_back({ sid, codec });
        p += 7;
    }
    return out;
}
struct Mux { std::string block; int idx = -1; std::unique_ptr<vibedab::DabService> svc; std::vector<std::shared_ptr<vibedab::DabService>> st; std::vector<uint32_t> sids;
             double merSum = 0; int merN = 0; uint64_t pcm0 = 0; };
}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: dab-wide-bench <capture.cu8> <rate> <centreHz> <block,block,…> [secs] [warm] [stations] [skip]\n"); return 2; }
    const double rate = std::atof(argv[2]), centre = std::atof(argv[3]);
    const double secs = argc > 5 ? std::atof(argv[5]) : 40, warm = argc > 6 ? std::atof(argv[6]) : 15;
    const std::string stArg = argc > 7 ? argv[7] : "all";
    const int perMux = stArg == "all" ? 1000 : std::atoi(stArg.c_str());
    const double skip = argc > 8 ? std::atof(argv[8]) : 0;
    FILE* fp = std::fopen(argv[1], "rb");
    if (!fp) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    if (skip > 0) std::fseek(fp, long(skip * rate) * 2, SEEK_SET);
    std::vector<Mux> mux;
    for (const char* p = argv[4]; *p; ) {
        const char* e = std::strchr(p, ','); const std::string b = e ? std::string(p, e) : std::string(p);
        Mux m; m.block = b;
        for (int i = 0; i < int(vibedab::kBandIIICount); ++i) if (b == vibedab::kBandIII[i].name) m.idx = i;
        if (m.idx < 0) { std::fprintf(stderr, "unknown block %s\n", b.c_str()); return 2; }
        m.svc = std::make_unique<vibedab::DabService>();
        m.svc->setRfRate(rate); m.svc->setRfCentre(centre); m.svc->setChannel(m.idx);
        mux.push_back(std::move(m));
        p = e ? e + 1 : p + std::strlen(p);
    }
    vibedab::dabScanLabels().store(0);
    const size_t blk = size_t(rate / 50);            // 20 ms of capture per feed
    std::vector<uint8_t> u(blk * 2); std::vector<float> f(blk * 2);
    const auto before = threads();
    const long rss0 = rssKB();
    std::map<long, std::pair<std::string, double>> mid;
    bool opened = false, measuring = false; long rssMid = 0;
    double fed = 0; const auto t0 = std::chrono::steady_clock::now();
    double nextPoll = 1.0;
    while (fed < warm + secs) {
        if (std::fread(u.data(), 1, u.size(), fp) != u.size()) { std::fseek(fp, long(skip * rate) * 2, SEEK_SET); continue; }   // loop the capture
        for (size_t i = 0; i < u.size(); ++i) f[i] = (float(u[i]) - 127.5f) / 127.5f;
        for (auto& m : mux) m.svc->feed(f.data(), blk);
        fed += double(blk) / rate;
        if (fed >= nextPoll) {
            nextPoll += 1.0;
            for (auto& m : mux) { std::string j; if (m.svc->jsonTry(j) && num(j, "mer") > 0) { m.merSum += num(j, "mer"); ++m.merN; } }
            // ★ every audio service of each multiplex as a station, once its list has been read (a few seconds in)
            if (!opened && fed >= warm * 0.6) {
                opened = true;
                for (auto& m : mux) {
                    const std::string j = m.svc->json();
                    const auto list = services(j);
                    if (list.empty()) continue;
                    const uint32_t own = list.front().first;   // ★ the multiplex's own decode plays the first service
                    m.svc->setService(own);
                    int n = 1;
                    for (const auto& s : list) {
                        if (n >= perMux) break;
                        if (s.first == own) continue;
                        auto st = m.svc->openStation(s.first);
                        if (st) { m.st.push_back(st); m.sids.push_back(s.first); ++n; }
                    }
                }
            }
        }
        if (opened && !measuring && fed >= warm) {
            measuring = true; mid = threads(); rssMid = rssKB();
            for (auto& m : mux) m.pcm0 = uint64_t(num(m.svc->json(), "pcmPushed", 0));
        }
        // drain every station's audio, as the audio clock would
        static std::vector<float> pcm(48000 * 2);
        for (auto& m : mux) { while (m.svc->takePcm(pcm.data(), 48000) > 0) {} for (auto& s : m.st) while (s->takePcm(pcm.data(), 48000) > 0) {} }
        std::this_thread::sleep_until(t0 + std::chrono::microseconds((long long)(fed * 1e6)));
    }
    const auto after = threads();
    std::map<std::string, double> byName; double total = 0;
    for (const auto& kv : after) {
        if (before.count(kv.first)) continue;
        auto m = mid.find(kv.first);
        const double used = 100.0 * (kv.second.second - (m != mid.end() ? m->second.second : 0.0)) / secs;
        byName[kv.second.first] += used; total += used;
    }
    int stations = 0, stationsOk = 0;
    for (auto& m : mux) {
        const std::string j = m.svc->json();
        std::printf("{\"mux\":\"%s\",\"label\":\"%s\",\"services\":%zu,\"stations\":%zu,\"merNow\":%.2f,\"merAvg\":%.2f,\"fibRate\":%.3f,\"mscBer\":%.4f,"
                    "\"locked\":%s,\"reacq\":%.0f,\"wideDropped\":%.0f,\"own\":\"%x\",\"sfOk\":%.0f,\"sfTried\":%.0f,\"mp2Out\":%.0f}\n",
                    m.block.c_str(), str(j, "label").c_str(), services(j).size(), m.st.size(), num(j, "mer"), m.merN ? m.merSum / m.merN : -1.0,
                    num(j, "fibRate"), num(j, "mscBer"), j.find("\"locked\":true") != std::string::npos ? "true" : "false",
                    num(j, "reacquires", 0), double(m.svc->wideDropped()), unsigned(num(j, "sid", 0)), num(j, "sfOk"), num(j, "sfTried"), num(j, "mp2Out"));
        for (size_t k = 0; k < m.st.size(); ++k) {
            const std::string sj = m.st[k]->json();
            const double sfOk = num(sj, "sfOk", 0), sfT = num(sj, "sfTried", 0), mp2o = num(sj, "mp2Out", 0), mp2b = num(sj, "mp2Bad", 0);
            const bool ok = (sfT > 0 && sfOk >= 0.9 * sfT) || (mp2o > 0 && mp2b < 0.1 * mp2o);
            ++stations; if (ok) ++stationsOk;
            std::printf("{\"mux\":\"%s\",\"sid\":\"%04X\",\"codec\":\"%s\",\"sfOk\":%.0f,\"sfTried\":%.0f,\"mp2Out\":%.0f,\"mp2Bad\":%.0f,\"pcmPushed\":%.0f,"
                        "\"dropped\":%u,\"frames\":%u,\"ok\":%s}\n",
                        m.block.c_str(), m.sids[k], str(sj, "codec").c_str(), sfOk, sfT, mp2o, mp2b, num(sj, "pcmPushed", 0),
                        m.st[k]->stationDropped(), m.st[k]->stationFrames(), ok ? "true" : "false");
        }
    }
    std::printf("{\"summary\":true,\"rate\":%.0f,\"secs\":%.0f,\"muxes\":%zu,\"stations\":%d,\"stationsDecoding\":%d,\"cores\":%ld,\"totalPct\":%.1f,"
                "\"rssKB\":%ld,\"rssStartKB\":%ld,\"rssMidKB\":%ld,\"threads\":{", rate, secs, mux.size(), stations, stationsOk,
                sysconf(_SC_NPROCESSORS_ONLN), total, rssKB(), rss0, rssMid);
    bool first = true;
    for (const auto& kv : byName) { std::printf("%s\"%s\":%.1f", first ? "" : ",", kv.first.c_str(), kv.second); first = false; }
    std::printf("}}\n");
    for (auto& m : mux) { for (auto& s : m.st) m.svc->closeStation(s); m.st.clear(); m.svc->stopWorker(); }
    std::fclose(fp);
    return 0;
}
