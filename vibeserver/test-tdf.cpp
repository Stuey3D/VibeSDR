// test-tdf.cpp — V12 2026-11-16 TDF / ALS162 (Allouis 162 kHz) through the time decoder (decoders/tdf_phase.h +
// time_decoder.cpp). docs/v12/TIME-TDF.md.
//
// ★ Every signal here is SYNTHETIC, built from the published format: a constant carrier, phase-modulated per second
//   by one element (0) or two (1) of +1 rad / 25 ms, −2 rad / 50 ms, +1 rad / 25 ms, the first element's mid-point
//   zero crossing at the top of the second, nothing at second 59; DCF77's time code in bits 15–58, bits 1–2 the
//   leap-second warnings. Heard as a CW/USB beat note.
//   1. Clean: two minutes → the second is announced, right; the tone found wherever it is (500 / 1000 / 2200 Hz).
//   2. Noise: an SNR sweep in the 2.5 kHz audio bandwidth (the threshold printed, the clean cases must pass).
//   3. Drift: the beat note moving 3 Hz over ten minutes (a receiver warming up) — still read.
//   4. Extra phase activity outside the reader's window (a stand-in for ALS162's "non-timing data", whose format we
//      do not have) at ±0.3 rad — still read.
//   5. Never a wrong time: ten minutes of noise; a plain DCF77 AM signal given to TDF; TDF given to DCF77; a minute with
//      one bit flipped after the parity — nothing announced from them.
//   6. The leap-second warning (bit 1) is carried; bit 19 is not read as one.
//   7. The timing: the minute is announced kDipAfterSecondMs later on TDF than the same minute on DCF77's AM — the
//      constant the host takes off the clock it offers (time_sync).
//   8. CPU: an hour of audio, timed.
#include "decoders/time_decoder.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <string>

using vibe::TimeDecoder;
using vibe::TdfPhase;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static const int SR = 48000;
struct Hhmm { int y, mo, d, h, mi; };
static bool leapY(int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }
static int dim(int y, int m) { static const int l[12] = {31,28,31,30,31,30,31,31,30,31,30,31}; return (m == 2 && leapY(y)) ? 29 : l[m - 1]; }
static Hhmm plus(Hhmm t, int minutes) {
    for (int i = 0; i < minutes; i++) {
        if (++t.mi < 60) continue;
        t.mi = 0; if (++t.h < 24) continue;
        t.h = 0; if (++t.d <= dim(t.y, t.mo)) continue;
        t.d = 1; if (++t.mo <= 12) continue;
        t.mo = 1; t.y++;
    }
    return t;
}
static int isoWd(const Hhmm& t) {   // 2026-01-01 was a Thursday
    long days = 0;
    for (int y = 2026; y < t.y; y++) days += leapY(y) ? 366 : 365;
    for (int m = 1; m < t.mo; m++) days += dim(t.y, m);
    days += t.d - 1;
    return int(((3 + days) % 7 + 7) % 7) + 1;
}
/** The DCF77 / TDF bits (seconds 0–58) that describe the minute `t` — sent in the minute BEFORE it. */
static void bitsFor(const Hhmm& t, int b[59], bool leapWarn = false) {
    for (int i = 0; i < 59; i++) b[i] = 0;
    b[20] = 1; b[17] = 0; b[18] = 1;   // CET (winter)
    if (leapWarn) b[1] = 1;
    auto put = [&](int from, int to, int val) {
        static const int w[] = { 1, 2, 4, 8, 10, 20, 40, 80 };
        int rem = val;
        for (int i = to, k = to - from; i >= from; i--, k--) if (rem >= w[k]) { b[i] = 1; rem -= w[k]; }
    };
    put(21, 27, t.mi); put(29, 34, t.h); put(36, 41, t.d); put(42, 44, isoWd(t)); put(45, 49, t.mo); put(50, 57, t.y % 100);
    auto even = [&](int from, int to, int p) { int n = 0; for (int i = from; i <= to; i++) n += b[i]; b[p] = n & 1; };
    even(21, 27, 28); even(29, 34, 35); even(36, 57, 58);
}

