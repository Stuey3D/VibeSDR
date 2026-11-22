// wefax_cpu — WHAT ONE WEFAX STATIONS CHANNEL COSTS (V12 2026-11-22). Measured on this thread's own CPU clock, the way
// the background runner measures itself: the shared channeliser FFT alone, then with N channels each doing what a WEFAX
// stations channel does per block — its slice cut (one small inverse FFT), the server's RxPipeline USB demodulator to
// 48 kHz, and the WEFAX decoder (background options). Per channel = (with N − alone) / N, as % of one core in real time.
//
//   build: as vibeserver/test-bgdecode (run-tests.sh flags_for/deps_for test-bgdecode), this file in its place
//   ./wefax_cpu [seconds=60] [channels=4] [rate=2000000]
// ★ Noise in every channel: the decoder does the same work per line whatever it hears (no start tone = nothing drawn,
//   which is cheaper still — so chart lines are fed to channel 0 for a worst case). SILENT.
#include "vibe_bgdecode.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>

using namespace vibe;
static double cpuS() { timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(int argc, char** argv) {
    const double secs = argc > 1 ? std::atof(argv[1]) : 60, rate = argc > 3 ? std::atof(argv[3]) : 2000000;
    const int nch = argc > 2 ? std::atoi(argv[2]) : 4;
    int fft = 4096; while (fft < 65536 && fft < rate / 40.0) fft <<= 1;   // ★ the runner's own FFT size (bgParams)
    const double binHz = rate / fft;
    int bins = 64; while (bins < fft && rate * bins / fft < 24000.0) bins <<= 1;
    const double crate = rate * bins / fft;
    std::mt19937 rng(3); std::normal_distribution<float> g(0, 0.02f);
    const size_t block = 65536;
    std::vector<vibedsp::cf32> iq(block);
    auto run = [&](int n) {
        vibedsp::Channelizer ch(fft);
        struct C { std::vector<vibedsp::cf32> slice; vibedsp::Channelizer::ExtractCtx ctx; std::unique_ptr<vibedsp::RxPipeline> rx;
                   std::unique_ptr<WefaxDecoder> dec; std::vector<int16_t> pcm; int cb = 0; };
        std::vector<std::unique_ptr<C>> cs;
        for (int k = 0; k < n; k++) {
            auto c = std::make_unique<C>();
            c->slice.resize(bins); c->cb = (int)std::lround((-300000.0 + 70000.0 * k) / binHz);
            WefaxDecoder::Config cfg; cfg.autoStart = true; cfg.autoStop = true;
            c->dec.reset(new WefaxDecoder(48000, cfg));
            c->rx.reset(new vibedsp::RxPipeline());
            vibedsp::RxPipeline::Callbacks cb{}; cb.ctx = c.get();
            cb.audio = [](void* ctx, const float* p, int fr, int chn, int) {
                auto* cc = (C*)ctx; cc->pcm.resize((size_t)fr);
                for (int i = 0; i < fr; i++) { const float v = p[(size_t)i * (chn == 2 ? 2 : 1)] * 32767.0f; cc->pcm[(size_t)i] = (int16_t)std::max(-32768.0f, std::min(32767.0f, v)); }
                cc->dec->process(cc->pcm.data(), fr);
            };
            c->rx->start(crate, 1024, 10.0, 48000, cb);
            c->rx->setRdsEnabled(false);
            c->rx->setTune(0, vibedsp::RxPipeline::Mode::SSB_USB, 2700.0);
            cs.push_back(std::move(c));
        }
        long long bi = 0;
        const double t0 = cpuS();
        for (double done = 0; done < secs; done += block / rate) {
            for (auto& x : iq) x = vibedsp::cf32(g(rng), g(rng));
            ch.feed(iq.data(), (int)iq.size(), [&](const vibedsp::cf32* b, int) {
                for (auto& c : cs) { const int got = ch.extract(b, c->cb, bins, c->slice.data(), c->ctx, bi); if (got > 0) c->rx->feed(c->slice.data(), got); }
                bi++;
            });
        }
        return cpuS() - t0;
    };
    // the noise generator's own cost, to take out
    double tg0 = cpuS(); for (double done = 0; done < secs; done += block / rate) for (auto& x : iq) x = vibedsp::cf32(g(rng), g(rng)); const double gen = cpuS() - tg0;
    const double a = run(0) - gen, b = run(nch) - gen;
    std::printf("rate %.0f S/s, FFT %d, channel %d bins (%.0f S/s), %.0f s of IQ\n", rate, fft, bins, crate, secs);
    std::printf("channeliser alone: %.2f %% of one core\n", 100 * a / secs);
    std::printf("%d WEFAX channels: %.2f %% — per channel %.3f %% of one core (slice cut + USB demod + WEFAX decoder)\n",
                nch, 100 * b / secs, 100 * (b - a) / nch / secs);
    return 0;
}
