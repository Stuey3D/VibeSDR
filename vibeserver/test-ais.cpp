// test-ais.cpp — V12 AIS (vibe_hdlc.h, vibe_ais.h, vibe_ais_service.h), docs/v12/AIS-APRS.md.
//
//  1. The HDLC layer: the FCS-16, stuffing round trips, a flipped level repaired, a frame that must not be "repaired".
//  2. The messages against PUBLISHED worked examples (the AIVDM sentences in the gpsd AIVDM/AIVDO protocol notes), then
//     every type we read encoded from M.1371 Annex 8 and decoded back, field by field.
//  3. The whole receiver on synthetic captures: GMSK BT 0.4 at 9600 bit/s, NRZI, HDLC, on BOTH channels at once, with a
//     crystal offset, at 2.4 and 2.048 MS/s, over a range of Eb/N0 — the decode rate per SNR, with and without repair.
//  3b. The real world, harsher: crystal −9…+6 kHz, ±500 Hz transmitters with an 8-bit ramp and a settling synthesiser,
//     a second path, FM on both adjacent channels, a stronger ship in the slot before (`--conditions`: the full table).
//  4. Noise only, ten minutes of channel bits: ZERO frames.
//  5. The service: one row per MMSI merged from several messages, the readable line, the JSON, the trail, the trend.
//  6. CPU: real time per capture second at 2.4 MS/s (this Mac; the Pi 2 is estimated in the doc).
//  `test-ais --write-clip <file.cu8> <rate>` writes a capture (RTL's u8 IQ, centred on 162.000 MHz) for the server test.
//  `test-ais --cu8 <file.cu8> <rate>` decodes a REAL rtl_sdr capture centred on 162.000 MHz (AIS_DEBUG=1: every frame);
//  `test-ais --wav <file.wav> <channelHz>` one channel from a 16-bit stereo IQ WAV (AIS_WAV_NOISE=σ adds noise).
//  Experiment switches after the mode word: disc nocoh noafc W= Wf= ph= dph= (vibeais::Tuning).
#include "vibe_ais_service.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>
#include <algorithm>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using vibeais::cf;

// ── a message builder (MSB-first bits, M.1371 Annex 8 layout) ───────────────────────────────────────────────────────
struct Builder {
    std::vector<int> bits;
    void u(uint64_t v, int len) { for (int i = len - 1; i >= 0; --i) bits.push_back(int((v >> i) & 1)); }
    void s(int64_t v, int len) { u(uint64_t(v) & ((1ull << len) - 1), len); }
    void text(const std::string& t, int chars) {
        for (int i = 0; i < chars; ++i) {
            int c = i < int(t.size()) ? t[size_t(i)] : '@';
            int v = c >= 64 ? c - 64 : c;
            u(uint64_t(v & 63), 6);
        }
    }
    std::vector<uint8_t> bytes() const {
        std::vector<uint8_t> b((bits.size() + 7) / 8, 0);
        for (size_t k = 0; k < bits.size(); ++k) if (bits[k]) b[k / 8] |= uint8_t(1 << (7 - k % 8));
        return b;
    }
};
static std::vector<uint8_t> msgType1(uint32_t mmsi, double lat, double lon, double sog, double cog, int hdg, int nav, int rot = 0) {
    Builder B; B.u(1, 6); B.u(0, 2); B.u(mmsi, 30); B.u(uint64_t(nav), 4); B.s(rot, 8); B.u(uint64_t(std::lround(sog * 10)), 10); B.u(1, 1);
    B.s(std::lround(lon * 600000), 28); B.s(std::lround(lat * 600000), 27); B.u(uint64_t(std::lround(cog * 10)), 12); B.u(uint64_t(hdg), 9);
    B.u(30, 6); B.u(0, 2); B.u(0, 3); B.u(0, 1); B.u(0x1234, 19);
    return B.bytes();
}
static std::vector<uint8_t> msgType5(uint32_t mmsi, const std::string& name, const std::string& call, const std::string& dest, int type) {
    Builder B; B.u(5, 6); B.u(0, 2); B.u(mmsi, 30); B.u(0, 2); B.u(9134270, 30); B.text(call, 7); B.text(name, 20); B.u(uint64_t(type), 8);
    B.u(120, 9); B.u(30, 9); B.u(10, 6); B.u(12, 6); B.u(1, 4); B.u(10, 4); B.u(27, 5); B.u(6, 5); B.u(30, 6); B.u(85, 8);
    B.text(dest, 20); B.u(0, 1); B.u(0, 1);
    return B.bytes();
}
static std::vector<uint8_t> msgType18(uint32_t mmsi, double lat, double lon, double sog, double cog) {
    Builder B; B.u(18, 6); B.u(0, 2); B.u(mmsi, 30); B.u(0, 8); B.u(uint64_t(std::lround(sog * 10)), 10); B.u(0, 1);
    B.s(std::lround(lon * 600000), 28); B.s(std::lround(lat * 600000), 27); B.u(uint64_t(std::lround(cog * 10)), 12); B.u(511, 9);
    B.u(20, 6); B.u(0, 2); B.u(1, 1); B.u(0, 1); B.u(1, 1); B.u(1, 1); B.u(1, 1); B.u(0, 1); B.u(0, 1); B.u(0, 20);
    return B.bytes();
}
static std::vector<uint8_t> msgType24A(uint32_t mmsi, const std::string& name) {
    Builder B; B.u(24, 6); B.u(0, 2); B.u(mmsi, 30); B.u(0, 2); B.text(name, 20); return B.bytes();
}
static std::vector<uint8_t> msgType24B(uint32_t mmsi, int type, const std::string& call) {
    Builder B; B.u(24, 6); B.u(0, 2); B.u(mmsi, 30); B.u(1, 2); B.u(uint64_t(type), 8); B.u(0, 18); B.u(0, 4); B.u(0, 20); B.text(call, 7);
    B.u(8, 9); B.u(4, 9); B.u(2, 6); B.u(1, 6); B.u(0, 6); return B.bytes();
}
static std::vector<uint8_t> msgType21(uint32_t mmsi, int at, const std::string& name, double lat, double lon, bool virt) {
    Builder B; B.u(21, 6); B.u(0, 2); B.u(mmsi, 30); B.u(uint64_t(at), 5); B.text(name, 20); B.u(1, 1);
    B.s(std::lround(lon * 600000), 28); B.s(std::lround(lat * 600000), 27); B.u(0, 9); B.u(0, 9); B.u(0, 6); B.u(0, 6); B.u(7, 4);
    B.u(60, 6); B.u(0, 1); B.u(0, 8); B.u(0, 1); B.u(virt ? 1 : 0, 1); B.u(0, 1); B.u(0, 1);
    return B.bytes();
}
static std::vector<uint8_t> msgType4(uint32_t mmsi, double lat, double lon) {
    Builder B; B.u(4, 6); B.u(0, 2); B.u(mmsi, 30); B.u(2026, 14); B.u(10, 4); B.u(26, 5); B.u(12, 5); B.u(34, 6); B.u(56, 6); B.u(1, 1);
    B.s(std::lround(lon * 600000), 28); B.s(std::lround(lat * 600000), 27); B.u(7, 4); B.u(0, 10); B.u(0, 1); B.u(0, 19);
    return B.bytes();
}
static std::vector<uint8_t> msgType14(uint32_t mmsi, const std::string& text) {
    Builder B; B.u(14, 6); B.u(0, 2); B.u(mmsi, 30); B.u(0, 2); B.text(text, int(text.size()));
    while (B.bits.size() % 8) B.u(0, 1);
    return B.bytes();
}

