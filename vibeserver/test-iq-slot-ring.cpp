// test-iq-slot-ring.cpp — the over-buffer between a radio's receive thread and the DSP (vibe_iq_slot_ring.h).
// Two real threads: a producer at a steady block rate (the radio), a consumer that stalls now and then (the DSP).
//   · every block arrives, in order, with its samples intact, while the stalls fit inside the ring
//   · a radio overflow arrives with the block AFTER it — never earlier, never lost
//   · a DSP a whole ring behind drops blocks (counted) and the next block carries the hole; the producer never waits
#include "vibe_iq_slot_ring.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

static int fails = 0;
static void ok(bool c, const char* what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what); if (!c) ++fails; }
using clk = std::chrono::steady_clock;

struct Got { uint32_t seq; uint64_t holes; bool intact; };

/** The radio makes `blocks` blocks of `n` samples, one every `periodUs`; block i's samples all equal i. Overflows are
 *  noted before the blocks in `ovBefore`. The DSP stalls `stallMs` every `stallEvery` blocks. */
static std::vector<Got> run(size_t slots, int blocks, int periodUs, int stallEvery, int stallMs,
                            const std::vector<int>& ovBefore, double& worstProducerUs, uint64_t& fullDrops) {
    const size_t n = 2048;
    vibe::IqSlotRing ring; ring.init(slots, n * 2);
    std::atomic<bool> run{true};
    std::vector<Got> got;
    std::thread dsp([&] {
        const float* d; size_t m; uint64_t h; int k = 0;
        while (run.load() || ring.waiting()) {
            if (!ring.pop(d, m, h, 50)) continue;
            bool same = m == n;
            for (size_t i = 0; same && i < m * 2; ++i) same = d[i] == d[0];
            got.push_back({ (uint32_t)d[0], h, same });
            ring.release();
            if (stallEvery && ++k % stallEvery == 0) std::this_thread::sleep_for(std::chrono::milliseconds(stallMs));
        }
    });
    worstProducerUs = 0;
    auto next = clk::now();
    for (int b = 0; b < blocks; ++b) {
        for (int o : ovBefore) if (o == b) ring.noteHole();
        const auto t0 = clk::now();
        float* p = ring.acquire();
        for (size_t i = 0; i < n * 2; ++i) p[i] = float(b);
        ring.commit(n, 0);
        worstProducerUs = std::max(worstProducerUs, std::chrono::duration<double, std::micro>(clk::now() - t0).count());
        next += std::chrono::microseconds(periodUs);
        std::this_thread::sleep_until(next);
    }
    run.store(false); ring.stop(); dsp.join();
    fullDrops = ring.fullDrops();
    return got;
}

int main() {
    std::printf("test-iq-slot-ring\n");
    double worst = 0; uint64_t full = 0;

    // 20 slots of 1 ms (the B210's ~100 ms at its block size, scaled), DSP stalling 8 ms every 30 blocks
    auto g = run(20, 400, 1000, 30, 8, { 100, 250 }, worst, full);
    bool inOrder = g.size() == 400, intact = true;
    for (size_t i = 0; i < g.size(); ++i) { if (g[i].seq != i) inOrder = false; if (!g[i].intact) intact = false; }
    ok(inOrder, "★ every block arrives, in order, through 8 ms DSP stalls (ring holds 20 ms)");
    ok(intact, "and every block's samples are its own (filled in place, never overwritten while in hand)");
    ok(full == 0, "no block dropped while the stalls fit inside the ring");
    ok(g.size() == 400 && g[100].holes == 1 && g[250].holes == 1, "★ each radio overflow arrives with the block after it");
    uint64_t total = 0; for (auto& x : g) total += x.holes;
    ok(total == 2, "and nowhere else");
    char msg[120]; std::snprintf(msg, sizeof msg, "the radio's thread never waits on the DSP (worst hand-in %.0f µs)", worst);
    ok(worst < 2000.0, msg);

    // a DSP far behind: 4 slots, stalling 30 ms every 5 blocks
    auto g2 = run(4, 200, 1000, 5, 30, {}, worst, full);
    bool ordered = true; for (size_t i = 1; i < g2.size(); ++i) if (g2[i].seq <= g2[i - 1].seq) ordered = false;
    std::snprintf(msg, sizeof msg, "a DSP a whole ring behind drops blocks, counted (%llu dropped, %zu delivered)",
                  (unsigned long long)full, g2.size());
    ok(full > 0 && g2.size() + full == 200, msg);
    bool marked = true;
    for (size_t i = 1; i < g2.size(); ++i) if (g2[i].seq != g2[i - 1].seq + 1 && g2[i].holes == 0) marked = false;
    ok(ordered && marked, "★ what is delivered stays in order, and every gap is marked on the block after it");
    std::snprintf(msg, sizeof msg, "and the radio's thread still never waits (worst %.0f µs)", worst);
    ok(worst < 2000.0, msg);

    std::printf(fails ? "  %d FAILED\n" : "  all passed\n", fails);
    return fails ? 1 : 0;
}
