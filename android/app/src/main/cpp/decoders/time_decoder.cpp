#include "time_decoder.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>

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

/** ★★★ BCD, MOST SIGNIFICANT BIT FIRST — which is how MSF sends every field.
 *  bit 17A is 80, 18A is 40 … 24A is 1 (NPL's published table). The LSB-first reader below is for
 *  stations that do it the other way; using the wrong one produces a bit-REVERSED number that is
 *  still a plausible date, which is precisely how Anthorn decoded as "2064-02-22 06:16". */
int bcdMsb(const int* bits, int from, int to) {
    int v = 0;
    for (int i = from; i <= to; i++) v = (v * 2) + (bits[i] ? 1 : 0);
    // The field is BCD-weighted, not plain binary: rebuild from the published weights.
    int out = 0, n = to - from + 1;
    static const int w10[] = { 80, 40, 20, 10, 8, 4, 2, 1 };
    for (int i = 0; i < n; i++) if (bits[from + i]) out += w10[8 - n + i];
    (void)v;
    return out;
}

int parityOdd(const int* bits, int from, int to) {
    int n = 0;
    for (int i = from; i <= to; i++) n += bits[i] ? 1 : 0;
    return n & 1;
}

}  // namespace

TimeDecoder::TimeDecoder(int sampleRate, Station station)
    : sr_(sampleRate > 0 ? sampleRate : 48000), station_(station) {
    // ★★ WWV's code is on a 100 Hz SUBCARRIER, so the envelope has to be taken of THAT, not of the
    //    audio as a whole — WWV also carries voice announcements and 500/600 Hz tones, and an
    //    envelope of the lot would follow the announcer rather than the timecode. A narrow
    //    bandpass first (RBJ cookbook, Q=8) is what separates them.
    if (station_ == Station::WWV) {
        const double w0 = 2.0 * M_PI * 100.0 / sr_, Q = 8.0;
        const double alpha = std::sin(w0) / (2.0 * Q), c = std::cos(w0);
        const double a0 = 1.0 + alpha;
        bpB0_ =  alpha / a0; bpB1_ = 0.0; bpB2_ = -alpha / a0;
        bpA1_ = (-2.0 * c) / a0; bpA2_ = (1.0 - alpha) / a0;
    }
}

void TimeDecoder::setState(State s) {
    if (s == state_) return;
    state_ = s;
    if (onState) onState(s);
}

