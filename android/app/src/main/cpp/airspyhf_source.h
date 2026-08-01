// Airspy HF+ capture source (HF+ Discovery, HF+ Dual Port) for VibeServer.
//
// ★★ THIS ONE IS GENUINELY BUNDLED, and that is the difference from the RSP. libairspyhf is
// BSD-3-Clause and ships a static archive, so it links straight into the binary: plug the
// radio in and it works, with nothing for the user to install. SdrplaySource has to dlopen a
// closed-source dylib that the user installs separately, which is why it carries all the
// "is the API even present" machinery this file does not need.
//
// ★ NOT the same library as an Airspy R2 or Mini. Those speak libairspy, which is a different
// API, GPL-2.0, and deprecated upstream after its code was relicensed. Supporting them is a
// separate decision, not a small extension of this file.
//
// Three differences from librtlsdr shape the code here:
//   1. Samples arrive as INTERLEAVED COMPLEX FLOAT, already scaled to roughly +/-1. The dongle
//      and RSP paths both hand the shim int16, so this one feeds the cf32 path directly rather
//      than quantising down and back up again.
//   2. Streaming is a callback started with airspyhf_start(), not a blocking read loop.
//   3. Gain is not a single index: an HF+ has a switchable +6 dB LNA, a 0-48 dB attenuator in
//      6 dB steps, and its own AGC with a low/high threshold. See setGainTenthDb for how the
//      one slider every client already has is mapped onto that.
//
// ★ COVERAGE HAS A HOLE IN IT. An HF+ Discovery tunes 0.5 kHz - 31 MHz and 60 - 260 MHz, and
// NOTHING between: 31-60 MHz is not a weak spot, it is absent. Anything offering a dial has to
// know that, or it will silently sit on a dead frequency. See tuneRangeContains().
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include "halfband.h"

namespace vibe {

class AirspyHfSource {
public:
    /** Interleaved complex float IQ, +/-1.0 nominal — the engine's native format. */
    using IqSink = std::function<void(const float* interleavedIq, int sampleCount)>;

    AirspyHfSource();
    ~AirspyHfSource();
    AirspyHfSource(const AirspyHfSource&) = delete;
    AirspyHfSource& operator=(const AirspyHfSource&) = delete;

    /** How many HF+ radios are attached. Cheap, and safe to call with none. */
    static int  deviceCount();
    /** Display name for the picker, e.g. "Airspy HF+ (0x1234ABCD)". "" if there is no such
     *  device. ★ The serial is part of the name because two HF+ units are otherwise
     *  indistinguishable, and an operator with two needs to know which one they picked. */
    static std::string deviceName(int index);

    /** ★ THE TUNING HOLE, as a predicate. 31-60 MHz does not exist on this hardware, so a
     *  caller must be able to ASK rather than discover it as silence. */
    static bool tuneRangeContains(double hz);

    bool open(int index, double sampleRateHz, double centreHz, int gainTenthDb, std::string& err);
    /** ★★ OPEN AN ALREADY-OPEN USB FILE DESCRIPTOR — the only way in on Android, where
     *  UsbManager hands you an fd and forbids enumeration entirely. Needs the vendored
     *  libairspyhf (VIBE_AIRSPYHF_HAS_FD); Homebrew's build has no such entry point, so this
     *  fails cleanly on a desktop rather than pretending. libusb takes ownership of the fd. */
    bool openFd(int fd, double sampleRateHz, double centreHz, int gainTenthDb, std::string& err);
    void close();
    bool isOpen() const { return open_; }

    void setSink(IqSink sink) { sink_ = std::move(sink); }
    bool start(std::string& err);
    void stop();

