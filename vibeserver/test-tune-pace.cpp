// ★★★ TUNE PACING (vibe_tune_pace.h) — a burst of tunes must reach the radio as the NEWEST one, the
//     last tune must always land, and a loaded server must space its retunes out. Stuart, 2026-10-05:
//     "if the server's CPU is reporting that it is struggling we need to slow down the amount of tune
//     commands so that we don't overload it" (the Pi 2 in the garage). Driven on a synthetic clock and
//     a simulated reader loop, so it says the same thing on any machine.
#include "vibe_tune_pace.h"
#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
static void ok(bool c, const std::string& w) {
  std::printf("  %s %s\n", c ? "\033[32mok\033[0m  " : "\033[31mFAIL\033[0m", w.c_str()); if (!c) fails++;
}
static std::string tune(long long hz, const char* mode = "wfm") {
  return "{\"type\":\"tune\",\"frequency\":" + std::to_string(hz) + ",\"mode\":\"" + mode + "\"}";
}

/** The reader loop in miniature: frames arrive at given times, each apply costs `applyMs`.
 *  Returns what was applied, in order. */
struct Frame { int64_t at; std::string msg; };
static std::vector<std::string> run(const std::vector<Frame>& in, int pace, int applyMs, uint64_t* merged = nullptr) {
  vibetune::TuneHold h;
  std::vector<std::string> out;
  int64_t now = 0;
  size_t i = 0;
  auto apply = [&](const std::string& m) { out.push_back(m); now += applyMs; h.applied(now); };
  while (i < in.size() || h.holding()) {
    // Frames that arrived while we were busy are "already in the socket": read them before waiting.
    if (i < in.size() && in[i].at <= now) {
      bool pass = false;
      const std::string first = h.offer(in[i].msg, now, [&] { return pace; }, pass);
      if (!first.empty()) apply(first);
      if (pass) apply(in[i].msg);
      ++i;
      continue;
    }
    const int w = h.waitMs(now);
    const int64_t next = i < in.size() ? in[i].at : INT64_MAX;
    if (w >= 0 && now + w <= next) { now += w; apply(h.take()); continue; }
    now = next;
  }
  if (merged) *merged = h.merged;
  return out;
}

