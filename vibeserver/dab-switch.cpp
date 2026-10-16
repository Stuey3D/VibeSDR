// dab-switch — HOW LONG DOES CHANGING STATION TAKE, and what does the label scan cost it?
//
// ★★★ Stuart, 2026-09-20: a Sony TV with the whole-multiplex scan OFF switches station in 1-2 s; the Pi 500 and
//     the Lenovo, with it ON, take 3-4 s. Every rotation of the scanner makes the front end wait for the MSC
//     thread to drain, and a station change that lands mid-rotation queues behind work nobody is listening to.
//     This measures that, on the benchmark's own multiplex, so the fix is judged by a number and not by feel.
//
// usage: dab-switch <clip.vbu8> [switches]     (VIBE_DAB_SCAN=0/1, VIBE_DAB_SCAN_HOLD=0 for the A/B)
#include "vibe_benchmark_dab.h"
#include "vibe_thread_layout.h"
#include <cstdio>
#include <chrono>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: dab-switch <clip.vbu8> [switches]\n"); return 2; }
    const int switches = argc > 2 ? std::atoi(argv[2]) : 6;
    vibe::DabClip clip;
    if (!vibe::loadDabClip(argv[1], clip)) { std::printf("cannot read %s\n", argv[1]); return 2; }
    vibe::vibeApplyDefaultThreadLayout();   // ★ the served layout — see vibe_thread_layout.h
    vibedab::DabService svc;
    svc.setRfRate(clip.rate);
    svc.setRfCentre(clip.centre);
    for (int i = 0; i < int(vibedab::kBandIIICount); ++i)
        if (std::fabs(double(vibedab::kBandIII[i].centreHz) - clip.centre) < 1000.0) { svc.setChannel(i); break; }

    const size_t blk = size_t(clip.rate / 20) * 2;      // 50 ms of interleaved I/Q
    size_t pos = 0;
    std::vector<float> pcm(48000);
    const auto t0 = std::chrono::steady_clock::now();
    double fed = 0;
    auto feedFor = [&](double secs, uint64_t* firstAudioAt) -> bool {
        const double until = fed + secs;
        while (fed < until) {
            const size_t n = std::min(blk, clip.iq.size() - pos);
            svc.feed(clip.iq.data() + pos, n / 2);
            pos = (pos + n) % clip.iq.size();
            fed += double(n / 2) / clip.rate;
            if (svc.takePcm(pcm.data(), pcm.size() / 2) > 0 && firstAudioAt && !*firstAudioAt) {
                *firstAudioAt = 1;
                return true;                              // audio started
            }
            std::this_thread::sleep_until(t0 + std::chrono::microseconds((long long)(fed * 1e6)));
        }
        return false;
    };
    // Lock, and get the first service playing.
    svc.setService(clip.sid);
    feedFor(12.0, nullptr);
    auto learn = svc.learnable();
    if (learn.size() < 2) { std::printf("only %zu services learned — not enough to switch between\n", learn.size()); return 1; }
    std::printf("scan %s, hold %s — %zu services\n",
                vibedab::dabScanLabelsOn() ? "ON" : "off",
                (std::getenv("VIBE_DAB_SCAN_HOLD") && std::getenv("VIBE_DAB_SCAN_HOLD")[0] == '0') ? "off" : "ON",
                learn.size());
    double total = 0; int done = 0;
    for (int i = 0; i < switches; ++i) {
        const uint32_t sid = learn[(i + 1) % learn.size()].sid;
        // ★ Let the scanner get going again between switches, or every switch but the first measures a quiet box.
        feedFor(5.0, nullptr);
        const double mark = fed;
        uint64_t got = 0;
        svc.setService(sid);
        const bool ok = feedFor(10.0, &got);
        if (ok) { total += fed - mark; done++; std::printf("  switch %d: %.2f s\n", i + 1, fed - mark); }
        else     std::printf("  switch %d: no audio within 10 s\n", i + 1);
    }
    if (done) std::printf("mean %.2f s over %d switches\n", total / done, done);
    return 0;
}