    void setFrequency(double hz);
    /** The sample rates THIS radio offers, ascending. Enumerated from the device rather than
     *  assumed: an HF+ Discovery tops out around 912 kHz where a dongle does 2.4 MSPS, so a
     *  hard-coded list would offer rates it cannot do. Empty until open(). */
    /** ★ The rates a CLIENT may choose: hardware rates plus the ones software decimation reaches.
     *  `rates_` stays the hardware truth (nearestRate and the delivered-rate measurement use it);
     *  this is what the picker should show. */
    const std::vector<uint32_t>& sampleRates() const { return offered_.empty() ? rates_ : offered_; }
    const std::vector<uint32_t>& hardwareRates() const { return rates_; }
    /** The list in the RADIO'S OWN ORDER — libairspyhf indexes into this to build the USB
     *  command, so it is the wire format, not a display detail. */
    const std::vector<uint32_t>& rawSampleRates() const { return rawRates_; }
    /** ★★★ SOFTWARE DECIMATION. The radio's own low rates are the untrustworthy ones on this
     *  firmware (228 kHz tunes to the wrong frequency, measured 2026-08-02), and decimating a rate
     *  it gets right is strictly better: same span, correct tuning, ~3 dB of processing gain per
     *  halving, and FEWER bins to send. So hardware rates are used down to the top rate halved,
     *  and everything below that is reached by dividing one of those in software.
     *  ★ Returns the EFFECTIVE rate — what the DSP must be built for. */
    int decimation() const { return decim_; }

    /** ★★★ THE RATE THE RADIO IS ACTUALLY RUNNING AT, measured after the last change — not the
     *  one that was asked for. This device advertises rates it does not implement and reports
     *  success anyway; everything downstream (DSP decimation, FFT span, waterfall calibration)
     *  must be built on this number or the audio comes out pitch-shifted. */
    double currentRate() const { return decim_ > 1 ? curRate_ / decim_ : curRate_; }
    /** The rate the RADIO is running, before software decimation. */
    double hardwareRate() const { return curRate_; }
    /** Nearest supported rate to `hz` — the picker offers only real ones, but a saved
     *  preference from a different radio can still ask for something impossible. */
    uint32_t nearestRate(double hz) const;
    bool setSampleRate(double hz);
    /** Split a wanted EFFECTIVE rate into a hardware rate plus a software decimation factor. */
    void chooseRate(double wantHz, uint32_t& hwOut, int& decimOut) const;

    /** ★ The single gain slider every client already has, mapped onto this radio's controls.
     *  See the note in the .cpp: it drives the ATTENUATOR, because on an HF+ the useful range
     *  is attenuation rather than gain. Negative = let the radio's own AGC decide. */
    void setGainTenthDb(int tenthDb);

    // ── HF+ specific, surfaced only when one of these is the running radio ────
    // ★ NOTE ON NAMING: libairspyhf calls these hf_agc / hf_att / hf_lna, but the "hf" is the
    // PRODUCT (HF+), not the HF band — every one of them acts on the whole device and works
    // identically in the 60-260 MHz window. Do not carry that prefix into user-facing text; it
    // reads as "HF only" and is simply untrue.
    /** The radio's own AGC. On by default, and the right answer for most listening. */
    void setAgc(bool on);
    /** AGC threshold: false = low, true = high. The HF+'s only AGC tuning knob. */
    void setAgcThreshold(bool high);
    /** Attenuator, 0-8 in 6 dB steps (0-48 dB). Ignored while the AGC is on. */
    void setAttenuation(int steps);
    /** +6 dB preamp, compensated in the digital path. */
    void setLna(bool on);
    /** Frequency calibration in PARTS PER BILLION — not ppm. The HF+ is a far better
     *  reference than a dongle, so ppm would be too coarse a control to express it. */
    void setCalibrationPpb(int ppb);

    int  attenuation() const { return att_; }
    bool agc() const { return agc_; }
    bool agcThresholdHigh() const { return agcHigh_; }
    bool lna() const { return lna_; }
    std::string model() const;

    /** ★ IDLE PARK WITHOUT TOUCHING THE LIBRARY, the same trade SdrplaySource makes: with no
     *  listeners we drop samples rather than stop the device. Stopping and restarting a stream
     *  to save power is how a power optimisation ends up taking the server down; dropping costs
     *  the host nothing measurable and cannot fail. */
    void setPaused(bool p) { paused_ = p; }
    bool paused() const { return paused_; }

