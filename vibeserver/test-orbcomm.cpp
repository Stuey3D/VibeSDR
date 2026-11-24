// test-orbcomm.cpp — the ORBCOMM downlink decoder (vibe_orbcomm.h) on synthetic signals from vibe_orbcomm_tx.h:
// the packet fields (Kenny's published examples), the framer (sync, ephemeris length, repair), the whole receiver at
// several offsets / Doppler slopes / clock errors / Es/N0, two carriers at once, and silence (nothing decoded).
// Silent (no audio). Run: scripts/run-tests.sh (VIBE_TESTS="test-orbcomm").
#include "vibe_orbcomm_tx.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) { g_pass++; } else { g_fail++; std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace vibeorb;

static Packet fromHex(const char* h) {
    Packet p; p.len = 0;
    for (const char* c = h; *c;) {
        while (*c == ' ') c++;
        if (!c[0] || !c[1]) break;
        unsigned v; std::sscanf(c, "%2x", &v); p.b[size_t(p.len++)] = uint8_t(v); c += 2;
    }
    p.ok = fletcherOk(p.b.data(), p.len);
    return p;
}

static void testFields() {
    std::printf("── packet fields (Kenny's published examples)\n");
    // his Appendix C6 sync packet: 65 a8f9 19 10 b0 90 000000 3061
    Packet s = fromHex("65 A8 F9 19 10 B0 90 00 00 00 30 61");
    CHECK(s.ok, "Kenny's sync packet passes Fletcher");
    SyncInfo si; CHECK(decodeSync(s, si) && si.id == 0x19 && si.channel == 0xB0 && si.frame == 9, "sync id %d ch %d frame %d", si.id, si.channel, si.frame);
    CHECK(std::fabs(downlinkHz(si.channel) - 137.44e6) < 1, "channel 176 = 137.4400 MHz");
    // the zero fill: 1E 00… B6 2C
    CHECK(fromHex("1E 00 00 00 00 00 00 00 00 00 B6 2C").ok, "zero fill passes");
    // uplink: 1B 10 54 35 F2 23 00 00 00 00 2B 0C → 565, 575 (149.4125 / 149.4375 MHz)
    Packet u = fromHex("1B 10 54 35 F2 23 00 00 00 00 2B 0C");
    UplinkInfo ui; CHECK(u.ok && decodeUplink(u, ui) && ui.channels.size() == 2 && ui.channels[0] == 565 && ui.channels[1] == 575 && ui.retries == 5 && ui.slots == 4,
                         "uplink %zu ch", ui.channels.size());
    CHECK(std::fabs(uplinkHz(575) - 149.4375e6) < 1, "uplink 575 = 149.4375 MHz");
    // downlink set: 1C 30 B0 00 05 64 00 14 1D 01 98 D1 → 176 80 100 320 285
    Packet d = fromHex("1C 30 B0 00 05 64 00 14 1D 01 98 D1");
    DownlinkInfo di; CHECK(d.ok && decodeDownlink(d, di) && di.count == 3 && di.index == 0 && di.channels.size() == 5 && di.channels[0] == 176 && di.channels[1] == 80
                           && di.channels[2] == 100 && di.channels[3] == 320 && di.channels[4] == 285, "downlink set");
    // elements: 22 02 02 D4 8F A4 80 82 CA F7 2F E1 → MA 231.4151°, MM 14.51900806
    Packet e = fromHex("22 02 02 D4 8F A4 80 82 CA F7 2F E1");
    ElementsInfo ei; CHECK(e.ok && decodeElements(e, ei) && ei.sc == 2 && std::fabs(ei.meanAnomalyDeg - 231.4151) < 0.001 && std::fabs(ei.meanMotionRevDay - 14.519008) < 1e-5,
                           "elements sc %d MA %.4f MM %.6f", ei.sc, ei.meanAnomalyDeg, ei.meanMotionRevDay);
    // ephemeris: his Appendix C1 line (FM-23, week 0x0420, TOW 0x04249F)
    Packet ep = fromHex("1F 17 6A BC E3 57 C6 0E E3 54 F4 56 F2 88 E2 D3 3C 9F 24 04 20 04 C3 FC");
    Ephemeris eph; CHECK(ep.ok && ep.len == 24, "Kenny's ephemeris passes Fletcher over 24 bytes");
    CHECK(decodeEphemeris(ep, eph) && eph.id == 0x17 && eph.week == 0x0420 && eph.tow == 0x04249F, "ephemeris id/week/tow %d %d %d", eph.id, eph.week, eph.tow);
    const double r = std::sqrt(eph.x * eph.x + eph.y * eph.y + eph.z * eph.z);
    CHECK(r > 7.0e6 && r < 7.3e6 && eph.speedKmS > 6.8 && eph.speedKmS < 7.8, "a LEO orbit: r %.0f km, v %.3f km/s, alt %.0f km", r / 1000, eph.speedKmS, eph.altKm);
    CHECK(eph.altKm > 650 && eph.altKm < 900, "altitude %.0f km", eph.altKm);
    // Fletcher round trip + the encoder over random data
    std::vector<uint8_t> q = tx::packet(kMessage, { 0x30, 1, 2, 3, 4, 5, 6, 7, 8 });
    CHECK(fletcherOk(q.data(), 12), "encoder → check");
    q[5] ^= 0x10; CHECK(!fletcherOk(q.data(), 12), "one flipped bit fails");
}

