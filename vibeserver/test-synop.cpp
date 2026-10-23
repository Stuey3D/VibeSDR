// test-synop.cpp — the RTTY weather-report translator (vibe_synop.h): SYNOP and SHIP in plain English (V12, 2026-10-23).
//
// ★★★ WHAT IT PINS. Each report's line says what its groups say and NOTHING ELSE: a group RTTY damaged is skipped and
//     counted, never read; a lost "333" does not turn section 3 into section 1; a station not in the public list is its
//     number; a report heard without its AAXX says "time not received" instead of borrowing an old bulletin's. And the
//     stream reader: a report is released at the end of ITS line, the memory it holds is bounded, and text that is not a
//     report (DWD's "CQ CQ DE DDK2", RYRYRY, "FREQUENCIES 4583 KHZ") never becomes one.
// ★ The real text is tools/rtty-bench/data/ddk-4583khz-2026-10-22.m4a through rtty_wav (the server's own RTTY decoder,
//   DWD's 50 Bd / 450 Hz reversed) — the decoder's output, pasted here verbatim, damage and all.
#include "vibe_synop.h"
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace vibe::synop;
static int fails = 0, passes = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (c) passes++; else fails++; }
static bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }
static bool detail(const Report& r, const std::string& sub) { for (auto& d : r.details) if (has(d, sub)) return true; return false; }

/** Everything the reader releases for `text`, pushed in `chunk`-byte pieces (the decoder prints a character or two at a
 *  time), then flushed. */
static std::vector<Report> readAll(const std::string& text, size_t chunk = 1, bool flush = true) {
    Reader rd; std::vector<Report> out;
    for (size_t o = 0; o < text.size(); o += chunk)
        for (auto& x : rd.push(text.substr(o, chunk))) out.push_back(x.report);
    if (flush) for (auto& x : rd.flush()) out.push_back(x.report);
    return out;
}

