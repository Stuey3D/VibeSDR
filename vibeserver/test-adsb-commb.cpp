// test-adsb-commb.cpp — vibe_adsb_commb.h: Mode S Comm-B (DF20/21) registers on the worked examples of "The 1090
// Megahertz Riddle" (Junzi Sun, 2nd ed.), then the inference rules through vibe_adsb.h's Tracker on synthetic replies:
// cross-checks against the aircraft's own ADS-B, the second-reply confirmation, data parity, the 1,7 capability veto,
// an ACAS advisory, a Mode S-only aircraft named by 2,0, and random MB fields (how often noise would be believed).
#include "vibe_adsb.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <string>

using namespace vibeadsb;
using namespace vibeadsb::commb;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); ++fails; } } while (0)

static Frame hexFrame(const char* h) {
    Frame f;
    f.len = int(std::strlen(h) / 2);
    for (int i = 0; i < f.len; ++i) { unsigned v; std::sscanf(h + 2 * i, "%2x", &v); f.b[i] = uint8_t(v); }
    return f;
}
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
static uint64_t mbHex(const char* h) { Frame f = hexFrame(h); return mbOf(f.b); }

/** A DF20 (or 21) reply carrying `mbv` from `icao`: altitude code 0 (absent), parity = CRC XOR address — or, with
 *  `dpCode`, the address XOR the register number in its top 8 bits (data parity). */
