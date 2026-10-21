// ★★★ DAB BLOCK STEPS ON THE SERVER (vibe_tune_pace.h, 2026-10-21) — the backstop for every client that
//     does not coalesce its own presses (apps from before, scripts, anybody's third-party client).
//     Stuart: "if I connect at 12b and rapidly tune down to 7D hammering the multiplex button multiple
//     times a second to get it down quick, dont tune the SDR just hold it and tune on final move. However
//     if I am tuning a multiplex and holding there for a second to see the waterfall then tuning again we
//     need to tune as normal". A block change is a whole re-acquire; a burst must cost the block it ENDED
//     on, and a lone press must not wait.
//     Driven on a synthetic clock through the reader loop in miniature — the SAME loop shape as
//     local_sdr_shim.cpp's (a held message is applied when due; a pass-through is applied with no
//     applied() call, exactly as the server does).
#include "vibe_tune_pace.h"
#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
static void ok(bool c, const std::string& w) {
  std::printf("  %s %s\n", c ? "\033[32mok\033[0m  " : "\033[31mFAIL\033[0m", w.c_str()); if (!c) fails++;
}
static std::string blk(int i) { return "{\"type\":\"dab\",\"on\":1,\"channel\":" + std::to_string(i) + "}"; }

struct Frame { int64_t at; std::string msg; };
struct Applied { int64_t at; std::string msg; };
/** The reader loop: frames arrive at given times; each apply costs `applyMs` of the reader's time. */
static std::vector<Applied> run(const std::vector<Frame>& in, int pace = 0, int applyMs = 5, uint64_t* merged = nullptr) {
  vibetune::TuneHold h;
  std::vector<Applied> out;
  int64_t now = 0;
  size_t i = 0;
  auto apply = [&](const std::string& m, bool held) { out.push_back({now, m}); now += applyMs; if (held) h.applied(now); };
  while (i < in.size() || h.holding()) {
    if (i < in.size() && in[i].at <= now) {
      bool pass = false;
      const std::string first = h.offer(in[i].msg, now, [&] { return pace; }, pass);
      if (!first.empty()) apply(first, true);
      if (pass) apply(in[i].msg, false);
      ++i;
      continue;
    }
    const int w = h.waitMs(now);
    const int64_t next = i < in.size() ? in[i].at : INT64_MAX;
    if (w >= 0 && now + w <= next) { now += w; apply(h.take(), true); continue; }
    now = next;
  }
  if (merged) *merged = h.merged;
  return out;
}
static int dabApplies(const std::vector<Applied>& a) {
  int n = 0; for (auto& x : a) if (vibetune::typeIs(x.msg, "dab")) ++n; return n;
}

