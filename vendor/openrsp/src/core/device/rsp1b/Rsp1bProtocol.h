// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the OpenRSP authors
//
// This file is part of OpenRSP.
//
// OpenRSP is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, either version 3
// of the License, or (at your option) any later version.
//
// OpenRSP is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with OpenRSP. If
// not, see <https://www.gnu.org/licenses/>.

// RSP1B hardware protocol engine (pure, portable). It issues the
// control-transfer sequences for Init (stream state,
// hardware init, start), Uninit, Update (every reason bit, incl. other models'
// bits, which are also executed on an RSP1B), InternalUpdate,
// Release and the AGC gain hand-off, plus the front-end (MCP23017) shadow
// registers, toggle bookkeeping, GainChange events and gainVals.
//
// It never touches a device: every hardware action goes through the Io sink,
// which the service implements with libusb and unit tests implement as a
// recorder fed with canned device answers.
// Reads (FNADDR, tuner trim readback, first-block toggle states) are inputs
// supplied by the Io sink.
//
// Threading: the engine is not internally locked. The service calls the API
// entry points with the API mutex held, and the stream thread
// reports confirmed toggles through onPacketToggles() under the same lock.
#pragma once

#include "core/log/Here.h"
#include "core/log/Logger.h"

#include "core/api/ApiTypes.h"
#include "core/device/common/CommonRspProtocol.h"
#include "core/device/rsp1b/Rsp1bCalc.h"

#include <array>
#include <cstdint>
#include <string>

