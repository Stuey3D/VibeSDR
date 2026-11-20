// test-navtex-warm.cpp — V12 THE SHIPPING MAP KEPT BUILT IN THE BACKGROUND (vibe_navtex_warm.h, 2026-11-20): the CPU gate
// (a benchmark, a busy or throttled box, the start-up grace, the test knob, off), the adaptive slice, the worker (passes
// until done, pauses the moment the gate shuts, resumes on the next kick), and THE COST: navwarn::sync over a large
// synthetic gallery (the fixtures repeated) in warm-up-sized slices — per message, per pass overhead, the longest pass.
//   test-navtex-warm [<messages, default 3000; 15000 = the Lenovo's gallery, ~70 s>]
// Build: g++ -std=c++17 -O2 -I android/app/src/main/cpp -o test-navtex-warm vibeserver/test-navtex-warm.cpp -lpthread
#include "vibe_navtex_warm.h"
#include "vibe_navtex_warn.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace vibe;
static int fails = 0, passes = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); (c ? passes : fails)++; }
static double msSince(std::chrono::steady_clock::time_point t0) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
template <class P> static bool waitFor(P p, int ms) { for (int i = 0; i < ms / 5; i++) { if (p()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); } return p(); }

int main(int argc, char** argv) {
    std::printf("test-navtex-warm — the shipping map kept built in the background\n");
    std::printf("── the CPU gate ──\n");
    {
        using S = navwarm::State;
        navwarm::GateIn in;
        ok(navwarm::gate(in) == S::Working, "an idle box past its grace: work");
        in.cpuLevel = 1; ok(navwarm::gate(in) == S::PausedCpu, "CPU WARM (pressure ≥ 50 %): paused");
        in.cpuLevel = 3; ok(navwarm::gate(in) == S::PausedCpu, "CPU CRITICAL: paused");
        in.cpuLevel = 0; in.throttled = true; ok(navwarm::gate(in) == S::PausedThrottle, "throttling: paused");
        in.throttled = false; in.benchRunning = true; ok(navwarm::gate(in) == S::PausedBench, "a benchmark running: paused (it must have the machine)");
        in.cpuLevel = 0; in.inGrace = true; ok(navwarm::gate(in) == S::PausedBench, "…a benchmark wins over the grace");
        in.benchRunning = false; ok(navwarm::gate(in) == S::Starting, "the start-up grace: not yet");
        in.inGrace = false; in.testBusy = true; ok(navwarm::gate(in) == S::PausedTest, "the test knob: paused");
        in.off = true; ok(navwarm::gate(in) == S::Off, "switched off: off");
        ok(std::string(navwarm::stateName(S::PausedBench)) == "paused-benchmark" && std::string(navwarm::stateName(S::Idle)) == "idle", "state names");
    }
    std::printf("── the slice ──\n");
    {
        navwarm::Slicer sl;
        for (int i = 0; i < 20; i++) sl.update(sl.budget, sl.budget * 0.1);   // 0.1 ms a message → 250 wanted
        ok(sl.budget >= 240 && sl.budget <= 260, "0.1 ms a message: ~250 a pass (" + std::to_string(sl.budget) + ")");
        for (int i = 0; i < 20; i++) sl.update(sl.budget, sl.budget * 2.0);   // 2 ms a message (a Pi) → 12
        ok(sl.budget >= 12 && sl.budget <= 14, "2 ms a message: ~12 a pass (" + std::to_string(sl.budget) + ")");
        for (int i = 0; i < 20; i++) sl.update(sl.budget, sl.budget * 50.0);
        ok(sl.budget == navwarm::Slicer::kMinBudget, "very slow: never below the floor (" + std::to_string(sl.budget) + ")");
        for (int i = 0; i < 40; i++) sl.update(sl.budget, sl.budget * 0.001);
        ok(sl.budget == navwarm::Slicer::kMaxBudget, "very fast: never above the request's own 400");
        const size_t b = sl.budget; sl.update(0, 5); ok(sl.budget == b, "a pass that parsed nothing changes nothing");
    }
    std::printf("── the worker ──\n");
    {
        std::atomic<int> left{50}, calls{0};
        std::atomic<int> g{(int)navwarm::State::Working};
        navwarm::Worker w([&](size_t budget, size_t* done) {
                              calls++;
                              const int take = std::min<int>(left.load(), (int)std::min<size_t>(budget, 5));
                              left -= take; *done = (size_t)take;
                              std::this_thread::sleep_for(std::chrono::milliseconds(1));
                              return left.load() > 0;
                          },
                          [&] { return (navwarm::State)g.load(); }, nullptr);
        g.store((int)navwarm::State::PausedBench);
        w.start(); w.kick();
        ok(waitFor([&] { return w.state() == navwarm::State::PausedBench; }, 2000) && calls.load() == 0, "a benchmark running: kicked, no pass");
        g.store((int)navwarm::State::Working);
        w.kick();
        ok(waitFor([&] { return left.load() <= 30; }, 3000), "the gate opens, the next kick works (" + std::to_string(50 - left.load()) + " done)");
        g.store((int)navwarm::State::PausedCpu);
        ok(waitFor([&] { return w.state() == navwarm::State::PausedCpu; }, 2000), "the CPU gets busy: paused within one pass");
        const int at = left.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        ok(left.load() == at && at > 0, "…and stays paused (" + std::to_string(at) + " left)");
        g.store((int)navwarm::State::Working);
        w.kick();
        ok(waitFor([&] { return w.state() == navwarm::State::Idle; }, 5000) && left.load() == 0, "resumed on the next kick and finished: idle");
        ok(w.passes() == (uint64_t)calls.load() && w.parsed() == 50 && w.pauses() >= 1, "counters: " + std::to_string(w.passes()) + " passes, " + std::to_string(w.parsed()) + " parsed, " + std::to_string(w.pauses()) + " pause(s)");
        const int c0 = calls.load();
        w.kick();
        ok(waitFor([&] { return calls.load() == c0 + 1; }, 2000) && w.state() == navwarm::State::Idle, "nothing left: one cheap pass a kick, idle");
    }
    std::printf("── the cost: a large synthetic gallery, in warm-up slices ──\n");
    {
        const size_t N = argc > 1 ? (size_t)std::atol(argv[1]) : 3000;
        const std::string idxIn = "scripts/navtex-fixtures/index.jsonl", fdir = "scripts/navtex-fixtures/files";
        struct Src { std::string line, text; };
        std::vector<Src> src;
        navmap::fileLines(idxIn)([&](const std::string& l) {
            std::string f; if (!navmap::jstr(l, "f", f)) return;
            FILE* fp = std::fopen((fdir + "/" + f).c_str(), "r"); if (!fp) return;
            char b[16384]; const size_t n = std::fread(b, 1, sizeof b, fp); std::fclose(fp);
            src.push_back({l, std::string(b, n)});
        });
        ok(!src.empty(), "the fixtures read (" + std::to_string(src.size()) + " messages, repeated to " + std::to_string(N) + ")");
        if (src.empty()) { std::printf("%d passed, %d failed\n", passes, fails); return 1; }
        // ★ copy i of a fixture: its own name, an hour earlier per round, its warning number moved (the text differs)
        auto nameOf = [](size_t i) { return "syn" + std::to_string(i) + ".txt"; };
        auto textOf = [&](size_t i) {
            std::string t = src[i % src.size()].text;
            const size_t p = t.find("ZCZC "); if (p != std::string::npos && p + 9 <= t.size()) { char d[4]; std::snprintf(d, sizeof d, "%02zu", (i / src.size()) % 100); t[p + 7] = d[0]; t[p + 8] = d[1]; }
            return t;
        };
        std::vector<std::string> lines; lines.reserve(N);
        for (size_t i = 0; i < N; i++) {
            std::string l = src[i % src.size()].line;
            const size_t a = l.find("\"f\":\"") + 5, b = l.find('"', a); l.replace(a, b - a, nameOf(i));
            double t = 0; navmap::jnum(l, "t", t);
            const size_t ta = l.find("\"t\":") + 4, tb = l.find(',', ta); l.replace(ta, tb - ta, std::to_string((long long)t - (long long)(i / src.size()) * 3600));
            lines.push_back(l);
        }
        char tmpl[256]; std::snprintf(tmpl, sizeof tmpl, "%s/navwarm-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp");
        const std::string dir = mkdtemp(tmpl) ? tmpl : "/tmp";
        const std::string idx = dir + "/navtex-warn.idx", met = dir + "/navtex-met.idx";
        const navwarn::LiveFn each = [&](const std::function<void(const std::string&)>& fn) { for (auto& l : lines) fn(l); };
        const navwarn::TextFn text = [&](const std::string& f) { return textOf((size_t)std::atol(f.c_str() + 3)); };
        // ★ the per-call overhead (the index read + the live walk), measured on an index already whole
        navwarm::Slicer sl;
        size_t passesN = 0, parsed = 0; double maxMs = 0, sumMs = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            std::unordered_set<uint64_t> live; size_t idxLines = 0;
            const auto p0 = std::chrono::steady_clock::now();
            const auto sr = navwarn::sync(idx, each, text, sl.budget, &live, &idxLines, met);
            const double ms = msSince(p0);
            passesN++; parsed += sr.added; sumMs += ms; maxMs = std::max(maxMs, ms);
            sl.update(sr.added, ms);
            if (!sr.pending) break;
        }
        const double total = msSince(t0);
        std::unordered_set<uint64_t> live; size_t il = 0;
        const auto o0 = std::chrono::steady_clock::now();
        const auto sr = navwarn::sync(idx, each, text, sl.budget, &live, &il, met);
        const double overhead = msSince(o0);
        const auto q0 = std::chrono::steady_clock::now();
        const auto q = navwarn::query(idx, live, 0, 1793560000);
        const double queryMs = msSince(q0);
        std::printf("    %zu messages: %zu passes (slice settled at %zu), %.0f ms in all = %.3f ms a message; a pass %.1f ms mean, %.1f ms longest\n",
                    parsed, passesN, sl.budget, total, total / std::max<size_t>(1, parsed), sumMs / passesN, maxMs);
        std::printf("    an up-to-date pass (index read + live walk, nothing to parse): %.1f ms · the warnings query a request then runs: %.1f ms (%zu warnings)\n",
                    overhead, queryMs, q.groups);
        ok(parsed == N && sr.added == 0 && sr.pending == 0, "the whole gallery indexed, an up-to-date pass parses nothing");
        ok(maxMs < 250, "no pass hogs a core (longest " + std::to_string((int)maxMs) + " ms on this machine)");
        std::remove(idx.c_str()); std::remove(met.c_str()); ::rmdir(dir.c_str());
    }
    std::printf("%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
