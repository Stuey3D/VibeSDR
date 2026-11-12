// test-dab-clockwalk.cpp — a radio whose SAMPLE CLOCK is a few ppm out must hold a DAB lock for hours, not ~3 h ± luck.
//
// ★★★ THE FAULT (2026-11-12, MTVNL 7D on a tester's B210, no GPSDO, 4.02 ppm): "clear as day until suddenly it broke up
//     and stopped; tuned down then up again and it's working again". A clock that runs SLOW gives fewer samples per
//     frame of air, so the null walks towards the front of the decode window by 0.8 samples a frame (at 4 ppm). The
//     worker advanced exactly one frame per push and the tracker could not look behind the window's start: it pinned at
//     sample 0 while the real null kept walking, the FFT windows slid out of the guard interval, the audio broke up
//     while the FIC still read, then the FIC died. A retune re-acquired from scratch. Found by replaying the bench clip
//     through a 4 ppm resampler (dab-weak --ppm -4 --trace 1): frameAt pinned at 0, irPeak -8 samples a second, MER
//     33 → 7 dB, re-acquired only when the FIC had been dead for 1.5 s.
//
// ★★ HOW THIS TEST WALKS THE CLOCK WITHOUT A RESAMPLER: the null symbol is silence, so taking samples out of it (or
//    adding them) moves every later frame exactly as a clock error does — with no inter-carrier interference, so the only
//    thing that can go wrong is the tracking. 8 samples a frame is a 40 ppm clock; the shipped bug pins within 1 frame of
//    the window's front and is ~250 samples out of the guard's centre 32 frames later.
//
//    The receiver is driven exactly as DabService::workerLoop drives it: a two-frame window, then advanceAfterPush().
//    The OLD loop (a fixed one-frame advance) is run too and must show the fault — or this test proves nothing.
#include "vibe_dab_receiver.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <vector>

using namespace vibedab;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); ++fails; } else printf("  ok: %s\n", msg); } while (0)
static uint32_t seed = 4021; static uint32_t rnd() { seed = seed * 1664525u + 1013904223u; return seed; }

/** One Mode I frame: null, phase reference, 75 symbols of random DQPSK. A twiddle table, not a trig call per term. */
static std::vector<Cplx> makeFrame() {
    const Mode& m = modeI();
    const int N = m.usefulSamples, G = m.guardSamples, K = m.carriers;
    std::vector<std::vector<C32>> syms;
    std::vector<C32> ref(static_cast<size_t>(K), C32{}); prsSymbol(ref.data()); syms.push_back(ref);
    static const C32 rot[4] = { {0.7071f, 0.7071f}, {-0.7071f, 0.7071f}, {-0.7071f, -0.7071f}, {0.7071f, -0.7071f} };
    for (int s = 1; s < m.symbolsPerFrame; ++s) {
        std::vector<C32> cur(size_t(K), C32{});
        for (int k = 0; k < K; ++k) cur[size_t(k)] = syms.back()[size_t(k)] * rot[rnd() & 3];
        syms.push_back(cur);
    }
    std::vector<double> twr(static_cast<size_t>(N)), twi(static_cast<size_t>(N));
    for (int n = 0; n < N; ++n) { twr[size_t(n)] = std::cos(2 * M_PI * n / N); twi[size_t(n)] = std::sin(2 * M_PI * n / N); }
    std::vector<Cplx> out(size_t(m.nullSamples), Cplx{0.0f, 0.0f});
    std::vector<Cplx> useful(static_cast<size_t>(N));
    for (const auto& c : syms) {
        for (int t = 0; t < N; ++t) {
            double re = 0, im = 0; int j = 0;
            for (int k = -K / 2; k <= K / 2; ++k) {
                if (k == 0) continue;
                const size_t w = size_t(((long(k) * t) % N + N) % N);
                const C32 v = c[size_t(j++)];
                re += double(v.real()) * twr[w] - double(v.imag()) * twi[w];
                im += double(v.real()) * twi[w] + double(v.imag()) * twr[w];
            }
            useful[size_t(t)] = { float(re / N), float(im / N) };
        }
        for (int i = 0; i < G; ++i) out.push_back(useful[size_t(N - G + i)]);
        out.insert(out.end(), useful.begin(), useful.end());
    }
    return out;
}

struct Run { int frames = 0, decoded = 0, worstIr = 0, recentres = 0, reacq = 0, dup = 0, skip = 0; };