/** NMEA armoured payload → message bytes (the AIVDM "6-bit armouring"). */
static std::vector<uint8_t> unarmour(const std::string& p, int fill) {
    Builder B;
    for (char c : p) { int v = c - 48; if (v > 40) v -= 8; B.u(uint64_t(v & 63), 6); }
    for (int i = 0; i < fill; ++i) B.bits.pop_back();
    return B.bytes();
}

// ── the GMSK modulator (M.1371 Annex 2: BT 0.4, h 0.5, NRZI, training, flags) ───────────────────────────────────────
static std::vector<int> airBits(const std::vector<uint8_t>& msg) {
    std::vector<uint8_t> f = msg; vibehdlc::appendFcs(f);
    std::vector<int> b;
    for (int i = 0; i < 24; ++i) b.push_back(i & 1);        // the training sequence 0101…
    vibehdlc::flagBits(b);
    vibehdlc::stuffBits(f, b);
    vibehdlc::flagBits(b);
    for (int i = 0; i < 8; ++i) b.push_back(1);              // part of the buffer
    std::vector<int> lv; int level = 0; vibehdlc::nrzi(b, lv, level);
    return lv;
}
/** ★ The real world a synthetic burst should survive (M.1371 Annex 2 §3.1: a transmitter may take up to 8 bits to ramp
 *  its power up, and its carrier is good to ±500 Hz; an RTL dongle's crystal adds the same offset to every burst). */
struct Cond {
    const char* name = "clean";
    double xtalHz = 1200;          ///< the receiver's crystal error, common to every burst
    double txSpreadHz = 0;         ///< each transmitter's own error, uniform ±this
    int rampBits = 0;              ///< power ramp-up, levels modulated through it (0 = a 3-sample ramp, the old test)
    double chirpHz = 0;            ///< a synthesiser still settling at key-up: +chirpHz decaying with τ = 3 bits
    double ray2Db = -99, ray2Bits = 0;   ///< a second path: its level and its delay in bits (random phase per burst)
    double adjDb = -99;            ///< FM carriers on the adjacent 25 kHz channels (161.950, 162.050) this far over a burst
    double prevDb = -99;           ///< a stronger burst in the PREVIOUS slot on the same channel (back to back)
    double rate = 2.4e6;
    int warm = 16;                 ///< bursts 10 dB stronger first (other ships, not counted): the AFC settles, as it has
                                   ///< long since on a receiver that has been running (a cold start costs ~10 bursts)
};
struct Burst { double t0; double offHz; std::vector<int> levels; double amp; int rampBits = 0; double chirpHz = 0;
               double ray2 = 0, ray2Bits = 0, ray2Ph = 0; };
/** Add one burst into `iq` (rate fs) at its offset from 162.000 MHz. The frequency pulse is the NRZ rectangle through
 *  the Gaussian (BT 0.4): p(t) = ½[erf((t+T/2)/(√2σT)) − erf((t−T/2)/(√2σT))], σ = √ln2 / (2π·BT), tabulated over ±3 T. */
static void addBurst(std::vector<cf>& iq, double fs, const Burst& b0, uint64_t seed = 1) {
    Burst b = b0;
    if (b.rampBits > 0) {   // ★ levels sent through the ramp (the transmitter is already modulating): random ones
        std::vector<int> pre; uint64_t s = seed * 0x9E3779B97F4A7C15ull + 7;
        for (int i = 0; i < b.rampBits; ++i) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; pre.push_back(int(s & 1)); }
        b.levels.insert(b.levels.begin(), pre.begin(), pre.end());
    }
    const double sps = fs / 9600.0, BT = 0.4, sigma = std::sqrt(std::log(2.0)) / (2 * M_PI * BT);
    const int half = int(std::ceil(3 * sps));
    std::vector<float> p(size_t(2 * half + 1));
    for (int i = -half; i <= half; ++i) {
        const double t = i / sps, k = std::sqrt(2.0) * sigma;
        p[size_t(i + half)] = float(0.5 * (std::erf((t + 0.5) / k) - std::erf((t - 0.5) / k)));
    }
    const size_t n = size_t((b.levels.size() + 6) * sps);
    std::vector<float> f(n, 0.f);
    for (size_t k = 0; k < b.levels.size(); ++k) {
        const double c = (k + 3) * sps;   // the symbol's centre, 3 T in
        const float a = b.levels[k] ? 1.f : -1.f;
        const long i0 = std::lround(c);
        for (int j = -half; j <= half; ++j) { const long i = i0 + j; if (i >= 0 && size_t(i) < n) f[size_t(i)] += a * p[size_t(j + half)]; }
    }
    double ph = 0;
    const size_t s0 = size_t(b.t0 * fs);
    const double rampLen = b.rampBits > 0 ? sps * b.rampBits : sps * 3;
    std::vector<cf> z(n);
    for (size_t i = 0; i < n; ++i) {
        const double chirp = b.chirpHz * std::exp(-double(i) / (3 * sps));
        ph += 2 * M_PI * (b.offHz + chirp + 2400.0 * f[i]) / fs;
        const double ramp = std::min(1.0, std::min(double(i) / rampLen, double(n - i) / (sps * 3)));
        z[i] = cf(float(b.amp * ramp * std::cos(ph)), float(b.amp * ramp * std::sin(ph)));
    }
    const long dly = std::lround(b.ray2Bits * sps);
    const cf r2 = std::polar(float(b.ray2), float(b.ray2Ph));
    for (size_t i = 0; i < n + size_t(dly) && s0 + i < iq.size(); ++i) {
        cf v = i < n ? z[i] : cf(0, 0);
        if (b.ray2 > 0 && long(i) >= dly && size_t(long(i) - dly) < n) v += r2 * z[size_t(long(i) - dly)];
        iq[s0 + i] += v;
    }
}
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 2654435761ull + 1) {}
    double u() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (double(s >> 11) + 0.5) / 9007199254740992.0; }
    void gauss2(float& a, float& b) { const double r = std::sqrt(-2 * std::log(u())), t = 2 * M_PI * u(); a = float(r * std::cos(t)); b = float(r * std::sin(t)); }
};
static void addNoise(std::vector<cf>& iq, double sigmaPerDim, Rng& rng) {
    for (auto& z : iq) { float a, b; rng.gauss2(a, b); z += cf(a * float(sigmaPerDim), b * float(sigmaPerDim)); }
}

