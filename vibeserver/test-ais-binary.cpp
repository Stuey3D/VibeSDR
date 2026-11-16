// test-ais-binary.cpp — V12 2026-11-16 AIS BINARY MESSAGES (vibe_ais_asm.h), docs/v12/AIS-APRS.md "Binary messages".
//
//  Every vector is built here, field by field, from the published tables (IMO SN.1/Circ.289 for DAC 001 FI 31 / 22 /
//  23 / 29 / 30; the withdrawn SN/Circ.236 for FI 11; IALA A-126 for the type 21 status byte; ITU-R M.1371-5 Annex 8
//  for the type 6 / 8 headers). No third-party decoder is used as an oracle.
//   1. FI 31 met/hydro: every field read back at its scale; every "not available" code becomes nothing; the edges
//      (≤ 799 hPa, ≥ 1201 hPa, "visibility more than", negative temperatures, water level −10 m and +30 m).
//   2. FI 11 (Circ.236): lat-before-lon and the offset encodings.
//   3. Type 6 (addressed): the longer header, the destination MMSI.
//   4. Area notices (FI 22 / 23): the description table, start time, duration (0 = cancelled), a circle's place
//      and radius with its scale factor, a rectangle, a text sub-area joined to the words.
//   5. Text description (FI 29).
//   6. The type 21 status byte: light on / off / fault, RACON, alarm; a page other than 7 is not read.
//   7. The NMEA armour round trip (a sentence a shore station would log decodes the same).
//   8. The service: the met report on the station's row and card JSON, a station with no position of its own drawn at
//      the readings', four sites at most per station, notices deduplicated and bounded, the gallery lines.
//   9. The met store: round trip through ais-met.dat, merge of sites, bounded by the gallery size, the shipping-map
//      JSON carries "met".
//  10. Garbage: every random payload of every length on every DAC/FI — no crash, no out-of-range number in the JSON.
#include "vibe_ais_store.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

namespace am = vibeais::asmsg;
namespace st = vibeais::store;

struct Builder {
    std::vector<int> bits;
    void u(uint64_t v, int len) { for (int i = len - 1; i >= 0; --i) bits.push_back(int((v >> i) & 1)); }
    void s(int64_t v, int len) { u(uint64_t(v) & ((1ull << len) - 1), len); }
    void text(const std::string& t, int chars) {
        for (int i = 0; i < chars; ++i) { int c = i < int(t.size()) ? t[size_t(i)] : '@'; u(uint64_t((c >= 64 ? c - 64 : c) & 63), 6); }
    }
    void pad8() { while (bits.size() % 8) u(0, 1); }
    std::vector<uint8_t> bytes() const {
        std::vector<uint8_t> b((bits.size() + 7) / 8, 0);
        for (size_t k = 0; k < bits.size(); ++k) if (bits[k]) b[k / 8] |= uint8_t(1 << (7 - k % 8));
        return b;
    }
};
static void hdr8(Builder& B, uint32_t mmsi, int dac, int fi) { B.u(8, 6); B.u(0, 2); B.u(mmsi, 30); B.u(0, 2); B.u(uint64_t(dac), 10); B.u(uint64_t(fi), 6); }
static void hdr6(Builder& B, uint32_t mmsi, uint32_t to, int dac, int fi) {
    B.u(6, 6); B.u(0, 2); B.u(mmsi, 30); B.u(0, 2); B.u(to, 30); B.u(0, 1); B.u(0, 1); B.u(uint64_t(dac), 10); B.u(uint64_t(fi), 6);
}

