// ★★★ DOES THE TIME DECODER RECOVER A KNOWN TIME FROM A SYNTHESISED MINUTE?
//
// A 1-bit-per-second signal cannot be tested by ear or by eye at the bench: a whole minute is 59
// bits, so "it looks like it is working" means nothing until a parity-checked timestamp comes out
// the other end. This builds a minute of MSF and of DCF77 from a KNOWN time, plays it through the
// real decoder as audio, and requires exactly that time back.
//
// ★★ NOISE IS PART OF THE TEST, not an afterthought. The decoder's whole threshold design is
//    adaptive because a real LF signal fades; a test on a clean square wave would pass with a
//    fixed threshold and tell us nothing about the receiver it has to survive. Measured on the
//    demo's Airspy: MSF sits ~19 dB above a neighbouring empty channel, DCF77 ~23 dB, so the
//    noise here is set to leave a comparable margin.
//
// ★ Deliberately also feeds a minute of pure noise and requires NO timestamp: a decoder that
//   invents a plausible time from nothing is far worse than one that stays quiet.
#include "decoders/time_decoder.h"
#include <ctime>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <vector>

using vibe::TimeDecoder;

static int fails = 0;
static void ok(bool c, const char* w) {
    std::printf("  %s %s\n", c ? "\033[32mok\033[0m  " : "\033[31mFAIL\033[0m", w);
    if (!c) fails++;
}

static const int SR = 48000;
static unsigned seed = 12345;
static double noise() {           // deterministic, so a failure is reproducible
    seed = seed * 1103515245u + 12345u;
    return ((double)((seed >> 16) & 0x7fff) / 16384.0 - 1.0);
}

// ★★★ A CW TONE, NOT A DC LEVEL — because that is how these are actually received.
//
// Stuart, 2026-08-11: "I use CW for MSF/DCF77." That is the right mode and it settles a design
// question this test was originally getting wrong. In CW the carrier is mixed against the BFO, so
// it arrives as an AUDIO TONE whose amplitude follows the carrier — the code becomes tone
// amplitude, and the audio is AC, so whether the demodulator blocks DC stops mattering at all.
// An AM path would have handed us a near-DC envelope that a DC-blocking demodulator would have
// flattened into silence between edges.
//
// ★★ So the model here is a tone: the decoder's rectify-and-smooth envelope follower has to pull
//    the amplitude back off it, which is the thing that actually has to work on air. Testing
//    against a DC level would have exercised none of that.
static double phase = 0;
static double toneHz = 800.0;
static void emit(std::vector<int16_t>& out, double ms, double level, double noiseAmp = 0.05) {
    const int n = (int)(ms * SR / 1000.0);
    const double w = 2.0 * M_PI * toneHz / SR;     // CW beat note, or WWV's 100 Hz subcarrier
    for (int i = 0; i < n; i++) {
        phase += w;
        const double v = level * 0.60 * std::sin(phase) + noise() * noiseAmp;
        out.push_back((int16_t)std::lround(std::fmax(-1.0, std::fmin(1.0, v)) * 32767));
    }
}

// ── DCF77: carrier to ~15 % for 100 ms (bit 0) or 200 ms (bit 1); second 59 has no dip ──────────
static void dcf77Minute(std::vector<int16_t>& out, const int bits[59]) {
    for (int s = 0; s < 59; s++) {
        const double dip = bits[s] ? 200.0 : 100.0;
        emit(out, dip, 0.15);
        emit(out, 1000.0 - dip, 1.0);
    }
    emit(out, 1000.0, 1.0);                 // second 59: no dip at all — this IS the minute mark
}

