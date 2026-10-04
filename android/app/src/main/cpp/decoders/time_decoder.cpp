#include "time_decoder.h"
#include <ctime>

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

// ── The NIST field maps — ONE table per station, read by the decode AND the progress line ────
// ★★★ THERE WERE TWO COPIES OF THE WWV MAP AND THEY DISAGREED (audit 2026-10-04 row 6). The
//     decode was corrected to the IRIG-H positions on 2026-08-12; the progress line kept the old
//     map (minute at seconds 1-8, hour at 10-16), so the panel showed the minute's bits as the
//     hour while the decode was right. One rule, two readers — now one table each.
// ★ Every position below is from NIST SP 432 (2002 ed.): WWV/WWVH Table 3.13 (p. 47), WWVB
//   Table 2.3 (p. 20). Both are BCD; WWV sends each group LSB first, WWVB MSB first.
struct BcdField { int n; int sec[10]; int wt[10]; };
constexpr BcdField kWwvMinute    = { 7, {10,11,12,13,15,16,17},             {1,2,4,8,10,20,40} };
constexpr BcdField kWwvHour      = { 6, {20,21,22,23,25,26},                {1,2,4,8,10,20} };
constexpr BcdField kWwvDoy       = {10, {30,31,32,33,35,36,37,38,40,41},    {1,2,4,8,10,20,40,80,100,200} };
/** ★★ THE WWV YEAR IS SPLIT ACROSS THE FRAME — SP 432 p. 48: "The last digit of the year is sent
 *  using bits 4 through 7. The next to last digit of the year, or the decade indicator, is sent
 *  using bits 51 through 54." Each digit is a 1-2-4-8 group, LSB first. */
constexpr BcdField kWwvYearUnits = { 4, {4,5,6,7},                          {1,2,4,8} };
constexpr BcdField kWwvYearTens  = { 4, {51,52,53,54},                      {1,2,4,8} };
constexpr int      kWwvDst1 = 2, kWwvLsw = 3, kWwvDst2 = 55;   // SP 432 Table 3.13 / p. 48
constexpr BcdField kWwvbMinute   = { 7, {1,2,3,5,6,7,8},                    {40,20,10,8,4,2,1} };
constexpr BcdField kWwvbHour     = { 6, {12,13,15,16,17,18},                {20,10,8,4,2,1} };
constexpr BcdField kWwvbDoy      = {10, {22,23,25,26,27,28,30,31,32,33},    {200,100,80,40,20,10,8,4,2,1} };
constexpr BcdField kWwvbYear     = { 8, {45,46,47,48,50,51,52,53},          {80,40,20,10,8,4,2,1} };

int readField(const int* bits, const BcdField& f) {
    int v = 0;
    for (int i = 0; i < f.n; i++) if (bits[f.sec[i]]) v += f.wt[i];
    return v;
}
/** The second at which the field's last bit has arrived — the progress line may show it from then. */
int fieldDone(const BcdField& f) {
    int m = 0;
    for (int i = 0; i < f.n; i++) if (f.sec[i] > m) m = f.sec[i];
    return m;
}

bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }
int  daysIn(int y, int m) {
    static const int len[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    return (m == 2 && isLeap(y)) ? 29 : len[(m - 1) % 12];
}

/** Day-of-year to month/day. False if the day is past the end of that year. */
bool doyToDate(int year, int doy, int& month, int& day) {
    if (doy < 1) return false;
    int d = doy;
    for (int m = 1; m <= 12; m++) {
        if (d <= daysIn(year, m)) { month = m; day = d; return true; }
        d -= daysIn(year, m);
    }
    return false;
}

/** ★★ A REAL MINUTE COUNT, so "exactly one minute later" survives the end of a short month.
 *  The old key packed every month as 31 days, so 30 April 23:59 -> 1 May 00:00 was 1441 "minutes"
 *  apart and the corroboration chain broke at four month-ends a year. That never mattered while
 *  WWV/WWVB reported the minute ENDING — the +1 carry below now crosses those boundaries itself.
 *  (Howard Hinnant's days_from_civil.) */
long long minuteIndex(int y, int m, int d, int hh, int mm) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const long long yoe = y - era * 400;
    const long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = era * 146097 + doe - 719468;
    return days * 1440 + hh * 60 + mm;
}

