// Ettus USRP B2xx capture source for VibeServer — EXPERIMENTAL (V12, 2026-11-02). docs/v12/USRP.md.
//
// ★★★ NOBODY HERE HAS ONE. Written against UHD's published C++ API (uhd::usrp::multi_usrp, UHD 4.9.0.1) and Ettus's
//     B200/B210/B200mini documentation, and never run against a radio by its author. It ships as "added, needs a
//     tester" (Stuart, 2026-11-02). Treat every hardware claim below as "what the documentation says".
//
// ★★★ HIGH-END HOSTS ONLY: Linux amd64, Linux arm64 and the Mac (Stuart, 2026-11-02). UHD is linked into those three
//     builds only (vibeserver/CMakeLists.txt, VIBE_HAVE_USRP); everywhere else — armhf (Pi 2), i386, Android, iOS —
//     this header compiles to the INLINE STUB at the bottom, which reports zero devices. So no other build needs a
//     source file, a library or an #ifdef to carry it, and a host that cannot drive a USRP never offers one (no dead
//     control — AGENTS.md).
//
// ★★★ PLUG AND PLAY: UHD is static inside vibeserver and the B2xx firmware + FPGA images ship in the package
//     (/usr/share/vibeserver/uhd-images; the Mac app's Resources/uhd-images). Nothing is downloaded on the owner's
//     machine. vibe_usrp_caps.h resolves which image file is used (an owner override directory first — the only way
//     a clone that needs a third-party FPGA image can be run — then ours).
//
// What shapes this file, against the radios already supported:
//   1. ONE gain element. The AD9361's RX gain is a single number UHD exposes as "PGA", 0-76 dB in 1 dB steps on a
//      B2xx (read from the device, get_rx_gain_range — never assumed). So it rides the generic gain slider like a
//      dongle's table, and needs no panel of its own.
//   2. A HARDWARE AGC (the AD9361's own, set_rx_agc). AUTO on the slider (a negative gain) engages it; any number
//      turns it off. It is published as hwAgc only when the device accepted it at open.
//   3. TWO RX PORTS PER CHANNEL (TX/RX and RX2), and a B210 has TWO CHANNELS (A, B). Each (channel, port) pair is one
//      entry in antennas() — four on a B210, two on a B200 / B200mini. VibeServer never creates a TX streamer, so
//      choosing the TX/RX socket only RECEIVES on it; nothing here can transmit.
//   4. SAMPLES ARRIVE AS FLOAT. The stream is "fc32" on the host, "sc16" over the wire: UHD's own converter (SSE2 /
//      NEON, with a generic fallback, inside libuhd) does the int16 → float step, so there is no new hot loop of ours
//      to vectorise. The AD9361's own DC-offset and IQ-balance tracking are on at open (UHD's default) unless the
//      owner switched them off (setOptions).
//   5. THE RATE IS CONTINUOUS (UHD picks the master clock), so the list is chosen, not enumerated: vibe_usrp_caps.h,
//      capped by the USB link speed the device reports (USB 2 carries far less than USB 3).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "vibe_usrp_caps.h"   // ★ header-only policy (Options, OptionCaps, CapsView) — the stub needs it too

namespace vibe {

#if defined(VIBE_HAVE_USRP)

class UsrpSource {
public:
    /** Interleaved complex float IQ, +/-1.0 nominal — the engine's native format. */
    using IqSink = std::function<void(const float* interleavedIq, int sampleCount)>;

    UsrpSource();
    ~UsrpSource();
    UsrpSource(const UsrpSource&) = delete;
    UsrpSource& operator=(const UsrpSource&) = delete;

    /** Always true in this build. */
    static constexpr bool compiledIn() { return true; }
    /** ★ How many B2xx are on the USB bus, by VID:PID — a descriptor walk, NO open and NO firmware load. Cheap, and
     *  safe with none (and with one another process is streaming). */
    static int  deviceCount();
    /** "Ettus USRP B210 (30AD2C1)" / "Ettus USRP B2xx (…)" — the serial is in the name for the same reason as every
     *  other radio's: two are otherwise indistinguishable. "" if there is no such device. */
    static std::string deviceName(int index);
    /** The USB serial (the B2xx EEPROM's), "" when it could not be read (held by someone else). */
    static std::string deviceSerial(int index);

    bool open(int index, double sampleRateHz, double centreHz, int gainTenthDb, std::string& err);
    void close();
    bool isOpen() const { return open_; }

    void setSink(IqSink sink) { sink_ = std::move(sink); }
    bool start(std::string& err);
    void stop();

    void setFrequency(double hz);
    /** The device's own tuning range (get_rx_freq_range), Hz. 0/0 before open. */
    double freqMinHz() const { return fmin_; }
    double freqMaxHz() const { return fmax_; }

    const std::vector<uint32_t>& sampleRates() const { return rates_; }
    uint32_t nearestRate(double hz) const;
    /** Stops the stream if it is running, sets the rate (UHD re-picks the master clock), restarts it. */
    bool setSampleRate(double hz);