/** The Circ.289 FI 31 fields, as the table gives them (raw codes). */
struct Fi31 {
    double lon = -1.2345, lat = 50.6789; int day = 16, hour = 9, minute = 41;
    int wind = 14, gust = 21, wdir = 225, gdir = 230, air = 112, hum = 87, dew = 91, pres = 1012 - 799, ptend = 1;
    int visMore = 0, vis = 45, level = 321 + 1000, ltrend = 2, cur = 13, cdir = 80;
    int cur2 = 255, cdir2 = 360, cdep2 = 31, cur3 = 255, cdir3 = 360, cdep3 = 31;
    int wave = 12, waveS = 6, waveDir = 200, swell = 255, swellS = 63, swellDir = 360, sea = 4, water = 120, precip = 1, sal = 510, ice = 0;
};
static std::vector<uint8_t> msgFi31(uint32_t mmsi, const Fi31& f, bool spare = true) {
    Builder B; hdr8(B, mmsi, 1, 31);
    B.s(std::lround(f.lon * 60000), 25); B.s(std::lround(f.lat * 60000), 24); B.u(1, 1);
    B.u(uint64_t(f.day), 5); B.u(uint64_t(f.hour), 5); B.u(uint64_t(f.minute), 6);
    B.u(uint64_t(f.wind), 7); B.u(uint64_t(f.gust), 7); B.u(uint64_t(f.wdir), 9); B.u(uint64_t(f.gdir), 9);
    B.s(f.air, 11); B.u(uint64_t(f.hum), 7); B.s(f.dew, 10); B.u(uint64_t(f.pres), 9); B.u(uint64_t(f.ptend), 2);
    B.u(uint64_t(f.visMore), 1); B.u(uint64_t(f.vis), 7); B.u(uint64_t(f.level), 12); B.u(uint64_t(f.ltrend), 2);
    B.u(uint64_t(f.cur), 8); B.u(uint64_t(f.cdir), 9); B.u(uint64_t(f.cur2), 8); B.u(uint64_t(f.cdir2), 9); B.u(uint64_t(f.cdep2), 5);
    B.u(uint64_t(f.cur3), 8); B.u(uint64_t(f.cdir3), 9); B.u(uint64_t(f.cdep3), 5);
    B.u(uint64_t(f.wave), 8); B.u(uint64_t(f.waveS), 6); B.u(uint64_t(f.waveDir), 9);
    B.u(uint64_t(f.swell), 8); B.u(uint64_t(f.swellS), 6); B.u(uint64_t(f.swellDir), 9);
    B.u(uint64_t(f.sea), 4); B.s(f.water, 10); B.u(uint64_t(f.precip), 3); B.u(uint64_t(f.sal), 9); B.u(uint64_t(f.ice), 2);
    if (spare) B.u(0, 10);
    B.pad8();
    return B.bytes();
}
static std::vector<uint8_t> msgFi31NA(uint32_t mmsi) {
    Fi31 f; f.lon = 181; f.lat = 91; f.day = 0; f.hour = 24; f.minute = 60;
    f.wind = 127; f.gust = 127; f.wdir = 360; f.gdir = 360; f.air = -1024; f.hum = 101; f.dew = 501; f.pres = 511; f.ptend = 3;
    f.vis = 127; f.level = 4001; f.ltrend = 3; f.cur = 255; f.cdir = 360; f.wave = 255; f.waveS = 63; f.waveDir = 360;
    f.sea = 13; f.water = 501; f.precip = 7; f.sal = 510; f.ice = 3;
    return msgFi31(mmsi, f);
}

static bool has(const std::string& j, const std::string& k) { return j.find(k) != std::string::npos; }

