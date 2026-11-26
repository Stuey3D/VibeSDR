// test-adsb-delta.cpp — ★★ V12 beta96 the ADS-B DELTA protocol's server side (vibe_adsb_delta.h).
//
//   test-adsb-delta                 the encoder's own checks (quantising, the header walk, ids, keyframes, sizes)
//   test-adsb-delta --stream [N]    a synthetic sky through the REAL Tracker for N ticks (default 900), one JSON line a
//                                   tick: {"tick","full":<the adsb report an older client gets>,"d":<adsb_d>,
//                                   "k":<adsb_k>,"keyAll"} — scripts/test_adsb_delta.ts rebuilds the table from the
//                                   deltas with the clients' own store and checks it against "full" field for field.
//
// The sky: 150 aircraft at a time around a receiver (52.29, -0.85), each with its own reception (some heard every
// second, some now and then), positions twice a second when heard, velocity, identification, Mode S squawks and
// altitudes, Comm-B replies from some, aircraft arriving and leaving all the time (expired by the Tracker at 60 s),
// three on the ground, one squawking 7700 (TC 28) from tick 200 and back to normal at 400.
#include "vibe_adsb_delta.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace vibeadsb;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { std::printf("  FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); ++fails; } } while (0)

static void crc(Frame& f) {
    const uint32_t p = syndrome(f.b, f.len);
    const int o = f.len - 3;
    f.b[o] = uint8_t(p >> 16); f.b[o + 1] = uint8_t(p >> 8); f.b[o + 2] = uint8_t(p);
}
static Frame df17(uint32_t icao, uint64_t me) {
    Frame f; f.len = 14;
    f.b[0] = 0x8D; f.b[1] = uint8_t(icao >> 16); f.b[2] = uint8_t(icao >> 8); f.b[3] = uint8_t(icao);
    for (int k = 0; k < 7; ++k) f.b[4 + k] = uint8_t(me >> (48 - 8 * k));
    crc(f);
    return f;
}
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
    return df17(icao, (uint64_t(11) << 51) | (uint64_t(alt) << 36) | (uint64_t(odd) << 34) | (uint64_t(la) << 17) | lo);
}
static Frame idFrame(uint32_t icao, const char* cs, int cat) {
    static const char* set = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";
    uint64_t me = (uint64_t(4) << 51) | (uint64_t(cat) << 48);
    for (int i = 0; i < 8; ++i) {
        const char c = i < (int)std::strlen(cs) ? cs[i] : ' ';
        const char* at = std::strchr(set, c);
        me |= ((at && c != '#') ? uint64_t(at - set) : 32) << (42 - 6 * i);
    }
    return df17(icao, me);
}
static Frame velFrame(uint32_t icao, double gs, double trk, int vr) {
    const double r = trk * 3.14159265358979323846 / 180.0;
    const double ve = gs * std::sin(r), vn = gs * std::cos(r);
    uint64_t me = 0;
    auto set = [&](int a, int b, uint64_t x) { const int n = b - a + 1; me |= (x & ((1ull << n) - 1)) << (56 - b); };
    set(1, 5, 19); set(6, 8, 1);
    set(14, 14, ve < 0); set(15, 24, uint64_t(std::lround(std::fabs(ve))) + 1);
    set(25, 25, vn < 0); set(26, 35, uint64_t(std::lround(std::fabs(vn))) + 1);
    set(37, 37, vr < 0); set(38, 46, uint64_t(std::abs(vr) / 64) + 1);
    return df17(icao, me);
}
/** Squawk → the 13-bit identity code (the inverse of squawk13). */
static uint32_t idCode(int sq) {
    const int A = sq / 1000 % 10, B = sq / 100 % 10, C = sq / 10 % 10, D = sq % 10;
    uint32_t id = 0;
    auto put = [&](int i, int v) { if (v) id |= 1u << (12 - i); };
    put(5, A >> 2 & 1); put(3, A >> 1 & 1); put(1, A & 1);
    put(11, B >> 2 & 1); put(9, B >> 1 & 1); put(7, B & 1);
    put(4, C >> 2 & 1); put(2, C >> 1 & 1); put(0, C & 1);
    put(12, D >> 2 & 1); put(10, D >> 1 & 1); put(8, D & 1);
    return id;
}
static Frame emergFrame(uint32_t icao, int state, int sq) {
    uint64_t me = 0;
    auto set = [&](int a, int b, uint64_t x) { const int n = b - a + 1; me |= (x & ((1ull << n) - 1)) << (56 - b); };
    set(1, 5, 28); set(6, 8, 1); set(9, 11, uint64_t(state)); set(12, 24, idCode(sq));
    return df17(icao, me);
}
/** DF5 (identity) or DF4 (altitude) surveillance reply; fs 1 = on the ground. Address/parity. */
static Frame survFrame(uint32_t icao, int df, int fs, uint32_t code13) {
    Frame f; f.len = 7;
    f.b[0] = uint8_t(df << 3) | uint8_t(fs & 7);
    f.b[2] = uint8_t(code13 >> 8); f.b[3] = uint8_t(code13);
    const uint32_t p = syndrome(f.b, 7) ^ icao;
    f.b[4] = uint8_t(p >> 16); f.b[5] = uint8_t(p >> 8); f.b[6] = uint8_t(p);
    return f;
}
static uint32_t altCode(int ft) {
    const uint32_t n = uint32_t((ft + 1000) / 25);
    return ((n >> 5) << 7) | (((n >> 4) & 1) << 5) | 0x10 | (n & 0xF);
}
/** A DF20 Comm-B reply with BDS 6,0 (heading, IAS, Mach, vertical rates). */
static Frame cb60(uint32_t icao, double hdg, int ias, double mach, int vr) {
    uint64_t v = 0;
    auto put = [&](int a, int b, uint64_t x) { const int n = b - a + 1; const uint64_t m = ((1ull << n) - 1) << (56 - b); v = (v & ~m) | ((x << (56 - b)) & m); };
    auto sgnf = [](double x, int bitsN) { int i = int(std::lround(x)); if (i < 0) i += 1 << bitsN; return uint64_t(i) & ((1ull << bitsN) - 1); };
    const double h = hdg > 180 ? hdg - 360 : hdg;
    put(1, 1, 1); put(2, 12, sgnf(h * 512.0 / 90.0, 11));
    put(13, 13, 1); put(14, 23, uint64_t(ias));
    put(24, 24, 1); put(25, 34, uint64_t(std::lround(mach * 512.0 / 2.048)));
    put(35, 35, 1); put(36, 45, sgnf(vr / 32.0, 10));
    put(46, 46, 1); put(47, 56, sgnf(vr / 32.0, 10));
    Frame f; f.len = 14;
    f.b[0] = uint8_t(20 << 3);
    for (int k = 0; k < 7; ++k) f.b[4 + k] = uint8_t(v >> (48 - 8 * k));
    const uint32_t p = syndrome(f.b, 14) ^ icao;
    f.b[11] = uint8_t(p >> 16); f.b[12] = uint8_t(p >> 8); f.b[13] = uint8_t(p);
    return f;
}