/** ★★★ ONE MINUTE ON, WITH EVERY CARRY — hour, day, month, year, and 29 February. */
void addMinute(TimeDecoder::TimeStamp& t) {
    if (++t.minute < 60) return;
    t.minute = 0;
    if (++t.hour < 24) return;
    t.hour = 0;
    if (t.weekday) t.weekday = t.weekday % 7 + 1;
    if (++t.day <= daysIn(t.year, t.month)) return;
    t.day = 1;
    if (++t.month <= 12) return;
    t.month = 1;
    t.year++;
}

/** ★ A dip must begin within this of a whole second after the anchor to count as that second's.
 *  The real second edges are a quartz-exact 1.000 s apart; 120 ms is three times the envelope's
 *  edge scatter on a marginal signal and still far from the mid-second where noise lands. */
constexpr double kGridTolS = 0.12;

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
        if (snrDb_ < 3.0 && state_ != State::NoSignal) {
            second_ = -1; anchorClock_ = 0; frameClosed_ = true; lastStamp_ = 0;
            setState(State::NoSignal);
        }
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
/* ★★★ EVERY WRITE TO bitsA_/bitsB_ IS CHECKED (audit 2026-10-03). They hold 60 entries and the
 *  index is a second counter driven by off-air timing; WWV stepped it by a rounded gap length,
 *  wrapped it ONCE, and a long fade (a 2-minute gap is a step of 120) wrote past the array. */
