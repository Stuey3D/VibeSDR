// VibeSDR — LF/HF time-signal decoder (MSF 60 kHz, DCF77 77.5 kHz).
//
// ★★★ WHY THIS IS AN AUDIO DECODER AND NOT AN IQ ONE. Both stations carry their time code as
//     AMPLITUDE: the carrier is switched off (MSF) or reduced to ~15% (DCF77) for a tenth or two
//     of a second, once per second. Tune AM at the carrier and the code is simply the ENVELOPE of
//     the demodulated audio — so this rides the same audio tap FT8, RTTY and WEFAX already use,
//     and needs no new plumbing. AIS, VDL2 and ADS-B are genuinely IQ-shaped; these are not.
//
// ★★ ONE BIT PER SECOND, WHICH CHANGES WHAT "WORKING" MEANS. A minute of clean signal is 59 bits,
//    so there is no averaging away a bad decode: it is right or it waits another minute. Hence the
//    parity checks are not optional decoration — they are the only thing standing between a
//    plausible wrong time and a correct one, and a clock that is confidently wrong is worse than
//    one that says "waiting".
//
// ★ Structured so WWV/WWVH and RWM can join: they are the same shape (a pulse width per second,
//   decoded into BCD) at a different rate and on a subcarrier. See Station.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace vibe {

class TimeDecoder {
public:
    enum class Station {
        MSF,      ///< Anthorn, 60 kHz. Carrier OFF 100 ms at each second; 500 ms marks the minute.
        DCF77,    ///< Mainflingen, 77.5 kHz. Carrier to ~15% for 100 ms (0) or 200 ms (1).
        /** ★★ WWV/WWVH (2.5/5/10/15/20 MHz) — and it is NOT the same job as the two above. The
         *  timecode rides a 100 Hz SUBCARRIER rather than the main carrier's envelope, so it needs
         *  a bandpass first, and the symbol is a PULSE WIDTH: 170 ms = 0, 470 ms = 1, 770 ms = a
         *  position marker. Tune it in AM, not CW. */
        WWV,
        /** ★★★ RWM (Moscow, 4.996/9.996/14.996 MHz) CARRIES NO TIMECODE AT ALL. It sends second
         *  and minute markers and a Morse callsign — there is no BCD date or time to decode, and
         *  a decoder that claimed otherwise would be inventing one. So this station reports LOCK
         *  and the seconds it is counting, and never emits a TimeStamp. Useful for calibration and
         *  for proving propagation; useless as a clock, and it must say so. */
        RWM,
    };

    /** What the decoder is doing, so the UI can be honest rather than blank. */
    enum class State {
        NoSignal,   ///< nothing above the noise — say so rather than showing a stale time
        Searching,  ///< carrier found, hunting for the minute boundary
        Reading,    ///< aligned, collecting a minute's bits
        Locked,     ///< a full minute decoded and parity-checked
    };

    /** A decoded minute. Only ever emitted once every bit of it has passed its parity check. */
    struct TimeStamp {
        int year = 0, month = 0, day = 0;      ///< year is 4-digit
        int hour = 0, minute = 0;
        int weekday = 0;                        ///< 1 = Monday .. 7 = Sunday
        bool dst = false;                       ///< summer time in force at the transmitter
        /** ★ The station's own warning that a leap second is coming — worth surfacing because it
         *  is the one night a year a clock disagrees with everybody for a good reason. */
        bool leapSecondPending = false;
    };

    TimeDecoder(int sampleRate, Station station);

    /** Mono audio, as every other decoder here takes it. */
    void process(const int16_t* samples, int count);

