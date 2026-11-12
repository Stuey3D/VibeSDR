// drm-synth-clip.cpp — a SYNTHETIC DRM replay clip for fake-rtl-tcp.mjs (--hf), from the test transmitter (drm_tx.h):
// the same file format scripts/drm-clip.py makes from a recording, so the server and web DRM tests can run with no
// off-air recording on the machine (scripts/test-web-drm-constellation.mjs; docs/v12/DRM.md "The constellation").
//
//   drm-synth-clip <out.cu8> <seconds> [snr dB = 24] [msc 16|64 = 16] [mode A|B|D = B]
//
// The transmitter's 24 kHz baseband goes through annex B-style AWGN (drm_tx.h Channel, SNR in the signal's bandwidth),
// is interpolated ×12 to 288 kS/s by a windowed-sinc polyphase filter (no images inside the capture), moved 30 kHz above
// the clip's centre (off the dongle's DC, as drm-clip.py does) and quantised to unsigned 8-bit I/Q as rtl_tcp sends it.
// The label is "VIBE TEST"; the audio is the transmitter's counter pattern declared as xHE-AAC, so nothing plays — the
// constellation, the lock and the SNR are what a test reads.
//   g++ -std=c++17 -O2 -I android/app/src/main/cpp -I android/app/src/main/cpp/vibedsp vibeserver/drm-synth-clip.cpp -o drm-synth-clip
#include "decoders/drm/drm_tx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace vibedrm;

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s out.cu8 seconds [snr] [16|64] [A|B|D]\n", argv[0]); return 2; }
    const double secs = std::atof(argv[2]);
    const double snr = argc > 3 ? std::atof(argv[3]) : 24.0;
    const int qam = argc > 4 ? std::atoi(argv[4]) : 16;
    const char ml = argc > 5 ? argv[5][0] : 'B';
    TxConfig c;
    c.mode = ml == 'A' ? Mode::A : ml == 'D' ? Mode::D : Mode::B;
    c.mscLevels = qam == 64 ? 3 : 2;
    c.sdcLevels = 1;
    Transmitter tx(c);
    int kmin, kmax; carrierRange(c.mode, c.so, kmin, kmax);
    Channel ch(std::vector<PathSpec>{ {0, 1, 0, 0} }, snr, 0.0, 0.0, (kmax - kmin + 1) * double(kFs) / modeInfo(c.mode).tu, 5);
    std::vector<cf> bb;
    while (double(bb.size()) < secs * kFs) { const auto y = ch.run(tx.superFrame()); bb.insert(bb.end(), y.begin(), y.end()); }

    // ×12 polyphase interpolation: a Blackman-windowed sinc at the input's Nyquist (12 kHz), 24 taps per phase
    const int R = 12, T = 24, N = R * T;
    std::vector<float> h(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) {
        const double t = (i - (N - 1) / 2.0) / R;
        const double s = t == 0 ? 1.0 : std::sin(M_PI * t) / (M_PI * t);
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (N - 1)) + 0.08 * std::cos(4 * M_PI * i / (N - 1));
        h[size_t(i)] = float(s * w);
    }
    std::vector<cf> up(bb.size() * size_t(R));
    for (size_t n = 0; n < bb.size(); ++n)
        for (int p = 0; p < R; ++p) {
            cf acc(0, 0);
            for (int k = 0; k < T; ++k) {
                const long j = long(n) - k;
                if (j < 0) break;
                acc += bb[size_t(j)] * h[size_t(k * R + p)];
            }
            up[n * size_t(R) + size_t(p)] = acc;
        }
    // +30 kHz, then the level: the 99.9th percentile of |y| at 90 counts (drm-clip.py's)
    const double w0 = 2 * M_PI * 30000.0 / 288000.0;
    std::vector<float> mag(up.size());
    for (size_t i = 0; i < up.size(); ++i) { up[i] *= cf(float(std::cos(w0 * double(i))), float(std::sin(w0 * double(i)))); mag[i] = std::abs(up[i]); }
    std::vector<float> m2 = mag;
    const size_t q = size_t(double(m2.size()) * 0.999);
    std::nth_element(m2.begin(), m2.begin() + long(q), m2.end());
    const float g = m2[q] > 0 ? 90.0f / m2[q] : 1.0f;
    std::vector<uint8_t> out(up.size() * 2);
    auto u8 = [](float v) { const float r = std::round(v + 127.5f); return uint8_t(r < 0 ? 0 : r > 255 ? 255 : r); };
    for (size_t i = 0; i < up.size(); ++i) { out[i * 2] = u8(up[i].real() * g); out[i * 2 + 1] = u8(up[i].imag() * g); }
    std::FILE* f = std::fopen(argv[1], "wb");
    if (!f) { std::perror(argv[1]); return 1; }
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    std::printf("%s %.1f s, mode %c, %d-QAM, %.0f dB\n", argv[1], double(up.size()) / 288000.0, modeLetter(c.mode), qam, snr);
    return 0;
}