static inline bool inMinute(int s) { return s >= 0 && s < 60; }

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
        // ★★★ AND THE SECOND IS MEASURED FROM THE MINUTE MARKER, NOT COUNTED (2026-10-04, audit
        //     row 9). This used to step a counter on every dip more than 400 ms into the second, so
        //     ONE faded dip or ONE noise dip mid-second moved every later bit by a second — and the
        //     minute then read as a plausible neighbour's data rather than as a failure. Now each
        //     dip's START is placed by its distance from the 500 ms marker: within ±120 ms of a
        //     whole second it is that second's; 150–400 ms past one it is that second's B window;
        //     anywhere else it is noise and is ignored. A missing or unreadable second is an
        //     ERASURE and the minute is not decoded — never decoded one place out.
        const bool marker = near(dipMs, kMsfMinute, 120.0);
        if (marker) {
            // ★ The 500 ms dip IS second zero. Nothing else in the minute is that long, so the
            //   newest marker always wins: if it is not ~60 s after the last one, the last one was
            //   a merged pair of dips and the minute it began is abandoned (closeFrame fails it on
            //   its erasures), not decoded against the wrong origin.
            if (anchorClock_ > 0) closeFrame();
            beginFrame(dipStartClock_);
            if (onBit) onBit(0, 0);
            emitPartial();
            return;
        }
        if (anchorClock_ == 0) return;                   // still hunting for the minute marker
        double e = (double)(dipStartClock_ - anchorClock_) / sr_;
        if (e > 59.5 + kGridTolS) {
            // ★ Past second 59 with no marker seen: the marker itself faded. Keep the grid (it is
            //   quartz on both ends) for one more minute, but nothing in it can be decoded without
            //   its second 0 — and two minutes without one is lost lock.
            closeFrame();
            if (e > 119.5) { loseFrame(); return; }
            beginFrame(anchorClock_ + 60LL * sr_);
            e -= 60.0;
        }
        const long r = std::lround(e);
        if (std::fabs(e - (double)r) <= kGridTolS && r >= 1 && r <= 59) {
            // The second's own dip. 100 ms = A0 B0; 200 = A1 B0; 300 = A1 B1 (the two windows ran
            // together). Anything else is unreadable — an erasure, not a best guess.
            int a = 0, bb = 0; bool readable = true;
            if      (near(dipMs, 100.0, 50.0)) { a = 0; bb = 0; }
            else if (near(dipMs, 200.0, 50.0)) { a = 1; bb = 0; }
            else if (near(dipMs, 300.0, 50.0)) { a = 1; bb = 1; }
            else readable = false;
            place((int)r, a, bb, a, readable);
            second_ = (int)r;
            if (onBit) onBit(second_, bitsA_[second_]);
            emitPartial();
            if (r == 59) closeFrame();
            return;
        }
        const int s = (int)std::floor(e);
        const double f = e - s;
        if (s >= 1 && s <= 59 && f >= 0.15 && f <= 0.40) {
            // The B window of second s — legal only as a 100 ms dip after a 100 ms (A=0) one.
            if (slot_[s] == 1 && !bitsA_[s] && !bitsB_[s] && near(dipMs, 100.0, 50.0)) bitsB_[s] = 1;
            else slot_[s] = 2;
            return;
        }
        return;                                          // off the grid: noise, not a second
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
        // ★ capped at a minute BEFORE the int cast: a gap of hours is still just "lost the frame"
        const int steps = std::max(1, (int)std::lround(std::min(60.0, (gapMs + dipMs) / 1000.0)));

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
        // ★ A frame read while HUNTING was never read at all — its bits are whatever the last
        //   minute left. Only a minute that began at an anchor may be decoded.
        const bool frameWasRead = second_ >= 0;
        if (sym == 2 && dipMs > kWwvHoleMs) {
            // ★★★ THE ANCHOR IS A CLOCK REFERENCE, NOT JUST A RESET. Everything after it is placed
            //     by DISTANCE from here — see below.
            anchorClock_ = (long long)((double)dipStartClock_ - gapMs * sr_ / 1000.0);
            second_ = 59;                          // this marker is second 59; the hole after it is 0
            setState(State::Reading);
        } else if (second_ < 0) {
            lastWasMarker_ = (sym == 2);
            return;                                // still hunting for the marker-then-hole
        }
        lastWasMarker_ = (sym == 2);
        // ★★★ WHICH SECOND IS THIS? MEASURE IT, DO NOT COUNT IT.
        //
        //     Incrementing per symbol assumes exactly one symbol per second, and on a real HF path
        //     that is not true: a fading pulse SPLITS and arrives as two events, so the counter
        //     gains a second and every field after it reads its neighbour's data. Measured on a
        //     live WT8P capture (2026-08-12): the frame decoded with hour=5,6,7 across three
        //     consecutive minutes — the MINUTE counter, sitting where the hour should be — and the
        //     array's own content put the hour field 10 places later than the map looked, with the
        //     minute field one place before that. Two different offsets in one frame is the
        //     signature of counting rather than measuring, since each slip moves everything after
        //     it and nothing before.
        //     ★★ The anchor gives a hard reference once a minute, and every second in that minute
        //        is a known distance from it: second N begins N seconds after second 0, which is
        //        the hole immediately following the anchor. So place each symbol by rounding that
        //        distance — a split pulse then lands in the SAME second twice instead of shifting
        //        the frame, and a missed pulse leaves a gap instead of pulling everything back.
        //     ★ Only when anchored. Before the first anchor the count is all we have, and it is
        //       discarded anyway (second_ < 0 returns above).
        if (anchorClock_ > 0) {
            // ★★★ MEASURE FROM THE PULSE'S START, NOT ITS END. Every second's pulse BEGINS at a
            //     fixed 30 ms past the tick and then runs for 170, 470 or 770 ms depending on what
            //     it is saying — so the end moves by 600 ms with the DATA. Timing off the end put
            //     symbols half a second either side of the truth and rounded them into the wrong
            //     second; timing off the start is the same instant every time.
            const double pulseStart = (double)dipStartClock_ - gapMs * sr_ / 1000.0;
            const double since = (pulseStart - (double)anchorClock_) / (double)sr_;
            // The anchor IS second 59, so everything is measured relative to that.
            const int measured = (int)(((59 + (long long)std::lround(since)) % 60 + 60) % 60);
            if (measured >= 0 && measured <= 59) second_ = measured;
        }
        if (!inMinute(second_)) { second_ = -1; setState(State::Searching); return; }
        bitsA_[second_] = (sym == 1) ? 1 : 0;
        // ★★ Which seconds actually arrived readable. WWV still records an unreadable pulse as 0
        //    and decodes on (a live HF minute drops 3-8 pulses, and an all-or-nothing rule would
        //    never decode one) — but the YEAR is only taken from the air when all eight of its
        //    seconds were read; otherwise decodeWwv falls back to the host. See there.
        slot_[second_] = (slot_[second_] == 0 && sym >= 0) ? 1 : 2;
        if (sym >= 0 && onBit) onBit(second_, bitsA_[second_]);
        // ★ Not on the anchoring edge after a hunt: second_ is 59 there, so every field would be
        //   "complete" — and drawn from bits nobody read (a progress line of 00:00, year 2000).
        if (frameWasRead) emitPartial();
        if (second_ == 59) {
            if (frameWasRead) {
                TimeStamp ts;
                const bool ok = decodeWwv(ts);
                finishMinute(ok, ts);
            }
            // ★★★ THE NEXT SYMBOL IS SECOND 1's, NOT SECOND 0's. Second 0 is the hole and never
            //     produces an edge at all, so resetting to 0 here left every following bit one
            //     second early for the whole minute.
            bitsA_[0] = 0;                         // the hole carries no data
            for (int i = 0; i < 60; i++) slot_[i] = 0;
            second_ = 1;
            return;
        }
        second_ += steps;
        // ★ Stepped clean over the minute without seeing second 59 — a dropout on the marker
        //   itself. Wrap, and drop the corroboration chain rather than decode a half frame.
        if (second_ > 59) {                    // ★ % not -= : see inMinute
            second_ %= 60; lastStamp_ = 0;
            for (int i = 0; i < 60; i++) slot_[i] = 0;
        }
        return;
    } else if (station_ == Station::WWVB) {
        // ── WWVB ─────────────────────────────────────────────────────────────
        // ★ Same polarity as MSF/DCF77 — the carrier is ATTENUATED and the dip carries the symbol,
        //   so this reuses the envelope path they prove rather than WWV's subcarrier one.
        const int sym = near(dipMs, 800.0, 130.0) ? 2
                      : near(dipMs, 500.0, 110.0) ? 1
                      : near(dipMs, 200.0, 90.0)  ? 0 : -1;
        const long long prevMarker = lastDipClock_;
        if (sym == 2) lastDipClock_ = dipStartClock_;
        // ★★ The minute is TWO MARKERS IN A ROW (second 59's P0 then second 0's Pr, SP 432 p. 21)
        //    — WWVB transmits no unique minute pulse to look for. ★ "In a row" is now a MEASURED
        //    1.0 s between their leading edges, not "the previous dip we happened to classify".
        if (anchorClock_ == 0) {
            if (sym == 2 && prevMarker > 0
                && std::fabs((double)(dipStartClock_ - prevMarker) / sr_ - 1.0) <= kGridTolS) {
                beginFrame(dipStartClock_);
                place(0, 0, 0, 2, true);
                if (onBit) onBit(0, 0);
                emitPartial();
            }
            return;
        }
        // ★★★ PLACED BY ELAPSED TIME, NOT COUNTED (2026-10-04, audit row 9). This used to
        //     `return` on an unreadable dip WITHOUT advancing the second, and count every
        //     readable one — so one fade moved the rest of the minute a second early, and a
        //     noise dip of about 200 ms moved it a second late. Now each dip is the second its
        //     leading edge falls on (±120 ms), an unreadable one ERASES that second, and a dip
        //     between the seconds is noise and ignored.
        const double e = (double)(dipStartClock_ - anchorClock_) / sr_;
        long r = std::lround(e);
        if (std::fabs(e - (double)r) > kGridTolS) return;     // off the grid: noise
        if (r >= 60) {
            closeFrame();
            if (r == 60 && sym == 2) {                          // ~60 s on: the next Pr, re-anchor
                beginFrame(dipStartClock_);
                place(0, 0, 0, 2, true);
                if (onBit) onBit(0, 0);
                emitPartial();
                return;
            }
            // ★ A readable NON-marker where second 0 must be means the anchor was wrong (a noise
            //   marker beside a real one). Hunt again rather than read a minute out of phase.
            if (r >= 120 || (r == 60 && sym >= 0)) { loseFrame(); return; }
            beginFrame(anchorClock_ + 60LL * sr_);              // Pr itself faded: keep the grid
            r -= 60;
        }
        if (r < 1) return;
        place((int)r, sym == 1 ? 1 : 0, 0, sym, sym >= 0);
        second_ = (int)r;
        if (onBit) onBit(second_, bitsA_[second_]);
        emitPartial();
        if (r == 59) closeFrame();
        return;
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
        // ★★★ BUT ONE MISSED DIP LOOKS EXACTLY THE SAME (2026-10-04, audit row 9). A 1.9 s gap
        //     was taken as a minute start wherever it fell, so one faded second re-anchored the
        //     frame mid-minute, and a noise dip mid-second stepped the counter. Now:
        //       • the minute gap is accepted only when it is the FIRST one (hunting), or when the
        //         dip after it lands ~60 s after the current anchor;
        //       • every other dip is placed by the distance of its leading edge from the anchor
        //         (±120 ms of a whole second, or it is noise and ignored);
        //       • a missing or unreadable second is an ERASURE and fails the minute;
        //       • a dip AT second 59 is something DCF77 never sends, so the anchor was a missed
        //         dip, not the minute — drop it and hunt again. (A leap second does put a dip at 59;
        //         it costs one minute of lock twice a decade, which is the right way round.)
        const bool readable = near(dipMs, kDipUnit, 50.0) || near(dipMs, 2 * kDipUnit, 50.0);
        const int  bit = near(dipMs, 2 * kDipUnit, 50.0) ? 1 : 0;
        const double sinceLast = lastDipClock_ > 0
            ? (double)(dipStartClock_ - lastDipClock_) / sr_ : 1e9;
        lastDipClock_ = dipStartClock_;
        if (anchorClock_ == 0) {
            if (sinceLast < 1.85) return;                       // hunting for the minute gap
            beginFrame(dipStartClock_);
            place(0, bit, 0, bit, readable);
            if (onBit) onBit(0, bit);
            emitPartial();
            return;
        }
        const double e = (double)(dipStartClock_ - anchorClock_) / sr_;
        long r = std::lround(e);
        if (std::fabs(e - (double)r) > kGridTolS) return;     // off the grid: noise
        if (r == 59) { loseFrame(); return; }                   // see above: the anchor was wrong
        if (r >= 60) {
            closeFrame();
            if (r >= 120) { loseFrame(); return; }
            // r == 60 is the consistent minute gap: re-anchor on the real edge. Past it, second 0
            // itself faded — keep the grid; that minute fails on its erasure.
            beginFrame(r == 60 ? dipStartClock_ : anchorClock_ + 60LL * sr_);
            r -= 60;
        }
        place((int)r, bit, 0, bit, readable);
        second_ = (int)r;
        if (onBit) onBit(second_, bit);
        emitPartial();
        if (r == 58) closeFrame();
        return;
    }
}