// ════════════════════════════════════════════════════════════════════════════════════════════
// ★★★ GENERATORS WITH DAMAGE, AND A RUNNER THAT KNOWS WHEN EACH ANNOUNCEMENT HAPPENED
//     (2026-10-04, audit rows 6-9). Every case below asks two questions of each announcement:
//     is it the RIGHT minute for the moment it was made, and did a damaged minute ever produce
//     a WRONG one. "A timestamp came out" is not enough — the bugs this pins all produced one.
// ════════════════════════════════════════════════════════════════════════════════════════════
struct Hhmm { int y, mo, d, h, mi; };
static bool leapY(int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }
static int dim(int y, int m) {
    static const int l[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return (m == 2 && leapY(y)) ? 29 : l[m - 1];
}
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
static int doyOf(const Hhmm& t) { int n = t.d; for (int m = 1; m < t.mo; m++) n += dim(t.y, m); return n; }
static bool same(const TimeDecoder::TimeStamp& a, const Hhmm& b) {
    return a.year == b.y && a.month == b.mo && a.day == b.d && a.hour == b.h && a.minute == b.mi;
}
struct Heard { double atS; TimeDecoder::TimeStamp t; };
/** Feeds 100 ms at a time so each announcement carries the moment it was made. */
static std::vector<Heard> runTimed(TimeDecoder& d, const std::vector<int16_t>& a) {
    std::vector<Heard> out;
    size_t fed = 0;
    d.onTime = [&](const TimeDecoder::TimeStamp& t) { out.push_back({ (double)fed / SR, t }); };
    const size_t step = SR / 10;
    for (size_t i = 0; i < a.size(); i += step) {
        const size_t n = std::min(step, a.size() - i);
        fed = i + n;
        d.process(a.data() + i, (int)n);
    }
    return out;
}

// One second with an optional deletion (the dip faded) or a spurious extra dip at +550 ms.
static void lfSecond(std::vector<int16_t>& out, double dipMs, double lowLevel, bool drop,
                     bool extra, double extraMs) {
    if (drop) { emit(out, 1000.0, 1.0); return; }
    emit(out, dipMs, lowLevel);
    if (!extra) { emit(out, 1000.0 - dipMs, 1.0); return; }
    emit(out, 550.0 - dipMs, 1.0);
    emit(out, extraMs, lowLevel);
    emit(out, 1000.0 - 550.0 - extraMs, 1.0);
}

// ── DCF77 (PTB): bits describe the minute that BEGINS at the next minute mark ──
static void dcfBitsFor(const Hhmm& t, int b[59]) {
    for (int i = 0; i < 59; i++) b[i] = 0;
    b[20] = 1; b[17] = 1; b[18] = 0;
    auto put = [&](int from, int to, int val) {
        static const int w[] = { 1, 2, 4, 8, 10, 20, 40, 80 };
        int rem = val;
        for (int i = to, k = to - from; i >= from; i--, k--) if (rem >= w[k]) { b[i] = 1; rem -= w[k]; }
    };
    int wd = 0; { // weekday, ISO 1..7 (Zeller-free: count from 2026-01-01, a Thursday)
        long days = 0; for (int y = 2026; y < t.y; y++) days += leapY(y) ? 366 : 365;
        days += doyOf(t) - 1; wd = (int)((3 + days) % 7) + 1; }
    put(21, 27, t.mi); put(29, 34, t.h); put(36, 41, t.d); put(42, 44, wd); put(45, 49, t.mo);
    put(50, 57, t.y % 100);
    auto evenPar = [&](int from, int to, int pbit) {
        int n = 0; for (int i = from; i <= to; i++) n += b[i]; b[pbit] = (n & 1);
    };
    evenPar(21, 27, 28); evenPar(29, 34, 35); evenPar(36, 57, 58);
}
static void dcfMinuteDamaged(std::vector<int16_t>& out, const Hhmm& t, int drop, int extra) {
    int b[59]; dcfBitsFor(t, b);
    for (int s = 0; s < 59; s++) lfSecond(out, b[s] ? 200.0 : 100.0, 0.15, s == drop, s == extra, 100.0);
    emit(out, 1000.0, 1.0);
}

// ── MSF (NPL): A and B windows; the frame describes the minute beginning at the NEXT marker ──
static void msfMinuteDamaged(std::vector<int16_t>& out, const Hhmm& t, int drop, int extra) {
    int A[60] = {0}, B[60] = {0};
    auto putA = [&](int from, int to, int val) {
        static const int w10[] = { 80, 40, 20, 10, 8, 4, 2, 1 };
        const int n = to - from + 1; int rem = val;
        for (int i = 0; i < n; i++) { const int wt = w10[8 - n + i]; if (rem >= wt) { A[from + i] = 1; rem -= wt; } }
    };
    putA(17, 24, t.y % 100); putA(25, 29, t.mo); putA(30, 35, t.d); putA(36, 38, 2);
    putA(39, 44, t.h); putA(45, 51, t.mi);
    auto oddPar = [&](int from, int to, int pbit) {
        int n = 0; for (int i = from; i <= to; i++) n += A[i]; B[pbit] = (n & 1) ? 0 : 1;
    };
    oddPar(17, 24, 54); oddPar(25, 35, 55); oddPar(36, 38, 56); oddPar(39, 51, 57);
    B[58] = 1;
    emit(out, 500.0, 0.0); emit(out, 500.0, 1.0);
    for (int sec = 1; sec <= 59; sec++) {
        if (sec == drop) { emit(out, 1000.0, 1.0); continue; }   // the whole second faded
        emit(out, 100.0, 0.0);
        emit(out, 100.0, A[sec] ? 0.0 : 1.0);
        emit(out, 100.0, B[sec] ? 0.0 : 1.0);
        if (sec == extra) { emit(out, 250.0, 1.0); emit(out, 100.0, 0.0); emit(out, 350.0, 1.0); }
        else emit(out, 700.0, 1.0);
    }
}

// ── WWVB (SP 432 Table 2.3): the frame describes the minute that BEGAN at its own second 0 ──
static void wwvbMinuteDamaged(std::vector<int16_t>& out, const Hhmm& t, int drop, int extra) {
    int sym[60] = {0};
    for (int p2 : { 0, 9, 19, 29, 39, 49, 59 }) sym[p2] = 2;
    auto put = [&](const int* bits, const int* wts, int n, int val) {
        int rem = val; for (int i = 0; i < n; i++) if (rem >= wts[i]) { sym[bits[i]] = 1; rem -= wts[i]; }
    };
    { const int bi[] = {1,2,3,5,6,7,8};     const int wt[] = {40,20,10,8,4,2,1};  put(bi,wt,7,t.mi); }
    { const int bi[] = {12,13,15,16,17,18}; const int wt[] = {20,10,8,4,2,1};     put(bi,wt,6,t.h); }
    { const int bi[] = {22,23,25,26,27,28,30,31,32,33};
      const int wt[] = {200,100,80,40,20,10,8,4,2,1};                              put(bi,wt,10,doyOf(t)); }
    { const int bi[] = {45,46,47,48,50,51,52,53};
      const int wt[] = {80,40,20,10,8,4,2,1};                                      put(bi,wt,8,t.y % 100); }
    sym[55] = leapY(t.y) ? 1 : 0;
    for (int sec = 0; sec < 60; sec++) {
        const double dip = sym[sec] == 2 ? 800.0 : sym[sec] == 1 ? 500.0 : 200.0;
        lfSecond(out, dip, 0.15, sec == drop, sec == extra, 200.0);
    }
}

// ── WWV (SP 432 Table 3.13): 100 Hz subcarrier, pulse = symbol, frame = minute at its START ──
static void wwvMinuteSpec(std::vector<int16_t>& out, const Hhmm& t, int yy, bool dst, bool lsw,
                          int corruptSec = -1) {
    int sym[60] = {0};
    for (int p : { 9, 19, 29, 39, 49, 59 }) sym[p] = 2;
    auto put = [&](const int* bits, const int* wts, int n, int val) {
        int rem = val; for (int i = n - 1; i >= 0; i--) if (rem >= wts[i]) { sym[bits[i]] = 1; rem -= wts[i]; }
    };
    { const int bi[] = {10,11,12,13,15,16,17}; const int wt[] = {1,2,4,8,10,20,40}; put(bi,wt,7,t.mi); }
    { const int bi[] = {20,21,22,23,25,26};    const int wt[] = {1,2,4,8,10,20};    put(bi,wt,6,t.h); }
    { const int bi[] = {30,31,32,33,35,36,37,38,40,41};
      const int wt[] = {1,2,4,8,10,20,40,80,100,200};                              put(bi,wt,10,doyOf(t)); }
    // ★★ The year is SPLIT (SP 432 p. 48): units at 4-7, tens at 51-54, each LSB first.
    { const int bi[] = {4,5,6,7};     const int wt[] = {1,2,4,8}; put(bi,wt,4,yy % 10); }
    { const int bi[] = {51,52,53,54}; const int wt[] = {1,2,4,8}; put(bi,wt,4,yy / 10); }
    if (dst) { sym[2] = 1; sym[55] = 1; }
    if (lsw) sym[3] = 1;
    for (int sec = 0; sec < 60; sec++) {
        if (sec == 0) { emit(out, 1000.0, 0.0); continue; }
        const double pulse = sec == corruptSec ? 270.0                 // between a 0 and a 1: unreadable
                           : sym[sec] == 2 ? 770.0 : sym[sec] == 1 ? 470.0 : 170.0;
        emit(out, 30.0, 0.0); emit(out, pulse, 1.0); emit(out, 1000.0 - 30.0 - pulse, 0.0);
    }
}

/** ★★★ THE DAMAGE CASE, the same for every LF station: F0 good, F1 good, F2 DAMAGED, F3, F4 good.
 *  Every announcement must equal the minute right for the moment it is made — so a damaged
 *  minute may yield its own correct time or nothing, never a wrong one — and F4 must be
 *  announced, which needs F3 read cleanly: resynchronised by the very next minute. */
enum class Damage { Drop, Extra, ExtraEveryMinute };
static void damageCase(const char* name, TimeDecoder::Station st, Hhmm f0, Damage dmg, int damageSec,
                       void (*gen)(std::vector<int16_t>&, const Hhmm&, int, int),
                       int announceOffsetMin, int firstAnnounced) {
    std::vector<int16_t> a;
    const double preS = 3.0;
    emit(a, preS * 1000.0, 1.0);
    for (int k = 0; k < 5; k++) {
        const bool hit = (k == 2) || dmg == Damage::ExtraEveryMinute;
        gen(a, plus(f0, k), hit && dmg == Damage::Drop ? damageSec : -1,
                            hit && dmg != Damage::Drop ? damageSec : -1);
    }
    emit(a, 1500.0, 1.0);
    TimeDecoder d(SR, st);
    const auto heard = runTimed(d, a);
    int wrong = 0; bool gotF4 = false;
    for (const auto& h : heard) {
        const int k = (int)std::floor((h.atS - preS) / 60.0);
        const Hhmm want = plus(f0, k + announceOffsetMin);
        if (!same(h.t, want)) {
            wrong++;
            std::printf("    WRONG at %.1f s: %04d-%02d-%02d %02d:%02d, want %02d:%02d\n", h.atS,
                        h.t.year, h.t.month, h.t.day, h.t.hour, h.t.minute, want.h, want.mi);
        }
        if (k == 4 && same(h.t, want)) gotF4 = true;
    }
    char msg[200];
    const char* what = dmg == Damage::Drop  ? "one dip DELETED in minute 3"
                     : dmg == Damage::Extra ? "one dip ADDED in minute 3"
                                            : "the SAME noise dip in every minute";
    std::snprintf(msg, sizeof msg, "★★★ %s, %s: no WRONG time (%zu announced)", name, what, heard.size());
    ok(wrong == 0, msg);
    if (dmg == Damage::Drop) {
        std::snprintf(msg, sizeof msg, "%s, %s: back in sync by the next minute (minute 5 announced)", name, what);
        ok(gotF4, msg);
    } else {
        // ★★ A dip BETWEEN the seconds is noise, and noise must cost NOTHING: every minute from
        //    the first that can be corroborated is announced. A counter took it as a second and
        //    lost the minute — and the next, because corroboration then had nothing to follow.
        const int want = 5 - firstAnnounced;
        std::snprintf(msg, sizeof msg, "★★ %s, %s: costs nothing — all %d minutes announced", name, what, want);
        ok(wrong == 0 && (int)heard.size() == want, msg);
    }
}

int main() {
    std::printf("time-signal decoder — a known minute, as a CW beat note\n");

    // ── DCF77: 14:32 then 14:33 on Tuesday 11 August 2026, CEST ─────────────
    // ★★ TWO CONSECUTIVE MINUTES, because the decoder requires a reading to be corroborated by
    //    the one before it — one minute of parity-checked noise is a 1-in-16 event on MSF, and
    //    the first live run against Anthorn produced exactly that. A fixture that sent the SAME
    //    minute twice was testing something no transmitter ever does.
    auto dcfBits = [](int hh, int mm, int b[59]) {
        for (int i = 0; i < 59; i++) b[i] = 0;
        b[20] = 1;                                   // start of encoded time
        b[17] = 1; b[18] = 0;                        // CEST
        auto put = [&](int from, int to, int val) {
            static const int w[] = { 1, 2, 4, 8, 10, 20, 40, 80 };
            int rem = val;
            for (int i = to, k = to - from; i >= from; i--, k--) {
                if (rem >= w[k]) { b[i] = 1; rem -= w[k]; }
            }
        };
        put(21, 27, mm); put(29, 34, hh);
        put(36, 41, 11); put(42, 44, 2); put(45, 49, 8); put(50, 57, 26);
        auto evenPar = [&](int from, int to, int pbit) {
            int n = 0; for (int i = from; i <= to; i++) n += b[i];
            b[pbit] = (n & 1);
        };
        evenPar(21, 27, 28); evenPar(29, 34, 35); evenPar(36, 57, 58);
    };

    int b1[59], b2[59];
    dcfBits(14, 32, b1);
    dcfBits(14, 33, b2);

    std::vector<int16_t> audio;
    emit(audio, 3000.0, 1.0);                    // a little carrier first, to settle the AGC
    dcf77Minute(audio, b1);                      // hunts the marker, then reads 14:32
    dcf77Minute(audio, b2);                      // 14:33 — corroborates, and is what is announced

    TimeDecoder dec(SR, TimeDecoder::Station::DCF77);
    TimeDecoder::TimeStamp got{}; bool fired = false;
    dec.onTime = [&](const TimeDecoder::TimeStamp& t) { got = t; fired = true; };
    dec.process(audio.data(), (int)audio.size());

    std::printf("    SNR seen by the decoder: %.1f dB\n", dec.snrDb());
    ok(fired, "★★★ DCF77: a corroborated timestamp came out");
    if (fired) {
        std::printf("    decoded: %04d-%02d-%02d %02d:%02d (weekday %d, dst %d)\n",
                    got.year, got.month, got.day, got.hour, got.minute, got.weekday, (int)got.dst);
        ok(got.year == 2026 && got.month == 8 && got.day == 11, "DCF77: date is 2026-08-11");
        ok(got.hour == 14 && got.minute == 33, "DCF77: the SECOND minute is the one announced");
        ok(got.weekday == 2, "DCF77: weekday is Tuesday");
        ok(got.dst, "DCF77: CEST flag read");
    }
    ok(dec.minutesGood() >= 1, "DCF77: at least one minute passed parity AND corroboration");

    // ── MSF: 07:45 on Tuesday 11 August 2026 ────────────────────────────────
    // ★★★ THE POINT OF THIS CASE IS THE PARITY BITS, which MSF carries as A=0,B=1 — off, on, off,
    //     i.e. TWO dips inside one second. A decoder that treats every dip as a new second
    //     mis-frames the rest of the minute, and it fails on exactly the bits meant to validate
    //     it. The first version of this decoder did precisely that.
    {
        std::vector<int16_t> a2;
        emit(a2, 3000.0, 1.0);
        for (int MINUTE = 45; MINUTE <= 46; MINUTE++) {
        int A[60] = {0}, B[60] = {0};
        // ★★★ MSB FIRST, from NPL's published table: 17A is 80, 18A is 40 … 24A is 1.
        //     The first version of this test built the bits LSB-first — the SAME wrong assumption
        //     the decoder had — so the pair agreed with each other and disagreed with Anthorn.
        //     A test written from the same misunderstanding as the code proves only that they
        //     match. It took a live decode of "2064-02-22" to expose it.
        auto putA = [&](int from, int to, int val) {
            static const int w10[] = { 80, 40, 20, 10, 8, 4, 2, 1 };
            const int n = to - from + 1;
            int rem = val;
            for (int i = 0; i < n; i++) {
                const int wt = w10[8 - n + i];
                if (rem >= wt) { A[from + i] = 1; rem -= wt; }
            }
        };
        putA(17, 24, 26);      // year 2026
        putA(25, 29, 8);       // August
        putA(30, 35, 11);      // 11th
        putA(36, 38, 2);       // Tuesday
        putA(39, 44, 7);       // 07
        putA(45, 51, MINUTE);  // :45 then :46
        auto oddPar = [&](int from, int to, int pbit) {
            int n = 0; for (int i = from; i <= to; i++) n += A[i];
            B[pbit] = (n & 1) ? 0 : 1;            // ODD parity over the data
        };
        oddPar(17, 24, 54);
        oddPar(25, 35, 55);
        oddPar(36, 38, 56);
        oddPar(39, 51, 57);
        B[58] = 1;                                 // summer time in force

        {
            emit(a2, 500.0, 0.0);  emit(a2, 500.0, 1.0);        // second 0: the 500 ms minute mark
            for (int sec = 1; sec <= 59; sec++) {
                // [0,100) always off; [100,200) off if A; [200,300) off if B.
                emit(a2, 100.0, 0.0);
                emit(a2, 100.0, A[sec] ? 0.0 : 1.0);
                emit(a2, 100.0, B[sec] ? 0.0 : 1.0);
                emit(a2, 700.0, 1.0);
            }
        }
        }
        TimeDecoder m(SR, TimeDecoder::Station::MSF);
        TimeDecoder::TimeStamp mt{}; bool mf = false;
        m.onTime = [&](const TimeDecoder::TimeStamp& t) { mt = t; mf = true; };
        m.process(a2.data(), (int)a2.size());
        ok(mf, "★★★ MSF: a timestamp came out (the A=0,B=1 parity bits framed correctly)");
        if (mf) {
            std::printf("    decoded: %04d-%02d-%02d %02d:%02d (weekday %d, dst %d)\n",
                        mt.year, mt.month, mt.day, mt.hour, mt.minute, mt.weekday, (int)mt.dst);
            ok(mt.year == 2026 && mt.month == 8 && mt.day == 11, "MSF: date is 2026-08-11");
            ok(mt.hour == 7 && mt.minute == 46, "MSF: the SECOND minute is announced (07:46)");
            ok(mt.dst, "MSF: summer-time flag read");
        }
    }

    // ── WWV: 09:07 UTC on day 223 ───────────────────────────────────────────
    // ★★★ WWV HAS NO PARITY WHATSOEVER, so nothing in the frame validates it. That makes the
    //     bit map worth pinning hard: a single wrong weight here produces a confident wrong clock
    //     and there is no check inside the standard to catch it. It also makes the corroboration
    //     rule load-bearing rather than belt-and-braces.
    // ★★ The polarity is INVERTED relative to MSF/DCF77: each second begins with ~30 ms of NO
    //    subcarrier and the symbol is the PULSE that follows — 170 ms = 0, 470 ms = 1, 770 ms = a
    //    position marker. And the code rides a 100 Hz SUBCARRIER, so the tone here is 100 Hz.
    // ★★★ THIS GENERATOR USED TO ENCODE THE SAME MISUNDERSTANDING THE DECODER DID, so the two
    //     agreed and the test proved only that. Corrected against a LIVE signal on 2026-08-12
    //     (WT8P, Sammamish WA, 15 MHz): SECOND 0 IS A HOLE, not a marker, and the time starts in
    //     the SECOND decade (IRIG-H). ★★ And on 2026-10-04 against NIST SP 432 Table 3.13 itself:
    //     the YEAR is split — units at 4-7, tens at 51-54 — and seconds 2/55 (DST) and 3 (leap-
    //     second warning) carry flags. The old generator wrote an 8-bit year at 51-58, which no
    //     transmitter sends; it was another copy of a wrong model.
    {
        const time_t nowT = time(nullptr);
        const struct tm* utcNow = gmtime(&nowT);
        const int hostYear = utcNow ? utcNow->tm_year + 1900 : 2026;
        // ★★ The AIR's year is the host's MINUS ONE — inside the decoder's ±1 cross-check, but
        //    different, so a decoder still reading the host clock fails here. It also keeps the
        //    test true in any year, unlike a literal 2026.
        const int airYear = hostYear - 1;
        const Hhmm t0 { airYear, 8, 11, 9, 7 };        // day-of-year from the calendar of airYear
        std::vector<int16_t> w;
        toneHz = 100.0;
        emit(w, 2000.0, 0.0);
        // ★ FOUR minutes: the first is spent hunting the marker-then-hole, the second is read, the
        //   third corroborates — and the FOURTH's first pulse is what closes the third.
        for (int k = 0; k < 4; k++) wwvMinuteSpec(w, plus(t0, k), airYear % 100, true, true);
        toneHz = 800.0;

        TimeDecoder d(SR, TimeDecoder::Station::WWV);
        std::vector<TimeDecoder::Partial> partials;
        d.onPartial = [&](const TimeDecoder::Partial& p) { partials.push_back(p); };
        const auto heard = runTimed(d, w);
        ok(!heard.empty(), "★★★ WWV: a corroborated timestamp came out");
        if (!heard.empty()) {
            const auto& t = heard.back().t;
            std::printf("    decoded: %04d-%02d-%02d %02d:%02d UTC dst %d leap %d, at %.2f s\n",
                        t.year, t.month, t.day, t.hour, t.minute, (int)t.dst,
                        (int)t.leapSecondPending, heard.back().atS);
            // ★★★ THE MINUTE IN PROGRESS, NOT THE ONE JUST ENDED (audit row 8). The frame carries
            //     the time at its own START (SP 432 p. 46) and is closed by the next minute's first
            //     pulse, 1.03 s into it — so the right answer is the frame's value PLUS ONE.
            const int k = (int)std::floor((heard.back().atS - 2.0) / 60.0);   // the frame in progress
            ok(same(t, plus(t0, k)), "★★★ WWV: the announced time is the TRUE current minute (09:10)");
            ok(t.year == airYear, "★★ WWV: the year is read from the air (seconds 4-7 + 51-54)");
            ok(t.dst, "WWV: DST read from the air (seconds 2 and 55)");
            ok(t.leapSecondPending, "WWV: leap-second warning read from the air (second 3)");
        }
        // ★★★ THE PROGRESS LINE MUST READ THE SAME MAP AS THE DECODE (audit row 6).
        bool sawFull = false, badField = false;
        for (const auto& p : partials) {
            if (p.hour && p.t.hour != 9) badField = true;
            if (p.minute && (p.t.minute < 7 || p.t.minute > 10)) badField = true;
            if (p.year && p.t.year != airYear) badField = true;
            if (p.second >= 54 && p.minute && p.hour && p.year && p.month && p.day
                && p.t.minute == 8 && p.t.month == 8 && p.t.day == 11) sawFull = true;
        }
        ok(!badField, "★★★ WWV: the progress line's minute/hour/year are the decode's fields");
        ok(sawFull, "WWV: the progress line fills minute, hour, then year and date (09:08, 11 Aug)");
    }

    // ── WWV: an unreadable year falls back to the host; a contradicting one is refused ──
    {
        const time_t nowT = time(nullptr);
        const struct tm* utcNow = gmtime(&nowT);
        const int hostYear = utcNow ? utcNow->tm_year + 1900 : 2026;
        auto runWwv = [&](int yy, int corrupt) {
            std::vector<int16_t> w;
            toneHz = 100.0;
            emit(w, 2000.0, 0.0);
            const Hhmm t0 { hostYear, 3, 3, 12, 20 };
            for (int k = 0; k < 4; k++) wwvMinuteSpec(w, plus(t0, k), yy, false, false, corrupt);
            toneHz = 800.0;
            TimeDecoder d(SR, TimeDecoder::Station::WWV);
            return runTimed(d, w);
        };
        // A year second that cannot be read (a 320 ms pulse at second 5) -> the host's year.
        const auto h1 = runWwv((hostYear - 1) % 100, 5);
        ok(!h1.empty() && h1.back().t.year == hostYear,
           "WWV: a year second unreadable -> the host's year, and only then");
        ok(!h1.empty() && !h1.back().t.dst && !h1.back().t.leapSecondPending,
           "WWV: DST and leap warning read as OFF when the air says off");
        // ★★ A clean air year five years from the host's: one of them is wrong, and with no
        //    parity we cannot say which — so no time at all, rather than either guess.
        const auto h2 = runWwv((hostYear - 5) % 100, -1);
        ok(h2.empty(), "★★ WWV: an air year contradicting the host by >1 is refused, not announced");
    }

    // ── WWVB: 14:32 UTC on day 223 of 2026 (= 11 August) ────────────────────
    // ★★★ MSB FIRST, THE OPPOSITE OF WWV. Seconds 1–8 carry 40,20,10,(unused),8,4,2,1. The two
    //     stations share a broadcaster and almost nothing else, and neither carries parity — so a
    //     map taken from the wrong one yields a plausible wrong clock with nothing to catch it.
    //     This test exists mainly to pin the ORDER.
    {
        auto wwvbMinute = [&](std::vector<int16_t>& out, int hh, int mm, int doy, int yy, bool leap) {
            int sym[60];
            for (int i = 0; i < 60; i++) sym[i] = 0;
            for (int p2 : { 0, 9, 19, 29, 39, 49, 59 }) sym[p2] = 2;      // frame ref + P1..P6
            auto put = [&](const int* bits, const int* wts, int n, int val) {
                int rem = val;
                for (int i = 0; i < n; i++) if (rem >= wts[i]) { sym[bits[i]] = 1; rem -= wts[i]; }
            };
            { const int bi[] = {1,2,3,5,6,7,8};   const int wt[] = {40,20,10,8,4,2,1};   put(bi,wt,7,mm); }
            { const int bi[] = {12,13,15,16,17,18}; const int wt[] = {20,10,8,4,2,1};    put(bi,wt,6,hh); }
            { const int bi[] = {22,23,25,26,27,28,30,31,32,33};
              const int wt[] = {200,100,80,40,20,10,8,4,2,1};                            put(bi,wt,10,doy); }
            { const int bi[] = {45,46,47,48,50,51,52,53};
              const int wt[] = {80,40,20,10,8,4,2,1};                                    put(bi,wt,8,yy); }
            sym[55] = leap ? 1 : 0;                       // leap-year indicator
            for (int sec = 0; sec < 60; sec++) {
                const double dip = sym[sec] == 2 ? 800.0 : sym[sec] == 1 ? 500.0 : 200.0;
                emit(out, dip, 0.15);                     // carrier attenuated
                emit(out, 1000.0 - dip, 1.0);             // full carrier
            }
        };
        std::vector<int16_t> v;
        emit(v, 3000.0, 1.0);
        wwvbMinute(v, 14, 32, 223, 26, false);   // spent finding the 59/0 double marker
        wwvbMinute(v, 14, 33, 223, 26, false);   // read
        wwvbMinute(v, 14, 34, 223, 26, false);   // corroborates

        TimeDecoder d(SR, TimeDecoder::Station::WWVB);
        TimeDecoder::TimeStamp t{}; bool got = false;
        d.onTime = [&](const TimeDecoder::TimeStamp& x) { t = x; got = true; };
        d.process(v.data(), (int)v.size());
        ok(got, "★★★ WWVB: a corroborated timestamp came out");
        if (got) {
            std::printf("    decoded: %04d-%02d-%02d %02d:%02d UTC\n",
                        t.year, t.month, t.day, t.hour, t.minute);
            // ★★★ 14:35, NOT 14:34 (audit row 8). The 14:34 frame describes the minute that began
            //     at its own second 0 (SP 432 p. 21: the on-time point is Pr's leading edge); it is
            //     decoded at second 59, under a second before 14:35 begins — which is what is due,
            //     exactly as MSF and DCF77 announce the minute about to begin.
            ok(t.hour == 14 && t.minute == 35, "★★★ WWVB: the minute about to begin is announced (14:35)");
            ok(t.year == 2026, "WWVB: year 2026");
            ok(t.month == 8 && t.day == 11, "★★ WWVB: day-of-year 223 converted to 11 August");
        }
    }

    // ── WWVB: the +1 minute carries across hour, day, month, year and 29 February ──
    // ★★ Each frame is announced one minute on, so the carry is in the decoder's hands now:
    //    23:59 on the last day of a month is where a lazy +1 says "24:00" or "32 December".
    {
        auto carry = [&](Hhmm f0, Hhmm want, const char* what) {
            std::vector<int16_t> v;
            emit(v, 3000.0, 1.0);
            for (int k = 0; k < 3; k++) wwvbMinuteDamaged(v, plus(f0, k), -1, -1);
            TimeDecoder d(SR, TimeDecoder::Station::WWVB);
            const auto h = runTimed(d, v);
            if (!h.empty()) std::printf("    %s: %04d-%02d-%02d %02d:%02d\n", what, h.back().t.year,
                                        h.back().t.month, h.back().t.day, h.back().t.hour, h.back().t.minute);
            ok(!h.empty() && same(h.back().t, want), what);
        };
        carry({2026, 12, 31, 23, 57}, {2027, 1, 1, 0, 0}, "★★ WWVB: 31 Dec 23:59 frame -> 2027-01-01 00:00");
        carry({2028, 2, 28, 23, 57}, {2028, 2, 29, 0, 0}, "★★ WWVB: leap year, 28 Feb 23:59 -> 29 Feb 00:00");
        // ★★ AND THE CORROBORATION KEY MUST COUNT REAL MINUTES: it packed every month as 31 days,
        //    so 30 April 23:59 -> 1 May 00:00 looked 1441 minutes apart and nothing was announced.
        carry({2026, 4, 30, 23, 57}, {2026, 5, 1, 0, 0}, "★★ WWVB: 30 Apr 23:59 -> 1 May 00:00 (short month)");
    }

    // ── ONE DIP DELETED, ONE DIP ADDED — MSF, DCF77, WWVB (audit row 9) ─────
    // ★★★ A SECOND COUNTER SHIFTS FOR THE REST OF THE MINUTE. These stations used to frame by
    //     counting dips, so one fade or one noise spike moved every later bit by a second, and
    //     DCF77 took a single faded second's 1.9 s gap as the minute mark. A minute read one
    //     place out is still a plausible minute — which is the failure that matters.
    damageCase("MSF",   TimeDecoder::Station::MSF,   {2026, 8, 11, 7, 45},  Damage::Drop,  30, msfMinuteDamaged,  0, 1);
    damageCase("MSF",   TimeDecoder::Station::MSF,   {2026, 8, 11, 7, 45},  Damage::Extra, 30, msfMinuteDamaged,  0, 1);
    damageCase("DCF77", TimeDecoder::Station::DCF77, {2026, 8, 11, 14, 30}, Damage::Drop,  25, dcfMinuteDamaged,  0, 1);
    damageCase("DCF77", TimeDecoder::Station::DCF77, {2026, 8, 11, 14, 30}, Damage::Extra, 30, dcfMinuteDamaged,  0, 1);
    damageCase("WWVB",  TimeDecoder::Station::WWVB,  {2026, 8, 11, 14, 30}, Damage::Drop,  25, wwvbMinuteDamaged, 1, 2);
    damageCase("WWVB",  TimeDecoder::Station::WWVB,  {2026, 8, 11, 14, 30}, Damage::Extra, 30, wwvbMinuteDamaged, 1, 2);
    // ★★★ THE INTERFERER THAT CLICKS AT THE SAME POINT EVERY MINUTE (a timer, a thermostat, a
    //     neighbour's appliance on a cycle). A counter shifts the SAME way each minute, so the two
    //     wrong minutes agree with each other and corroboration — the only check WWVB has — waves
    //     them through. Measured on the old decoder: announced, corroborated, "2013-04-21 02:31" then
    //     "02:32" for a true 14:33/14:35.
    damageCase("WWVB",  TimeDecoder::Station::WWVB,  {2026, 8, 11, 14, 30}, Damage::ExtraEveryMinute, 3, wwvbMinuteDamaged, 1, 2);
    damageCase("MSF",   TimeDecoder::Station::MSF,   {2026, 8, 11, 7, 45},  Damage::ExtraEveryMinute, 44, msfMinuteDamaged, 0, 1);
    damageCase("DCF77", TimeDecoder::Station::DCF77, {2026, 8, 11, 14, 30}, Damage::ExtraEveryMinute, 30, dcfMinuteDamaged, 0, 1);

    // ── RWM carries no timecode, and must never pretend otherwise ───────────
    {
        std::vector<int16_t> r;
        emit(r, 2000.0, 1.0);
        for (int sec = 0; sec < 120; sec++) { emit(r, 100.0, 0.0); emit(r, 900.0, 1.0); }
        TimeDecoder d(SR, TimeDecoder::Station::RWM);
        bool got = false;
        d.onTime = [&](const TimeDecoder::TimeStamp&) { got = true; };
        d.process(r.data(), (int)r.size());
        ok(!got, "★★★ RWM: never emits a timestamp — it carries no timecode to decode");
        ok(!d.carriesTimeCode(), "RWM: declares that it has no timecode, so the UI can say so");
        ok(d.secondNow() >= 0, "RWM: but it IS counting second markers (that is the useful part)");
    }

    // ── RWM's Morse callsign ────────────────────────────────────────────────
    // ★★ THE ONLY THING RWM SENDS THAT PROVES IT IS BEING HEARD. Without it the panel can say
    //    only "counting markers", which is indistinguishable from counting noise.
    // ★ Keyed at 20 wpm (60 ms dot). R = .-. , W = .-- , M = --
    {
        std::vector<int16_t> m;
        const double U = 60.0;                         // one dot
        auto key = [&](const char* code) {
            for (const char* c = code; *c; c++) {
                emit(m, *c == '-' ? U * 3 : U, 1.0);   // mark: carrier keyed ON
                emit(m, U, 0.0);                       // gap between elements
            }
            emit(m, U * 2, 0.0);                       // rest of the letter gap (3 units total)
        };
        emit(m, 2000.0, 0.0);
        for (int rep = 0; rep < 3; rep++) {            // "RWM RWM RWM"
            key(".-."); key(".--"); key("--");
            emit(m, U * 4, 0.0);                       // word gap
        }
        TimeDecoder d(SR, TimeDecoder::Station::RWM);
        std::string heard;
        d.onMorse = [&](char c) { heard += c; };
        d.process(m.data(), (int)m.size());
        std::printf("    heard: \"%s\"\n", heard.c_str());
        // ★ The FIRST character is often mangled and that is expected, not a defect: the envelope
        //   thresholds are still adapting when it arrives, so the leading element can be missed
        //   ("NWMRWMRW" — N for R, then clean). The assertion is that the callsign APPEARS, not
        //   that every repetition is perfect; demanding the latter would be demanding the decoder
        //   be right before it has heard anything.
        ok(heard.find("RWM") != std::string::npos,
           "★★★ RWM: the Morse callsign is decoded off the envelope");
    }

    // ── Pure noise must produce NOTHING ─────────────────────────────────────
    {
        std::vector<int16_t> junk;
        for (int i = 0; i < SR * 90; i++) junk.push_back((int16_t)std::lround(noise() * 3000));
        TimeDecoder d2(SR, TimeDecoder::Station::DCF77);
        bool any = false;
        d2.onTime = [&](const TimeDecoder::TimeStamp&) { any = true; };
        d2.process(junk.data(), (int)junk.size());
        ok(!any, "★★★ 90 s of pure noise yields NO timestamp — it never invents a time");
    }

    std::printf(fails ? "\n\033[31m%d failed\033[0m\n" : "\n\033[32mpassed\033[0m\n", fails);
    return fails ? 1 : 0;
}