int main() {
    std::printf("── WMO-shaped SYNOP, every section-1 group ──\n");
    {
        // ★ The example the pymetdecoder project uses (a Falklands station, 00 UTC, knots).
        auto v = readAll("AAXX 01004\r\n88889 12782 61506 10094 20047 30111 40197 53007 60001 81541 333 81656 86070=\r\n");
        ok(v.size() == 1, "one report");
        if (v.size() == 1) {
            const Report& r = v[0];
            std::printf("     %s\n", r.line.c_str());
            ok(r.kind == "synop" && r.unreadable == 0, "a SYNOP, nothing unreadable");
            ok(has(r.line, "88889"), "the station number leads the line");
            ok(has(r.line, "00:00 UTC"), "the hour from AAXX's YYGGi");
            ok(has(r.line, "wind 150\xc2\xb0 6 kt (11 km/h)"), "wind: dd 15 → 150°, ff 06 with iw 4 → knots, and km/h");
            ok(has(r.line, "9.4 \xc2\xb0""C") && has(r.line, "1019.7 hPa"), "temperature 10094 → 9.4 °C, sea-level 40197 → 1019.7 hPa");

            ok(detail(r, "Visibility 40 km") && detail(r, "Lowest cloud 1500\xe2\x80\x93""2000 m"), "iRiXhVV 12782: cloud base 7, VV 82 = 40 km");
            ok(detail(r, "Cloud cover 6/8"), "N 6 → 6/8");
            ok(detail(r, "Dew point 4.7") && detail(r, "Pressure at the station 1011.1 hPa"), "dew point and station pressure");
            ok(detail(r, "Pressure has risen 0.7 hPa in 3 hours"), "5appp 53007: a 3 → risen, 0.7 hPa");
            ok(detail(r, "Precipitation 0 mm in 6 hours"), "6RRRt 60001: 0 mm in 6 hours");
            ok(detail(r, "Clouds: stratocumulus, patchy altocumulus, cirrus"), "8NhCLCMCH 81541: CL 5, CM 4, CH 1");
            ok(r.raw.rfind("88889 12782 61506", 0) == 0, "the raw groups are kept, as received");
        }
    }
    std::printf("── the 7-group, present and past weather ──\n");
    {
        auto v = readAll("AAXX 23064\n10384 11/96 82508 10118 76165=\n");
        ok(v.size() == 1 && has(v[0].line, "slight rain") && detail(v[0], "Weather before: rain, drizzle"),
           "76165: ww 61 slight rain (4677), W1 6 rain, W2 5 drizzle (4561)");
        auto c = readAll("AAXX 23064\n10384 11/96 82508 10118 70261=\n");
        ok(c.size() == 1 && detail(c[0], "Weather now: sky unchanged") && !has(c[0].line, "sky unchanged") && detail(c[0], "Weather before: rain, variable cloud"),
           "70261: ww 02 'sky unchanged' in the details, kept off the line (00-03 are cloud, not weather)");
        auto a = readAll("AAXX 23064\n10384 17/96 82508 10118 76165=\n");
        ok(a.size() == 1 && has(a[0].line, "slight rain") && detail(a[0], "Weather before: rain, drizzle"),
           "iX 7 (automatic codes): wawa 61 slight rain (4680), Wa 6 rain, 5 drizzle (4531)");
        auto x = readAll("AAXX 23064\n10384 17/96 82508 10118 71366=\n");
        ok(x.size() == 1 && x[0].unreadable == 1 && !detail(x[0], "Weather now"), "iX 7 with wawa 13 (a code 4680 reserves): unreadable, not guessed");
        auto n = readAll("AAXX 23064\n10384 12/96 82508 10118 76166=\n");
        ok(n.size() == 1 && n[0].unreadable == 1 && !detail(n[0], "Weather now"), "iX 2 says there is no 7-group: one that arrives anyway is unreadable");
    }
    std::printf("── a DWD-style bulletin: the station list, section 3 ──\n");
    {
        const std::string b = "ZCZC 123\r\nSMDL01 EDZW 230600\r\nAAXX 23064\r\n"
                              "10384 41/96 82508 10118 20072 30115 40206 57012 8807/ 333 10173 20065 91118=\r\n"
                              "10385 NIL=\r\n"
                              "99999 41/96 82508 10118=\r\nNNNN\r\n";
        auto v = readAll(b);
        ok(v.size() == 3, "three reports (" + std::to_string(v.size()) + ")");
        if (v.size() == 3) {
            std::printf("     %s\n     %s\n     %s\n", v[0].line.c_str(), v[1].line.c_str(), v[2].line.c_str());
            ok(has(v[0].line, "10384 \xc2\xb7 Tempelhof, Germany"), "10384 named from the ISD list: Tempelhof, Germany");
            ok(has(v[0].line, "06:00 UTC") && has(v[0].line, "wind 250\xc2\xb0 8 kt (15 km/h)") && has(v[0].line, "11.8 \xc2\xb0""C") && has(v[0].line, "1020.6 hPa"),
               "06:00 UTC · wind 250° 8 kt · 11.8 °C · 1020.6 hPa");
            ok(detail(v[0], "Pressure has fallen 1.2 hPa in 3 hours"), "57012: falling 1.2 hPa");
            ok(detail(v[0], "Highest temperature 17.3") && detail(v[0], "Lowest temperature 6.5"), "section 3: 10173 highest, 20065 lowest");
            ok(detail(v[0], "Highest gust 18 kt"), "section 3: 91118 = highest gust 18 kt (iw knots)");
            ok(detail(v[0], "Visibility 4 km") && !detail(v[0], "Lowest cloud"), "41/96: VV 96 = 4 km; h '/' = no cloud base claimed");
            ok(has(v[1].line, "10385") && has(v[1].line, "no report (NIL)"), "10385 NIL: no report");
            ok(v[2].line.rfind("99999 \xc2\xb7", 0) == 0 && !has(v[2].line, ","), "99999 is not in the list: the number only, no name");
        }
    }
    std::printf("── RTTY damage: that group, never the report ──\n");
    {
        auto v = readAll("AAXX 23064\n10384 41/96 82508 1O118 20072 3011 40206=\n");
        ok(v.size() == 1, "one report");
        if (v.size() == 1) {
            ok(v[0].unreadable == 2 && has(v[0].line, "2 groups unreadable"), "two damaged groups counted (" + std::to_string(v[0].unreadable) + ")");
            ok(!has(v[0].line, "\xc2\xb0""C") && has(v[0].line, "1020.6 hPa") && detail(v[0], "Dew point 7.2"),
               "no temperature from '1O118'; the readable groups around it still read");
            ok(detail(v[0], "Unreadable (RTTY damage): 1O118 3011"), "...and the damaged groups are listed as received");
        }
        auto u = readAll("AAXX 23064\n10384 41/96 82508 10118 2007_ 40206=\n");
        ok(u.size() == 1 && u[0].unreadable == 1 && !detail(u[0], "Dew point"), "a lost character ('_') makes its group unreadable");
        auto w = readAll("AAXX 23064\n10384 41/53 82508 10118=\n");
        ok(w.size() == 1 && w[0].unreadable == 1 && !detail(w[0], "Visibility") && !detail(w[0], "Manned"),
           "VV 53 (code table 4377 leaves 51-55 unused): the WHOLE iRiXhVV is unreadable");
        auto d = readAll("AAXX 23064\n10384 41/96 84508 10118=\n");
        ok(d.size() == 1 && !has(d[0].line, "wind") && !detail(d[0], "Cloud cover"), "dd 45 (over 36): no wind, and no cloud cover from the same group");
        auto t = readAll("AAXX 23064\n10384 41/96 82508 10718 40206=\n");
        ok(t.size() == 1 && t[0].unreadable == 1 && !has(t[0].line, "71.8"), "a 71.8 °C air temperature is impossible: unreadable");
        auto p = readAll("AAXX 23064\n10384 41/96 82508 10118 45206=\n");
        ok(p.size() == 1 && !has(p[0].line, "hPa"), "45206 is 4a3hhh (500 hPa height), not a sea-level pressure");
        auto o = readAll("AAXX 23064\n10384 41/96 82508 10118 40206 10173 20065=\n");
        ok(o.size() == 1 && o[0].unreadable == 2 && !detail(o[0], "Highest") && !detail(o[0], "Dew point"),
           "a lost '333': 10173 20065 after 40206 are OUT OF ORDER — unreadable, never read as section 1");
        auto s = readAll("AAXX 23064\n1O384 41/96 82508 10118=\n");
        ok(s.size() == 1 && has(s[0].line, "station unreadable") && has(s[0].line, "11.8"), "a damaged station number: 'station unreadable', the report still reads");
        auto h = readAll("AAXX 23964\n10384 41/96 82508 10118=\n");
        ok(h.size() == 1 && has(h[0].line, "time not received") && has(h[0].line, "8 (unit not received)"),
           "a damaged AAXX header (hour 96): no time and no wind unit — not guessed");
    }
    std::printf("── SHIP (FM 13) ──\n");
    {
        auto v = readAll("BBXX\r\nDBBH 23064 99543 10079 41/96 82508 10118 40206 22273 00125 20304 70015=\r\n"
                         "PBWQ 23061 99123 50456 41/97 80612 11012 49986=\r\nNNNN\r\n");
        ok(v.size() == 2, "two ship reports (" + std::to_string(v.size()) + ")");
        if (v.size() == 2) {
            std::printf("     %s\n     %s\n", v[0].line.c_str(), v[1].line.c_str());
            ok(v[0].kind == "ship" && v[0].line.rfind("DBBH", 0) == 0, "the ship's call sign leads");
            ok(has(v[0].line, "54.3\xc2\xb0N 7.9\xc2\xb0""E"), "99543 10079: Qc 1 (NE) → 54.3°N 7.9°E");
            ok(has(v[0].line, "06:00 UTC") && has(v[0].line, "wind 250\xc2\xb0 8 kt"), "its own YYGGiw: 06 UTC, knots");
            ok(detail(v[0], "Ship heading NW, 11\xe2\x80\x93""15 kt"), "222Dsvs 22273: heading NW at 11-15 kt");
            ok(detail(v[0], "Sea temperature 12.5") && detail(v[0], "Wind waves 2.0 m, every 3 s") && detail(v[0], "Waves (measured) 1.5 m"),
               "section 2: sea 12.5 °C, wind waves 2.0 m / 3 s, 70015 measured 1.5 m");
            ok(has(v[1].line, "12.3\xc2\xb0S 45.6\xc2\xb0W"), "Qc 5 (SW): 12.3°S 45.6°W");
            ok(has(v[1].line, "12 m/s (23 kt, 43 km/h)"), "iw 1: metres per second, with kt and km/h");
            ok(has(v[1].line, "-1.2 \xc2\xb0""C") && has(v[1].line, "998.6 hPa"), "11012 → -1.2 °C; 49986 → 998.6 hPa");
        }
        auto bad = readAll("BBXX\nDBBH 23064 99543 10079 41/96 82508 22200 00610=\n");
        ok(bad.size() == 1 && bad[0].unreadable == 1 && !detail(bad[0], "Sea temperature"), "a 61.0 °C sea: unreadable, not shown");
    }
    std::printf("── no AAXX heard (tuned in mid-bulletin) ──\n");
    {
        auto v = readAll("\r\n03772 12782 61506 10094 40197=\r\n");
        ok(v.size() == 1 && has(v[0].line, "London Heathrow") && has(v[0].line, "time not received") && has(v[0].line, "6 (unit not received)"),
           "a plain SYNOP line is read, but its time and wind unit are NOT known and it says so");
        ok(v.size() == 1 && !has(v[0].line, "Heathrow,"), "...and the ISD's mixed country code (ISO 'GB' = FIPS Gabon) is not believed: no country");
        auto n = readAll("AAXX 23064\n10384 41/96 82508=\nNNNN\n10385 41/96 82508 10118=\n");
        ok(n.size() == 2 && has(n[1].line, "time not received"), "a header never carries past its NNNN into the next bulletin");
        auto s = readAll("SYNOP 10384 41/96 82508 10118 20072=\r\n");
        ok(s.size() == 1 && has(s[0].line, "Tempelhof"), "\"SYNOP 10384 …\" (a plain-language lead-in): read");
        auto none = readAll("RYRYRYRYRYRY\r\nCQ CQ CQ DE DDK2 DDH7 DDK9\r\nFREQUENCIES   4583 KHZ   7646 KHZ   10100.8 KHZ\r\n"
                            "10100 4583 7646=\r\nTHE 12345 IS NOT A REPORT=\r\n12345 67890 11111=\r\n");
        ok(none.empty(), "DWD's call-up, RYRY, frequencies and stray numbers are NEVER a report (" + std::to_string(none.size()) + ")");
        auto unterminated = readAll("\n10384 41/96 82508 10118 20072\nRYRYRY\n");
        ok(unterminated.empty(), "a headless candidate with no '=' is not believed");
    }
    std::printf("── real text off the air: DDK2 4583 kHz, 2026-10-22 22:36 UTC (rtty_wav, 50 Bd / 450 Hz reversed) ──\n");
    {
        // ★ The decoder's output for the whole clip, verbatim — the AAXX was before the recording started.
        const std::string clip = "XX ZPQRT OY RPPYU WWPM\n0015//;\n62170149514 120 46/97  1015 20125 4005 222\n006 10 70008 90710 9112\n";
        auto v = readAll(clip);
        // ★ The first line "XX ZPQRT OY RPPYU WWPM" is figures through a lost FIGS ("RPPYU" = 40067): shift damage, not text.
        ok(v.empty(), "as heard (no AAXX, a lost space in the first group, no '='): NOTHING is claimed (" + std::to_string(v.size()) + ")");
        // ★ The same text as though the bulletin's "AAXX 22221" had been heard: what CAN be read is, the rest is not.
        auto w = readAll("AAXX 22221\n" + clip + "=\n");
        ok(w.size() == 1, "with its bulletin header: one report");
        if (w.size() == 1) {
            std::printf("     %s\n", w[0].line.c_str());
            for (auto& d : w[0].details) std::printf("       %s\n", d.c_str());
            ok(has(w[0].line, "station unreadable") && has(w[0].line, "22:00 UTC"), "the merged '62170149514' is no station; the time is the header's");
            ok(!detail(w[0], "Visibility") && !detail(w[0], "Cloud cover"),
               "'120' sits in the iRiXhVV slot (damaged), so '46/97' falls in the Nddff slot and does not fit it: unreadable — "
               "it LOOKS like an iRiXhVV, but a slot is never re-guessed");
            ok(detail(w[0], "Dew point 12.5"), "20125 identifies itself (indicator 2): dew point 12.5 °C");
            ok(!has(w[0].line, "wind") && !has(w[0].line, "hPa"), "'1015' and '4005' (a lost figure each) give no wind and no pressure");
            ok(detail(w[0], "Waves (measured) 0.8 m"), "section 2's 70008: waves 0.8 m");
            ok(!detail(w[0], "station") && w[0].values == 2, "two things read, and only two (" + std::to_string(w[0].values) + ")");
            ok(w[0].unreadable >= 5, "the damaged groups are counted (" + std::to_string(w[0].unreadable) + ")");
        }
    }
    std::printf("── the stream: where a report is released ──\n");
    {
        Reader rd;
        const std::string a = "AAXX 23064\r\n10384 41/96 82508 10118=", b = "\r\n", c = "10385 NIL=", d = " 10386";
        size_t n1 = rd.push(a).size();
        ok(n1 == 0 && rd.held() == 1, "after '=' the report is HELD until its line ends");
        auto r2 = rd.push(b);
        ok(r2.size() == 1 && r2[0].at == 2, "...released at the newline, AFTER it (offset " + std::to_string(r2.empty() ? 0 : r2[0].at) + " of 2)");
        rd.push(c);
        auto r4 = rd.push(d);
        ok(r4.size() == 1 && r4[0].at == 1, "no newline: released just BEFORE the next printing character (offset " + std::to_string(r4.empty() ? 0 : r4[0].at) + ")");
        auto f = rd.flush();
        ok(f.empty(), "a lone station number at the end is not a report");
        // ';' is DWD's '=' through this server's ITA2 figures table.
        auto s = readAll("AAXX 23064\n10384 41/96 82508 10118;\n");
        ok(s.size() == 1 && has(s[0].line, "11.8"), "';' ends a report (DWD's '=' prints as ';' in our ITA2 figures)");
        auto ff = readAll("AAXX 23064\n10384 41/96 82508 10118 20072 30115 40206", 3);
        ok(ff.size() == 1 && has(ff[0].line, "1020.6 hPa"), "the decoder stopping mid-report: flush reads what arrived");
        // Wrapped over lines, fed in odd chunks.
        auto wr = readAll("AAXX 23064\r\n10384 41/96 82508\r\n 10118 20072\r\n 40206=\r\n", 7);
        ok(wr.size() == 1 && has(wr[0].line, "1020.6 hPa") && has(wr[0].line, "11.8"), "a report wrapped over three lines, in 7-byte chunks");
    }
    std::printf("── bounded (a Pi 2) ──\n");
    {
        Reader rd;
        std::mt19937 g(7); const char al[] = "0123456789/ \r\n=;AXZBN";
        size_t got = 0;
        std::string s;
        for (int i = 0; i < 400000; ++i) s += al[g() % (sizeof al - 1)];
        for (size_t o = 0; o < s.size(); o += 97) got += rd.push(s.substr(o, 97)).size();
        ok(rd.held() <= Reader::kMaxHeld, "400 KB of noise: never more than kMaxHeld reports waiting (" + std::to_string(rd.held()) + ")");
        std::printf("     (%zu 'reports' released from noise — each judged on its own groups)\n", got);
        Reader r2;
        std::string longest = "AAXX 23064\n";
        for (int i = 0; i < 5000; ++i) longest += "11111 ";
        auto x = r2.push(longest + "=\n");
        ok(x.empty(), "a 5000-group run is not a report (kMaxTokens), and is not held");
        std::string huge(100000, '7');
        r2.push(huge);
        ok(r2.held() == 0, "a 100 KB token is cut at kMaxTok, nothing kept");
    }
    std::printf("── the frame body ──\n");
    {
        Report r; r.kind = "synop"; r.raw = "10384 \"x\\"; r.line = "a\nb"; r.details = { "d1", "d\"2" }; r.unreadable = 1;
        const std::string j = r.json();
        ok(j == "{\"type\":\"weather_report\",\"kind\":\"synop\",\"raw\":\"10384 \\\"x\\\\\",\"line\":\"a\\u000ab\",\"unreadable\":1,\"details\":[\"d1\",\"d\\\"2\"]}",
           "JSON: quotes, backslashes and control characters escaped (" + j + ")");
    }
    std::printf("── %d passed, %d failed ──\n", passes, fails);
    return fails ? 1 : 0;
}