// ── Framing by elapsed time ──────────────────────────────────────────────────────────────────
void TimeDecoder::beginFrame(long long anchorClock) {
    anchorClock_ = anchorClock;
    for (int i = 0; i < 60; i++) { bitsA_[i] = bitsB_[i] = 0; slot_[i] = 0; sym_[i] = 0; }
    frameClosed_ = false;
    second_ = 0;
    setState(State::Reading);
}

void TimeDecoder::place(int sec, int a, int b, int sym, bool readable) {
    if (!inMinute(sec)) return;
    // ★ Two symbols claiming one second is not "take the later one": one of them is noise and we
    //   cannot say which, so the second is erased.
    if (slot_[sec] != 0) { slot_[sec] = 2; return; }
    slot_[sec] = readable ? 1 : 2;
    bitsA_[sec] = a; bitsB_[sec] = b; sym_[sec] = (signed char)sym;
}

bool TimeDecoder::slotsComplete(int from, int to) const {
    for (int i = from; i <= to; i++) if (slot_[i] != 1) return false;
    return true;
}

void TimeDecoder::loseFrame() {
    anchorClock_ = 0;
    frameClosed_ = true;
    second_ = -1;
    lastStamp_ = 0;
    setState(State::Searching);
}

/** Decode the minute in hand, once. ★★ An erasure anywhere the station sends code fails it. */
void TimeDecoder::closeFrame() {
    if (frameClosed_) return;
    frameClosed_ = true;
    TimeStamp ts;
    bool ok = false;
    switch (station_) {
        case Station::MSF:   ok = slotsComplete(1, 59) && decodeMsf(ts);   break;
        case Station::DCF77: ok = slotsComplete(0, 58) && decodeDcf77(ts); break;
        case Station::WWVB: {
            // ★ The free framing check: markers at 0, 9, 19 … 59 and nowhere else (SP 432
            //   Table 2.3). A complete minute with them elsewhere was read out of phase.
            const bool complete = slotsComplete(0, 59);
            bool framed = complete;
            for (int i = 0; framed && i < 60; i++)
                if ((sym_[i] == 2) != (i == 0 || i % 10 == 9)) framed = false;
            if (complete && !framed) { finishMinute(false, ts); loseFrame(); return; }
            ok = framed && decodeWwvb(ts);
            break;
        }
        default: break;
    }
    finishMinute(ok, ts);
}