struct Got { std::map<int, int> types; int ok = 0, bad = 0, repaired = 0; std::vector<Rx> rx; };

static Got run(const std::vector<std::complex<float>>& x, double fs, int maxChans = 4) {
    Got g;
    Receiver::Cfg c; c.maxChans = maxChans;
    Receiver r(c);
    r.onPacket = [&](const Rx& p) { if (p.p.ok) { g.ok++; g.types[p.p.type()]++; if (p.p.repaired) g.repaired++; } else g.bad++; g.rx.push_back(p); };
    const int B = 8192;
    for (size_t i = 0; i < x.size(); i += B) r.feed(&x[i], int(std::min<size_t>(B, x.size() - i)), fs, 137.3e6, double(i) / fs);
    return g;
}

static std::vector<uint8_t> frames(int n, int id, int ch) {
    std::vector<uint8_t> all;
    const double pos[3] = { 3979776.0, -500000.0, 5900000.0 }, vel[3] = { -1000.0, 7000.0, 900.0 };
    for (int f = 0; f < n; f++) {
        std::vector<std::vector<uint8_t>> body;
        body.push_back(tx::packet(kUplink, { 0x10, 0x54, 0x35, 0xF2, 0x23 }));
        if (f % 4 == 0) body.push_back(tx::ephemerisPacket(id, pos, vel, 2394, 200000 + f));
        body.push_back(tx::packet(kDownlink, { 0x30, 0xB0, 0x00, 0x05, 0x64, 0x00, 0x14, 0x1D, 0x01 }));
        body.push_back(tx::packet(kMessage, { 0x30, 0x01, 0x82, 0xBD, 0x60, 0xC4, 0x88, 0x6B, 0xBB }));
        auto fr = tx::minorFrame(id, ch, f, body);
        all.insert(all.end(), fr.begin(), fr.end());
    }
    return all;
}

