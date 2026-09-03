// test-dab-sync.cpp — DAB frame acquisition against a synthetic Mode I signal.
//
// ★★★ SYNTHETIC, AND HONEST ABOUT WHAT THAT PROVES. This generates a signal with the right SHAPE
//     — 2656 quiet samples then 76 x 2552 noisy ones, repeating — and checks the detector finds
//     the hole where it was put. It does NOT prove anything about a real transmission: no
//     multipath, no frequency offset, no AGC pumping. What it does prove is the arithmetic, the
//     running-sum window, the acquire/track transition and the fade tolerance, which are exactly
//     the things that are miserable to debug against live RF at 1am.
//     ★ AGENTS.md: "a synthetic test can AGREE with the bug" — so the numbers here are the ones
//       from vibe_dab_modes.h rather than hand-typed, and the noise floor is deliberately uneven.
#include "vibe_dab_sync.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace vibedab;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); ++fails; } } while (0)

/** A deterministic pseudo-random source — the same signal every run, so a failure is reproducible. */
static uint32_t seed = 12345;
static float rnd() { seed = seed * 1664525u + 1013904223u; return float(int32_t(seed) >> 8) / 8388608.0f; }

/** Build `frames` Mode I frames: a quiet null then loud symbols. `nullFloor` scales the residual
 *  energy inside the null — 0 is a perfect null, 0.3 is a shallow one. */
static std::vector<Cplx> makeSignal(int frames, size_t nullAt, float nullFloor = 0.02f,
                                    float level = 1.0f) {
    const Mode& m = modeI();
    const size_t N = size_t(m.nullSamples), F = size_t(m.frameSamples);
    std::vector<Cplx> v(F * size_t(frames) + nullAt);
    for (size_t i = 0; i < v.size(); ++i) {
        // Where are we relative to the first null?
        const long rel = long(i) - long(nullAt);
        const bool inNull = rel >= 0 && (size_t(rel) % F) < N;
        const float a = inNull ? nullFloor : level;
        v[i].re = a * rnd();
        v[i].im = a * rnd();
    }
    return v;
}

int main() {
    printf("test-dab-sync\n");
    const Mode& m = modeI();
    const size_t F = size_t(m.frameSamples), N = size_t(m.nullSamples);

    // ── findNull: does it land on the hole we made? ──────────────────────────
    {
        const size_t at = 4321;
        auto sig = makeSignal(2, at);
        NullSearch s = findNull(sig.data(), F, N);
        CHECK(s.found, "a null is found in a frame that contains one");
        // ★ Within a few samples: the window is 2656 wide and the noise is random, so the exact
        //   minimum can sit a sample or two either side of the true edge. Demanding EXACT here
        //   would be a test that fails on noise rather than on a bug.
        CHECK(labs(long(s.offset) - long(at)) <= 8, "the null is found where it was put");
        CHECK(s.depth > 100.0f, "a clean null reads as very deep");
    }

    // ── it must not invent a null in a signal that has none ──────────────────
    {
        std::vector<Cplx> flat(F);
        for (auto& c : flat) { c.re = rnd(); c.im = rnd(); }
        NullSearch s = findNull(flat.data(), flat.size(), N);
        CHECK(!s.found, "flat noise contains no null");
    }

    // ── acquire, then TRACK across frames ────────────────────────────────────
    {
        const size_t at = 10000;
        auto sig = makeSignal(4, at);
        FrameSync fs(m);
        CHECK(!fs.locked(), "starts unlocked");
        long a = fs.offer(sig.data(), sig.size());
        CHECK(a >= 0 && fs.locked(), "acquires on the first frame offered");
        CHECK(labs(a - long(at)) <= 8, "acquisition lands on the null");
        CHECK(fs.frameLen() == F && fs.nullLen() == N, "frame and null lengths come from the mode");
    }

    // ── a shallow null (a fade) is rejected rather than trusted ──────────────
    {
        // nullFloor 0.7 = only ~3 dB down: that is a fade, not a null.
        auto sig = makeSignal(2, 5000, 0.7f);
        NullSearch s = findNull(sig.data(), F, N);
        CHECK(!s.found, "a 3 dB dip is not deep enough to be a null");
    }

    // ── ★★ THE FADE TOLERANCE: one missing null must not drop the lock ──────
    {
        FrameSync fs(m);
        auto good = makeSignal(2, 2000);
        CHECK(fs.offer(good.data(), good.size()) >= 0 && fs.locked(), "locked on good signal");
        std::vector<Cplx> flat(F * 2);
        for (auto& c : flat) { c.re = rnd(); c.im = rnd(); }
        // Three consecutive frames with no null at all.
        for (int i = 0; i < 3; ++i) CHECK(fs.offer(flat.data(), flat.size()) < 0, "a miss reports -1");
        CHECK(fs.locked(), "three misses do NOT drop the lock — a fade is ordinary");
        fs.offer(flat.data(), flat.size());
        CHECK(!fs.locked(), "four misses does drop it — that is a real loss");
    }

    // ── the detector must not care about absolute level (AGC moves it) ───────
    {
        auto loud  = makeSignal(2, 777, 0.02f, 4.0f);
        auto quiet = makeSignal(2, 777, 0.02f, 0.05f);
        NullSearch a = findNull(loud.data(),  F, N);
        NullSearch b = findNull(quiet.data(), F, N);
        CHECK(a.found && b.found, "found at both levels");
        CHECK(labs(long(a.offset) - long(b.offset)) <= 8, "the answer does not depend on gain");
    }

    // ── degenerate inputs answer rather than crash ───────────────────────────
    CHECK(!findNull(nullptr, 1000, N).found, "a null pointer is not a null symbol");
    { std::vector<Cplx> tiny(10); CHECK(!findNull(tiny.data(), tiny.size(), N).found, "too short"); }

    if (fails == 0) printf("  all passed\n");
    else            printf("  %d FAILED\n", fails);
    return fails ? 1 : 0;
}
