// SDRplay (RSP) capture source for VibeServer.
//
// ★ WHY, and it is a measurement rather than a preference: an RTL-SDR is 8-BIT. RDS is
// injected ~30 dB below peak deviation, and 8 bits gives ~48 dB of usable range, so on a
// strong FM carrier the subcarrier sits close to the quantisation floor — the audio uses the
// top 30 dB and sounds perfect while RDS struggles. An RSP is 14-bit. On 2026-07-26 we proved
// the DECODER is not the limit (0% block errors on a clean synthetic signal, and parity with
// SDR++ Brown on the same dongle), which leaves the front end, and this is how we test that.
//
// ★ It also gives the RDS↔pilot phase work a second opinion. Two different receivers agreeing
// on a station's phase would be strong evidence the number is real; disagreeing would say it
// is ours. That is worth more than any amount of further reasoning about it.
//
// The API differs from librtlsdr in three ways that matter here:
//   1. Samples arrive as SEPARATE 16-bit I and Q buffers, not interleaved bytes.
//   2. Streaming is Init/Uninit with callbacks, not a blocking read_async.
//   3. Gain is TWO-DIMENSIONAL — an LNA state plus an IF gain reduction — so it cannot be
//      mapped onto the dongle's single index without deciding a policy. See setGainTenthDb.
#pragma once
#include <vector>
#include <cmath>
#include <cstdint>
#include <functional>
#include <atomic>
#include <chrono>
#include <string>

namespace vibe {

class SdrplaySource {
public:
    /** Interleaved int16 IQ, ready for the shim's existing enqueueIqInt16 path. */
    using IqSink = std::function<void(const int16_t* interleaved, int sampleCount)>;

    SdrplaySource();
    ~SdrplaySource();
    SdrplaySource(const SdrplaySource&) = delete;
    SdrplaySource& operator=(const SdrplaySource&) = delete;

    /** Is the API present at all? The library is a separate install, so a build that links it
     *  may still run on a machine without it — say so rather than failing obscurely. */
    static bool available();
    static int  deviceCount();
    static std::string deviceName(int index);
    /** ★ The API stopped answering — a stale system-wide lock held by another process, often
     *  one that crashed inside the API. Reported so it can be SHOWN rather than waited on:
     *  a hang has no crash report and nothing for the user to act on. */
    static bool apiUnresponsive();
    /** Allow one more attempt after the API was written off. Called from an explicit
     *  user-initiated Refresh, never automatically — retrying a wedged system lock on a
     *  timer is what caused the pile-up in the first place. */
    static void retryApi();
private:
    static std::string deviceNameLocked(int index);
public:

    bool open(int index, double sampleRateHz, double centreHz, int gainTenthDb,
              std::string& err);
    void close();
    bool isOpen() const { return open_; }

    void setFrequency(double hz);
    void setSampleRate(double hz);
    /** ★ DAB THROUGH THE API'S OWN DECIMATION. On: a 2.048 MS/s request is served as 4.096 MS/s
     *  at the converter through the 5 MHz analogue filter, decimated by 2 inside the API with its
     *  wideband filter, and delivered to us at 2.048 MS/s — so the whole 1.536 MHz ensemble passes
     *  FLAT (the 1.536 filter's −3 dB points sit exactly on the outer carriers, 3 dB down on the
     *  edge subchannels, where the RTL is given a 2.048 filter and loses nothing) and the
     *  neighbours ±1.7 MHz away are removed digitally before the samples reach us. 4.096 keeps the
     *  RSP1A's converter at 14 bits; 8.192 would not. Stuart's ask, 2026-09-15, for 10D. */
    void setDabDecimation(bool on) { dabDecim_ = on; }
    bool dabDecimation() const { return dabDecim_; }
    /** < 0 = the API's own AGC. Otherwise mapped onto LNA state + IF gain; see the .cpp. */
    void setGainTenthDb(int tenthDb);
    /** Bias-T for an active antenna or LNA at the mast. */
    void setBiasT(bool on);