static void testReceiver(bool quick) {
    std::printf("── the receiver on synthetic SDPSK\n");
    const double fs = 250000;
    const std::vector<uint8_t> bytes = frames(quick ? 8 : 16, 0x2B, 0x73);
    struct Case { double off, slope, ppm, esn0; int minOk; const char* what; };
    const int n = int(bytes.size() / 12);
    const Case cs[] = {
        { 0, 0, 0, 25, n * 3 / 4, "clean, on the centre" },
        { -47500, 0, 0, 25, n * 3 / 4, "−47.5 kHz in the slice" },
        { 61234, -35, 40, 18, n * 7 / 10, "+61 kHz, −35 Hz/s Doppler, 40 ppm clock, 18 dB" },
        { -12000, 30, -60, 12, n * 6 / 10, "12 dB, +30 Hz/s, −60 ppm" },
    };
    for (const Case& c : cs) {
        tx::Signal s; s.fs = fs; s.offsetHz = c.off; s.slopeHzS = c.slope; s.clockPpm = c.ppm; s.esn0Db = c.esn0; s.seed = unsigned(c.off + 1000);
        auto x = tx::modulate(bytes, s);
        Got g = run(x, fs);
        int sync = g.types[kSync], eph = g.types[kEphemeris];
        std::printf("   %-48s ok %d (of ~%d) bad %d repaired %d · sync %d eph %d\n", c.what, g.ok, n, g.bad, g.repaired, sync, eph);
        CHECK(g.ok >= c.minOk, "%s: %d good packets (want ≥ %d)", c.what, g.ok, c.minOk);
        CHECK(sync >= 3 && eph >= 1, "%s: sync %d, ephemeris %d", c.what, sync, eph);
        bool rfOk = false;
        for (auto& r : g.rx) if (r.p.ok && std::fabs(r.rfHz - (137.3e6 + c.off + c.slope * r.t)) < 300) rfOk = true;
        CHECK(rfOk, "%s: the carrier's frequency is reported", c.what);
        // the decoded sync names the channel
        bool chOk = false;
        for (auto& r : g.rx) { SyncInfo si; if (r.p.ok && decodeSync(r.p, si) && si.channel == 0x73 && si.id == 0x2B) chOk = true; }
        CHECK(chOk, "%s: sync names channel 0x73 / ID 0x2B", c.what);
    }
    // two carriers at once (−80 kHz and +40 kHz, different satellites)
    {
        tx::Signal a; a.fs = fs; a.offsetHz = -80000; a.esn0Db = 20; a.seed = 7;
        tx::Signal b = a; b.offsetHz = 40000; b.seed = 8; b.amp = 0.15;
        auto xa = tx::modulate(frames(quick ? 6 : 10, 0x21, 0x64), a);
        auto xb = tx::modulate(frames(quick ? 6 : 10, 0x22, 0x7D), b);
        for (size_t i = 0; i < xa.size() && i < xb.size(); i++) xa[i] += xb[i];
        Got g = run(xa, fs);
        std::map<int, int> ids;
        for (auto& r : g.rx) { SyncInfo si; if (r.p.ok && decodeSync(r.p, si)) ids[si.id]++; }
        std::printf("   two carriers: ok %d · sync from 0x21 ×%d, 0x22 ×%d\n", g.ok, ids[0x21], ids[0x22]);
        CHECK(ids[0x21] >= 2 && ids[0x22] >= 2, "both satellites' sync packets");
    }
    // noise only: nothing passes
    {
        tx::Signal s; s.fs = fs; s.amp = 0; s.esn0Db = -100; s.seed = 3;
        std::vector<uint8_t> z(600 * 6, 0);
        auto x = tx::modulate(z, s);
        for (auto& v : x) v = std::complex<float>(v.real() * 0 + 0.01f * float((rand() % 2001) - 1000) / 1000.f, 0.01f * float((rand() % 2001) - 1000) / 1000.f);
        Got g = run(x, fs);
        CHECK(g.ok == 0, "noise: %d packets passed", g.ok);
    }
}

/** ★ The x⁴ check that a scanner peak IS an SDPSK carrier (the first raw capture: 268 "carriers" from the RTL's ripple). */
static void testCarrierCheck() {
    std::printf("── carrier check (x⁴ line over the comb)\n");
    std::vector<uint8_t> all;
    for (int f = 0; f < 2; f++) { auto fr = tx::minorFrame(0x2B, 0x73, f, {}); all.insert(all.end(), fr.begin(), fr.end()); }
    auto ratio = [](const std::vector<std::complex<float>>& x, double fs, double off, bool& rej) {
        Chan::Cfg c; c.fsIn = fs; c.offsetHz = off; Chan ch(c);
        for (size_t i = 0; i < x.size() && !ch.rejected(); i += 4096) ch.feed(&x[i], int(std::min<size_t>(4096, x.size() - i)));
        rej = ch.rejected(); return ch.acqRatio();
    };
    tx::Signal s; s.fs = 250000; s.offsetHz = 20000; s.esn0Db = 6; s.seed = 11;
    bool rej = true; const double r6 = ratio(tx::modulate(all, s), s.fs, 20600, rej);
    CHECK(!rej, "a 6 dB carrier is accepted (x⁴ ratio %.1f, step %s)", r6, "≥ 0.72");
    s.amp = 0; s.esn0Db = -100;
    auto noise = tx::modulate(all, s);
    std::mt19937 g(4); std::normal_distribution<float> n(0, 0.05f);
    for (auto& v : noise) v = { n(g), n(g) };
    int rejected = 0;
    for (double off : { -90000.0, -30000.0, 0.0, 45000.0, 100000.0 }) { bool r = false; ratio(noise, s.fs, off, r); rejected += r; }
    CHECK(rejected == 5, "noise is rejected at every offset (%d of 5)", rejected);
    // a steady CW spur (C/N 10 dB in 6.7 kHz) handed straight to a carrier: refused, and as a SPUR (the longer ban)
    {
        std::vector<std::complex<float>> x = noise;
        const double a = 0.05 * std::sqrt(2.0 * s.fs / 6720.0 * 10.0);
        for (size_t i = 0; i < x.size(); i++) x[i] += std::complex<float>(std::polar(a, 2 * M_PI * 30000.0 * double(i) / s.fs));
        Chan::Cfg c; c.fsIn = s.fs; c.offsetHz = 30200; Chan ch(c);
        for (size_t i = 0; i < x.size() && !ch.rejected(); i += 4096) ch.feed(&x[i], int(std::min<size_t>(4096, x.size() - i)));
        CHECK(ch.rejected() && ch.spur(), "a CW spur is refused as a steady line (step %.2f)", ch.stepMean());
    }
}