static Frame cbFrame(uint32_t icao, uint64_t mbv, int df = 20, int dpCode = -1) {
    Frame f; f.len = 14;
    f.b[0] = uint8_t(df << 3);
    for (int k = 0; k < 7; ++k) f.b[4 + k] = uint8_t(mbv >> (48 - 8 * k));
    uint32_t ap = icao;
    if (dpCode >= 0) ap ^= uint32_t(dpCode) << 16;
    const uint32_t p = syndrome(f.b, 14) ^ ap;               // parity bytes are 0: this is the CRC
    f.b[11] = uint8_t(p >> 16); f.b[12] = uint8_t(p >> 8); f.b[13] = uint8_t(p);
    return f;
}
/** MB bits a..b set to x. */
static void put(uint64_t& v, int a, int b, uint64_t x) {
    const int n = b - a + 1;
    const uint64_t mask = ((1ull << n) - 1) << (56 - b);
    v = (v & ~mask) | ((x << (56 - b)) & mask);
}
static uint64_t mb50(double roll, double trk, int gs, double rate, int tas) {
    uint64_t v = 0;
    auto sgnf = [](double x, int bitsN) { int i = int(std::lround(x)); if (i < 0) i += 1 << bitsN; return uint64_t(i) & ((1ull << bitsN) - 1); };
    put(v, 1, 1, 1); put(v, 2, 11, sgnf(roll * 256.0 / 45.0, 10));
    const double t = trk > 180 ? trk - 360 : trk;
    put(v, 12, 12, 1); put(v, 13, 23, sgnf(t * 512.0 / 90.0, 11));
    put(v, 24, 24, 1); put(v, 25, 34, uint64_t(gs / 2));
    put(v, 35, 35, 1); put(v, 36, 45, sgnf(rate * 256.0 / 8.0, 10));
    put(v, 46, 46, 1); put(v, 47, 56, uint64_t(tas / 2));
    return v;
}
static uint64_t mb60(double hdg, int ias, double mach, int vr) {
    uint64_t v = 0;
    auto sgnf = [](double x, int bitsN) { int i = int(std::lround(x)); if (i < 0) i += 1 << bitsN; return uint64_t(i) & ((1ull << bitsN) - 1); };
    const double h = hdg > 180 ? hdg - 360 : hdg;
    put(v, 1, 1, 1); put(v, 2, 12, sgnf(h * 512.0 / 90.0, 11));
    put(v, 13, 13, 1); put(v, 14, 23, uint64_t(ias));
    put(v, 24, 24, 1); put(v, 25, 34, uint64_t(std::lround(mach * 512.0 / 2.048)));
    put(v, 35, 35, 1); put(v, 36, 45, sgnf(vr / 32.0, 10));
    put(v, 46, 46, 1); put(v, 47, 56, sgnf(vr / 32.0, 10));
    return v;
}
/** DF17 TC19 subtype 1: ground speed `gs` kt on `trk` degrees, `vr` ft/min. */
static Frame velFrame(uint32_t icao, double gs, double trk, int vr) {
    const double r = trk * 3.14159265358979323846 / 180.0;
    const double ve = gs * std::sin(r), vn = gs * std::cos(r);
    uint64_t me = 0;
    auto set = [&](int a, int b, uint64_t x) { const int n = b - a + 1; me |= (x & ((1ull << n) - 1)) << (56 - b); };
    set(1, 5, 19); set(6, 8, 1);
    set(14, 14, ve < 0); set(15, 24, uint64_t(std::lround(std::fabs(ve))) + 1);
    set(25, 25, vn < 0); set(26, 35, uint64_t(std::lround(std::fabs(vn))) + 1);
    set(37, 37, vr < 0); set(38, 46, uint64_t(std::abs(vr) / 64) + 1);
    Frame f; f.len = 14;
    f.b[0] = 0x8D; f.b[1] = uint8_t(icao >> 16); f.b[2] = uint8_t(icao >> 8); f.b[3] = uint8_t(icao);
    for (int k = 0; k < 7; ++k) f.b[4 + k] = uint8_t(me >> (48 - 8 * k));
    const uint32_t p = syndrome(f.b, 14);
    f.b[11] = uint8_t(p >> 16); f.b[12] = uint8_t(p >> 8); f.b[13] = uint8_t(p);
    return f;
}
/** DF4 surveillance altitude reply at `ft` (25 ft code), from `icao`. */
static Frame altFrame(uint32_t icao, int ft) {
    const uint32_t n = uint32_t((ft + 1000) / 25);
    // 13-bit AC: M (bit 7 of 13) = 0, Q (bit 9) = 1 — the 11 remaining bits are n
    const uint32_t ac = ((n >> 5) << 7) | (((n >> 4) & 1) << 5) | 0x10 | (n & 0xF);
    Frame f; f.len = 7;
    f.b[0] = uint8_t(4 << 3);
    f.b[2] = uint8_t(ac >> 8); f.b[3] = uint8_t(ac);
    const uint32_t p = syndrome(f.b, 7) ^ icao;
    f.b[4] = uint8_t(p >> 16); f.b[5] = uint8_t(p >> 8); f.b[6] = uint8_t(p);
    return f;
}
/** An aircraft known to the tracker (a DF11 all-call, remainder 0). */
static Frame allCall(uint32_t icao) {
    Frame f; f.len = 7;
    f.b[0] = uint8_t(11 << 3) | 5;
    f.b[1] = uint8_t(icao >> 16); f.b[2] = uint8_t(icao >> 8); f.b[3] = uint8_t(icao);
    const uint32_t p = syndrome(f.b, 7);
    f.b[4] = uint8_t(p >> 16); f.b[5] = uint8_t(p >> 8); f.b[6] = uint8_t(p);
    return f;
}
static bool has(const std::string& s, const char* k) { return s.find(k) != std::string::npos; }

