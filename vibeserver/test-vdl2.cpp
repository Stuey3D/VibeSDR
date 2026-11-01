// test-vdl2.cpp — V12 VDL Mode 2 (vibe_vdl2_fec.h, vibe_vdl2.h), docs/v12/AVIATION.md §VDL2.
//  `test-vdl2 --cu8 <file.u8> <rate> <hwCentreHz> [chanHz…]` decodes a REAL capture (VDL2_DEBUG=1: every failure).
#include "vibe_vdl2.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
using vibevdl2::cf32;

static void printFrame(const vibevdl2::Frame& f) {
    std::printf("%8.3f %.3f lvl %5.1f snr %4.1f ferr %+5.0f Hz rs %d hdr %d er %d | %06X %-8s -> %06X %-8s %c%s %s", f.t, f.hz / 1e6,
                f.levelDb, f.snrDb, f.freqErrHz, f.rsFixed, f.hdrFixed, f.erased, f.src.addr, vibevdl2::addrKind(f.src.type),
                f.dst.addr, vibevdl2::addrKind(f.dst.type), f.kind, f.uName.empty() ? "" : (" " + f.uName).c_str(), f.airborne ? "" : "(on ground)");
    if (f.hasAcars) {
        std::string t = f.acars.text; for (auto& c : t) if (c == '\r' || c == '\n') c = ' ';
        std::printf(" | ACARS%s %s lbl %s flt %s | %s", f.acarsCrcOk ? "" : " (BCS bad)", f.acars.reg.c_str(), f.acars.label.c_str(), f.acars.flight.c_str(), t.c_str());
    } else std::printf(" | %zu bytes", f.bytes.size());
    std::printf("\n");
}

static int cu8(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: test-vdl2 --cu8 file rate hwCentreHz [chanHz...]\n"); return 2; }
    const double rate = std::atof(argv[3]), hw = std::atof(argv[4]);
    std::vector<double> ch;
    for (int i = 5; i < argc; ++i) ch.push_back(std::atof(argv[i]));
    if (ch.empty()) ch = vibevdl2::defaultChannels();
    FILE* f = std::fopen(argv[2], "rb");
    if (!f) { std::perror(argv[2]); return 2; }
    vibevdl2::Receiver rx(rate, hw, ch);
    std::vector<uint8_t> buf(1 << 18);
    std::vector<cf32> z(1 << 17);
    size_t samples = 0; int n = 0, acars = 0;
    std::map<uint32_t, int> aircraft;
    const auto t0 = std::chrono::steady_clock::now();
    const bool quiet = std::getenv("VDL2_QUIET") != nullptr;
    while (true) {
        const size_t r = std::fread(buf.data(), 2, 1 << 17, f);
        if (!r) break;
        vibeavi::u8ToCf(buf.data(), r, z.data());
        samples += r;
        rx.feed(z.data(), r, [&](const vibevdl2::Frame& fr) {
            ++n; acars += fr.hasAcars;
            if (fr.src.type == 1) aircraft[fr.src.addr]++;
            if (fr.dst.type == 1) aircraft[fr.dst.addr]++;
            if (!quiet) printFrame(fr);
        });
    }
    std::fclose(f);
    const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    for (size_t c = 0; c < rx.channels(); ++c) {
        const auto& s = rx.demod(c).stats();
        if (!s.syncs) continue;
        std::printf("ch %.3f: syncs %llu, header bad %llu, bursts %llu, RS fail %llu, frames %llu (FCS bad %llu), RS bytes fixed %llu, erasure saves %llu\n",
                    ch[c] / 1e6, (unsigned long long)s.syncs, (unsigned long long)s.hdrBad, (unsigned long long)s.bursts,
                    (unsigned long long)s.rsFail, (unsigned long long)s.frames, (unsigned long long)s.fcsBad,
                    (unsigned long long)s.rsFixed, (unsigned long long)s.erasureSaves);
    }
    std::printf("frames %d (ACARS %d), aircraft %zu, in %.0f s of capture; %.2f s here = %.2f %% of a core\n", n, acars, aircraft.size(),
                samples / rate, el, 100.0 * el / (samples / rate));
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "--cu8")) return cu8(argc, argv);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