// ── tests ────────────────────────────────────────────────────────────────────────────────────────────────────────────
static void testHdlc() {
    std::printf("── HDLC ──\n");
    const char* s = "123456789";
    CHECK(vibehdlc::fcs16((const uint8_t*)s, 9) == 0x906E, "FCS-16/X.25 check value 0x906E, got %04X", vibehdlc::fcs16((const uint8_t*)s, 9));
    std::vector<uint8_t> f = msgType1(235000001, 51.5, -1.2, 10.3, 87.1, 88, 0);
    std::vector<int> lv = airBits(f);
    std::vector<std::vector<uint8_t>> got;
    vibehdlc::Deframer::Options o; o.repairMax = 10; o.validate = vibeais::plausible; o.validateClean = true;
    vibehdlc::Deframer d(o);
    auto run = [&](const std::vector<float>& soft) { got.clear(); for (float v : soft) d.push(v, [&](const vibehdlc::Frame& fr) { got.push_back(fr.bytes); }); };
    std::vector<float> soft; for (int l : lv) soft.push_back(l ? 0.9f : -0.9f);
    run(soft);
    CHECK(got.size() == 1 && got[0] == f, "a clean frame round trip (%zu frames)", got.size());
    // flip one level inside the frame, mark it weak: repaired
    std::vector<float> bad = soft; bad[100] = -bad[100] * 0.1f;
    run(bad);
    CHECK(got.size() == 1 && got[0] == f, "one weak wrong level repaired (%zu frames)", got.size());
    // flip one level, NOT weak: no repair (it is not among the weakest)
    for (size_t i = 40; i < 200; i += 7) bad[i] *= 0.3f;
    bad[100] = -soft[100];
    run(bad);
    CHECK(got.empty(), "a confident wrong level is not guessed at (%zu frames)", got.size());
    // two weak wrong levels: one flip cannot fix it, and nothing false comes out
    std::vector<float> two = soft; two[90] = -two[90] * 0.1f; two[150] = -two[150] * 0.1f;
    run(two);
    CHECK(got.empty(), "two errors: no frame (%zu)", got.size());
}

static void testPublished() {
    std::printf("── messages: published worked examples (gpsd AIVDM notes) ──\n");
    vibeais::Msg m;
    // !AIVDM,1,1,,B,177KQJ5000G?tO`K>RA1wUbN0TKH,0*5C — type 1, MMSI 477553000, moored, 51°, 0 kn, heading 181
    bool ok = vibeais::decode(unarmour("177KQJ5000G?tO`K>RA1wUbN0TKH", 0), m);
    CHECK(ok && m.type == 1 && m.mmsi == 477553000u, "type 1 MMSI 477553000 (got %d %u)", m.type, m.mmsi);
    CHECK(m.nav == 5 && m.heading == 181 && std::fabs(m.cog - 51.0) < 0.05 && m.sog == 0, "moored, hdg 181, cog 51, sog 0 (nav %d hdg %d cog %.1f sog %.1f)", m.nav, m.heading, m.cog, m.sog);
    CHECK(std::fabs(m.lon + 122.345832) < 1e-5 && std::fabs(m.lat - 47.582833) < 1e-5, "position 47.582833 −122.345832 (got %.6f %.6f)", m.lat, m.lon);
    const vibeais::Mid* fl = vibeais::flagOf(m.mmsi);
    CHECK(fl && std::strcmp(fl->iso, "HK") == 0, "MID 477 → Hong Kong");
    // !AIVDM,2,1,1,A,55?MbV02;H;s<HtKR20EHE:0@T4@Dn2222222216L961O5Gf0NSQEp6ClRp8,0*1C + ,88888888880,2*25 — type 5
    ok = vibeais::decode(unarmour("55?MbV02;H;s<HtKR20EHE:0@T4@Dn2222222216L961O5Gf0NSQEp6ClRp888888888880", 2), m);
    CHECK(ok && m.type == 5 && m.mmsi == 351759000u, "type 5 MMSI 351759000 (got %d %u)", m.type, m.mmsi);
    CHECK(m.name == "EVER DIADEM" && m.callsign == "3FOF8" && m.dest == "NEW YORK", "EVER DIADEM / 3FOF8 / NEW YORK (got '%s' '%s' '%s')", m.name.c_str(), m.callsign.c_str(), m.dest.c_str());
    CHECK(m.imo == 9134270u && m.shipType == 70 && m.bow == 225 && m.stern == 70 && m.port == 1 && m.stbd == 31,
          "IMO 9134270, cargo 70, 225+70 × 1+31 (got %u %d %d %d %d %d)", m.imo, m.shipType, m.bow, m.stern, m.port, m.stbd);
    CHECK(std::fabs(m.draught - 12.2) < 0.01 && m.etaMonth == 5 && m.etaDay == 15 && m.etaHour == 14 && m.etaMin == 0,
          "draught 12.2, ETA 05-15 14:00 (got %.1f %d-%d %d:%d)", m.draught, m.etaMonth, m.etaDay, m.etaHour, m.etaMin);
    fl = vibeais::flagOf(m.mmsi);
    CHECK(fl && std::strcmp(fl->iso, "PA") == 0, "MID 351 → Panama");
}

