// HackRF One capture source for VibeServer — EXPERIMENTAL, Linux only.
//
// ★★★ NOBODY HERE HAS ONE. This driver was written against libhackrf's API and the published
// hardware description, and has never been run against a radio by its author. Exactly one person
// can test it (a listener who owns one), which is why it is Experimental, why it is mentioned only
// in the README and the release notes, and why it is NOT advertised in the app. Treat every
// hardware claim below as "what the documentation says", not "what was measured" — the rest of
// this codebase earns its comments by measurement and this file cannot, yet.
//
// ★★★ AND THAT IS WHY THERE IS NO VibeAGC HERE. Stuart's rule: a gain loop tuned against radios
// that behave nothing like this one has no business driving it unheard. VibeAGC has been shaped
// all week against an RTL dongle, an HF+ and an RSP, using a floor-rise test, shoulder
// measurements and per-band ceilings — none of which has been checked against an 8-bit radio with
// three gain stages. The controls here are MANUAL, and stay manual until somebody can listen to
// the loop working. See setGainTenthDb().
//
// Four differences from the radios already supported shape this file:
//   1. Samples are INTERLEAVED SIGNED 8-BIT, not int16 (dongle, RSP) and not float (HF+). They
//      are converted to the engine's cf32 here, which is one pass and no quantising loss —
//      8 bits into a float is exact.
//   2. THREE gain stages, not one: an RF amp that is a 0/14 dB SWITCH, an LNA in 8 dB steps to
//      40, and a VGA in 2 dB steps to 62. There is no single "gain" this radio has.
//   3. NO HARDWARE AGC. libhackrf exposes no automatic mode — amp, LNA and VGA are all manual.
//      Anything offering an "AGC" control for this radio would be offering a button that does
//      nothing, which is the fault AGENTS.md names outright.
//   4. The sample rate is CONTINUOUS, not a list read from the device, and its floor is 2 MSPS —
//      well above the 1.2 MSPS an RTL dongle is usually run at here. A HackRF therefore costs
//      more DSP than any other supported radio before it has done anything useful.
//
// ★★ 8 BITS IS THE WHOLE CHARACTER OF THIS RADIO. An RTL dongle is 8-bit too but has an AGC and a
// tuner designed for broadcast; an HF+ is effectively 18-bit. A HackRF will clip where the others
// cope, and the gain stages are how you avoid it — which is precisely why they are exposed rather
// than hidden behind a slider.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vibe {

class HackRfSource {
public:
    /** Interleaved complex float IQ, +/-1.0 nominal — the engine's native format. */
    using IqSink = std::function<void(const float* interleavedIq, int sampleCount)>;

    HackRfSource();
    ~HackRfSource();
    HackRfSource(const HackRfSource&) = delete;
    HackRfSource& operator=(const HackRfSource&) = delete;

    /** How many HackRFs are attached. Cheap, and safe to call with none. */
    static int  deviceCount();
    /** Display name for the picker, e.g. "HackRF One (0000...c8d1)". "" if there is no such
     *  device. ★ The serial is in the name for the same reason as the HF+: two are otherwise
     *  indistinguishable and an operator with two needs to know which one they picked. */
    static std::string deviceName(int index);

    /** ★ 1 MHz - 6 GHz. Unlike the HF+ there is no hole in the middle, but the bottom end is a
     *  real limit: a HackRF cannot hear the medium-wave or long-wave broadcast bands at all
     *  without an upconverter, and a dial that offers them would be offering silence. */
    static bool tuneRangeContains(double hz);

    bool open(int index, double sampleRateHz, double centreHz, int gainTenthDb, std::string& err);
    void close();
    bool isOpen() const { return open_; }

    void setSink(IqSink sink) { sink_ = std::move(sink); }
    bool start(std::string& err);
    void stop();

    void setFrequency(double hz);

