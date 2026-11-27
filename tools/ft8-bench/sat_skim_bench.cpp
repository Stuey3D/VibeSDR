// sat_skim_bench.cpp — ★★★ beta97 the satellite digital-mode skimmer's CPU (decoders/sat_skim.h): a synthetic transponder
// passband of W windows, ONE FT8 or FT4 station in every window (a busy transponder — most real passes are emptier), on
// noise, fed through the skimmer for `slots` FT8 slots with ONE worker and a queue big enough to keep everything (so every
// task is decoded and timed, however slow the machine). Prints, per 15 s slot:
//   · the decode thread's share (the tuners: mix + decimate + filter per window, and the copy into the slot buffers),
//     measured on the feeding thread's own CPU clock, per second of stream;
//   · the workers' CPU (all FT8 + FT4 decodes of a slot), from Skimmer::stats — at full depth, as the live default runs it.
// Usage: sat_skim_bench [windows=25] [slots=4] [rate=96000]
//   build: tools/ft8-bench/build.sh (sat_skim_bench beside ft8_wav); armhf: tools/ft8-bench/qemu/xbuild.sh's flags.
#include "decoders/sat_skim.h"
#include "vibe_clock.h"
extern "C" {
#include "ft8/message.h"
#include "ft8/encode.h"
#include "ft8/constants.h"
}
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <string>
#include <vector>

using cf32 = std::complex<float>;
static double cpuNow() { timespec ts{}; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

static std::vector<uint8_t> tonesOf(const std::string& msg, bool ft4) {
    ftx_message_t m; ftx_message_init(&m);
    if (ftx_message_encode(&m, nullptr, msg.c_str()) != FTX_MESSAGE_RC_OK) return {};
    std::vector<uint8_t> t(ft4 ? FT4_NN : FT8_NN);
    if (ft4) ft4_encode(m.payload, t.data()); else ft8_encode(m.payload, t.data());
    return t;
}
static void addFtx(std::vector<cf32>& x, double fs, double offHz, double startSec, const std::vector<uint8_t>& tones, bool ft4, double amp) {
    const double sym = ft4 ? FT4_SYMBOL_PERIOD : FT8_SYMBOL_PERIOD, spacing = 1.0 / sym;
    const size_t s0 = (size_t)(startSec * fs), ns = (size_t)std::llround(sym * fs);
    double ph = 0;
    for (size_t k = 0; k < tones.size(); ++k)
        for (size_t j = 0; j < ns; ++j) {
            const size_t i = s0 + k * ns + j;
            if (i >= x.size()) return;
            ph += 2 * M_PI * (offHz + tones[k] * spacing) / fs;
            x[i] += cf32((float)(amp * std::cos(ph)), (float)(amp * std::sin(ph)));
        }
}

int main(int argc, char** argv) {
    const int W = argc > 1 ? std::atoi(argv[1]) : 25;
    const int slots = argc > 2 ? std::atoi(argv[2]) : 4;
    const double fs = argc > 3 ? std::atof(argv[3]) : 96000;
    const double centre = 435640000;
    const double span = (W - 1) * vibe::satskim::kWinStepHz + 2800 - 2 * vibe::satskim::kEdgeHz;
    vibe::satskim::Skimmer::Config c;
    c.centreHz = centre; c.loHz = centre - span / 2; c.hiHz = centre + span / 2;
    c.mode = vibe::satskim::Skimmer::Mode::CatchUp; c.workers = 1; c.capBytes = (size_t)2 << 30;
    uint64_t spots = 0;
    c.emit = [&](const vibe::digi::Spot&, double, const vibe::satskim::Tag&) { ++spots; };
    vibe::satskim::Skimmer sk(c);
    if ((int)sk.dials().size() != W) std::printf("(%zu windows)\n", sk.dials().size());
    const double secs = slots * 15.0 + 1.5;
    std::vector<cf32> iq((size_t)(secs * fs));
    std::mt19937 rng(5); std::normal_distribution<float> g(0.0f, 1.0f);
    for (auto& v : iq) v = cf32(g(rng), g(rng));
    const double amp = std::sqrt(std::pow(10.0, -12 / 10.0) * 2 / fs * 2500.0);   // −12 dB in 2500 Hz
    const char* calls[] = { "G4ABC", "DL1XYZ", "F5AAA", "W1AW", "JA1QRZ", "VK2DEF", "PY2ABC", "ZS6XYZ", "EA4BBB", "K9CCC", "OH2DDD", "SM5EEE" };
    for (int w = 0; w < (int)sk.dials().size(); ++w) {
        const bool ft4 = w % 2 == 1;
        const auto t = tonesOf(std::string("CQ ") + calls[w % 12] + " IO91", ft4);
        const double off = sk.dials()[(size_t)w] + 900 + 37 * (w % 7) - centre;
        for (int s = 0; s < slots * (ft4 ? 2 : 1); ++s) addFtx(iq, fs, off, 1.0 + s * (ft4 ? 7.5 : 15.0) + 0.5, t, ft4, amp);
    }
    const double t0 = std::ceil(vibe::vibeUtcNow() / 15.0) * 15.0 - 1.0;
    const int blk = (int)(fs / 50);
    const double c0 = cpuNow();
    for (size_t off = 0; off < iq.size(); off += (size_t)blk) {
        const int n = (int)std::min<size_t>((size_t)blk, iq.size() - off);
        sk.feed(iq.data() + off, n, fs, t0 + (double)(off + n) / fs);
    }
    const double feedCpu = cpuNow() - c0;
    sk.finish();
    sk.waitIdle(3600);
    const auto st = sk.stats();
    std::printf("windows %d, %.0f kS/s slice, %d slots, one station a window (FT8 / FT4 alternately, -12 dB)\n", (int)sk.dials().size(), fs / 1000, slots);
    std::printf("decode thread (tuners + copy): %.1f ms CPU per second of stream (%.2f %% of one core)\n", feedCpu / secs * 1000, feedCpu / secs * 100);
    std::printf("workers: %llu tasks, %.1f ms CPU a task (mean), %.0f ms CPU per 15 s slot (%.1f %% of one core), %llu spots\n",
                (unsigned long long)st.tasksDecoded, st.tasksDecoded ? st.cpuMsPerSlot / (3.0 * sk.dials().size()) : 0.0,
                st.cpuMsPerSlot, st.cpuMsPerSlot / 150.0, (unsigned long long)spots);
    return 0;
}