void TimeDecoder::process(const int16_t* samples, int count) {
    if (!samples || count <= 0) return;
    // Time constants: fast enough to see a 100 ms dip, slow enough to ignore audio-band noise.
    // ★★★ THE SMOOTHING MUST MATCH THE SUBCARRIER, NOT JUST THE SYMBOL. Rectifying a tone leaves
    //     ripple at TWICE its frequency, and the envelope has to remove that or it chatters across
    //     the threshold in the middle of a pulse. MSF/DCF77 arrive as a CW beat note of several
    //     hundred Hz, where 10 ms is ample. WWV's code is on a 100 Hz subcarrier — 200 Hz ripple —
    //     and 10 ms let it through: a 170 ms pulse was being read as 97 ms, then 119, then 137,
    //     with a scatter of 9–75 ms fragments in between, so a THIRD of all symbols were rejected
    //     and the frame never decoded.
    // ★ 30 ms is still six times shorter than WWV's shortest symbol, so nothing is blurred that
    //   matters — the constraint is "slower than the ripple, faster than the symbol".
    const double envMs = (station_ == Station::WWV) ? 0.030 : 0.010;
    const double aFast = 1.0 - std::exp(-1.0 / (envMs * sr_));
    const double aSlow = 1.0 - std::exp(-1.0 / (5.000 * sr_));   //  5 s

    for (int i = 0; i < count; i++) {
        double raw = (double)samples[i];
        if (station_ == Station::WWV) {
            const double y = bpB0_ * raw + bpB1_ * bpX1_ + bpB2_ * bpX2_ - bpA1_ * bpY1_ - bpA2_ * bpY2_;
            bpX2_ = bpX1_; bpX1_ = raw; bpY2_ = bpY1_; bpY1_ = y;
            raw = y;
        }
        const double x = std::fabs(raw);
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
        // ★★★ EXCEPT ON WWV, WHERE 40/60 SAT TOO HIGH AND CLIPPED EVERY PULSE SHORT. `onLevel_`
        //     tracks the PEAK — attack fast, release very slowly — which is right for MSF and
        //     DCF77, whose carrier is steady and whose peak IS the "on" level. WWV arrives over a
        //     fading HF path where the peak is far above a typical pulse, so the threshold ended up
        //     near the top of the envelope: MEASURED on a K3FEF capture, only 14.5 % of the time
        //     was spent above `hi` where WWV's duty cycle is 25-30 %, and the symbols came out as
        //     60-140 ms fragments of the 170 ms they should have been.
        //     ★ 0.15/0.28 measured against the same capture: 205 symbols -> 228, of a theoretical
        //       254. Lower still is worse (0.10/0.20 gives 197), which is the noise floor being
        //       crossed — so this is a measured optimum, not a guess in a direction.
        const double loFrac = (station_ == Station::WWV) ? 0.15 : 0.40;
        const double hiFrac = (station_ == Station::WWV) ? 0.28 : 0.60;
        const double lo = offLevel_ + loFrac * (onLevel_ - offLevel_);
        const double hi = offLevel_ + hiFrac * (onLevel_ - offLevel_);

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
            // ★★ RWM's callsign, off the SAME envelope. In CW the ID keys the carrier ON, so a
            //    MARK is a gap between dips and the dip we have just measured is the SILENCE
            //    after it — which is exactly the pair the Morse timer needs.
            if (station_ == Station::RWM) { morseMark(gapBeforeMs_); morseGap(dipMs); }
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
    } else if (station_ == Station::WWV) {
        // ── WWV/WWVH ─────────────────────────────────────────────────────────
        // ★★★ HERE THE SYMBOL IS THE PULSE, NOT THE DIP — the polarity is inverted relative to
        //     MSF and DCF77, and getting that backwards would decode noise very convincingly.
        //     Each second begins with ~30 ms of NO subcarrier, then the subcarrier is present for
        //     170 ms (0), 470 ms (1) or 770 ms (a position marker). So at every rising edge the
        //     short dip is the second tick and the gap BEFORE it is the previous second's symbol.
        const double pulse = gapMs;
        const int sym = near(pulse, 770.0, 120.0) ? 2
                      : near(pulse, 470.0, 110.0) ? 1
                      : near(pulse, 170.0, 90.0)  ? 0 : -1;
        // ★★★ AN UNREADABLE PULSE MUST STILL ADVANCE THE CLOCK. `return` here kept the second
        //     counter where it was, so ONE fade shifted every remaining bit of the minute one
        //     second early — and the fields then read as plausible nonsense rather than as an
        //     obvious failure (a live minute decoded as "20:12, day 24, year 120"). On a real HF
        //     path this is not rare: 15 MHz from K3FEF dropped 3-8 pulses a minute.
        //     ★★ So the second is derived from ELAPSED TIME, not from a count of pulses we happened
        //        to recognise. One pulse plus the dip after it IS one second by construction, so
        //        rounding that period gives how many seconds to step — including the 2 s step over
        //        a missing pulse. The unreadable second is recorded as 0 and the frame survives.
        const int steps = std::max(1, (int)std::lround((gapMs + dipMs) / 1000.0));

        // ★★★ THE SYMBOL BELONGS TO THE SECOND THAT HAS JUST ENDED, NOT THE ONE BEGINNING.
        //     The rising edge we are standing on STARTS the next second's pulse, so what we have
        //     just measured is the PREVIOUS second's. Recording it against the new second put
        //     every bit one second late — and because the data bits are sparse, the fields read
        //     back as zeros rather than as anything obviously wrong. All four came out 0 and the
        //     frame simply never validated.
        //     ★ So: place the symbol, THEN advance.
        // ★★★ THE MINUTE IS A HOLE, NOT A DOUBLE MARKER — MEASURED OFF THE AIR, NOT ASSUMED.
        //
        //     This waited for two 770 ms markers in a row and WWV NEVER SENDS THAT, so the only
        //     alignment rule the decoder had could not fire: it sat in Searching for ever and
        //     emitted not one bit. Recorded from K3FEF (Milford PA) on 15 MHz, 2026-08-11, the
        //     markers arrive at a flat 10.000 s cadence across a whole minute — 9, 19, 29, 39,
        //     49, 59 — with no pair anywhere.
        //
        //     What identifies the minute is that SECOND 0 CARRIES NO PULSE AT ALL. After second
        //     59's marker the subcarrier stays down for ~1.23 s (the marker's own 230 ms tail,
        //     then a silent second, then the 30 ms lead-in) where every other second gives at
        //     most 830 ms. In the capture that hole landed exactly on 20:57:00 UTC.
        //     ★★ So the threshold sits between those two: 830 ms is the longest ordinary dip (it
        //        follows a 170 ms "0") and 1230 ms is the hole. 1050 leaves ~200 ms either side,
        //        which is the margin a fading HF path actually needs.
        //     ★ The pulse being measured when this fires is therefore SECOND 1, not second 0 —
        //       the silent second is the one that was skipped, and it carries no data.
        //     ★★★ AND THE MARKER AND ITS HOLE ARRIVE ON THE SAME EDGE. onSecondEdge is handed the
        //         pulse and THEN the dip that followed it, so the marker that precedes the hole is
        //         THIS call's symbol, not the previous one's. Testing lastWasMarker_ here framed
        //         one minute and then lost it — right rule, read one second late.
        constexpr double kWwvHoleMs = 1050.0;
        if (sym == 2 && dipMs > kWwvHoleMs) {
            second_ = 59;                          // this marker is second 59; the hole after it is 0
            setState(State::Reading);
        } else if (second_ < 0) {
            lastWasMarker_ = (sym == 2);
            return;                                // still hunting for the marker-then-hole
        }
        lastWasMarker_ = (sym == 2);
        bitsA_[second_] = (sym == 1) ? 1 : 0;
        if (sym >= 0 && onBit) onBit(second_, bitsA_[second_]);
        emitPartial();
        if (second_ == 59) {
            TimeStamp ts;
            if (decodeWwv(ts)) {
                const long long stamp = (((long long)ts.year * 12 + ts.month) * 31 + ts.day) * 1440
                                      + ts.hour * 60 + ts.minute;
                const bool follows = (lastStamp_ != 0) && (stamp == lastStamp_ + 1);
                lastStamp_ = stamp;
                if (follows) { good_++; setState(State::Locked); if (onTime) onTime(ts); }
            } else { bad_++; lastStamp_ = 0; }
            // ★★★ THE NEXT SYMBOL IS SECOND 1's, NOT SECOND 0's. Second 0 is the hole and never
            //     produces an edge at all, so resetting to 0 here left every following bit one
            //     second early for the whole minute.
            bitsA_[0] = 0;                         // the hole carries no data
            second_ = 1;
            return;
        }
        second_ += steps;
        // ★ Stepped clean over the minute without seeing second 59 — a dropout on the marker
        //   itself. Wrap, and drop the corroboration chain rather than decode a half frame.
        if (second_ > 59) { second_ -= 60; lastStamp_ = 0; }
        return;
    } else if (station_ == Station::WWVB) {
        // ── WWVB ─────────────────────────────────────────────────────────────
        // ★ Same polarity as MSF/DCF77 — the carrier is ATTENUATED and the dip carries the symbol,
        //   so this reuses the envelope path they prove rather than WWV's subcarrier one.
        const int sym = near(dipMs, 800.0, 130.0) ? 2
                      : near(dipMs, 500.0, 110.0) ? 1
                      : near(dipMs, 200.0, 90.0)  ? 0 : -1;
        if (sym < 0) return;
        // ★★ The minute is TWO MARKERS IN A ROW (second 59's P6 then second 0's frame reference),
        //    exactly as WWV does it — WWVB transmits no unique minute pulse to look for.
        if (sym == 2 && lastWasMarker_) { second_ = 0; setState(State::Reading); }
        else if (second_ < 0) { lastWasMarker_ = (sym == 2); return; }
        else second_++;
        lastWasMarker_ = (sym == 2);
        if (second_ > 59) { second_ = -1; setState(State::Searching); return; }
        bitsA_[second_] = (sym == 1) ? 1 : 0;
        if (onBit) onBit(second_, bitsA_[second_]);
    } else if (station_ == Station::RWM) {
        // ── RWM ──────────────────────────────────────────────────────────────
        // ★★★ RWM SENDS NO TIMECODE, so there is nothing here to decode into a clock and this
        //     branch deliberately never produces one. What it can honestly report is that the
        //     station is being heard and that its second markers are being counted — which is what
        //     RWM is actually for: calibration and propagation. Anything more would be invented.
        if (second_ < 0) { second_ = 0; setState(State::Reading); }
        else second_ = (second_ + 1) % 60;
        if (onBit) onBit(second_, 1);
        // ★ Still report progress: RWM has no minute to decode, but the host uses this tick to
        //   flush any callsign heard — and a panel with no heartbeat looks dead.
        emitPartial();
        return;                                   // never falls through to a minute decode
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

    // ★ Tell the UI what we have SO FAR, every second. See TimeDecoder::Partial.
    emitPartial();

    // A complete minute — try to read it.
    const bool endOfMinute = (station_ == Station::MSF)   ? (second_ == 59)
                           : (station_ == Station::WWV)   ? (second_ == 59)
                           : (station_ == Station::WWVB)  ? (second_ == 59)
                                                          : (second_ == 58);
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

/**
 * What is known this far into the minute.
 *
 * ★★ EACH FIELD BECOMES MEANINGFUL AT A DIFFERENT SECOND, because each station sends them in its
 *    own order — MSF puts the year first and the minute last, DCF77 the minute first and the year
 *    last. So this is per-station, and reporting a field before its last bit has arrived would
 *    show a number that is briefly, confidently wrong.
 */
void TimeDecoder::emitPartial() {
    if (!onPartial || second_ < 0) return;
    Partial p;
    p.second = second_;
    const int* A = bitsA_;
    switch (station_) {
        case Station::MSF:
            if (second_ >= 24) { p.t.year    = 2000 + bcdMsb(A, 17, 24); p.year = true; }
            if (second_ >= 29) { p.t.month   = bcdMsb(A, 25, 29);        p.month = true; }
            if (second_ >= 35) { p.t.day     = bcdMsb(A, 30, 35);        p.day = true; }
            if (second_ >= 38) { const int wd = bcdMsb(A, 36, 38);
                                 p.t.weekday = wd == 0 ? 7 : wd;         p.weekday = true; }
            if (second_ >= 44) { p.t.hour    = bcdMsb(A, 39, 44);        p.hour = true; }
            if (second_ >= 51) { p.t.minute  = bcdMsb(A, 45, 51);        p.minute = true; }
            break;
        case Station::DCF77:
            if (second_ >= 27) { p.t.minute  = bcd(A, 21, 27);        p.minute = true; }
            if (second_ >= 34) { p.t.hour    = bcd(A, 29, 34);        p.hour = true; }
            if (second_ >= 41) { p.t.day     = bcd(A, 36, 41);        p.day = true; }
            if (second_ >= 44) { p.t.weekday = bcd(A, 42, 44);        p.weekday = true; }
            if (second_ >= 49) { p.t.month   = bcd(A, 45, 49);        p.month = true; }
            if (second_ >= 57) { p.t.year    = 2000 + bcd(A, 50, 57); p.year = true; }
            break;
        case Station::WWV: {
            auto w = [&](int bit, int weight) { return A[bit] ? weight : 0; };
            if (second_ >= 8)  { p.t.minute = w(1,1)+w(2,2)+w(3,4)+w(4,8)+w(6,10)+w(7,20)+w(8,40);
                                 p.minute = true; }
            if (second_ >= 16) { p.t.hour   = w(10,1)+w(11,2)+w(12,4)+w(13,8)+w(15,10)+w(16,20);
                                 p.hour = true; }
            break;
        }
        case Station::WWVB: {
            auto w = [&](int bit, int weight) { return A[bit] ? weight : 0; };
            if (second_ >= 8)  { p.t.minute = w(1,40)+w(2,20)+w(3,10)+w(5,8)+w(6,4)+w(7,2)+w(8,1);
                                 p.minute = true; }
            if (second_ >= 18) { p.t.hour   = w(12,20)+w(13,10)+w(15,8)+w(16,4)+w(17,2)+w(18,1);
                                 p.hour = true; }
            break;
        }
        case Station::RWM:
            break;      // nothing to fill in — it carries no timecode
    }
    onPartial(p);
}


// ── RWM's Morse identifier ───────────────────────────────────────────────────
//
// ★★ A DOT IS SHORTER THAN A DASH, AND THAT IS ALL WE ASSUME. The unit length adapts to what is
//    actually being sent: every mark nudges the estimate towards a dot (if it looks like one) or a
//    third of a dash (if it looks like one), so the decoder follows the operator's speed instead of
//    demanding a fixed one. RWM's ID is keyed at the station and its speed is not ours to assume.
// ★ Standard proportions: dash = 3 units, letter gap = 3, word gap = 7.

namespace {
/** Morse table, longest-first is unnecessary — the symbol string is an exact key. */
const char* morseFor(const std::string& sym) {
    struct E { const char* code; const char* ch; };
    static const E kTable[] = {
        {".-","A"},{"-...","B"},{"-.-.","C"},{"-..","D"},{".","E"},{"..-.","F"},{"--.","G"},
        {"....","H"},{"..","I"},{".---","J"},{"-.-","K"},{".-..","L"},{"--","M"},{"-.","N"},
        {"---","O"},{".--.","P"},{"--.-","Q"},{".-.","R"},{"...","S"},{"-","T"},{"..-","U"},
        {"...-","V"},{".--","W"},{"-..-","X"},{"-.--","Y"},{"--..","Z"},
        {"-----","0"},{".----","1"},{"..---","2"},{"...--","3"},{"....-","4"},
        {".....","5"},{"-....","6"},{"--...","7"},{"---..","8"},{"----.","9"},
        {"-..-.","/"},{"-...-","="},{".-.-.","+"},
    };
    for (const auto& e : kTable) if (sym == e.code) return e.ch;
    return nullptr;
}
}  // namespace

void TimeDecoder::morseMark(double onMs) {
    if (onMs < 15.0 || onMs > 2000.0) return;          // noise spike, or a marker pulse
    // Classify against the current unit, then let it adapt towards what we just saw.
    const bool dash = onMs > morseUnitMs_ * 2.0;
    morseSym_ += dash ? '-' : '.';
    const double impliedUnit = dash ? onMs / 3.0 : onMs;
    morseUnitMs_ += 0.25 * (impliedUnit - morseUnitMs_);
    if (morseUnitMs_ < 20.0)  morseUnitMs_ = 20.0;     // 60 wpm
    if (morseUnitMs_ > 240.0) morseUnitMs_ = 240.0;    // 5 wpm
    morseSilenceMs_ = 0;
}

void TimeDecoder::morseGap(double offMs) {
    if (morseSym_.empty()) return;
    // ★ A gap of three units ends the CHARACTER. Anything shorter is the space between a dot and
    //   a dash inside one, which must not break it up.
    if (offMs >= morseUnitMs_ * 2.0) morseFlush();
}

void TimeDecoder::morseFlush() {
    if (morseSym_.empty()) return;
    const char* ch = morseFor(morseSym_);
    morseSym_.clear();
    // ★ Unrecognised patterns are DROPPED, not guessed at. On a fading HF signal most rubbish is
    //   rubbish, and a decoder that emits its best guess turns a clean "RWM" into noise.
    if (ch && onMorse) onMorse(ch[0]);
}

bool TimeDecoder::decodeMinute(TimeStamp& out) const {
    switch (station_) {
        case Station::MSF:  return decodeMsf(out);
        case Station::WWV:  return decodeWwv(out);
        case Station::WWVB: return decodeWwvb(out);
        case Station::RWM:  return false;          // no timecode — see the note in onSecondEdge
        default:            return decodeDcf77(out);
    }
}

/**
 * WWV/WWVH, the NIST format. Two things make it unlike the European stations:
 *  ★★ THE DATE IS A DAY-OF-YEAR, not a month and a day, so it has to be converted — and the
 *     conversion needs the leap year, which is why the year is read first.
 *  ★ THERE IS NO PARITY AT ALL. Nothing in the frame validates it, so the corroboration rule
 *    (this minute must be exactly one later than the last) is not a belt-and-braces extra here —
 *    it is the ONLY check standing between noise and a confident wrong clock.
 */
/**
 * WWVB, the NIST 60 kHz format.
 *
 * ★★★ MSB FIRST, WHICH IS THE OPPOSITE OF WWV — seconds 1–8 carry 40,20,10,(unused),8,4,2,1. The
 *     two stations share a broadcaster and a purpose and almost nothing else, and using one map
 *     for the other yields a plausible wrong time with nothing to catch it.
 * ★★ THERE IS NO PARITY IN WWVB EITHER, so as with WWV the corroboration rule is the only
 *    validation: a reading is announced only when the next minute agrees with it.
 * ★ The date is a DAY-OF-YEAR, so the year is needed first to know whether to allow 29 February.
 */
bool TimeDecoder::decodeWwvb(TimeStamp& out) const {
    const int* b = bitsA_;
    auto w = [&](int bit, int weight) { return b[bit] ? weight : 0; };

    const int minute = w(1,40)+w(2,20)+w(3,10) + w(5,8)+w(6,4)+w(7,2)+w(8,1);
    const int hour   = w(12,20)+w(13,10) + w(15,8)+w(16,4)+w(17,2)+w(18,1);
    const int doy    = w(22,200)+w(23,100)
                     + w(25,80)+w(26,40)+w(27,20)+w(28,10)
                     + w(30,8)+w(31,4)+w(32,2)+w(33,1);
    const int yy     = w(45,80)+w(46,40)+w(47,20)+w(48,10)
                     + w(50,8)+w(51,4)+w(52,2)+w(53,1);

    if (hour > 23 || minute > 59 || doy < 1 || doy > 366 || yy > 99) return false;

    const int year = 2000 + yy;
    // ★ WWVB states the leap year itself (bit 55) — but deriving it is safer than trusting one
    //   unparity-checked bit, and the two must agree or the frame is suspect.
    const bool leapCalc = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    if (b[55] != (leapCalc ? 1 : 0)) return false;
    static const int len[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int d = doy, m = 0;
    for (; m < 12; m++) {
        const int dm = len[m] + ((m == 1 && leapCalc) ? 1 : 0);
        if (d <= dm) break;
        d -= dm;
    }
    if (m >= 12) return false;

    out.year = year; out.month = m + 1; out.day = d;
    out.hour = hour; out.minute = minute;
    out.weekday = 0;                       // WWVB sends no weekday
    out.dst = b[58] != 0;                  // DST in effect
    out.leapSecondPending = b[56] != 0;
    return true;
}

bool TimeDecoder::decodeWwv(TimeStamp& out) const {
    const int* b = bitsA_;
    auto w = [&](int bit, int weight) { return b[bit] ? weight : 0; };

    const int minute = w(1,1)+w(2,2)+w(3,4)+w(4,8) + w(6,10)+w(7,20)+w(8,40);
    const int hour   = w(10,1)+w(11,2)+w(12,4)+w(13,8) + w(15,10)+w(16,20);
    const int doy    = w(20,1)+w(21,2)+w(22,4)+w(23,8)
                     + w(25,10)+w(26,20)+w(27,40)+w(28,80)
                     + w(30,100)+w(31,200);
    const int yy     = w(50,1)+w(51,2)+w(52,4)+w(53,8) + w(55,10)+w(56,20)+w(57,40)+w(58,80);

    if (hour > 23 || minute > 59 || doy < 1 || doy > 366 || yy > 99) return false;

    const int year = 2000 + yy;
    const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    static const int len[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int d = doy, m = 0;
    for (; m < 12; m++) {
        const int dm = len[m] + ((m == 1 && leap) ? 1 : 0);
        if (d <= dm) break;
        d -= dm;
    }
    if (m >= 12) return false;                   // day-of-year past the end of the year

    out.year = year; out.month = m + 1; out.day = d;
    out.hour = hour; out.minute = minute;
    // ★ WWV broadcasts UTC and sends no weekday and no local DST — reporting either would be
    //   inventing information the station does not carry.
    out.weekday = 0;
    out.dst = false;
    out.leapSecondPending = false;
    return true;
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

    // ★★★ MSB FIRST. See bcdMsb() — reading these LSB-first bit-reverses every field and yields a
    //     date that passes every range check while being completely wrong.
    const int year  = bcdMsb(A, 17, 24);
    const int month = bcdMsb(A, 25, 29);
    const int day   = bcdMsb(A, 30, 35);
    const int wday  = bcdMsb(A, 36, 38);
    const int hour  = bcdMsb(A, 39, 44);
    const int min   = bcdMsb(A, 45, 51);

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