static void testFi31() {
    std::printf("── DAC 1 FI 31 met/hydro (Circ.289) ──\n");
    am::Kind k; am::Met m; am::Notice n;
    const auto b = msgFi31(992351001, Fi31());
    CHECK(b.size() * 8 == 360, "a full FI 31 is 360 bits (%zu)", b.size() * 8);
    CHECK(am::parse(b, k, m, n) == 1 && k.dac == 1 && k.fi == 31 && !k.addressed, "read as met (dac %d fi %d)", k.dac, k.fi);
    CHECK(std::abs(m.lon / 1e7 - -1.2345) < 2e-5 && std::abs(m.lat / 1e7 - 50.6789) < 2e-5, "position %f %f", m.lat / 1e7, m.lon / 1e7);
    CHECK(m.day == 16 && m.hour == 9 && m.minute == 41, "the observation time");
    CHECK(m.windKn == 14 && m.gustKn == 21 && m.windDir == 225 && m.gustDir == 230, "wind");
    CHECK(m.air10 == 112 && m.hum == 87 && m.dew10 == 91, "air, humidity, dew point");
    CHECK(m.hPa == 1012 && m.pTrend == 1, "pressure %d trend %d", m.hPa, m.pTrend);
    CHECK(m.vis10 == 45 && !m.visMore, "visibility 4.5 NM");
    CHECK(m.level100 == 321 && m.levelTrend == 2, "tide 3.21 m rising (%d)", m.level100);
    CHECK(m.cur10 == 13 && m.curDir == 80, "surface current");
    CHECK(m.wave10 == 12 && m.waveS == 6 && m.waveDir == 200, "waves");
    CHECK(m.swell10 == am::kNone && m.swellS == am::kNone && m.swellDir == am::kNone, "swell not available → nothing");
    CHECK(m.sea == 4 && m.water10 == 120 && m.precip == 1 && m.sal10 == am::kNone && m.ice == 0, "sea state, water, rain, salinity n/a, no ice");
    const std::string line = am::metLine(m);
    CHECK(line == "wind SW 14 kn gusting 21 \xc2\xb7 11.2 \xc2\xb0" "C \xc2\xb7 1012 hPa falling \xc2\xb7 visibility 4.5 NM \xc2\xb7 tide 3.21 m rising \xc2\xb7 waves 1.2 m 6 s \xc2\xb7 sea 12.0 \xc2\xb0" "C",
          "the readable line: %s", line.c_str());
    std::string j; am::metJson(m, 42, j);
    CHECK(has(j, "\"wind\":14") && has(j, "\"airC\":11.2") && has(j, "\"hPa\":1012") && has(j, "\"level\":3.21") && has(j, "\"obs\":\"16 09:41\"")
          && has(j, "\"age\":42") && !has(j, "\"swell\"") && !has(j, "\"sal\""), "the JSON: %s", j.c_str());

    // ★ every "not available" code
    CHECK(am::parse(msgFi31NA(992351001), k, m, n) == 0, "a report with nothing available is not shown");
    am::metFi31(vibeais::Bits{msgFi31NA(1)}, 56, m);
    CHECK(m.lat == am::kNoPos32 && m.hour == 24 && m.minute == 60 && !am::hasAny(m), "all n/a: no position, no time, no reading");

    // ★ the edges
    Fi31 e; e.pres = 0; e.air = -123; e.dew = -200; e.water = -15; e.level = 0; e.visMore = 1; e.vis = 126; e.wind = 126;
    am::parse(msgFi31(1, e), k, m, n);
    CHECK(m.hPa == 799 && m.air10 == -123 && m.dew10 == -200 && m.water10 == -15 && m.level100 == -1000 && m.visMore && m.vis10 == 126 && m.windKn == 126,
          "edges: 799 hPa, −12.3 °C, −20.0 dew, −1.5 sea, −10.00 m, over 12.6 NM, 126 kn");
    e.pres = 402; e.level = 4000; am::parse(msgFi31(1, e), k, m, n);
    CHECK(m.hPa == 1201 && m.level100 == 3000, "1201 hPa (≥), +30.00 m");
    CHECK(am::metLine(m).find("visibility over 12.6 NM") != std::string::npos, "\"over\" in words");
    // ★ a transmitter that leaves the 10 spare bits off still decodes; one cut inside the table does not
    CHECK(am::parse(msgFi31(1, Fi31(), false), k, m, n) == 1, "no trailing spare: still read");
    auto cut = msgFi31(1, Fi31()); cut.resize(30);
    CHECK(am::parse(cut, k, m, n) == 0, "cut short inside the table: not read");
}