    /** ★★ A CHOSEN LIST, NOT AN ENUMERATED ONE. libhackrf accepts a continuous rate, so unlike
     *  the HF+ there is nothing to read back from the radio — these are the rates worth offering.
     *  The floor is the hardware's own 2 MSPS; the ceiling is what the DSP can plausibly carry
     *  rather than what the radio can produce (it will do 20, which nothing here could process). */
    const std::vector<uint32_t>& sampleRates() const { return rates_; }
    uint32_t nearestRate(double hz) const;
    bool setSampleRate(double hz);

    /** ★★★ THE ONE SLIDER EVERY CLIENT HAS, MAPPED ONTO THREE STAGES — and deliberately simple.
     *  It drives the LNA and then the VGA in the order that costs least noise figure, leaving the
     *  RF amp alone (that one is a switch, and switching it under a listener is a 14 dB jump).
     *  ★ Negative is NOT "let the radio decide" here, because this radio cannot: there is no
     *    hardware AGC. Negative means "leave the stages where the owner put them". */
    void setGainTenthDb(int tenthDb);

    // ── HackRF specific, surfaced only when one of these is the running radio ─────────────
    // ★ These are the controls the setup page draws for driver == "hackrf". They exist because
    //   this radio genuinely has three independent stages; offering them for any other radio
    //   would be the "control that only works on one radio" fault in reverse.
    /** RF amp: a 0 or +14 dB SWITCH in front of everything. Off is the right default — on a
     *  crowded band it is the fastest way to overload an 8-bit converter. */
    void setAmpEnable(bool on);
    /** LNA (IF) gain, 0-40 dB. The hardware accepts 8 dB steps only; anything else is rounded
     *  DOWN, because rounding up is how you clip a radio with no headroom to spare. */
    void setLnaGainDb(int db);
    /** VGA (baseband) gain, 0-62 dB, 2 dB steps, rounded down for the same reason. */
    void setVgaGainDb(int db);
    /** Bias-T: 3.3 V, 50 mA on the antenna port for a powered LNA. ★ OFF unless asked for —
     *  feeding voltage up a coax at somebody else's antenna is not a default. */
    void setBiasTee(bool on);

    int  ampEnabled() const { return amp_; }
    int  lnaGainDb()  const { return lna_; }
    int  vgaGainDb()  const { return vga_; }
    bool biasTee()    const { return bias_; }

    /** Seconds since the last buffer arrived, or a large number if none ever has — the same
     *  liveness signal the other sources provide, and what a stall watchdog reads. */
    double secondsSinceLastRx() const;

    /** ★ Drop buffers without tearing the device down, for the idle saver. Stopping and
     *  restarting a HackRF is far more disruptive than simply not keeping what it sends. */
    void setPaused(bool paused);

private:
    struct Impl;
    Impl*   impl_ = nullptr;
    IqSink  sink_;
    bool    open_  = false;
    /* ★★★ EVERY STAGE STARTS AT ZERO, AND THE RF AMP AND BIAS-T START OFF. THIS IS A HARDWARE
     *     SAFETY DEFAULT, NOT A TASTE ONE. Stuart: "the hackrf MUST DEFAULT TO 0 GAIN AND PREAMP,
     *     those things have a bad habit of blowing up their preamps." A HackRF has no AGC and no
     *     protection worth the name: come up from nothing and the worst case is a quiet waterfall
     *     the owner turns up, while starting hot on an unknown aerial can cost them the radio.
     * ★★ THE SAME LOGIC THE RSP ALREADY USES, one step further. That one starts at minimum gain
     *    and maximum attenuation because "approaching a gain target from the QUIET side is the
     *    only safe direction"; here the downside of the other direction is not clipping, it is
     *    damage.
     * ★★ AND BIAS-T OFF. It puts 3.3 V up the coax. On somebody else's antenna, with an unknown
     *    feed at the far end, that is not a thing to switch on by default — or to leave on
     *    because a previous session did.
     * ★ These are also what open() applies before any saved slider value, so a preference
     *   inherited from a DIFFERENT radio cannot land the stages hot on a fresh HackRF. */
    int     amp_   = 0;
    int     lna_   = 0;
    int     vga_   = 0;
    bool    bias_  = false;
    std::vector<uint32_t> rates_;
};

}  // namespace vibe