    // ── ★★ THE CONTROLS AN RSP ACTUALLY HAS ──────────────────────────────────
    // Reference: SoapySDRPlay3, which is open source and authoritative about the per-model
    // ranges the API itself does not expose.
    /** RF gain is an LNA STATE, not decibels: 0..lnaStateCount()-1, 0 being most RF gain.
     *  ★ This is the control that decides whether the front end overloads — and RF overload
     *  is what destroys the RDS subcarrier, which is the whole reason this device is here.
     *  A dongle's single gain slider cannot express it, so it must not pretend to. */
    void setLnaState(int state);
    /** IF gain REDUCTION in dB, 20..59. Higher means LESS gain — it is a reduction. */
    /* ★ Returns FALSE when the radio's own AGC owns gRdB and the write was refused, so a
     *   caller can tell "applied" from "silently dropped" — a readout that reports a value the
     *   hardware never took is the fault that made the IF slider look dead. */
    bool setIfGainReduction(int gRdB);
    /** API-side AGC on the IF stage. While enabled the IF reduction cannot be set by hand. */
    void setIfAgc(bool on);
    /** ★★ THE AGC's TARGET LEVEL in dBfs, which SDRconnect exposes and which decides how
     *  hard the AGC drives. It is a TARGET, not a limit: -60 aims for a quiet output and so
     *  applies LESS gain, -20 aims loud and applies MORE.
     *  ★ Getting that backwards is easy and I did: raising it from the API's -60 default to
     *  -30 made the AGC drive harder, which is precisely the "IF auto gain is huge" that
     *  followed (Stuart, 2026-07-26). The right value depends on band and antenna, so it
     *  belongs to the user rather than to a constant of mine. */
    void setIfAgcSetPoint(int dBfs);
    /** ★★ THE AGC'S LOOP DYNAMICS, which the API defaults to ZERO and almost nobody sets.
     *  SDRconnect uses attack 500 ms, decay 500 ms, decay delay 200 ms, decay threshold 5 dB
     *  — and an AGC with no time constants has no loop behaviour at all: it slams straight to
     *  whatever the instantaneous level suggests, which is what "IF auto gain is huge" and
     *  "auto overloads but manual is fine" actually look like from outside.
     *  ★ This is very likely the real bodge that SoapySDRPlay3 and OWRX inherit — not the
     *  setpoint, which SDRconnect also puts at -30 dBFS (Stuart, 2026-07-26). */
    void setIfAgcDynamics(int attackMs, int decayMs, int decayDelayMs, int decayThresholdDb);
    /** RSP1A/1B/2/duo/dx: broadcast FM notch. ★ Wanted ON for HF or airband, where a strong
     *  local FM transmitter is what overloads the front end — and OFF when the FM band is
     *  what you came to listen to. */
    void setRfNotch(bool on);
    /** RSP1A/1B/duo/dx: DAB band notch, same reasoning as the FM notch. */
    void setDabNotch(bool on);