    /** Set when the radio stops delivering or disappears — polled by the shim's watchdog
     *  exactly as the dongle and RSP paths are. */
    bool deviceLost() const { return lost_; }

    /** ★★ WHEN THE RADIO LAST DELIVERED, regardless of whether we KEPT the samples.
     *  The silence watchdog asks "is the radio still producing?", and while idle-parked the
     *  answer is YES — we are simply throwing the samples away. Timing liveness off the shim's
     *  sink instead meant a parked Airspy looked identical to an unplugged one: after three
     *  seconds with no listener the server declared the radio lost, and the next client to
     *  connect (or REFRESH) was told "no radio connected" over perfectly working audio
     *  (Stuart, 2026-07-27).
     *  ★ Seconds on the same monotonic clock the shim uses. 0 = nothing yet. */
    double lastRxSecs() const;

    /** ★★★ RECOVER A STALLED STREAM IN PLACE — the Airspy equivalent of
     *  SdrplaySource::restartStream(). Nudging the phone was enough to make the HF+ stop
     *  delivering IQ while its LEDs stayed lit and the device stayed enumerated: nothing had
     *  been unplugged, so the watchdog's `deviceCount() > 0` said the radio was present and
     *  cleared `deviceLost` — leaving the server reporting a healthy radio while delivering no
     *  samples at all, until the operator restarted it (Stuart, 2026-07-31).
     *
     *  Two escalating attempts, because a USB glitch leaves one of two states:
     *    1. the stream stalled but the handle is still good -> stop + start fixes it;
     *    2. the handle itself is stale                      -> only close + reopen will.
     *  `deep` selects the second. Rate, tuning and every gain setting are re-applied from the
     *  members we already hold, so a listener sees a gap and nothing else.
     *
     *  ★★ SAFE FROM THE WATCHDOG THREAD, and this is the crucial difference from the RTL
     *  path, which deliberately does NOT reopen: `dev` there is touched unlocked from the
     *  control threads, so closing underneath them crashed the whole server on replug. Every
     *  library call on THIS object already takes impl_->mtx — the same property that made the
     *  RSP's in-place restart safe — and so does this.
     *
     *  ★ `deep` CANNOT WORK ON ANDROID when the device genuinely re-enumerated: the handle
     *  came from a USB file descriptor owned by the Java layer (openFd), and a re-enumerated
     *  device needs a fresh one that only Java can hand us. Reopening by serial is right on
     *  desktop and is the best we can do here; if it fails we report honestly rather than
     *  pretend. */
    bool restartStream(bool deep, std::string& err);

private:
    bool finishOpen(double sampleRateHz, double centreHz, int gainTenthDb, std::string& err);
    struct Impl;
    Impl* impl_ = nullptr;
    IqSink sink_;
    std::vector<uint32_t> rates_;
    std::vector<uint32_t> rawRates_;
    /** Effective rates offered = hardware rates (down to top/2) plus software-decimated ones. */
    std::vector<uint32_t> offered_;
    int decim_ = 1;
    HalfBandChain chain_;
    std::mutex chainMtx_;
    /** Samples the radio has delivered — the basis for measuring its REAL rate. See
     *  setSampleRate: this device advertises rates it does not implement, and says rc 0. */
    std::atomic<long long> sampCount_{0};
    bool open_ = false, streaming_ = false, lost_ = false, paused_ = false;
    bool agc_ = true, agcHigh_ = false, lna_ = false;
    int  att_ = 0;
    // ★ WHAT WE WERE LAST ASKED FOR — kept solely so restartStream(deep) can put the radio
    //   back exactly as the operator had it. Everything else (agc_, lna_, att_) is already
    //   mirrored above; only these three were passed in and forgotten.
    double curRate_ = 0.0, curCentre_ = 0.0;
    int    curGainTenth_ = -1;
};

}  // namespace vibe
