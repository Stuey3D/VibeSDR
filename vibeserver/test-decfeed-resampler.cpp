// ★★★ THE DECODER FEED LOSES NOTHING AT A PACKET BOUNDARY (2026-10-07).
//
// Stuart's live JMH WEFAX off a Japanese Kiwi leaned ~2 px a line. The app upsamples a network
// backend's audio to the decoders' 48 kHz packet by packet, and the old loop dropped one input sample
// (and the fractional phase) per packet: 1/1024 of a Kiwi's audio → 1.77 px a WEFAX line. See
// vibe_decfeed_resampler.h.
//
// ★ THREE RULES:
//   1. IT COUNTS EXACTLY. Ten minutes at a Kiwi's fractional rate (12001.135 Hz), in Kiwi-sized and in
//      random packets, gives in·48000/rate output samples to within ONE INPUT SAMPLE (the outputs that
//      fall between the last sample and one not yet arrived wait for the next packet — a fixed lag), and
//      the shortfall at ten minutes equals the shortfall at one: NO DRIFT. The per-packet loop it
//      replaces is shown losing ~0.1 % on the same feed, so the test can fail.
//   2. A BOUNDARY IS INVISIBLE. A 1 kHz tone fed in packets comes out matching the same tone fed in
//      ONE packet sample for sample (so no click, no phase jump, at any boundary).
//   3. A RATE CHANGE RESTARTS CLEANLY and the count is exact again at the new rate.
// ★ SILENT: audio is counted, never played.
#include "vibe_decfeed_resampler.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("   %s   %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) fails++;
}

static std::vector<int16_t> tone(size_t n, double rate, double hz) {
    std::vector<int16_t> v(n);
    for (size_t i = 0; i < n; i++) v[i] = (int16_t)std::lrint(12000.0 * std::sin(2 * M_PI * hz * i / rate));
    return v;
}

// The loop this replaced, verbatim in behaviour: restarts at 0 and stops before the last sample.
static size_t oldCount(const std::vector<int16_t>& x, int pkt, int rate) {
    size_t out = 0;
    const double step = rate / 48000.0;
    for (size_t at = 0; at < x.size(); at += pkt) {
        const int n = (int)std::min<size_t>(pkt, x.size() - at);
        for (double s = 0; s < n - 1; s += step) out++;
    }
    return out;
}

int main() {
    const double kiwi = 12001.135;
    const size_t N = (size_t)(kiwi * 600);   // ten minutes
    const auto x = tone(N, kiwi, 1000.0);
    const double want = N * 48000.0 / kiwi;
    const double LAG = 48000.0 / kiwi + 1.0;   // one input sample's worth of outputs

    // 1. exact count — Kiwi-sized packets
    {
        vibe::DecFeedResampler r; std::vector<float> out; out.reserve((size_t)want + 16);
        for (size_t at = 0; at < N; at += 1024) r.push(&x[at], (int)std::min<size_t>(1024, N - at), kiwi, out);
        const double err = (double)out.size() - want;
        std::printf("   .. 10 min @ %.3f Hz in 1024-sample packets: %zu out, want %.1f (%+.2f)\n", kiwi, out.size(), want, err);
        check(std::fabs(err) <= LAG, "★ 1024-sample packets: output count exact to one input sample over ten minutes");
        // no drift: the same shortfall after one minute as after ten
        vibe::DecFeedResampler r1; std::vector<float> o1; const size_t N1 = (size_t)(kiwi * 60);
        for (size_t at = 0; at < N1; at += 1024) r1.push(&x[at], (int)std::min<size_t>(1024, N1 - at), kiwi, o1);
        const double err1 = (double)o1.size() - N1 * 48000.0 / kiwi;
        std::printf("   .. shortfall after 1 min %+.2f, after 10 min %+.2f\n", err1, err);
        check(std::fabs(err - err1) <= 1.0, "★ NO DRIFT: the shortfall after ten minutes equals the shortfall after one");
        const size_t old = oldCount(x, 1024, 12001);
        const double lost = (want - (double)old) / want * 100.0;
        std::printf("   .. the per-packet loop it replaces: %zu out — %.3f %% short (1.77 px a WEFAX line)\n", old, lost);
        check(lost > 0.08, "the OLD loop is measurably short on the same feed (this test can fail)");
    }
    // 1b. random packet sizes, including 1-sample packets
    {
        std::mt19937 rng(7); std::uniform_int_distribution<int> d(1, 3000);
        vibe::DecFeedResampler r; std::vector<float> out; out.reserve((size_t)want + 16);
        for (size_t at = 0; at < N;) { const int n = (int)std::min<size_t>(d(rng), N - at); r.push(&x[at], n, kiwi, out); at += n; }
        check(std::fabs((double)out.size() - want) <= LAG, "random packet sizes (1-3000): count still exact to one input sample");
    }
    // 2. boundary invisible: packets == one big push, sample for sample
    {
        const size_t M = 48000;
        vibe::DecFeedResampler a, b; std::vector<float> oa, ob;
        a.push(&x[0], (int)M, kiwi, oa);
        std::mt19937 rng(11); std::uniform_int_distribution<int> d(1, 900);
        for (size_t at = 0; at < M;) { const int n = (int)std::min<size_t>(d(rng), M - at); b.push(&x[at], n, kiwi, ob); at += n; }
        size_t k = std::min(oa.size(), ob.size()); double worst = 0;
        for (size_t i = 0; i < k; i++) worst = std::max(worst, (double)std::fabs(oa[i] - ob[i]));
        std::printf("   .. one push %zu vs packets %zu, worst sample difference %.2e\n", oa.size(), ob.size(), worst);
        check(oa.size() == ob.size() && worst < 1e-6, "★ a packet boundary is invisible: packets match one continuous push sample for sample");
    }
    // 3. rate change
    {
        vibe::DecFeedResampler r; std::vector<float> out;
        const auto y = tone(12000 * 10, 12000.0, 700.0);
        for (size_t at = 0; at < N / 60; at += 512) r.push(&x[at], (int)std::min<size_t>(512, N / 60 - at), kiwi, out);
        out.clear();
        for (size_t at = 0; at < y.size(); at += 512) r.push(&y[at], (int)std::min<size_t>(512, y.size() - at), 12000.0, out);
        check(std::fabs((double)out.size() - 480000.0) <= 48000.0 / 12000.0 + 1.0, "a rate change restarts cleanly: 10 s at 12000 Hz → 480000 out");
    }
    std::printf(fails ? "\n%d FAILED\n" : "\nall good\n", fails);
    return fails ? 1 : 0;
}