void TimeDecoder::finishMinute(bool decoded, const TimeStamp& ts) {
    if (!decoded) {
        lastStamp_ = 0;         // ★ a bad minute breaks the chain; corroboration restarts
        // ★ A failed parity is DISCARDED, not shown with a warning. See the header: a clock
        //   that is confidently wrong is worse than one that says it is still waiting.
        bad_++;
        setState(State::Reading);
        return;
    }
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
    const long long stamp = minuteIndex(ts.year, ts.month, ts.day, ts.hour, ts.minute);
    const bool follows = (lastStamp_ != 0) && (stamp == lastStamp_ + 1);
    lastStamp_ = stamp;
    if (!follows) { setState(State::Reading); return; }   // not yet corroborated
    good_++;
    setState(State::Locked);
    if (onTime) onTime(ts);
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
            // ★★★ THE SAME TABLE decodeWwv READS (audit 2026-10-04 row 6) — this used the pre-
            //     2026-08-12 map, minute at 1-8 and hour at 10-16, so the line showed the MINUTE's
            //     bits as the hour. These are the transmitted fields: the time at the START of
            //     this frame, which is the minute in progress (SP 432 p. 46).
            if (second_ >= fieldDone(kWwvMinute)) { p.t.minute = readField(A, kWwvMinute); p.minute = true; }
            if (second_ >= fieldDone(kWwvHour))   { p.t.hour   = readField(A, kWwvHour);   p.hour = true; }
            // ★ The day needs the year (for leap), and the year's tens digit is the LAST field to
            //   arrive (second 54) — so date and year appear together, once both halves are in.
            if (second_ >= fieldDone(kWwvYearTens)) {
                const int u = readField(A, kWwvYearUnits), t = readField(A, kWwvYearTens);
                if (u <= 9 && t <= 9) {
                    p.t.year = 2000 + t * 10 + u; p.year = true;
                    if (doyToDate(p.t.year, readField(A, kWwvDoy), p.t.month, p.t.day))
                        p.month = p.day = true;
                }
            }
            break;
        }
        case Station::WWVB: {
            if (second_ >= fieldDone(kWwvbMinute)) { p.t.minute = readField(A, kWwvbMinute); p.minute = true; }
            if (second_ >= fieldDone(kWwvbHour))   { p.t.hour   = readField(A, kWwvbHour);   p.hour = true; }
            if (second_ >= fieldDone(kWwvbYear))   {
                p.t.year = 2000 + readField(A, kWwvbYear); p.year = true;
                if (doyToDate(p.t.year, readField(A, kWwvbDoy), p.t.month, p.t.day))
                    p.month = p.day = true;
            }
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
    const int minute = readField(b, kWwvbMinute);
    const int hour   = readField(b, kWwvbHour);
    const int doy    = readField(b, kWwvbDoy);
    const int yy     = readField(b, kWwvbYear);

    if (hour > 23 || minute > 59 || doy < 1 || doy > 366 || yy > 99) return false;

    const int year = 2000 + yy;
    // ★ WWVB states the leap year itself (bit 55) — but deriving it is safer than trusting one
    //   unparity-checked bit, and the two must agree or the frame is suspect.
    if (b[55] != (isLeap(year) ? 1 : 0)) return false;
    int month = 0, day = 0;
    if (!doyToDate(year, doy, month, day)) return false;

    out.year = year; out.month = month; out.day = day;
    out.hour = hour; out.minute = minute;
    out.weekday = 0;                       // WWVB sends no weekday
    out.dst = b[58] != 0;                  // DST in effect
    out.leapSecondPending = b[56] != 0;
    // ★★★ THE FRAME CARRIES THE MINUTE THAT IS ENDING, NOT THE ONE ABOUT TO BEGIN — the opposite
    //     of MSF and DCF77 (audit 2026-10-04 row 8). SP 432 p. 21: "The on-time reference point of
    //     the time code frame is the leading edge of the reference bit Pr" — second 0 — so the
    //     bits describe the minute that STARTED at this frame's second 0. We decode at second 59,
    //     0.2-0.8 s before the next minute begins, and announce THAT minute, as MSF/DCF77 do;
    //     reporting the frame's own value made the clock a minute late.
    addMinute(out);
    return true;
}