static void testFi11() {
    std::printf("── DAC 1 FI 11 met/hydro (Circ.236, withdrawn) ──\n");
    Builder B; hdr8(B, 2320001, 1, 11);
    B.u(uint64_t(std::lround(51.5 * 60000)), 24); B.s(std::lround(-0.25 * 60000), 25);
    B.u(3, 5); B.u(22, 5); B.u(5, 6);
    B.u(30, 7); B.u(42, 7); B.u(270, 9); B.u(275, 9);
    B.u(600 + 55, 11); B.u(70, 7); B.u(200 + 10, 10); B.u(1005 - 800, 9); B.u(0, 2);
    B.u(80, 8); B.u(100 + 45, 9); B.u(1, 2); B.u(25, 8); B.u(90, 9);
    B.u(255, 8); B.u(511, 9); B.u(31, 5); B.u(255, 8); B.u(511, 9); B.u(31, 5);
    B.u(30, 8); B.u(8, 6); B.u(250, 9); B.u(255, 8); B.u(63, 6); B.u(511, 9); B.u(6, 4); B.u(100 + 95, 10); B.u(7, 3); B.u(511, 9); B.u(3, 2); B.u(0, 6);
    B.pad8();
    am::Kind k; am::Met m; am::Notice n;
    CHECK(am::parse(B.bytes(), k, m, n) == 1 && k.fi == 11 && m.fi == 11, "read as met FI 11");
    CHECK(std::abs(m.lat / 1e7 - 51.5) < 2e-5 && std::abs(m.lon / 1e7 - -0.25) < 2e-5, "lat first, then lon (%f %f)", m.lat / 1e7, m.lon / 1e7);
    CHECK(m.windKn == 30 && m.gustKn == 42 && m.windDir == 270 && m.air10 == 55 && m.dew10 == 10 && m.hPa == 1005 && m.pTrend == 0, "offsets: 5.5 °C, 1.0 dew, 1005 hPa steady");
    CHECK(m.vis10 == 80 && m.level100 == 450 && m.levelTrend == 1 && m.cur10 == 25 && m.wave10 == 30 && m.waveS == 8 && m.sea == 6 && m.water10 == 95,
          "visibility 8 NM, tide 4.5 m falling, current, waves, sea 9.5 °C (level %d)", m.level100);
    CHECK(m.precip == am::kNone && m.ice == am::kNone && m.swell10 == am::kNone, "n/a stays nothing");
}

static void testType6() {
    std::printf("── type 6 (addressed) ──\n");
    Builder B; hdr6(B, 2320002, 235012345, 1, 30); B.u(7, 10); B.text("PILOT BOARDS 1400", 17); B.pad8();
    am::Kind k; am::Met m; am::Notice n;
    CHECK(am::parse(B.bytes(), k, m, n) == 2 && k.addressed && k.fi == 30 && n.to == 235012345 && n.text == "PILOT BOARDS 1400", "addressed text to %u: '%s'", n.to, n.text.c_str());
    Builder C; hdr6(C, 2320002, 1, 235, 10); C.u(0, 40); C.pad8();
    CHECK(am::parse(C.bytes(), k, m, n) == 0 && k.dac == 235 && k.fi == 10, "a regional DAC is counted, not read");
}

