// test-ft8-hunt.cpp — FT8 finds a clock that is seconds out, and its spots say by how much (2026-11-14).
//
// ★ WHY: Stuart's R36S, fresh card, its clock 4.1 s slow (the RTC's). The window cut at slot + 0 held nothing:
//   1 FT8 decode where the same audio gives 67. And every FT8 spot carried dt 0 (Ft8Engine never passed it on), so
//   the handheld's FT8 clock auto-set — which needs DTs — always found the clock "synchronised". Now:
//   1. huntStep: the leads in order, a lead that decodes stays, two thin slots before it is left;
//   2. THE RECORDING (~/VibeSDR-test-audio/ft8/r36s-7074-20261114-clock4s-slow.wav, 12 kHz mono, 85 s) through the
//      LIVE Ft8Decoder, stamped with the R36S's own (slow) clock: the hunt finds it and the spots' DT says ≈ −4.1 s;
//   3. the same audio stamped with the TRUE time: decodes at lead 0, DT ≈ 0, the hunt never moves;
//   4. a measured clock (SNTP / a clock set) never hunts.
#include "decoders/ft8_decoder.h"
#include "vibe_clock.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace vibe;
static int fails = 0, passes = 0;
#define CHECK(c, ...) do { if (c) { ++passes; } else { ++fails; std::printf("FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static bool readWav(const char* p, std::vector<int16_t>& pcm, int& rate) {
    FILE* f = std::fopen(p, "rb"); if (!f) return false;
    std::vector<unsigned char> b; unsigned char buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
    std::fclose(f);
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4)) return false;
    size_t i = 12; int ch = 0, bits = 0;
    while (i + 8 <= b.size()) {
        const uint32_t len = b[i + 4] | b[i + 5] << 8 | b[i + 6] << 16 | (uint32_t)b[i + 7] << 24;
        if (!std::memcmp(&b[i], "fmt ", 4)) { ch = b[i + 10]; rate = b[i + 12] | b[i + 13] << 8 | b[i + 14] << 16; bits = b[i + 22]; }
        if (!std::memcmp(&b[i], "data", 4)) {
            if (ch != 1 || bits != 16) return false;
            const size_t m = std::min<size_t>(len, b.size() - i - 8) / 2;
            pcm.resize(m); std::memcpy(pcm.data(), &b[i + 8], m * 2);
            return true;
        }
        i += 8 + len + (len & 1);
    }
    return false;
}

struct Run { int spots = 0; std::vector<double> dts; std::vector<double> leads; };
// Feed the recording, each block stamped start + its last sample's time.
static Run play(const std::vector<int16_t>& pcm, int rate, double startUtc) {
    Run r; std::mutex m;
    Ft8Decoder d(rate, FtxMode::FT8);
    d.onSpot = [&](const std::string&, const std::string&, const std::string&, int, float) {
        std::lock_guard<std::mutex> lk(m); r.spots++; r.dts.push_back(d.decodingDt());
    };
    const int blk = rate / 50;   // 20 ms blocks at 25× real time: a slot's decode (a few ms here) has ~100 ms
    unsigned seen = 0;
    for (size_t i = 0; i + blk <= pcm.size(); i += blk) {
        d.process(pcm.data() + i, blk, startUtc + (double)(i + blk) / rate);
        std::this_thread::sleep_for(std::chrono::microseconds(800));
        const unsigned now = d.slotsDecoded.load();
        if (now != seen) { seen = now; r.leads.push_back(d.huntLead()); }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    return r;
}
static double median(std::vector<double> v) { if (v.empty()) return NAN; std::sort(v.begin(), v.end()); return v[v.size() / 2]; }

int main() {
    // 1. the step rule
    {
        Ft8Decoder::HuntState h;
        CHECK(Ft8Decoder::huntStep(h, 0) == -2.4, "a thin slot at 0 moves to -2.4");
        CHECK(Ft8Decoder::huntStep(h, 1) == 2.4, "another moves to +2.4");
        CHECK(Ft8Decoder::huntStep(h, 30) == 2.4 && h.locked, "a lead that decodes stays");
        CHECK(Ft8Decoder::huntStep(h, 0) == 2.4, "one thin slot does not leave a lead that decoded");
        CHECK(Ft8Decoder::huntStep(h, 0) == -4.8, "two do");
        Ft8Decoder::HuntState w; double last = 0;
        for (int i = 0; i < Ft8Decoder::kHuntLeadCount; ++i) last = Ft8Decoder::huntStep(w, 0);
        CHECK(last == 0.0, "the leads wrap back to 0 (%.1f)", last);
    }
    const char* home = std::getenv("HOME");
    const std::string path = std::string(home ? home : "") + "/VibeSDR-test-audio/ft8/r36s-7074-20261114-clock4s-slow.wav";
    std::vector<int16_t> pcm; int rate = 0;
    if (!readWav(path.c_str(), pcm, rate)) {
        std::printf("   recording NOT RUN — %s missing\n", path.c_str());
    } else {
        // the R36S's own stamp of its first sample (file time − length), and the truth 4.1 s later
        const double r36s = 1794653558.924;
        clockMeasured().store(false);
        Run slow = play(pcm, rate, r36s);
        const double dSlow = median(slow.dts);
        std::printf("   R36S clock (4.1 s slow): %d spots, median DT %+.2f s, leads", slow.spots, dSlow);
        for (double l : slow.leads) std::printf(" %+.1f", l);
        std::printf("\n");
        CHECK(slow.spots >= 40, "the hunt finds the slow clock's signals (%d spots)", slow.spots);
        CHECK(std::fabs(dSlow + 4.1) < 0.3, "★ the spots' DT says the clock is ~4.1 s slow (%+.2f)", dSlow);

        Run right = play(pcm, rate, r36s + 4.1);
        const double dRight = median(right.dts);
        std::printf("   true clock: %d spots, median DT %+.2f s\n", right.spots, dRight);
        CHECK(right.spots >= 50, "a right clock decodes at once (%d spots)", right.spots);
        CHECK(std::fabs(dRight) < 0.3, "and its DT is ~0 (%+.2f)", dRight);
        bool moved = false; for (double l : right.leads) if (l != 0.0) moved = true;
        CHECK(!moved, "the hunt never moves a right clock's window");

        clockMeasured().store(true);
        Run measured = play(pcm, rate, r36s);
        bool mv = false; for (double l : measured.leads) if (l != 0.0) mv = true;
        CHECK(!mv, "a MEASURED clock never hunts");
        CHECK(measured.spots < 10, "(so the slow stamps decode as badly as before: %d)", measured.spots);
        clockMeasured().store(false);
    }
    std::printf(fails ? "%d FAILED, %d passed\n" : "all passed (%d checks)\n", fails ? fails : passes, passes);
    return fails ? 1 : 0;
}
