// ★★★ WHEN MAY A DAB SERVICE LIST DELETE A BOOKMARK? (vibe_dab_confirm.h, V12 2026-10-26)
//     Stuart: "DAB multiplexes learn every time you tune to them so that a stale Radio X 90's cannot linger" — and then
//     "a weak multiplex causing issues, especially if one day you see all stations then another day you see hardly any
//     on the same multiplex". So: a list is CONFIRMED only after it has held, identical, at the quality bar, for several
//     samples over several seconds; and even a confirmed list removes a service only on a SECOND visit at least 6 h after
//     the first that lacked it, never on a visit missing more than half the saved list at once, never a hand-saved row.
//     Synthetic clock, no radio.
#include "vibe_dab_confirm.h"
#include <cstdio>
#include <string>

static int fails = 0;
static void ok(bool c, const std::string& w) {
  std::printf("  %s %s\n", c ? "\033[32mok\033[0m  " : "\033[31mFAIL\033[0m", w.c_str()); if (!c) fails++;
}
using namespace vibedab;

static std::set<uint32_t> range(uint32_t a, int n) { std::set<uint32_t> s; for (int i = 0; i < n; ++i) s.insert(a + i); return s; }

int main() {
  std::printf("the confirmation\n");
  {
    ServiceListConfirm c;
    const auto L = range(0xC000, 12);
    bool got = false; double at = -1;
    for (int i = 0; i <= 20 && !got; ++i) { got = c.feed(29, 0xC0DE, true, 0.98f, L, i * 0.5); if (got) at = i * 0.5; }
    ok(got && at >= ServiceListConfirm::kMinSecs, "an identical complete list at the bar is confirmed after " + std::to_string(at) + " s, not before 4 s");
    ok(c.feed(29, 0xC0DE, true, 0.98f, L, 11.0), "…and stays confirmed while every sample agrees");
    ok(!c.feed(29, 0xC0DE, true, 0.85f, L, 11.5), "one sample below the FIB bar breaks it");
    ok(!c.feed(29, 0xC0DE, true, 0.98f, L, 12.0) && !c.feed(29, 0xC0DE, true, 0.98f, L, 12.5), "…and the count starts again");
  }
  {
    ServiceListConfirm c;
    bool any = false;
    // ★ A flapping signal: the list keeps changing by one service — never confirmed, however long.
    for (int i = 0; i < 200; ++i) any |= c.feed(29, 0xC0DE, true, 0.95f, range(0xC000, 11 + (i % 2)), i * 0.5);
    ok(!any, "a list that keeps changing is never confirmed (100 s)");
  }
  {
    ServiceListConfirm c;
    bool any = false;
    for (int i = 0; i < 40; ++i) any |= c.feed(29, 0xC0DE, false, 0.99f, range(0xC000, 12), i * 0.5);
    ok(!any, "an MCI that is not complete is never confirmed");
    for (int i = 0; i < 40; ++i) any |= c.feed(29, 0, true, 0.99f, range(0xC000, 12), 20 + i * 0.5);
    ok(!any, "no ensemble id is never confirmed");
    for (int i = 0; i < 40; ++i) any |= c.feed(i % 2 ? 29 : 30, 0xC0DE, true, 0.99f, range(0xC000, 12), 40 + i * 0.5);
    ok(!any, "a block change restarts it");
    for (int i = 0; i < 40; ++i) any |= c.feed(29, i % 2 ? 0xC0DE : 0xC0DF, true, 0.99f, range(0xC000, 12), 60 + i * 0.5);
    ok(!any, "an ensemble-id change restarts it");
    c.reset();
    for (int i = 0; i < 3; ++i) any |= c.feed(29, 0xC0DE, true, 0.99f, range(0xC000, 12), 100 + i * 10.0);
    ok(!any, "three samples are not enough, however far apart");
  }

  std::printf("the two-visit removal\n");
  const long long day1 = 1'800'000'000LL, H = 3600;
  auto saved = [](int n, uint32_t base = 0xC000) { std::vector<ReconRow> r; for (int i = 0; i < n; ++i) r.push_back({ base + uint32_t(i), false, 0 }); return r; };
  {
    // A genuine swap: Radio X 90s (0xC00B) replaced by Radio X Classic Rock (0xC0FF); 11 unchanged.
    auto rows = saved(12);
    std::set<uint32_t> air = range(0xC000, 11); air.insert(0xC0FF);
    auto p = reconcilePlan(rows, air, day1);
    ok(!p.suspect && p.missing == 1, "a one-service swap is not suspect (1 of 12 missing)");
    ok(p.acts[11] == ReconAct::MarkMissed, "visit 1: the gone service is only MARKED");
    int removes = 0; for (auto a : p.acts) removes += a == ReconAct::Remove; ok(removes == 0, "visit 1 removes nothing");
    rows[11].missedAt = day1;
    p = reconcilePlan(rows, air, day1 + 2 * H);
    ok(p.acts[11] == ReconAct::Keep, "a visit 2 h later does not remove it (under 6 h)");
    p = reconcilePlan(rows, air, day1 + 6 * H);
    ok(p.acts[11] == ReconAct::Remove, "a visit 6 h later, still missing → REMOVED");
    // ★ A sighting between the visits clears the mark (the shim's learn path does it; here the plan says so too).
    std::set<uint32_t> back = range(0xC000, 12);
    p = reconcilePlan(rows, back, day1 + 3 * H);
    ok(p.acts[11] == ReconAct::ClearMissed, "a list that has it again clears the pending removal");
  }
  {
    // The weak day: saved 12, today's confirmed list has only 3.
    auto rows = saved(12);
    for (auto& r : rows) r.missedAt = day1 - 7 * H;   // all marked long ago — the worst case
    auto p = reconcilePlan(rows, range(0xC000, 3), day1);
    int removes = 0, marks = 0; for (auto a : p.acts) { removes += a == ReconAct::Remove; marks += a == ReconAct::MarkMissed; }
    ok(p.suspect && removes == 0 && marks == 0, "a weak day seeing 3 of 12 is SUSPECT: nothing marked, nothing removed");
    p = reconcilePlan(rows, range(0xC000, 6), day1);
    removes = 0; for (auto a : p.acts) removes += a == ReconAct::Remove;
    ok(!p.suspect && removes == 6, "exactly half missing is not suspect (the bar is MORE than half)");
  }
  {
    auto rows = saved(4);
    rows[3].manual = true; rows[3].missedAt = day1 - 30 * 24 * H;
    auto p = reconcilePlan(rows, range(0xC000, 3), day1);
    ok(p.acts[3] == ReconAct::Keep, "a hand-saved row is never marked or removed");
  }
  ok(reconcilePlan({}, range(0xC000, 5), day1).acts.empty(), "nothing saved: nothing to do");

  std::printf(fails ? "\033[31m%d failed\033[0m\n" : "\033[32mall passed\033[0m\n", fails);
  return fails ? 1 : 0;
}
