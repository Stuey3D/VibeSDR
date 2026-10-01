// ★★★ AN ADMIN READ MUST NEVER HOLD THE CONNECTION LOG'S LOCK WHILE IT WORKS.
//
// The chain this pins (Pi 2, 2026-10-01 — "IQ overrun — dropping a buffer (the DSP thread was
// blocked)" at 16:43:51 and :53, with the admin page open on CONNECTION HISTORY):
//   · the admin page polls /vibeserver/admin/connections and /status every 2 s;
//   · ConnLog::json() built 300 rows — a country AND an ASN lookup each — while holding mtx_, and
//     uniqueSince()/topCountriesJson() ran quadratic searches over 2000 rows under it too;
//   · a listener's socket closing calls ConnLog::close() — and that path held clientMtx at the time;
//   · the DSP thread takes clientMtx on every block. So it waited for the admin page.
// Measured on an M4 with 2000 rows and the real country + ASN tables: a close() waited up to 1.15 s
// behind back-to-back admin reads. After the fix: 0.06 ms.
//
// ★★ CPU-INDEPENDENT ON PURPOSE. The resolvers here SLEEP, so one json() takes ~120 ms on any
//    machine; a close() racing it must still come back in a few ms. Under the old code it waited
//    for the whole build. A test that depends on how fast this Mac is would pass on the Mac and
//    say nothing about a Pi.
#include "vibe_admin.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

static int fails = 0;
static void ok(bool c, const std::string& w) {
  std::printf("  %s %s\n", c ? "\033[32mok\033[0m  " : "\033[31mFAIL\033[0m", w.c_str()); if (!c) fails++;
}
using clk = std::chrono::steady_clock;
static double msSince(clk::time_point t) { return std::chrono::duration<double, std::milli>(clk::now() - t).count(); }

static std::string ipOf(unsigned i) {
  return std::to_string(1 + (i >> 16) % 200) + "." + std::to_string((i >> 8) & 255) + "." + std::to_string(i & 255) + ".7";
}

// The old, obviously-right answers, to check the rewritten ones against.
static int naiveUnique(const std::vector<std::pair<std::string, std::string>>& rows) {
  std::vector<std::string> seen;
  for (auto& r : rows) {
    if (vibeadmin::cloudflareWorkerAddr(r.first)) continue;
    if (std::find(seen.begin(), seen.end(), r.first) == seen.end()) seen.push_back(r.first);
  }
  return (int)seen.size();
}

