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
    std::printf("── RateGuard (beta95: a radio that stopped at a boosted rate — the rules a–e) ──\n");
    const long long T = 1764100000, H = 3600, DAY = 24 * H;
    CHECK(vibe::RateFault::rateText(2.048e6) == "2.048 MS/s" && vibe::RateFault::rateText(3.0e6) == "3 MS/s"
          && vibe::RateFault::rateText(2.4e6) == "2.4 MS/s", "rates in words");
    // rule (c): one rung down, never below the ADS-B floor from above it
    CHECK(vibe::rateGuardRungBelow(2.56e6) == 2.4e6, "c: 2.56 → 2.4");
    CHECK(vibe::rateGuardRungBelow(3.0e6) == 2.88e6, "c: 3.0 → 2.88");
    CHECK(vibe::rateGuardRungBelow(2.4e6) == 0 && !vibe::rateGuardCanStep(2.4e6), "c': 2.4 is the floor — no step below it");
    CHECK(vibe::rateGuardRungBelow(2.048e6) == 0 && !vibe::rateGuardCanStep(2.048e6), "c': 2.048 (DAB) never steps either");
    CHECK(vibe::rateGuardRungBelow(2.5e6) == 2.4e6 && vibe::rateGuardCanStep(2.56e6), "c: anything above the floor lands on it at worst");
    CHECK(vibe::rateGuardRungBelow(8.0e6) == 6.0e6, "c: 8 → 6");

    char dir[] = "/tmp/vibe-rateguard-XXXXXX";
    if (!mkdtemp(dir)) { CHECK(false, "mkdtemp"); return; }
    const std::string base = std::string(dir) + "/rate-guard-TEST";
    auto wipe = [&] { std::remove((base + ".json").c_str()); std::remove((base + ".marker").c_str()); };
    using B = vibe::RateGuard::Boot;
    {   // ★ rule (a): an UPDATE — the marker was written by another build
        wipe();
        { vibe::RateGuard a; a.setBase(base, "12.0.0~beta94@old"); a.mark(2.56e6, "ADS-B", T); }   // the process "dies"
        vibe::RateGuard b; b.setBase(base, "12.0.0~beta95@new");
        std::string why;
        CHECK(b.boot(T + 60, &why) == B::Explained && !why.empty(), "a: another build's marker is an update, not a crash");
        CHECK(!b.markerPresent() && !b.fault(T + 60).any() && b.allows(2.56e6, T + 60), "a: nothing recorded, 2.56 allowed");
    }
    {   // ★ rule (a): a marker from before beta95 (no build) is an update too
        wipe();
        if (FILE* fp = std::fopen((base + ".marker").c_str(), "wb")) { std::fputs("entering 2560000 Hz for ADS-B at 1764100000\n", fp); std::fclose(fp); }
        vibe::RateGuard b; b.setBase(base, "B1");
        CHECK(b.boot(T + 60) == B::Explained && !b.fault(T + 60).any(), "a: a build-less (pre-beta95) marker is not counted");
    }
    {   // ★ rule (a): a deliberate stop takes the marker up
        wipe();
        { vibe::RateGuard g; g.setBase(base, "B1"); g.mark(2.56e6, "ADS-B", T); g.unmark(); CHECK(!g.markerPresent(), "a: a clean stop takes the marker up"); }
        vibe::RateGuard h; h.setBase(base, "B1"); CHECK(h.boot(T + 5) == B::None, "a: and the next start finds nothing");
    }
    {   // ★ rule (b): one death = noted, same rate allowed; a second within 24 h latches (c) one rung down
        wipe();
        { vibe::RateGuard a; a.setBase(base, "B1"); a.mark(2.56e6, "ADS-B", T); }
        {
            vibe::RateGuard b; b.setBase(base, "B1");
            CHECK(b.boot(T + 30) == B::Noted, "b: the first death is noted");
            const auto f = b.fault(T + 30);
            CHECK(f.kind == "crashed" && f.noted == 1 && !f.latched && f.rate == 2.56e6 && f.mode == "ADS-B", "b: noted: %s", f.json().c_str());
            CHECK(b.allows(2.56e6, T + 30), "b: the same rate is allowed again");
            CHECK(f.say().find("noted once") != std::string::npos, "b: the line says it was noted: %s", f.say().c_str());
            b.mark(2.56e6, "ADS-B", T + 10 * H);   // ten hours later it dies again
        }
        vibe::RateGuard c; c.setBase(base, "B1");
        CHECK(c.boot(T + 10 * H + 30) == B::Latched, "b: the second death within 24 h latches");
        const auto f = c.fault(T + 10 * H + 30);
        CHECK(f.latched && f.ceiling == 2.4e6, "c: held at 2.4 (ADS-B floor): %s", f.json().c_str());
        CHECK(!c.allows(2.56e6, T + 11 * H) && c.allows(2.4e6, T + 11 * H) && c.allows(2.048e6, T + 11 * H), "c: 2.56 refused, 2.4 allowed");
        CHECK(f.say().find("(ADS-B still runs)") != std::string::npos && f.say().find("twice in 24 hours") != std::string::npos,
              "e: the owner is told: %s", f.say().c_str());
        CHECK(f.json().find("\"retryAt\":") != std::string::npos && f.json().find("\"blocked\":true") != std::string::npos, "e: retryAt + blocked in the json");
    }
    {   // the latch persists across a restart; (d) it lifts after 24 h for ONE retry; a death on the retry latches for good
        vibe::RateGuard d; d.setBase(base, "B1");
        const long long L = T + 10 * H + 30;
        CHECK(d.latched(L + H) && !d.allows(2.56e6, L + H), "the latch survives a restart");
        CHECK(d.allows(2.56e6, L + DAY + 1) && !d.latched(L + DAY + 1) && d.fault(L + DAY + 1).probation, "d: 24 h on, 2.56 is tried again");
        CHECK(d.fault(L + DAY + 2).say().find("Trying 2.56 MS/s again") == 0, "d: and says so: %s", d.fault(L + DAY + 2).say().c_str());
        CHECK(d.recordStall(2.56e6, "ADS-B", L + DAY + H), "d: a stop on the retry latches at once");
        CHECK(!d.allows(2.56e6, L + 5 * DAY) && d.fault(L + 5 * DAY).retried, "d: …and stays (no second self-clear)");
        CHECK(d.fault(L + 5 * DAY).say().find("until you clear this") != std::string::npos, "d: the line: %s", d.fault(L + 5 * DAY).say().c_str());
        d.clear();   // (e) the owner clears it
        CHECK(!d.latched(L + 5 * DAY) && d.allows(2.56e6, L + 5 * DAY) && !d.fault().any(), "e: Clear forgets everything");
    }
    {   // (d) a retry that runs 24 h clean is forgotten
        wipe();
        vibe::RateGuard e; e.setBase(base, "B1");
        e.recordStall(2.56e6, "ISM", T); CHECK(!e.latched(T), "b: a first stall only notes");
        CHECK(e.recordStall(2.56e6, "ISM", T + H), "b: a second stall latches");
        CHECK(e.allows(2.56e6, T + H + DAY) , "d: probation");
        CHECK(!e.fault(T + H + 2 * DAY + 5).any() && e.allows(2.56e6, T + H + 2 * DAY + 5), "d: 24 h clean on the retry: forgotten");
    }
    {   // (b) deaths more than 24 h apart, or at different rates, never latch
        wipe();
        vibe::RateGuard g; g.setBase(base, "B1");
        CHECK(!g.recordStall(2.56e6, "ADS-B", T), "first");
        CHECK(!g.recordStall(2.56e6, "ADS-B", T + DAY + 10), "b: 24 h later is a first again");
        CHECK(g.fault(T + DAY + 10).noted == 1, "b: the old one aged out");
        CHECK(!g.recordStall(2.048e6, "DAB", T + DAY + 20), "b: another rate counts on its own");
        CHECK(g.fault(T + 3 * DAY).noted == 0 && !g.fault(T + 3 * DAY).any(), "b: notes age out after 24 h");
    }
    {   // ★★★ (c') Stuart 11-26: "2.4msps is the safe rate" — two stops AT 2.4 are told to the owner, NEVER capped
        wipe();
        vibe::RateGuard g; g.setBase(base, "B1");
        CHECK(!g.recordStall(2.4e6, "ADS-B", T), "c': a first stop at 2.4 only notes");
        CHECK(g.fault(T + 1).say().find("never held lower") != std::string::npos, "c': the note says 2.4 is never held lower: %s", g.fault(T + 1).say().c_str());
        CHECK(!g.recordStall(2.4e6, "ADS-B", T + 60), "c': a second stop at 2.4 within 24 h does NOT latch");
        const auto f = g.fault(T + 61);
        CHECK(!f.latched && f.floor && f.floorRate == 2.4e6 && f.floorMode == "ADS-B", "c': the floor note is raised: %s", f.json().c_str());
        CHECK(g.allows(2.4e6, T + 61) && g.allows(2.56e6, T + 61) && g.allows(2.048e6, T + 61), "c': every rate still allowed");
        CHECK(f.say().find("kept at that rate") != std::string::npos && f.say().find("USB power supply") != std::string::npos,
              "c': the owner is told it is the radio: %s", f.say().c_str());
        CHECK(f.json().find("\"floor\":true") != std::string::npos && f.json().find("\"floorRate\":2400000") != std::string::npos, "c': floor in the json");
        vibe::RateGuard g2; g2.setBase(base, "B1");
        CHECK(g2.fault(T + 120).floor && !g2.latched(T + 120), "c': the note survives a restart");
        CHECK(!g2.fault(T + 60 + DAY + 1).floor && !g2.fault(T + 60 + DAY + 1).any(), "c': and is forgotten 24 h after the last stop");
    }
    {   // (c') DAB at 2.048 twice: noted for the owner, never capped (it would lose DAB AND ADS-B)
        wipe();
        vibe::RateGuard g; g.setBase(base, "B1");
        g.recordStall(2.048e6, "DAB", T); g.recordStall(2.048e6, "DAB", T + 30);
        CHECK(!g.latched(T + 31) && g.allows(2.048e6, T + 31) && g.fault(T + 31).floor, "c': DAB at 2.048 twice: told, not capped");
    }
    {   // (c') latched at 2.4 (from 2.56), then it stops twice AT 2.4: the latch stays at 2.4, never 2.048
        wipe();
        vibe::RateGuard g; g.setBase(base, "B1");
        g.recordStall(2.56e6, "ADS-B", T); CHECK(g.recordStall(2.56e6, "ADS-B", T + 10), "latched from 2.56");
        g.recordStall(2.4e6, "ADS-B", T + 20); g.recordStall(2.4e6, "ADS-B", T + 30);
        const auto f = g.fault(T + 31);
        CHECK(f.latched && f.ceiling == 2.4e6 && g.allows(2.4e6, T + 31) && !g.allows(2.56e6, T + 31), "c': still held at 2.4, not below: %s", f.json().c_str());
        CHECK(f.floor && f.say().find("twice in 24 hours") != std::string::npos && f.say().find("USB power supply") != std::string::npos,
              "c': the latch line carries the floor note: %s", f.say().c_str());
        CHECK(f.rate == 2.56e6, "c': the latch's own stop (2.56) is still the one it names");
    }
    {   // ★ a PRE-beta95 record (it latched on one stop and switched the owner's setting off) becomes one noted stop
        wipe();
        if (FILE* fp = std::fopen((base + ".json").c_str(), "wb")) {
            std::fputs("{\"fault\":\"crashed\",\"at\":1764100000,\"rate\":2560000,\"mode\":\"ADS-B\",\"blocked\":true,\"say\":\"x\"}", fp); std::fclose(fp);
        }
        vibe::RateGuard g; g.setBase(base, "B1");
        std::string m;
        CHECK(g.takeLegacy(&m) && m == "ADS-B" && !g.takeLegacy(), "legacy: reported once, with its mode");
        CHECK(!g.latched(T + 60) && g.allows(2.56e6, T + 60) && g.fault(T + 60).noted == 1, "legacy: one noted stop, nothing held");
        vibe::RateGuard g2; g2.setBase(base, "B1");
        CHECK(!g2.takeLegacy() && g2.fault(T + 60).noted == 1, "legacy: rewritten in the new form, not reported again");
    }
    {   // hand-over and settle (unchanged)
        wipe();
        vibe::RateGuard d; d.setBase(base, "B1");
        d.mark(2.048e6, "DAB", T); d.mark(2.4e6, "ADS-B", T + 10); d.settled(2.4e6);
        CHECK(d.markerPresent(), "a hand-over keeps the marker");
        d.settled(1.024e6);
        CHECK(!d.markerPresent() && !d.boosted(), "back at the base rate: no marker, not boosted");
    }
    {   vibe::RateGuard m; m.mark(2.4e6, "ADS-B", 1); CHECK(!m.markerPresent() && m.boot(2) == B::None && m.boosted(), "no base = memory only"); }
    wipe(); rmdir(dir);
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