    /** 0 … max in 1 dB steps as the DEVICE reports them, in tenths (the slider's contract). */
    const std::vector<int>& gainListTenthDb() const { return gains_; }
    /** ★ Negative = the AD9361's own AGC (when hasHwAgc()); otherwise the nearest step, AGC off. */
    void setGainTenthDb(int tenthDb);
    int  gainTenthDb() const { return gainTenth_; }
    bool hasHwAgc() const { return hasAgc_; }
    bool hwAgc() const { return agcOn_; }

    /** "RX2", "TX/RX" — and on a B210 "RX2 (A)", "TX/RX (A)", "RX2 (B)", "TX/RX (B)": the case's labels, with the channel.
     *  ★ EMPTY on a radio with only one choice, so no selector is drawn for it (AGENTS.md). */
    const std::vector<std::string>& antennas() const { return antennas_; }
    std::string antenna() const;
    /** False for a name the radio does not have — refused, never guessed. Switching channel rebuilds the stream. */
    bool setAntenna(const std::string& label);

    std::string model() const { return model_; }     // "B210", "B200mini" … from UHD (the EEPROM), never inferred
    std::string serial() const { return serial_; }
    int  usbSpeed() const { return usbSpeed_; }       // 2 or 3 (0 = unknown)
    /** Overflows ('O') reported by UHD since the last call — the USB link or the host fell behind. */
    uint64_t takeOverflows();

    double secondsSinceLastRx() const;
    /** ★ Drop buffers without tearing the stream down, for the idle saver. */
    void setPaused(bool paused);

    /** ★★ V12 THE OWNER'S OPTIONS (vibe_usrp_caps.h Options): reference clock, time source, the AD9361's DC-offset and
     *  IQ-balance correction, the analogue RX bandwidth. Stored for the next open() and applied NOW when the radio is
     *  open. A choice this radio did not report is refused in words (optionsNote in the view) and its default kept —
     *  never guessed. Survives close()/open() on the same object (the watchdog's and lend/take-back's reopen). */
    void setOptions(const usrpcaps::Options& o);
    usrpcaps::Options options() const;
    /** What the open radio reported it can do — all false / empty before open. */
    usrpcaps::OptionCaps optionCaps() const;
    /** The options half of the hwinfo view: capability, the radio's own answer now (sensors read, cached ~3 s), and any
     *  refusal. */
    void fillOptionView(usrpcaps::CapsView& c) const;

private:
    bool rebuildStream(std::string& err);   // (re)make the RX streamer for the current channel
    void applyOptionsLocked(bool atOpen);   // ★ caller holds impl_->mtx and the radio is made
    struct Impl;
    Impl*   impl_ = nullptr;
    IqSink  sink_;
    bool    open_ = false;
    bool    wantStreaming_ = false;
    double  fmin_ = 0, fmax_ = 0;
    std::vector<uint32_t>    rates_;
    std::vector<int>         gains_;
    std::vector<std::string> antennas_;
    std::string model_, serial_;
    int     usbSpeed_ = 0;
    /* ★★★ GAIN STARTS AT ZERO, AGC OFF, UNTIL THE CALLER SAYS OTHERWISE — the HackRF's hardware-safety default
     *     (Stuart: those front ends "have a bad habit of blowing up their preamps"). A B2xx's LNA is just as
     *     exposed; approaching a working gain from the quiet side is the only safe direction on an unknown aerial. */
    int     gainTenth_ = 0;
    bool    hasAgc_ = false, agcOn_ = false;
};

#else   // ── no UHD in this build: the inline stub (armhf, i386, Android, iOS) ─────────────────────────────────────

class UsrpSource {
public:
    using IqSink = std::function<void(const float*, int)>;
    static constexpr bool compiledIn() { return false; }
    static int  deviceCount() { return 0; }
    static std::string deviceName(int) { return ""; }
    static std::string deviceSerial(int) { return ""; }
    bool open(int, double, double, int, std::string& err) { err = "this VibeServer build has no USRP support"; return false; }
    void close() {}
    bool isOpen() const { return false; }
    void setSink(IqSink) {}
    bool start(std::string& err) { err = "no USRP support"; return false; }
    void stop() {}
    void setFrequency(double) {}
    double freqMinHz() const { return 0; }
    double freqMaxHz() const { return 0; }
    const std::vector<uint32_t>& sampleRates() const { static const std::vector<uint32_t> e; return e; }
    uint32_t nearestRate(double) const { return 0; }
    bool setSampleRate(double) { return false; }
    const std::vector<int>& gainListTenthDb() const { static const std::vector<int> e; return e; }
    void setGainTenthDb(int) {}
    int  gainTenthDb() const { return 0; }
    bool hasHwAgc() const { return false; }
    bool hwAgc() const { return false; }
    const std::vector<std::string>& antennas() const { static const std::vector<std::string> e; return e; }
    std::string antenna() const { return ""; }
    bool setAntenna(const std::string&) { return false; }
    std::string model() const { return ""; }
    std::string serial() const { return ""; }
    int  usbSpeed() const { return 0; }
    uint64_t takeOverflows() { return 0; }
    double secondsSinceLastRx() const { return 1e9; }
    void setPaused(bool) {}
    void setOptions(const usrpcaps::Options&) {}
    usrpcaps::Options options() const { return {}; }
    usrpcaps::OptionCaps optionCaps() const { return {}; }
    void fillOptionView(usrpcaps::CapsView&) const {}
};

#endif

}  // namespace vibe
