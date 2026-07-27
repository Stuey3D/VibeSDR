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
#include <cstdint>
#include <functional>
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
    void setIfGainReduction(int gRdB);
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
    bool hasRfNotch() const;
    bool hasDabNotch() const;
    bool hasBiasT() const;
    std::string model() const;
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
    /** ★★ THE RADIO'S OWN OVERLOAD FLAG. The API raises a PowerOverloadChange event when the
     *  ADC is being driven into clipping — so we do not have to INFER overload from the
     *  spectrum, as the auto-gain brief proposes for a dongle: on an RSP the hardware simply
     *  says so. SDRconnect shows it as a badge, and tonight established that RF overload is
     *  precisely what destroys RDS, so it is worth shouting about (Stuart, 2026-07-26).
     *  ★ The event MUST be acknowledged or the API stops sending them. */
    bool overloaded() const { return overload_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    IqSink sink_;
    bool open_ = false;
    bool lost_ = false;
    bool paused_ = false;
    bool overload_ = false;
};

}  // namespace vibe