namespace sdrplay {
namespace device {
namespace rsp1b {

// One USB control transfer as data. For IN transfers the Io fills data[0..wLength).
struct ControlTransfer {
    std::uint8_t bmRequestType = 0;
    std::uint8_t bRequest = 0;
    std::uint16_t wValue = 0;
    std::uint16_t wIndex = 0;
    std::uint16_t wLength = 0;
    std::array<std::uint8_t, 16> data{};
    bool isIn() const { return (bmRequestType & 0x80) != 0; }
};

// Device-specific control requests (bmRequestType 0x40 out / 0xC0 in)
enum : std::uint8_t {
    kReqWriteReg = 0x41,
    kReqReadReg = 0x42,
    kReqStart = 0x43,
    kReqStop = 0x45,
    kReqEepromRead = 0x46,
    kReqFnAddr = 0x48,
    kReqGpioExt = 0x49,
    kReqGpioDual0 = 0x4A,
    kReqGpioDual1 = 0x4B,
    kReqSetInterface = 0x0B, // standard SET_INTERFACE (bmRequestType 0x01)
};

// Register numbers, fixed register values and front-end shadow bits, shared by the
// RSP1B, RSP1A / RSP1 and RSP2 engines.
namespace hw {

// Bridge registers (setRegister; bits 7:5 of the register byte carry the sync-update flag).
enum : std::uint8_t {
    kRegBridge0 = 0x00,
    kRegBridge2 = 0x02,
    kRegAdcPll = 0x03,       // ADC PLL word (AdcPll::reg3)
    kRegAdcFrac = 0x04,      // ADC PLL FRAC (AdcPll::reg4)
    kRegBridge5 = 0x05,
    kRegPacketFormat = 0x07, // FsState::reg7
    kRegBridgeGpio = 0x08,   // bridge GPIO configuration
    kRegTuner = 0x09,        // tuner serial word: bits 3:0 tuner register, 23:4 payload
    kRegSyncPeriod = 0x12,
    kRegSyncSampleLo = 0x13,
    kRegSyncSampleHi = 0x14,
};

// Fixed register values of the Init / Uninit sequences.
constexpr std::uint32_t kBridge0FirstInit = 0x000200;  // first Init after Select only
constexpr std::uint32_t kBridge2FirstInit = 0x004801;  // first Init after Select only
constexpr std::uint32_t kBridge5Open = 0x00000C;       // first Init after Select
constexpr std::uint32_t kBridge5Closed = 0x000004;     // bridgeUninit
constexpr std::uint32_t kAdcPllUninit = 0x091300;      // bridgeUninit
constexpr std::uint32_t kAdcPllFsChange = 0x011300;    // before a packet-format change
constexpr std::uint32_t kTunerReg5Init = 0x28BB85;     // tuner register 5 (initHardware)
constexpr std::uint32_t kTunerReg13Clear = 0x00000D;   // tuner trim register 13, no trims
constexpr std::uint32_t kTunerReg14Clear = 0x00000E;   // tuner trim register 14, no trims
constexpr std::uint32_t kTunerReadbackStart = 0x00001C;
constexpr std::uint32_t kTunerReadbackEnd = 0x000000;
constexpr std::uint32_t kTunerReg0Uninit = 0x063000;   // tuner register 0 (bridgeUninit)
constexpr std::uint32_t kTunerReg14Uninit = 0x00800E;  // tuner register 14 (bridgeUninit)

// Tuner register 6, DC offset calibration: register number 6 in bits 3:0, trackTime from
// bit 4, refreshRateTime from bit 10, and bit 21 always set.
constexpr std::uint32_t kTunerReg6 = 0x000006;
constexpr std::uint32_t kTunerReg6Bit21 = 0x200000;
inline std::uint32_t tunerDcWord(int refreshRateTime, int trackTime)
{
    return static_cast<std::uint32_t>(refreshRateTime) << 10 | static_cast<std::uint32_t>(trackTime) << 4 |
           kTunerReg6Bit21 | kTunerReg6;
}

// MCP23017-style expander registers (kReqGpioExt / kReqGpioDual0 / kReqGpioDual1).
enum : std::uint8_t { kExpIodirA = 0x00, kExpIodirB = 0x01, kExpGpioA = 0x12, kExpGpioB = 0x13 };

// Expander output shadows. Update runs every model's front-end handler on every model,
// so the engine keeps a shadow for each of them.
struct GpioShadows {
    std::uint8_t a = 0, b = 0;         // RSP1A / RSP1B GPIOA / GPIOB
    std::uint8_t rsp2A = 0, rsp2B = 0; // RSP2 GPIOA / GPIOB
    std::uint8_t duo1A = 0;            // RSPduo expander 0 GPIOA
    std::uint8_t duo2A = 0, duo2B = 0; // RSPduo expander 1 GPIOA / GPIOB
    std::uint8_t dx = 0;               // RSPdx GPIOA
};

// Front-end shadow bits. An "Off" bit set switches the function off (active-low enables).
enum : std::uint8_t {
    // RSP1A GPIOA / GPIOB (the RSP1B shares kRsp1aRfTest*)
    kRsp1aAttMask = 0xE0,          // GPIOA 7:5 step attenuator
    kRsp1aBandMask = 0x1F,         // GPIOA 4:0 band code
    kRsp1aDabNotchOff = 0x01,      // GPIOB 0
    kRsp1aPad = 0x02,              // GPIOB 1, 20 dB pad
    kRsp1aBroadcastNotchOff = 0x04,// GPIOB 2
    kRsp1aRfTestOff = 0x08,        // GPIOB 3
    kRsp1aBiasTOff = 0x10,         // GPIOB 4
    kRsp1aRfTestSrcStability = 0x20, // GPIOB 5
    // RSP1B GPIOA / GPIOB
    kRsp1bBiasTOff = 0x02,         // GPIOA 1
    kRsp1bBroadcastNotchOff = 0x10,// GPIOA 4
    kRsp1bPad = 0x20,              // GPIOA 5, 20 dB pad
    kRsp1bDabNotchOff = 0x40,      // GPIOA 6
    kRsp1bBandA = 0x80,            // GPIOA 7 band bit
    kRsp1bAttMask = 0x1C,          // GPIOB 4:2 step attenuator
    kRsp1bBandBMask = 0xC3,        // GPIOB 7:6 and 1:0 band bits
    // RSP2 GPIOA / GPIOB
    kRsp2AntennaMask = 0x07,       // GPIOA 2:0 antenna select (5 = A, 6 = B)
    kRsp2AntennaNone = 0x00,       // antenna field (and antennaSel) after an invalid selection
    kRsp2BandMask = 0x38,          // GPIOA 5:3 filter bank
    kRsp2ExtRefOff = 0x40,         // GPIOA 6
    kRsp2BiasTOff = 0x01,          // GPIOB 0
    kRsp2AttMask = 0x3C,           // GPIOB 5:2 step attenuator
    kRsp2NotchMask = 0xC0,         // GPIOB 7:6 RF notch: 10 off, 01 on
    kRsp2NotchOffBits = 0x80,
    kRsp2NotchOnBits = 0x40,
    // RSPduo expander 0 GPIOA, expander 1 GPIOA / GPIOB
    kDuoExtRefOff = 0x20,          // duo1A 5
    kDuoRfTestOff = 0x04,          // duo2A 2
    kDuoTuner1AmNotchOff = 0x08,   // duo2A 3
    kDuoBroadcastNotchAOff = 0x10, // duo2A 4
    kDuoDabNotchAOff = 0x40,       // duo2A 6
    kDuoBroadcastNotchBOff = 0x80, // duo2A 7
    kDuoDabNotchBOff = 0x02,       // duo2B 1
    kDuoPadA = 0x20,               // duo2A 5, tuner 1 20 dB pad
    kDuoPadB = 0x01,               // duo2B 0, tuner 2 20 dB pad
    kDuoAttStepAMask = 0x1C,       // duo2B 4:2 tuner 1 step attenuator
    kDuoAttStepBMask = 0xE0,       // duo2B 7:5 tuner 2 step attenuator
    kDuoBandAHighBits = 0x1C,      // duo1A 4:2, tuner 1 band-code bits 4:2
    kDuoBandAAttPath = 0x02,       // duo1A 1, tuner 1 band-code bit 1
    kDuoBandBHighBits = 0x38,      // duo1B 5:3, tuner 2 band-code bits 4:2
    kDuoBandBAttPath = 0x04,       // duo1B 2, tuner 2 band-code bit 1
    // RSPdx GPIOA
    kDxBiasTOff = 0x02,            // 1
    kDxAntennaMask = 0x0C,         // 3:2 antenna select
    kDxAntennaB = 0x04,
    kDxAntennaA = 0x08,
    kDxBroadcastNotchOff = 0x10,   // 4
    kDxDabNotchOff = 0x40,         // 6
};

// v with the bits of `bits` set (on) or cleared (off).
inline std::uint8_t withBits(std::uint8_t v, std::uint8_t bits, bool on)
{
    return static_cast<std::uint8_t>(on ? (v | bits) : (v & ~bits));
}
// v with the field `mask` replaced by `value` (value must lie inside mask).
inline std::uint8_t withField(std::uint8_t v, std::uint8_t mask, std::uint8_t value)
{
    return static_cast<std::uint8_t>((v & ~mask) | value);
}

} // namespace hw

// Streaming actions that are not control transfers.
enum class StreamOp {
    Init,      // submit 8 transfers for the current alt
    Uninit,    // cancel and drain
    Flush,     // bulk flush (up to 32 x 1024-byte bulk reads, 100 ms)
    ResetDevice, // device reset when it reports function address 2 (not expected from current firmware)
};

struct ToggleBits {
    bool gr = false, rf = false, fs = false; // header bits 13/14/15 of packet 383
};

// How the LOG line that accompanies an error record is written.
enum class LogAs { Error, Warning, Verbose, None };

struct ErrorRecord {
    const char *file = "";
    const char *function = "";
    int line = 0;
    std::string message;
    LogAs logAs = LogAs::Error;
    // A LOG line only (no error record), at logLevel: LOG lines that are not followed
    // by a record, e.g. the RSPduo programGain limits line.
    bool logOnly = false;
    log::Level logLevel = log::Level::Error;
};

// Hardware changes whose completion the stream confirms through a packet header toggle.
enum class Change { Gr, Rf, Fs };

struct GainChangeEvent {
    int tuner = 1;
    unsigned gRdB = 0;
    unsigned lnaGRdB = 0;
    double currGain = 0.0;
};

class Io {
public:
    virtual ~Io() = default;
    // One control transfer (1000 ms timeout). 0 = success (actual length == wLength), else failure.
    virtual int control(ControlTransfer &t) = 0;
    virtual int stream(StreamOp op) { (void)op; return 0; }
    // The first stream read after a start (resyncToggles): 384 KiB; 0 on success with the
    // toggle bits of the last packet, nonzero when no data arrived.
    virtual int readFirstBlock(ToggleBits &t) { t = ToggleBits{}; return 0; }
    virtual void sleepMs(unsigned ms) { (void)ms; }
    virtual void gainChange(const GainChangeEvent &e) { (void)e; }
    virtual void overloadCorrected(int tuner) { (void)tuner; } // PowerOverloadChange{Corrected} at Uninit
    virtual void error(const ErrorRecord &r) { (void)r; }
    // Stream-side bookkeeping (service wiring, Rsp1bDevice): a change was
    // programmed and stays pending until a packet header shows `toggle`
    // (report: the change is to be reported to the user when confirmed. Gr: programGain ran for a
    // user request; Rf: the Update's rf flag consumed by programRf; Fs: the Update's fs flag
    // consumed by programSampleRate);
    // a pending change was dropped (dropPendingUpdates, resyncToggles); programSyncUpdate
    // deferred its sample-number write until the counter is past the wrap.
    virtual void armed(Change c, bool toggle, bool report) { (void)c; (void)toggle; (void)report; }
    virtual void disarmed(Change c) { (void)c; }
    virtual void syncDeferred() {}
};

// Requests to the DSP / AGC side produced by Init/Update (consumed by the stream thread).
struct DspRequests {
    bool streamRestarted = false;   // Dev_Fs restart: next block reset=1, forced fsChanged
    bool downConverterReinit = false;
    bool dcIqReset = false;
    bool decimationChanged = false;
    bool agcChanged = false;        // Ctrl_Agc: recompute coefficients
    bool agcForced = false;         // AGC must apply a gain step (enable 0->x, user Gr with AGC on)
    bool agcUserRequest = false;    // ... and report it as user-originated (grChanged)
    bool overloadAck = false;
};

class Protocol : public common::ProtocolBase {
public:
    // dev/rx/internal point to the shared parameter storage (CmdMap); they are
    // latched at Init/Update entry, and outputs
    // (gainVals, gRdB, resetFlags) are written back to them.
    Protocol(sdrplay_api_DevParamsT *dev, sdrplay_api_RxChannelParamsT *rx, sdrplay_api_InternalRfTestT *internal,
             Io &io);

