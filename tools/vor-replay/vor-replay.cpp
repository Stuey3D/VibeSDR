// vor-replay: a cu8 (rtl_tcp 8-bit IQ, header dropped) through decoders/nav_rx.h's NavRx, one line a second.
// build: g++ -std=c++17 -O2 -I android/app/src/main/cpp -o /tmp/vor-replay tools/vor-replay/vor-replay.cpp   (from the repo root)
#include "decoders/nav_rx.h"
#include <cstdio>
#include <vector>
int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: vor-replay FILE.cu8 RATE\n"); return 2; }
    FILE* f = std::fopen(argv[1], "rb"); if (!f) return 2;
    const double fs = std::atof(argv[2]);
    vibe::nav::NavRx rx(fs);
    int sec = 0;
    rx.onSecond = [&](const vibe::nav::Report& r) {
        ++sec;
        const auto& s = r.last;
        std::printf("%3d s  kind %d  bearing %6.1f sigma %5.1f run %d | carrier %.2f var30 %.3f sub %.3f dev %.0f refCoh %.2f sec-bearing %6.1f vor %d ils %d\n",
                    sec, (int)r.kind, r.bearing, r.sigma, r.seconds, s.carrier, s.var30, s.sub, s.dev, s.refCoh, s.bearing, (int)s.vor, (int)s.ils);
    };
    rx.keyer.onWord = [&](const std::string& w, double wpm) { std::printf("        IDENT \"%s\" (%.1f wpm)\n", w.c_str(), wpm); };
    std::vector<unsigned char> b(2 * 4800); std::vector<std::complex<float>> z(4800);
    size_t n;
    while ((n = std::fread(b.data(), 1, b.size(), f)) >= 2) {
        int m = (int)(n / 2);
        for (int i = 0; i < m; ++i) z[(size_t)i] = { (b[2 * i] - 127.5f) / 128.f, (b[2 * i + 1] - 127.5f) / 128.f };
        rx.feed(z.data(), m);
    }
    return 0;
}