static void subCircle(Builder& B, double lat, double lon, int scale, int radius) {
    B.u(0, 3); B.u(uint64_t(scale), 2); B.s(std::lround(lon * 60000), 25); B.s(std::lround(lat * 60000), 24); B.u(4, 3); B.u(uint64_t(radius), 12); B.u(0, 18);
}
static void subText(Builder& B, const std::string& t) { B.u(5, 3); B.text(t, 14); }
static void testNotice() {
    std::printf("── area notices (FI 22 / 23) and text (FI 29) ──\n");
    Builder B; hdr8(B, 2320003, 1, 22);
    B.u(12, 10); B.u(18, 7); B.u(11, 4); B.u(16, 5); B.u(14, 5); B.u(0, 6); B.u(180, 18);
    subCircle(B, 50.79, -1.11, 1, 150);   // 150 × 10 m = 1.5 km
    subText(B, "BERTH 44"); subText(B, "DREDGER");
    B.pad8();
    am::Kind k; am::Met m; am::Notice n;
    CHECK(am::parse(B.bytes(), k, m, n) == 2 && n.fi == 22 && n.what == 18, "fairway closed (what %d)", n.what);
    CHECK(n.shape == 0 && std::abs(n.lat - 50.79) < 1e-4 && std::abs(n.lon - -1.11) < 1e-4 && std::abs(n.sizeM - 1500) < 0.1, "circle 1.5 km at the place");
    CHECK(n.month == 11 && n.day == 16 && n.hour == 14 && n.minute == 0 && n.durationMin == 180 && n.text == "BERTH 44 DREDGER", "time, 3 h, the words '%s'", n.text.c_str());
    const std::string line = am::noticeLine(n);
    CHECK(line == "Caution: fairway closed: BERTH 44 DREDGER \xc2\xb7 circle 1.5 km at 50.7900 -1.1100 \xc2\xb7 from 14:00 UTC 16-11, for 3 h", "the line: %s", line.c_str());

    Builder R; hdr8(R, 2320003, 1, 22);
    R.u(1, 10); R.u(37, 7); R.u(11, 4); R.u(17, 5); R.u(8, 5); R.u(30, 6); R.u(262143, 18);
    R.u(1, 3); R.u(2, 2); R.s(std::lround(-4.0 * 60000), 25); R.s(std::lround(50.2 * 60000), 24); R.u(4, 3); R.u(30, 8); R.u(50, 8); R.u(0, 9); R.u(0, 5);
    R.pad8();
    CHECK(am::parse(R.bytes(), k, m, n) == 2 && n.what == 37 && n.shape == 1 && std::abs(n.sizeM - 5000) < 0.1 && n.durationMin == 262143, "rectangle 5 km, firing area");
    CHECK(am::noticeLine(n).find("Restricted: firing danger area") == 0 && am::noticeLine(n).find(", for") == std::string::npos, "262143 = no duration given: %s", am::noticeLine(n).c_str());
    n.durationMin = 0;
    CHECK(am::noticeLine(n).find(", CANCELLED") != std::string::npos, "duration 0 = the notice is cancelled");
    CHECK(std::string(am::noticeName(22)).empty() && std::string(am::noticeName(126)) == "Cancelled", "reserved code empty; 126 cancelled");
    n.what = 22; n.text.clear(); n.shape = -1;
    CHECK(am::noticeLine(n).find("Area notice 22") == 0, "a reserved code still says what it is");

    Builder T; hdr8(T, 2320004, 1, 29); T.u(3, 10); T.text("SWING BRIDGE OPEN", 17); T.pad8();
    CHECK(am::parse(T.bytes(), k, m, n) == 2 && n.fi == 29 && n.text == "SWING BRIDGE OPEN" && am::noticeLine(n) == "SWING BRIDGE OPEN", "text description");
}

static std::vector<uint8_t> msg21(uint32_t mmsi, const std::string& name, double lat, double lon, int status) {
    Builder B; B.u(21, 6); B.u(0, 2); B.u(mmsi, 30); B.u(20, 5); B.text(name, 20); B.u(1, 1);
    B.s(std::lround(lon * 600000), 28); B.s(std::lround(lat * 600000), 27); B.u(0, 9); B.u(0, 9); B.u(0, 6); B.u(0, 6); B.u(7, 4);
    B.u(60, 6); B.u(0, 1); B.u(uint64_t(status), 8); B.u(0, 1); B.u(0, 1); B.u(0, 1); B.u(0, 1);
    return B.bytes();
}
static void testAtonStatus() {
    std::printf("── type 21: the AtoN status byte (IALA A-126 page 7) ──\n");
    vibeais::Msg m;
    CHECK(vibeais::decode(msg21(992351234, "NAB TOWER", 50.668, -0.952, 0b11100010), m) && m.atonStatus == 0b11100010, "the byte carried (%d)", m.atonStatus);
    am::AtonStatus s = am::atonStatus(0b11100010);
    CHECK(s.valid && s.racon == 0 && s.light == 1 && !s.alarm, "light on, no RACON, healthy");
    s = am::atonStatus(0b11110111);
    CHECK(s.valid && s.racon == 2 && s.light == 3 && s.alarm, "RACON working, light FAULT, alarm");
    CHECK(!am::atonStatus(0b00000111).valid && !am::atonStatus(-1).valid, "page 0 is not read");
    vibeais::Service svc;
    vibeais::RawFrame f; f.bytes = msg21(992351234, "NAB TOWER", 50.668, -0.952, 0b11100111); svc.acceptForTest(0, f);
    const std::string j = svc.report();
    CHECK(has(j, "\"light\":3") && has(j, "\"racon\":0") && has(j, "\"alarm\":1") && has(j, "LIGHT FAULT") && has(j, "ALARM"), "the report and the line: %s", j.substr(0, 600).c_str());
}

