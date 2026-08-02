// VibeSDR — channelizer CPU bench (host only, not part of the app build).
//
// THE QUESTION THIS ANSWERS: for VibeServer's multi-listener mode, does ka9q-style fast
// convolution actually cost less than giving every listener its own DDC? The brief asserts it
// does. An assertion is not a measurement, and the whole point of the 8 MHz RSP demo is that
// several people share one radio — so the number that matters is the SLOPE (cost per extra
// listener), not the intercept.
//
// Both paths are timed doing the same real job: take an 8 MSPS complex stream, and hand each
// listener a 15.625 kHz complex channel at their own centre frequency.
//
//   A. FAST CONVOLUTION — one shared forward FFT of the whole band, then per listener a slice
//      of bins + a small inverse FFT. The forward FFT is paid ONCE no matter how many listen.
//   B. PER-CLIENT DDC — an NCO mixing at the FULL input rate plus a decimating FIR cascade,
//      per listener. Nothing is shared, so every listener pays the full price again.
//
//   cmake --build build --target vibedsp_bench_channelizer && ./build/vibedsp_bench_channelizer
//
// Optional args: [sampleRate] [maxClients]
#include "vibedsp.h"
#include <cstdio>
#include <cmath>
#include <chrono>
#include <vector>
#include <memory>
#include <cstdlib>

using namespace vibedsp;

