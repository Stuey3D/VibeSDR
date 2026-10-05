// ft8_synth — a synthetic FT8 band with KNOWN answers: real messages (ft8_lib's encoder), GFSK as WSJT-X sends
// them, at set SNRs (WSJT-X's 2500 Hz definition), DTs and frequencies, in white noise — including weak signals
// overlapping strong ones, which only subtraction can recover.
//
//   ft8_synth OUT.wav SLOTS SEED     → writes 12 kHz mono s16; prints "T <slot> <snr> <dt> <hz> <message>" per signal
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

extern "C" {
#include "ft8/constants.h"
#include "ft8/encode.h"
#include "ft8/message.h"
}

static void gfskAdd(std::vector<double>& out, const uint8_t* tones, int nn, double f0, double t0, double amp, int fs) {
    const int nsps = (int)(0.16 * fs);
    const double bt = 2.0, k = 5.336446;
    std::vector<double> pulse((size_t)3 * nsps);
    for (int i = 0; i < 3 * nsps; ++i) {
        const double t = i / (double)nsps - 1.5;
        pulse[(size_t)i] = (std::erf(k * bt * (t + 0.5)) - std::erf(k * bt * (t - 0.5))) / 2;
    }
    const int nw = nn * nsps;
    std::vector<double> dphi((size_t)(nw + 2 * nsps), 2 * M_PI * f0 / fs);
    const double peak = 2 * M_PI / nsps;
    for (int i = 0; i < nn; ++i)
        for (int j = 0; j < 3 * nsps; ++j) dphi[(size_t)(i * nsps + j)] += peak * tones[i] * pulse[(size_t)j];
    for (int j = 0; j < 2 * nsps; ++j) {
        dphi[(size_t)j] += peak * pulse[(size_t)(j + nsps)] * tones[0];
        dphi[(size_t)(j + nn * nsps)] += peak * pulse[(size_t)j] * tones[nn - 1];
    }
    const long s0 = std::lround(t0 * fs);
    const int ramp = nsps / 8;
    double phi = 0;
    for (int n = 0; n < nw; ++n) {
        double env = 1;
        if (n < ramp) env = (1 - std::cos(M_PI * n / ramp)) / 2;
        if (n >= nw - ramp) env = (1 - std::cos(M_PI * (nw - 1 - n) / ramp)) / 2;
        const long i = s0 + n;
        if (i >= 0 && i < (long)out.size()) out[(size_t)i] += amp * env * std::sin(phi);
        phi += dphi[(size_t)(n + nsps)];
    }
}

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: ft8_synth OUT.wav SLOTS SEED\n"); return 2; }
    const int fs = 12000, slots = std::atoi(argv[2]);
    std::mt19937 rng((unsigned)std::atoi(argv[3]));
    std::uniform_real_distribution<double> U(0, 1);
    std::vector<double> x((size_t)slots * 15 * fs, 0.0);
    const double sigma = 0.02;                                   // noise std (full 6 kHz band)
    const double n2500 = sigma * sigma * 2500.0 / (fs / 2.0);    // noise power in 2500 Hz
    const char* pre[] = { "G", "M", "DL", "F", "EA", "I", "SP", "OK", "PA", "ON", "LA", "OH", "SM", "K", "W" };
    for (int s = 0; s < slots; ++s) {
        const int n = 22;
        for (int i = 0; i < n; ++i) {
            char call[16], msg[40];
            std::snprintf(call, sizeof call, "%s%d%c%c%c", pre[rng() % 15], (int)(rng() % 10), 'A' + (int)(rng() % 26), 'A' + (int)(rng() % 26), 'A' + (int)(rng() % 26));
            std::snprintf(msg, sizeof msg, "CQ %s %c%c%d%d", call, 'I' + (int)(rng() % 3), 'L' + (int)(rng() % 5), (int)(rng() % 10), (int)(rng() % 10));
            ftx_message_t m; ftx_message_init(&m);
            if (ftx_message_encode(&m, nullptr, msg) != FTX_MESSAGE_RC_OK) continue;
            uint8_t tones[FT8_NN]; ft8_encode(m.payload, tones);
            // half the signals strong and spread out, half weak and overlapping one of the strong ones in frequency
            double hz, snr;
            if (i < n / 2) { hz = 300 + i * 230 + U(rng) * 60; snr = -8 + U(rng) * 16; }
            else { hz = 300 + (i - n / 2) * 230 + 15 + U(rng) * 25; snr = -20 + U(rng) * 6; }
            const double dt = -0.3 + U(rng) * 1.0;
            const double amp = std::sqrt(2 * n2500 * std::pow(10.0, snr / 10));
            gfskAdd(x, tones, FT8_NN, hz, s * 15.0 + 0.5 + dt, amp, fs);
            std::printf("T %d %.0f %.2f %.1f %s\n", s * 15, snr, dt, hz, msg);
        }
    }
    std::normal_distribution<double> N(0, sigma);
    FILE* f = std::fopen(argv[1], "wb");
    if (!f) return 2;
    const uint32_t nb = (uint32_t)x.size() * 2, r = fs, br = fs * 2;
    const uint16_t one = 1, bits = 16, ba = 2;
    const uint32_t riff = 36 + nb, fmtSz = 16;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmtSz, 4, 1, f); std::fwrite(&one, 2, 1, f); std::fwrite(&one, 2, 1, f);
    std::fwrite(&r, 4, 1, f); std::fwrite(&br, 4, 1, f); std::fwrite(&ba, 2, 1, f); std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f); std::fwrite(&nb, 4, 1, f);
    for (double v : x) {
        const double y = (v + N(rng)) * 32767.0;
        const int16_t s = (int16_t)std::max(-32768.0, std::min(32767.0, std::round(y)));
        std::fwrite(&s, 2, 1, f);
    }
    std::fclose(f);
    return 0;
}