    /** How many LNA states this model offers — 4 on an RSP1, 10 on an RSP1A/1B, 28 on a dx. */
    int  lnaStateCount() const;
    /** ★ True only while the tuner's AGC has REPORTED since our last write — the one condition
     *  under which currentIfGr()/systemGainDb() are measurements rather than echoes of the struct.
     *  Measured 2026-09-15: after a DAB rate change no GainChange arrived, currentIfGr() fell back
     *  to the commanded 59 dB, and the RF loop walked the LNA to its end stop on that echo. */
    bool ifAgcReporting() const { return liveValid_.load(std::memory_order_relaxed)
                                      && !liveStale_.load(std::memory_order_relaxed); }
    /** ★ Seconds since the IF AGC's loop was last RESTARTED by us — every disable/enable dance
     *  (an LNA write, a rate change, a stream restart) puts it back at its 59 dB rail and it then
     *  takes its decay (5 s) to come down. A reading inside that window is the restart, not the
     *  signal. Measured 2026-09-15: each DAB block hop produced "59 dB for 1.0 s" one second after
     *  the hop and an RF step down, until the front end was starved. */
    double secondsSinceAgcRestart() const {
        const long long t = agcRestartedAtMs_.load(std::memory_order_relaxed);
        if (t <= 0) return 1e9;
        const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return (now - t) / 1000.0;
    }
    /** ★ The IF AGC's set point as configured (dBFS). */
    int ifAgcSetPointDbfs() const;
    /** ★★ RESTART A SILENT IF AGC: disable, park gRdB at `gr`, enable — the same transition the
     *  stall recovery makes, callable when the loop has gone quiet while the level sits over
     *  target (2026-09-15: after a DAB rate change no GainChange ever came, the readout echoed
     *  59 while the real reduction was ~28 and the ADC peaked 26 dB over target). */
    void restartIfAgc(int gr);
    void noteAgcRestart() {
        agcRestartedAtMs_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
    }
    std::atomic<long long> agcRestartedAtMs_{0};
    /** ★ How many LNA states exist AT THIS FREQUENCY. Fewer below 60 MHz and in L-band than in
     *  between, per the API's own per-band constants — the no-argument form answers for wherever
     *  the radio is tuned now. Offering a state the band does not have gives a gain loop a dead
     *  zone it cannot tell from a rail. */
    int  lnaStateCount(double hz) const;

    /** ★★★ TEACH THE RADIO'S LNA LADDER FROM ORDINARY OPERATION. Call once per settled tick
     *  with the state we are in, the total gain the tuner reports, and the IF reduction applied;
     *  the LNA's own contribution is total + reduction. Learned per band, because the gain tables
     *  change at the API's band edges. No sweep, no extra writes, nothing disturbed. */
    void  noteLnaGain(int state, float totalGainDb, int ifGrDb);
    /** Learned gain of an LNA state in the current band, or NaN if that state has never been
     *  visited here. Callers MUST cope with NaN rather than assume a step size. */
    float lnaGainDb(int state) const;
    /** Which of the API's gain-table bands a frequency falls in. */
    static int lnaBandId(double hz);
    bool hasRfNotch() const;
    bool hasDabNotch() const;
    bool hasBiasT() const;
    std::string model() const;

    /* ── ★★★ ANTENNA PORTS ────────────────────────────────────────────────────────────────────
     *  GitHub #29 (bower01, an RSPdx-R2): "Didn't find the antenna switch either in the admin
     *  panel, either in the receiver panel. Even when logged in." It was never built.
     *
     *  ★★★ THE LIST IS THE FEATURE. A radio with one aerial socket returns an EMPTY list and no
     *      selector is drawn anywhere — which is every RSP Stuart owns, and is why this cannot
     *      regress his receivers: the write below is never reached on an RSP1/1A/1B.
     *      AGENTS.md, "a control that only works on one radio should not be there": the honest
     *      form of that rule is to ask the radio what it has rather than to branch on a model
     *      name at the far end.
     *  ★★ NAMES ARE THE ONES PRINTED ON THE CASE — "A", "B", "C", "Hi-Z" — because the owner is
     *     looking at the box while they choose. They are also what travels on the wire and what
     *     a per-band rule is written in ("hf:Hi-Z, vhf:A"), so they must stay stable.
     *  ★ The RSPduo is deliberately absent: its "antenna" is a TUNER SWAP (and Hi-Z lives on
     *    tuner 1 only), which is a different operation from writing a field and re-Updating.
     *    An empty list draws nothing, which is the honest answer until that is built and testable.
     *  ✗ UNTESTED ON HARDWARE — nobody here owns a multi-antenna RSP (Stuart, 2026-09-24: "I will
     *    not be able to test this myself though as all my SDR's are single antenna modes"). */
    /* ── ★★ THE REST OF WHAT AN RSP HAS ──────────────────────────────────────────────────────
     *  Swept field by field against the vendor headers (2026-09-24). Each is gated by a
     *  capability so a radio without it draws nothing, exactly as antennaPorts() is.
     *  ✗ UNTESTED: only RSP1A/1B here. Dual-tuner / diversity deliberately excluded. */

