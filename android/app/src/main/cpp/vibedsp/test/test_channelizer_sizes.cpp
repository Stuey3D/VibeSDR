// test_channelizer_sizes.cpp — V12 (2026-11-19) THE LISTENERS' FFT AT A 2^a·3^b·5^c SIZE IS AS CLEAN AS THE POWER OF TWO.
// Stuart: "as long as the signal quality and audio quality is maintained" / "we cannot degrade the listening and use
// experience". vibedsp.h listenerFftSize picks e.g. 30720 points at 30.72 MS/s so a WFM listener's channel is 512 kHz,
// not 960. For each wide rate, the NEW geometry against the OLD (power of two) at the same rate, the same channel ask:
//   · a tone lands at the frequency it was sent at, at unit gain — swept over centre bins that are and are not a
//     multiple of 4 (the phase-continuation's blind spot) and over offsets across the channel's flat half;
//   · the passband is as flat (±0.05 dB across the WFM listener's ±100 kHz);
//   · the spur floor — what is left after the tone is fitted out — is no higher than the power of two's on the air
//     today, and below −100 dBc (the block-rate comb a broken join radiates shows here first; channelizer.cpp delays
//     the roll-off into the discarded head at these sizes — the power of two measures −46…−54 dBc without it);
//   · a tone well outside the channel is rejected as well as before.
// And every rate where a power of two already fits keeps it (bit for bit the old geometry).
#include "../vibedsp.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>
using namespace vibedsp;

static int failures = 0;
static void check(bool ok, const char* what) { std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) failures++; }
static int pow2For(double rate) { int s = 4096; while (s < (int)(rate / 75.0) && s < 32768) s *= 2; return s; }

struct Fit { double freq = 0, amp = 0, spurDb = 0; };
// Tone through a channel: input at `fIn` cycles/sample; returns the recovered tone and the residual after fitting it.
static Fit runTone(int N, int chanBins, int centreBin, double fIn, double chanRate, double rate) {
    Channelizer ch(N);
    std::vector<cf32> out, chunk((size_t)chanBins);
    const int hop = ch.hop();
    std::vector<cf32> in((size_t)hop);
    double ph = 0;
    const int blocks = 40;
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < hop; ++i) { in[(size_t)i] = cf32((float)std::cos(ph), (float)std::sin(ph)); ph += 2 * M_PI * fIn; if (ph > 2 * M_PI) ph -= 2 * M_PI; }
        Channelizer::ExtractCtx ctx;
        ch.feed(in.data(), hop, [&](const cf32* bins, int) {
            static thread_local Channelizer::ExtractCtx keep; (void)ctx;
            const int n = ch.extract(bins, centreBin, chanBins, chunk.data(), keep, ch.blockIndex());
            for (int i = 0; i < n; ++i) out.push_back(chunk[(size_t)i]);
        });
    }
    std::vector<cf32> t(out.begin() + (long)(out.size() / 4), out.end());   // settled
    // the expected frequency at the channel's rate
    const double binHz = rate / N;
    const double fHz = fIn * rate - centreBin * binHz;
    const double w = 2 * M_PI * fHz / chanRate;
    // least-squares complex amplitude at the known frequency
    std::complex<double> acc = 0;
    for (size_t i = 0; i < t.size(); ++i) acc += std::complex<double>(t[i]) * std::polar(1.0, -w * (double)i);
    acc /= (double)t.size();
    double res = 0, sig = 0;
    for (size_t i = 0; i < t.size(); ++i) {
        const std::complex<double> e = std::complex<double>(t[i]) - acc * std::polar(1.0, w * (double)i);
        res += std::norm(e); sig += std::norm(acc);
    }
    // measured frequency from the mean phase step
    std::complex<double> st = 0;
    for (size_t i = 1; i < t.size(); ++i) st += std::complex<double>(t[i]) * std::conj(std::complex<double>(t[i - 1]));
    Fit f; f.freq = std::arg(st) / (2 * M_PI) * chanRate; f.amp = std::abs(acc);
    f.spurDb = 10 * std::log10(res / std::max(sig, 1e-30) + 1e-30);
    (void)fHz;
    return f;
}