static void testEncoded() {
    std::printf("── messages: every type we read, encoded from Annex 8 ──\n");
    vibeais::Msg m;
    CHECK(vibeais::decode(msgType1(235012345, 50.81234, -1.30987, 12.3, 245.6, 247, 0, 20), m) && m.mmsi == 235012345u
          && std::fabs(m.lat - 50.81234) < 2e-6 && std::fabs(m.lon + 1.30987) < 2e-6 && std::fabs(m.sog - 12.3) < 1e-9
          && std::fabs(m.cog - 245.6) < 1e-9 && m.heading == 247 && m.rot == 20, "type 1 fields");
    CHECK(vibeais::decode(msgType5(232001234, "WILSON GOOLE", "MXYZ7", "ROTTERDAM", 70), m) && m.name == "WILSON GOOLE"
          && m.callsign == "MXYZ7" && m.dest == "ROTTERDAM" && m.shipType == 70 && std::fabs(m.draught - 8.5) < 1e-9, "type 5 fields");
    CHECK(vibeais::decode(msgType18(235099999, 50.7, -1.1, 6.2, 90.0), m) && m.type == 18 && std::fabs(m.sog - 6.2) < 1e-9
          && m.heading < 0 && std::fabs(m.lat - 50.7) < 2e-6, "type 18 fields (heading 511 = none)");
    CHECK(vibeais::decode(msgType24A(235099999, "SEA BREEZE"), m) && m.part == 0 && m.name == "SEA BREEZE", "type 24 A name");
    CHECK(vibeais::decode(msgType24B(235099999, 37, "2ABC3"), m) && m.part == 1 && m.shipType == 37 && m.callsign == "2ABC3" && m.bow == 8,
          "type 24 B type/callsign/dims");
    CHECK(vibeais::decode(msgType21(992351234, 20, "NAB TOWER", 50.668, -0.952, false), m) && m.atonType == 20 && m.name == "NAB TOWER"
          && !m.virt && std::fabs(m.lat - 50.668) < 2e-6, "type 21 AtoN");
    CHECK(vibeais::decode(msgType4(2320001, 50.9, -1.4), m) && m.type == 4 && m.year == 2026 && m.hour == 12 && m.hasPos, "type 4 base station");
    CHECK(vibeais::decode(msgType14(970123456, "SART ACTIVE"), m) && m.type == 14 && m.text == "SART ACTIVE", "type 14 text '%s'", m.text.c_str());
    const char* what = nullptr;
    CHECK(vibeais::midOf(2320001, &what) == 232 && std::strcmp(what, "coast station") == 0, "00MID = coast station, UK");
    CHECK(vibeais::midOf(992351234, &what) == 235 && std::strcmp(what, "aid to navigation") == 0, "99MID = AtoN, UK");
    CHECK(vibeais::midOf(111232500, &what) == 232 && std::strcmp(what, "SAR aircraft") == 0, "111MID = SAR aircraft");
    std::vector<uint8_t> t1 = msgType1(1, 0, 0, 0, 0, 0, 0); t1.push_back(0);
    CHECK(!vibeais::plausible(t1), "a type 1 one byte long is refused");
    CHECK(vibeais::shipTypeName(84) == "tanker" && vibeais::shipTypeName(36) == "sailing", "ship type names");
}

/** Make a capture: `n` frames from `n/6` ships (each sends a type 5 then positions), alternating channels, at `ebn0` dB,
 *  rate fs, with a crystal offset. Frames are counted through the SERVICE's rule: a repaired frame counts only when its
 *  MMSI was heard cleanly before (Service::accept_). */
