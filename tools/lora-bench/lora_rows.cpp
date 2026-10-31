// tools/lora-bench/lora_rows.cpp — ONLY the benchmark's two LoRa rows (dec_lora, dec_lora_all), exactly as vibeserver-bench
// runs them (vibe_benchmark_more.h loraRealTimeRow: the threaded engine, real time, scored by the hottest thread), for a
// box where the whole benchmark is not wanted (the Pi 2 beside its live server). Prints each row, its grade, every thread.
//   build (Mac): c++ -std=c++17 -O3 -ffp-contract=fast -I android/app/src/main/cpp -o lora_rows tools/lora-bench/lora_rows.cpp -lpthread
//   armhf: the same with arm-linux-gnueabihf-g++ -marm -mfpu=neon-vfpv4 -mfloat-abi=hard (the server's own flags)
//   usage: lora_rows [repeats]   ★ silent: no audio anywhere
#include "vibe_benchmark_decoders.h"
#include "vibe_benchmark_more.h"
#include <cstdio>
#include <cstdlib>

// ★ vibe_benchmark.h benchdetail::grade, restated so this tool needs none of the listener benchmark's libraries
static const char* grade(double pct) { return pct < 0 ? "none" : pct < 70 ? "green" : pct <= 85 ? "amber" : "red"; }

int main(int argc, char** argv) {
    const int reps = argc > 1 ? std::max(1, std::atoi(argv[1])) : 1;
    for (int k = 0; k < reps; ++k)
        for (const auto& rr : { std::make_pair("dec_lora", 0.0), std::make_pair("dec_lora_all", (double)INFINITY) }) {
            const auto r = vibe::benchdec::loraRealTimeRow(rr.first, rr.first == std::string("dec_lora") ? "Decoder: LoRa (868 default channels)"
                                                                                                    : "Decoder: LoRa (868, every channel)", rr.second);
            std::printf("%-13s %6.1f %%  grade %-5s  hottest %-11s decoded %d  %s\n", r.id.c_str(), r.pct, grade(r.pct),
                        r.hottest.c_str(), r.decoded, r.label.c_str());
            for (const auto& t : r.threads) std::printf("      %-14s %6.1f %%\n", t.first.c_str(), t.second);
            std::fflush(stdout);
        }
    return 0;
}