/** `walk` samples taken out of every null (negative = added): a clock -walk/196608 out. `fixedAdvance` = the old loop. */
static Run drive(const std::vector<Cplx>& frame, int walk, int nFrames, bool fixedAdvance, int startAt) {
    const Mode& m = modeI();
    const size_t F = size_t(m.frameSamples);
    std::vector<Cplx> air;
    air.reserve(F * size_t(nFrames + 3));
    air.insert(air.end(), frame.begin() + startAt, frame.end());   // ★ the first lock lands `F - startAt` into the window
    for (int f = 0; f < nFrames + 2; ++f) {
        const long nullLen = long(m.nullSamples) - walk;
        air.insert(air.end(), size_t(nullLen), Cplx{0.0f, 0.0f});
        air.insert(air.end(), frame.begin() + m.nullSamples, frame.end());
    }
    Run r; r.frames = nFrames + 2;   // the whole frames after the (partial) first
    DabReceiver rx;
    size_t head = 0; long lastAbs = -1;
    while (head + 2 * F <= air.size()) {
        if (rx.push(air.data() + head, 2 * F)) {
            ++r.decoded;
            // ★ where this frame's null is in the AIR: one frame on from the last, never the same one twice, never two on
            const long abs = long(head) + rx.lastFrameStart();
            if (lastAbs >= 0) { const long d = abs - lastAbs; if (d < long(F) / 2) ++r.dup; else if (d > long(F) * 3 / 2) ++r.skip; }
            lastAbs = abs;
            // ★ only once the tracker is settled: the first frames are acquisition
            if (r.decoded > 2) r.worstIr = std::max(r.worstIr, std::abs(rx.stats().irPeakSamples));
        }
        const size_t adv = fixedAdvance ? F : rx.advanceAfterPush(F);
        if (adv != F) ++r.recentres;
        rx.syncConsumed(adv);
        head += adv;
    }
    r.reacq = rx.stats().reacquires;
    return r;
}

int main() {
    printf("test-dab-clockwalk\n");
    const auto frame = makeFrame();
    char msg[200];

    // ── the shipped loop must show the fault, or the rest proves nothing ──────────────────────────────
    {
        const Run r = drive(frame, 8, 60, true, 0);
        snprintf(msg, sizeof msg, "★ the OLD fixed advance pins: a 40 ppm slow clock walks the null %d samples off the window (> 200)", r.worstIr);
        CHECK(r.worstIr > 200, msg);
    }
    // ── ★★★ a SLOW clock: the window re-centres, the null stays where the tracker is looking ────────────
    for (int walk : { 1, 8, 20 }) {
        const int n = walk == 1 ? 120 : 400;
        const Run r = drive(frame, walk, n, false, 0);
        snprintf(msg, sizeof msg, "slow clock (%d samples a frame, %d frames): null within 64 samples of the window (worst %d), %d re-centres",
                 walk, n, r.worstIr, r.recentres);
        CHECK(r.worstIr <= 64 && r.recentres >= 1, msg);
        // ★ nothing skipped, nothing read twice: every frame transmitted is decoded once (the first may be the acquisition's)
        snprintf(msg, sizeof msg, "  and every frame decoded exactly once (%d decoded for %d sent; %d read twice, %d skipped)",
                 r.decoded, r.frames, r.dup, r.skip);
        CHECK(r.dup == 0 && r.skip == 0 && r.decoded >= r.frames - 1, msg);
    }
    // ── a FAST clock walks the other way: past the window's end, one push declines, the next window finds it ──
    for (int startAt : { 0, 1000 }) {
        const Run r = drive(frame, -20, 400, false, startAt);
        snprintf(msg, sizeof msg, "fast clock (20 samples a frame, start %d): worst %d samples, %d decoded for %d sent, %d twice, %d skipped",
                 startAt, r.worstIr, r.decoded, r.frames, r.dup, r.skip);
        CHECK(r.worstIr <= 64 && r.dup == 0 && r.skip == 0 && r.decoded >= r.frames - 1, msg);
    }
    // ── a perfect clock never re-centres after the first lock settles mid-window ───────────────────────
    {
        const Run r = drive(frame, 0, 100, false, 100000);
        snprintf(msg, sizeof msg, "a perfect clock locked mid-window never re-centres (%d) and decodes every frame (%d of %d)",
                 r.recentres, r.decoded, r.frames);
        CHECK(r.recentres == 0 && r.decoded >= r.frames - 1, msg);
    }

    if (fails == 0) printf("  all passed\n");
    else            printf("  %d FAILED\n", fails);
    return fails ? 1 : 0;
}
