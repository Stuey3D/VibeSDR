// test-hfdl.cpp — V12 HFDL (vibe_hfdl.h), docs/v12/AVIATION.md §HFDL.
//  `test-hfdl --wav <file.wav> <centreHz> [dialHz…]` decodes a REAL 16-bit stereo IQ WAV (UberSDR iq192); with
//  `--ref <file.ref.txt>` it matches what it decoded against the reference transcript (HFDL_DEBUG=1: every burst).
#include "vibe_hfdl.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
using vibehfdl::cf32;

static void printMsg(const vibehfdl::Message& m) {
    std::printf("%8.2f %.0f kHz %4d bps %c snr %4.1f f %+5.1f | %s %s%d -> %s%d %s", m.t, m.hz / 1e3, vibehfdl::bitRate(m.mode), m.mode < 4 ? 'S' : 'D', m.snrDb, m.freqErrHz,
                m.downlink ? "DN" : "UP", m.downlink ? "AC" : "GS", m.srcId, m.downlink ? "GS" : "AC", m.dstId, m.typeName.c_str());
    if (m.icao) std::printf(" icao %06X", m.icao);
    if (!m.flight.empty()) std::printf(" flt %s", m.flight.c_str());
    if (!std::isnan(m.lat)) std::printf(" pos %.4f %.4f", m.lat, m.lon);
    if (m.hfnpdu >= 0) std::printf(" hfnpdu %02X", m.hfnpdu);
    if (m.hasAcars) { std::string t = m.acars.text; for (auto& c : t) if (c == '\r' || c == '\n') c = ' '; std::printf(" | ACARS%s %s lbl %s | %s", m.acarsCrcOk ? "" : "(BCS bad)", m.acars.reg.c_str(), m.acars.label.c_str(), t.c_str()); }
    std::printf("\n");
}

struct RefMsg { double time; int freqKhz; int rate; std::string flight, reg, label, type, text; int src; bool matched = false; };
static std::string jstr(const std::string& l, const char* key) {
    const std::string k = std::string("\"") + key + "\":";
    size_t p = l.find(k); if (p == std::string::npos) return "";
    p += k.size();
    if (l[p] == '"') { size_t e = l.find('"', p + 1); return l.substr(p + 1, e - p - 1); }
    size_t e = l.find_first_of(",}", p); return l.substr(p, e - p);
}