    /** ★★★ HDR — the RSPdx family's high-dynamic-range path BELOW 2 MHz, which is precisely where
     *  a strong medium-wave signal overloads an ordinary front end. A plain toggle: the hardware
     *  does the rest (Stuart, 2026-09-24: "its just an on off toggle and the hardware handles it").
     *  ★ The API also offers an HDR BANDWIDTH; it is left at the 1.7 MHz default rather than
     *    exposed, because it only narrows what HDR passes and a second control here would be one
     *    more thing to explain for no gain a listener can hear. */
    bool hasHdr() const;
    void setHdr(bool on);

    /** ★ The Duo's AM broadcast notch — a THIRD filter, separate from the FM and DAB notches, and
     *  the one that matters when a local MW transmitter is flattening 160 m. Tuner 1 only: it sits
     *  on the Hi-Z port, which is why the API names the field after that tuner. */
    bool hasAmNotch() const;
    void setAmNotch(bool on);

    /** ★ 24 MHz reference output (RSP2 and Duo). The ONE control on a Duo that is genuinely shared
     *  between its two tuners — everything else in RspDuoTunerParamsT is per tuner. */
    bool hasExtRefOut() const;
    void setExtRefOut(bool on);

    /** ★★ Frequency correction in ppm. Every dongle has had this for ever and no RSP ever did:
     *  LocalSdrShim::setPpm returns early unless there is a librtlsdr handle. The RSP keeps it on
     *  devParams, so it is a property of the radio rather than of a channel. */
    void setPpm(int ppm);

    std::vector<std::string> antennaPorts() const;
    /** The port now selected, or "" when this model has none. */
    std::string antenna() const;
    /** Select by NAME, as antennaPorts() gives them. Unknown name or single-port model = no-op. */
    void setAntenna(const std::string& port);
private:
    /** ★ Duo only: choose the tuner on the DeviceT before SelectDevice freezes it. No-op elsewhere. */
    void applyDuoChoice();
public:
    /** ★★ TOTAL SYSTEM GAIN in dB — the single number SDRconnect shows above its two
     *  sliders, and the thing that makes them comprehensible. LNA state and IF reduction are
     *  each meaningless alone; what a user actually wants to know is what they have ended up
     *  with. The API computes it for us (gainVals.curr), so not showing it was simply an
     *  omission (Stuart, 2026-07-26). 0 = unknown. */
    float systemGainDb() const;
    /** What the IF reduction currently IS — the AGC moves it, so a slider position is not
     *  the truth while AGC is on. */
    int currentIfGr() const;
    int currentLnaState() const;
    /** ★★★ WHAT THE API'S OWN STRUCT SAYS THE GAIN IS — never the AGC's event value.
     *
     *  systemGainDb() prefers the event figure while the AGC is enabled, which is right for a
     *  DISPLAY and useless for the one question this answers: **did our write land?** The API
     *  refreshes `gainVals.curr` on every Update_Tuner_Gr, so it moves when a write is honoured
     *  and sits still when it is not — including when the AGC is wedged and no events come, which
     *  is precisely when the event figure is frozen and cannot tell us anything.
     *  ★ -999 for "cannot read it", same sentinel as systemGainDb(), because 0 dB is a legitimate
     *    system gain on this radio at medium wave. */
    float structGainDb() const;
    /** The API's own bandwidth choice for a sample rate, following SoapySDRPlay3's mapping. */
    static int bandwidthKHzForRate(double sampleRateHz);

