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

#include <cmath>
#include <cstdio>
#include <cstdlib>
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
        auto putA = [&](int from, int to, int val) {
            static const int w[] = { 1, 2, 4, 8, 10, 20, 40, 80 };
            int rem = val;
            for (int i = to, k = to - from; i >= from; i--, k--) {
                if (rem >= w[k]) { A[i] = 1; rem -= w[k]; }
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

    // ── WWV: 09:07 UTC on day 223 of 2026 (= 11 August) ─────────────────────
    // ★★★ WWV HAS NO PARITY WHATSOEVER, so nothing in the frame validates it. That makes the
    //     bit map worth pinning hard: a single wrong weight here produces a confident wrong clock
    //     and there is no check inside the standard to catch it. It also makes the corroboration
    //     rule load-bearing rather than belt-and-braces.
    // ★★ The polarity is INVERTED relative to MSF/DCF77: each second begins with ~30 ms of NO
    //    subcarrier and the symbol is the PULSE that follows — 170 ms = 0, 470 ms = 1, 770 ms = a
    //    position marker. And the code rides a 100 Hz SUBCARRIER, so the tone here is 100 Hz.
    {
        auto wwvMinute = [&](std::vector<int16_t>& out, int hh, int mm, int doy, int yy) {
            int sym[60];
            for (int i = 0; i < 60; i++) sym[i] = 0;
            sym[0] = 2;                                    // frame reference
            for (int p : { 9, 19, 29, 39, 49, 59 }) sym[p] = 2;   // P1..P6
            auto put = [&](const int* bits, const int* wts, int n, int val) {
                int rem = val;
                for (int i = n - 1; i >= 0; i--) if (rem >= wts[i]) { sym[bits[i]] = 1; rem -= wts[i]; }
            };
            { const int bi[] = {1,2,3,4,6,7,8};      const int wt[] = {1,2,4,8,10,20,40};       put(bi,wt,7,mm); }
            { const int bi[] = {10,11,12,13,15,16};  const int wt[] = {1,2,4,8,10,20};          put(bi,wt,6,hh); }
            { const int bi[] = {20,21,22,23,25,26,27,28,30,31};
              const int wt[] = {1,2,4,8,10,20,40,80,100,200};                                  put(bi,wt,10,doy); }
            { const int bi[] = {50,51,52,53,55,56,57,58};
              const int wt[] = {1,2,4,8,10,20,40,80};                                          put(bi,wt,8,yy); }
            for (int sec = 0; sec < 60; sec++) {
                emit(out, 30.0, 0.0);                                     // the second tick
                emit(out, sym[sec] == 2 ? 770.0 : sym[sec] == 1 ? 470.0 : 170.0, 1.0);
                emit(out, 1000.0 - 30.0 - (sym[sec] == 2 ? 770.0 : sym[sec] == 1 ? 470.0 : 170.0), 0.0);
            }
        };
        std::vector<int16_t> w;
        toneHz = 100.0;                              // the 100 Hz subcarrier
        emit(w, 2000.0, 0.0);
        // ★ THREE minutes, and that is not padding. WWV's only alignment feature is the DOUBLE
        //   marker spanning seconds 59 and 0, so it cannot lock until it has seen a minute
        //   BOUNDARY — the first minute is spent hunting. Then one minute to read and one more to
        //   corroborate. Two minutes looks like enough and is not.
        wwvMinute(w, 9, 7, 223, 26);                 // spent finding the 59/0 boundary
        wwvMinute(w, 9, 8, 223, 26);                 // read
        wwvMinute(w, 9, 9, 223, 26);                 // corroborates
        toneHz = 800.0;

        TimeDecoder d(SR, TimeDecoder::Station::WWV);
        TimeDecoder::TimeStamp t{}; bool got = false;
        d.onTime = [&](const TimeDecoder::TimeStamp& x) { t = x; got = true; };
        d.process(w.data(), (int)w.size());
        ok(got, "★★★ WWV: a corroborated timestamp came out");
        if (got) {
            std::printf("    decoded: %04d-%02d-%02d %02d:%02d UTC\n",
                        t.year, t.month, t.day, t.hour, t.minute);
            ok(t.hour == 9 && t.minute == 9, "WWV: the corroborated minute is announced (09:09)");
            ok(t.year == 2026, "WWV: year 2026");
            ok(t.month == 8 && t.day == 11, "★★ WWV: day-of-year 223 converted to 11 August");
        }
    }

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