int main() {
  std::printf("tune pacing — the newest tune wins, the last one always lands\n");

  // ── The thresholds ──
  {
    vibetune::Load l;
    ok(vibetune::paceMs(l) == 0, "an idle server paces nothing (0 ms) — a healthy box behaves exactly as before");
    l.dspPct = 69; ok(vibetune::paceMs(l) == 0, "dspCpu 69 % is still idle");
    l.dspPct = 70; ok(vibetune::paceMs(l) == vibetune::kPaceLoadedMs, "dspCpu 70 % = LOADED (150 ms)");
    l = {}; l.cpuLevel = 2; ok(vibetune::paceMs(l) == 150, "health CPU HIGH (>= 75 %) = LOADED");
    l = {}; l.cpuLevel = 1; ok(vibetune::paceMs(l) == 0, "health CPU WARM is not load");
    l = {}; l.throttled = true; ok(vibetune::paceMs(l) == 150, "the snail (throttled) = LOADED");
    l = {}; l.dspPct = 103; ok(vibetune::paceMs(l) == vibetune::kPaceChokedMs, "★ the Pi 2's pinned 103 % = CHOKED (350 ms)");
    l = {}; l.demodBlocked = true; ok(vibetune::paceMs(l) == 350, "a FULL demod queue (demodWaits rising) = CHOKED");
    l = {}; l.backlogMs = 300; ok(vibetune::paceMs(l) == 350, "300 ms of IQ already queued = CHOKED");
    l = {}; l.cpuLevel = 3; ok(vibetune::paceMs(l) == 350, "health CPU CRITICAL = CHOKED");
  }

  // ── What may be merged ──
  {
    using vibetune::coalesceKey;
    ok(coalesceKey(tune(96100000)) == coalesceKey(tune(99700000)), "two tunes in the same mode share a key");
    ok(coalesceKey(tune(96100000)) != coalesceKey(tune(96100000, "nfm")), "★ a MODE change never merges into a frequency move");
    ok(coalesceKey("{\"type\": \"tune\", \"frequency\": 1.467e6, \"mode\": \"am\"}")
       == coalesceKey("{\"type\": \"tune\", \"frequency\": 909000, \"mode\": \"am\"}"), "spaces and exponent form are read");
    ok(coalesceKey("{\"type\":\"tune\",\"bandwidthLow\":-5000,\"bandwidthHigh\":5000}").empty(), "a bandwidth-only tune is never held");
    ok(coalesceKey("{\"type\":\"zoom\",\"frequency\":96100000,\"binBandwidth\":50}").empty(), "a zoom is not a tune");
    ok(coalesceKey("{\"type\":\"tuneStep\",\"frequency\":96100000}").empty(), "a type that only STARTS with tune is not a tune");
    ok(coalesceKey("{\"type\":\"tune\",\"frequency\":225648000,\"mode\":\"dab\",\"sid\":50000}")
       != coalesceKey("{\"type\":\"tune\",\"frequency\":225648000,\"mode\":\"dab\",\"sid\":50001}"), "a different DAB service is a different tune");
  }

  // ── Idle: a lone tune lands at once, a backlog collapses to its newest ──
  {
    auto a = run({{0, tune(96100000)}}, 0, 5);
    ok(a.size() == 1 && a[0] == tune(96100000), "idle: a single tune is applied immediately");
    // Ten tunes already sitting in the socket (they arrived while the reader was busy).
    std::vector<Frame> f;
    for (int k = 0; k < 10; k++) f.push_back({0, tune(96100000 + k * 100000)});
    uint64_t m = 0;
    a = run(f, 0, 5, &m);
    ok(a.size() == 1 && a.back() == tune(96100000 + 9 * 100000), "idle: a BACKLOG of 10 applies only the newest");
    ok(m == 9, "and counts the 9 it merged");
  }

  // ── The brief's burst: 50 tunes in 1 s ──
  {
    std::vector<Frame> f;
    for (int k = 0; k < 50; k++) f.push_back({k * 20, tune(88000000 + k * 100000)});
    const std::string last = tune(88000000 + 49 * 100000);
    // Idle box, each retune 5 ms: arrivals are 20 ms apart, so nothing is waiting — all 50 apply.
    auto a = run(f, 0, 5);
    ok(a.size() == 50 && a.back() == last, "idle, cheap retunes: all 50 apply, in order (nothing changes on a healthy box)");
    // A Pi 2 retune that takes 60 ms: the backlog is merged even with pace 0.
    a = run(f, 0, 60);
    ok(a.size() < 25 && a.back() == last, "idle but SLOW retunes (60 ms): the backlog collapses (" + std::to_string(a.size()) + " applies) and ends on the last");
    // Loaded: 150 ms between applies.
    a = run(f, vibetune::kPaceLoadedMs, 60);
    ok(a.size() <= 6 && a.back() == last, "LOADED: <= 6 applies for 50 tunes in 1 s (" + std::to_string(a.size()) + "), ends on the LAST frequency");
    a = run(f, vibetune::kPaceChokedMs, 60);
    ok(a.size() <= 4 && a.back() == last, "CHOKED: <= 4 applies (" + std::to_string(a.size()) + "), ends on the LAST frequency");
    ok(a.front() == tune(88000000), "★ the FIRST tune still goes at once — a leading edge, so a single click is never delayed");
  }

  // ── Order is kept around anything that is not a plain tune ──
  {
    const std::string mode = "{\"type\":\"mode\",\"mode\":\"am\"}";
    auto a = run({{0, tune(96100000)}, {10, tune(96200000)}, {20, mode}, {30, tune(909000, "am")}},
                 vibetune::kPaceLoadedMs, 5);
    ok(a.size() == 4 && a[0] == tune(96100000) && a[1] == tune(96200000) && a[2] == mode && a[3] == tune(909000, "am"),
       "★ a held tune is applied BEFORE the message that follows it — order kept, nothing lost");
    // A mode change in a tune: the older frequency-move still lands before it.
    a = run({{0, tune(96100000)}, {10, tune(96200000)}, {20, tune(96200000, "nfm")}}, vibetune::kPaceLoadedMs, 5);
    ok(a.size() == 3 && a[1] == tune(96200000) && a[2] == tune(96200000, "nfm"),
       "a mode switch is never swallowed by — nor swallows — the frequency moves around it");
  }

  // ── The held tune is never lost ──
  {
    vibetune::TuneHold h;
    bool pass = false;
    h.offer(tune(96100000), 0, [] { return 0; }, pass);
    h.applied(0); (void)h.take();
    h.offer(tune(97000000), 10, [] { return 350; }, pass);
    ok(h.holding() && h.waitMs(10) == 340, "a tune 10 ms after the last apply waits out the 350 ms pace (340 ms)");
    ok(h.take() == tune(97000000), "★ on close the held tune is still there to apply — the last tune is never dropped");
  }

  std::printf(fails ? "\033[31m%d FAILED\033[0m\n" : "\033[32mall passed\033[0m\n", fails);
  return fails ? 1 : 0;
}