static std::string armour(const std::vector<uint8_t>& b, size_t nbits, int& fill) {
    fill = int((6 - nbits % 6) % 6);
    std::string o;
    for (size_t i = 0; i < nbits + size_t(fill); i += 6) {
        int v = 0;
        for (int k = 0; k < 6; ++k) { const size_t at = i + size_t(k); v = (v << 1) | (at < nbits ? (b[at / 8] >> (7 - at % 8)) & 1 : 0); }
        o += char(v < 40 ? v + 48 : v + 56);
    }
    return o;
}
static std::vector<uint8_t> unarmour(const std::string& p, int fill) {
    Builder B;
    for (char c : p) { int v = c - 48; if (v > 40) v -= 8; B.u(uint64_t(v & 63), 6); }
    for (int i = 0; i < fill; ++i) B.bits.pop_back();
    return B.bytes();
}
static void testNmea() {
    std::printf("── the NMEA armour round trip ──\n");
    const auto b = msgFi31(992351001, Fi31());
    int fill = 0;
    const std::string p = armour(b, 360, fill);
    std::printf("   !AIVDM,…,%s,%d (synthetic FI 31)\n", p.c_str(), fill);
    am::Kind k; am::Met m, m2; am::Notice n;
    CHECK(p.size() == 60 && fill == 0, "60 characters, no fill");
    CHECK(am::parse(unarmour(p, fill), k, m, n) == 1 && am::parse(b, k, m2, n) == 1 && std::memcmp(&m, &m2, sizeof m) == 0, "the same report after the armour");
}