int main() {
    // ── 1. The book's worked examples (each frame's CRC remainder is a real aircraft address) ─────────────────
    {
        std::string cs;
        Frame f = hexFrame("A000083E202CC371C31DE0AA1CCF");
        CHECK(syndrome(f.b, 14) == 0x484163, "2,0 example address 484163");
        CHECK(dec20(mbOf(f.b), cs) && cs == "KLM1017", "2,0 callsign KLM1017 (got %s)", cs.c_str());
        CHECK(dec20(mbHex("A0001838201584F23468207CDFA5"), cs) && cs == "EXS2MF", "2,0 callsign EXS2MF (got %s)", cs.c_str());

        Bds40 a;
        CHECK(dec40(mbHex("A000029C85E42F313000007047D3"), a) && a.mcpFt == 3000 && a.fmsFt == 3000 && near(a.baroHpa, 1020.0, 0.05),
              "4,0 MCP 3000 / FMS 3000 / 1020.0 hPa (got %d %d %.1f)", a.mcpFt, a.fmsFt, a.baroHpa);
        Bds50 b;
        CHECK(dec50(mbHex("A000139381951536E024D4CCF6B5"), b) && near(b.roll, 2.1, 0.02) && near(b.track, 114.258, 0.001)
              && b.gs == 438 && near(b.rate, 0.125, 1e-6) && b.tas == 424,
              "5,0 roll 2.1 / track 114.258 / GS 438 / rate 0.125 / TAS 424 (got %.2f %.3f %d %.3f %d)", b.roll, b.track, b.gs, b.rate, b.tas);
        Bds60 c;
        CHECK(dec60(mbHex("A00004128F39F91A7E27C46ADC21"), c) && near(c.heading, 42.715, 0.001) && c.ias == 252
              && near(c.mach, 0.42, 1e-3) && c.vrBaro == -1920 && c.vrIns == -1920,
              "6,0 heading 42.715 / IAS 252 / M0.42 / -1920 / -1920 (got %.3f %d %.3f %d %d)", c.heading, c.ias, c.mach, c.vrBaro, c.vrIns);
        Bds44 d;
        CHECK(dec44(mbHex("A0001692185BD5CF400000DFC696"), d) && d.windKt == 22 && near(d.windDir, 344.5, 0.05) && near(d.tempC, -48.75, 1e-6),
              "4,4 wind 22 kt from 344.5, -48.75 °C (got %d %.1f %.2f)", d.windKt, d.windDir, d.tempC);
        Bds17 e;
        CHECK(dec17(mbHex("A0000638FA81C10000000081A92F"), e) && caps17(e, 0x20) && caps17(e, 0x40) && caps17(e, 0x50) && caps17(e, 0x60)
              && !caps17(e, 0x44), "1,7 capability flags (got %06x)", e.caps);
        Bds10 g;
        CHECK(dec10(mbHex("A800178D10010080F50000D5893C"), g), "1,0 data link capability");
        // the inference picks each of them alone, with no ADS-B reference at all
        const char* ex[] = {"A000083E202CC371C31DE0AA1CCF", "A000029C85E42F313000007047D3", "A000139381951536E024D4CCF6B5",
                            "A00004128F39F91A7E27C46ADC21", "A0001692185BD5CF400000DFC696", "A0000638FA81C10000000081A92F",
                            "A800178D10010080F50000D5893C"};
        const int want[] = {R20, R40, R50, R60, R44, R17, R10};
        for (int i = 0; i < 7; ++i) {
            const Inference in = infer(mbHex(ex[i]), Ref{});
            CHECK(in.reg == want[i], "example %s inferred as %s (got %s)", ex[i], regName(want[i]), in.reg >= 0 ? regName(in.reg) : "none");
        }
        // the 6,0 example with its own altitude: IAS 252 kt at M0.42 is ~ 9 000 ft ISA — the check agrees there, not at FL350
        Ref r; r.hasAlt = true; r.altFt = 9000;
        CHECK(infer(mbHex("A00004128F39F91A7E27C46ADC21"), r).reg == R60, "6,0 example agrees with 9 000 ft");
        r.altFt = 35000;
        CHECK(infer(mbHex("A00004128F39F91A7E27C46ADC21"), r).reg != R60, "6,0 example refused at FL350 (IAS/Mach disagree)");
    }

    // ── 2. Through the tracker: an aircraft with ADS-B velocity, a 5,0 that agrees is shown at once ────────────
    const uint32_t A = 0x3C4DD2;
    {
        Tracker t;
        CHECK(t.accept(velFrame(A, 438, 114, 0), 0), "ADS-B velocity accepted");
        CHECK(t.accept(altFrame(A, 37000), 0.1), "DF4 altitude accepted");
        CHECK(t.accept(cbFrame(A, mb50(2, 115, 436, 0.1, 424)), 1), "5,0 reply accepted");
        const std::string j = t.json(1);
        CHECK(has(j, "\"commb\":{\"roll\":1.9") && has(j, "\"true_track\":115.0") && has(j, "\"tas\":424"), "5,0 agreeing with ADS-B is shown (%s)", j.c_str());
        // a 5,0-shaped field 180 kt off the aircraft's own ground speed: not 5,0
        Tracker u;
        u.accept(velFrame(A, 438, 114, 0), 0);
        u.accept(cbFrame(A, mb50(2, 115, 250, 0.1, 240)), 1);
        u.accept(cbFrame(A, mb50(2, 115, 250, 0.1, 240)), 2);
        CHECK(!has(u.json(2), "\"aircraft\":[{\"hex\":\"3c4dd2\",\"commb\"") && !has(u.json(2), "true_track"), "5,0 disagreeing with ADS-B is refused");
    }
    // ── 3. No ADS-B reference: shown only after a second agreeing reply ───────────────────────────────────────
    {
        Tracker t;
        t.accept(allCall(A), 0);
        t.accept(cbFrame(A, mb60(250, 280, 0.78, -1000)), 1);
        CHECK(!has(t.json(1), "mag_heading"), "one 6,0 with nothing to check it against: not shown");
        t.accept(cbFrame(A, mb60(252, 281, 0.78, -1024)), 5);
        const std::string j = t.json(5);
        CHECK(has(j, "\"mag_heading\":252") && has(j, "\"ias\":281") && has(j, "\"mach\":0.78"), "second agreeing 6,0 confirms it (%s)", j.c_str());
        // and it ages out
        CHECK(!has(t.json(40), "mag_heading"), "6,0 older than 30 s is dropped");
        // a second reply that does NOT agree replaces the pending one instead of confirming
        Tracker u;
        u.accept(allCall(A), 0);
        u.accept(cbFrame(A, mb60(250, 280, 0.78, -1000)), 1);
        u.accept(cbFrame(A, mb60(70, 150, 0.30, 2000)), 2);
        CHECK(!has(u.json(2), "mag_heading"), "two disagreeing 6,0 replies: neither shown");
    }
    // ── 4. Data parity: the reply's parity names the register ─────────────────────────────────────────────────
    {
        Tracker t;
        t.accept(allCall(A), 0);
        CHECK(t.accept(cbFrame(A, mb60(10, 300, 0.80, 0), 20, 0x60), 1), "data-parity reply accepted for a known aircraft");
        CHECK(has(t.json(1), "\"mag_heading\":10"), "data parity confirms 6,0 at once");
        CHECK(t.commbStats().dataParity == 1, "counted as data parity");
        Tracker u;   // an unknown aircraft is not found by the overlay either
        u.accept(allCall(0x123456), 0);
        CHECK(!u.accept(cbFrame(A, mb60(10, 300, 0.80, 0), 20, 0x60), 1), "data-parity reply from an unknown aircraft refused");
    }
    // ── 5. 1,7 says the aircraft has no 6,0: a 6,0-shaped field is not believed ──────────────────────────────
    {
        Tracker t;
        t.accept(allCall(A), 0);
        uint64_t caps = 0; put(caps, 7, 7, 1); put(caps, 9, 9, 1);   // 2,0 and 4,0 only
        t.accept(cbFrame(A, caps), 1);
        t.accept(cbFrame(A, mb60(250, 280, 0.78, -1000)), 2);
        t.accept(cbFrame(A, mb60(250, 280, 0.78, -1000)), 3);
        const std::string j = t.json(3);
        CHECK(!has(j, "mag_heading") && has(j, "\"bds\":[\"4,0\"]"), "capability report vetoes an inferred 6,0 (%s)", j.c_str());
    }
    // ── 6. A Mode S-only aircraft (no ADS-B) named by 2,0, and an ACAS advisory ───────────────────────────────
    {
        Tracker t;
        t.accept(allCall(0x484163), 0);
        t.accept(hexFrame("A000083E202CC371C31DE0AA1CCF"), 1);
        CHECK(t.aircraft().at(0x484163).callsign == "KLM1017", "2,0 names an aircraft that sends no identification");
        CHECK(has(t.json(1), "\"flight\":\"KLM1017\"") && has(t.json(1), "\"ident\":\"KLM1017\""), "callsign in the JSON");
        uint64_t ra = 0; put(ra, 1, 8, 0x30); put(ra, 9, 9, 1); put(ra, 10, 10, 1); put(ra, 15, 15, 1);   // corrective, up, positive
        put(ra, 29, 30, 1); put(ra, 31, 54, 0x4CA123);
        t.accept(cbFrame(0x484163, ra, 21), 2);
        const std::string j = t.json(3);
        CHECK(has(j, "\"acas_ra\":{\"age\":1,\"active\":true") && has(j, "\"advisory\":\"Climb\"") && has(j, "\"threat\":\"4ca123\""),
              "ACAS RA: Climb, threat 4CA123 (%s)", j.c_str());
        CHECK(!has(t.json(70), "acas_ra"), "the RA leaves after a minute");
    }
    // ── 7. Noise: random MB fields from a known aircraft with a full ADS-B reference. How many are believed? ──
    {
        std::mt19937_64 rng(7);
        const int N = 200000;
        int shown = 0, identified = 0, amb = 0;
        Tracker t;
        t.accept(velFrame(A, 440, 90, 0), 0);
        t.accept(altFrame(A, 36000), 0);
        double now = 1;
        for (int i = 0; i < N; ++i) {
            const uint64_t v = rng() & ((1ull << 56) - 1);
            Ref r; r.hasGs = true; r.gs = 440; r.hasTrack = true; r.track = 90; r.hasAlt = true; r.altFt = 36000; r.hasVr = true; r.vr = 0;
            const Inference in = infer(v, r);
            if (in.ambiguous) ++amb;
            if (in.reg >= 0) ++identified;
            if (in.reg >= 0 && in.confirmed) ++shown;
        }
        (void)now;
        printf("  random MB fields: %d of %d pass one register's checks (%.3f %%), %d confirmed by ADS-B alone, %d ambiguous\n",
               identified, N, 100.0 * identified / N, shown, amb);
        CHECK(identified < N / 100, "fewer than 1 %% of random fields fit a register (got %d)", identified);
        CHECK(shown < N / 2000, "fewer than 0.05 %% of random fields confirmed alone (got %d)", shown);
    }
    // ── 8. Bounded: the per-aircraft state is made only for aircraft that answer Comm-B ───────────────────────
    {
        printf("  per-aircraft Comm-B state: %zu bytes (only for aircraft that answered a radar)\n", sizeof(State));
        CHECK(sizeof(State) < 8192, "Comm-B state under 8 KB an aircraft");
        Tracker t;
        t.accept(allCall(A), 0);
        CHECK(!t.aircraft().at(A).cb, "no Comm-B state until a reply");
    }

    // ── 9. The owner's switch off: the reply is still the aircraft's (altitude/squawk), the MB field is not read ──
    {
        Tracker t;
        t.setCommB(false);
        t.accept(allCall(A), 0);
        CHECK(t.accept(cbFrame(A, mb60(10, 300, 0.80, 0)), 1), "switched off: the reply still counts for the aircraft");
        CHECK(!t.accept(cbFrame(A, mb60(10, 300, 0.80, 0), 20, 0x60), 1), "switched off: no data-parity lookup either");
        CHECK(!t.aircraft().at(A).cb && t.commbStats().fields == 0, "switched off: nothing read, nothing kept");
    }

    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
