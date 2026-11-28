// bench-gainstep.cpp — what the SDRplay gain-step smoother (vibedsp/gainstep.h) costs on the DSP thread at 8 MS/s, in the
// RSP's own packet size (1344 samples a call — measured on the Pi 500's RSP1B, memory rsp_dsp_load_is_the_wide_emit).
// Standalone, silent, one thread. perf put it (with the libc copies under it) at ~12 % of the RSP's DSP thread on 11-28.
//   c++ -O3 -std=c++17 -I android/app/src/main/cpp vibeserver/bench-gainstep.cpp -o bench-gainstep && ./bench-gainstep
#include "vibedsp/gainstep.h"

#include <chrono>
#include <cstdio>
#include <vector>

int main() {
    const double rate = 8e6;
    const int pkt = 1344;
    vibedsp::GainStepSmoother g;
    g.configure(rate);
    std::vector<std::complex<float>> buf(pkt);
    for (int i = 0; i < pkt; ++i) buf[(size_t)i] = { 0.01f * (float)(i % 7), -0.01f * (float)(i % 5) };
    const int calls = (int)(rate * 10 / pkt);   // 10 s of capture
    for (int i = 0; i < 2000; ++i) g.process(buf.data(), pkt);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < calls; ++i) g.process(buf.data(), pkt);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("GainStepSmoother at %.0f MS/s, %d-sample packets: %.2f %% of one core (%.1f ns/sample)\n",
                rate / 1e6, pkt, 100.0 * s / 10.0, 1e9 * s / (calls * (double)pkt));
    return 0;
}