    // SelectDevice: device-object reset (all shadows 0, first Init after select
    // pending). No USB traffic.
    void select();

    // Service-level result codes (sdrplay_api_ErrT values). The client maps any
    // nonzero Init result to Fail (1).
    int init();
    int uninit() { return uninitImpl(true); }
    int update(int tuner, std::uint32_t reason, std::uint32_t ext1);
    int internalUpdate(std::uint32_t reason);
    // ReleaseDevice / device destruction: locked Uninit if initialised (no overload
    // event), then SET_INTERFACE 0 if the bridge is still open (failed Init).
    void release();

    // AGC hand-off: programGain(gRdB) without the API validation (configureAgc path).
    int applyAgcGain(int gRdB);

    // Stream-thread hooks: packet header toggles (confirms pending updates).
    void onPacketToggles(const ToggleBits &t);
    // Packet reader: a sync update deferred near the sample-counter wrap is
    // re-issued (programSyncUpdate again) once a header sampleNum is below 2^31, i.e. the
    // counter has wrapped.
    int onPacketSampleNum(std::uint32_t sampleNum);
    void confirmAllPending(); // test helper: every pending update has been seen
    void confirmGrPending() { grPending_ = false; } // test helper: the gain toggle has been seen

    // ---- state for the service / DSP side
    bool initialised() const { return state_ != common::kStateIdle; }
    int state() const { return state_; }
    // initFailStage() / updateFailStep(): common::ProtocolBase
    const FsState &fsState() const { return fs_; }
    const BandState &band() const { return band_; }
    DownConv downConv() const;
    int userDecimShift() const { return decimShift_; }
    double mixHz() const { return mix_; }
    std::uint8_t shadowA() const { return gpio_.a; }
    std::uint8_t shadowB() const { return gpio_.b; }
    int appliedGr() const { return appliedGr_; }
    DspRequests takeDspRequests();
    const DspRequests &dspRequests() const { return dsp_; }

private:
    // primitives
    int ctl(std::uint8_t bmRT, std::uint8_t req, std::uint16_t wValue, std::uint16_t wIndex, std::uint16_t wLength,
            std::uint8_t *in = nullptr);
    int setRegister(std::uint8_t regByte, std::uint32_t value);
    int tunerWrite(std::uint32_t word, std::uint8_t syncFlag = 0)
    {
        return setRegister(static_cast<std::uint8_t>((syncFlag & 7) << 5 | hw::kRegTuner), word);
    }
    int gpioWrite(std::uint8_t reg, std::uint8_t val);
    int gpioDualWrite(int devId, std::uint8_t reg, std::uint8_t val);
    int setInterface(std::uint8_t alt);
    int startRx();
    int stopRx();
    void err(const char *file, const char *fn, int line, std::string msg, LogAs as = LogAs::Error);
    void errWarning(const char *file, const char *fn, int line, std::string msg)
    {
        err(file, fn, line, std::move(msg), LogAs::Warning);
    }
    void errVerbose(const char *file, const char *fn, int line, std::string msg)
    {
        err(file, fn, line, std::move(msg), LogAs::Verbose);
    }
    void errQuiet(const char *file, const char *fn, int line, std::string msg)
    {
        err(file, fn, line, std::move(msg), LogAs::None);
    }