    void setSink(IqSink sink) { sink_ = std::move(sink); }
    /** ★★ IDLE PARK WITHOUT TOUCHING THE API. Closing the device on idle CRASHED inside the
     *  SDRplay API's own shared mutex (ReleaseDevice, from the connection thread, 2026-07-26):
     *  its lifecycle is process-wide shared state and will not tolerate being unwound from
     *  under an arriving client. The park exists to stop a DONGLE drawing power; on an RSP
     *  simply dropping the samples achieves the same for the host, costs nothing, and cannot
     *  crash. ★ A power optimisation must never be able to take the server down. */
    void setPaused(bool p) { paused_ = p; }
    /** Set when the device disappears — the shim's watchdog polls this exactly as it does
     *  for a dongle that has been unplugged. */
    bool deviceLost() const { return lost_; }
    /** ★★★ RE-INITIALISE THE STREAM IN PLACE, after the API has gone quiet without saying so.
     *  The SDRplay API can simply STOP calling the stream callback while every handle stays
     *  valid and every call still returns Success — audio and spectrum freeze, the server
     *  reports itself perfectly healthy, and only killing the process brings it back (Stuart,
     *  2026-07-27: "I have to do a full close and open of vibeserver to resurrect it").
     *  ★ Uninit + Init, NOT ReleaseDevice/SelectDevice: unwinding device selection is what
     *  crashed inside the API's own shared mutex before (see setPaused), and the stream is the
     *  part that has actually died. Device selection and the params struct stay untouched, so
     *  gain, AGC and tuning survive the restart.
     *  ★ Serialised against every other API-touching call on this object, because the caller
     *  is a WATCHDOG THREAD and the control sockets keep taking gain changes throughout.
     *  @return true if the stream was re-initialised; `err` describes any failure. */
    bool restartStream(std::string& err);
    /** ★★★ THROW THE DEVICE AWAY AND OPEN IT AGAIN — the recovery restartStream() cannot be.
     *  A NUDGED USB PLUG IS NOT A STALL. The device leaves the bus and comes back, usually at a
     *  new address, so the selected handle is dead for good: Uninit + Init on it can only ever
     *  fail, however many times it is tried. That is why a nudge needed the whole server stopped
     *  and started (Stuart, 2026-08-02) — process exit is what finally released the device.
     *  ★★ Yes, this unwinds device selection, which the setPaused notes warn about: ReleaseDevice
     *  from a CONNECTION thread, with a client arriving, crashed inside the API's own shared
     *  mutex (2026-07-26). Two things make it safe here and neither is optional — it runs under
     *  api_mtx, which every API-touching call on this object now takes, and it runs ONLY after
     *  the stream is already dead, so there is no live callback to unwind underneath.
     *  ★ LAST RESORT, NOT FIRST. The watchdog tries restartStream() twice first: a genuine stall
     *  is far commoner than a re-enumeration and re-Init fixes it without the listener noticing.
     *  ★ REOPENS BY SERIAL. Enumeration order is not stable across a re-plug, and a box may have
     *  more than one RSP — "device 0" could hand back a different radio than the one being
     *  listened to. Restores the live rate, frequency and gain, not the ones it first opened with.
     *  @return true if the device was reopened and streaming; `err` describes any failure. */
    bool reopen(std::string& err);
    /** ★★ THE RADIO'S OWN OVERLOAD FLAG. The API raises a PowerOverloadChange event when the
     *  ADC is being driven into clipping — so we do not have to INFER overload from the
     *  spectrum, as the auto-gain brief proposes for a dongle: on an RSP the hardware simply
     *  says so. SDRconnect shows it as a badge, and tonight established that RF overload is
     *  precisely what destroys RDS, so it is worth shouting about (Stuart, 2026-07-26).
     *  ★ The event MUST be acknowledged or the API stops sending them. */
    bool overloaded() const { return overload_; }
    /** ★★★ HAS THE API ITSELF REPORTED A FAILURE? Set from sdrplay_api_DeviceFailure, which is
     *  the library saying it has fallen over in its own words — no inference from behaviour, and
     *  so no false positives on a radio that is merely settled. Cleared by the caller once it has
     *  acted on it. */
    bool apiFailed() const { return apiFailed_.load(std::memory_order_relaxed); }
    /* ★ The API SERVICE (sdrplay_apiService) has stopped answering: Uninit said
     *   ServiceNotResponding, or Init said AlreadyInitialised after an Uninit that never landed.
     *   No call from this process cures it — the service has to be restarted (Linux: the
     *   maintenance helper does it; elsewhere the operator is told). Cleared by a successful
     *   Init. Measured 2026-09-15 19:49 on the Lenovo RSP1A. */
    bool serviceUnresponsive() const { return serviceDead_.load(std::memory_order_relaxed); }
    void clearApiFailed() { apiFailed_.store(false, std::memory_order_relaxed); }