/** ★★ LIKE THE LIVE FM116 PASS (2026-11-24 15:09, Pi 500 V4 + YouLoop + LNA): the carrier at ~6.5 dB with a pass's Doppler
 *  rate and an oscillator's wander, a steady spur 6 kHz beside it and two more across the slice (the RTL / LNA lines the
 *  live log kept finding at 137.474 / 137.282 / 137.666). The carrier must decode; no spur may ever lock or yield a packet. */
static void testLiveLike(bool quick) {
    std::printf("── live-like: 6.5 dB, −30 Hz/s, phase wander, spurs beside and across\n");
    const double fs = 1024000, centre = 137.5e6;
    std::vector<uint8_t> bytes;
    for (int f = 0; f < (quick ? 12 : 20); f++) { auto fr = tx::minorFrame(0x2E, 0x0A, f, { tx::packet(kUplink, { 0x10, 0x54, 0x35, 0xF2, 0x23 }) }); bytes.insert(bytes.end(), fr.begin(), fr.end()); }
    const double carrier = 160000;   // 137.660 MHz
    tx::Signal s; s.fs = fs; s.offsetHz = carrier; s.slopeHzS = -30; s.esn0Db = 6.5; s.clockPpm = 30; s.phaseWalkHz = 20; s.seed = 21;
    auto x = tx::modulate(bytes, s);
    // the spurs: C/N (in 6.7 kHz) 10, 6 and 4 dB; the noise per component the modulator used: N0·fs/2 with N0 = Es/snr
    const double es = s.amp * s.amp / kBaud, n0 = es / std::pow(10.0, s.esn0Db / 10.0);
    const struct { double off, cn; } sp[] = { { carrier + 6000, 10 }, { -26000, 6 }, { -218000, 4 } };
    for (size_t i = 0; i < x.size(); i++)
        for (auto& q : sp) x[i] += std::complex<float>(std::polar(std::sqrt(n0 * 6720 * std::pow(10.0, q.cn / 10.0)), 2 * M_PI * q.off * double(i) / fs));
    Receiver::Cfg c; c.maxChans = 4; Receiver r(c);
    int ok = 0, fromSpur = 0;
    r.onPacket = [&](const Rx& p) { if (!p.p.ok) return; ok++; if (std::fabs(p.rfHz - (centre + carrier)) > 3000) fromSpur++; };
    bool spurLocked = false, spurOpened = false;
    for (size_t i = 0; i < x.size(); i += 16384) {
        r.feed(&x[i], int(std::min<size_t>(16384, x.size() - i)), fs, centre, double(i) / fs);
        for (auto& ch : r.channels()) { if (ch.locked && std::fabs(ch.rfHz - (centre + carrier)) > 3000) spurLocked = true; for (auto& q : sp) if (std::fabs(ch.rfHz - centre - q.off) < 1000) spurOpened = true; }
    }
    const int n = int(bytes.size() / 12);
    std::printf("   good %d of ~%d · spurs refused %llu · peaks without an x⁴ line %llu\n", ok, n, (unsigned long long)r.spursRejected(), (unsigned long long)r.notSdpskRejected());
    CHECK(ok > n / 3, "the live-like carrier decodes (%d of ~%d)", ok, n);
    CHECK(fromSpur == 0 && !spurLocked, "no spur locks or yields a packet (%d)", fromSpur);
    CHECK(!spurOpened, "no carrier is ever opened on a spur (the scanner cuts narrow lines)");
}

