// test-sdrplay-seq.cpp — the RSP's inferred sample-loss counter (SampleSeqWatch, sdrplay_source.h).
//
// ★★ WHY A TEST FOR A DIAGNOSTIC. The counter is inferred from `firstSampleNum`, whose step per
//    delivered sample is LEARNED from the stream rather than assumed. The failure that matters is
//    a misunderstanding reported as drops: a box that shows "12 000 samples lost" when nothing was
//    lost sends the next person hunting a fault that does not exist. So the cases below are mostly
//    the ones that must NOT count: learning, a reset, a rate change, a counter that wraps, and a
//    stream whose numbering fits no whole step at all.
//
//   g++ -std=c++17 -I android/app/src/main/cpp vibeserver/test-sdrplay-seq.cpp -o /tmp/t && /tmp/t
#include "sdrplay_source.h"
#include <cstdio>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); ++fails; } } while (0)

// Feed `n` contiguous buffers of `num` samples with counter step `k`, starting at `first`.
static uint64_t run(vibe::SampleSeqWatch& w, uint32_t& first, uint32_t num, uint32_t k, int n) {
    uint64_t lost = 0;
    for (int i = 0; i < n; ++i) { lost += w.step(first, num, false); first += k * num; }
    return lost;
}

int main() {
    std::printf("SampleSeqWatch — the RSP's inferred IQ loss\n");

    for (uint32_t k : {1u, 2u, 4u}) {
        vibe::SampleSeqWatch w;
        uint32_t f = 1000;
        const uint64_t lost = run(w, f, 1008, k, 40);
        CHECK(lost == 0, "k=%u: a contiguous stream reported %llu lost", k, (unsigned long long)lost);
        CHECK(w.ratio() == k, "k=%u: learned ratio %u", k, w.ratio());
        // A real hole: skip 3 buffers' worth.
        f += k * 1008 * 3;
        const uint64_t miss = w.step(f, 1008, false);
        CHECK(miss == 3 * 1008, "k=%u: a 3-buffer hole read as %llu samples", k, (unsigned long long)miss);
        f += k * 1008;
        CHECK(run(w, f, 1008, k, 10) == 0, "k=%u: the stream after the hole was not clean", k);
        CHECK(w.irregular() == 0, "k=%u: %llu irregular jumps on a clean stream", k, (unsigned long long)w.irregular());
    }

    {   // ★ Nothing may be counted while the ratio is still being learned.
        vibe::SampleSeqWatch w;
        uint32_t f = 0;
        run(w, f, 504, 1, 3);
        f += 504 * 50;                                   // a jump during learning
        CHECK(w.step(f, 504, false) == 0, "a jump during learning was counted");
    }

    {   // ★ The counter wraps at 2^32 — unsigned arithmetic must make that seamless.
        vibe::SampleSeqWatch w;
        uint32_t f = 0xFFFFFFFFu - 1008u * 20u;
        CHECK(run(w, f, 1008, 1, 40) == 0, "the 32-bit wrap was reported as a loss");
    }

    {   // ★ `reset` (and a rate change, which the source maps onto it) is not a hole.
        vibe::SampleSeqWatch w;
        uint32_t f = 5000;
        run(w, f, 1008, 1, 20);
        CHECK(w.step(123, 1008, true) == 0, "a reset was counted");
        CHECK(w.ratio() == 0, "the ratio survived a reset — it must be learned again");
    }

    {   // ★★ Numbering that fits no whole step must read as IRREGULAR, never as drops.
        vibe::SampleSeqWatch w;
        uint32_t f = 0;
        run(w, f, 1000, 2, 20);
        const uint64_t miss = w.step(f + 7, 1000, false); // 2*1000 + 7: not a whole number of samples
        CHECK(miss == 0, "an irregular jump was counted as %llu lost samples", (unsigned long long)miss);
        CHECK(w.irregular() == 1, "the irregular jump was not recorded (%llu)", (unsigned long long)w.irregular());
        CHECK(w.ratio() == 0, "the ratio was kept after an irregular jump — it must be re-learned");
    }

    {   // ★ A counter that goes BACKWARDS (a restart the API did not flag) is irregular, not 4 G lost.
        vibe::SampleSeqWatch w;
        uint32_t f = 100000;
        run(w, f, 1000, 1, 20);
        CHECK(w.step(50, 1000, false) == 0, "a backwards counter was counted as a loss");
    }

    {   // ★ Buffer size may change between callbacks: the expected step is the PREVIOUS buffer's.
        vibe::SampleSeqWatch w;
        uint32_t f = 0;
        run(w, f, 1000, 1, 20);
        CHECK(w.step(f, 500, false) == 0, "size change: first short buffer");
        f += 500;
        CHECK(w.step(f, 1000, false) == 0, "size change: back to full size");
    }

    if (fails) { std::printf("✗ %d check(s) failed\n", fails); return 1; }
    std::printf("ok   SampleSeqWatch: learns its step, counts real holes, ignores resets, wraps and noise\n");
    return 0;
}