struct Gen {
    double f0 = 1000, ph = 0, amp = 8000, noise = 0, drift = 0;   // drift: Hz per second
    double extra = 0;                                               // ±rad of extra phase activity at 400–900 ms
    unsigned seed = 1;
    double t = 0;
    double rnd() { seed = seed * 1103515245u + 12345u; return ((seed >> 8) & 0xFFFF) / 65536.0; }
    double gauss() { double s = 0; for (int i = 0; i < 12; i++) s += rnd(); return s - 6.0; }
};
/** The phase of one element at `u` seconds from its start (0 … 0.1). */
static double element(double u) {
    if (u < 0 || u >= 0.1) return 0;
    if (u < 0.025) return u / 0.025;
    if (u < 0.075) return 1.0 - 2.0 * (u - 0.025) / 0.050;
    return -1.0 + (u - 0.075) / 0.025;
}
/** One minute of TDF: second s's element(s) centred on the top of the second (the first starts 50 ms before). */
static void tdfMinute(Gen& g, std::vector<int16_t>& out, const int bits[59], int flip = -1) {
    for (int s = 0; s < 60; s++) {
        const int bit = s < 59 ? (s == flip ? 1 - bits[s] : bits[s]) : -1;
        for (int k = 0; k < SR; k++) {
            const double u = k / double(SR);   // 0 = top of second s
            double phm = 0;
            if (bit >= 0) { phm += element(u + 0.050); if (bit == 1) phm += element(u - 0.050); }
            // the next second's first element begins 50 ms before its top (second 59 → second 0 of the next minute)
            if (s != 58) phm += element(u - 0.950);   // ★ second 59 carries nothing, so nothing leads into it
            if (g.extra > 0 && u > 0.40 && u < 0.90) phm += g.extra * std::sin(2 * M_PI * 7.0 * u + s);
            g.ph += 2 * M_PI * (g.f0 + g.drift * g.t) / SR;
            if (g.ph > 2 * M_PI) g.ph -= 2 * M_PI;
            double x = g.amp * std::sin(g.ph + phm) + g.noise * g.gauss();
            x = std::max(-32767.0, std::min(32767.0, x));
            out.push_back(int16_t(x));
            g.t += 1.0 / SR;
        }
    }
}
/** One minute of DCF77's AM (carrier to 15 % for 100 / 200 ms at the top of each second; none at 59) — for (7). */
static void dcfMinuteAm(Gen& g, std::vector<int16_t>& out, const int bits[59]) {
    for (int s = 0; s < 60; s++) {
        const double dip = s < 59 ? (bits[s] ? 0.200 : 0.100) : 0;
        for (int k = 0; k < SR; k++) {
            const double u = k / double(SR);
            g.ph += 2 * M_PI * g.f0 / SR; if (g.ph > 2 * M_PI) g.ph -= 2 * M_PI;
            const double a = u < dip ? 0.15 : 1.0;
            out.push_back(int16_t(std::max(-32767.0, std::min(32767.0, g.amp * a * std::sin(g.ph) + g.noise * g.gauss()))));
        }
    }
}
struct Heard { double atS; TimeDecoder::TimeStamp t; };
static std::vector<Heard> run(TimeDecoder& d, const std::vector<int16_t>& a) {
    std::vector<Heard> out;
    size_t fed = 0;
    d.onTime = [&](const TimeDecoder::TimeStamp& t) { out.push_back({ double(fed) / SR, t }); };
    const size_t step = SR / 200;   // 5 ms
    for (size_t i = 0; i < a.size(); i += step) { const size_t n = std::min(step, a.size() - i); fed = i + n; d.process(a.data() + i, int(n)); }
    return out;
}
/** `n` minutes starting with the one that DESCRIBES `first` (so the first audio minute is the one before it). */
static std::vector<int16_t> tdfAudio(Gen& g, Hhmm first, int n, int flipMinute = -1, int flipSec = -1, bool leap = false) {
    std::vector<int16_t> a; a.reserve(size_t(n) * 60 * SR);
    // ★ start mid-minute (second 30 of the minute before), as a listener tunes in
    int b[59]; bitsFor(first, b, leap);   // (only its last half is heard: no complete minute)
    { Gen h = g; std::vector<int16_t> pre; tdfMinute(h, pre, b); a.insert(a.end(), pre.begin() + 30 * SR, pre.end()); g = h; }
    for (int m = 0; m < n; m++) { bitsFor(plus(first, m), b, leap); tdfMinute(g, a, b, m == flipMinute ? flipSec : -1); }
    return a;
}
static bool same(const TimeDecoder::TimeStamp& a, const Hhmm& b) { return a.year == b.y && a.month == b.mo && a.day == b.d && a.hour == b.h && a.minute == b.mi; }
static bool allRight(const std::vector<Heard>& h, const Hhmm& first) {
    for (const auto& x : h) {
        // the minute announced is the one whose bits ended at that boundary: find it among first..first+20
        bool okOne = false;
        for (int m = 0; m < 20 && !okOne; m++) okOne = same(x.t, plus(first, m));
        if (!okOne) return false;
    }
    return true;
}

