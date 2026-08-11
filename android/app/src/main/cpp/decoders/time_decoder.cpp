#include "time_decoder.h"

#include <cmath>
#include <cstdlib>

namespace vibe {
namespace {

// ── Timing, in milliseconds ──────────────────────────────────────────────────
// ★ Generous windows. These are 1 bit/second signals received on a wire aerial in a house full of
//   switch-mode noise; a decoder that insists on ±10 ms will decode nothing real. The patterns are
//   100 ms apart, so ±35 ms is still unambiguous.
constexpr double kTol         = 35.0;
constexpr double kMsfMinute   = 500.0;   ///< MSF: carrier off 500 ms marks second 0
constexpr double kDipUnit     = 100.0;   ///< both stations' base dip
constexpr double kMsfSampleA  = 150.0;   ///< MSF bit A is the carrier state 100–200 ms in
constexpr double kMsfSampleB  = 250.0;   ///< MSF bit B, 200–300 ms in
constexpr double kSecond      = 1000.0;

inline bool near(double v, double target, double tol = kTol) { return std::fabs(v - target) <= tol; }

/** BCD out of a bit range, LSB first — which is how both stations send it. */
int bcd(const int* bits, int from, int to) {
    static const int w[] = { 1, 2, 4, 8, 10, 20, 40, 80 };
    int v = 0;
    for (int i = from, k = 0; i <= to && k < 8; i++, k++) if (bits[i]) v += w[k];
    return v;
}

int parityOdd(const int* bits, int from, int to) {
    int n = 0;
    for (int i = from; i <= to; i++) n += bits[i] ? 1 : 0;
    return n & 1;
}

}  // namespace

TimeDecoder::TimeDecoder(int sampleRate, Station station)
    : sr_(sampleRate > 0 ? sampleRate : 48000), station_(station) {}

void TimeDecoder::setState(State s) {
    if (s == state_) return;
    state_ = s;
    if (onState) onState(s);
}

void TimeDecoder::process(const int16_t* samples, int count) {
    if (!samples || count <= 0) return;
    // Time constants: fast enough to see a 100 ms dip, slow enough to ignore audio-band noise.
    const double aFast = 1.0 - std::exp(-1.0 / (0.010 * sr_));   // 10 ms
    const double aSlow = 1.0 - std::exp(-1.0 / (5.000 * sr_));   //  5 s

    for (int i = 0; i < count; i++) {
        const double x = std::fabs((double)samples[i]);
        envFast_ += aFast * (x - envFast_);
        envSlow_ += aSlow * (x - envSlow_);

        // ★★ ADAPTIVE LEVELS, NOT A FIXED THRESHOLD. The carrier's absolute level depends on the
        //    aerial, the gain and the hour of the day — LF propagation alone moves it enormously
        //    between noon and midnight. What is stable is the RATIO between "carrier present" and
        //    "carrier reduced", so the decoder tracks both and puts the threshold between them.
        //    A fixed number would work on the bench and fail on every real receiver.
        // ★★★ THE RELEASE MUST BE SLOWER THAN A SECOND, and this is where the first attempt
        //     failed: a release of ~1 s let `offLevel_` climb back towards the carrier during the
        //     800–900 ms it is ON in every second, so the two levels converged and the measured
        //     SNR collapsed to ~1 dB on a signal that was actually 20 dB clean. The "off" level
        //     has to remember the QUIETEST it has been over many seconds, not over one.
        // ★ Attack fast (catch the real extreme within a dip), release very slowly (~20 s), which
        //   is still quick enough to follow LF fading over a night.
        const double kAttack = 0.002, kRelease = 0.000001;
        if (envFast_ > onLevel_)  onLevel_  += kAttack  * (envFast_ - onLevel_);
        else                      onLevel_  += kRelease * (envFast_ - onLevel_);
        if (offLevel_ == 0.0)     offLevel_  = envFast_;
        else if (envFast_ < offLevel_) offLevel_ += kAttack  * (envFast_ - offLevel_);
        else                           offLevel_ += kRelease * (envFast_ - offLevel_);

        const double denom = offLevel_ > 1.0 ? offLevel_ : 1.0;
        snrDb_ = 20.0 * std::log10((onLevel_ > denom ? onLevel_ : denom) / denom);

        // ★ Hysteresis, or a noisy envelope crossing the threshold chatters and every dip is read
        //   as several. 40/60 % of the way between off and on.
        const double lo = offLevel_ + 0.40 * (onLevel_ - offLevel_);
        const double hi = offLevel_ + 0.60 * (onLevel_ - offLevel_);

        const bool wasDip = inDip_;
        if (inDip_) { if (envFast_ > hi) inDip_ = false; }
        else        { if (envFast_ < lo) inDip_ = true;  }

        if (inDip_) dipSamples_ += 1; else gapSamples_ += 1;

        // ★★★ CAPTURE THE GAP WHEN THE DIP BEGINS, NOT WHEN IT ENDS. This read gapSamples_ at the
        //     dip's END while zeroing it at the dip's START, so the "gap before this dip" was
        //     always ~0 — and DCF77's minute detection, which is entirely "was there a gap of
        //     about two seconds?", could never fire. The decoder sat in Searching for ever on a
        //     signal it was otherwise reading perfectly.
        //     ★ DCF77 marks its minute by an ABSENCE, so the gap is not incidental telemetry
        //       here: it is the only synchronising feature the station transmits.
        clock_ += 1;
        if (!wasDip && inDip_) {
            gapBeforeMs_ = gapSamples_ * 1000.0 / sr_;
            gapSamples_ = 0;
            dipStartClock_ = clock_;
        }
        // A rising edge ends a dip: measure it and act.
        if (wasDip && !inDip_) {
            const double dipMs = dipSamples_ * 1000.0 / sr_;
            dipSamples_ = 0;
            onSecondEdge(dipMs, gapBeforeMs_);
        }

        // ★ No carrier at all: say NoSignal rather than sitting in Reading with a stale time on
        //   screen. 6 dB is the floor below which these are not decodable anyway.
        if (snrDb_ < 3.0 && state_ != State::NoSignal) { second_ = -1; setState(State::NoSignal); }
    }
}

/**
 * One dip has ended. `dipMs` is how long the carrier was down, `gapMs` how long it was up before.
 *
 * ★★★ THE SECOND BOUNDARY IS THE FALLING EDGE, not the rising one. Both stations drop the carrier
 *     AT the second, so the leading edge is the timing reference; the trailing edge only tells us
 *     how long the dip was. Timing off the rising edge would put every bit 100–200 ms late and
 *     make the two MSF sample windows land in the wrong slots.
 */
void TimeDecoder::onSecondEdge(double dipMs, double gapMs) {
    if (snrDb_ < 3.0) return;
    if (state_ == State::NoSignal) setState(State::Searching);

    if (station_ == Station::MSF) {
        // ★★★ A SECOND MAY CONTAIN TWO DIPS, AND THAT IS THE WHOLE DIFFICULTY OF MSF.
        //
        //     A and B are INDEPENDENT 100 ms windows: [0,100) is always off, [100,200) is off if
        //     A=1, [200,300) is off if B=1. So A=0,B=1 transmits as off-on-off — TWO dips inside
        //     one second. Reading every dip as a new second gets that wrong, and it is not an edge
        //     case: bits B54–B58 are the PARITY bits and are carried with A=0, so a decoder that
        //     mis-frames them fails on exactly the bits that were meant to validate the minute.
        //
        //     So the second boundary is a CLOCK, not "the next dip": a dip beginning more than
        //     ~700 ms after the current second started is the next second; one beginning around
        //     200 ms in is this second's B window.
        const double sinceSecondMs =
            secondStartClock_ > 0 ? (dipStartClock_ - secondStartClock_) * 1000.0 / sr_ : 1e9;

        if (sinceSecondMs > 150.0 && sinceSecondMs < 400.0 && second_ >= 0) {
            // The B window of the second already in progress.
            bitsB_[second_] = 1;
            return;
        }

        secondStartClock_ = dipStartClock_;
        // ★ The 500 ms dip IS second zero. Nothing else in the minute is that long, which is why
        //   MSF needs no gap-counting trick to find the minute.
        if (near(dipMs, kMsfMinute, 120.0)) {
            second_ = 0;
            setState(State::Reading);
            bitsA_[0] = bitsB_[0] = 0;
            if (onBit) onBit(0, 0);
            return;
        }
        if (second_ < 0) return;                       // still hunting for the minute
        second_++;
        if (second_ > 59) {                            // a minute passed with no marker
            second_ = -1;
            setState(State::Searching);
            return;
        }
        // A dip running past 150 ms carries A=1; past 250 ms it also carries B=1 (the two windows
        // ran together). A separate dip near 200 ms is handled above.
        bitsA_[second_] = dipMs > kMsfSampleA ? 1 : 0;
        bitsB_[second_] = dipMs > kMsfSampleB ? 1 : 0;
        if (onBit) onBit(second_, bitsA_[second_]);
    } else {
        // ── DCF77 ────────────────────────────────────────────────────────────
        // ★★★ THE MINUTE IS MARKED BY AN ABSENCE. Second 59 carries NO dip, so the tell is a gap
        //     of about two seconds between dips — the next dip is second 0. A decoder looking for
        //     a special pulse would never find one, because there isn't one.
        if (gapMs > 1500.0) {
            second_ = 0;
        } else if (second_ >= 0) {
            second_++;
            if (second_ > 59) { second_ = -1; setState(State::Searching); return; }
        } else {
            return;                                     // hunting for the minute gap
        }
        setState(State::Reading);
        const int bit = near(dipMs, 2 * kDipUnit, 50.0) ? 1 : 0;
        bitsA_[second_] = bit;
        if (onBit) onBit(second_, bit);
    }

    // A complete minute — try to read it.
    const bool endOfMinute = (station_ == Station::MSF) ? (second_ == 59) : (second_ == 58);
    if (endOfMinute) {
        TimeStamp ts;
        if (decodeMinute(ts)) {
            // ★★★ PARITY ALONE IS NOT ENOUGH, AND ON AIR THAT IS NOT THEORETICAL. MSF carries FOUR
            //     parity bits, so random noise satisfies all of them one time in sixteen — over a
            //     few minutes of marginal signal a false lock is likely rather than exotic. The
            //     first live run against Anthorn produced a confidently parity-checked
            //     "2064-02-22 06:16", which is exactly what that failure looks like: plausible
            //     structure, impossible content.
            //     ★★ So a minute must AGREE WITH THE ONE BEFORE IT — be exactly 60 seconds later.
            //        Two independent noise minutes landing one minute apart is ~1 in a million,
            //        and the cost to a real signal is one extra minute before the first reading.
            //        For a clock, being a minute late is nothing; being wrong is everything.
            const long long stamp = (((long long)ts.year * 12 + ts.month) * 31 + ts.day) * 1440
                                  + ts.hour * 60 + ts.minute;
            const bool follows = (lastStamp_ != 0) && (stamp == lastStamp_ + 1);
            lastStamp_ = stamp;
            if (!follows) {
                // Not yet corroborated. Keep reading rather than announcing a time we cannot back.
                setState(State::Reading);
                second_ = -1;
                return;
            }
            good_++;
            setState(State::Locked);
            if (onTime) onTime(ts);
        } else {
            lastStamp_ = 0;         // ★ a bad minute breaks the chain; corroboration restarts
            // ★ A failed parity is DISCARDED, not shown with a warning. See the header: a clock
            //   that is confidently wrong is worse than one that says it is still waiting.
            bad_++;
            setState(State::Reading);
        }
    }
}

bool TimeDecoder::decodeMinute(TimeStamp& out) const {
    return station_ == Station::MSF ? decodeMsf(out) : decodeDcf77(out);
}

/**
 * MSF, as published by NPL. The time is sent in bit A of seconds 17–51 and describes the minute
 * that STARTS at the next 500 ms marker — so what we decode at second 59 is the minute about to
 * begin, and that is what we report.
 * ★ Parity bits (B54–B57) are ODD over their named ranges. All four must pass.
 */
bool TimeDecoder::decodeMsf(TimeStamp& out) const {
    const int* A = bitsA_;
    const int* B = bitsB_;

    const int year  = bcd(A, 17, 24);
    const int month = bcd(A, 25, 29);
    const int day   = bcd(A, 30, 35);
    const int wday  = bcd(A, 36, 38);
    const int hour  = bcd(A, 39, 44);
    const int min   = bcd(A, 45, 51);

    // Odd parity, each over its own span.
    if (parityOdd(A, 17, 24) == B[54]) return false;
    if (parityOdd(A, 25, 35) == B[55]) return false;
    if (parityOdd(A, 36, 38) == B[56]) return false;
    if (parityOdd(A, 39, 51) == B[57]) return false;

    if (month < 1 || month > 12 || day < 1 || day > 31) return false;
    if (hour > 23 || min > 59) return false;

    out.year = 2000 + year;
    out.month = month; out.day = day;
    out.weekday = wday == 0 ? 7 : wday;     // MSF sends Sunday as 0; we report ISO 1..7
    out.hour = hour; out.minute = min;
    out.dst = B[58] != 0;
    out.leapSecondPending = false;
    return true;
}

/**
 * DCF77. Bit 20 is the start-of-time marker and must be 1; the three parity bits are EVEN.
 * ★ Bit 17/18 are the DST flags and are complementary — if both agree, the minute is corrupt,
 *   which is a cheap extra check the standard hands us for free.
 */
bool TimeDecoder::decodeDcf77(TimeStamp& out) const {
    const int* b = bitsA_;
    if (!b[20]) return false;                       // start of encoded time
    if (b[17] == b[18]) return false;               // CEST/CET flags must differ

    const int min   = bcd(b, 21, 27);
    const int hour  = bcd(b, 29, 34);
    const int day   = bcd(b, 36, 41);
    const int wday  = bcd(b, 42, 44);
    const int month = bcd(b, 45, 49);
    const int year  = bcd(b, 50, 57);

    if (parityOdd(b, 21, 28) != 0) return false;    // even parity => sum over data+parity is even
    if (parityOdd(b, 29, 35) != 0) return false;
    if (parityOdd(b, 36, 58) != 0) return false;

    if (month < 1 || month > 12 || day < 1 || day > 31) return false;
    if (hour > 23 || min > 59 || wday < 1 || wday > 7) return false;

    out.year = 2000 + year;
    out.month = month; out.day = day; out.weekday = wday;
    out.hour = hour; out.minute = min;
    out.dst = b[17] != 0;
    out.leapSecondPending = b[19] != 0;
    return true;
}

void TimeDecoder::pushBit(int) {}

}  // namespace vibe