    // Init / Update / Uninit building blocks
    int uninitImpl(bool apiCall);
    void latchDev();
    void latchChannel();
    int bridgeInit();
    void bridgeUninit();
    int initHardware();
    int start(bool toggles);
    void stopStream();
    int resyncToggles();
    int programSampleRate();
    int writeRate();
    int programRf();
    int writeRf();
    int programGain(bool fromAgc);
    int writeGain();
    int applyTunerDc();
    int applyLoMode();
    int selectBand(BandState &out);
    // Band program: tuner r0, front end, synthesiser. bwUpdate (Update(Tuner_BwType)): r0
    // with the LO code bits when the LO flag is set, then the synthesiser only; otherwise
    // r0 without them, the synthesiser and either the LO-flag trim sequence or the
    // cleared trim registers 13 / 14.
    int programBand(bool bwUpdate);
    int readTunerTrims(std::uint32_t &v);
    void dropPendingUpdates();
    int programSyncUpdate();
    int applyPpm();
    int configureAgc();
    void updateDecimation();
    void configureDcIq();
    void configureDownConverter();
    void computeGainVals(int gApplied, int lnaState, GainVals &gv) const;

    // RSP1B front end (shadow A = GPIOA 0x12, B = GPIOB 0x13)
    int rsp1bBiasT(bool write, bool forceOff);
    int rsp1bDabNotch(bool write);
    int rsp1bBroadcastNotch(bool write);
    int rsp1aRfTest(bool write);
    // other models' handlers (run for every model, no model check)
    int rsp2BiasT(bool write);
    int rsp2Antenna(bool write);
    int rsp2RfNotch(bool write);
    int rsp2ExtRef(bool write);
    int rsp2AmPort();
    int rspDxBiasT(bool write);
    int rspDxAntenna(bool write);
    int rspDxBroadcastNotch(bool write);
    int rspDxDabNotch();
    int rspDuoExtRef(bool write);
    int rspDuoTuner1AmNotch(int tuner, bool write);
    int rspDuoBroadcastNotch(int tuner, bool write);
    int rspDuoDabNotch(int tuner, bool write);
    int rspDuoRfTest(bool write);

