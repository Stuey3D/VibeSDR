// Airspy R2 / Mini capture source for VibeServer.
//
// ★★★ A DIFFERENT LIBRARY FROM THE HF+, AND A DIFFERENT RADIO. The HF+ speaks libairspyhf; an
// R2 or Mini speaks libairspy (airspy/airspyone_host). Both are VENDORED and both are
// permissive — libairspy's airspy.c/.h/airspy_commands.h are BSD-3-Clause and its
// iqconverter*/filters.h are MIT (libairspy/LICENSE). The note in airspyhf_source.h claiming
// libairspy is GPL-2.0 was wrong and is corrected there; the licence file says otherwise, which
// is what made this driver possible (Stuart, 2026-09-22, for a user request).
// ★ BSD clause 3: the Airspy name may not be used to endorse or promote VibeSDR. Naming the
//   hardware we support is fine; implying Airspy endorses us is not. Both notices ship in the
//   app's credits.
//
// ★★ UPSTREAM ALREADY HAS THE ANDROID DOOR. libairspyhf had to be patched to add an fd entry
// point; libairspy ships airspy_open_fd() on libusb_wrap_sys_device(), so this one is vendored
// UNMODIFIED — which is the licence's "redistribution in source form" case and nothing more.
//
// Shape, against the other sources:
//   1. Samples arrive as INTERLEAVED COMPLEX FLOAT (AIRSPY_SAMPLE_FLOAT32_IQ), already the
//      engine's native format — as the HF+ does, and unlike the dongle's int16.
//   2. Streaming is a callback started with airspy_start_rx(), not a blocking read loop.
//   3. Gain is THREE stages (LNA, mixer, VGA), or one of two 0-21 PRESET curves that set all
//      three: "linearity" and "sensitivity". The single slider every client has drives the
//      linearity curve — see setGainTenthDb.
//
// ★ COVERAGE: 24 - 1800 MHz. No HF without an upconverter, and NO direct-sampling branch — so
//   unlike a dongle there is nothing to switch out below 24 MHz, and the app must not offer it.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct airspy_device;

namespace vibe {

class AirspySource {
public:
    /** Interleaved complex float IQ, +/-1.0 nominal — the engine's native format. */
    using IqSink = std::function<void(const float* interleavedIq, int sampleCount)>;

    AirspySource();
    ~AirspySource();
    AirspySource(const AirspySource&) = delete;
    AirspySource& operator=(const AirspySource&) = delete;

    static int  deviceCount();
    static std::string deviceName(int index);
    /** ★ 24-1800 MHz, and nothing below it. Asked rather than discovered as silence. */
    static bool tuneRangeContains(double hz);

    bool open(int index, double sampleRateHz, double centreHz, int gainTenthDb, std::string& err);
    /** ★★ Android hands out an already-open USB fd and forbids enumeration; libusb takes
     *  ownership of it. Upstream libairspy provides this entry point (see the header note). */
    bool openFd(int fd, double sampleRateHz, double centreHz, int gainTenthDb, std::string& err);
    void close();
    bool isOpen() const { return open_; }

    void setSink(IqSink sink) { sink_ = std::move(sink); }
    bool start(std::string& err);
    void stop();

    void setFrequency(double hz);

    /** The rates THIS radio offers, ascending, enumerated from the device: a Mini does
     *  3/6 MSPS and an R2 2.5/10, so a hard-coded list would offer rates it cannot do. */
    const std::vector<uint32_t>& sampleRates() const { return rates_; }
    uint32_t nearestRate(double hz) const;
    bool setSampleRate(double hz);

    /** ★ The one slider every client has, mapped onto the LINEARITY preset (0-21) — Airspy's own
     *  recommended curve for general listening. tenths of a dB in, 0..210, so the existing
     *  "gain in tenths" contract is unchanged; each step is one preset position.
     *  ★ Negative = the radio's own LNA+mixer AGC, which is the nearest thing an R2/Mini has to
     *    "auto". VibeAGC is RTL-only for now (Stuart, 2026-09-22). */
    void setGainTenthDb(int tenthDb);
    int  gainTenthDb() const { return gainTenth_; }
    /** The pseudo gain list the clients' slider is drawn from: 0, 10, … 210 (the 22 presets). */
    static std::vector<int> gainListTenthDb();

    // ── Airspy R2 / Mini specific ───────────────────────────────────────────
    /** Which preset curve the slider drives: false = linearity (default), true = sensitivity. */
    void setSensitivityCurve(bool sensitivity);
    bool sensitivityCurve() const { return sensitivity_; }
    /** Manual stages, each 0-15. Setting any of them leaves preset mode. */
    void setLnaGain(int v);
    void setMixerGain(int v);
    void setVgaGain(int v);
    int  lnaGain()   const { return lna_; }
    int  mixerGain() const { return mixer_; }
    int  vgaGain()   const { return vga_; }
    /** The radio's own per-stage AGC. */
    void setLnaAgc(bool on);
    void setMixerAgc(bool on);
    bool lnaAgc()   const { return lnaAgc_; }
    bool mixerAgc() const { return mixerAgc_; }
    /** 4.5 V on the aerial socket. Off at open, as every other source here. */
    void setBiasTee(bool on);
    bool biasTee() const { return bias_; }
    /** 12-bit sample packing over USB: less bandwidth, a little more CPU. */
    void setPacking(bool on);
    bool packing() const { return packing_; }

    /** ★ Called by the library's RX callback (a free function in the .cpp — its signature needs
     *  libairspy's own types, which this header deliberately does not include). */
    void deliver(const float* iq, int sampleCount) { if (sink_) sink_(iq, sampleCount); }

    const std::string& model()  const { return model_; }
    const std::string& serial() const { return serial_; }

private:
    bool finishOpen(double sampleRateHz, double centreHz, int gainTenthDb, std::string& err);
    void applyGain();

    airspy_device* dev_ = nullptr;
    IqSink sink_;
    std::vector<uint32_t> rates_;
    std::string model_  = "Airspy";
    std::string serial_;
    double centreHz_ = 100e6;
    int  gainTenth_  = 150;      // ★ preset 15 of 21 — Airspy's own "start here" for linearity
    bool sensitivity_ = false;
    int  lna_ = -1, mixer_ = -1, vga_ = -1;   // -1 = preset mode, no manual stage set
    bool lnaAgc_ = false, mixerAgc_ = false;
    bool bias_ = false, packing_ = false;
    bool open_ = false, streaming_ = false;
};

} // namespace vibe