int main() {
    std::printf("Listener channeliser at 2^a·3^b·5^c sizes vs the power of two\n");
    // ── the rates where a power of two fits keep it, and their slices, exactly ──
    for (double r : { 1.024e6, 2.048e6, 2.4e6, 8e6, 16e6, 32e6 }) {
        const int p = pow2For(r);
        char b[160]; std::snprintf(b, sizeof b, "%.3f MS/s keeps its power of two (%d)", r / 1e6, p);
        check(listenerFftSize(r, p) == p, b);
    }
    {
        bool same = true;
        for (int n : { 4096, 8192, 16384, 32768 })
            for (double r : { 1.024e6, 2.4e6, 8e6, 30.72e6 })
                for (double need : { 24e3, 31.25e3, 500e3, 1e6 }) {
                    int b = 64; while (b < n && r * b / n < need) b <<= 1; b = std::min(b, n);
                    if (sliceBinsFor(n, r, need) != b) same = false;
                }
        check(same, "a power of two's slices are chosen exactly as before");
    }
    // ── the wide rates ──
    for (double rate : { 6e6, 10e6, 20e6, 25e6, 30.72e6, 40e6, 56e6 }) {
        const int P = pow2For(rate), N = listenerFftSize(rate, P);
        std::printf(" %.2f MS/s: %d -> %d points\n", rate / 1e6, P, N);
        check(N != P && fftSmooth235(N) && N % 64 == 0, "  a 2·3·5 size, a multiple of 64");
        const int cbN = sliceBinsFor(N, rate, kWfmChannelNeedHz), cbP = sliceBinsFor(P, rate, kWfmChannelNeedHz);
        const double crN = rate * cbN / N, crP = rate * cbP / P;
        char m[200]; std::snprintf(m, sizeof m, "  WFM channel %.1f kHz (was %.1f), never under 500", crN / 1e3, crP / 1e3);
        check(crN >= 500e3 - 1 && crN < crP, m);
        double worstFreq = 0, worstAmp = 0, worstSpurN = -999, worstSpurP = -999;
        // centre bins: a station a quarter of the way up, and the next three bins (not a multiple of 4)
        for (int dc = 0; dc < 4; ++dc)
            for (double offHz : { 0.0, 37e3, -61e3, 99e3, -99e3 }) {
                const double centreHz = rate / 4 + dc * (rate / N);
                const int cb = (int)std::lround(centreHz / (rate / N));
                const double fIn = (cb * (rate / N) + offHz) / rate;
                const Fit a = runTone(N, cbN, cb, fIn, crN, rate);
                const int cbp = (int)std::lround(centreHz / (rate / P));
                const double fInP = (cbp * (rate / P) + offHz) / rate;
                const Fit o = runTone(P, cbP, cbp, fInP, crP, rate);
                worstFreq = std::max(worstFreq, std::fabs(a.freq - offHz));
                worstAmp = std::max(worstAmp, std::fabs(20 * std::log10(a.amp)));
                worstSpurN = std::max(worstSpurN, a.spurDb); worstSpurP = std::max(worstSpurP, o.spurDb);
            }
        std::snprintf(m, sizeof m, "  tones land where sent (worst %.3f Hz off)", worstFreq); check(worstFreq < 1.0, m);
        std::snprintf(m, sizeof m, "  flat across ±100 kHz (worst %.3f dB)", worstAmp); check(worstAmp < 0.05, m);
        std::snprintf(m, sizeof m, "  spur floor %.1f dBc (the power of two on the air today %.1f) — no higher, under -100", worstSpurN, worstSpurP);
        check(worstSpurN < -100 && worstSpurN <= worstSpurP, m);
        // rejection: a tone a whole channel away
        {
            const int cb = (int)std::lround(rate / 4 / (rate / N));
            const Fit a = runTone(N, cbN, cb, (cb * (rate / N) + crN) / rate, crN, rate);
            const int cbp = (int)std::lround(rate / 4 / (rate / P));
            const Fit o = runTone(P, cbP, cbp, (cbp * (rate / P) + crP) / rate, crP, rate);
            std::snprintf(m, sizeof m, "  a tone a channel away rejected (%.1f dB; power of two %.1f dB)", 20 * std::log10(a.amp + 1e-12), 20 * std::log10(o.amp + 1e-12));
            check(20 * std::log10(a.amp + 1e-12) < -100, m);
        }
    }
    // ── the waterfall's own transform: the very block a power-of-two channeliser would hand out ──
    {
        const int n = 32768;
        Channelizer ch(n); PeriodicFft tap(n);
        std::vector<cf32> in(12345);
        double ph = 0; unsigned s = 1; long long blk = 0; double worst = 0; int compared = 0;
        std::vector<std::vector<cf32>> want;
        for (int round = 0; round < 60; ++round) {
            for (auto& v : in) { s = s * 1103515245u + 12345u; v = cf32((float)std::cos(ph), (float)std::sin(ph)) + cf32(((s >> 16) & 1023) / 4096.0f, 0); ph += 0.0123; }
            ch.feed(in.data(), (int)in.size(), [&](const cf32* b, int nb) { { const long long k = blk++; if (k > 0 && k % 3 == 0) want.emplace_back(b, b + nb); } });
            size_t k = 0;
            tap.feed(in.data(), (int)in.size(), [] { return 3; }, [&](const cf32* b, int nb) {
                (void)k;
                if (compared < (int)want.size()) {
                    double e = 0, r = 0;
                    for (int i = 0; i < nb; ++i) { e += std::norm(b[i] - want[(size_t)compared][(size_t)i]); r += std::norm(want[(size_t)compared][(size_t)i]); }
                    worst = std::max(worst, e / std::max(r, 1e-30)); ++compared;
                }
            });
        }
        char m[160]; std::snprintf(m, sizeof m, "the waterfall's own transform = the channeliser's block (%d compared, error %.1e)", compared, worst);
        check(compared >= 8 && worst < 1e-10, m);
    }
    std::printf(failures ? "\nFAILURES: %d\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
