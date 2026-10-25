// test-adsb.cpp — vibe_adsb.h: CRC, field decoding and CPR on the worked examples of "The 1090 Megahertz
// Riddle" (Junzi Sun, 2nd ed.), then the demodulator end to end on frames modulated into noisy IQ at the
// capture rates the radios actually run (2.0, 2.048, 2.4 MS/s).
#include "vibe_adsb.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace vibeadsb;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); ++fails; } } while (0)

static Frame hexFrame(const char* h) {
    Frame f;
    f.len = int(std::strlen(h) / 2);
    for (int i = 0; i < f.len; ++i) { unsigned v; std::sscanf(h + 2 * i, "%2x", &v); f.b[i] = uint8_t(v); }
    return f;
}

/** A DF17 airborne position (TC 11, barometric) for `icao` at lat/lon/alt, CPR-encoded even or odd, CRC set —
 *  so a test can fly an aircraft along any path. CPR from the textbook formulae (NZ 15, 17-bit). */
static Frame posFrame(uint32_t icao, double lat, double lon, int altFt, bool odd) {
    auto mod = [](double x, double y) { return x - y * std::floor(x / y); };
    const double i = odd ? 1 : 0, dLat = 360.0 / (60 - i);
    const double yz = std::floor(131072.0 * mod(lat, dLat) / dLat + 0.5);
    const double rLat = dLat * (yz / 131072.0 + std::floor(lat / dLat));
    const double dLon = 360.0 / std::max(1.0, cprNL(rLat) - i);
    const double xz = std::floor(131072.0 * mod(lon, dLon) / dLon + 0.5);
    const uint32_t la = uint32_t(yz) & 0x1FFFF, lo = uint32_t(xz) & 0x1FFFF;
    const uint32_t n = uint32_t((altFt + 1000) / 25);
    const uint32_t alt = ((n >> 4) << 5) | 0x10 | (n & 0xF);
    // ME bits 1-56: TC(5) SS(2) SAF(1) ALT(12) T(1) F(1) LAT(17) LON(17)
    uint64_t me = (uint64_t(11) << 51) | (uint64_t(alt) << 36) | (uint64_t(odd) << 34) | (uint64_t(la) << 17) | lo;
    Frame f; f.len = 14;
    f.b[0] = 0x8D; f.b[1] = uint8_t(icao >> 16); f.b[2] = uint8_t(icao >> 8); f.b[3] = uint8_t(icao);
    for (int k = 0; k < 7; ++k) f.b[4 + k] = uint8_t(me >> (48 - 8 * k));
    const uint32_t p = syndrome(f.b, 14);                     // parity bytes are 0 here, so this is the CRC
    f.b[11] = uint8_t(p >> 16); f.b[12] = uint8_t(p >> 8); f.b[13] = uint8_t(p);
    return f;
}
static size_t trailPts(const Tracker& t, const char* hex) {
    const std::string j = t.trailJson(hex);
    size_t n = 0; for (char c : j) if (c == '[') ++n;
    return n ? n - 1 : 0;                                       // minus the "pts":[ itself
}

static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

/** PPM-modulate frames into complex IQ at `rate`: each 0.5 µs chip integrated over every sample it overlaps,
 *  so a pulse straddling a sample boundary at 2.4 MS/s splits its energy as a real receiver's would. */
static std::vector<std::complex<float>> modulate(const std::vector<Frame>& frames, double rate, double amp,
                                                 double noise, uint32_t seed, double gapUs = 300) {
    std::vector<std::pair<double, double>> pulses;            // [start µs, end µs)
    std::mt19937 rng0(seed * 7919u);
    std::uniform_real_distribution<double> jitter(0, 1);
    double t = 50 + jitter(rng0);
    for (const auto& f : frames) {
        for (double p : {0.0, 1.0, 3.5, 4.5}) pulses.push_back({t + p, t + p + 0.5});
        for (int i = 0; i < f.len * 8; ++i) {
            const bool one = (f.b[i >> 3] >> (7 - (i & 7))) & 1;
            const double s = t + 8 + i + (one ? 0 : 0.5);
            pulses.push_back({s, s + 0.5});
        }
        t += 8 + f.len * 8 + gapUs + jitter(rng0);         // ★ a random sub-sample start for every reply
    }
    const size_t n = size_t((t + 50) * 1e-6 * rate);
    std::vector<double> env(n, 0.0);
    const double dt = 1e6 / rate;                                // µs per sample
    for (auto& p : pulses) {
        size_t a = size_t(p.first / dt), b = size_t(p.second / dt);
        for (size_t k = a; k <= b && k < n; ++k) {
            const double s0 = k * dt, s1 = s0 + dt;
            const double ov = std::max(0.0, std::min(s1, p.second) - std::max(s0, p.first));
            env[k] += ov / dt;
        }
    }
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0, noise / std::sqrt(2.0));
    std::uniform_real_distribution<double> ph(0, 6.283185307);
    const double phase = ph(rng);
    std::vector<std::complex<float>> iq(n);
    for (size_t k = 0; k < n; ++k)
        iq[k] = { float(amp * env[k] * std::cos(phase + k * 0.01) + g(rng)), float(amp * env[k] * std::sin(phase + k * 0.01) + g(rng)) };
    return iq;
}