struct E2E { int sent = 0, heard = 0, repaired = 0, wrong = 0, dropped = 0; double cpuS = 0, capS = 0; };
static E2E endToEnd(double fs, double ebn0, int n, bool repair, uint64_t seed, double xtalHz = 1200, std::vector<cf>* keep = nullptr,
                    const Cond* cond = nullptr) {
    E2E r;
    Cond cd; if (cond) cd = *cond; else cd.xtalHz = xtalHz;
    const int warm = cond ? cond->warm : 0;
    const double gapS = 0.04, t0s = 0.1 + warm * gapS, dur = t0s + n * gapS + 0.1;
    std::vector<cf> iq(size_t(dur * fs), cf(0, 0));
    std::vector<std::vector<uint8_t>> sent, extra;
    const double amp = 0.25;
    const int ships = std::max(1, n / 6);
    Rng crng(seed * 31 + 5);
    Rng wrng(seed * 7 + 3);
    for (int i = 0; i < warm; ++i) {
        std::vector<uint8_t> wm = msgType1(266000000u + uint32_t(i), 53.0, 3.0, 2, 2, 2, 0);
        Burst wb{ 0.1 + i * gapS, (i & 1 ? 25e3 : -25e3) + cd.xtalHz + cd.txSpreadHz * (2 * wrng.u() - 1), airBits(wm), amp * std::sqrt(10.0) };
        addBurst(iq, fs, wb, uint64_t(i) + 77);
        extra.push_back(wm);
    }
    for (int i = 0; i < n; ++i) {
        const int sh = i % ships, k = i / ships;
        const uint32_t mmsi = 235000000u + uint32_t(sh) * 7 + uint32_t(seed % 5);
        std::vector<uint8_t> m = (k % 3 == 1) ? msgType5(mmsi, "TEST SHIP " + std::to_string(sh), "M" + std::to_string(sh), "SOUTHAMPTON", 70)
                                              : msgType1(mmsi, 50.0 + sh * 0.01 + k * 0.0003, -1.0 - sh * 0.01, 5 + k, (sh * 37 + k) % 360, (sh * 41) % 360, 0);
        const double chOff = (i & 1 ? 25e3 : -25e3) + cd.xtalHz;
        Burst b{ t0s + i * gapS, chOff + cd.txSpreadHz * (2 * crng.u() - 1), airBits(m), amp, cd.rampBits, cd.chirpHz,
                 cd.ray2Db > -90 ? std::pow(10.0, cd.ray2Db / 20) : 0.0, cd.ray2Bits, 2 * M_PI * crng.u() };
        if (cd.prevDb > -90) {
            // ★ back to back: a stronger ship in the slot before (26.67 ms), ending ~2 ms before this one starts
            std::vector<uint8_t> pm = msgType1(244000000u + uint32_t(i), 52.0, 3.0, 1, 1, 1, 0);
            std::vector<int> pl = airBits(pm);
            Burst pb{ b.t0 - (pl.size() + 6 + 20) / 9600.0, chOff + cd.txSpreadHz * (2 * crng.u() - 1), pl, amp * std::pow(10.0, cd.prevDb / 20),
                      cd.rampBits, cd.chirpHz };
            addBurst(iq, fs, pb, uint64_t(i) + 991);
            extra.push_back(pm);
        }
        addBurst(iq, fs, b, uint64_t(i) + seed * 1000);
        sent.push_back(m);
    }
    if (cd.adjDb > -90) {
        // ★ FM voice carriers on the neighbouring 25 kHz channels: a 1 kHz tone at ±3 kHz deviation, continuous
        const double a = amp * std::pow(10.0, cd.adjDb / 20);
        for (int s = -1; s <= 1; s += 2) {
            double ph = 0;
            for (size_t i = 0; i < iq.size(); ++i) {
                ph += 2 * M_PI * (s * 50e3 + cd.xtalHz + 3000.0 * std::sin(2 * M_PI * 1000.0 * i / fs + s)) / fs;
                iq[i] += cf(float(a * std::cos(ph)), float(a * std::sin(ph)));
            }
        }
    }
    Rng rng(seed);
    // Eb/N0 → noise: total complex noise power σ² = A² fs / (9600 · Eb/N0)
    const double ebn0lin = std::pow(10.0, ebn0 / 10.0);
    const double sigma2 = amp * amp * fs / (9600.0 * ebn0lin);
    addNoise(iq, std::sqrt(sigma2 / 2), rng);
    if (keep) *keep = iq;
    vibeais::Front fe(fs, vibeais::kCentreHz);
    std::vector<std::vector<uint8_t>> got; std::vector<int> gotRep;
    std::set<uint32_t> known;
    const auto t0 = std::chrono::steady_clock::now();
    fe.feed(iq.data(), iq.size(), [&](int, const vibeais::RawFrame& f) {
        vibeais::Msg m;
        if (!vibeais::decode(f.bytes, m)) return;
        if (f.repaired && (!repair || !known.count(m.mmsi))) { ++r.dropped; return; }
        if (!f.repaired) known.insert(m.mmsi);
        got.push_back(f.bytes); gotRep.push_back(f.repaired); if (f.repaired) ++r.repaired;
    });
    r.cpuS = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (std::getenv("AIS_DEBUG")) std::printf("   [afc %.0f Hz, crystal %.0f]\n", fe.afcHz(), cd.xtalHz);
    r.capS = dur;
    r.sent = n;
    std::vector<int> seen(sent.size(), 0);
    for (size_t gi = 0; gi < got.size(); ++gi) {
        const auto& g = got[gi];
        int idx = -1;
        for (size_t q = 0; q < sent.size(); ++q) if (sent[q] == g && !seen[q]) { idx = int(q); break; }
        if (idx < 0) for (size_t q = 0; q < sent.size(); ++q) if (sent[q] == g) { idx = int(q); break; }
        if (idx >= 0) { if (!seen[size_t(idx)]++) ++r.heard; }
        else if (std::find(extra.begin(), extra.end(), g) == extra.end()) {   // ★ the strong ship before is not wrong
            ++r.wrong;
            if (std::getenv("AIS_DEBUG")) {
                vibeais::Msg wm; vibeais::decode(g, wm);
                std::printf("   WRONG: type %d MMSI %u, %zu bytes, repaired %d\n", wm.type, wm.mmsi, g.size(), gotRep[gi]);
            }
        }
    }
    if (std::getenv("AIS_DEBUG")) {
        std::printf("   missed:");
        for (size_t q = 0; q < sent.size(); ++q) if (!seen[q]) std::printf(" %zu(t%d)", q, sent[q][0] >> 2);
        std::printf("\n");
    }
    return r;
}

