// SPIKE — can a per-client demod run off the SHARED forward FFT?
//
// The question this answers, before any refactor of local_sdr_shim.cpp: take one Channelizer at
// the capture rate, extract a narrow slice at an arbitrary offset, feed that slice to an
// RxPipeline STARTED AT THE CHANNEL RATE, and get correct audio out. If that works, N listeners
// each tuning independently costs one extract + one narrow demod each — the +0.02%/listener
// figure — instead of an NCO at the full rate per listener (+13% each, out of road at ~8).
//
// ★★ WHY A SPIKE AND NOT A BRANCH. The shim is 6000 lines and ships inside the phone app. Finding
// out there that a narrow-fed pipeline mistunes, or that the channel filter is too sharp for the
// overlap, would mean unpicking a refactor to learn a DSP fact. This learns the DSP fact first.
//
// Build:
//   c++ -std=c++17 -O2 -I android/app/src/main/cpp/vibedsp \
//       tools/spike-per-client-channel.cpp \
//       android/app/src/main/cpp/vibedsp/*.cpp \
//       android/app/src/main/cpp/vibedsp/third_party/kissfft/kiss_fft.c \
//       android/app/src/main/cpp/vibedsp/third_party/kissfft/kiss_fftr.c -o /tmp/spike
//
// The test signal is three AM tones at known offsets, exactly like fake-rtl-tcp: if two "clients"
// tuned to two different tones recover two different modulations, independent tuning works.
#include "vibedsp.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <chrono>
#include <memory>

using vibedsp::cf32;