    /** A whole minute, parity-checked. Fires ON the minute boundary it describes. */
    std::function<void(const TimeStamp&)> onTime;
    /** Every second: the bit just read, and which second of the minute it was (-1 = unaligned).
     *  ★ Emitted even while unlocked, because watching bits arrive is how an owner can tell "my
     *    antenna is picking it up but the parity keeps failing" from "there is nothing there". */
    std::function<void(int second, int bit)> onBit;
    std::function<void(State)> onState;
    /** ★★★ FIELDS AS THEY ARRIVE, not a whole minute or nothing.
     *
     *  A minute is 59 bits at one bit per second, so a panel that shows nothing until the end
     *  looks broken for a MINUTE — and if corroboration then withholds the reading, it looks
     *  broken for two. Stuart, 2026-08-11, watching MSF read cleanly and display nothing: "it
     *  looks like it decodes but shows nothing. There is a Time Signal decoder that I've seen
     *  that populates the fields as they are received, so minute then hour then day etc."
     *
     *  ★★ It is also the best DIAGNOSTIC this decoder has. Watching year fill, then month, then
     *     the hour go wrong tells you exactly where the framing slipped — which a single
     *     pass/fail at the end of the minute never could.
     *  ★ `ready` says which fields are complete; anything else in the struct is not yet meaningful
     *    and must be drawn as blank rather than as zero. Fires at most once a second.
     */
    struct Partial {
        TimeStamp t;
        bool year = false, month = false, day = false, weekday = false, hour = false, minute = false;
        int  second = -1;      ///< how far through the minute we are
    };
    std::function<void(const Partial&)> onPartial;

    // ── Health, in the same spirit as FskDecoder's ───────────────────────────
    /** Carrier-to-noise as the decoder sees it, in dB: the difference between the "on" and "off"
     *  envelope levels. ★ THE one number that says whether this will ever work here — below ~6 dB
     *  no amount of decoding cleverness helps, and telling the user that is more use than a
     *  blank panel. */
    double snrDb() const { return snrDb_; }
    State  state() const { return state_; }
    /** Minutes decoded, and minutes thrown away on a failed parity. The RATIO is the diagnostic:
     *  a few failures is a marginal antenna, all failures is the wrong station or the wrong mode. */
    unsigned long minutesGood() const { return good_; }
    unsigned long minutesFailed() const { return bad_; }
    /** ★ RWM has no date to report, so "how many second markers have been counted cleanly" IS the
     *  reading. Also useful on the other stations as a framing sanity check. */
    int  secondNow() const { return second_; }
    /** True for a station that can never produce a TimeStamp — the UI must not sit waiting. */
    bool carriesTimeCode() const { return station_ != Station::RWM; }

private:
    void  setState(State s);
    void  onSecondEdge(double dipMs, double gapMs);
    void  pushBit(int bit);
    bool  decodeMinute(TimeStamp& out) const;
    bool  decodeMsf(TimeStamp& out) const;
    bool  decodeDcf77(TimeStamp& out) const;
    bool  decodeWwv(TimeStamp& out) const;
    void  emitPartial();

    const int      sr_;
    const Station  station_;
    State          state_ = State::NoSignal;

    // Envelope follower: rectify, then a slow and a fast average. The pair is what separates a
    // genuine carrier dip from a fade — a fade moves both, a dip moves only the fast one.
    double envFast_ = 0, envSlow_ = 0;
    /** ★ WWV only: a 100 Hz bandpass ahead of the envelope, because the code is on a SUBCARRIER.
     *  Two biquad states; the coefficients are computed once in the constructor. */
    double bpB0_ = 1, bpB1_ = 0, bpB2_ = 0, bpA1_ = 0, bpA2_ = 0;
    double bpX1_ = 0, bpX2_ = 0, bpY1_ = 0, bpY2_ = 0;
    double onLevel_ = 0, offLevel_ = 0;      // adaptive, so no fixed threshold to get wrong
    double snrDb_ = 0;

    bool   inDip_ = false;
    double dipSamples_ = 0, gapSamples_ = 0;
    /** ★ The gap measured at the dip's START. Read at its END — see the note in process(). */
    double gapBeforeMs_ = 0;
    /** ★ A sample clock, and where the CURRENT second began. MSF needs it: a second can contain
     *  two dips (A=0,B=1 is off-on-off), so "the next dip" is not "the next second". */
    long long clock_ = 0, dipStartClock_ = 0, secondStartClock_ = 0;

    // ★ MSF carries TWO bits per second (A and B) in different 100 ms windows, DCF77 one. Both
    //   fit here; B stays zero where a station has no B bit.
    int    bitsA_[60] = {0}, bitsB_[60] = {0};
    int    second_ = -1;                     // -1 until the minute marker is seen
    unsigned long good_ = 0, bad_ = 0;
    /** ★ The previous parity-passing minute, as a minute count. A reading is only announced when
     *  it is exactly one minute later than this — see the note in onSecondEdge(). */
    long long lastStamp_ = 0;
    /** WWV: the minute is two position markers in a row, so the previous symbol matters. */
    bool lastWasMarker_ = false;
};

}  // namespace vibe
