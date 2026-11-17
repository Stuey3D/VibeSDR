// test-dab-bank.cpp — V12 DAB PHASE 2 §11.1 "LARGE N": the overlap-save filter bank (vibe_dab_bank.h) against the per-mux
// DDC it replaces (vibe_dab_ddc.h). The bank's output for a block must be the DDC's (same rate, same band, same contract):
//   · the plan at 8 / 16 / 32 / 56 MS/s (N, H, D, mid rate = the DDC's own);
//   · a tone in the block comes out pure, at its own frequency, phase-continuous across thousands of hops (56 MS/s, a block
//     OFF the bin grid);
//   · a neighbour block's carrier (1.712 MHz away) is rejected like the DDC rejects it;
//   · in-band content: the bank's output IS the DDC's (correlation at the best lag);
//   · the costs (one forward FFT for everyone, a small inverse per block) are printed for the record.
// Silent; exits non-zero on a failure.
#include "vibe_dab_bank.h"
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <mutex>
#include <climits>

using namespace vibedab;
static int fails = 0, passes = 0;
static void ok(bool c, const std::string& what) { if (c) ++passes; else { ++fails; std::printf("  FAIL %s\n", what.c_str()); } }
using C = std::complex<double>;

static std::vector<float> tones(double fs, double secs, const std::vector<std::pair<double, double>>& f /* hz, amp */, uint32_t seed = 1) {
    const size_t n = size_t(fs * secs);
    std::vector<float> v(2 * n, 0.0f);
    std::mt19937 rng(seed); std::uniform_real_distribution<double> ph(0, 2 * M_PI);
    for (const auto& t : f) {
        const double w = 2 * M_PI * t.first / fs, p0 = ph(rng);
        C r = std::polar(t.second, p0), s = std::polar(1.0, w);
        for (size_t i = 0; i < n; ++i) { v[2 * i] += float(r.real()); v[2 * i + 1] += float(r.imag()); r *= s; if ((i & 4095) == 0) r /= std::abs(r) / t.second; }
    }
    return v;
}
/** The power at `hz` (a projection) and the rest, over the second half of `y` (2.048 MS/s). */
static void toneSnr(const std::vector<float>& y, double hz, double& pTone, double& pRest) {
    const size_t n = y.size() / 2, a = n / 2;
    C acc = 0; const double w = -2 * M_PI * hz / DabDdc::kOutHz;
    for (size_t i = a; i < n; ++i) acc += C(y[2 * i], y[2 * i + 1]) * std::polar(1.0, w * double(i));
    acc /= double(n - a);
    double tot = 0; for (size_t i = a; i < n; ++i) tot += double(y[2 * i]) * y[2 * i] + double(y[2 * i + 1]) * y[2 * i + 1];
    tot /= double(n - a);
    pTone = std::norm(acc); pRest = std::max(1e-30, tot - pTone);
}
static double powerOf(const std::vector<float>& y) { const size_t n = y.size() / 2, a = n / 2; double t = 0;
    for (size_t i = a; i < n; ++i) t += double(y[2 * i]) * y[2 * i] + double(y[2 * i + 1]) * y[2 * i + 1]; return t / double(std::max<size_t>(1, n - a)); }
static std::vector<float> runBank(double fs, double shift, const std::vector<float>& x) {
    bank::Bank b; b.configure(fs); b.setSlice(0, shift);
    std::vector<std::vector<float>> outs(1);
    const size_t chunk = 65536;
    for (size_t i = 0; i < x.size() / 2; i += chunk) b.process(x.data() + 2 * i, std::min(chunk, x.size() / 2 - i), outs);
    b.slice(0)->flush(outs[0]);
    return outs[0];
}
static std::vector<float> runDdc(double fs, double shift, const std::vector<float>& x) {
    DabDdc d; d.configure(fs, shift); std::vector<float> out;
    const size_t chunk = 65536;
    for (size_t i = 0; i < x.size() / 2; i += chunk) d.process(x.data() + 2 * i, std::min(chunk, x.size() / 2 - i), out);
    return out;
}