    sdrplay_api_DevParamsT *devShared_;
    sdrplay_api_RxChannelParamsT *rxShared_;
    sdrplay_api_InternalRfTestT *internalShared_;
    Io &io_;

    // latched copies
    sdrplay_api_DevParamsT dev_{};
    sdrplay_api_RxChannelParamsT ch_{};
    sdrplay_api_InternalRfTestT internal_{};

    common::ProtocolState state_ = common::kStateIdle;
    bool bridgeOpen_ = false;         // bridge opened and not yet closed
    bool fnAddrDone_ = false;         // first Init after Select done
    hw::GpioShadows gpio_{};

    FsState fs_{};
    BandState band_{};
    int firstLO_ = 0;
    bool loFlag_ = false;
    double mix_ = 0.0;
    // Lif flag and IF Hz of the last configureDownConverter, as the
    // ifType code reg0Word() takes: 0 = zero IF, 2048/1620/450, -1 = low IF with no IF bits
    // (any other ifType). programBand builds tuner r0 from these, not from the
    // ifType latched by the current Update.
    int dcIfType_ = 0;
    double fSynth_ = 0.0;             // last programmed synth, Hz
    std::uint8_t dcCalApplied_ = 0, speedUpApplied_ = 0;
    int appliedGr_ = -1;
    int totalGr_ = 0;
    int decimShift_ = 0;
    // toggles / pending
    bool rfToggle_ = false, fsToggle_ = false, grToggle_ = false;
    bool rfPending_ = false, fsPending_ = false, grPending_ = false;
    std::uint32_t syncPeriod_ = 0;
    static constexpr std::uint32_t kNoSyncDeferred = 0xFFFFFFFFu;
    std::uint32_t syncDeferred_ = kNoSyncDeferred; // sample number of a deferred sync update
    bool grUserFlag_ = false;
    // report requests consumed by the next successful programRf / programSampleRate:
    // set by Update(Tuner_Frf) / Update(Dev_Fs) right before programming
    bool rfUser_ = false, fsUser_ = false;
    DspRequests dsp_{};
    int agcEnableState_ = 0;
    int curTuner_ = 1;                // tuner value used in messages/events
};

} // namespace rsp1b
} // namespace device
} // namespace sdrplay