// ── the synthetic sky ──
struct Plane {
    uint32_t icao; char cs[9]; int cat; double lat, lon, alt, gs, trk, vr; double rx; int sq; bool ground, commb;
    int from, until;   // ticks
};
struct Sky {
    std::mt19937 rng{20261126};
    std::vector<Plane> p;
    uint32_t nextIcao = 0x400100;
    double U(double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); }
    Plane make(int tick) {
        Plane q{};
        q.icao = nextIcao; nextIcao += 0x13;
        std::snprintf(q.cs, sizeof q.cs, "%s%d", (const char*[]){"BAW", "EZY", "RYR", "KLM", "DLH", "TOM"}[q.icao % 6], int(q.icao % 9000) + 100);
        q.cat = 0xA0 | int(q.icao % 5 + 1);
        const double r = U(5, 200) / 60.0, b = U(0, 6.2832);
        q.lat = 52.29 + r * std::cos(b); q.lon = -0.85 + r * std::sin(b) / std::cos(52.29 * 0.01745);
        q.alt = std::round(U(2000, 39000) / 25) * 25; q.gs = U(150, 480); q.trk = U(0, 360);
        q.vr = U(0, 1) < 0.3 ? (U(0, 1) < 0.5 ? -1 : 1) * 64 * std::round(U(8, 40)) : 0;
        q.rx = U(0, 1) < 0.6 ? U(0.85, 1.0) : U(0.15, 0.85);
        q.sq = int(U(0, 1) < 0.5 ? 7000 + int(U(0, 7)) * 100 + int(U(0, 7)) * 10 + int(U(0, 7)) : 1000 + int(U(0, 7)) * 100 + int(U(0, 7)));
        q.ground = false; q.commb = U(0, 1) < 0.25;
        q.from = tick; q.until = tick + int(U(120, 1200));
        return q;
    }
    explicit Sky() {
        for (int i = 0; i < 150; ++i) { Plane q = make(0); q.until = int(U(30, 1200)); p.push_back(q); }
        for (int i = 0; i < 3; ++i) { p[size_t(i)].ground = true; p[size_t(i)].gs = 12; p[size_t(i)].alt = 0; p[size_t(i)].vr = 0; p[size_t(i)].commb = false; }
    }
    /** One second: move everyone, send what each would send (into the tracker, at times inside the second). */
    void second(int tick, double t0, Tracker& tr) {
        for (size_t i = 0; i < p.size(); ++i) {
            Plane& q = p[i];
            if (tick >= q.until && !q.ground) { q = make(tick); continue; }   // leaves; another arrives (always ~150)
            const double kt = q.gs / 3600.0 / 60.0;   // degrees of latitude per second at gs
            q.lat += kt * std::cos(q.trk * 0.01745); q.lon += kt * std::sin(q.trk * 0.01745) / std::cos(q.lat * 0.01745);
            if (!q.ground) { q.alt = std::max(1000.0, q.alt + q.vr / 60.0); if (U(0, 1) < 0.02) q.trk = std::fmod(q.trk + U(-20, 20) + 360, 360);
                             if (U(0, 1) < 0.01) q.vr = q.vr ? 0 : 64 * std::round(U(-30, 30)); }
            if (U(0, 1) > q.rx) continue;   // not heard this second
            const int altQ = int(std::round(q.alt / 25) * 25);
            double t = t0 + U(0.0, 0.5);
            auto send = [&](const Frame& f) { Frame g = f; g.level = float(-30 + 25 * q.rx + U(-2, 2)); g.snr = float(12 + 25 * q.rx + U(-2, 2)); tr.accept(g, t); t += U(0.01, 0.2); };
            if (q.ground) send(survFrame(q.icao, 4, 1, 0));
            else { send(posFrame(q.icao, q.lat, q.lon, altQ, false)); send(posFrame(q.icao, q.lat, q.lon, altQ, true)); }
            if (U(0, 1) < 0.7) send(velFrame(q.icao, q.gs, q.trk, int(q.vr)));
            if ((tick + int(i)) % 5 == 0) send(idFrame(q.icao, q.cs, q.cat & 7));
            int sq = q.sq;
            if (i == 10 && tick >= 200 && tick < 400) { sq = 7700; send(emergFrame(q.icao, 1, 7700)); }
            if (i == 10 && tick >= 400 && tick < 403) send(emergFrame(q.icao, 0, q.sq));
            if (U(0, 1) < 0.3) send(survFrame(q.icao, 5, q.ground ? 1 : 0, idCode(sq)));
            if (U(0, 1) < 0.3 && !q.ground) send(survFrame(q.icao, 4, 0, altCode(altQ)));
            if (q.commb && U(0, 1) < 0.4) send(cb60(q.icao, q.trk, int(q.gs * 0.7), 0.78, int(q.vr)));
        }
    }
};