int main() {
  std::printf("DAB block steps — a burst re-acquires on its LAST block, a lone press goes at once\n");

  // ── What is a block step ──
  {
    using vibetune::coalesceKey;
    ok(!coalesceKey(blk(32)).empty() && coalesceKey(blk(32)) == coalesceKey(blk(11)), "two block steps share a key whatever the block");
    ok(coalesceKey("{\"type\":\"dab\",\"on\":1,\"channel\":\"12B\"}") == coalesceKey("{\"type\":\"dab\",\"on\":1,\"channel\":\"7D\"}"),
       "a block named as text (\"12B\") is a step too");
    ok(coalesceKey("{\"type\":\"dab\", \"on\": 1, \"channel\": 5}") == coalesceKey("{\"type\":\"dab\", \"on\": 1, \"channel\": 40}"), "spaces are read");
    ok(coalesceKey("{\"type\":\"dab\",\"on\":1,\"channel\":32,\"sid\":50000}").empty(), "★ a PICK (block + service) is never held");
    ok(coalesceKey("{\"type\":\"dab\",\"on\":1}").empty(), "an entry with no block is never held");
    ok(coalesceKey("{\"type\":\"dab\",\"on\":0}").empty(), "leaving DAB is never held");
    ok(coalesceKey("{\"type\":\"dab_service\",\"sid\":50000}").empty(), "a service change is not a step");
    ok(coalesceKey(blk(3)) != coalesceKey("{\"type\":\"tune\",\"frequency\":3,\"mode\":\"wfm\"}"), "a step and a tune never share a key");
    ok(vibetune::typeIs("{\"type\":\"tune\",\"frequency\":1}", "tune") && !vibetune::typeIs("{\"type\":\"tuneStep\"}", "tune"), "typeIs is exact");
  }

  // ── A lone press is not delayed ──
  {
    auto a = run({{1000, blk(31)}});
    ok(a.size() == 1 && a[0].at == 1000 && a[0].msg == blk(31), "a LONE block request is applied the instant it arrives (no added delay)");
    a = run({{1000, blk(31)}, {2000, blk(30)}});
    ok(a.size() == 2 && a[0].at == 1000 && a[1].at == 2000, "press, look a second, press: both applied on arrival — tuning as normal");
    // An app that already coalesces sends once per burst, bursts >= 450 ms apart: never held.
    a = run({{0, blk(31)}, {450, blk(25)}, {900, blk(20)}});
    ok(a.size() == 3 && a[1].at == 450 && a[2].at == 900, "a coalescing client's sends (>= 450 ms apart) are never held by the server");
  }

  // ── THE ASK: 10 block requests in 500 ms ──
  {
    std::vector<Frame> f;
    for (int k = 0; k < 10; k++) f.push_back({k * 50, blk(32 - (k + 1))});   // 12B walking down, a press every 50 ms
    uint64_t m = 0;
    auto a = run(f, 0, 5, &m);
    ok(a.back().msg == blk(22), "10 presses in 500 ms: the radio ends on the LAST block asked for");
    ok(dabApplies(a) == 2, "…with " + std::to_string(dabApplies(a)) + " re-acquires: the first press (nobody can know it starts a burst) and the last");
    ok(a.size() == 2 && a[0].msg == blk(31) && a[0].at == 0, "…the first went at once, as a lone press must");
    ok(m == 8, "…and the 8 in between were never applied (" + std::to_string(m) + " merged)");
    ok(a.back().at <= 450 + vibetune::kDabBurstTrailMs + 10, "…landing " + std::to_string(a.back().at - 450) + " ms after the last press");
    // The same 10 already queued in the socket (a slow link / a busy Pi 2 reader): ONE re-acquire.
    std::vector<Frame> q;
    for (int k = 0; k < 10; k++) q.push_back({0, blk(32 - (k + 1))});
    a = run(q, 0, 5, &m);
    ok(a.size() == 1 && a[0].msg == blk(22), "the same 10 arriving as one clump: ONE re-acquire, on the final block");
  }

  // ── A held-down key cannot hold the dial off for ever ──
  {
    std::vector<Frame> f;
    for (int k = 0; k < 15; k++) f.push_back({k * 200, blk(40 - k)});   // 3 s at 5 presses a second
    auto a = run(f);
    ok(a.back().msg == blk(26), "3 s of presses: ends on the last block");
    ok(dabApplies(a) <= 4, "…in " + std::to_string(dabApplies(a)) + " re-acquires, not 15");
    bool capped = true;
    for (size_t k = 1; k < a.size(); k++) if (a[k].at - a[k - 1].at > vibetune::kDabBurstCapMs + vibetune::kDabBlockGapMs) capped = false;
    ok(capped, "…and the dial never waits longer than the burst cap between moves");
  }

  // ── What else may arrive in a burst ──
  {
    const std::string off = "{\"type\":\"dab\",\"on\":0}";
    auto a = run({{0, blk(31)}, {50, blk(30)}, {100, blk(29)}, {150, off}});
    ok(a.size() == 2 && a[0].msg == blk(31) && a[1].msg == off && a[1].at == 150,
       "leaving DAB mid-burst: the held step is DROPPED (never tuned), the exit goes at once");
    const std::string pick = "{\"type\":\"dab\",\"on\":1,\"channel\":12,\"sid\":50000}";
    a = run({{0, blk(31)}, {50, blk(30)}, {100, pick}});
    ok(a.size() == 2 && a[1].msg == pick && a[1].at == 100, "a bookmark pick mid-burst replaces the held step and goes at once");
    const std::string ping = "{\"type\":\"ping\"}";
    a = run({{0, blk(31)}, {50, blk(30)}, {100, ping}, {150, blk(29)}});
    ok(dabApplies(a) == 2 && a.back().msg == blk(29), "a keep-alive ping inside a burst does not flush the held step");
    const std::string svc = "{\"type\":\"dab_service\",\"sid\":50000}";
    a = run({{0, blk(31)}, {50, blk(30)}, {100, svc}});
    ok(a.size() == 3 && a[1].msg == blk(30) && a[2].msg == svc,
       "★ anything else keeps its ORDER: the held step lands BEFORE the message after it");
  }

  // ── A loaded server's pace still applies on top ──
  {
    auto a = run({{0, blk(31)}, {500, blk(30)}}, vibetune::kPaceChokedMs);
    ok(a.size() == 2 && a[1].at == 500, "a step after the gap on a CHOKED box: its 350 ms pace is already over");
    a = run({{0, "{\"type\":\"tune\",\"frequency\":225648000,\"mode\":\"wfm\"}"}, {10, blk(30)}}, vibetune::kPaceChokedMs);
    ok(a.size() == 2 && a[1].at >= 350, "a step straight after a tune on a CHOKED box waits the load pace");
  }

  // ── The held step is never lost ──
  {
    vibetune::TuneHold h;
    bool pass = false;
    h.offer(blk(31), 0, [] { return 0; }, pass); (void)h.take(); h.applied(0);
    h.offer(blk(30), 50, [] { return 0; }, pass);
    ok(h.holding() && h.waitMs(50) == vibetune::kDabBlockGapMs - 50, "a step 50 ms after an applied one waits out the gap");
    ok(h.take() == blk(30), "★ on close the held step is still there to apply — the radio ends where the user stopped");
  }

  std::printf(fails ? "\033[31m%d FAILED\033[0m\n" : "\033[32mall passed\033[0m\n", fails);
  return fails ? 1 : 0;
}