static void testReceiver() {
    std::printf("── the receiver: both channels, GMSK on synthetic captures ──\n");
    {
        vibeais::Front fe(2.4e6, vibeais::kCentreHz);
        std::printf("   2.4 MS/s → %d shared stages → %.0f kS/s → channels at %.1f kS/s\n", fe.stages(), fe.sharedRate() / 1e3, fe.channelRate() / 1e3);
        vibeais::Front fe2(2.048e6, vibeais::kCentreHz);
        std::printf("   2.048 MS/s → %d shared stages → %.0f kS/s → channels at %.2f kS/s\n", fe2.stages(), fe2.sharedRate() / 1e3, fe2.channelRate() / 1e3);
        CHECK(fe.channelRate() >= 38.4e3 && fe2.channelRate() >= 38.4e3, "≥ 4 samples a bit");
    }
    E2E clean = endToEnd(2.4e6, 30, 30, true, 1);
    CHECK(clean.heard == clean.sent && clean.wrong == 0, "2.4 MS/s, clean: %d of %d, %d wrong", clean.heard, clean.sent, clean.wrong);
    E2E clean2 = endToEnd(2.048e6, 30, 30, true, 2, -3000);
    // ★ a COLD start 3 kHz off: the coherent detector follows ±1200 Hz of where the AFC has the channel, so the first
    //   burst or two go by while the AFC settles (Front::afcStep_; the service then keeps it across restarts)
    CHECK(clean2.heard >= clean2.sent - 2 && clean2.wrong == 0, "2.048 MS/s, −3 kHz crystal, cold: %d of %d, %d wrong", clean2.heard, clean2.sent, clean2.wrong);
    E2E slow = endToEnd(250e3, 30, 20, true, 3, 500);
    CHECK(slow.heard == slow.sent, "250 kS/s capture: %d of %d", slow.heard, slow.sent);
    std::printf("   Eb/N0 (dB)   decoded of 120: no repair   with repair (repaired)   wrong\n");
    const double snrs[] = { 8, 10, 11, 12, 13, 14, 16, 18 };
    int at14 = 0, at18 = 0, at12r = 0, at12n = 0, at10 = 0, at8 = 0;
    for (double e : snrs) {
        E2E a = endToEnd(2.4e6, e, 120, false, 100 + uint64_t(e));
        E2E r = endToEnd(2.4e6, e, 120, true, 100 + uint64_t(e));
        std::printf("   %5.0f        %3d (%5.1f %%)           %3d (%5.1f %%) (%3d)         %d\n", e, a.heard, 100.0 * a.heard / a.sent,
                    r.heard, 100.0 * r.heard / r.sent, r.repaired, r.wrong + a.wrong);
        CHECK(r.wrong == 0 && a.wrong == 0, "no wrong frames at %.0f dB", e);
        if (e == 14) at14 = r.heard;
        if (e == 10) at10 = r.heard;
        if (e == 8) at8 = r.heard;
        if (e == 18) at18 = r.heard;
        if (e == 12) { at12r = r.heard; at12n = a.heard; }
    }
    // ★ 2026-10-29, the coherent detector: 50 % now falls at ~8.5 dB (was 13 dB). Measured 45 / 118 / 120 / 120 / 120.
    CHECK(at18 >= 120, "120/120 at 18 dB (%d)", at18);
    CHECK(at14 >= 119, "≥ 119/120 at 14 dB (%d)", at14);
    CHECK(at12r >= 118, "≥ 118/120 at 12 dB (%d)", at12r);
    CHECK(at10 >= 100, "≥ 100/120 at 10 dB (%d)", at10);
    CHECK(at8 >= 30, "≥ 30/120 at 8 dB (%d)", at8);
    CHECK(at12r >= at12n, "the repair never loses a frame at 12 dB (%d vs %d)", at12r, at12n);
}

/** ★★ THE REAL WORLD, HARSHER: a dongle's crystal ±3 kHz and +6 kHz (≈ 37 ppm), the transmitters' own ±500 Hz, an 8-bit
 *  power ramp with a settling synthesiser, a second path, FM carriers on both adjacent channels, and a stronger ship in
 *  the slot before. Each row: frames decoded of 60 (repair on, through the service's rule) per Eb/N0. */
static const Cond kConds[] = {
    { "plain (1.2 kHz)" },
    { "crystal -3 kHz", -3000 },
    { "crystal +3 kHz", 3000 },
    { "crystal +6 kHz", 6000 },
    { "crystal -9 kHz (55 ppm)", -9000 },
    { "tx +-500 Hz, 8-bit ramp, 1 kHz settle", 1500, 500, 8, 1000 },
    { "2nd path -6 dB, 0.25 bit", 1200, 0, 0, 0, -6, 0.25 },
    { "adjacent FM +40 dB", 1200, 0, 0, 0, -99, 0, 40 },
    { "previous slot +15 dB", 1200, 0, 0, 0, -99, 0, -99, 15 },
    { "all of it (+2.5 kHz)", 2500, 500, 8, 1000, -6, 0.25, 40, 15 },
};
static void conditionTable(const double* snrs, int ns, int n, std::vector<std::vector<int>>* out = nullptr) {
    std::printf("   condition (%d frames, repair on)        ", n);
    for (int j = 0; j < ns; ++j) std::printf("%5.0f", snrs[j]);
    std::printf("  dB   wrong\n");
    int ci = 0;
    for (const Cond& c : kConds) {
        if (std::getenv("AIS_COND") && std::atoi(std::getenv("AIS_COND")) != ci) { ++ci; continue; }   // ★ one row (debugging)
        std::printf("   %-40s", c.name);
        int wrong = 0;
        std::vector<int> row;
        for (int j = 0; j < ns; ++j) {
            if (std::getenv("AIS_SNR") && std::atof(std::getenv("AIS_SNR")) != snrs[j]) continue;
            E2E r = endToEnd(c.rate, snrs[j], n, true, 500 + uint64_t(j) * 17 + uint64_t(ci) * 101, 0, nullptr, &c);
            std::printf("%5.0f", 100.0 * r.heard / r.sent); std::fflush(stdout);
            wrong += r.wrong; row.push_back(r.heard);
        }
        std::printf("  %%    %d\n", wrong);
        CHECK(wrong == 0, "%s: no wrong frames", c.name);
        // ★ every condition: half the frames by 10 dB, 90 % by 14 (the first column is 8 dB when the table is short)
        for (int j = 0; j < ns && int(row.size()) == ns; ++j) {
            if (snrs[j] == 10) CHECK(row[size_t(j)] * 2 >= n, "%s: ≥ 50 %% at 10 dB (%d of %d)", c.name, row[size_t(j)], n);
            if (snrs[j] == 14) CHECK(row[size_t(j)] * 10 >= n * 9, "%s: ≥ 90 %% at 14 dB (%d of %d)", c.name, row[size_t(j)], n);
        }
        if (out) out->push_back(row);
        ++ci;
    }
}
static void testConditions(bool full) {
    std::printf("── the receiver under real-world conditions ──\n");
    const double all[] = { 8, 9, 10, 11, 12, 14, 16, 20 }, quick[] = { 8, 10, 14 };
    std::vector<std::vector<int>> t;
    if (full) conditionTable(all, 8, 60, &t); else conditionTable(quick, 3, 60, &t);
}

