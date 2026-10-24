// test-adsb-capture.cpp — ADS-B's sample supply and the boosted-rate guard (2026-10-24):
//
//   1. CaptureMeter (vibe_adsb_capture.h) — the live "capture N %": full rate, a short stream, a stall that must read
//      as falling, the window.
//   2. RateGuard (vibe_rate_guard.h) — the stall / crash record, the marker across a "restart", the sentence.
//   3. vibeadsb::Service — "capture" in the report, on a real-time feed at half the promised rate.
//   4. The benchmark's ADS-B row (vibe_benchmark_adsb.h) on a FAKE source: frames modulated into noise decode and the
//      row scores; noise alone scores -1 ("measured nothing"); a clip file round-trips and a truncated one is refused.
#include "vibe_adsb_capture.h"
#include "vibe_rate_guard.h"
#include "vibe_adsb_service.h"
#include "vibe_benchmark_adsb.h"
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace vibeadsb;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { std::printf("  FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); ++fails; } } while (0)

static void testMeter() {
    std::printf("── CaptureMeter (live capture %%) ──\n");
    const double rate = 2.4e6;
    {
        CaptureMeter m; m.reset(rate, 0);
        for (int i = 1; i <= 200; ++i) m.add(size_t(rate / 100), i * 0.01);    // 2 s at full rate
        CHECK(m.percent(2.0) == -1, "under 3 s of history = -1 (got %d)", m.percent(2.0));
        for (int i = 201; i <= 1200; ++i) m.add(size_t(rate / 100), i * 0.01);  // to 12 s
        CHECK(m.percent(12.0) == 100, "full rate = 100 (got %d)", m.percent(12.0));
    }
    {
        CaptureMeter m; m.reset(rate, 0);
        for (int i = 1; i <= 1000; ++i) m.add(size_t(rate / 100 * 0.74), i * 0.01);
        CHECK(std::abs(m.percent(10.0) - 74) <= 1, "74 %% of the rate = 74 (got %d)", m.percent(10.0));
    }
    {   // a stall: 20 s good, then nothing for 5 s — must FALL, not freeze at 100
        CaptureMeter m; m.reset(rate, 0);
        for (int i = 1; i <= 2000; ++i) m.add(size_t(rate / 100), i * 0.01);
        const int p = m.percent(25.0);
        CHECK(p <= 55 && p >= 45, "5 s silent in a 10 s window reads ~50 (got %d)", p);
        CHECK(m.percent(40.0) == 0, "long silent = 0 (got %d)", m.percent(40.0));
    }
    {   // the window: 10 s at half rate, then 12 s full — only the last 10 s count
        CaptureMeter m; m.reset(rate, 0);
        int i = 1;
        for (; i <= 1000; ++i) m.add(size_t(rate / 200), i * 0.01);
        for (; i <= 2200; ++i) m.add(size_t(rate / 100), i * 0.01);
        CHECK(m.percent(22.0) >= 99, "an old bad stretch ages out of the window (got %d)", m.percent(22.0));
    }
}

static void testGuard() {
    std::printf("── RateGuard (a radio that fell over at a boosted rate) ──\n");
    vibe::RateFault f;
    CHECK(!f.any() && f.say().empty(), "nothing recorded: nothing to say");
    CHECK(vibe::RateFault::rateText(2.048e6) == "2.048 MS/s" && vibe::RateFault::rateText(3.0e6) == "3 MS/s"
          && vibe::RateFault::rateText(2.4e6) == "2.4 MS/s", "rates in words");
    f.kind = "crashed"; f.rate = 2.4e6; f.mode = "ADS-B"; f.at = 1761300000;
    CHECK(f.say().find("SDR crashed at 2.4 MS/s during ADS-B") == 0, "crashed: %s", f.say().c_str());
    f.kind = "stalled"; f.rate = 2.048e6; f.mode = "DAB";
    CHECK(f.say().find("SDR stopped responding at 2.048 MS/s during DAB") == 0, "stalled: %s", f.say().c_str());
    vibe::RateFault r; CHECK(r.parse(f.json()) && r.kind == "stalled" && r.rate == 2.048e6 && r.mode == "DAB" && r.at == 1761300000,
                             "json round trip: %s", f.json().c_str());
    CHECK(f.json().find("\"blocked\":true") != std::string::npos, "blocked in the json");
    CHECK(!r.parse("nonsense") && !r.any(), "nonsense parses to nothing");

    char dir[] = "/tmp/vibe-rateguard-XXXXXX";
    if (!mkdtemp(dir)) { CHECK(false, "mkdtemp"); return; }
    const std::string base = std::string(dir) + "/rate-guard-TEST";
    {   // ★ a run that died at a boosted rate: the marker is there at the next start
        vibe::RateGuard a; a.setBase(base);
        a.mark(2.4e6, "ADS-B", 1761300200);
        CHECK(a.markerPresent() && a.boosted(), "marker written before the boost");
        a.settled(2.4e6);
        CHECK(a.markerPresent(), "landing ON the boosted rate keeps it");
        // (the process "dies" here)
    }
    {
        vibe::RateGuard b; b.setBase(base);
        CHECK(!b.blocked(), "nothing recorded before boot");
        CHECK(b.boot(1761300999), "boot finds the marker");
        CHECK(!b.markerPresent(), "and removes it");
        const auto x = b.fault();
        CHECK(x.kind == "crashed" && x.rate == 2.4e6 && x.mode == "ADS-B" && x.at == 1761300200 && b.blocked(),
              "crashed at the marker's rate, mode and time: %s", b.json().c_str());
        CHECK(!b.boot(1761301000), "boot runs once per process");
    }
    {   // persisted across another restart; Clear forgets it
        vibe::RateGuard c; c.setBase(base);
        CHECK(c.fault().kind == "crashed" && c.blocked(), "the crash survives a restart");
        CHECK(!c.boot(1761301001), "no marker now — no new crash");
        c.clear();
        CHECK(!c.blocked(), "Clear forgets it");
        c.recordStall(2.048e6, "DAB", 1761301100);
        vibe::RateGuard c2; c2.setBase(base);
        CHECK(c2.fault().kind == "stalled" && c2.fault().mode == "DAB", "a stall is persisted too");
        c2.clear();
    }
    {   // the boost ends normally: back at the base rate, the marker comes up
        vibe::RateGuard d; d.setBase(base);
        d.mark(2.048e6, "DAB", 1761301300);
        d.mark(2.4e6, "ADS-B", 1761301310);        // ★ DAB → ADS-B hand-over: one marker, the latest boost
        d.settled(2.4e6);
        CHECK(d.markerPresent(), "a hand-over keeps the marker");
        d.settled(1.024e6);
        CHECK(!d.markerPresent() && !d.boosted(), "back at the base rate: no marker, not boosted");
        vibe::RateGuard e; e.setBase(base);
        CHECK(!e.boot(1761301400) && !e.blocked(), "and the next start finds no crash");
    }
    {   // a clean stop
        vibe::RateGuard g; g.setBase(base);
        g.mark(2.4e6, "ADS-B", 1); g.unmark();
        CHECK(!g.markerPresent(), "a clean stop takes the marker up");
    }
    {   // a marker with no time still records a crash, at boot's time
        if (FILE* fp = std::fopen((base + ".marker").c_str(), "wb")) { std::fputs("entering 2048000 Hz for DAB\n", fp); std::fclose(fp); }
        vibe::RateGuard h; h.setBase(base);
        CHECK(h.boot(1761309999) && h.fault().at == 1761309999 && h.fault().mode == "DAB", "a marker without a time: %s", h.json().c_str());
        h.clear();
    }
    {   vibe::RateGuard m; m.mark(2.4e6, "ADS-B", 1); CHECK(!m.markerPresent() && !m.boot(2) && m.boosted(), "no base = memory only"); }
    std::remove((base + ".json").c_str()); std::remove((base + ".marker").c_str()); rmdir(dir);
}

static void testServiceReport() {
    std::printf("── Service report: capture, on a real-time feed at half the rate (~4 s) ──\n");
    Service svc;
    const double rate = 2.4e6;
    svc.start(rate, NAN, NAN);
    std::vector<std::complex<float>> blk(size_t(rate / 200));   // 5 ms of samples every 10 ms = 50 %
    const auto t0 = std::chrono::steady_clock::now();
    std::string early;
    for (int i = 0; i < 400; ++i) {
        svc.feed(blk.data(), blk.size());
        if (i == 100) early = svc.report();
        std::this_thread::sleep_until(t0 + std::chrono::milliseconds(10 * (i + 1)));
    }
    const std::string r = svc.report();
    svc.stop();
    CHECK(early.find("\"capture\":-1") != std::string::npos, "after 1 s: capture -1 (not yet known): %s", early.c_str());
    const char* p = std::strstr(r.c_str(), "\"capture\":");
    const int cap = p ? std::atoi(p + 10) : -99;
    CHECK(cap >= 45 && cap <= 55, "half the promised samples reads ~50 (got %d)", cap);
    CHECK(r.find("\"type\":\"adsb\"") == 0 + 1 && r.back() == '}', "still a well-formed report");
}

// ── a FAKE source for the benchmark row: frames modulated into noise ──
static std::vector<std::complex<float>> modulated(double rate, double seconds, bool frames, uint32_t seed) {
    const size_t n = size_t(rate * seconds);
    std::vector<std::complex<float>> iq(n);
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0, 0.02f);
    for (auto& v : iq) v = { g(rng), g(rng) };
    if (!frames) return iq;
    // DF17 identification "8D4840D6202CC371C32CE0576098" (KLM1023, The 1090 MHz Riddle), every 2 ms
    const char* hex = "8D4840D6202CC371C32CE0576098";
    uint8_t b[14]; for (int i = 0; i < 14; ++i) { unsigned v; std::sscanf(hex + 2 * i, "%2x", &v); b[i] = uint8_t(v); }
    const double us = rate / 1e6;
    for (double t0 = 100; (t0 + 130) * us < n; t0 += 2000) {
        auto pulse = [&](double s) {   // ★ each sample gets the share of the 0.5 µs chip it overlaps (test-adsb's rule)
            const double a0 = s * us, a1 = (s + 0.5) * us;
            for (size_t k = size_t(a0); k < n && double(k) < a1; ++k) {
                const double ov = std::max(0.0, std::min(double(k + 1), a1) - std::max(double(k), a0));
                iq[k] += std::complex<float>(float(0.5 * ov), 0.0f);
            }
        };
        for (double p : { 0.0, 1.0, 3.5, 4.5 }) pulse(t0 + p);
        for (int i = 0; i < 112; ++i) pulse(t0 + 8 + i + (((b[i >> 3] >> (7 - (i & 7))) & 1) ? 0 : 0.5));
    }
    return iq;
}

static void testBenchRow() {
    std::printf("── benchmark ADS-B row, fake source (~7 s) ──\n");
    vibe::AdsbClip c; c.rate = 2.4e6; c.centre = 1090e6; c.iq = modulated(c.rate, 1.0, true, 1);
    vibe::AdsbRowStats st;
    const auto r = vibe::runAdsbRow("adsb_2400", "ADS-B @ 2.4 MS/s", c, 3.5, 0.5, &st);
    CHECK(st.messages > 100, "the fake sky decodes (%llu messages)", (unsigned long long)st.messages);
    CHECK(r.pct >= 0 && r.pct < 100 && !r.threads.empty(), "the row scores: %.1f %% (%s)", r.pct, r.hottest.c_str());
    CHECK(st.fedSecs >= 3.9 && st.fedSecs < 4.2, "fed in real time for warm + seconds (%.2f s)", st.fedSecs);
    CHECK(st.capture >= 97, "a real-time feed reads full capture (%d)", st.capture);
    CHECK(std::string(vibe::benchdetail::grade(r.pct)) != "none", "graded");
    vibe::AdsbClip q; q.rate = 2.4e6; q.iq = modulated(q.rate, 1.0, false, 2);
    const auto r2 = vibe::runAdsbRow("adsb_2400", "x", q, 1.0, 0.5);
    CHECK(r2.pct == -1 && std::string(vibe::benchdetail::grade(r2.pct)) == "none", "noise alone measured nothing: -1 / none");

    // a clip file: written, read back, truncated → refused
    const std::string path = "/tmp/vibe-adsb-clip-test.vbu8";
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite("VIBEAB81", 1, 8, f);
        for (double rate : { 2.4e6, 3.0e6 }) {
            const double centre = 1090e6; const uint32_t nb = uint32_t(rate) * 2;
            std::fwrite(&rate, 8, 1, f); std::fwrite(&centre, 8, 1, f); std::fwrite(&nb, 4, 1, f);
            std::vector<uint8_t> u(nb, 127); std::fwrite(u.data(), 1, nb, f);
        }
        std::fclose(f);
    }
    auto clips = vibe::loadAdsbClips(path);
    CHECK(clips.size() == 2 && clips[0].rate == 2.4e6 && clips[1].rate == 3.0e6 && clips[1].iq.size() == 3000000,
          "two records load (%zu)", clips.size());
    truncate(path.c_str(), 8 + 20 + 4800000 + 20 + 1000);
    CHECK(vibe::loadAdsbClips(path).empty(), "a truncated download is refused");
    std::remove(path.c_str());
    CHECK(vibe::runAdsbRows("", 1.0).empty(), "no clip = no rows (never a guessed figure)");
}

int main() {
    testMeter();
    testGuard();
    testServiceReport();
    testBenchRow();
    std::printf("%d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