static std::string header(const Tracker& tr, double now) {
    std::string t = tr.json(now, false);
    return "{\"type\":\"adsb\"," + t.substr(1, t.size() - 2) + ",\"rate\":0,\"noise\":-40.0,\"msgRate\":" + std::to_string(int(now) % 7 + 1400) + ",\"dropped\":0,\"capture\":-1}";
}
static std::string full(const Tracker& tr, double now) {
    std::string t = tr.json(now, true);
    return "{\"type\":\"adsb\"," + t.substr(1, t.size() - 2) + ",\"rate\":0,\"noise\":-40.0,\"msgRate\":" + std::to_string(int(now) % 7 + 1400) + ",\"dropped\":0,\"capture\":-1}";
}

static int stream(int ticks) {
    Sky sky; Tracker tr; tr.setReceiver(52.29, -0.85); tr.setMaxRangeNm(360);
    delta::Encoder enc; enc.reset();
    const double start = 1000.37;             // the tracker clock at the first tick
    const double phi = start - 0.85;
    for (int k = 0; k < ticks; ++k) {
        const double now = start + k + (k % 3) * 0.04;   // a tick may be a little late (the shim's 100 ms polls)
        sky.second(k, now - 1.0, tr);
        tr.expire(now, 60);
        std::vector<delta::Row> rows;
        delta::rowsOf(tr, now, phi, rows);
        bool keyAll = false;
        const std::string d = enc.tick(std::move(rows), header(tr, now), (int64_t)std::floor(now - phi), keyAll);
        const std::string kf = enc.keyframe();
        std::printf("{\"tick\":%d,\"keyAll\":%s,\"full\":%s,\"d\":%s,\"k\":%s}\n", k, keyAll ? "true" : "false", full(tr, now).c_str(), d.c_str(), kf.c_str());
    }
    return 0;
}

