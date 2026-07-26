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

    void setSink(IqSink sink) { sink_ = std::move(sink); }
    /** Set when the device disappears — the shim's watchdog polls this exactly as it does
     *  for a dongle that has been unplugged. */
    bool deviceLost() const { return lost_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    IqSink sink_;
    bool open_ = false;
    bool lost_ = false;
};

}  // namespace vibe