static void testNoise() {
    std::printf("── noise only: zero frames ──\n");
    // ★ ten minutes of channel bits on both channels: a 120 kS/s capture (one stage, the same demodulators)
    const double fs = 120e3, secs = 600;
    vibeais::Front fe(fs, vibeais::kCentreHz);
    Rng rng(77);
    std::vector<cf> blk(static_cast<size_t>(fs));
    int frames = 0;
    for (int s = 0; s < int(secs); ++s) {
        for (auto& z : blk) { float a, b; rng.gauss2(a, b); z = cf(a * 0.1f, b * 0.1f); }
        fe.feed(blk.data(), blk.size(), [&](int, const vibeais::RawFrame&) { ++frames; });
    }
    CHECK(frames == 0, "%d frames in %.0f s of noise (2 channels × 3 slicers, repair on)", frames, secs);
}

static void testService() {
    std::printf("── the service: rows, readable lines, JSON, trail ──\n");
    vibeais::Service s;
    auto put = [&](int ch, const std::vector<uint8_t>& b) { vibeais::RawFrame f; f.bytes = b; f.levelDb = -30; f.snrDb = 25; s.acceptForTest(ch, f); };
    put(0, msgType1(235012345, 50.8, -1.3, 12.3, 245.6, 247, 0));
    put(1, msgType5(235012345, "WILSON GOOLE", "MXYZ7", "ROTTERDAM", 70));
    put(0, msgType1(235012345, 50.801, -1.302, 12.4, 245.0, 246, 0));
    put(0, msgType21(992351234, 20, "NAB TOWER", 50.668, -0.952, false));
    put(1, msgType14(970123456, "SART ACTIVE"));
    const std::string j = s.report();
    CHECK(s.vesselCount() == 3, "3 rows (ship, AtoN, the SART), got %zu", s.vesselCount());
    CHECK(j.find("\"name\":\"WILSON GOOLE\"") != std::string::npos && j.find("\"dest\":\"ROTTERDAM\"") != std::string::npos
          && j.find("\"flag\":\"GB\"") != std::string::npos && j.find("\"family\":\"cargo\"") != std::string::npos, "the ship merged from 1 + 5");
    CHECK(j.find("WILSON GOOLE \xc2\xb7 cargo \xc2\xb7 12 kn SW \xe2\x86\x92 ROTTERDAM") != std::string::npos, "the readable line");
    CHECK(j.find("\"kind\":\"aton\"") != std::string::npos && j.find("NAB TOWER \xc2\xb7 cardinal mark N") != std::string::npos, "the AtoN line");
    CHECK(j.find("\"safety\":[{") != std::string::npos && j.find("SART ACTIVE") != std::string::npos, "a safety text");
    const std::string tr = s.trailJson(235012345);
    CHECK(tr.find("[50.80000,-1.30000") != std::string::npos && tr.find("[50.80100,-1.30200") != std::string::npos, "the trail: %s", tr.c_str());
    CHECK(vibeais::readable(vibeais::Vessel{}).find("MMSI 0") == 0, "a nameless row reads as its MMSI");
}

static void testCpu() {
    std::printf("── CPU ──\n");
    E2E r = endToEnd(2.4e6, 20, 60, true, 9);
    std::printf("   2.4 MS/s: %.3f s for %.2f s of capture = %.1f %% of one core (this Mac)\n", r.cpuS, r.capS, 100 * r.cpuS / r.capS);
    E2E r2 = endToEnd(2.048e6, 20, 60, true, 10);
    std::printf("   2.048 MS/s: %.1f %% of one core\n", 100 * r2.cpuS / r2.capS);
    CHECK(r.cpuS < r.capS, "faster than real time");
}

static int writeClip(const char* path, double fs) {
    std::vector<cf> iq;
    // 40 frames at 20 dB, then the capture as RTL u8 IQ
    E2E r = endToEnd(fs, 20, 40, true, 42, 800, &iq);
    FILE* f = std::fopen(path, "wb");
    if (!f) return 1;
    std::vector<uint8_t> u(iq.size() * 2);
    for (size_t i = 0; i < iq.size(); ++i) {
        u[2 * i] = uint8_t(std::max(0.f, std::min(255.f, 127.5f + iq[i].real() * 127.f)));
        u[2 * i + 1] = uint8_t(std::max(0.f, std::min(255.f, 127.5f + iq[i].imag() * 127.f)));
    }
    std::fwrite(u.data(), 1, u.size(), f); std::fclose(f);
    std::fprintf(stderr, "wrote %s: %.2f s at %.0f S/s, %d frames heard by the decoder\n", path, r.capS, fs, r.heard);
    return 0;
}

/** ★ A REAL recording of ONE channel: a 16-bit stereo IQ WAV (I left, Q right) at ≥ 38.4 kS/s, the channel `offHz` from
 *  its centre. Shifted, cut to ±7/14 kHz as the front end does, then one GmskDemod. Prints each frame. */