static void unit() {
    std::printf("1. quantising from the printed text\n");
    CHECK(delta::fixedOf(52.123455, 5) == 5212346 || delta::fixedOf(52.123455, 5) == 5212345, "lat 5 dp");
    CHECK(delta::fixedOf(-0.000004, 5) == 0, "a tiny negative prints -0.00000 → 0");
    CHECK(delta::fixedOf(-12.04, 1) == -120 && delta::floorDiv(-120, 10) == -12, "rssi -12.04 → -12.0 → -12 (as the text)");
    CHECK(delta::floorDiv(-121, 10) == -13, "rssi -12.1 → -13 (rounded down: the bars' thresholds hold)");
    {   // ★★ beta96 the SNR column's 3 dB bands sit on ADS-B's colour edges (snrColour.ts adsb: red < 13, green ≥ 16): the
        //    value the client shows (11.5 + 3k) is always the same colour as the true figure
        auto colour = [](double v) { return v < 13 ? 0 : v < 16 ? 1 : 2; };
        int bad = 0;
        for (int t = -100; t <= 600; ++t) { const double v = t / 10.0; if (colour(11.5 + 3 * double(delta::snrBand(float(v)))) != colour(v)) ++bad; }
        CHECK(bad == 0, "SNR bands never change a colour (%d of 701 figures did)", bad);
        CHECK(delta::snrBand(12.9f) == 0 && delta::snrBand(13.0f) == 1 && delta::snrBand(15.95f) == 1 && delta::snrBand(16.0f) == 2 && delta::snrBand(9.9f) == -1,
              "the band edges: 13.0 and 16.0 start a band");
    }
    CHECK(delta::jsRound(4505, 10) == 451 && delta::jsRound(-4505, 10) == -450, "JS Math.round on tenths (half up)");
    CHECK(delta::jsRound(35012, 25) == 1400 && delta::jsRound(35013, 25) == 1401 && delta::jsRound(-1000, 25) == -40, "altitude in 25 ft");

    std::printf("2. the header walk\n");
    auto h = delta::topPairs("{\"type\":\"adsb\",\"now\":12.5,\"commb\":{\"a\":[1,2],\"b\":\"x,}\"},\"aircraft\":[{\"hex\":\"1\"}],\"gainWhy\":\"q\\\"r\",\"capture\":-1}");
    CHECK(h.size() == 4 && h[0].first == "now" && h[1].second == "{\"a\":[1,2],\"b\":\"x,}\"}" && h[2].second == "\"q\\\"r\"" && h[3].second == "-1",
          "keys and raw values, nested and escaped (%zu)", h.size());

    std::printf("3. the encoder\n");
    delta::Encoder e; e.reset();
    auto row = [](uint32_t key, int64_t lat, int64_t msgs, int64_t seenAt) {
        delta::Row r; r.key = key; char b[8]; std::snprintf(b, sizeof b, "%06x", key); r.hex = b;
        r.has = (1u << delta::cLat) | (1u << delta::cLon) | (1u << delta::cPosAt) | (1u << delta::cSeenAt) | (1u << delta::cMsgs);
        r.n[delta::cLat] = lat; r.n[delta::cLon] = -85000; r.n[delta::cPosAt] = seenAt; r.n[delta::cSeenAt] = seenAt; r.n[delta::cMsgs] = msgs;
        return r;
    };
    bool ka = false;
    std::string d1 = e.tick({row(1, 5229000, 10, 100), row(2, 5230000, 5, 100)}, "{\"now\":1}", 100, ka);
    CHECK(ka, "the first tick after a reset is a keyframe for everyone");
    CHECK(d1.find("\"n\":[0,\"000001\",19,5229000,-85000,10,1,\"000002\",19,5230000,-85000,5]") != std::string::npos,
          "new aircraft with ids 0, 1; the two times implied (= T): %s", d1.c_str());
    std::string d2 = e.tick({row(1, 5229150, 12, 101), row(2, 5230000, 5, 100)}, "{\"now\":2}", 101, ka);
    CHECK(!ka && d2 == "{\"type\":\"adsb_d\",\"s\":2,\"t\":101,\"h\":{\"now\":2},\"u\":[0,17,150,2]}",
          "a move: lat +150 and 2 more messages; both times implied (= T): %s", d2.c_str());
    std::string d3 = e.tick({row(2, 5230000, 5, 100), row(3, 5231000, 1, 99)}, "{\"now\":2}", 102, ka);
    CHECK(d3.find("\"g\":[0]") != std::string::npos && d3.find("\"n\":[0,\"000003\"") != std::string::npos,
          "gone frees id 0, and the new aircraft takes it in the same tick: %s", d3.find("\"h\"") == std::string::npos ? d3.c_str() : "(header resent!)");
    CHECK(d3.find("[0,\"000003\",31,5231000,-85000,99,99,1]}") != std::string::npos, "times not T are sent: %s", d3.c_str());
    std::string d4 = e.tick({row(2, 5230000, 5, 100), row(3, 5231000, 1, 99)}, "{\"now\":2}", 103, ka);
    CHECK(d4 == "{\"type\":\"adsb_d\",\"s\":4,\"t\":103}", "nothing changed → an empty tick: %s", d4.c_str());
    const std::string k = e.keyframe();
    CHECK(k.find("\"s\":4") != std::string::npos && k.find("\"a\":[1,\"000002\"") != std::string::npos && k.find("\"h\":{\"now\":2}") != std::string::npos,
          "the keyframe: the state after tick 4: %s", k.c_str());
    int keys = 0;
    for (int i = 0; i < 60; ++i) { e.tick({row(2, 5230000, 5, 100)}, "{}", 104 + i, ka); keys += ka; }
    CHECK(keys == 1, "a keyframe for everyone every 60 ticks (%d in 60)", keys);

    std::printf("4. a synthetic sky: sizes and cost\n");
    Sky sky; Tracker tr; tr.setReceiver(52.29, -0.85); tr.setMaxRangeNm(360);
    delta::Encoder enc; enc.reset();
    const double start = 1000.37, phi = start - 0.85;
    size_t fullB = 0, dB = 0, kB = 0, n = 0, ac = 0; double encUs = 0, fullUs = 0;
    for (int t = 0; t < 600; ++t) {
        const double now = start + t;
        sky.second(t, now - 1.0, tr);
        tr.expire(now, 60);
        auto c0 = std::chrono::steady_clock::now();
        std::vector<delta::Row> rows; delta::rowsOf(tr, now, phi, rows);
        const std::string d = enc.tick(std::move(rows), header(tr, now), (int64_t)std::floor(now - phi), ka);
        auto c1 = std::chrono::steady_clock::now();
        const std::string f = full(tr, now);
        auto c2 = std::chrono::steady_clock::now();
        if (t >= 120) {   // after the sky has settled
            fullB += f.size(); dB += d.size(); kB += enc.keyframe().size(); ++n; ac += tr.aircraft().size();
            encUs += std::chrono::duration<double, std::micro>(c1 - c0).count(); fullUs += std::chrono::duration<double, std::micro>(c2 - c1).count();
        }
    }
    const double perF = double(fullB) / n / 1024, perD = double(dB) / n / 1024, perK = double(kB) / n / 1024;
    std::printf("   %.0f aircraft: full list %.2f KB a tick, delta %.2f KB, keyframe %.2f KB → with a keyframe a minute %.2f KB/s\n",
                double(ac) / n, perF, perD, perK, perD + perK / 60);
    std::printf("   cost a tick: delta (rows + encode) %.0f µs, full list %.0f µs\n", encUs / n, fullUs / n);
    CHECK(perD + perK / 60 < 3.0, "≤ 3 KB/s at ~150 aircraft (%.2f)", perD + perK / 60);
    CHECK(perD < perF / 4, "the delta is under a quarter of the full list");
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--stream") == 0) return stream(argc > 2 ? std::atoi(argv[2]) : 900);
    unit();
    std::printf("\ntest-adsb-delta: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