    /** Ask the tuner to recalibrate its DC offset now — the offset is gain-dependent, so this is
     *  called after every gain change. See the definition for why it matters. */
    void dcRecalibrate();

    /** ★★★ THE OVERLOAD FLAG, CORROBORATED BY THE SAMPLES — AND THIS IS THE ONE TO USE.
     *  `overloaded()` is the API's raw PowerOverloadChange latch. It is set on an overload event
     *  and cleared only by a clearing event, which may simply never arrive — so in practice it
     *  sticks ON and stays there. A warning lamp that is always lit says exactly as much as one
     *  that never lights: nothing. (The inverse of the "fires once then goes quiet" trap the
     *  acknowledgement in eventCb exists to avoid.)
     *  ★ Now that we MEASURE the level ourselves, the flag can be checked against reality: an
     *  overload means samples at the rail, or a peak effectively there. Uncorroborated, it is a
     *  stale latch and is ignored.
     *  ★★ ONE READER FOR ONE FACT. VibeAGC and the client's OVERLOAD badge must not disagree
     *  about whether the radio is overloading — tonight they did, because the loop was taught to
     *  distrust the latch and the telemetry was not (Stuart's screenshot, 2026-09-12: the badge
     *  lit at 6.4 dB of system gain with nothing clipping). */
    bool overloadReal() const {
        if (!overload_) return false;
        if (windows_.load(std::memory_order_relaxed) == 0) return true;   // no measurement yet
        return clipPct_.load(std::memory_order_relaxed) > 0.0
            || peakDbfs_.load(std::memory_order_relaxed) > -1.0;
    }