int main() {
    printf("test-adsb\n");

    // ── CRC on real frames ───────────────────────────────────────────────────────────────────────────
    const char* good[] = { "8D4840D6202CC371C32CE0576098", "8D40621D58C382D690C8AC2863A7",
                           "8D40621D58C386435CC412692AD6", "8D485020994409940838175B284F",
                           "8DA05F219B06B6AF189400CBC33F" };
    for (const char* h : good) { Frame f = hexFrame(h); CHECK(syndrome(f.b, f.len) == 0, "CRC of %s", h); }

    // ── Identification: KLM1023 ──────────────────────────────────────────────────────────────────────
    {
        Tracker t;
        CHECK(t.accept(hexFrame("8D4840D6202CC371C32CE0576098"), 0), "identification accepted");
        auto it = t.aircraft().find(0x4840D6);
        CHECK(it != t.aircraft().end() && it->second.callsign == "KLM1023", "callsign KLM1023 (got %s)",
              it == t.aircraft().end() ? "-" : it->second.callsign.c_str());
        CHECK(it != t.aircraft().end() && it->second.category == 0xA0, "category A0");
    }

    // ── Airborne position, global: 52.2572 / 3.91937, 38 000 ft ─────────────────────────────────────
    {
        Tracker t;
        t.accept(hexFrame("8D40621D58C386435CC412692AD6"), 100);   // odd, older
        t.accept(hexFrame("8D40621D58C382D690C8AC2863A7"), 102);   // even, newer
        const Aircraft& a = t.aircraft().at(0x40621D);
        CHECK(a.hasPos && near(a.lat, 52.2572, 1e-3) && near(a.lon, 3.91937, 1e-3), "global CPR (got %.5f %.5f)", a.lat, a.lon);
        CHECK(a.hasAlt && a.altBaro == 38000, "altitude 38000 (got %d)", a.altBaro);
    }

    // ── Airborne position, local from the receiver (needs two agreeing frames) ──────────────────────
    {
        double lat, lon;
        Frame f = hexFrame("8D40621D58C382D690C8AC2863A7");
        cprLocal(bits(f.b + 4, 23, 39), bits(f.b + 4, 40, 56), false, false, 52.258, 3.918, lat, lon);
        CHECK(near(lat, 52.2572, 1e-3) && near(lon, 3.91937, 1e-3), "local CPR (got %.5f %.5f)", lat, lon);
    }

    // ── Velocity: ground speed 159.20 kt, track 182.88°, -832 ft/min ────────────────────────────────
    {
        Tracker t;
        t.accept(hexFrame("8D485020994409940838175B284F"), 0);
        const Aircraft& a = t.aircraft().at(0x485020);
        CHECK(a.hasGs && near(a.gs, 159.20, 0.05), "gs 159.2 (got %.2f)", a.gs);
        CHECK(a.hasTrack && near(a.track, 182.88, 0.05), "track 182.88 (got %.2f)", a.track);
        CHECK(a.hasVr && a.vr == -832, "vr -832 (got %d)", a.vr);
    }

    // ── Airspeed subtype: heading 243.98°, TAS 375 kt, -2304 ft/min ─────────────────────────────────
    {
        Tracker t;
        t.accept(hexFrame("8DA05F219B06B6AF189400CBC33F"), 0);
        const Aircraft& a = t.aircraft().at(0xA05F21);
        CHECK(a.hasHdg && near(a.heading, 243.98, 0.05), "heading 243.98 (got %.2f)", a.heading);
        CHECK(a.hasAs && a.airspeed == 375 && a.tas, "TAS 375 (got %d)", a.airspeed);
        CHECK(a.hasVr && a.vr == -2304, "vr -2304 (got %d)", a.vr);
    }

    // ── NL table: the published first transition latitude (59 → 58 at 10.47047130°) ────────────────
    CHECK(cprNL(0) == 59 && cprNL(10.47) == 59 && cprNL(10.471) == 58 && cprNL(86.9) == 2 && cprNL(87.1) == 1,
          "NL transitions (NL(10.47)=%d NL(10.471)=%d)", cprNL(10.47), cprNL(10.471));

    // ── Altitude and squawk fields ───────────────────────────────────────────────────────────────────
    {
        int ft = 0;
        // Q = 1: N = 1560 → 38 000 ft. AC13 = N with M (bit 6) and Q (bit 4) inserted.
        const uint32_t n = 1560;
        const uint32_t ac = ((n & 0x7E0) << 2) | ((n & 0x10) << 1) | 0x10 | (n & 0xF);
        CHECK(altitude13(ac, ft) && ft == 38000, "AC13 Q=1 → 38000 (got %d)", ft);
        CHECK(!altitude13(0, ft), "AC13 0 = no altitude");
        // Squawk 7700: A=7 B=7 C=0 D=0 → bits A4 A2 A1 B4 B2 B1 set
        const uint32_t id = (1u << (12 - 5)) | (1u << (12 - 3)) | (1u << (12 - 1)) | (1u << (12 - 11)) | (1u << (12 - 9)) | (1u << (12 - 7));
        CHECK(squawk13(id) == 7700, "squawk 7700 (got %04d)", squawk13(id));
    }

    // ── One bad bit is repaired; two are refused ────────────────────────────────────────────────────
    {
        Tracker t;
        Frame f = hexFrame("8D4840D6202CC371C32CE0576098");
        f.b[6] ^= 0x10;
        CHECK(t.accept(f, 0) && t.repaired() == 1, "1-bit error repaired");
        CHECK(t.aircraft().at(0x4840D6).callsign == "KLM1023", "repaired frame decodes");
        f.b[9] ^= 0x01;
        CHECK(!t.accept(f, 1), "2-bit error refused");
    }

    // ── An address/parity reply counts only for an aircraft already heard ───────────────────────────
    {
        Tracker t;
        Frame df4; df4.len = 7; df4.b[0] = 4 << 3; df4.b[1] = 0x00; df4.b[2] = 0x18; df4.b[3] = 0x30;  // altitude field set
        const uint32_t addr = 0x4840D6;
        const uint32_t p = syndrome(df4.b, 7) ^ addr;                  // overlay the address on the parity
        df4.b[4] = uint8_t(p >> 16); df4.b[5] = uint8_t(p >> 8); df4.b[6] = uint8_t(p);
        CHECK(!t.accept(df4, 0), "DF4 from an unknown address refused");
        t.accept(hexFrame("8D4840D6202CC371C32CE0576098"), 1);
        CHECK(t.accept(df4, 2), "DF4 from a known address accepted");
    }

    // ── The demodulator, end to end ──────────────────────────────────────────────────────────────────
    std::vector<Frame> frames;
    for (const char* h : good) frames.push_back(hexFrame(h));
    // ★★ 2.0 and 2.048 MS/s are here to be MEASURED, not passed: a 0.5 µs pulse that splits 50/50 across two
    //    samples makes both halves of a bit look the same, and at those rates every reply drifts through that
    //    split. ADS-B mode asks the radio for 2.4 MS/s or more (vibe_adsb_mode.h) — the rates held to the bar.
    for (double rate : {2.0e6, 2.048e6, 2.4e6, 2.5e6, 3.0e6, 4.0e6}) {
        for (double snrDb : {20.0, 14.0}) {
            int heard = 0, total = 0;
            for (uint32_t seed = 1; seed <= 20; ++seed) {
                const double noise = 0.02;
                const double amp = noise * std::pow(10.0, snrDb / 20.0);
                auto iq = modulate(frames, rate, amp, noise, seed);
                Demod d(rate);
                Tracker t;
                d.feed(iq.data(), iq.size(), [&](const Frame& f) { t.accept(f, 0); }, [&](uint32_t a) { return t.known(a, 0); });
                total += int(frames.size());
                heard += int(t.good());
            }
            const double pct = 100.0 * heard / total;
            printf("  %.3f MS/s, %2.0f dB: %3.0f%% of frames\n", rate / 1e6, snrDb, pct);
            if (rate >= 2.4e6) CHECK(pct >= (snrDb >= 20 ? 97 : 85), "demod %.3f MS/s at %.0f dB heard %.0f%%", rate / 1e6, snrDb, pct);
        }
    }

    // Noise alone must produce no aircraft.
    {
        std::mt19937 rng(7);
        std::normal_distribution<float> g(0, 0.02f);
        std::vector<std::complex<float>> iq(20000000);           // 10 s at 2 MS/s
        for (auto& v : iq) v = {g(rng), g(rng)};
        Demod d(2.0e6);
        Tracker t;
        d.feed(iq.data(), iq.size(), [&](const Frame& f) { t.accept(f, 0); }, [&](uint32_t a) { return t.known(a, 0); });
        CHECK(t.aircraft().empty(), "10 s of noise made %zu aircraft (%llu preambles)", t.aircraft().size(),
              (unsigned long long)d.preambles());
    }

    // ★★ THE TRAIL SURVIVES (2026-10-25: the longest-tracked aircraft showed no track). An airliner east at 450 kt,
    //    a position every 0.5 s, for 40 minutes; the server's blocks put two fixes on ONE time stamp now and then,
    //    and a frame is sometimes heard twice. Then 2 minutes unheard, and back: the trail it had carries on.
    {
        Tracker t;
        const uint32_t icao = 0x4CA123;
        double lat = 52.3, lon = -1.5;
        const double stepDeg = 450.0 / 3600 * 0.5 / 60 / std::cos(lat * M_PI / 180);   // 0.5 s of 450 kt, east
        size_t i = 0;
        for (; i < 4800; ++i) {
            const double ts = std::floor(i * 0.5 / 0.3) * 0.3;            // 300 ms blocks: pairs share a stamp
            Frame f = posFrame(icao, lat, lon, 36000, i & 1);
            CHECK(t.accept(f, ts) || i < 2, "position %zu accepted", i);
            if (i % 7 == 0) t.accept(f, ts);                              // heard twice
            lon += stepDeg;
            t.expire(ts, 60);
        }
        const auto& a = t.aircraft().at(icao);
        const size_t n1 = trailPts(t, "4ca123");
        CHECK(a.trackedNm > 290 && a.trackedNm < 310, "tracked %.1f NM over 40 min at 450 kt (300 expected)", a.trackedNm);
        CHECK(n1 > 400, "the whole 300 NM is in the trail: %zu points", n1);
        // silent for 2 minutes — out of the table — then heard again further east
        double ts = i * 0.5 + 120;
        t.expire(ts, 60);
        CHECK(t.aircraft().count(icao) == 0 && t.goneTrails() == 1, "gone from the table, trail kept");
        lon += stepDeg * 240;
        for (int k = 0; k < 4; ++k, ts += 0.5, lon += stepDeg) t.accept(posFrame(icao, lat, lon, 36000, k & 1), ts);
        const size_t n2 = trailPts(t, "4ca123");
        CHECK(n2 > n1, "back after 2 min: the trail carries on (%zu → %zu points)", n1, n2);
        CHECK(t.aircraft().at(icao).trackedNm > 290, "and so does the distance tracked");
        // a fix that is not flight (300 NM in a second) still clears the trail
        t.accept(posFrame(icao, lat + 5, lon, 36000, 0), ts + 1); t.accept(posFrame(icao, lat + 5, lon, 36000, 1), ts + 1.5);
        CHECK(trailPts(t, "4ca123") <= 2, "an impossible jump starts the trail again (%zu points)", trailPts(t, "4ca123"));
    }

    // JSON carries the familiar fields.
    {
        Tracker t;
        t.accept(hexFrame("8D40621D58C386435CC412692AD6"), 100);
        t.accept(hexFrame("8D40621D58C382D690C8AC2863A7"), 102);
        const std::string j = t.json(103);
        CHECK(j.find("\"hex\":\"40621d\"") != std::string::npos && j.find("\"lat\":52.257") != std::string::npos
              && j.find("\"alt_baro\":38000") != std::string::npos, "json: %s", j.c_str());
    }

    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "ok", checks, fails);
    return fails ? 1 : 0;
}