/** ★★★ REGRESSION — the live FM116 pass, 2026-11-24 16:46 (build 894c2abd7): a carrier with a steady spur ON it was never
 *  opened again — the x⁴ score's centre slot took the spur, the steps read 0°, the carrier was refused and its place banned
 *  (doubling, up to 5 minutes). A strong carrier with a pass's Doppler and a spur on it (and the spur comb across the slice)
 *  must open and LOCK from a cold start within 3 s, and no refusal may ban a place for more than kMaxBanSec. */
static void testSpurOnCarrier() {
    std::printf("── regression: a spur ON the carrier (live FM116 16:46)\n");
    for (double esn0 : { 20.0, 8.0 }) {
        const double fs = 1024000, centre = 137.5e6, carrier = 162500;   // 137.6625 MHz
        std::vector<uint8_t> bytes;
        for (int f = 0; f < 8; f++) { auto fr = tx::minorFrame(0x2E, 0x0A, f, {}); bytes.insert(bytes.end(), fr.begin(), fr.end()); }
        tx::Signal s; s.fs = fs; s.offsetHz = carrier + 1500; s.slopeHzS = -30; s.esn0Db = esn0; s.clockPpm = 30; s.phaseWalkHz = 5; s.seed = 31;
        auto x = tx::modulate(bytes, s);
        const double es = s.amp * s.amp / kBaud, n0 = es / std::pow(10.0, 8.0 / 10.0);   // the spurs' level is the 8 dB case's
        // the spur on the channel (137.666, C/N −4 dB in 6.7 kHz — the live comb's +10 dB in a 250 Hz bin) and the comb every 40 kHz
        std::vector<std::pair<double, double>> sp = { { 166000, -4 } };
        for (double o = -480000; o < 480000; o += 40000) sp.push_back({ o + 20000, -4 });   // the comb, none on the channel
        for (size_t i = 0; i < x.size(); i++)
            for (auto& q : sp) x[i] += std::complex<float>(std::polar(std::sqrt(n0 * 6720 * std::pow(10.0, q.second / 10.0)), 2 * M_PI * q.first * double(i) / fs));
        Receiver r;
        int ok = 0; double firstOk = -1;
        r.onPacket = [&](const Rx& p) { if (p.p.ok) { ok++; if (firstOk < 0) firstOk = p.t; } };
        for (size_t i = 0; i < x.size(); i += 16384) r.feed(&x[i], int(std::min<size_t>(16384, x.size() - i)), fs, centre, double(i) / fs);
        std::printf("   %2.0f dB: good %d, first at %.1f s, refused spurs %llu / not SDPSK %llu\n", esn0, ok, firstOk,
                    (unsigned long long)r.spursRejected(), (unsigned long long)r.notSdpskRejected());
        CHECK(firstOk >= 0 && firstOk < 3.0, "%.0f dB with a spur on it: locked from a cold start in under 3 s (%.1f)", esn0, firstOk);
        CHECK(ok > int(bytes.size() / 12) / 2, "%.0f dB: most packets decode (%d)", esn0, ok);
    }
    bool capped = true;
    for (int k = 0; k < 40; k++) for (bool spur : { false, true }) if (Receiver::banFor(k, spur, 12) > Receiver::kMaxBanSec) capped = false;
    CHECK(capped && Receiver::kMaxBanSec <= 60, "no refusal bans a place for more than %.0f s", Receiver::kMaxBanSec);
}

static void testSensitivity() {
    std::printf("── sensitivity (Es/N0 → good packets)\n");
    const double fs = 100000;
    const std::vector<uint8_t> bytes = frames(10, 0x2B, 0x73);
    const int n = int(bytes.size() / 12);
    for (double e : { 6.0, 8.0, 10.0, 12.0 }) {
        tx::Signal s; s.fs = fs; s.offsetHz = 3000; s.esn0Db = e; s.seed = unsigned(e * 10);
        Got g = run(tx::modulate(bytes, s), fs);
        std::printf("   Es/N0 %4.1f dB: %3d of ~%d good (%d repaired)\n", e, g.ok, n, g.repaired);
        if (e >= 10) CHECK(g.ok >= n / 2, "Es/N0 %.0f dB: %d good", e, g.ok);
    }
}

int main(int argc, char** argv) {
    const bool quick = argc > 1 && std::strcmp(argv[1], "--quick") == 0;
    testFields();
    testReceiver(quick);
    testCarrierCheck();
    testLiveLike(quick);
    testSpurOnCarrier();
    if (!quick) testSensitivity();
    std::printf("test-orbcomm: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