bool TimeDecoder::decodeWwv(TimeStamp& out) const {
    const int* b = bitsA_;

    // ★★★ THE FIELDS SIT ONE DECADE LATER THAN THIS USED TO LOOK — WWV IS AN IRIG-H FRAME.
    //
    //     Seconds 1-9 carry DUT1 and flags, NOT the minute: the BCD time starts in the SECOND
    //     decade. Reading it from second 1 put every field one decade early, so the "hour" slot
    //     returned the MINUTE and the "day" slot returned the HOUR — which is exactly what a live
    //     capture showed: hour=5,6,7 across three consecutive minutes at 19:05, 19:06, 19:07, and
    //     doy=19 when the hour was 19 (WT8P, Sammamish WA, 15 MHz, 2026-08-12).
    //
    // ★★★ AND THE FRAMING WAS NEVER AT FAULT, which is why this took so long to see. WWV marks
    //     each minute with an 800 ms 1000 Hz tone; in that capture the tone falls at capture
    //     seconds 12, 72, 132, 192 and the decoder's anchor fires at 13, 73, 133, 193 — one second
    //     later, exactly as it should for a marker at second 59 followed by the second-0 hole.
    //     A clock-independent landmark settled in one measurement what argument could not.
    // ★★ Verified against the same frames: hour=19 and day-of-year=224 both come out right, and
    //    the bits that differ between consecutive minutes are the 1, 2 and 4 weights of the minute
    //    field — 5, 6, 7 in LSB-first BCD — sitting in the second decade.
    const int minute = readField(b, kWwvMinute);
    const int hour   = readField(b, kWwvHour);
    const int doy    = readField(b, kWwvDoy);
    if (hour > 23 || minute > 59 || doy < 1 || doy > 366) return false;

    // ★★★ THE YEAR IS ON THE AIR — SPLIT ACROSS THE FRAME, WHICH IS WHY IT WAS NEVER FOUND
    //     (2026-10-04, audit row 7). The 2026-08-12 note here said no placement of a two-digit
    //     BCD year in the TAIL of the frame gave 26, so the year was taken from the host clock.
    //     That search was looking for one field; SP 432 p. 48 sends two: "The last digit of the
    //     year is sent using bits 4 through 7. The next to last digit … using bits 51 through 54."
    //     For 26 that is 6 at seconds 5+6 and 2 at second 52 — one set bit in the tail, which is
    //     what those six live frames showed.
    // ★★ CROSS-CHECKED AGAINST THE HOST, NOT TRUSTED BLIND: WWV has no parity, and the year
    //    decides leap (a misread 2012 once put every date a day early). So:
    //      • all eight year seconds read cleanly and both digits valid → the air's year — unless
    //        the host has a believable clock and differs by more than one, in which case one of
    //        them is wrong and with no parity we cannot say which: the minute is not decoded;
    //      • any year second unreadable → the host's year (the old behaviour), and only then.
    int hostYear = 0, hostYday = 0;
    {
        const time_t nowT = time(nullptr);
        const struct tm* utc = gmtime(&nowT);
        if (utc) { hostYear = utc->tm_year + 1900; hostYday = utc->tm_yday; }
    }
    const bool hostBelievable = hostYear >= 2024;   // ★ a reset device boots in 1970 or 2000
    bool yearRead = true;
    for (int i = 0; i < 4; i++)
        if (slot_[kWwvYearUnits.sec[i]] != 1 || slot_[kWwvYearTens.sec[i]] != 1) yearRead = false;
    const int units = readField(b, kWwvYearUnits), tens = readField(b, kWwvYearTens);
    int year = 0;
    if (yearRead && units <= 9 && tens <= 9) {
        year = 2000 + tens * 10 + units;
        if (hostBelievable && std::abs(year - hostYear) > 1) return false;
    } else if (hostBelievable) {
        // ★ The frame describes its own START, so on 1 January a frame for 31 December belongs
        //   to the host's PREVIOUS year.
        year = (doy > 300 && hostYday < 31) ? hostYear - 1 : hostYear;
    } else {
        return false;                                // neither source can be believed
    }

    int month = 0, day = 0;
    if (!doyToDate(year, doy, month, day)) return false;   // day-of-year past the end of the year

    out.year = year; out.month = month; out.day = day;
    out.hour = hour; out.minute = minute;
    // ★ WWV broadcasts UTC and sends no weekday — reporting one would be inventing it.
    out.weekday = 0;
    // ★★ DST AND THE LEAP-SECOND WARNING ARE ON THE AIR TOO, and were hard-coded false.
    //    SP 432 p. 48: DST at seconds 2 and 55 — both 0 in standard time, both 1 in DST; on a
    //    change day second 55 flips at 0000 UTC and second 2 "exactly 24 hours later". So 55 is
    //    the status for the day ahead and 2 the status the day began with; `dst` takes second 2,
    //    the same lagging bit decodeWwvb reads (WWVB's 58 also flips 24 h after 57, p. 21).
    //    Leap-second warning: second 3, "a leap second will be added to UTC at the end of the
    //    current month".
    out.dst = b[kWwvDst1] != 0;
    out.leapSecondPending = b[kWwvLsw] != 0;
    (void)kWwvDst2;
    // ★★★ AND THE TIME IS THE MINUTE JUST ENDED (audit 2026-10-04 row 8). SP 432 p. 46: "The
    //     information in the time code refers to the time at the start of the one-minute frame."
    //     The frame is closed by the first pulse of the NEXT minute (1.03 s into it), so the
    //     minute in progress when this is reported is the frame's own value plus one. Reporting
    //     the frame's value made WWV a minute late — unlike MSF and DCF77, which announce ahead.
    addMinute(out);
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
    if (b[0]) return false;                         // ★ M: start of minute, always 0 (PTB) — free
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