static void testClean() {
    std::printf("── clean: the tone found, two minutes, the time right ──\n");
    for (const double f0 : { 500.0, 1000.0, 2200.0 }) {
        Gen g; g.f0 = f0; g.noise = 50;
        const Hhmm t0 { 2026, 11, 16, 14, 31 };
        const auto a = tdfAudio(g, t0, 4);
        TimeDecoder d(SR, TimeDecoder::Station::TDF);
        const auto h = run(d, a);
        CHECK(!h.empty() && allRight(h, t0), "%.0f Hz: %zu minute(s) announced, all right", f0, h.size());
        CHECK(std::string(d.stationTag()) == "TDF", "the tag says TDF");
        if (!h.empty()) std::printf("   %.0f Hz: first %04d-%02d-%02d %02d:%02d at %.2f s, %zu announced, carrier %.0f dB\n", f0,
                                    h[0].t.year, h[0].t.month, h[0].t.day, h[0].t.hour, h[0].t.minute, h[0].atS, h.size(), d.snrDb());
        CHECK(h.size() >= 2, "at least two minutes announced in four (%zu)", h.size());
        if (!h.empty()) CHECK(!h[0].t.dst, "CET (bit 18) = not summer time");
    }
}

static int minutesAt(double snrDb, unsigned seed, double drift = 0, double extra = 0, int n = 6) {
    Gen g; g.seed = seed; g.drift = drift; g.extra = extra;
    // SNR in a 2.5 kHz audio bandwidth: noise power over 24 kHz × (2.5/24)
    const double sPow = g.amp * g.amp / 2;
    g.noise = std::sqrt(sPow / std::pow(10, snrDb / 10) * (24000.0 / 2500.0));
    const Hhmm t0 { 2026, 11, 16, 9, 2 };
    const auto a = tdfAudio(g, t0, n);
    TimeDecoder d(SR, TimeDecoder::Station::TDF);
    const auto h = run(d, a);
    return allRight(h, t0) ? int(h.size()) : -1;
}
static void testNoise() {
    std::printf("── noise: SNR in 2.5 kHz ──\n");
    int at[6]; const double snr[6] = { 20, 10, 5, 0, -3, -6 };
    for (int i = 0; i < 6; i++) { at[i] = minutesAt(snr[i], 7u + unsigned(i)); std::printf("   %+5.0f dB: %d minute(s) of 6 announced%s\n", snr[i], at[i], at[i] < 0 ? " — A WRONG TIME" : ""); }
    for (int i = 0; i < 6; i++) CHECK(at[i] >= 0, "never a wrong time at %+.0f dB", snr[i]);
    CHECK(at[0] >= 4 && at[1] >= 4 && at[2] >= 3, "20 / 10 / 5 dB read");
}
static void testDrift() {
    std::printf("── drift and other phase activity ──\n");
    const int n = minutesAt(15, 3u, 3.0 / 600.0, 0, 10);
    CHECK(n >= 7, "the beat note moving 3 Hz in 10 min: %d of 10", n);
    const int m = minutesAt(15, 4u, 0, 0.3, 6);
    CHECK(m >= 4, "±0.3 rad of other phase activity at 400–900 ms: %d of 6", m);
    const int big = minutesAt(15, 5u, 0, 1.0, 6);
    std::printf("   ±1 rad of other activity at 400–900 ms: %d of 6 (not required; recorded for the doc)\n", big);
    CHECK(big >= 0, "never a wrong time with ±1 rad of other activity");
}
static void testNever() {
    std::printf("── never a wrong time ──\n");
    {   Gen g; g.noise = 3000; std::vector<int16_t> a;
        for (long i = 0; i < 600L * SR; i++) a.push_back(int16_t(std::max(-32767.0, std::min(32767.0, g.noise * g.gauss()))));
        TimeDecoder d(SR, TimeDecoder::Station::TDF);
        CHECK(run(d, a).empty(), "ten minutes of noise: nothing"); }
    {   Gen g; g.noise = 50; std::vector<int16_t> a; const Hhmm t0 { 2026, 11, 16, 9, 2 };
        int b[59]; for (int m = 0; m < 4; m++) { bitsFor(plus(t0, m), b); dcfMinuteAm(g, a, b); }
        TimeDecoder d(SR, TimeDecoder::Station::TDF);
        CHECK(run(d, a).empty(), "DCF77's AM given to TDF: nothing (the phase is flat)");
        Gen h; h.noise = 50; const auto t = tdfAudio(h, t0, 4);
        TimeDecoder e(SR, TimeDecoder::Station::DCF77);
        CHECK(run(e, t).empty(), "TDF given to DCF77: nothing (the envelope is flat)"); }
    {   Gen g; g.noise = 50; const Hhmm t0 { 2026, 11, 16, 9, 2 };
        const auto a = tdfAudio(g, t0, 3, 1, 24);   // minute 2 has its minute-units bit 1 flipped after the parity
        TimeDecoder d(SR, TimeDecoder::Station::TDF);
        const auto h = run(d, a);
        CHECK(allRight(h, t0), "a flipped bit after the parity: no wrong time (%zu announced)", h.size()); }
}
static void testLeap() {
    std::printf("── the leap-second warning ──\n");
    Gen g; g.noise = 50; const Hhmm t0 { 2026, 12, 31, 23, 30 };
    const auto a = tdfAudio(g, t0, 3, -1, -1, true);
    TimeDecoder d(SR, TimeDecoder::Station::TDF);
    const auto h = run(d, a);
    CHECK(!h.empty() && h.back().t.leapSecondPending, "bit 1 → leap second pending");
    Gen g2; g2.noise = 50; const auto b = tdfAudio(g2, t0, 3);
    TimeDecoder e(SR, TimeDecoder::Station::TDF);
    const auto h2 = run(e, b);
    CHECK(!h2.empty() && !h2.back().t.leapSecondPending, "no warning without it");
}
static void testTiming() {
    std::printf("── the timing against DCF77's AM ──\n");
    const Hhmm t0 { 2026, 11, 16, 9, 2 };
    Gen g; g.noise = 50; std::vector<int16_t> dcf; int b[59];
    for (int m = 0; m < 4; m++) { bitsFor(plus(t0, m), b); dcfMinuteAm(g, dcf, b); }
    Gen h; h.noise = 50; std::vector<int16_t> tdf;
    for (int m = 0; m < 4; m++) { bitsFor(plus(t0, m), b); tdfMinute(h, tdf, b); }
    TimeDecoder dd(SR, TimeDecoder::Station::DCF77), dt(SR, TimeDecoder::Station::TDF);
    const auto hd = run(dd, dcf), ht = run(dt, tdf);
    bool found = false;
    for (const auto& x : ht) for (const auto& y : hd) if (same(x.t, Hhmm{ y.t.year, y.t.month, y.t.day, y.t.hour, y.t.minute })) {
        if (found) continue;
        found = true;
        const double lagMs = (x.atS - y.atS) * 1000.0;
        std::printf("   the same minute: DCF77 at %.3f s, TDF at %.3f s — TDF %+.0f ms (kDipAfterSecondMs %.0f)\n", y.atS, x.atS, lagMs, TdfPhase::kDipAfterSecondMs);
        CHECK(std::fabs(lagMs - TdfPhase::kDipAfterSecondMs) <= 15.0, "TDF's lag matches the constant the host uses (%+.0f ms)", lagMs);
    }
    CHECK(found, "both announced a common minute (DCF77 %zu, TDF %zu)", hd.size(), ht.size());
}
static void testCpu() {
    std::printf("── CPU ──\n");
    Gen g; g.noise = 2000; const Hhmm t0 { 2026, 11, 16, 9, 2 };
    const auto a = tdfAudio(g, t0, 10);
    TimeDecoder d(SR, TimeDecoder::Station::TDF);
    const auto c0 = std::chrono::steady_clock::now();
    for (int rep = 0; rep < 6; rep++) for (size_t i = 0; i < a.size(); i += 960) d.process(a.data() + i, int(std::min<size_t>(960, a.size() - i)));
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
    std::printf("   an hour of 48 kHz audio in %.2f s (%.2f %% of one core)\n", s, s / 3600.0 * 100);
    CHECK(s < 36.0, "under 1 %% of a core on this Mac");
}

