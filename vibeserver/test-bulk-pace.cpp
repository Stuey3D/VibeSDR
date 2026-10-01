// ★★ THE MAP'S SHARED BYTE BUDGET (vibe_bulk_pace.h) — a zoom-out's burst of tile reads must not
//    leave the receiver faster than the budget, however many connections it arrives on, while a
//    single small fetch still goes at once ("zooming out of the map caused a tiny stutter",
//    Stuart, 2026-10-01). Driven on a synthetic clock, so it says the same thing on any machine.
#include "vibe_bulk_pace.h"
#include <cmath>
#include <cstdio>
#include <string>

static int fails = 0;
static void ok(bool c, const std::string& w) {
  std::printf("  %s %s\n", c ? "\033[32mok\033[0m  " : "\033[31mFAIL\033[0m", w.c_str()); if (!c) fails++;
}
using clk = std::chrono::steady_clock;

int main() {
  std::printf("bulk pacing — one budget for every map connection\n");
  {
    vibebulk::Bucket b(100.0 * 1024, 16.0 * 1024);          // 100 KB/s, 16 KB burst
    const auto t0 = b.last;
    ok(b.charge(16 * 1024, t0) == 0, "a fetch inside the burst goes at once (the style, a glyph range, a tile)");
    const double w1 = b.charge(16 * 1024, t0);
    ok(std::fabs(w1 - 0.16) < 1e-6, "the next 16 KB at the same instant waits its share: 0.16 s");
    // ★ A SECOND CONNECTION does not get its own bucket — that is the whole point.
    const double w2 = b.charge(16 * 1024, t0);
    ok(std::fabs(w2 - 0.32) < 1e-6, "★★ a second connection queues BEHIND the first (0.32 s), not beside it");
    // Twenty connections of one zoom, 48 KB each, all at once: the last finishes at total/rate.
    vibebulk::Bucket z(512.0 * 1024, 128.0 * 1024);
    double worst = 0;
    for (int i = 0; i < 20; i++) worst = std::max(worst, z.charge(48 * 1024, z.last));
    const double expect = (20 * 48.0 - 128.0) / 512.0;
    ok(std::fabs(worst - expect) < 1e-6, "★★★ a 960 KB zoom on 20 connections leaves at the budget: last byte after "
                                         + std::to_string(worst).substr(0, 4) + " s, not at once");
  }
  {
    vibebulk::Bucket b(100.0 * 1024, 16.0 * 1024);
    const auto t0 = b.last;
    (void)b.charge(16 * 1024, t0);
    ok(b.charge(10 * 1024, t0 + std::chrono::milliseconds(100)) == 0, "the budget refills with time (100 ms = 10 KB)");
    ok(b.charge(64 * 1024, t0 + std::chrono::seconds(60)) > 0, "but never past the burst, however long it sat idle");
  }
  {
    vibebulk::Bucket off(0, 0);
    ok(off.charge(100 * 1024 * 1024, off.last) == 0, "rate 0 = unpaced (VIBESERVER_BULK_KBPS=0, for measurement)");
  }
  std::printf(fails ? "\n\033[31m%d FAILED\033[0m\n" : "\n\033[32mpassed\033[0m\n", fails);
  return fails ? 1 : 0;
}