static int wav(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: test-hfdl --wav file centreHz [dialHz...] [--ref file]\n"); return 2; }
    const double centre = std::atof(argv[3]);
    std::vector<double> dial; std::string refPath;
    for (int i = 4; i < argc; ++i) { if (!std::strcmp(argv[i], "--ref") && i + 1 < argc) refPath = argv[++i]; else dial.push_back(std::atof(argv[i])); }
    std::vector<RefMsg> ref; double refStart = 0;
    if (!refPath.empty()) {
        FILE* rf = std::fopen(refPath.c_str(), "r");
        char line[8192];
        while (rf && std::fgets(line, sizeof line, rf)) {
            std::string l(line);
            if (l.find("\"start\"") != std::string::npos) {
                const std::string s = jstr(l, "start");   // 2026-10-31T21:57:57.451Z
                int Y, Mo, D, h, mi; double sec;
                if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%lfZ", &Y, &Mo, &D, &h, &mi, &sec) == 6) {
                    struct tm tmv{}; tmv.tm_year = Y - 1900; tmv.tm_mon = Mo - 1; tmv.tm_mday = D; tmv.tm_hour = h; tmv.tm_min = mi; tmv.tm_sec = 0;
                    refStart = double(timegm(&tmv)) + sec;
                }
            }
            if (l.find("\"type\":\"message\"") == std::string::npos) continue;
            RefMsg m;
            m.time = std::atof(jstr(l, "time").c_str()); m.freqKhz = std::atoi(jstr(l, "freq_khz").c_str()); m.rate = std::atoi(jstr(l, "bit_rate").c_str());
            m.flight = jstr(l, "flight"); m.reg = jstr(l, "reg"); m.label = jstr(l, "label"); m.type = jstr(l, "msg_type"); m.text = jstr(l, "msg_text");
            m.src = std::atoi(jstr(l, "src_id").c_str());
            ref.push_back(m);
        }
        if (rf) std::fclose(rf);
        if (dial.empty()) { std::set<int> f; for (auto& m : ref) f.insert(m.freqKhz); for (int k : f) dial.push_back(k * 1000.0); }
    }
    FILE* f = std::fopen(argv[2], "rb");
    if (!f) { std::perror(argv[2]); return 2; }
    std::fseek(f, 44, SEEK_SET);   // the canonical 44-byte header (iqrec writes plain PCM)
    const double rate = 192000;
    vibehfdl::Receiver rx(rate, centre, dial);
    std::vector<int16_t> buf(2 * 65536);
    std::vector<cf32> z(65536);
    size_t samples = 0; int n = 0;
    std::vector<vibehfdl::Message> got;
    const auto t0 = std::chrono::steady_clock::now();
    while (true) {
        const size_t r = std::fread(buf.data(), 4, 65536, f);
        if (!r) break;
        for (size_t i = 0; i < r; ++i) z[i] = cf32(buf[2 * i] / 32768.f, buf[2 * i + 1] / 32768.f);
        samples += r;
        rx.feed(z.data(), r, [&](const vibehfdl::Message& m) { ++n; got.push_back(m); printMsg(m); });
    }
    std::fclose(f);
    const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    for (size_t c = 0; c < rx.channels(); ++c) {
        const auto& s = rx.demod(c).stats();
        std::printf("%.0f kHz: detections %llu, no M1 %llu, bursts %llu, CRC-bad bursts %llu, messages %llu\n", dial[c] / 1e3, (unsigned long long)s.detections,
                    (unsigned long long)s.noM1, (unsigned long long)s.bursts, (unsigned long long)s.crcBad, (unsigned long long)s.messages);
    }
    if (!ref.empty()) {
        // a reference message is matched by frequency, time (±4 s) and its flight / registration / type
        int matched = 0;
        for (auto& r : ref) {
            for (auto& m : got) {
                const double t = refStart + m.t;
                if (std::lround(m.hz / 1e3) != r.freqKhz || std::fabs(t - r.time) > 6) continue;
                const bool same = (!r.flight.empty() && r.flight == m.flight) || (!r.reg.empty() && m.hasAcars && r.reg == m.acars.reg) || r.type == m.typeName;
                if (same && m.downlink && m.srcId == r.src) { r.matched = true; break; }
            }
            matched += r.matched;
            if (!r.matched) std::printf("   reference not decoded: %.0f %d kHz %d bps AC%d %s %s %s\n", r.time - refStart, r.freqKhz, r.rate, r.src, r.type.c_str(), r.flight.c_str(), r.reg.c_str());
        }
        std::printf("REFERENCE: %d of %zu messages decoded (ours: %d messages, %d downlink)\n", matched, ref.size(), n,
                    int(std::count_if(got.begin(), got.end(), [](const vibehfdl::Message& m) { return m.downlink; })));
    }
    std::printf("messages %d in %.0f s; %.2f s here = %.2f %% of a core (%zu channels)\n", n, samples / rate, el, 100 * el / (samples / rate), dial.size());
    return 0;
}

int main(int argc, char** argv) {
    if (const char* t = std::getenv("HFDL_TUNE")) {
        auto& T = vibehfdl::tuning(); std::string s(t);
        auto vf = [&](const char* k, float& v) { const size_t p = s.find(k); if (p != std::string::npos) v = float(std::atof(s.c_str() + p + std::strlen(k))); };
        vf("coh=", T.minCoh); vf("ts=", T.tSpan); vf("det=", T.detQ); vf("mu=", T.mu); vf("pa=", T.pllA); vf("pb=", T.pllB);
        { const size_t q = s.find("taps="); if (q != std::string::npos) T.eqTaps = std::atoi(s.c_str() + q + 5); }
    }
    if (argc > 1 && !std::strcmp(argv[1], "--wav")) return wav(argc, argv);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