/** ★ Replay a recording (tools/ubersdr-rec: 16-bit mono WAV, USB with the dial 1 kHz below 162 kHz). Prints what the
 *  decoder says; with `startUtc` (the recorder's start_utc=) it also checks each minute against the recording's clock. */
static int replayWav(const char* path, double startUtc) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("cannot open %s\n", path); return 3; }
    std::vector<uint8_t> raw; uint8_t buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) raw.insert(raw.end(), buf, buf + n);
    std::fclose(f);
    if (raw.size() < 44) return 3;
    const int rate = int(raw[24] | raw[25] << 8 | raw[26] << 16 | raw[27] << 24);
    size_t at = 12; size_t data = 0, len = 0;
    while (at + 8 <= raw.size()) {
        const uint32_t sz = raw[at + 4] | raw[at + 5] << 8 | raw[at + 6] << 16 | uint32_t(raw[at + 7]) << 24;
        if (!std::memcmp(&raw[at], "data", 4)) { data = at + 8; len = std::min<size_t>(sz, raw.size() - data); break; }
        at += 8 + sz;
    }
    std::vector<int16_t> a(len / 2);
    std::memcpy(a.data(), &raw[data], a.size() * 2);
    std::printf("replay %s: %.0f s at %d Hz\n", path, a.size() / double(rate), rate);
    TimeDecoder d(rate, TimeDecoder::Station::TDF);
    size_t fed = 0; int good = 0, wrong = 0;
    d.onState = [&](TimeDecoder::State s) { std::printf("   %6.1f s  state %d  carrier %.1f dB\n", fed / double(rate), int(s), d.snrDb()); };
    d.onPartial = [&](const TimeDecoder::Partial& p) { if (p.second == 58) std::printf("   %6.1f s  partial %02d:%02d %02d-%02d\n", fed / double(rate), p.t.hour, p.t.minute, p.t.day, p.t.month); };
    d.onTime = [&](const TimeDecoder::TimeStamp& t) {
        std::printf("   %6.1f s  TDF %04d-%02d-%02d %02d:%02d%s\n", fed / double(rate), t.year, t.month, t.day, t.hour, t.minute, t.dst ? " CEST" : " CET");
        if (startUtc > 0) {
            const double nowUtc = startUtc + fed / double(rate);
            const long long m = (long long)std::floor((nowUtc + 30) / 60.0) * 60;   // the minute boundary nearest
            const long long local = m + (t.dst ? 7200 : 3600);
            const int hh = int((local / 3600) % 24), mm = int((local / 60) % 60);
            if (hh == t.hour && mm == t.minute) ++good; else { ++wrong; std::printf("      ✗ expected %02d:%02d\n", hh, mm); }
        }
    };
    const size_t step = size_t(rate) / 100;
    for (size_t i = 0; i < a.size(); i += step) { const size_t k = std::min(step, a.size() - i); fed = i + k; d.process(a.data() + i, int(k)); }
    std::printf("   minutes good %lu failed %lu voted %lu · announced %d right, %d wrong · tone found: %s\n", d.minutesGood(), d.minutesFailed(), d.minutesVoted(), good, wrong, d.snrDb() > 0 ? "yes" : "no");
    return wrong ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--wav") return replayWav(argv[2], argc >= 4 ? std::atof(argv[3]) : 0);
    testClean();
    testNoise();
    testDrift();
    testNever();
    testLeap();
    testTiming();
    testCpu();
    std::printf("\ntest-tdf: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