static void testService() {
    std::printf("── the service: met on the station's row, notices ──\n");
    vibeais::Service s;
    std::vector<vibestation::Heard> heard;
    auto put = [&](int ch, const std::vector<uint8_t>& b) { vibeais::RawFrame f; f.bytes = b; f.levelDb = -30; f.snrDb = 25; s.acceptForTest(ch, f); };
    put(0, msgFi31(992351001, Fi31()));
    std::string j = s.report();
    CHECK(s.vesselCount() == 1 && has(j, "\"mmsi\":992351001,\"kind\":\"aton\"") && has(j, "\"lat\":50.67890") && has(j, "\"met\":[{\"fi\":31"), "a met-only AtoN: a row at the readings' place: %s", j.substr(0, 500).c_str());
    CHECK(has(j, "\"binary\":1"), "counted");
    // its own type 21 moves it to its own position; the met stays
    put(1, msg21(992351001, "BRAMBLE BANK", 50.79, -1.29, 0b11100010));
    j = s.report();
    CHECK(has(j, "\"name\":\"BRAMBLE BANK\"") && has(j, "\"lat\":50.79000") && has(j, "\"met\":[{") && has(j, "\"light\":1"), "own position wins, met kept");
    // ★ a base station sending FIVE sites' weather: four kept, newest first, the oldest gone
    for (int i = 0; i < 5; ++i) { Fi31 f; f.lat = 50.0 + i * 0.1; f.wind = 10 + i; put(0, msgFi31(2320777, f)); }
    j = s.report();
    const size_t at = j.find("\"mmsi\":2320777");
    const std::string row = j.substr(at, j.find("\"line\"", at) - at);
    int sites = 0; for (size_t p = row.find("\"fi\":31"); p != std::string::npos; p = row.find("\"fi\":31", p + 1)) ++sites;
    CHECK(has(row, "\"kind\":\"base\"") && sites == 4 && row.find("\"wind\":14") < row.find("\"wind\":13") && !has(row, "\"wind\":10,"), "a base station: 4 sites, newest first (%d)", sites);
    // the same site again: replaced, not added
    { Fi31 f; f.lat = 50.4; f.wind = 33; put(0, msgFi31(2320777, f)); }
    j = s.report();
    { const size_t a2 = j.find("\"mmsi\":2320777"); const std::string r2 = j.substr(a2, j.find("\"line\"", a2) - a2);
      CHECK(has(r2, "\"wind\":33") && !has(r2, "\"wind\":14,"), "the same site is replaced"); }
    // notices: deduplicated and bounded
    Builder B; hdr8(B, 2320003, 1, 29); B.u(3, 10); B.text("SWING BRIDGE OPEN", 17); B.pad8();
    put(0, B.bytes()); put(1, B.bytes());
    j = s.report();
    int nn = 0; for (size_t p = j.find("SWING BRIDGE"); p != std::string::npos; p = j.find("SWING BRIDGE", p + 1)) ++nn;
    CHECK(has(j, "\"notices\":[{") && nn == 1, "a repeated notice once (%d)", nn);
    for (int i = 0; i < 40; ++i) { Builder C; hdr8(C, 2320003, 1, 29); C.u(3, 10); C.text("NOTICE " + std::to_string(i), 9); C.pad8(); put(0, C.bytes()); }
    j = s.report();
    const std::string notes = j.substr(j.find("\"notices\":["));
    int cnt = 0; for (size_t p = notes.find("\"fi\":29"); p != std::string::npos; p = notes.find("\"fi\":29", p + 1)) ++cnt;
    CHECK(cnt == 20 && has(notes, "NOTICE 39") && !has(notes, "NOTICE 19\""), "bounded at 20, newest kept (%d)", cnt);
    // a regional binary message: counted, no row
    Builder R; hdr8(R, 244000001, 200, 10); R.u(0, 100); R.pad8();
    const size_t before = s.vesselCount(); put(0, R.bytes());
    CHECK(s.vesselCount() == before, "a regional binary message makes no row");
}

