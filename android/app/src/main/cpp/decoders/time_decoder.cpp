#include "time_decoder.h"
#include <ctime>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>

namespace vibe {
namespace {

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
/** ★★ MSF AND DCF77 FROM TABLES TOO (2026-10-05), so the BCD-digit check below can see each
 *  digit's bits — the old bcd()/bcdMsb() readers summed the weights and could not tell "10" sent
 *  as tens=1 from "10" sent as units=1010, which no transmitter sends and parity cannot catch.
 *  The VALUES are what those readers gave, weight for weight: MSF is MSB first (NPL — bit 17A is
 *  80 … 24A is 1), DCF77 LSB first (PTB). */
constexpr BcdField kMsfYear      = { 8, {17,18,19,20,21,22,23,24},          {80,40,20,10,8,4,2,1} };
constexpr BcdField kMsfMonth     = { 5, {25,26,27,28,29},                   {10,8,4,2,1} };
constexpr BcdField kMsfDay       = { 6, {30,31,32,33,34,35},                {20,10,8,4,2,1} };
constexpr BcdField kMsfWeekday   = { 3, {36,37,38},                         {4,2,1} };
constexpr BcdField kMsfHour      = { 6, {39,40,41,42,43,44},                {20,10,8,4,2,1} };
constexpr BcdField kMsfMin       = { 7, {45,46,47,48,49,50,51},             {40,20,10,8,4,2,1} };
constexpr BcdField kDcfMinute    = { 7, {21,22,23,24,25,26,27},             {1,2,4,8,10,20,40} };
constexpr BcdField kDcfHour      = { 6, {29,30,31,32,33,34},                {1,2,4,8,10,20} };
constexpr BcdField kDcfDay       = { 6, {36,37,38,39,40,41},                {1,2,4,8,10,20} };
constexpr BcdField kDcfWeekday   = { 3, {42,43,44},                         {1,2,4} };
constexpr BcdField kDcfMonth     = { 5, {45,46,47,48,49},                   {1,2,4,8,10} };
constexpr BcdField kDcfYear      = { 8, {50,51,52,53,54,55,56,57},          {1,2,4,8,10,20,40,80} };
/** ★ MSF's minute identifier, bits A52-A59 (NPL): 0 1 1 1 1 1 1 0. Port of the check in
 *  madpsy/ubersdr-ntp (GPL-3.0-or-later), MsfDecoder.cpp identifierAt(). */
constexpr int kMsfIdentifier[8] = { 0, 1, 1, 1, 1, 1, 1, 0 };
/** ★ WWVB seconds that are always 0 (SP 432 Table 2.3). */
constexpr int kWwvbZeroBits[11] = { 4, 10, 11, 14, 20, 21, 24, 34, 35, 44, 54 };

int readField(const int* bits, const BcdField& f) {
    int v = 0;
    for (int i = 0; i < f.n; i++) if (bits[f.sec[i]]) v += f.wt[i];
    return v;
}
/** ★★★ EVERY BCD DIGIT ≤ 9 (2026-10-05). A nibble of 1010-1111 is a misread that the field's
 *  RANGE check can miss — minute units 1010 sums to "10", a perfectly good minute — and that
 *  parity passes whenever the misread came in pairs. Digits are grouped by decade of weight
 *  (1-8 units, 10-80 tens, 100-200 hundreds). Ported from madpsy/ubersdr-ntp
 *  (GPL-3.0-or-later): Dcf77Decoder.cpp / MsfDecoder.cpp check each digit; their WWV/WWVB do
 *  not, and ours do — those two stations have no parity at all, so they need it most. */
bool digitsOk(const int* bits, const BcdField& f) {
    int d[3] = { 0, 0, 0 };
    for (int i = 0; i < f.n; i++) {
        if (!bits[f.sec[i]]) continue;
        const int w = f.wt[i];
        if (w >= 100) d[2] += w / 100; else if (w >= 10) d[1] += w / 10; else d[0] += w;
    }
    return d[0] <= 9 && d[1] <= 9 && d[2] <= 9;
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
long long daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const long long yoe = y - era * 400;
    const long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
long long minuteIndex(int y, int m, int d, int hh, int mm) {
    return daysFromCivil(y, m, d) * 1440 + hh * 60 + mm;
}
/** ISO weekday, 1 = Monday .. 7 = Sunday (1970-01-01 was a Thursday). */
int isoWeekday(int y, int m, int d) {
    const long long days = daysFromCivil(y, m, d);
    return (int)(((days + 3) % 7 + 7) % 7) + 1;
}
/** ★★ THE DATE MUST EXIST AND THE WEEKDAY MUST BE ITS WEEKDAY (2026-10-05). MSF and DCF77 send
 *  both, so a 30 February, or a Tuesday the calendar calls a Thursday, is a misread that happened
 *  to pass parity — and it costs nothing on a real minute, where they always agree. Ported from
 *  madpsy/ubersdr-ntp (GPL-3.0-or-later), Dcf77Decoder.cpp / MsfDecoder.cpp. */
bool dateOk(int year, int month, int day, int isoWday) {
    if (month < 1 || month > 12 || day < 1 || day > daysIn(year, month)) return false;
    return isoWday == 0 || isoWeekday(year, month, day) == isoWday;
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

/** ★★ How long the progress line keeps holding a frame to the last locked minute (2026-10-05).
 *  Five minutes: long enough to ride out a fade of a minute or two (which is exactly when a
 *  half-read frame shows nonsense), short enough that a lock lost for good stops judging. */
constexpr double kExpectTtlS = 300.0;

// ── The envelope history (2026-10-05) ──
// ★ 200 envelope samples a second (5 ms): finer than any edge this decoder times, coarse enough
//   that 3 s of percentiles is 600 values. Ring of ~20 s.
constexpr int    kEnvHz   = 200;
constexpr size_t kEnvRing = 4096;
constexpr int    kPctWin  = 600;     ///< 3 s of envelope for the percentiles
constexpr double kClipX   = 3.0;     ///< |x| past this many times the "on" level is an impulse

}  // namespace

TimeDecoder::TimeDecoder(int sampleRate, Station station)
    : sr_(sampleRate > 0 ? sampleRate : 48000), station_(station) {
    decim_ = std::max(1, sr_ / kEnvHz);
    env_.assign(kEnvRing, 0.0f);
    if (station_ != Station::RWM) initReader();
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

/** One input sample's worth of envelope bookkeeping: every decim_ samples the envelope is kept
 *  (200 per second), and every 100 ms the 3 s percentiles are re-read from those. */
bool TimeDecoder::pushEnvelope() {
    if (++decimCount_ < decim_) return false;
    decimCount_ = 0;
    env_[(size_t)(envCount_ % kEnvRing)] = (float)envFast_;
    envCount_++;
    if (envCount_ % (kEnvHz / 10) != 0) return true;
    const long long n = std::min<long long>(kPctWin, envCount_);
    if (n < kEnvHz / 4) return true;
    pctScratch_.resize((size_t)n);
    for (long long i = 0; i < n; i++) pctScratch_[(size_t)i] = env_[(size_t)((envCount_ - n + i) % kEnvRing)];
    const size_t k05 = (size_t)(0.05 * (double)n), k90 = (size_t)(0.90 * (double)n);
    std::nth_element(pctScratch_.begin(), pctScratch_.begin() + k05, pctScratch_.end());
    pLo_ = pctScratch_[k05];
    std::nth_element(pctScratch_.begin(), pctScratch_.begin() + k90, pctScratch_.end());
    pHi_ = pctScratch_[k90];
    pctReady_ = true;
    // ★ The carrier-to-dip ratio as the reader sees it — the "carrier" figure on the state line.
    const double d = pLo_ > 1.0 ? pLo_ : 1.0;
    snrDb_ = 20.0 * std::log10((pHi_ > d ? pHi_ : d) / d);
    return true;
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
        double x = std::fabs(raw);
        clock_ += 1;
        // ★★ A LIGHTNING CRASH IS CLIPPED BEFORE IT REACHES THE ENVELOPE (2026-10-05). A rectified
        //    sine never exceeds ~1.6x its own average and a carrier-plus-noise rarely 3x, so
        //    anything past 3x the "on" level is an impulse — and unclipped, a 5 ms crash at 30x
        //    the carrier lifts the envelope above the carrier for ~50 ms, which inside a dip reads
        //    as the carrier coming back. RWM keeps the old path (its Morse timer is tuned to it).
        if (station_ != Station::RWM) {
            if (pHi_ > 0.0 && x > kClipX * pHi_) x = kClipX * pHi_;
            envFast_ += aFast * (x - envFast_);
            if (pushEnvelope()) readerStep();
            continue;
        }
        envFast_ += aFast * (x - envFast_);
        envSlow_ += aSlow * (x - envSlow_);

        // ── RWM: the edge-reactive path, unchanged ──────────────────────────────────────────
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

        // ★★★ CAPTURE THE GAP WHEN THE DIP BEGINS, NOT WHEN IT ENDS — a gap read at the dip's end
        //     after being zeroed at its start was always ~0.
        if (!wasDip && inDip_) {
            gapBeforeMs_ = gapSamples_ * 1000.0 / sr_;
            gapSamples_ = 0;
        }
        // A rising edge ends a dip: measure it and act.
        if (wasDip && !inDip_) {
            const double dipMs = dipSamples_ * 1000.0 / sr_;
            dipSamples_ = 0;
            // ★★ RWM's callsign, off the SAME envelope. In CW the ID keys the carrier ON, so a
            //    MARK is a gap between dips and the dip we have just measured is the SILENCE
            //    after it — which is exactly the pair the Morse timer needs.
            morseMark(gapBeforeMs_); morseGap(dipMs);
            onRwmEdge();
        }

        // ★ No carrier at all: say NoSignal rather than sitting in Reading with a stale time on
        //   screen.
        if (snrDb_ < 3.0 && state_ != State::NoSignal) {
            second_ = -1;
            setState(State::NoSignal);
        }
    }
}

/** ★★★ RWM SENDS NO TIMECODE, so there is nothing here to decode into a clock and this
 *  deliberately never produces one. What it can honestly report is that the station is being
 *  heard and that its second markers are being counted — which is what RWM is actually for:
 *  calibration and propagation. Anything more would be invented. */
void TimeDecoder::onRwmEdge() {
    if (snrDb_ < 3.0) return;
    if (state_ == State::NoSignal) setState(State::Searching);
    if (second_ < 0) { second_ = 0; setState(State::Reading); }
    else second_ = (second_ + 1) % 60;
    if (onBit) onBit(second_, 1);
    // ★ Still report progress: RWM has no minute to decode, but the host uses this tick to
    //   flush any callsign heard — and a panel with no heartbeat looks dead.
    emitPartial();
}

// ═════════════════════════════════════════════════════════════════════════════════════════════
// ★★★ THE MATCHED-FILTER READER (2026-10-05) — MSF, DCF77, WWVB, WWV.
//
// What it replaced: a dip was measured edge to edge and its length matched against ±35-50 ms
// windows. Every ingredient of that is fragile on a marginal signal — one noise crossing inside a
// dip splits it in two, one crash merges two, and the second was found only by seeing an edge.
// The design below is the one madpsy/ubersdr-ntp (GPL-3.0-or-later; decoders from
// madpsy/ubersdr-clock) uses for WWVB/DCF77/MSF/WWV, adapted to this decoder's framing and
// checks — WwvbDecoder.cpp trackEdge()/findFallingEdgeNear()/classifyAndAdvance():
//
//  ★★ A ONE-SECOND FLYWHEEL. The stations' second edges are quartz, exactly one second apart, so
//     once the phase is known the NEXT edge is predicted, searched for only ±40 ms around the
//     prediction, and COASTED through when it is not there. A faded second is still a second —
//     it can no longer shift the frame, and a noise edge mid-second is never even looked at.
//     An alpha-beta tracker follows a receiver sample clock that is not quite 48 kHz.
//  ★★ EACH SECOND READ WHOLE, AGAINST EVERY SYMBOL IT COULD BE. The envelope over the second is
//     normalised between the 5th and 90th percentiles (0 = carrier down, 1 = up, clipped) and
//     compared with each symbol's template (squared error; with clipping, the decision is a
//     vote of every 5 ms slot where two symbols differ). The CONFIDENCE is the margin of the
//     best over the runner-up, per slot where they differ: 1 = clean, 0 = a coin toss. A slot
//     corrupted by a crash costs one slot's vote, not the symbol.
//  ★ The phase is found from a histogram of falling edges folded modulo one second — one noise
//    edge never seeds it, and a seed is held only while edges keep arriving there.
// ═════════════════════════════════════════════════════════════════════════════════════════════
namespace {
enum ClsKind : signed char { kData = 0, kMark = 1, kNone = 2 };
struct ClsDef { ClsKind kind; signed char a, b; int nlo; double lo[2][2]; };
// ★ Each symbol as the intervals (ms from the second's edge) in which the envelope is LOW.
//   WWV is read INVERTED — its symbol is the pulse of subcarrier, so "low" there means "pulse".
const ClsDef kMsfCls[]  = { {kData,0,0,1,{{0,100},{0,0}}}, {kData,1,0,1,{{0,200},{0,0}}},
                            {kData,1,1,1,{{0,300},{0,0}}}, {kData,0,1,2,{{0,100},{200,300}}},
                            {kMark,0,0,1,{{0,500},{0,0}}}, {kNone,0,0,0,{{0,0},{0,0}}} };
const ClsDef kDcfCls[]  = { {kData,0,0,1,{{0,100},{0,0}}}, {kData,1,0,1,{{0,200},{0,0}}},
                            {kNone,0,0,0,{{0,0},{0,0}}} };
const ClsDef kWwvbCls[] = { {kData,0,0,1,{{0,200},{0,0}}}, {kData,1,0,1,{{0,500},{0,0}}},
                            {kMark,0,0,1,{{0,800},{0,0}}}, {kNone,0,0,0,{{0,0},{0,0}}} };
// WWV: measured from the pulse's START, which is 30 ms after the second's tick.
const ClsDef kWwvCls[]  = { {kData,0,0,1,{{0,170},{0,0}}}, {kData,1,0,1,{{0,470},{0,0}}},
                            {kMark,0,0,1,{{0,770},{0,0}}}, {kNone,0,0,0,{{0,0},{0,0}}} };

constexpr double kMinContrast = 1.4;   ///< p90 / p05 below this: no dip to read (≈ 2.9 dB)
constexpr int    kEdgeTol     = 8;     ///< ±40 ms searched around the predicted edge
constexpr int    kScanLag     = 300;   ///< 1.5 s: edges are judged once the percentiles span them
constexpr double kHistDecay   = 0.95;  ///< per second — a stale phase fades in ~20 s
constexpr double kSeedScore   = 3.0;   ///< ~4 aligned edges to seed the phase
constexpr double kTrkAlpha    = 1.0 / 8.0, kTrkBeta = 1.0 / 128.0;
constexpr int    kTrkWarm     = 8;
/** ★★ A symbol at least this clear of its runner-up is READ; below it the second is an erasure.
 *  0.4 puts the 0/1 boundary where the old duration windows had it (WWV: a pulse of 260 ms or
 *  less is a 0, 380 or more a 1 — the old windows said 260 and 360). */
constexpr float  kReadConf    = 0.40f;
/** A marker / hole this clear is believed for FRAMING (anchoring a minute). */
constexpr float  kSyncConf    = 0.50f;
}  // namespace

void TimeDecoder::initReader() {
    const ClsDef* t = kMsfCls; int n = 6, winMs = 550;
    switch (station_) {
        case Station::DCF77: t = kDcfCls;  n = 3; winMs = 250; break;
        case Station::WWVB:  t = kWwvbCls; n = 4; winMs = 830; break;
        case Station::WWV:   t = kWwvCls;  n = 4; winMs = 830; break;
        default: break;
    }
    // ★ The window stops just after the longest symbol ends — so MSF's second 59 is read 550 ms
    //   in, WWVB's 830 ms in: each minute is still decoded BEFORE the next minute begins.
    nCls_ = n;
    win_ = winMs * kEnvHz / 1000;
    period_ = periodNom_ = (double)sr_ / (double)decim_;
    for (int c = 0; c < n; c++) {
        clsKind_[c] = (signed char)t[c].kind; clsA_[c] = t[c].a; clsB_[c] = t[c].b;
        for (int k = 0; k < win_; k++) {
            const double ms = (k + 0.5) * 1000.0 / kEnvHz;
            bool low = false;
            for (int j = 0; j < t[c].nlo; j++) if (ms >= t[c].lo[j][0] && ms < t[c].lo[j][1]) low = true;
            tpl_[c][k] = low ? 0 : 1;
        }
    }
    for (int a = 0; a < n; a++) for (int b = 0; b < n; b++) {
        int d = 0; for (int k = 0; k < win_; k++) d += tpl_[a][k] != tpl_[b][k];
        nd_[a][b] = d > 0 ? d : 1;
    }
}

float TimeDecoder::envAt(long long i) const {
    if (i < 0 || i >= envCount_ || envCount_ - i > (long long)kEnvRing) return 0.0f;
    return env_[(size_t)(i % kEnvRing)];
}

/** Normalised envelope at a fractional position: 0 = carrier down, 1 = up (WWV: 1 = no pulse). */
double TimeDecoder::zAt(double pos) const {
    const long long i0 = (long long)std::floor(pos);
    const double f = pos - (double)i0;
    const double e = envAt(i0) * (1.0 - f) + envAt(i0 + 1) * f;
    const double span = pHi_ - pLo_;
    double z = span > 1e-9 ? (e - pLo_) / span : 0.5;
    return station_ == Station::WWV ? 1.0 - z : z;
}

bool TimeDecoder::contrastOk() const {
    return pctReady_ && pHi_ >= kMinContrast * std::max(pLo_, 1e-9);
}

/** A falling edge between i-1 and i: high before, held low after. `pos` = the 0.5 crossing. */
bool TimeDecoder::isEdge(long long i, double& pos) const {
    const double z1 = zAt((double)i - 1), z0 = zAt((double)i);
    if (!(z1 >= 0.5 && z0 < 0.5)) return false;
    int high = 0, low = 0;
    for (int d = 2; d <= 6; d++) if (zAt((double)(i - d)) >= 0.6) high++;
    if (high < 3) return false;
    for (int d = 0; d <= 7; d++) if (zAt((double)(i + d)) < 0.5) low++;
    if (low < 6) return false;
    pos = (double)(i - 1) + (z1 - 0.5) / (z1 - z0);
    return true;
}

int TimeDecoder::phaseBin(double pos) const {
    double ph = std::fmod(pos, periodNom_);
    if (ph < 0) ph += periodNom_;
    int b = (int)(ph * kEnvHz / periodNom_);
    return b >= kEnvHz ? kEnvHz - 1 : b;
}

double TimeDecoder::histTriple(int b) const {
    return hist_[(b + kEnvHz - 1) % kEnvHz] + hist_[b] + hist_[(b + 1) % kEnvHz];
}

/** The best-supported phase bin, and whether it stands clear of every other phase. */
int TimeDecoder::histBest(double& score, bool& clear) const {
    int best = 0; score = -1;
    for (int b = 0; b < kEnvHz; b++) { const double s = histTriple(b); if (s > score) { score = s; best = b; } }
    double other = 0;
    for (int b = 0; b < kEnvHz; b++) {
        int d = std::abs(b - best); d = std::min(d, kEnvHz - d);
        if (d > 10) other = std::max(other, histTriple(b));
    }
    clear = score >= 2.0 * other;
    return best;
}

void TimeDecoder::seedAt(double edgePos) {
    // ★★ BACK-FILL FROM THE RING: the edges that built the histogram are still in it, so the
    //    seconds before the seed are read too — the first minute is not lost to acquisition.
    const long long first = std::max<long long>(0, envCount_ - (long long)kEnvRing) + 8;
    int back = (int)std::floor((edgePos - (double)first) / period_);
    back = std::max(0, std::min(back, 15));
    phaseKnown_ = true;
    curEdge_ = edgePos - (double)(back + 1) * period_;   // stage 1 searches near edgePos - back*period
    period_ = periodNom_;
    trkN_ = 0;
    missRun_ = 0;
    stage_ = 1;
}

/** ★ A new envelope sample has arrived: look for edges (for the phase), then read every second
 *  whose window is now complete. */
void TimeDecoder::readerStep() {
    // ── the "no carrier" floor ──
    if (envCount_ % (kEnvHz / 10) == 0 && pctReady_) {
        if (!contrastOk()) {
            lowSnrS_ += 0.1;
            if (state_ != State::NoSignal) setState(State::NoSignal);
            // ★ Two minutes with nothing to read: whatever was locked is not any more.
            if (lowSnrS_ > 120.0 && (phaseKnown_ || anchorIdx_ >= 0)) {
                loseFrame(); phaseKnown_ = false; expectClock_ = 0;
                for (double& h : hist_) h = 0;
            }
        } else {
            lowSnrS_ = 0;
            if (state_ == State::NoSignal) setState(anchorIdx_ >= 0 ? State::Reading : State::Searching);
        }
    }
    // ── edges into the phase histogram ──
    if (envCount_ % (long long)kEnvHz == 0) for (double& h : hist_) h *= kHistDecay;
    const long long scanTo = envCount_ - kScanLag;
    if (scanPos_ < scanTo - 2 * kEnvHz) scanPos_ = scanTo - 2 * kEnvHz;
    while (scanPos_ < scanTo) {
        const long long i = scanPos_++;
        double pos;
        if (i < 8 || !contrastOk() || !isEdge(i, pos)) continue;
        const int b = phaseBin(pos);
        hist_[b] += 1.0;
        scanPos_ = i + 10;                       // one edge per 50 ms
        if (!phaseKnown_) {
            double score; bool clear;
            const int best = histBest(score, clear);
            int d = std::abs(best - b); d = std::min(d, kEnvHz - d);
            if (score >= kSeedScore && clear && d <= 1) seedAt(pos);
        }
    }
    // ── the flywheel ──
    while (phaseKnown_) {
        if (stage_ == 0) {
            if ((double)envCount_ < curEdge_ + win_ + 2) break;
            SecRec r;
            classify(r);
            r.idx = secIdx_++;
            recs_[(size_t)(r.idx & (kRecRing - 1))] = r;
            onSecond(r);
            stage_ = 1;
        } else {
            const double pred = curEdge_ + period_;
            if ((double)envCount_ < pred + kEdgeTol + 10) break;
            double best = 0, bestD = 1e9; bool found = false;
            if (contrastOk())
                for (long long i = (long long)std::floor(pred) - kEdgeTol; i <= (long long)std::ceil(pred) + kEdgeTol; i++) {
                    double pos;
                    if (i < 8 || !isEdge(i, pos)) continue;
                    const double dd = std::fabs(pos - pred);
                    if (dd < bestD && dd <= kEdgeTol) { bestD = dd; best = pos; found = true; }
                }
            if (found) {
                const double r = best - pred;
                trkN_++;
                curEdge_ = pred + std::max(1.0 / trkN_, kTrkAlpha) * r;
                if (trkN_ > kTrkWarm)
                    period_ = std::min(periodNom_ * (1 + 3e-3), std::max(periodNom_ * (1 - 3e-3), period_ + kTrkBeta * r));
                missRun_ = 0;
            } else {
                curEdge_ = pred;
                missRun_++;
                // ★ Edges have stopped arriving where the flywheel expects them. If the histogram
                //   now holds a clear phase ELSEWHERE, the stream jumped (a re-tune, a dropped
                //   buffer): re-seed there — and the minute framing goes with it.
                if (missRun_ >= 6) {
                    double score; bool clear;
                    const int hb = histBest(score, clear);
                    int d = std::abs(hb - phaseBin(curEdge_)); d = std::min(d, kEnvHz - d);
                    if (score >= kSeedScore && clear && d > 6) {
                        loseFrame();
                        phaseKnown_ = false;
                        break;
                    }
                }
            }
            stage_ = 0;
        }
    }
}

/** ★★ Read one second against every symbol it could be (see the block comment above). */
void TimeDecoder::classify(SecRec& r) const {
    r.contrast = contrastOk();
    float err[kMaxCls];
    for (int c = 0; c < nCls_; c++) err[c] = 0;
    for (int k = 0; k < win_; k++) {
        double z = zAt(curEdge_ + k + 0.5);
        z = z < 0 ? 0 : z > 1 ? 1 : z;
        for (int c = 0; c < nCls_; c++) { const double d = z - tpl_[c][k]; err[c] += (float)(d * d); }
    }
    auto margin = [&](int a, int b) { return (err[b] - err[a]) / (float)nd_[a][b]; };
    int best = 0;
    for (int c = 1; c < nCls_; c++) if (err[c] < err[best]) best = c;
    int run = -1;
    for (int c = 0; c < nCls_; c++) if (c != best && (run < 0 || err[c] < err[run])) run = c;
    int dBest = -1, dRun = -1;
    for (int c = 0; c < nCls_; c++) if (clsKind_[c] == kData && (dBest < 0 || err[c] < err[dBest])) dBest = c;
    for (int c = 0; c < nCls_; c++) if (clsKind_[c] == kData && c != dBest && (dRun < 0 || err[c] < err[dRun])) dRun = c;
    r.cls = (signed char)best;
    r.conf = r.contrast ? margin(best, run) : 0.0f;
    r.dcls = (signed char)dBest;
    r.dconf = r.contrast ? margin(dBest, dRun) : 0.0f;
    // Soft bits: the best symbol with the bit at 0 against the best with it at 1, in [-1, 1].
    auto soft = [&](bool isB) {
        int z0 = -1, z1 = -1;
        for (int c = 0; c < nCls_; c++) {
            if (clsKind_[c] != kData) continue;
            const int v = isB ? clsB_[c] : clsA_[c];
            int& s = v ? z1 : z0;
            if (s < 0 || err[c] < err[s]) s = c;
        }
        if (z0 < 0 || z1 < 0) return 0.0f;
        return (err[z0] - err[z1]) / (float)nd_[z0][z1];
    };
    r.softA = r.contrast ? soft(false) : 0.0f;
    r.softB = (r.contrast && station_ == Station::MSF) ? soft(true) : 0.0f;
    // ★ A second that is plainly a marker or a missing dip carries no data bit, whatever the
    //   nearest data symbol would have been.
    if (clsKind_[best] != kData && margin(best, dBest) >= kSyncConf) { r.dconf = 0; r.softA = r.softB = 0; }
}

const TimeDecoder::SecRec& TimeDecoder::rec(long long idx) const {
    static const SecRec kNoRec{};
    if (idx < 0) return kNoRec;
    const SecRec& r = recs_[(size_t)(idx & (kRecRing - 1))];
    return r.idx == idx ? r : kNoRec;
}

bool TimeDecoder::strongKind(const SecRec& r, int kind) const {
    return r.idx >= 0 && r.contrast && r.cls >= 0 && clsKind_[r.cls] == kind && r.conf >= kSyncConf;
}

/** The symbol a second contributes to the frame: 2 = marker, 0/1 = data, -1 = unreadable. */
int TimeDecoder::symbolOf(const SecRec& r, bool& readable) const {
    readable = false;
    if (!r.contrast) return -1;
    if (clsKind_[r.cls] == kMark && r.conf >= kReadConf) { readable = true; return 2; }
    if (r.dcls >= 0 && r.dconf >= kReadConf) { readable = true; return clsA_[r.dcls]; }
    return -1;
}

void TimeDecoder::placeRec(int pos, const SecRec& r) {
    bool readable;
    const int sym = symbolOf(r, readable);
    int a = 0, b = 0;
    if (sym >= 0 && sym != 2) { a = clsA_[r.dcls]; b = clsB_[r.dcls]; }
    // MSF and DCF77 have no marker inside the minute: a marker there is an unreadable second.
    if (sym == 2 && (station_ == Station::MSF || station_ == Station::DCF77)) readable = false;
    place(pos, a, b, sym, readable);
    second_ = pos;
    if (onBit) onBit(second_, bitsA_[second_]);
    emitPartial();
}

/**
 * ★★★ ONE SECOND HAS BEEN READ; WHERE IS IT IN THE MINUTE?
 *
 * The flywheel guarantees exactly one read per second, so once a minute is anchored its seconds
 * are COUNTED — and a count of flywheel seconds cannot be shifted by a lost or an extra dip the
 * way a count of dips was (audit 2026-10-04 row 9). What each station anchors on:
 *  • MSF   — the 500 ms marker IS second 0;
 *  • DCF77 — second 59 carries NO dip; the dip after the missing one is second 0;
 *  • WWVB  — two markers in a row: second 59's P0, then second 0's Pr (SP 432 p. 21);
 *  • WWV   — second 59's marker, then a second with NO pulse (the hole) = second 0.
 * ★ The newest MSF marker always wins, as before; the other stations re-anchor only at a whole
 *   minute's distance (±2 s) from the last anchor, or once the anchor has gone two minutes
 *   unconfirmed — a fade looks exactly like DCF77's missing dip and WWV's hole.
 */
void TimeDecoder::onSecond(const SecRec& r) {
    if (state_ == State::NoSignal && r.contrast) setState(anchorIdx_ >= 0 ? State::Reading : State::Searching);
    const SecRec& p1 = rec(r.idx - 1);
    const long long pos = anchorIdx_ >= 0 ? r.idx - anchorIdx_ : -1;
    switch (station_) {
    case Station::MSF: {
        if (strongKind(r, kMark)) {
            if (anchorIdx_ >= 0 && !frameClosed_) closeFrame();
            startFrame(r.idx);
            unconfirmed_ = 0;
            if (onBit) onBit(0, 0);
            emitPartial();
            return;
        }
        if (anchorIdx_ < 0) return;                                // hunting for the marker
        long long p = pos;
        if (p >= 60) {
            // ★ Second 0 with no marker: the marker itself faded. The flywheel kept the count, so
            //   the grid carries on into the next minute — three minutes without one is lost lock.
            if (!frameClosed_) closeFrame();
            if (++unconfirmed_ >= 3) { loseFrame(); return; }
            startFrame(anchorIdx_ + 60);
            p -= 60;
            if (p == 0) { if (onBit) onBit(0, 0); emitPartial(); return; }
        }
        placeRec((int)p, r);
        if (p == 59) closeFrame();
        return;
    }
    case Station::DCF77: {
        if (anchorIdx_ < 0) {
            // ★★★ THE MINUTE IS MARKED BY AN ABSENCE: second 59 carries NO dip, so a missing dip
            //     followed by a dip makes the dip second 0.
            if (strongKind(p1, kNone) && r.contrast && r.dcls >= 0 && r.dconf >= kSyncConf
                && clsKind_[r.cls] == kData) {
                startFrame(r.idx);
                unconfirmed_ = 0;
                placeRec(0, r);
            }
            return;
        }
        long long p = pos;
        if (p == 59) {
            // ★ A DIP at second 59 is something DCF77 never sends, so the anchor was a missed dip,
            //   not the minute — drop it and hunt again.
            if (r.contrast && clsKind_[r.cls] == kData && r.conf >= kSyncConf) { loseFrame(); return; }
            if (strongKind(r, kNone)) unconfirmed_ = 0;
            second_ = 59;
            return;
        }
        if (p >= 60) {
            if (!frameClosed_) closeFrame();
            if (++unconfirmed_ >= 3) { loseFrame(); return; }
            startFrame(anchorIdx_ + 60);
            p -= 60;
        }
        placeRec((int)p, r);
        if (p == 58) closeFrame();
        return;
    }
    case Station::WWVB: {
        if (anchorIdx_ < 0) {
            if (strongKind(p1, kMark) && strongKind(r, kMark)) {
                startFrame(r.idx);
                unconfirmed_ = 0;
                placeRec(0, r);
            }
            return;
        }
        long long p = pos;
        if (p >= 60) {
            if (!frameClosed_) closeFrame();
            // ★ A readable NON-marker where second 0 must be means the anchor was wrong (a noise
            //   marker beside a real one). Hunt again rather than read a minute out of phase.
            if (r.contrast && clsKind_[r.cls] == kData && r.conf >= kSyncConf) { loseFrame(); return; }
            if (strongKind(r, kMark)) unconfirmed_ = 0;
            else if (++unconfirmed_ >= 3) { loseFrame(); return; }
            startFrame(anchorIdx_ + 60);
            p -= 60;
        }
        placeRec((int)p, r);
        if (p == 59) closeFrame();
        return;
    }
    case Station::WWV: {
        const bool holeAfterMarker = strongKind(p1, kMark) && strongKind(r, kNone);
        if (anchorIdx_ < 0) {
            if (holeAfterMarker) { startFrame(r.idx); unconfirmed_ = 0; second_ = 0; }
            return;
        }
        long long p = pos;
        // ★★★ A FADE LOOKS LIKE A HOLE. A marker-then-hole re-anchors only a whole minute (±2 s)
        //     after the last anchor, or once the anchor has gone unconfirmed for two minutes
        //     (rule from madpsy/ubersdr-ntp, GPL-3.0-or-later, WwvDecoder.cpp tryAnchor).
        const long long m = ((p % 60) + 60) % 60;
        if (holeAfterMarker && p % 60 != 0 && (m <= 2 || m >= 58 || unconfirmed_ >= 2)) {
            if (!frameClosed_) closeFrame();
            startFrame(r.idx);
            unconfirmed_ = 0; second_ = 0;
            return;
        }
        if (p >= 60) {
            // ★ The hole of the next minute: the frame just read is decoded HERE, as before — its
            //   time is the minute that began at its own second 0, so the minute in progress now
            //   is that plus one (SP 432 p. 46; audit 2026-10-04 row 8).
            if (!frameClosed_) closeFrame();
            if (holeAfterMarker) unconfirmed_ = 0; else unconfirmed_++;
            startFrame(anchorIdx_ + 60);
            second_ = 0;
            p -= 60;
            if (p == 0) return;
        }
        if (p == 0) return;
        placeRec((int)p, r);
        return;
    }
    default: return;
    }
}

// ── Framing ──────────────────────────────────────────────────────────────────────────────────
static inline bool inMinute(int s) { return s >= 0 && s < 60; }

void TimeDecoder::startFrame(long long idx) {
    anchorIdx_ = idx;
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
    anchorIdx_ = -1;
    frameClosed_ = true;
    second_ = -1;
    lastStamp_ = 0;
    setState(State::Searching);
}

/** Decode the minute in hand, once. ★★ An erasure anywhere the station sends code fails it
 *  (WWV excepted: it decodes over a dropped pulse, as it always has — see decodeWwv). */
void TimeDecoder::closeFrame() {
    if (frameClosed_) return;
    frameClosed_ = true;
    TimeStamp ts;
    bool ok = false;
    switch (station_) {
        case Station::MSF:   ok = slotsComplete(1, 59) && decodeMsf(ts);   break;
        case Station::DCF77: ok = slotsComplete(0, 58) && decodeDcf77(ts); break;
        case Station::WWV:   ok = decodeWwv(ts); break;
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
    // ★★ WHAT THE NEXT FRAME MUST SAY — for the progress line, which shows fields before their
    //    parity arrives (see emitPartial). Each station's convention, as its decode reports it:
    //    MSF and DCF77 frames describe the minute BEGINNING at their end, so `ts` is this frame's
    //    raw value and the next frame reads ts + 1. WWV and WWVB frames describe the minute that
    //    began at their own second 0 and the decode has already added one, so `ts` IS the next
    //    frame's raw value. Only a CORROBORATED minute sets it: a lone parity pass is wrong one
    //    time in sixteen, and would then mark a whole correct frame "??".
    expectNext_ = ts;
    if (station_ == Station::MSF || station_ == Station::DCF77) addMinute(expectNext_);
    expectClock_ = clock_ > 0 ? clock_ : 1;
    good_++;
    setState(State::Locked);
    if (onTime) onTime(ts);
}

bool TimeDecoder::expectedNow(TimeStamp& e) const {
    if (expectClock_ == 0 || second_ < 0) return false;
    const double el = (double)(clock_ - expectClock_) / sr_;
    if (el < 0.0 || el > kExpectTtlS) return false;
    // ★ Whole minutes since the locked one, measured from where THIS frame began: every station
    //   decodes within ~2 s of its frame's end, so (elapsed − second) is a whole minute ± 2 s.
    long k = std::lround((el - (double)second_) / 60.0);
    if (k < 0) k = 0;
    e = expectNext_;
    for (long i = 0; i < k; i++) addMinute(e);
    return true;
}

/**
 * What is known this far into the minute.
 *
 * ★★ EACH FIELD BECOMES MEANINGFUL AT A DIFFERENT SECOND, because each station sends them in its
 *    own order — MSF puts the year first and the minute last, DCF77 the minute first and the year
 *    last. So this is per-station, and reporting a field before its last bit has arrived would
 *    show a number that is briefly, confidently wrong.
 * ★★★ AND A FIELD THAT HAS ARRIVED IS STILL UNCHECKED (2026-10-05). MSF's year is complete at
 *     second 24 and its parity at 54; one flipped bit showed "2014" for half a minute, in a line
 *     shaped exactly like a confident time. Each field is now DOUBTED (xxxBad, drawn "??") when
 *     it is not a possible value, or when a recent lock says what this frame must read and it
 *     disagrees. The full decode is untouched — this only stops the line asserting a misread.
 *     ★ Twice a year (MSF/DCF77 send LOCAL time) the hour jumps at the clock change and is shown
 *       "??" for that one frame; the locked line still reads it correctly.
 */
void TimeDecoder::emitPartial() {
    if (!onPartial || second_ < 0) return;
    Partial p;
    p.second = second_;
    const int* A = bitsA_;
    // A field that is complete: its value, and whether its digits are digits.
    auto take = [&](const BcdField& f, bool& ready, bool& bad, int& v, int add = 0) {
        if (second_ < fieldDone(f)) return;
        ready = true; v = readField(A, f) + add; bad = !digitsOk(A, f);
    };
    // WWV/WWVB: the date is a day-of-year, convertible only once the year is in and sane.
    auto dateFromDoy = [&](const BcdField& doyF) {
        if (!p.year) return;
        p.month = p.day = true;
        if (p.yearBad || !digitsOk(A, doyF) || !doyToDate(p.t.year, readField(A, doyF), p.t.month, p.t.day))
            p.monthBad = p.dayBad = true;
    };
    switch (station_) {
        case Station::MSF:
            take(kMsfYear,   p.year,   p.yearBad,   p.t.year, 2000);
            take(kMsfMonth,  p.month,  p.monthBad,  p.t.month);
            take(kMsfDay,    p.day,    p.dayBad,    p.t.day);
            if (second_ >= fieldDone(kMsfWeekday)) {
                const int wd = readField(A, kMsfWeekday);
                p.weekday = true;
                p.weekdayBad = wd > 6;                  // ★ MSF sends 0-6; 7 is a misread
                p.t.weekday = wd == 0 ? 7 : wd;
            }
            take(kMsfHour,   p.hour,   p.hourBad,   p.t.hour);
            take(kMsfMin, p.minute, p.minuteBad, p.t.minute);
            break;
        case Station::DCF77:
            take(kDcfMinute, p.minute, p.minuteBad, p.t.minute);
            take(kDcfHour,   p.hour,   p.hourBad,   p.t.hour);
            take(kDcfDay,    p.day,    p.dayBad,    p.t.day);
            if (second_ >= fieldDone(kDcfWeekday)) {
                p.weekday = true; p.t.weekday = readField(A, kDcfWeekday);
                p.weekdayBad = p.t.weekday < 1;         // DCF77 sends 1-7
            }
            take(kDcfMonth,  p.month,  p.monthBad,  p.t.month);
            take(kDcfYear,   p.year,   p.yearBad,   p.t.year, 2000);
            break;
        case Station::WWV: {
            // ★★★ THE SAME TABLE decodeWwv READS (audit 2026-10-04 row 6) — this used the pre-
            //     2026-08-12 map, minute at 1-8 and hour at 10-16, so the line showed the MINUTE's
            //     bits as the hour. These are the transmitted fields: the time at the START of
            //     this frame, which is the minute in progress (SP 432 p. 46).
            take(kWwvMinute, p.minute, p.minuteBad, p.t.minute);
            take(kWwvHour,   p.hour,   p.hourBad,   p.t.hour);
            // ★ The day needs the year (for leap), and the year's tens digit is the LAST field to
            //   arrive (second 54) — so date and year appear together, once both halves are in.
            if (second_ >= fieldDone(kWwvYearTens)) {
                p.year = true;
                p.t.year = 2000 + readField(A, kWwvYearTens) * 10 + readField(A, kWwvYearUnits);
                p.yearBad = !digitsOk(A, kWwvYearUnits) || !digitsOk(A, kWwvYearTens);
                dateFromDoy(kWwvDoy);
            }
            break;
        }
        case Station::WWVB: {
            take(kWwvbMinute, p.minute, p.minuteBad, p.t.minute);
            take(kWwvbHour,   p.hour,   p.hourBad,   p.t.hour);
            take(kWwvbYear,   p.year,   p.yearBad,   p.t.year, 2000);
            if (p.year) dateFromDoy(kWwvbDoy);
            break;
        }
        case Station::RWM:
            break;      // nothing to fill in — it carries no timecode
    }
    // ── Ranges, the same ones the full decode applies ──
    if (p.month  && (p.t.month < 1 || p.t.month > 12)) p.monthBad = true;
    if (p.day    && (p.t.day < 1 || p.t.day > 31))     p.dayBad = true;
    if (p.day && p.month && !p.dayBad && !p.monthBad
        && p.t.day > daysIn(p.year && !p.yearBad ? p.t.year : 2000, p.t.month)) p.dayBad = true;
    if (p.hour   && p.t.hour > 23)   p.hourBad = true;
    if (p.minute && p.t.minute > 59) p.minuteBad = true;
    // ── After a lock: what THIS frame must say ──
    TimeStamp e;
    if (station_ != Station::RWM && expectedNow(e)) {
        if (p.year    && p.t.year    != e.year)    p.yearBad = true;
        if (p.month   && p.t.month   != e.month)   p.monthBad = true;
        if (p.day     && p.t.day     != e.day)     p.dayBad = true;
        if (p.hour    && p.t.hour    != e.hour)    p.hourBad = true;
        if (p.minute  && p.t.minute  != e.minute)  p.minuteBad = true;
        if (p.weekday && e.weekday && p.t.weekday != e.weekday) p.weekdayBad = true;
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
        case Station::RWM:  return false;          // no timecode — see onRwmEdge
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
    // ★★ WWVB HAS NO PARITY, so these are the only content checks it gets (2026-10-05):
    //  • every BCD digit ≤ 9 (madpsy/ubersdr-ntp checks this on DCF77/MSF only);
    if (!digitsOk(b, kWwvbMinute) || !digitsOk(b, kWwvbHour) || !digitsOk(b, kWwvbDoy)
        || !digitsOk(b, kWwvbYear)) return false;
    //  • the always-zero seconds. ★ MORE THAN ONE set is refused; ONE is tolerated. A slot only
    //    reaches here READABLE (an unreadable dip already erased and failed the minute), so a 1
    //    in a zero slot is a 200 ms dip measured as ~500 — a lengthened dip, which an LF fade
    //    right after the carrier returns does produce. One such in a reserved second leaves every
    //    data bit as read, and the data is still held to corroboration (two minutes agreeing);
    //    two or more means the dip lengths themselves are not being read, and the minute goes.
    int zeroSet = 0;
    for (int s : kWwvbZeroBits) zeroSet += b[s] ? 1 : 0;
    if (zeroSet > 1) return false;

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
    // ★ Display only (2026-10-05). s57 = DST at 24:00 UTC today, s58 at 00:00 (SP 432 p. 21).
    //   DUT1: sign s36-38 is 1 0 1 for + and 0 1 0 for −; magnitude s40-43 = 0.8/0.4/0.2/0.1 s.
    //   Any other sign pattern is a misread and is not shown. (Map as madpsy/ubersdr-ntp,
    //   GPL-3.0-or-later, WwvbDecoder.cpp decodeFrame.)
    out.hasDst2 = true;
    out.dst2 = b[57] != 0;
    {
        const bool plus = b[36] && !b[37] && b[38], minus = !b[36] && b[37] && !b[38];
        const int mag = (b[40] ? 8 : 0) + (b[41] ? 4 : 0) + (b[42] ? 2 : 0) + (b[43] ? 1 : 0);
        out.dut1Known = (plus || minus) && mag <= 9;
        out.dut1Tenths = minus ? -mag : mag;
    }
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
    // ★★ Every BCD digit ≤ 9 (2026-10-05) — WWV has no parity, so this and the calendar are all
    //    the content checking it gets. (The year's two digits are checked where it is read.)
    if (!digitsOk(b, kWwvMinute) || !digitsOk(b, kWwvHour) || !digitsOk(b, kWwvDoy)) return false;

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
    // ★ Display only (2026-10-05): DST2 (s55 — DST at 24:00 UTC today) and DUT1 (sign s50, 1 = +;
    //   magnitude s56/57/58 = 0.1/0.2/0.4 s; SP 432 p. 48; map as madpsy/ubersdr-ntp,
    //   GPL-3.0-or-later, WwvDecoder.cpp). WWV keeps decoding over a dropped pulse and records it
    //   as 0, so each is shown only when every second it needs actually arrived readable.
    out.hasDst2 = slot_[kWwvDst2] == 1;
    out.dst2 = out.hasDst2 && b[kWwvDst2] != 0;
    out.dut1Known = slot_[50] == 1 && slot_[56] == 1 && slot_[57] == 1 && slot_[58] == 1;
    {
        const int mag = (b[56] ? 1 : 0) + (b[57] ? 2 : 0) + (b[58] ? 4 : 0);
        out.dut1Tenths = out.dut1Known ? (b[50] ? mag : -mag) : 0;
    }
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

    // ★★★ MSB FIRST. See the kMsf* tables — reading these LSB-first bit-reverses every field and
    //     yields a date that passes every range check while being completely wrong.
    const int year  = readField(A, kMsfYear);
    const int month = readField(A, kMsfMonth);
    const int day   = readField(A, kMsfDay);
    const int wday  = readField(A, kMsfWeekday);
    const int hour  = readField(A, kMsfHour);
    const int min   = readField(A, kMsfMin);

    // Odd parity, each over its own span.
    if (parityOdd(A, 17, 24) == B[54]) return false;
    if (parityOdd(A, 25, 35) == B[55]) return false;
    if (parityOdd(A, 36, 38) == B[56]) return false;
    if (parityOdd(A, 39, 51) == B[57]) return false;

    if (month < 1 || month > 12 || day < 1 || day > 31) return false;
    if (hour > 23 || min > 59) return false;

    // ★★ THE CHECKS PARITY CANNOT MAKE (2026-10-05). Four parity bits pass one wrong minute in
    //    sixteen; these cost a real minute nothing, because a real minute always satisfies them.
    //  • A52-A59 must read 01111110 — NPL's minute identifier. Our A reading at 53-58 is the same
    //    200/300 ms classifier every data bit uses (A=1 there, with B=1 a single 300 ms dip), and
    //    52/59 are the plain 100 ms "A=0" — no less reliable than the date bits, so EXACT, not a
    //    tolerance: one wrong bit there is a misread minute, the same as one wrong bit anywhere.
    for (int j = 0; j < 8; j++) if (A[52 + j] != kMsfIdentifier[j]) return false;
    //  • every BCD digit ≤ 9;
    if (!digitsOk(A, kMsfYear) || !digitsOk(A, kMsfMonth) || !digitsOk(A, kMsfDay)
        || !digitsOk(A, kMsfHour) || !digitsOk(A, kMsfMin)) return false;
    //  • ★★★ THE WEEKDAY IS 0-6 (0 = Sunday). 7 is not a day MSF can send, and this used to map
    //    it — like 0 — to Sunday and accept it.
    if (wday > 6) return false;
    //  • the date exists, and the weekday sent is that date's weekday.
    if (!dateOk(2000 + year, month, day, wday == 0 ? 7 : wday)) return false;

    out.year = 2000 + year;
    out.month = month; out.day = day;
    out.weekday = wday == 0 ? 7 : wday;     // MSF sends Sunday as 0; we report ISO 1..7
    out.hour = hour; out.minute = min;
    out.dst = B[58] != 0;
    out.leapSecondPending = false;
    // ★ DUT1, display only: B1-B8 each +0.1 s, B9-B16 each −0.1 s (NPL). Both signs at once is
    //   a contradiction, so then it is not shown. (Mapping as madpsy/ubersdr-ntp, GPL-3.0-or-later.)
    int pos = 0, neg = 0;
    for (int i = 1; i <= 16; i++) if (B[i]) (i <= 8 ? pos : neg)++;
    out.dut1Known = !(pos && neg);
    out.dut1Tenths = pos ? pos : -neg;
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

    const int min   = readField(b, kDcfMinute);
    const int hour  = readField(b, kDcfHour);
    const int day   = readField(b, kDcfDay);
    const int wday  = readField(b, kDcfWeekday);
    const int month = readField(b, kDcfMonth);
    const int year  = readField(b, kDcfYear);

    if (parityOdd(b, 21, 28) != 0) return false;    // even parity => sum over data+parity is even
    if (parityOdd(b, 29, 35) != 0) return false;
    if (parityOdd(b, 36, 58) != 0) return false;

    if (month < 1 || month > 12 || day < 1 || day > 31) return false;
    if (hour > 23 || min > 59 || wday < 1 || wday > 7) return false;
    // ★★ What three EVEN parity bits cannot catch — any two flips inside one span (2026-10-05):
    //    every BCD digit ≤ 9, the date exists, and the weekday sent is that date's weekday.
    if (!digitsOk(b, kDcfMinute) || !digitsOk(b, kDcfHour) || !digitsOk(b, kDcfDay)
        || !digitsOk(b, kDcfMonth) || !digitsOk(b, kDcfYear)) return false;
    if (!dateOk(2000 + year, month, day, wday)) return false;

    out.year = 2000 + year;
    out.month = month; out.day = day; out.weekday = wday;
    out.hour = hour; out.minute = min;
    out.dst = b[17] != 0;
    out.leapSecondPending = b[19] != 0;
    return true;
}

void TimeDecoder::pushBit(int) {}

}  // namespace vibe