namespace {

constexpr double FS       = 2'400'000.0;   // capture rate
constexpr int    FFT_N    = 32768;         // the shared forward FFT
constexpr double AUDIO_SR = 48000.0;

/** Three AM carriers at fixed offsets from centre, each with its OWN audio tone — so which
 *  station you are tuned to is audible as a different pitch, not merely a different level. */
struct Source {
    double t = 0.0;
    struct Station { double offsetHz, toneHz, depth; };
    std::vector<Station> st = {
        { -600'000.0,  400.0, 0.8 },
        {        0.0, 1000.0, 0.8 },
        { +700'000.0, 2500.0, 0.8 },
    };
    void fill(cf32* out, int n) {
        for (int i = 0; i < n; i++) {
            double re = 0, im = 0;
            for (auto& s : st) {
                const double env = 1.0 + s.depth * std::sin(2 * M_PI * s.toneHz * t);
                const double ph  = 2 * M_PI * s.offsetHz * t;
                re += env * std::cos(ph);
                im += env * std::sin(ph);
            }
            out[i] = cf32{ (float)(re * 0.1), (float)(im * 0.1) };
            t += 1.0 / FS;
        }
    }
};

/** Estimate the dominant audio frequency by counting zero crossings of the DC-removed signal.
 *  Crude on purpose — we are asking "which station is this?", not measuring THD. */
double dominantHz(const std::vector<float>& x, double sr) {
    if (x.size() < 64) return 0;
    double mean = 0; for (float v : x) mean += v; mean /= (double)x.size();
    int crossings = 0;
    for (size_t i = 1; i < x.size(); i++) {
        const double a = x[i-1] - mean, b = x[i] - mean;
        if ((a <= 0 && b > 0) || (a >= 0 && b < 0)) crossings++;
    }
    return (crossings / 2.0) * sr / (double)x.size();
}

/** One listener: its own channel out of the shared FFT, its own demod, its own audio. */
struct Client {
    const char* name;
    double offsetHz;                  // where THIS listener is tuned, relative to band centre
    int    chanBins;                  // power of two dividing FFT_N; decimation = FFT_N/chanBins
    vibedsp::RxPipeline rx;
    std::vector<cf32>   slice;
    std::vector<float>  audio;
    double chanRate = 0;
    bool   verbose = false;

    static void audioCb(void* ctx, const float* pcm, int frames, int ch, int) {
        auto* c = (Client*)ctx;
        for (int i = 0; i < frames; i++) c->audio.push_back(pcm[i * ch]);
    }

    void start() {
        chanRate = FS * (double)chanBins / (double)FFT_N;
        slice.resize(chanBins);
        vibedsp::RxPipeline::Callbacks cb{};
        cb.ctx = this;
        cb.audio = &Client::audioCb;
        // ★ The pipeline runs at the CHANNEL rate, not the capture rate. That is the whole point:
        //   everything downstream of the extract is narrow, so it costs almost nothing.
        rx.start(chanRate, 1024, 10.0, (int)AUDIO_SR, cb);
        // ★ The slice is already centred on this listener's frequency, so the pipeline tunes to
        //   offset 0 — the channelizer did the down-conversion.
        rx.setTune(0.0, vibedsp::RxPipeline::Mode::AM, 8000.0);
        if (verbose)
            printf("  %-8s offset %+8.0f Hz  chanBins %5d  chanRate %9.1f Hz\n",
                   name, offsetHz, chanBins, chanRate);
    }

    /** centreBin for this listener, as a SIGNED offset from DC. */
    int centreBin() const { return (int)std::lround(offsetHz / (FS / (double)FFT_N)); }
};

} // namespace

/** ★★ THE COST CURVE IS THE WHOLE ARGUMENT. Per-client tuning is only worth having if the Nth
 *  listener is nearly free; if it is not, the honest answer is a low listener cap, not this
 *  architecture. Measure it rather than quoting the brief. */
int measureCost(int nClients) {
    vibedsp::Channelizer chan(FFT_N);
    Source src;
    std::vector<std::unique_ptr<Client>> cs;
    for (int i = 0; i < nClients; i++) {
        auto c = std::unique_ptr<Client>(new Client{"x", (i % 7 - 3) * 150'000.0, 512});
        c->start();
        cs.push_back(std::move(c));
    }
    std::vector<cf32> buf(65536);
    const int rounds = 40;
    const auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < rounds; r++) {
        src.fill(buf.data(), (int)buf.size());
        chan.feed(buf.data(), (int)buf.size(), [&](const cf32* bins, int nbins) {
            (void)nbins;
            for (auto& cl : cs) {
                const int n = chan.extract(bins, cl->centreBin(), cl->chanBins, cl->slice.data());
                if (n > 0) cl->rx.feed(cl->slice.data(), n);
            }
        });
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double audioSec = rounds * (double)buf.size() / FS;      // seconds of RF processed
    printf("  %3d listeners: %8.1f ms for %.2f s of RF   = %5.2f%% of one core\n",
           nClients, ms, audioSec, ms / (audioSec * 1000.0) * 100.0);
    return 0;
}

int main() {
    printf("SPIKE: per-client channel off one shared forward FFT\n");
    printf("  capture %.1f MHz, FFT %d, bin %.1f Hz\n\n", FS/1e6, FFT_N, FS/FFT_N);

    vibedsp::Channelizer chan(FFT_N);
    Source src;

    // Two listeners on DIFFERENT stations, plus one on the centre.
    Client a{"A", -600'000.0, 512}; a.verbose = true;
    Client b{"B",  +700'000.0, 512}; b.verbose = true;
    Client c{"C",         0.0, 512}; c.verbose = true;
    a.start(); b.start(); c.start();
    printf("\n");

    std::vector<cf32> buf(65536);
    const int rounds = 60;
    for (int r = 0; r < rounds; r++) {
        src.fill(buf.data(), (int)buf.size());
        chan.feed(buf.data(), (int)buf.size(), [&](const cf32* bins, int nbins) {
            (void)nbins;
            for (Client* cl : { &a, &b, &c }) {
                const int n = chan.extract(bins, cl->centreBin(), cl->chanBins, cl->slice.data());
                if (n > 0) cl->rx.feed(cl->slice.data(), n);
            }
        });
    }

    printf("recovered audio (expected: A=400 Hz, B=2500 Hz, C=1000 Hz)\n");
    int pass = 0;
    for (auto* cl : { &a, &b, &c }) {
        // Drop the first 20% — filter transients at start-up are not the thing under test.
        std::vector<float> tail(cl->audio.begin() + cl->audio.size()/5, cl->audio.end());
        const double hz = dominantHz(tail, AUDIO_SR);
        double want = cl == &a ? 400.0 : cl == &b ? 2500.0 : 1000.0;
        const bool ok = std::fabs(hz - want) < want * 0.25;
        printf("  %-8s %6zu samples   dominant %7.1f Hz   want %6.1f   %s\n",
               cl->name, cl->audio.size(), hz, want, ok ? "OK" : "WRONG");
        pass += ok ? 1 : 0;
    }
    if (pass == 3) {
        printf("\ncost, one shared forward FFT (quiet the start-up prints first):\n");
        for (int n : {0, 1, 2, 4, 8, 16, 30}) measureCost(n);
    }
    printf("\n%s\n", pass == 3
        ? "PASS — three listeners, three different stations, one shared FFT."
        : "FAIL — the narrow-fed pipeline does not recover the right station.");
    return pass == 3 ? 0 : 1;
}