int main() {
  std::printf("connection log — admin reads hold the lock for a copy, never for the work\n");
  {
    vibeadmin::ConnLog log;
    for (unsigned i = 0; i < 400; i++) {
      const std::string s = "s" + std::to_string(i);
      log.open(ipOf(i), s, "Mozilla/5.0", "GB");
      log.close(ipOf(i), s, "closed", 1000, 0);
    }
    for (unsigned i = 0; i < 60; i++) log.noteScan(ipOf(1000 + i), "/wp-login.php", "curl", "US");
    // ★ Slow lookups: 300 distinct addresses x (country + network) x 0.2 ms ≈ 120 ms per json().
    vibeadmin::ccResolver()  = [](const std::string&) { std::this_thread::sleep_for(std::chrono::microseconds(200)); return std::string("DE"); };
    vibeadmin::netResolver() = [](const std::string&) { std::this_thread::sleep_for(std::chrono::microseconds(200)); return std::string("AS3320 DTAG"); };

    auto t = clk::now(); const std::string j = log.json(); const double buildMs = msSince(t);
    std::printf("    one json() with slow lookups: %.0f ms\n", buildMs);
    ok(buildMs > 60, "the slow resolvers really are slow (otherwise this test proves nothing)");

    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};
    std::thread reader([&] {
      while (!stop) {
        (void)log.json(); (void)log.scansJson();
        (void)log.uniqueSince(86400); (void)log.topCountriesJson(86400);
        reads++;
      }
    });
    double worstClose = 0, worstOpen = 0;
    for (int i = 0; i < 80; i++) {
      const std::string s = "live-" + std::to_string(i);
      auto a = clk::now(); log.open("192.0.2." + std::to_string(i % 250), s, "probe"); worstOpen = std::max(worstOpen, msSince(a));
      std::this_thread::sleep_for(std::chrono::milliseconds(3));
      auto b = clk::now(); log.close("192.0.2." + std::to_string(i % 250), s, "closed", 10, 0); worstClose = std::max(worstClose, msSince(b));
      std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    stop = true; reader.join();
    std::printf("    %d admin reads raced; worst open() %.2f ms, worst close() %.2f ms\n", reads.load(), worstOpen, worstClose);
    ok(reads.load() >= 1, "the admin reads actually ran while the socket path wrote");
    ok(worstClose < 15, "★★★ a socket CLOSE never waits for an admin read to finish building (< 15 ms against a ~120 ms build)");
    ok(worstOpen < 15, "★★ nor does an OPEN");
    vibeadmin::ccResolver() = nullptr; vibeadmin::netResolver() = nullptr;
  }

  std::printf("\nconnection log — the rewritten counts give the old answers\n");
  {
    vibeadmin::ConnLog log;
    std::mt19937 rng(7);
    std::vector<std::pair<std::string, std::string>> rows;   // ip, cc — in arrival order
    const char* ccs[] = { "GB", "DE", "US", "FR", "BR", "JP" };
    for (int i = 0; i < 1500; i++) {
      // ★ Repeats on purpose (one listener, many visits), and a Cloudflare Worker address now and then.
      std::string ip = (i % 97 == 0) ? std::string("2a06:98c0:3600::103") : ipOf(rng() % 300);
      std::string cc = ccs[rng() % 6];
      if (i % 50 == 0) cc.clear();       // ★ an address with no country recorded
      const std::string s = "c" + std::to_string(i);
      log.open(ip, s, "x", cc);
      log.close(ip, s, "closed");
      rows.push_back({ ip, cc });
    }
    ok(log.uniqueSince(86400) == naiveUnique(rows), "uniqueSince = distinct non-Cloudflare addresses (" + std::to_string(naiveUnique(rows)) + ")");
    ok(log.uniqueSince(-100000) == 0, "uniqueSince over a window in the future counts nobody");
    // The chart: distinct addresses per country, biggest first, ties in order of first appearance.
    std::vector<std::pair<std::string, std::vector<std::string>>> by;
    for (auto& r : rows) {
      if (r.second.empty() || vibeadmin::cloudflareWorkerAddr(r.first)) continue;
      auto it = std::find_if(by.begin(), by.end(), [&](auto& e) { return e.first == r.second; });
      if (it == by.end()) { by.push_back({ r.second, { r.first } }); continue; }
      if (std::find(it->second.begin(), it->second.end(), r.first) == it->second.end()) it->second.push_back(r.first);
    }
    std::stable_sort(by.begin(), by.end(), [](auto& a, auto& b) { return a.second.size() > b.second.size(); });
    std::string want = "[";
    for (size_t i = 0; i < by.size() && i < 8; i++)
      want += std::string(i ? "," : "") + "{\"cc\":\"" + by[i].first + "\",\"n\":" + std::to_string(by[i].second.size()) + "}";
    want += "]";
    const std::string got = log.topCountriesJson(86400);
    ok(got == want, "topCountriesJson matches the old counting, order included");
    if (got != want) std::printf("      got  %s\n      want %s\n", got.c_str(), want.c_str());
  }

  std::printf("\nconnection log — the cached page follows every change\n");
  {
    vibeadmin::ConnLog log;
    log.open("198.51.100.1", "a", "x", "GB");
    const uint64_t g0 = log.generation();
    const std::string j1 = log.json();
    ok(log.json() == j1 && log.generation() == g0, "asked twice with nothing in between: the same page, the same generation");
    log.close("198.51.100.1", "a", "closed", 5000, 0);
    ok(log.generation() != g0, "a close moves the generation (the admin page's ?gen= key)");
    const std::string j2 = log.json();
    ok(j2 != j1 && j2.find("\"end\":0") == std::string::npos, "and the page shows the row closed, not the cached open one");
    log.open("198.51.100.2", "b", "x", "DE");
    ok(log.json().find("198.51.100.2") != std::string::npos, "a new visitor appears at once");
    log.markAdmin("198.51.100.2", "b");
    ok(log.json().find("\"admin\":true") != std::string::npos, "an admin mark appears at once");
    ok(log.json(1).find("198.51.100.1") == std::string::npos && log.json().find("198.51.100.1") != std::string::npos,
       "a different limit is a different page, not the cached one");
  }

  std::printf("\nhistory — the admin page asks for the new tail, not the hour\n");
  {
    vibeadmin::History h;
    for (int i = 0; i < 5; i++) { vibeadmin::HistSample s; s.atEpoch = 1000 + i; s.listeners = (uint16_t)i; h.push(s); }
    const std::string all = h.json(0);
    ok(all.find("\"after\"") == std::string::npos && all.find("[1000,") != std::string::npos && all.find("[1004,") != std::string::npos,
       "after=0: the whole ring, with no `after` (what an older page expects)");
    const std::string tail = h.json(1002);
    ok(tail.find("\"after\":1002") != std::string::npos, "after=N says so in the reply");
    ok(tail.find("[1002,") == std::string::npos && tail.find("[1003,") != std::string::npos && tail.find("[1004,") != std::string::npos,
       "after=N: only the rows taken after N");
    ok(h.json(2000).find("\"rows\":[]") != std::string::npos, "nothing new: an empty tail, not the hour");
  }

  std::printf(fails ? "\n\033[31m%d FAILED\033[0m\n" : "\n\033[32mpassed\033[0m\n", fails);
  return fails ? 1 : 0;
}