static int replayWav(const char* path, double offHz) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("cannot open %s\n", path); return 1; }
    std::vector<uint8_t> all; uint8_t buf[65536]; size_t got;
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) all.insert(all.end(), buf, buf + got);
    std::fclose(f);
    if (all.size() < 44 || std::memcmp(all.data(), "RIFF", 4) != 0) { std::printf("not a WAV\n"); return 1; }
    size_t p = 12; uint32_t rate = 0; int ch = 0, bitsPer = 0; size_t dataAt = 0, dataLen = 0;
    while (p + 8 <= all.size()) {
        const uint32_t len = uint32_t(all[p + 4]) | uint32_t(all[p + 5]) << 8 | uint32_t(all[p + 6]) << 16 | uint32_t(all[p + 7]) << 24;
        if (!std::memcmp(&all[p], "fmt ", 4)) { ch = all[p + 10]; rate = uint32_t(all[p + 12]) | uint32_t(all[p + 13]) << 8 | uint32_t(all[p + 14]) << 16; bitsPer = all[p + 22]; }
        if (!std::memcmp(&all[p], "data", 4)) { dataAt = p + 8; dataLen = std::min<size_t>(len, all.size() - p - 8); break; }
        p += 8 + len + (len & 1);
    }
    if (ch != 2 || bitsPer != 16 || !dataAt) { std::printf("need 16-bit stereo IQ (got %d ch, %d bits)\n", ch, bitsPer); return 1; }
    const size_t n = dataLen / 4;
    std::vector<vibedsp::cf32> iq(n);
    for (size_t i = 0; i < n; ++i) {
        const int16_t a = int16_t(all[dataAt + 4 * i] | all[dataAt + 4 * i + 1] << 8), b = int16_t(all[dataAt + 4 * i + 2] | all[dataAt + 4 * i + 3] << 8);
        iq[i] = vibedsp::cf32(a / 32768.f, b / 32768.f);
    }
    const double fs = rate;
    if (const char* nz = std::getenv("AIS_WAV_NOISE")) {   // ★ white noise added (σ per dimension, full scale = 1): real bursts made weaker
        Rng rng(11); const float sg = float(std::atof(nz));
        for (auto& z : iq) { float a, b; rng.gauss2(a, b); z += vibedsp::cf32(a * sg, b * sg); }
    }
    vibedsp::NCO nco(offHz / fs);
    std::vector<vibedsp::cf32> m(n + 16), c(n + 16);
    nco.mix(iq.data(), m.data(), int(n));
    vibedsp::FirDecimator fir(vibedsp::designLowpass(0.5 * (7e3 + 14e3) / fs, 7e3 / fs, true), 1);
    const int k = fir.process(m.data(), int(n), c.data());
    vibeais::GmskDemod d(fs);
    int frames = 0, rep = 0; std::set<uint32_t> ships;
    d.feed(reinterpret_cast<const cf*>(c.data()), k, [&](const vibeais::RawFrame& fr) {
        vibeais::Msg msg; if (!vibeais::decode(fr.bytes, msg)) return;
        ++frames; if (fr.repaired) ++rep; ships.insert(msg.mmsi);
        if (std::getenv("AIS_DEBUG")) std::printf("   type %2d MMSI %09u%s %s\n", msg.type, msg.mmsi, fr.repaired ? " (repaired)" : "", msg.name.c_str());
    });
    std::printf("%s: %.1f s at %.0f S/s, channel at %+.0f Hz: %d frames (%d repaired), %zu stations\n", path, n / fs, fs, offHz, frames, rep, ships.size());
    return 0;
}

/** ★ A REAL capture of BOTH channels: rtl_sdr's unsigned 8-bit IQ, centred on 162.000 MHz, at `rate` (≥ 96 kS/s) —
 *  the whole front end (AFC included), as the server runs it. Prints the frames, the stations and where the AFC settled. */
static int replayCu8(const char* path, double rate) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("cannot open %s\n", path); return 1; }
    vibeais::Front fe(rate, vibeais::kCentreHz);
    std::vector<uint8_t> u(size_t(rate) * 2);
    std::vector<cf> iq;
    int frames = 0, perCh[2] = {0, 0}; std::set<uint32_t> ships; double secs = 0;
    size_t got;
    while ((got = std::fread(u.data(), 1, u.size(), f)) >= 2) {
        iq.resize(got / 2);
        for (size_t i = 0; i < iq.size(); ++i) iq[i] = cf((u[2 * i] - 127.5f) / 127.5f, (u[2 * i + 1] - 127.5f) / 127.5f);
        fe.feed(iq.data(), iq.size(), [&](int c, const vibeais::RawFrame& fr) {
            vibeais::Msg msg; if (!vibeais::decode(fr.bytes, msg)) return;
            ++frames; ++perCh[c & 1]; ships.insert(msg.mmsi);
            if (std::getenv("AIS_DEBUG")) std::printf("   %6.2f s  %c  type %2d MMSI %09u %s\n", secs, c ? 'B' : 'A', msg.type, msg.mmsi, msg.name.c_str());
        });
        secs += iq.size() / rate;
    }
    std::fclose(f);
    std::printf("%s: %.1f s at %.0f S/s: %d frames (A %d, B %d), %zu stations, AFC %+.0f Hz (%.1f ppm)\n", path, secs, rate, frames,
                perCh[0], perCh[1], ships.size(), fe.afcHz(), fe.afcHz() / 162.0);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 4 && std::strcmp(argv[1], "--cu8") == 0) return replayCu8(argv[2], std::atof(argv[3]));
    if (argc >= 3 && std::strcmp(argv[1], "--wav") == 0) {
        for (int i = 4; i < argc; ++i) { auto& t = vibeais::tuning(); if (!std::strcmp(argv[i], "nodisc")) t.disc = false; if (!std::strcmp(argv[i], "disc")) t.disc = true; if (!std::strcmp(argv[i], "nocoh")) t.coh = false; }
        return replayWav(argv[2], argc >= 4 ? std::atof(argv[3]) : 0);
    }
    if (argc >= 4 && std::strcmp(argv[1], "--write-clip") == 0) return writeClip(argv[2], std::atof(argv[3]));
    // ★ experiment switches: nodisc nocoh noafc W=<bits> Wf=<bits> (after the mode word)
    for (int i = 2; i < argc; ++i) {
        auto& t = vibeais::tuning();
        if (!std::strcmp(argv[i], "nodisc")) t.disc = false;
        else if (!std::strcmp(argv[i], "disc")) t.disc = true;
        else if (!std::strcmp(argv[i], "nocoh")) t.coh = false;
        else if (!std::strcmp(argv[i], "noafc")) t.afc = false;
        else if (!std::strncmp(argv[i], "W=", 2)) t.W = std::atoi(argv[i] + 2);
        else if (!std::strncmp(argv[i], "Wf=", 3)) t.Wf = std::atoi(argv[i] + 3);
        else if (!std::strncmp(argv[i], "ph=", 3)) t.cohPhases = std::atoi(argv[i] + 3);
        else if (!std::strncmp(argv[i], "dph=", 4)) t.discPhases = std::atoi(argv[i] + 4);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--conditions") == 0) { testConditions(true); return 0; }
    if (argc >= 2 && std::strcmp(argv[1], "--cpu") == 0) { testCpu(); return 0; }
    if (argc >= 2 && std::strcmp(argv[1], "--noise") == 0) { testNoise(); return 0; }
    if (argc >= 2 && std::strcmp(argv[1], "--sweep") == 0) { testReceiver(); return 0; }
    testHdlc();
    testPublished();
    testEncoded();
    testReceiver();
    testConditions(false);   // ★ the full table: test-ais --conditions
    testNoise();
    testService();
    testCpu();
    std::printf("\ntest-ais: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