static std::string tmpDir() {
    char b[256]; std::snprintf(b, sizeof b, "/tmp/vibe-ais-binary-%d", (int)::getpid());
    std::string cmd = std::string("rm -rf ") + b; if (std::system(cmd.c_str())) {}
    ::mkdir(b, 0755);
    return b;
}
static void testStore() {
    std::printf("── the met store (ais-met.dat) ──\n");
    const std::string d = tmpDir();
    const int64_t wall = 1'800'000'000;
    vibeais::Vessel v; v.mmsi = 992351001; v.kind = "aton"; v.name = "BRAMBLE BANK"; v.lat = 50.79; v.lon = -1.29; v.last = 100; v.first = 10; v.posAt = 100;
    v.atonStatus = 0b11110111;
    am::Kind k; am::Notice n;
    am::parse(msgFi31(v.mmsi, Fi31()), k, v.met[0], n); v.metAt[0] = 90; v.nMet = 1;
    CHECK(st::write(d, 100ull << 20, { st::fromVessel(v, 100, wall) }) == 1 && st::writeMet(d, 100ull << 20, { v }, 100, wall) == 1, "written");
    st::Query q; size_t total = 0;
    const auto got = st::read(d, q, &total);
    const std::string j = st::toJson(got, wall + 30, false, total, d);
    CHECK(has(j, "\"met\":[{\"fi\":31,\"age\":40") && has(j, "\"wind\":14") && has(j, "\"level\":3.21"), "the map's JSON carries the weather: %s", j.c_str());
    CHECK(has(j, "\"light\":3") && has(j, "\"racon\":2") && has(j, "\"alarm\":1"), "and the status byte, through the record's flags");
    CHECK(!has(st::toJson(got, wall, false, total), "\"met\""), "no dir, no met (the old call)");
    // a second site, then the first again newer
    vibeais::Vessel w = v; Fi31 f2; f2.lat = 51.0; f2.wind = 5; am::parse(msgFi31(v.mmsi, f2), k, w.met[0], n); w.metAt[0] = 100; w.last = 100;
    st::writeMet(d, 100ull << 20, { w }, 100, wall + 60);
    auto mets = st::readMet(d);
    CHECK(mets.size() == 1 && mets[0].n == 2, "two sites merged (%zu, %d)", mets.size(), mets.empty() ? -1 : mets[0].n);
    // bounded: 300 stations into the smallest gallery
    std::vector<vibeais::Vessel> many;
    for (int i = 0; i < 300; ++i) { vibeais::Vessel x = v; x.mmsi = 992350000 + uint32_t(i); x.last = 100 + i; many.push_back(x); }
    const uint64_t small = 1ull << 20;
    st::writeMet(d, small, many, 400, wall + 1000);
    mets = st::readMet(d);
    struct stat sb{}; ::stat((d + "/ais-met.dat").c_str(), &sb);
    CHECK(mets.size() == st::metCapFor(small) && st::metCapFor(small) < 300 && uint64_t(sb.st_size) <= st::kHeader + uint64_t(st::metCapFor(small)) * st::kMetRecSize,
          "at most %u stations in a 1 MB gallery (%zu, %lld B)", st::metCapFor(small), mets.size(), (long long)sb.st_size);
    CHECK(st::metCapFor(100ull << 20) == st::kMetMaxCap && st::metCapFor(1) == st::kMetMinCap, "128 at the default size, 8 at the least");
    CHECK(st::bytesOnDisk(d) >= uint64_t(sb.st_size), "counted in the gallery's side bytes");
    st::write(d, 0, {});
    CHECK(::access((d + "/ais-met.dat").c_str(), F_OK) != 0, "a gallery of 0 removes it");
    std::string cmd = "rm -rf " + d; if (std::system(cmd.c_str())) {}
}

static void testGarbage() {
    std::printf("── garbage: random payloads on every DAC/FI ──\n");
    std::mt19937 rng(7);
    int crashes = 0, bad = 0, read = 0;
    for (int it = 0; it < 200000; ++it) {
        const int bytes = 1 + int(rng() % 126);
        std::vector<uint8_t> b{}; b.resize(size_t(bytes)); for (auto& x : b) x = uint8_t(rng());
        b[0] = uint8_t(((rng() & 1) ? 8 : 6) << 2);
        if (rng() & 1) { // force DAC 1 and a known FI half the time
            Builder B; const int fis[] = { 11, 22, 23, 29, 30, 31 }; const int fi = fis[rng() % 6];
            if (b[0] >> 2 == 8) hdr8(B, rng() & 0x3fffffff, 1, fi); else hdr6(B, rng() & 0x3fffffff, rng() & 0x3fffffff, 1, fi);
            const auto h = B.bytes(); for (size_t i = 0; i < h.size() && i < b.size(); ++i) b[i] = h[i];
        }
        am::Kind k; am::Met m; am::Notice n;
        const int r = am::parse(b, k, m, n);
        if (r == 1) {
            ++read;
            std::string j; am::metJson(m, 0, j);
            if (m.windKn != am::kNone && (m.windKn < 0 || m.windKn > 126)) ++bad;
            if (m.hPa != am::kNone && (m.hPa < 799 || m.hPa > 1201)) ++bad;
            if (m.lat != am::kNoPos32 && std::abs(m.lat / 1e7) > 90) ++bad;
            if (j.find("nan") != std::string::npos || j.find("inf") != std::string::npos) ++bad;
        } else if (r == 2) { ++read; const std::string l = am::noticeLine(n); if (l.find("nan") != std::string::npos) ++bad; }
    }
    CHECK(crashes == 0 && bad == 0, "no out-of-range value in %d read (%d bad)", read, bad);
}

int main() {
    testFi31();
    testFi11();
    testType6();
    testNotice();
    testAtonStatus();
    testNmea();
    testService();
    testStore();
    testGarbage();
    std::printf("\ntest-ais-binary: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
