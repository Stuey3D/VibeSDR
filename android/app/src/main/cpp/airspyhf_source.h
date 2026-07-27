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
    void close();
    bool isOpen() const { return open_; }

    void setSink(IqSink sink) { sink_ = std::move(sink); }
    bool start(std::string& err);
    void stop();

    void setFrequency(double hz);
    /** The sample rates THIS radio offers, ascending. Enumerated from the device rather than
     *  assumed: an HF+ Discovery tops out around 768 kHz where a dongle does 2.4 MSPS, so a
     *  hard-coded list would offer rates it cannot do. Empty until open(). */
    const std::vector<uint32_t>& sampleRates() const { return rates_; }
    /** Nearest supported rate to `hz` — the picker offers only real ones, but a saved
     *  preference from a different radio can still ask for something impossible. */
    uint32_t nearestRate(double hz) const;
    bool setSampleRate(double hz);

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

private:
    struct Impl;
    Impl* impl_ = nullptr;
    IqSink sink_;
    std::vector<uint32_t> rates_;
    bool open_ = false, streaming_ = false, lost_ = false, paused_ = false;
    bool agc_ = true, agcHigh_ = false, lna_ = false;
    int  att_ = 0;
};

}  // namespace vibe