int main() {
    // ── the plan ──
    for (double fs : { 8e6, 16e6, 32e6, 56e6 }) {
        const bank::Plan p = bank::plan(fs);
        DabDdc d; d.configure(fs, 0);
        ok(p.ok() && std::fabs(p.mid - d.midRate()) < 1.0, std::to_string(int(fs / 1e6)) + " MS/s: plan N " + std::to_string(p.N) + " H " + std::to_string(p.H)
           + " D " + std::to_string(p.D) + " L " + std::to_string(p.L) + ", mid rate " + std::to_string(p.mid) + " = the DDC's own");
        ok(p.H % p.D == 0 && p.N - p.H >= p.L - 1, "  its overlap holds the filter and its hop divides by D");
    }
    // ── a tone, 56 MS/s, a block off the bin grid ──
    const double fs = 56e6, shift = 13.123457e6;
    {
        const auto x = tones(fs, 0.4, { { shift + 300e3, 0.5 } });
        const auto y = runBank(fs, shift, x);
        double pt = 0, pr = 0; toneSnr(y, 300e3, pt, pr);
        const double snr = 10 * std::log10(pt / pr);
        ok(y.size() / 2 > size_t(0.38 * DabDdc::kOutHz), "the output is 2.048 MS/s (" + std::to_string(y.size() / 2) + " samples from 0.4 s)");
        ok(snr > 55, "★ a tone in the block comes out pure at its own frequency across " + std::to_string(int(0.4 * fs / bank::plan(fs).H)) + " hops: " + std::to_string(snr) + " dB");
        ok(std::fabs(10 * std::log10(pt / 0.25)) < 0.5, "  …at unity gain (" + std::to_string(10 * std::log10(pt / 0.25)) + " dB)");
    }
    // ── a neighbour block's carrier ──
    {
        const auto x = tones(fs, 0.3, { { shift + 1712e3, 0.5 } });
        const double pb = powerOf(runBank(fs, shift, x)), pd = powerOf(runDdc(fs, shift, x));
        const double rb = 10 * std::log10(std::max(1e-30, pb) / 0.25), rd = 10 * std::log10(std::max(1e-30, pd) / 0.25);
        ok(rb < -60, "★ the next block's carrier (1.712 MHz away) rejected: bank " + std::to_string(rb) + " dB (DDC " + std::to_string(rd) + " dB)");
    }
    // ── in-band content: the bank's output IS the DDC's — per tone, the same amplitude, and a phase that differs only by
    //    a pure delay (a straight line in frequency: the two front ends' filters delay by different fractions of a sample) ──
    for (double rate : { 8e6, 56e6 }) {
        const double sh = rate > 10e6 ? -17.333e6 : 1.712e6;
        std::vector<std::pair<double, double>> f; std::mt19937 rng(7); std::uniform_real_distribution<double> u(-740e3, 740e3);
        for (int i = 0; i < 40; ++i) f.push_back({ sh + u(rng), 0.05 });
        const auto x = tones(rate, 0.25, f, 3);
        const auto a = runBank(rate, sh, x), b = runDdc(rate, sh, x);
        const size_t n = std::min(a.size(), b.size()) / 2, s0 = n / 3, len = n / 2;
        std::vector<double> fr, dph; double worstDb = 0;
        for (const auto& t : f) {
            const double hz = t.first - sh, w = -2 * M_PI * hz / DabDdc::kOutHz;
            C pa = 0, pb = 0;
            for (size_t i = s0; i < s0 + len; ++i) { const C e = std::polar(1.0, w * double(i)); pa += C(a[2 * i], a[2 * i + 1]) * e; pb += C(b[2 * i], b[2 * i + 1]) * e; }
            worstDb = std::max(worstDb, std::fabs(20 * std::log10(std::abs(pa) / std::abs(pb))));
            fr.push_back(hz); dph.push_back(std::arg(pa / pb));
        }
        // the delay τ (samples) that best aligns them: max |Σ e^{j(dph - 2π f τ / Fs)}|; then each tone's residual phase
        double bestT = 0, bestM = -1;
        for (double tau = -40; tau <= 40; tau += 0.005) {
            C acc = 0; for (size_t i = 0; i < fr.size(); ++i) acc += std::polar(1.0, dph[i] + 2 * M_PI * fr[i] * tau / DabDdc::kOutHz);
            if (std::abs(acc) > bestM) { bestM = std::abs(acc); bestT = tau; }
        }
        C c0 = 0; for (size_t i = 0; i < fr.size(); ++i) c0 += std::polar(1.0, dph[i] + 2 * M_PI * fr[i] * bestT / DabDdc::kOutHz);
        double worstPh = 0;
        for (size_t i = 0; i < fr.size(); ++i) worstPh = std::max(worstPh, std::fabs(std::arg(std::polar(1.0, dph[i] + 2 * M_PI * fr[i] * bestT / DabDdc::kOutHz) / (c0 / std::abs(c0)))));
        const double k = -2 * M_PI * bestT / DabDdc::kOutHz;
        const double delaySamples = -k / (2 * M_PI) * DabDdc::kOutHz;
        ok(worstDb < 0.1 && worstPh < 0.02, "★ " + std::to_string(int(rate / 1e6)) + " MS/s: in-band, the bank's output is the DDC's — amplitude within "
           + std::to_string(worstDb) + " dB, phase within " + std::to_string(worstPh) + " rad of a pure delay (" + std::to_string(delaySamples) + " samples)");
    }
    // ── the runner: its own threads, 26 outputs at 56 MS/s, every sink fed, the queue never holding the reader ──
    {
        const double secs = 0.6;
        const int nb = 26;
        std::vector<std::pair<double, double>> ts;
        for (int k = 0; k < nb; ++k) ts.push_back({ -0.42 * fs + 0.8e6 + k * 1.712e6 + 200e3, 0.03 });
        const auto x = tones(fs, secs, ts, 11);
        bank::Runner r(fs, 3);
        std::vector<std::vector<float>> got(nb);
        std::mutex gm;
        std::vector<bank::Runner::Out> outs;
        for (int k = 0; k < nb; ++k) outs.push_back({ k, -0.42 * fs + 0.8e6 + k * 1.712e6, [&, k](const float* y, size_t n, size_t) { std::lock_guard<std::mutex> lk(gm); got[size_t(k)].insert(got[size_t(k)].end(), y, y + 2 * n); } });
        r.setOutputs(outs);
        const auto t0 = std::chrono::steady_clock::now();
        const size_t blk = 262144;
        // ★ at the radio's own pace (real time): the runner must keep up with nothing dropped
        for (size_t i = 0; i < x.size() / 2; i += blk) { r.feed(x.data() + 2 * i, std::min(blk, x.size() / 2 - i)); std::this_thread::sleep_until(t0 + std::chrono::microseconds((long long)(1e6 * double(i + blk) / fs))); }
        for (int w = 0; w < 400 && r.blocks() + r.dropped() / blk < (x.size() / 2 + blk - 1) / blk; ++w) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        size_t minN = SIZE_MAX; for (auto& g : got) minN = std::min(minN, g.size() / 2);
        ok(minN > size_t(0.55 * DabDdc::kOutHz), "★ the runner (3 workers): all 26 outputs fed, each ≥ " + std::to_string(minN) + " samples from 0.6 s at 56 MS/s");
        ok(r.dropped() == 0, "…nothing dropped, fed at the radio's pace (dropped " + std::to_string(r.dropped()) + ")");
        double pt = 0, pr = 0; toneSnr(got[13], 200e3, pt, pr);
        ok(10 * std::log10(pt / pr) > 40, "…and a block's tone is pure through the threads (" + std::to_string(10 * std::log10(pt / pr)) + " dB, 25 neighbours present)");
        std::printf("  runner 56 MS/s × 26 blocks, 3 workers + the bank thread: kept up in real time (%.2f s for %.1f s of capture)\n", wall, secs);
    }
    // ── the costs (for the record; the benchmark measures them on the box) ──
    for (double rate : { 8e6, 16e6, 32e6, 56e6 }) {
        const double secs = rate > 30e6 ? 0.5 : 1.0;
        const auto x = tones(rate, secs, { { 1e6, 0.3 } }, 9);
        const bank::Plan p = bank::plan(rate);
        const int nBlocks = std::max(1, int((rate * 0.84 - 1.6e6) / 1.712e6));   // ~ every block a capture this wide holds
        auto timeIt = [&](auto&& fn) { const auto t0 = std::chrono::steady_clock::now(); fn(); return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / secs * 100.0; };
        const double fwd = timeIt([&] { bank::Bank b; b.configure(rate); std::vector<std::vector<float>> o; for (size_t i = 0; i < x.size() / 2; i += 65536) b.process(x.data() + 2 * i, std::min<size_t>(65536, x.size() / 2 - i), o); });
        const double all = timeIt([&] { bank::Bank b; b.configure(rate);
            for (int k = 0; k < nBlocks; ++k) b.setSlice(size_t(k), -0.42 * rate + 0.8e6 + k * 1.712e6);
            std::vector<std::vector<float>> o(static_cast<size_t>(nBlocks) + 0); for (size_t i = 0; i < x.size() / 2; i += 65536) { b.process(x.data() + 2 * i, std::min<size_t>(65536, x.size() / 2 - i), o); for (auto& v : o) v.clear(); } });
        const double ddc1 = timeIt([&] { DabDdc d; d.configure(rate, 1e6); std::vector<float> o; for (size_t i = 0; i < x.size() / 2; i += 65536) { d.process(x.data() + 2 * i, std::min<size_t>(65536, x.size() / 2 - i), o); o.clear(); } });
        std::printf("  cost %2d MS/s (N %d): bank forward %.1f %% of a core; bank + %d blocks %.1f %% (%.2f %% each); one DDC %.1f %% (x%d = %.0f %%)\n",
                    int(rate / 1e6), p.N, fwd, nBlocks, all, (all - fwd) / nBlocks, ddc1, nBlocks, ddc1 * nBlocks);
    }
    std::printf("%s  test-dab-bank: %d passed, %d failed\n", fails ? "FAIL" : "ok", passes, fails);
    return fails ? 1 : 0;
}
