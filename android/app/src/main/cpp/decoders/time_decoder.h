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
#include <vector>

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
        /** ★★ WWVB (Fort Collins, 60 kHz) — an LF AMPLITUDE station like MSF and DCF77, so it
         *  rides the same envelope path and NOT WWV's subcarrier one, despite the name. Carrier
         *  attenuated for 200 ms (0), 500 ms (1) or 800 ms (a marker).
         *  ★★★ ITS BIT ORDER IS THE OPPOSITE OF WWV's: WWVB sends each field MSB FIRST (seconds
         *  1–8 are 40,20,10,-,8,4,2,1) where WWV sends LSB first. Taking one station's map for the
         *  other produces a confidently wrong clock, which is the worst failure this decoder has —
         *  and neither carries parity to catch it. */
        WWVB,
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
        /** ★ DISPLAY ONLY (2026-10-05) — none of these decides whether a minute is accepted.
         *  DUT1 = UT1 − UTC in tenths of a second, as the station states it (MSF B1-B16, WWV
         *  s50 + s56-58, WWVB s36-38 + s40-43; DCF77 does not send it). `dut1Known` is false when
         *  the station sends none or the bits contradict themselves (both signs set). */
        bool dut1Known = false;
        int  dut1Tenths = 0;
        /** ★ WWV/WWVB only: DST in force at 24:00 UTC TODAY (WWV s55, WWVB s57), where `dst` is the
         *  status at 00:00 UTC. The two differ on exactly the day the clocks change — the one day
         *  this flag is worth showing. `hasDst2` is false on stations that do not send it. */
        bool hasDst2 = false;
        bool dst2 = false;
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
        /** ★★★ AND WHICH OF THOSE ARE NOT TO BE BELIEVED (2026-10-05). A field arrives seconds —
         *  up to half a minute — before the parity bits that check it, so a raw field is shown
         *  unchecked; Stuart: "Occasionally MSF will give a real odd date and time of like 2014 or
         *  something but it usually corrects itself on the next pass". It read like a confident
         *  time. A field is DOUBTED (drawn as "??", never as its number) when:
         *    • it cannot be a field at all — a BCD digit past 9, month 13, hour 24 … — always;
         *    • or, after a recent locked minute, it disagrees with what THIS frame must say.
         *  Before any lock a valid raw field is still shown: the state line says "reading". */
        bool yearBad = false, monthBad = false, dayBad = false, weekdayBad = false,
             hourBad = false, minuteBad = false;
        int  second = -1;      ///< how far through the minute we are
    };
    std::function<void(const Partial&)> onPartial;
    /** ★★★ RWM's MORSE CALLSIGN — the only thing it transmits that PROVES it is being heard.
     *
     *  RWM sends no timecode, so without this the panel can only ever say "counting markers",
     *  which is indistinguishable from counting noise. Decoding "RWM" out of the air is the
     *  difference between "something is ticking" and "this is Moscow".
     *
     *  ★★ CHEAP HERE, EXPENSIVE IN GENERAL — and that distinction is the whole reason it is worth
     *     doing. What makes a CW decoder costly is the TONE TRACKING: Goertzel or FFT bins, an
     *     adaptive threshold, and speed estimation. This decoder already computes an envelope
     *     follower for the second markers, so dot/dash lengths come off a signal we are producing
     *     anyway — a handful of comparisons per second, no new DSP stage. A general-purpose CW
     *     decoder for arbitrary signals is the expensive one (Stuart: "we tried a while back and
     *     it was CPU intensive").
     *  ★ Emitted per decoded character, so the panel fills in as it is heard. */
    std::function<void(char)> onMorse;

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
    void  onRwmEdge();
    bool  decodeMinute(TimeStamp& out) const;
    bool  decodeMsf(TimeStamp& out) const;
    bool  decodeDcf77(TimeStamp& out) const;
    bool  decodeWwv(TimeStamp& out) const;
    bool  decodeWwvb(TimeStamp& out) const;
    void  emitPartial();
    void  pushBit(int bit);

    // ── The matched-filter reader (MSF, DCF77, WWVB, WWV) — see the block in the .cpp ─────────
    /** One second as read: the best symbol overall and its margin; the best DATA symbol and its
     *  margin; the soft value of each bit (+1 = surely 1, -1 = surely 0, 0 = nothing known). */
    struct SecRec {
        long long idx = -1;            ///< flywheel second number (-1 = none)
        signed char cls = -1, dcls = -1;
        float conf = 0, dconf = 0, softA = 0, softB = 0;
        bool contrast = false;         ///< the levels allowed a read at all
    };
    static constexpr int    kMaxCls = 6, kMaxWin = 170, kHistBins = 200;
    static constexpr size_t kRecRing = 256;
    void   initReader();
    void   readerStep();
    void   classify(SecRec& r) const;
    void   onSecond(const SecRec& r);
    void   placeRec(int pos, const SecRec& r);
    int    symbolOf(const SecRec& r, bool& readable) const;
    bool   strongKind(const SecRec& r, int kind) const;
    const SecRec& rec(long long idx) const;
    float  envAt(long long i) const;
    double zAt(double pos) const;
    bool   contrastOk() const;
    bool   isEdge(long long i, double& pos) const;
    int    phaseBin(double pos) const;
    double histTriple(int b) const;
    int    histBest(double& score, bool& clear) const;
    void   seedAt(double edgePos);
    // ── Framing ──
    void  startFrame(long long idx);
    void  place(int sec, int a, int b, int sym, bool readable);
    void  closeFrame();
    void  loseFrame();
    void  finishMinute(bool decoded, const TimeStamp& ts);
    bool  slotsComplete(int from, int to) const;

    int    nCls_ = 0, win_ = 0;
    signed char clsKind_[kMaxCls] = {0}, clsA_[kMaxCls] = {0}, clsB_[kMaxCls] = {0};
    unsigned char tpl_[kMaxCls][kMaxWin] = {{0}};
    int    nd_[kMaxCls][kMaxCls] = {{0}};
    double hist_[kHistBins] = {0};
    long long scanPos_ = 0;
    bool   phaseKnown_ = false;
    double curEdge_ = 0, period_ = 200, periodNom_ = 200;
    int    trkN_ = 0, missRun_ = 0, stage_ = 0;
    long long secIdx_ = 0;
    SecRec recs_[kRecRing];
    /** The flywheel second that is second 0 of the minute being read (-1 = hunting). */
    long long anchorIdx_ = -1;
    /** Minutes in a row whose minute mark was not seen (the grid coasts; 3 = lost). */
    int    unconfirmed_ = 0;
    double lowSnrS_ = 0;

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
    double onLevel_ = 0, offLevel_ = 0;      // adaptive, so no fixed threshold to get wrong (RWM)
    /** ★★ Every other station: the 5th and 90th percentiles of the last 3 s of envelope — see
     *  process(). Kept at 200 Hz in a ring, which the matched-filter reader also reads. */
    bool   pushEnvelope();
    int    decim_ = 240, decimCount_ = 0;
    std::vector<float> env_, pctScratch_;
    long long envCount_ = 0;
    double pLo_ = 0, pHi_ = 0;
    bool   pctReady_ = false;
    double snrDb_ = 0;

    bool   inDip_ = false;
    double dipSamples_ = 0, gapSamples_ = 0;
    /** RWM: the gap measured at the dip's START, read at its END. */
    double gapBeforeMs_ = 0;
    /** ★ The input sample clock. */
    long long clock_ = 0;
    /** The current minute has been decoded (or abandoned); later dips must not decode it again. */
    bool      frameClosed_ = true;
    /** ★★ Per second of the minute: 0 = nothing arrived, 1 = exactly one readable symbol,
     *  2 = ERASED (unreadable, or two symbols claimed the same second). A minute with an erasure
     *  in it is not decoded — it is a failed minute, never a guessed one. */
    unsigned char slot_[60] = {0};
    /** WWVB: the symbol per second (0, 1, 2 = marker), for the marker-position framing check. */
    signed char   sym_[60] = {0};

    // ★ MSF carries TWO bits per second (A and B) in different 100 ms windows, DCF77 one. Both
    //   fit here; B stays zero where a station has no B bit.
    int    bitsA_[60] = {0}, bitsB_[60] = {0};
    int    second_ = -1;                     // -1 until the minute marker is seen
    unsigned long good_ = 0, bad_ = 0;
    /** ★ The previous parity-passing minute, as a minute count. A reading is only announced when
     *  it is exactly one minute later than this — see the note in finishMinute(). */
    long long lastStamp_ = 0;
    /** ★★ What the NEXT frame's raw fields must read, from the last CORROBORATED minute, and the
     *  sample it was decoded at (0 = no expectation). The progress line compares against it,
     *  advanced by the whole minutes elapsed since; it expires after kExpectTtlS. */
    TimeStamp expectNext_{};
    long long expectClock_ = 0;
    /** True if this frame's raw fields are expected to equal `e` (fills `e`), false if unknown. */
    bool  expectedNow(TimeStamp& e) const;

    // ── RWM's Morse identifier ───────────────────────────────────────────────
    // ★ A dot/dash classifier over the SAME envelope the markers use. `unitMs_` adapts to the
    //   sending speed rather than assuming one, because RWM's ID is keyed by the station and its
    //   speed is not ours to assume.
    double morseUnitMs_ = 60.0;      ///< current estimate of one dot
    std::string morseSym_;           ///< dots and dashes of the character being built
    double morseSilenceMs_ = 0;      ///< how long since the last mark ended
    void   morseMark(double onMs);
    void   morseGap(double offMs);
    void   morseFlush();
};

}  // namespace vibe
