// test-rs41-service.cpp — V12 RS41: the SONDES mode's service, table and JSON (vibe_rs41_service.h), docs/v12/RS41.md.
//  The bands; the service on its own thread fed a three-sonde capture (rows by serial, positions, tracks, the channels
//  line, no temperature before the calibration is whole); a sonde heard for a minute (the temperature appears, right);
//  an encrypted sonde (no position); the table's bounds (64 rows, 400 track points); no NaN ever reaches the JSON.
#include "vibe_rs41_service.h"
#include "vibe_rs41_tx.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
using namespace vibers41;

static bool has(const std::string& s, const std::string& x) { return s.find(x) != std::string::npos; }

/** Feed `secs` of the sondes (20 dB) into a running service, a chunk at a time, letting it keep up. */
static void run(Service& sv, double fs, double hw, std::vector<vibers41tx::Sonde>& ss, const std::vector<double>& hz, int secs) {
    const double sigma = 0.02, n10k = 2 * sigma * sigma * 10e3 / fs;
    std::vector<vibers41tx::Gfsk> tx;
    for (size_t i = 0; i < ss.size(); ++i) {
        tx.emplace_back(fs, hz[i] - hw, std::sqrt(n10k * 100), 2400, 0.05 + 0.2 * i);
        for (int k = 0; k < secs; ++k) tx.back().addFrame(k, vibers41tx::frameOnAir(ss[i], k));
    }
    std::mt19937 rng(9);
    std::normal_distribution<float> g(0.f, float(sigma));
    const size_t total = size_t(fs * secs), chunk = 32768;
    std::vector<cf32> buf(chunk);
    for (size_t base = 0; base < total; base += chunk) {
        const size_t n = std::min(chunk, total - base);
        for (size_t i = 0; i < n; ++i) buf[i] = cf32(g(rng), g(rng));
        for (auto& t : tx) t.render(buf.data(), n, base);
        sv.feed(buf.data(), n);
        while (sv.queued() > chunk * 4) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    while (sv.queued() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

int main() {
    std::printf("1. the bands\n");
    {
        for (const auto& b : bands()) {
            const double r = std::fmod(b.centreHz, 10e3);
            CHECK(b.centreHz > b.loHz && b.centreHz < b.hiHz, "%s: the centre inside", b.id);
            CHECK(std::fabs(r - 5e3) < 1, "%s: the centre 5 kHz off the 10 kHz raster (%.0f)", b.id, r);
            CHECK(b.loHz >= 400e6 && b.hiHz <= 406e6, "%s: inside 400–406 MHz", b.id);
        }
        CHECK(bandById("mid") && !bandById("nope"), "by id");
        CHECK(bandById("all")->minRateHz > 6e6 && bandById("lo")->minRateHz < 1.1e6, "the whole band needs a wide capture");
    }

    const double fs = 2.4e6, hw = 403.005e6;
    std::printf("2. the service: three sondes in the 402–404 MHz band, 12 s\n");
    {
        Service sv;
        sv.start(*bandById("mid"), hw, fs);
        CHECK(std::fabs(sv.coverLo() - 402.0e6) < 1 && std::fabs(sv.coverHi() - 404.0e6) < 1, "covers %.3f–%.3f MHz", sv.coverLo() / 1e6, sv.coverHi() / 1e6);
        std::vector<vibers41tx::Sonde> ss;
        const char* ser[] = { "S3310411", "T1520789", "U0912345" };
        const double hz[] = { 403.300e6, 402.700e6, 403.900e6 };
        for (int i = 0; i < 3; ++i) { ss.emplace_back(uint32_t(11 + i)); ss.back().serial = ser[i]; ss.back().setTx(hz[i]); ss.back().lat = 50 + i; ss.back().vU = i == 2 ? -12 : 5; }
        ss[1].encrypted = true;
        run(sv, fs, hw, ss, { hz[0], hz[1], hz[2] }, 12);
        const std::string j = sv.report(*bandById("mid"), std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count());
        std::printf("   %zu bytes of JSON, %zu rows, %llu frames\n", j.size(), sv.tracker().size(), (unsigned long long)sv.tracker().frames());
        CHECK(has(j, "\"type\":\"sondes\"") && has(j, "\"band\":\"mid\"") && has(j, "\"lo\":402000000"), "the report's head");
        for (auto s : ser) CHECK(has(j, std::string("\"serial\":\"") + s + "\""), "row %s", s);
        CHECK(sv.tracker().size() == 3, "%zu rows", sv.tracker().size());
        CHECK(sv.tracker().frames() >= 30, "%llu frames of 36", (unsigned long long)sv.tracker().frames());
        CHECK(has(j, "\"lat\":50.") && has(j, "\"lat\":52."), "positions");
        CHECK(!has(j, "\"lat\":51."), "the encrypted sonde has no position");
        CHECK(has(j, "\"enc\":true"), "…and says it is encrypted");
        CHECK(has(j, "\"climb\":-12.0") && has(j, "\"climb\":5.0"), "climb rates");
        CHECK(has(j, "\"track\":[["), "tracks");
        CHECK(!has(j, "\"temp\""), "no temperature after 12 of 51 calibration subframes");
        CHECK(has(j, "\"cal\":1"), "the calibration count");
        CHECK(has(j, "\"chans\":[{\"hz\":40"), "the channels being demodulated");
        CHECK(!has(j, "nan") && !has(j, "inf"), "no NaN / inf");
        CHECK(sv.dropped() == 0, "no block dropped (%llu)", (unsigned long long)sv.dropped());
        sv.stop();
        CHECK(!sv.running(), "stopped");
    }
    std::printf("3. a minute of one sonde (1 MS/s): the calibration completes, the temperature appears\n");
    {
        Service sv;
        sv.start(*bandById("mid"), hw, 1.0e6);
        std::vector<vibers41tx::Sonde> ss; ss.emplace_back(5);
        ss[0].serial = "S5550001"; ss[0].tempC = -33.4; ss[0].frame0 = 51 * 40;   // ★ frame numbers from subframe 0
        run(sv, 1.0e6, hw, ss, { 403.1e6 }, 56);
        const std::string j = sv.report(*bandById("mid"), std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count());
        CHECK(has(j, "\"cal\":51"), "all 51 subframes");
        CHECK(has(j, "\"temp\":-33.4"), "temperature −33.4 °C: %s", j.substr(0, 400).c_str());
        CHECK(has(j, "\"txHz\":4033"), "the sonde's own frequency (403.3 MHz in its calibration)");
        sv.stop();
    }
    std::printf("4. the table's bounds\n");
    {
        Tracker t;
        Frame f; f.statusOk = true; f.posOk = true; f.lat = 50; f.lon = 0; f.alt = 100; f.vE = f.vN = 0; f.vU = 5;
        for (int i = 0; i < 100; ++i) { char b[9]; std::snprintf(b, sizeof b, "S%07d", i); f.serial = b; t.add(f, 1000 + i); }
        CHECK(t.size() == Tracker::kMaxRows, "%zu rows (64 at most)", t.size());
        const std::string j = t.json(1100);
        CHECK(!has(j, "S0000000") && has(j, "S0000099"), "the oldest went");
        Tracker u;
        f.serial = "S7777777";
        for (int i = 0; i < 2000; ++i) { f.alt = 100 + 60.0 * i; u.add(f, 5000 + i); }
        const std::string k = u.json(7000, 1e9, 10000);
        size_t pts = 0; for (size_t p = k.find("],["); p != std::string::npos; p = k.find("],[", p + 1)) ++pts;
        CHECK(pts + 1 <= Tracker::kMaxTrack && pts + 1 >= Tracker::kMaxTrack / 2, "%zu track points kept (≤ 400)", pts + 1);
        CHECK(has(k, "120040]"), "the newest point is the last one");
        const std::string m = u.json(7000, 1e9, 100);
        size_t p2 = 0; for (size_t p = m.find("],["); p != std::string::npos; p = m.find("],[", p + 1)) ++p2;
        CHECK(p2 + 1 <= 101, "the report's track thinned to %zu points", p2 + 1);
        CHECK(has(m, "120040]"), "…still ending on the newest");
        CHECK(u.json(1e9, 3600).find("S7777777") == std::string::npos, "a sonde unheard for hours leaves the report");
        // ★★★ beta96 maps2 the gallery's day card (vibe_posgal.h) is fed from heardSondes: the newest position, the highest
        const auto hs = u.heardSondes(7000, 120, 10);
        CHECK(hs.size() == 1 && hs[0].serial == "S7777777" && std::fabs(hs[0].alt - (100 + 60.0 * 1999)) < 1 && std::fabs(hs[0].maxAlt - hs[0].alt) < 1
              && std::fabs(hs[0].last - 6999) < 1e-6 && std::fabs(hs[0].first - 5000) < 1e-6, "heardSondes: its serial, newest altitude, highest, first / last heard");
        CHECK(u.heardSondes(1e9, 120, 10).empty(), "heardSondes: nothing heard in the last 2 minutes");
        CHECK(t.heardSondes(1100, 1e9, 5).size() == 5 && t.heardSondes(1100, 1e9, 5)[0].serial == "S0000099", "heardSondes: bounded, newest first");
    }
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