using Clock = std::chrono::steady_clock;
static double secsSince(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

int main(int argc, char** argv) {
    const double fs      = (argc > 1) ? std::atof(argv[1]) : 8000000.0;
    const int    maxCli  = (argc > 2) ? std::atoi(argv[2]) : 8;

    // The forward FFT size we ALREADY run for the spectrum at this rate — the whole argument for
    // fast convolution is that this cost is already on the books.
    const int N   = 32768;
    const int D   = 512;              // 8 MSPS / 512 = 15625 Hz per channel: enough for SSB/AM/NFM
    const int CH  = N / D;            // = 64 bins per channel slice
    const double chanRate = fs / D;

    const int    Ni   = 1 << 22;      // ~0.52 s of IQ at 8 MSPS
    const double secs = Ni / fs;

    std::printf("Channelizer bench — %.3f MSPS, %d-pt forward FFT, D=%d (%.0f Hz channels)\n",
                fs / 1e6, N, D, chanRate);
    std::printf("%.2f s of IQ per pass; times are %% of ONE core running in real time.\n\n", secs);

    // A stream with some structure in it, so nothing can be optimised away as zeros.
    std::vector<cf32> iq(Ni);
    {
        double p1 = 0, p2 = 0;
        for (int i = 0; i < Ni; ++i) {
            iq[i] = cf32((float)(0.5 * std::cos(p1) + 0.3 * std::cos(p2)),
                         (float)(0.5 * std::sin(p1) + 0.3 * std::sin(p2)));
            p1 += 2 * M_PI * 0.031; if (p1 > 2 * M_PI) p1 -= 2 * M_PI;
            p2 += 2 * M_PI * 0.187; if (p2 > 2 * M_PI) p2 -= 2 * M_PI;
        }
    }

    // ── A. Fast convolution ─────────────────────────────────────────────────────────────────
    // Measured twice: the forward FFT ALONE (the shared cost) and the forward FFT plus n slices,
    // so the per-client slope falls straight out of the difference.
    std::vector<double> fcTime(maxCli + 1, 0.0);
    for (int nc = 0; nc <= maxCli; ++nc) {
        Channelizer ch(N);
        std::vector<std::vector<cf32>> out(nc ? nc : 1, std::vector<cf32>(CH));
        // Spread the listeners across the band, as real ones would be.
        std::vector<int> centre(nc);
        for (int c = 0; c < nc; ++c) centre[c] = (int)((double)N * (0.1 + 0.8 * c / (nc ? nc : 1)));

        volatile float sink = 0;
        auto t0 = Clock::now();
        ch.feed(iq.data(), Ni, [&](const cf32* bins, int) {
            for (int c = 0; c < nc; ++c) {
                int n = ch.extract(bins, centre[c], CH, out[c].data());
                sink += out[c][n / 2].real();
            }
        });
        fcTime[nc] = secsSince(t0);
        (void)sink;
    }

    // ── B. Per-client DDC ───────────────────────────────────────────────────────────────────
    // Honest construction: a single-stage FIR at D=512 would need a preposterous tap count, so
    // this is the three-stage cascade anyone would actually write (8 x 8 x 8), with the tightest
    // filter last where it is cheapest to run.
    struct Ddc {
        NCO nco;
        std::unique_ptr<FirDecimator> s1, s2, s3;
        std::vector<cf32> a, b, c;
    };
    auto makeDdc = [&](double normFreq, int block) {
        auto d = std::make_unique<Ddc>();
        d->nco.setFreq(normFreq);
        d->s1 = std::make_unique<FirDecimator>(designLowpass(0.5 / 8, 0.25 / 8), 8);
        d->s2 = std::make_unique<FirDecimator>(designLowpass(0.5 / 8, 0.25 / 8), 8);
        d->s3 = std::make_unique<FirDecimator>(designLowpass(0.40,    0.10, true), 8);
        d->a.resize(block);
        d->b.resize(block / 8 + 8);
        d->c.resize(block / 64 + 8);
        return d;
    };

    const int BLK = 1 << 16;
    std::vector<double> ddcTime(maxCli + 1, 0.0);
    std::vector<cf32> dOut(BLK / D + 8);
    for (int nc = 1; nc <= maxCli; ++nc) {
        std::vector<std::unique_ptr<Ddc>> cli;
        for (int c = 0; c < nc; ++c) cli.push_back(makeDdc(0.4 * c / nc - 0.2, BLK));

        volatile float sink = 0;
        auto t0 = Clock::now();
        for (int off = 0; off + BLK <= Ni; off += BLK) {
            for (int c = 0; c < nc; ++c) {
                Ddc& d = *cli[c];
                d.nco.mix(iq.data() + off, d.a.data(), BLK);           // <-- at the FULL input rate
                int n1 = d.s1->process(d.a.data(), BLK,  d.b.data());
                int n2 = d.s2->process(d.b.data(), n1,   d.c.data());
                int n3 = d.s3->process(d.c.data(), n2,   dOut.data());
                if (n3 > 0) sink += dOut[0].real();
            }
        }
        ddcTime[nc] = secsSince(t0);
        (void)sink;
    }

    // ── Report ──────────────────────────────────────────────────────────────────────────────
    const double pc = 100.0 / secs;
    std::printf("  forward FFT alone (shared by everyone) : %6.2f %% of one core\n\n",
                fcTime[0] * pc);
    std::printf("  clients   fast convolution      per-client DDC     DDC / fastconv\n");
    for (int nc = 1; nc <= maxCli; ++nc)
        std::printf("  %6d   %7.2f %%             %7.2f %%            %5.1fx\n",
                    nc, fcTime[nc] * pc, ddcTime[nc] * pc,
                    fcTime[nc] > 0 ? ddcTime[nc] / fcTime[nc] : 0.0);

    const double fcSlope  = (fcTime[maxCli]  - fcTime[1]) / (maxCli - 1);
    const double ddcSlope = (ddcTime[maxCli] - ddcTime[1]) / (maxCli - 1);
    std::printf("\n  cost of ONE MORE listener:  fast convolution %5.2f %%   DDC %5.2f %%  (%.0fx)\n",
                fcSlope * pc, ddcSlope * pc, fcSlope > 0 ? ddcSlope / fcSlope : 0.0);
    if (fcSlope > 0)
        std::printf("  headroom on this machine, one core: ~%d listeners by fast convolution,"
                    " ~%d by DDC\n",
                    (int)((secs - fcTime[0]) / fcSlope), (int)(secs / ddcSlope));
    return 0;
}
