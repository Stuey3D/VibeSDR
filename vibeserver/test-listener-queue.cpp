// ★★★ A PER-VFO LISTENER'S BLOCK QUEUE HOLDS ~40 ms AT EVERY CAPTURE RATE (2026-10-07).
//
// The hand-off held four channelizer blocks. The channelizer's FFT is capped at 32768 points, so a
// block is a fixed number of samples and four of them were 41 ms on an RTL at 2.4 MS/s but 12 ms on
// an RSP at 8 MS/s — where the Pi 500's RSP1B dropped 10 blocks and its listener's audio stepped
// 3-6 ms each time. vibe_listener_queue.h sizes it in time. Pinned here:
//   1. every rate the servers run gets at least 40 ms of slack, and never fewer than four blocks;
//   2. the RTL's own figure is unchanged (it is the one proven on air);
//   3. a burst of blocks posted back to back — the DSP thread catching up — that the old cap threw
//      away at 8 MS/s is now absorbed, while a listener that is genuinely stuck still drops.
// ★ Header-only, no device, no clock.
#include "vibe_listener_queue.h"

#include <cstdio>
#include <deque>

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("   %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

static int fftSizeForRate(double rate) {      // the shim's rule (local_sdr_shim.cpp)
    double want = rate / 75.0; int s = 4096;
    while (s < (int)want && s < 32768) s *= 2;
    return s;
}
static int hopFor(double rate) { const int n = fftSizeForRate(rate); return n - n / 4; }   // OVERLAP_DIV 4

/** Simulate the hand-off: `burst` blocks posted at once while the listener is busy; how many drop? */
static int dropsFor(double rate, int burst, size_t cap) {
    std::deque<int> q; int drops = 0;
    for (int i = 0; i < burst; ++i) { if (q.size() >= cap) { q.pop_front(); ++drops; } q.push_back(i); }
    return drops;
}

int main() {
    std::printf("── 1. ≥ 40 ms of slack at every rate, never fewer than 4 blocks ──\n");
    const double rates[] = { 250000, 912000, 1024000, 2000000, 2048000, 2400000, 2560000, 3000000,
                             3200000, 6000000, 8000000, 10000000, 20000000 };
    for (double r : rates) {
        const int hop = hopFor(r);
        const size_t n = vibe::listenerQueueBlocks(r, hop);
        const double ms = n * hop / r * 1000.0;
        char b[160];
        std::snprintf(b, sizeof b, "%5.2f MS/s: hop %5d (%5.2f ms) -> %2zu blocks = %5.1f ms", r / 1e6, hop, hop / r * 1e3, n, ms);
        const double blockMs = hop / r * 1000.0;
        // At least 40 ms; and no more than one block past it, unless the floor of four set it.
        check(n >= 4 && (ms >= 39.9 || n == 64) && (n == 4 || ms < 40.0 + blockMs + 1e-6), b);
    }

    std::printf("── 2. the RTL keeps the four it was proven with ──\n");
    check(vibe::listenerQueueBlocks(2400000, hopFor(2400000)) == 4, "RTL 2.4 MS/s: 4 blocks (41 ms), as before");
    check(vibe::listenerQueueBlocks(2048000, hopFor(2048000)) == 4, "RTL 2.048 MS/s: 4 blocks (48 ms), as before");
    check(vibe::listenerQueueBlocks(0, 0) == 4 && vibe::listenerQueueBlocks(-1, 100) == 4, "nonsense in: the old four");

    std::printf("── 3. a catch-up burst at 8 MS/s ──\n");
    const size_t cap8 = vibe::listenerQueueBlocks(8000000, hopFor(8000000));
    char b[160];
    std::snprintf(b, sizeof b, "6 blocks back to back (18 ms): old cap 4 drops %d, now %zu drops %d",
                  dropsFor(8e6, 6, 4), cap8, dropsFor(8e6, 6, cap8));
    check(dropsFor(8e6, 6, 4) == 2 && dropsFor(8e6, 6, cap8) == 0, b);
    std::snprintf(b, sizeof b, "a stuck listener (100 blocks, 307 ms) still drops: %d", dropsFor(8e6, 100, cap8));
    check(dropsFor(8e6, 100, cap8) > 0, b);

    std::printf("   %s — %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
