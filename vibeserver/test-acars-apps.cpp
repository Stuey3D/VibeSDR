// test-acars-apps.cpp — V12 aviation datalink: the ACARS applications (vibe_acars_apps.h). docs/v12/AVIATION.md §Applications.
//   1. the two CRCs on published samples (ARINC 622: libacars' documentation; FMS downlinks: the MIT sample set)
//   2. ADS-C: the published samples decode to their published positions; CPDLC is NAMED and never decoded
//   3. FMS position reports (CRC-checked) — five published layouts
//   4. the fixed airline layouts: label 16, 15, 44, QP–QS, H2 02E (one of them from our own VDL2 capture)
//   5. near misses are NOT parsed: a wrong CRC, a minute of 60, a bogus text, an uplink where only downlinks are defined
//   6. multi-block reassembly (ETB), bounded
//   7. round trip: an ADS-C report through our ACARS transmitter (vibe_avi_tx.h), the receiver, the parser
//   8. the table: positions + altitude + time on the row, CPDLC and bodies never leave without the owner's opt-in
#include "vibe_acars_apps.h"
#include "vibe_avi_service.h"
#include "vibe_avi_tx.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using vibeacarsapps::parse;
using vibeacarsapps::Result;
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

int main() {
    std::printf("1. the CRCs on published samples\n");
    {
        const char* s622[] = { "ADS.VQ-BPJ1423CCA85D2D090886301D0D24C7D0704309088442255CC87CE2C90880DF97",
                               "ADS.N572UP07263B5872A048C9F21C1F0E5B88D700000239",
                               "AT1.9V-SVG21D0755D84AD067448398722949A7521C8AB4A1C8EAB5CE393" };
        for (const char* s : s622) {
            std::vector<uint8_t> b(s, s + 10);
            for (const char* p = s + 10; p[0] && p[1]; p += 2) b.push_back(uint8_t(vibeacarsapps::hexVal(p[0]) << 4 | vibeacarsapps::hexVal(p[1])));
            CHECK(vibeacarsapps::crc622(b.data(), b.size()) == 0x1D0F, "ARINC 622 residue %04x for %.12s", vibeacarsapps::crc622(b.data(), b.size()), s);
        }
        const char* s702[] = { "POSN43312W123174,EASON,215754,370,EBINY,220601,ELENN,M48,02216,185/TS215754,0921227A40",
                               "POSN38531W078000,CSN-01,112309,310,CYN-02,114151,ACK,M40,26067,22479226" };
        for (const char* s : s702) {
            const size_t n = std::strlen(s);
            const unsigned want = unsigned(std::strtoul(s + n - 4, nullptr, 16));
            CHECK(vibeacarsapps::crc702(s, n - 4) == want, "FMS CRC %04x want %04x", vibeacarsapps::crc702(s, n - 4), want);
        }
    }

    std::printf("2. ADS-C (ARINC 622): published samples → their published positions; CPDLC named only\n");
    {
        Result r = parse("B6", "/LPAFAYA.ADS.VQ-BPJ1423CCA85D2D090886301D0D24C7D0704309088442255CC87CE2C90880DF97", true);
        CHECK(r.app == "adsc" && r.hasPos && near(r.lat, 50.3429604, 1e-6) && near(r.lon, 16.3785553, 1e-6) && r.altFt == 37000 &&
              r.fixPastHour == 396 * 8, "waypoint-change event: %s %.7f %.7f %d %d", r.kind.c_str(), r.lat, r.lon, r.altFt, r.fixPastHour);
        CHECK(vibeacarsapps::fixTimeText(r) == ":06:36", "time past the hour '%s'", vibeacarsapps::fixTimeText(r).c_str());
        r = parse("H1", "#M1B/B6 LHWE1YA.ADS.N572UP07263B5872A048C9F21C1F0E5B88D700000239", true);
        CHECK(r.app == "adsc" && r.hasPos && near(r.lat, 53.7634850, 1e-6) && near(r.lon, 20.1490974, 1e-6) && r.altFt == 35996 &&
              r.fixPastHour == 3207 * 8, "H1 basic report: %.7f %.7f %d %d", r.lat, r.lon, r.altFt, r.fixPastHour);
        const char* more[] = { "/BOMASAI.ADS.VT-ANB072501A070A988CA73248F0E5DC10200000F5EE1ABC000102B885E0A19F5",
                               "/AUHASMO.ADS.A6-PFE0724D9586A36C92B2DCF1F0E74A8E4807C0F7219AF407C10422E9E08A1C4",
                               "/CTUE1YA.ADS.HB-JNB1424AB686D9308CA2EBA1D0D24A2C06C1B48CA004A248050667908CA004BF6",
                               "/YQXE2YA.ADS.SP-LRH1424FD087806C0B527769F0D2500B877ED00B5401E2516707755C01340B768" };
        for (const char* m : more) {
            r = parse("B6", m, true);
            std::printf("   %.20s → %s %.4f %.4f %d ft %s\n", m, r.kind.c_str(), r.lat, r.lon, r.altFt, vibeacarsapps::fixTimeText(r).c_str());
            CHECK(r.app == "adsc" && r.hasPos && r.altFt > 1000 && r.altFt < 50000, "published sample %.16s", m);
        }
        r = parse("AA", "/AKLCDYA.AT1.9V-SVG21D0755D84AD067448398722949A7521C8AB4A1C8EAB5CE393", false);
        CHECK(r.app == "cpdlc" && r.kind == "CPDLC" && !r.hasPos && r.from.empty() && r.flight.empty(), "CPDLC named, nothing decoded: '%s'", r.kind.c_str());
        r = parse("B6", "/LPAFAYA.ADS.VQ-BPJ1423CCA85D2D090886301D0D24C7D0704309088442255CC87CE2C90880DF97", false);
        CHECK(r.app == "adsc" && !r.hasPos, "an uplink ADS message is a contract request, no position: %s", r.kind.c_str());
    }

    std::printf("3. FMS position reports (H1, CRC-checked)\n");
    {
        struct V { const char* t; double la, lo; int alt; int sec; } v[] = {
            { "POSN43312W123174,EASON,215754,370,EBINY,220601,ELENN,M48,02216,185/TS215754,0921227A40", 43.52, -123.29, 37000, 21 * 3600 + 57 * 60 + 54 },
            { "POSN45209W122550,PEGTY,220309,134,MINNE,220424,HISKU,M6,060013,269,366,355K,292K,730A5B", 45.348, -122.917, 13400, 22 * 3600 + 3 * 60 + 9 },
            { "POSN33225W079428,SCOOB,232933,340,ENEME,235712,FETAL,M42,003051,15857F6", 33.375, -79.713, 34000, 23 * 3600 + 29 * 60 + 33 },
            { "#M1BPOS/ID746026,,/DC03032024,173207/MR1,/ET031846/PSN42579W108090,173207,320,WAIDE,031759,WEDAK,M49,267070,T468/CG264,110,360/FB742/VR324E17", 42.965, -108.15, 32000, 17 * 3600 + 32 * 60 + 7 },
            { "/.POS/TS100316,210324/PSS35333W058220,,100316,250,S37131W059150,101916,S39387W060377,M23,27282,241,780,MANUAL,0,813E711", -35.555, -58.367, 25000, 10 * 3600 + 3 * 60 + 16 },
        };
        for (const auto& x : v) {
            Result r = parse("H1", x.t, true);
            CHECK(r.app == "pos" && r.kind == "FMS position" && near(r.lat, x.la, 0.001) && near(r.lon, x.lo, 0.001) && r.altFt == x.alt && r.fixSec == x.sec,
                  "%.24s → %s %.4f %.4f %d %d", x.t, r.kind.c_str(), r.lat, r.lon, r.altFt, r.fixSec);
        }
        std::string bad = v[0].t; bad[5] = '4';   // one digit changed: the CRC refuses it
        CHECK(parse("H1", bad, true).kind.empty(), "a changed digit fails the CRC — nothing shown");
        CHECK(parse("H1", v[0].t, false).kind.empty(), "FMS downlink layout on an uplink: not parsed");
    }

    std::printf("4. the fixed airline layouts\n");
    {
        Result r = parse("16", "N 44.203,W 86.546,31965,6, 290", true);
        CHECK(r.hasPos && near(r.lat, 44.203, 1e-9) && near(r.lon, -86.546, 1e-9) && r.hasAlt && r.altFt == 31965, "16 v1 %.3f %.3f %d", r.lat, r.lon, r.altFt);
        r = parse("16", "N 44.988,W121.644,35940,6, 170", true);
        CHECK(r.hasPos && near(r.lon, -121.644, 1e-9) && r.altFt == 35940, "16 v1b");
        r = parse("16", "N 28.177/W 96.055", true);
        CHECK(r.hasPos && near(r.lat, 28.177, 1e-9) && near(r.lon, -96.055, 1e-9) && !r.hasAlt, "16 v2");
        r = parse("15", "(2N38448W 77216--- 28 20  7(Z", true);
        CHECK(r.hasPos && near(r.lat, 38.747, 0.001) && near(r.lon, -77.360, 0.001) && !r.hasAlt, "15 %.3f %.3f", r.lat, r.lon);
        r = parse("15", "(2N39018W 77284OFF11112418101313--------(Z", true);
        CHECK(r.hasPos && near(r.lat, 39.030, 0.001) && near(r.lon, -77.473, 0.001) && r.oooi == "off" && r.oooiSec == 18 * 3600 + 10 * 60, "15 OFF %s %d", r.oooi.c_str(), r.oooiSec);
        r = parse("15", "(2N42589W 83520OFF------13280606--------(Z", true);
        CHECK(r.oooi == "off" && r.oooiSec == 13 * 3600 + 28 * 60 && near(r.lat, 42.982, 0.001), "15 OFF no date");
        r = parse("44", "POS02,N38171W077507,319,KJFK,KUZA,0926,0245,0327,004.6", true);
        CHECK(r.hasPos && r.lat == 38 + 17.1 / 60 && near(r.lon, -77.845, 1e-9) && r.altFt == 31900 && r.from == "KJFK" && r.to == "KUZA", "44 POS");
        r = parse("44", "OFF02,N39247W077226,KFDK,KSNA,1106,2124,0248,011.1", true);
        CHECK(r.oooi == "off" && r.oooiSec == 21 * 3600 + 24 * 60 && r.from == "KFDK" && r.to == "KSNA" && near(r.lat, 39.412, 0.001), "44 OFF");
        r = parse("44", "ON01,N33522W084181,KCLT,KPDK,1106,004023,---.-,", true);
        CHECK(r.oooi == "on" && r.oooiSec == 40 * 60 + 23 && near(r.lon, -84.302, 0.001), "44 ON");
        r = parse("44", "IN01,N33528W084181,KCLT,KPDK,1106,0045,---.-", true);
        CHECK(r.oooi == "in" && r.oooiSec == 45 * 60 && near(r.lat, 33.880, 0.001), "44 IN");
        r = parse("QP", "KSFOKLAX1234", true);
        CHECK(r.oooi == "out" && r.from == "KSFO" && r.to == "KLAX" && r.oooiSec == 12 * 3600 + 34 * 60 && !r.hasPos, "QP");
        r = parse("QR", "EGLLEDDF2030", true);
        CHECK(r.oooi == "on" && r.from == "EGLL" && r.to == "EDDF" && r.oooiSec == 20 * 3600 + 30 * 60, "QR");
        r = parse("QS", "KJFKKBOS1015 STATUS=OK", true);
        CHECK(r.oooi == "in" && r.oooiSec == 10 * 3600 + 15 * 60, "QS with a tail");
        r = parse("QQ", "KSDLKLAS0025\r\n001FE09002543N3336.6W11155.90281750042", true);
        CHECK(r.oooi == "off" && r.oooiSec == 25 * 60 + 43 && r.hasPos && near(r.lat, 33.61, 0.001) && near(r.lon, -111.931667, 0.001), "QQ long %d %.4f %.4f", r.oooiSec, r.lat, r.lon);
        // ★ our own capture's frame (Pi 500 VDL2, G-EZGJ — the text test-vdl2 §4 carries): GMMX → EGGW, the Bay of Biscay
        r = parse("H2", "02E31GMMXEGGWN45520W00444021213801M587100017G", true);
        CHECK(r.hasPos && near(r.lat, 45 + 52.0 / 60, 1e-9) && near(r.lon, -(4 + 44.0 / 60), 1e-9) && r.altFt == 38000 && r.fixSec == 21 * 3600 + 21 * 60 &&
              r.from == "GMMX" && r.to == "EGGW", "H2 02E (ours): %.4f %.4f %d %d %s-%s", r.lat, r.lon, r.altFt, r.fixSec, r.from.c_str(), r.to.c_str());
        r = parse("H2", "02E20HEGNLKPRN40359E02208116253601M627259020G    QN41179E02134316323599M617247037G    QN41591E02100516393603M610266040G    "
                        "QN42393E02026716463602M600276033G    QN43197E01954316533598M592299037G    QN44023E01929517003596M587313033G    Q", true);
        CHECK(r.hasPos && near(r.lat, 44 + 2.3 / 60, 1e-9) && near(r.lon, 19 + 29.5 / 60, 1e-9) && r.fixSec == 17 * 3600 && r.altFt == 35900 && r.from == "HEGN",
              "H2 02E: the LAST observation %.4f %.4f %d", r.lat, r.lon, r.fixSec);
    }

    std::printf("5. near misses are not parsed (counted, never guessed)\n");
    {
        const struct { const char* l; const char* t; bool down; } miss[] = {
            { "16", "N Bogus message", true }, { "16", "N 44.203,W 86.546", true }, { "16", "N 94.203,W 86.546,31965,6, 290", true },
            { "15", "(2N38648W 77216--- 28 20  7(Z", true },   // 64.8 minutes
            { "15", "(2 Bogus message", true }, { "44", "POS02 Bogus message", true }, { "44", "POS02,N38171W077507,319,KJFK,kuza,0926", true },
            { "QP", "KSFOKLAX2460", true }, { "QP", "KSFOKLAX12", true }, { "QP", "KSFOKLAX1234", false },
            { "H2", "02E31GMMXEGGWN45520W00444021213801M587100017X", true },
            { "B6", "/LPAFAYA.ADS.VQ-BPJ1423CCA85D2D090886301D0D24C7D0704309088442255CC87CE2C90880DF98", true },   // CRC
            { "B6", "/LPAFAYA.XYZ.VQ-BPJ1423CCA85D2D090886301D0D24C7D0704309088442255CC87CE2C90880DF97", true },
            { "H1", "#M1B/B6 lhwe1ya.ADS.N572UP07263B5872A048C9F21C1F0E5B88D700000239", true },
            { "20", "POSN38160W077075,,211733,360,OTT,212041,,N42,19689,40,544", true },   // ★ two published readings disagree
            { "H1", "HELLO FROM THE FLIGHT DECK", true }, { "5Z", "/B6 SOME TEXT", true }, { "_\x7f", "", true },
        };
        int got = 0;
        for (const auto& m : miss) { const Result r = parse(m.l, m.t, m.down); if (!r.kind.empty()) { ++got; std::printf("   parsed?! %s '%s' → %s\n", m.l, m.t, r.kind.c_str()); } }
        CHECK(got == 0, "%d near misses were parsed", got);
        // random noise in every label never yields a position
        std::mt19937 rng(5);
        int pos = 0;
        const char* labels[] = { "16", "15", "44", "QP", "QQ", "H2", "H1", "B6" };
        const char alpha[] = "0123456789ABCDEFNSEW ,./(#-QZGM";
        for (int i = 0; i < 200000; ++i) {
            std::string t(size_t(8 + rng() % 80), ' ');
            for (char& c : t) c = alpha[rng() % (sizeof alpha - 1)];
            if (parse(labels[i % 8], t, true).hasPos) ++pos;
        }
        CHECK(pos == 0, "random text in a position label: %d positions out of 200000", pos);
    }

    std::printf("6. multi-block reassembly (ETB)\n");
    {
        vibeacarsapps::Reassembler ra;
        auto blk = [](const char* reg, const char* msg, bool more, const std::string& text) {
            vibeacars::Block b; b.reg = reg; b.label = "B6"; b.downlink = true; b.msgNo = msg; b.more = more; b.text = text; return b;
        };
        const std::string full = "/LPAFAYA.ADS.VQ-BPJ1423CCA85D2D090886301D0D24C7D0704309088442255CC87CE2C90880DF97";
        std::string out;
        CHECK(!ra.add(blk("VQ-BPJ", "J61A", true, full.substr(0, 40)), 0, out), "first block held");
        CHECK(!ra.add(blk("VQ-BPJ", "J61A", true, full.substr(0, 40)), 1, out), "a resent first block counts once");
        CHECK(!ra.add(blk("G-EUPT", "M01A", false, "QP-other"), 1, out) || out == "QP-other", "another aircraft's single block passes through");
        CHECK(ra.add(blk("VQ-BPJ", "J61B", false, full.substr(40)), 2, out) && out == full, "joined: '%s'", out.c_str());
        CHECK(parse("B6", out, true).hasPos, "the joined message decodes");
        CHECK(ra.pending() == 0, "nothing left pending");
        for (int i = 0; i < 100; ++i) { char m[8]; std::snprintf(m, sizeof m, "%03dA", i); ra.add(blk("X", m, true, "abc"), 3, out); }
        CHECK(ra.pending() <= 32, "bounded: %zu pending", ra.pending());
        ra.add(blk("Y", "999A", true, "x"), 100, out);
        CHECK(ra.pending() == 1, "expired after 60 s: %zu", ra.pending());
    }

    std::printf("7. round trip: an ADS-C report through our ACARS transmitter and receiver\n");
    {
        const double fs = 2.4e6, hw = 131.675e6;
        const std::vector<double> ch = { 131.525e6, 131.725e6, 131.825e6 };
        std::vector<vibeavi::cf32> cap(size_t(fs * 1.2));
        std::mt19937 rng(9);
        std::normal_distribution<float> g(0.f, 0.02f);
        for (auto& v : cap) v = vibeavi::cf32(g(rng), g(rng));
        const std::string txt = std::string("J61A") + "SU2549" + "/LPAFAYA.ADS.VQ-BPJ1423CCA85D2D090886301D0D24C7D0704309088442255CC87CE2C90880DF97";
        auto b = vibeavitx::makeBlock('2', ".VQ-BPJ", '!', "B6", '8', txt);
        vibeavitx::acarsBurst(cap, fs, 131.725e6 - hw + 120, 0.05, size_t(fs * 0.1), vibeavitx::airBits(b));
        vibeacars::Receiver rx(fs, hw, ch);
        vibeavi::Tracker tr(false);
        int blocks = 0;
        Result got;
        for (size_t o = 0; o < cap.size(); o += 65536)
            rx.feed(cap.data() + o, std::min<size_t>(65536, cap.size() - o), [&](const vibeacars::Block& x) {
                ++blocks; got = parse(x.label, x.text, x.downlink); tr.addAcars(x, 1000.0);
            });
        CHECK(blocks == 1 && got.hasPos && near(got.lat, 50.3429604, 1e-6) && got.altFt == 37000, "over the air: %d blocks, %s", blocks, got.kind.c_str());
        const std::string j = tr.json(1001.0);
        CHECK(j.find("\"lat\":50.3430") != std::string::npos && j.find("\"alt\":37000") != std::string::npos && j.find("\"posSrc\":\"ADS-C\"") != std::string::npos
              && j.find("\"posTime\":\":06:36\"") != std::string::npos, "the row carries it: %s", j.c_str());
        CHECK(j.find("LPAFAYA") == std::string::npos && j.find("\"text\"") == std::string::npos, "no body without the opt-in");
    }

    std::printf("7b. our own encoders (vibe_avi_tx.h) round trip through the parser\n");
    {
        int bad = 0;
        std::mt19937 rng(77);
        for (int i = 0; i < 2000; ++i) {
            const double la = -89 + 178.0 * (rng() % 100000) / 100000.0, lo = -179 + 358.0 * (rng() % 100000) / 100000.0;
            const int alt = int(rng() % 12000) * 4;
            const double ph = (rng() % 28800) / 8.0;
            const Result r = parse("B6", vibeavitx::adsCBasic("EGLLXYA", ".G-EUPT", la, lo, alt, ph), true);
            if (!(r.hasPos && near(r.lat, la, 2e-4) && near(r.lon, lo, 2e-4) && r.altFt == alt && r.fixPastHour == int(std::lround(ph * 8)))) ++bad;
        }
        CHECK(bad == 0, "ADS-C encoder → parser: %d of 2000 wrong", bad);
        const Result f = parse("H1", vibeavitx::fmsPos("N51286W000380", 123456, 350), true);
        CHECK(f.kind == "FMS position" && near(f.lat, 51 + 28.6 / 60, 1e-9) && near(f.lon, -(0 + 38.0 / 60), 1e-9) && f.altFt == 35000 && f.fixSec == 12 * 3600 + 34 * 60 + 56,
              "FMS encoder → parser: %s %.4f %.4f", f.kind.c_str(), f.lat, f.lon);
    }

    std::printf("8. the table: positions, OOOI, CPDLC named, bodies only with the owner's opt-in\n");
    {
        vibeavi::Tracker tr(false);
        auto acars = [&](const char* reg, const char* label, const std::string& text, double t, bool down = true) {
            vibeacars::Block b; b.reg = reg; b.label = label; b.downlink = down; b.text = text; b.hz = 131.725e6; b.msgNo = "M01A"; b.flight = "BA0123";
            tr.addAcars(b, t);
        };
        acars("G-EUPT", "QP", "EGLLEDDF2030", 10);
        acars("G-EUPT", "H1", "#M1BPOSN43312W123174,EASON,215754,370,EBINY,220601,ELENN,M48,02216,185/TS215754,0921227A40", 20);
        acars("9V-SVG", "AA", "/AKLCDYA.AT1.9V-SVG21D0755D84AD067448398722949A7521C8AB4A1C8EAB5CE393", 21, false);
        acars("D-AIZZ", "H1", "FREE TEXT FROM THE CREW", 22);
        std::string j = tr.json(25);
        std::printf("   %s\n", j.c_str());
        CHECK(j.find("\"from\":\"EGLL\",\"to\":\"EDDF\"") != std::string::npos && j.find("\"oooi\":\"out\",\"oooiTime\":\"20:30\"") != std::string::npos, "OOOI on the row");
        CHECK(j.find("\"lat\":43.5200,\"lon\":-123.2900") != std::string::npos && j.find("\"alt\":37000") != std::string::npos &&
              j.find("\"posTime\":\"21:57:54\"") != std::string::npos && j.find("\"posSrc\":\"FMS\"") != std::string::npos, "FMS position on the same row");
        CHECK(j.find("\"app\":\"CPDLC\"") != std::string::npos && j.find("21D0755") == std::string::npos, "CPDLC named, its content never sent");
        CHECK(j.find("FREE TEXT") == std::string::npos && j.find("\"text\"") == std::string::npos, "free text never without the opt-in");
        CHECK(j.find("\"apps\":{\"pos\":1,\"oooi\":1,\"adsc\":0,\"cpdlc\":1,\"unparsed\":1}") != std::string::npos, "the counters");
        tr.setBodies(true);
        acars("D-AIZZ", "H1", "FREE TEXT FROM THE CREW", 23);
        j = tr.json(25, 3600, true);
        CHECK(j.find("FREE TEXT") != std::string::npos, "with the opt-in, on the owner's socket: the body");
        CHECK(tr.json(25, 3600, false).find("FREE TEXT") == std::string::npos, "…never to anyone else");
        // a later message without a position keeps the last position
        acars("G-EUPT", "QR", "EGLLEDDF2130", 30);
        j = tr.json(31);
        CHECK(j.find("\"lat\":43.5200") != std::string::npos && j.find("\"oooi\":\"on\"") != std::string::npos, "the last position stays, the OOOI moves on");
        // VDL2 carries ACARS too: the ICAO-keyed row gains the position
        vibevdl2::Frame f; f.src.type = 1; f.src.addr = 0x4CA97B; f.hz = 136.975e6; f.hasAcars = true; f.acarsCrcOk = true;
        f.acars.reg = "EI-EST"; f.acars.label = "16"; f.acars.downlink = true; f.acars.text = "N 51.500,W  1.250,12000,6, 290";
        tr.addVdl2(f, 40);
        j = tr.json(41);
        CHECK(j.find("\"icao\":\"4CA97B\",\"reg\":\"EI-EST\"") != std::string::npos && j.find("\"lat\":51.5000,\"lon\":-1.2500") != std::string::npos &&
              j.find("\"alt\":12000") != std::string::npos, "VDL2 row with its position");
    }

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