    /** ★★★ THE RADIO'S OWN SIGNAL LEVEL, MEASURED BY US. VibeAGC for the dongle closes its loop
     *  on `g_adcPeakDbfs`, which is computed inside the u8→f32 conversion — an RTL-only path. The
     *  RSP hands us int16 through its own callback and was never measured at all, so the RSP's
     *  gain loop had nothing of its own to steer by and had to read SDRplay's IF AGC reduction as
     *  a PROXY for level. That is why it died the moment anyone turned that AGC off: its input
     *  was another controller's output (Stuart, 2026-09-12).
     *  ★ Peak of |I|,|Q| over the last window, in dBFS at the API's output (full scale 32768).
     *    -99 until the first window closes. Peak, not RMS: a gain control has to answer "how
     *    close to the rail", and only the peak knows.
     *  ★★ `adcClipPct` is the fraction of samples AT the rail, which is the honest overload
     *     evidence — the hardware's PowerOverloadChange event is coarse and latches. */
    double adcPeakDbfs() const { return peakDbfs_.load(std::memory_order_relaxed); }
    double adcClipPct()  const { return clipPct_.load(std::memory_order_relaxed); }
    /** Windows closed since open — 0 means nothing has been measured yet and no loop may run. */
    unsigned adcWindows() const { return windows_.load(std::memory_order_relaxed); }
    /** Discard the measurement window in progress — call on any retune or rate change. */
    void adcRestart() { gen_.fetch_add(1, std::memory_order_relaxed);
                        windows_.store(0, std::memory_order_relaxed);
                        peakDbfs_.store(-99.0, std::memory_order_relaxed);
                        clipPct_.store(0.0, std::memory_order_relaxed); }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    IqSink sink_;
    bool open_ = false;
    bool lost_ = false;
    // ★ What the radio is doing RIGHT NOW, not what it was opened with — reopen() has to put the
    //   listener back where they were, and they will have tuned and changed gain since.
    std::string curSerial_;
    double curRate_   = 0.0;
    double curCentre_ = 0.0;
    /* ★ The learned LNA ladder: gain per state, per API gain-table band. Filled in from
     *   ordinary operation by noteLnaGain(); NaN-free only where lnaSeen_ says so. */
    static constexpr int kLnaBands     = 5;
    static constexpr int kLnaStatesMax = 28;    // the RSPdx, the largest table we support
    float lnaObs_ [kLnaBands][kLnaStatesMax] = {};
    bool  lnaSeen_[kLnaBands][kLnaStatesMax] = {};
    int    curGain_   = -1;
    bool paused_ = false;
    bool overload_ = false;
    std::atomic<bool> apiFailed_{false};
    std::atomic<bool> serviceDead_{false};
    bool dabDecim_ = false;
    /** ★ The chosen port, kept HERE so it survives a re-Init: reopen() and the stall watchdog both
     *  rebuild the device parameters from defaults, and a selection that lived only in the API's
     *  struct would quietly revert to A on every recovery. Empty = never chosen / no ports. */
    std::string antenna_;
    // ★★★ THE AGC'S OWN NUMBERS, from the gain-change EVENT. The API reports what the loop has
    //     actually done here; our copy of tunerParams.gain is only what WE last wrote, so with the
    //     AGC running it never moves — the readouts sat still and the IF slider never tracked
    //     (Stuart, 2026-08-03). Written from the event callback thread, read from the DSP/HTTP
    //     threads, so they are atomic: torn reads of a gain figure are not worth a lock.
    std::atomic<int>   liveGr_{0};      // IF gain reduction the AGC has settled on, dB
    std::atomic<int>   liveLna_{0};     // LNA gain reduction, dB (not the LNA *state*)
    std::atomic<float> liveGain_{0.0f}; // total system gain, dB
    std::atomic<bool>  liveValid_{false};
    /** ★★★ HAS THE AGC CONFIRMED WHAT WE LAST WROTE?
     *
     *  Set when WE change the gain, cleared by the next GainChange event. While it is set, the
     *  AGC's reported figures describe a gain setting that no longer exists, so the readouts must
     *  come from the struct — which is what we commanded and, when the writes are landing, what
     *  the radio is actually doing.
     *  ★ This is NOT a staleness TIMER, and deliberately: a settled AGC fires no events for
     *    minutes at a time and its last figure stays perfectly true throughout. Only OUR OWN write
     *    can make it false, so only our own write raises this. */
    std::atomic<bool>  liveStale_{false};
    // ★ The level measurement above, filled by streamCb. Atomics because the callback is the
    //   API's thread and every reader is ours.
    /** ★★★ BUMPED WHENEVER THE CAPTURE CHANGES UNDER US (rate, frequency, gain re-plan), so the
     *  measurement window in progress is DISCARDED rather than averaged across the change. A
     *  window that straddles a retune contains samples taken at the old rate and the old gain,
     *  and reports them as if they were the new signal. */
    std::atomic<unsigned> gen_{0};
    std::atomic<double>   peakDbfs_{-99.0};
    std::atomic<double>   clipPct_{0.0};
    std::atomic<unsigned> windows_{0};
};

}  // namespace vibe
