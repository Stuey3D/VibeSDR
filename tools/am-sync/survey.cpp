// survey — list AM carriers in an iq192/iq96 WAV (stereo int16 I/Q): average 4096-bin spectrum over N seconds,
// report every line ≥ 20 dB over the local floor with its offset from the centre and level.
//   survey <iq.wav> <centreHz> [seconds]
#include "vibedsp.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace vibedsp;
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    FILE* f = std::fopen(argv[1], "rb"); if (!f) return 1;
    const double centre = std::atof(argv[2]), secs = argc > 3 ? std::atof(argv[3]) : 20.0;
    unsigned char h[44]; std::fread(h, 1, 44, f);
    const int rate = h[24] | h[25] << 8 | h[26] << 16 | h[27] << 24;
    const int N = 8192; ComplexFFT fft(N); std::vector<cf32> in(N), out(N); std::vector<double> acc(N, 0.0);
    std::vector<short> buf(2 * N); int frames = 0;
    while (frames * (double)N / rate < secs && std::fread(buf.data(), 2, 2 * N, f) == (size_t)(2 * N)) {
        for (int i = 0; i < N; ++i) in[i] = cf32(buf[2 * i] / 32768.f, buf[2 * i + 1] / 32768.f) * (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / N));
        fft.forward(in.data(), out.data());
        for (int i = 0; i < N; ++i) acc[i] += std::norm(out[i]);
        ++frames;
    }
    std::vector<double> db(N);
    for (int i = 0; i < N; ++i) db[i] = 10 * std::log10(acc[(i + N / 2) % N] / frames + 1e-30);   // shifted: index 0 = -fs/2
    const double bin = (double)rate / N;
    for (int i = 30; i < N - 30; ++i) {
        if (db[i] < db[i - 1] || db[i] < db[i + 1]) continue;
        std::vector<double> loc(db.begin() + i - 30, db.begin() + i + 31);
        std::nth_element(loc.begin(), loc.begin() + 10, loc.end());
        if (db[i] - loc[10] < 20) continue;
        const double off = (i - N / 2) * bin;
        if (std::fabs(off) > rate * 0.45) continue;
        std::printf("%10.1f kHz  off %+9.1f Hz  %6.1f dB  (+%.0f over floor)\n", (centre + off) / 1e3, off, db[i], db[i] - loc[10]);
    }
}
