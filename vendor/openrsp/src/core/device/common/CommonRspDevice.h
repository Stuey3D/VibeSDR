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

// Service-device wiring shared by every RSP model.
//
// The stream thread, DSP, AGC, Update and LOG behaviour is the same for every
// model; only the hardware programming (the per-model protocol engines and their tables) and a
// few model specifics differ. This template is that shared code; it was validated on a real
// RSP1B (control-transfer protocol, packet chunking, LOG lines, per-setter change-flag
// reports). Every model device (Rsp1bDevice, Rsp1aDevice,
// Rsp2Device, RspdxDevice, RspDuoDevice) derives from it and supplies:
//   - its protocol engine (Proto: rsp1b/rsp1a/rsp2/rspdx/rspduo::Protocol), created by the
//     derived constructor through setProtocol();
//   - CRTP hooks (Derived may hide any of the defaults below; they are resolved statically):
//       protoUpdate / protoAgcGain / protoAppliedGr / protoSampleNumSync  engine call shapes
//       liveChannel()        the rx channel the device runs (RSPduo: tuner B or the master map)
//       logTuner()           the tuner number in Init's LOG lines
//       overloadTuner()      tuner argument of PowerOverloadChange
//       agcMode()            rspDuoMode argument of the AGC set-point limit
//       hdrActive()          RSPdx HDR: no AGC, no dcCal re-trigger, HDR overload variant
//       trackArms()          the device does the change-flag bookkeeping (RSPduo: Single only)
//       latchDspForUpdate()  Update re-latches the DSP channel (RSPduo: per tuner channel)
//       configureDspAtInit() / applyDspRequests()  the DSP side of Init / Update (RSPduo non-Single)
//       logRecordModel()     model-specific LOG text/level of an error record (true = handled)
//       uninitIdle()         Uninit's "Already uninitialised" test (+ uninitClearsDsp)
//       blockProcessed()     per-block bookkeeping under the engine mutex (RSPdx HDR tracker)
//       modelDownConvert()   a model's own down-converter (RSPdx HDR decimators)
//       streamOther()        a stream iteration outside the per-block pipeline (RSPduo Dual/Master)
//
// Locking: one engine mutex guards the protocol, the stream
// processor and the AGC. API calls hold it for their whole duration except while the protocol
// polls a pending change (writeRf/Fs/Gr 25 ms sleeps), so the DSP thread can confirm the
// toggle meanwhile. The DSP thread holds it for the processing of one block, which makes every
// Update land between two blocks. Events and error records produced on the DSP
// thread are delivered after the engine mutex is released (record writes take
// DeviceContext::recordMutex, the mutex the command thread holds while it serves device
// GetLastError).
#pragma once

#include "core/api/LastError.h"
#include "core/device/DefaultParams.h"
#include "core/device/Device.h"
#include "core/device/common/CommonRspProtocol.h"
#include "core/device/rsp1b/Rsp1bProtocol.h"
#include "core/dsp/DspCommon.h"
#include "core/dsp/StreamProcessor.h"
#include "core/dsp/agc/Agc.h"
#include "core/dsp/agc/PowerEstimation.h"
#include "core/log/Logger.h"
#include "platform/Clock.h"
#include "platform/Thread.h"
#include "platform/Usb.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sdrplay {
namespace device {
namespace common {

// Enum fields live in shared memory that applications write freely: read their raw 32-bit
// value (an out-of-range enum load is undefined behaviour).
template <typename E> int raw(const E &e)
{
    static_assert(sizeof(E) == 4, "API enums are 32-bit");
    std::int32_t v;
    std::memcpy(&v, &e, sizeof v);
    return v;
}

constexpr unsigned kFirstBlockPolls = 1000;  // resyncToggles: 1 ms polls for the first block (1 s timeout)
constexpr std::uint32_t kFallbackPacketsPerBlock = 4; // packets per block if the processor gives 0 or too many
constexpr std::uint32_t kSlotWaitMs = 100;   // output ring: 10 x 10 ms
constexpr std::uint32_t kReadSliceMs = 100;
constexpr unsigned kPollSleepMs = 25;        // writeRf/Fs/Gr pending poll (eng_ released)
// Toggle value that never matches a header bit: the stream processor has no "disarm";
// dropPendingUpdates / resyncToggles only drop the pending flag.
constexpr std::uint32_t kNeverToggle = 2;

// Protocol::Io control transfers over platform::UsbDevice (1000 ms
// timeout, success iff the transferred length == wLength). SET_INTERFACE
// (bmRequestType 0x01, bRequest 0x0B) goes through UsbDevice::setAltSetting(wIndex, wValue)
// so libusb/the OS keep their interface state (the kernel sends the same standard request).
template <class IoBase> class UsbControlIoT : public IoBase {
public:
    explicit UsbControlIoT(platform::UsbDevice *usb, log::Logger *log = nullptr) : usb_(usb), log_(log) {}
    int control(rsp1b::ControlTransfer &t) override
    {
        if (!usb_) return 1;
        int r;
        if (t.bmRequestType == 0x01 && t.bRequest == rsp1b::kReqSetInterface) {
            r = usb_->setAltSetting(t.wIndex, t.wValue);
            if (r == 0) return 0;
        } else {
            r = usb_->controlTransfer(t.bmRequestType, t.bRequest, t.wValue, t.wIndex,
                                      t.isIn() ? t.data.data() : nullptr, t.wLength, 1000);
            if (r == static_cast<int>(t.wLength)) return 0;
        }
        lastErr_ = r < 0 ? r : -1;
        if (log_) log_->log(log::Level::Verbose, __func__, "Error: libusb_control_transfer() %d", r);
        return 1;
    }
    // Last libusb-style error of a failed transfer (0 if none).
    int lastUsbError() const { return lastErr_; }

protected:
    platform::UsbDevice *usb_;
    log::Logger *log_;
    int lastErr_ = 0;
};

// The LOG line that goes with a device error record (model-independent table).
// Almost every record comes right after a LOG line
// with the same function and text, at log::Level::Error; the record's logAs / logOnly /
// logLevel select the exceptions. `tuner` is the tuner number of Init's "not in
// uninitialised state".
void logRecordDefault(log::Logger &L, const rsp1b::ErrorRecord &r, int tuner);

struct DeviceStats {
    std::uint64_t blocks = 0, published = 0, dropped = 0, readErrors = 0, discarded = 0;
    std::uint64_t agcWrites = 0;
    std::uint64_t headerOnly = 0;   // RSPduo Dual/Master iterations that only confirmed toggles
    std::uint64_t hdrBlocks = 0;    // RSPdx: blocks processed while the DSP is in the HDR path
    std::uint64_t hdrDspResets = 0; // RSPdx: HDR down-converter resets (HdrDspTracker)
};

template <class Derived, class Proto> class CommonRspDevice : public Device {
public:
    using Stats = DeviceStats;

    ~CommonRspDevice() override = default;

    const ModelInfo &model() const override { return *modelByHwVer(hwVer_); }
    void writeDefaults(ipc::CmdMap &map) override
    {
        // Single tuner, not an RSPduo: rspDuoMode 0.
        writeDefaultParams(map, 0, 0.0, false);
    }
    sdrplay_api_ErrT init() override;
    sdrplay_api_ErrT uninit() override;
    sdrplay_api_ErrT update(sdrplay_api_TunerSelectT tuner, sdrplay_api_ReasonForUpdateT reason,
                            sdrplay_api_ReasonForUpdateExtension1T ext1) override;
    sdrplay_api_ErrT internalUpdate(sdrplay_api_ReasonForInternalUpdateT reason) override;
    void releaseHardware() override;
    void runStream(const std::atomic<bool> &kill) override
    {
        while (!kill) {
            if (!self().streamOnce(kReadSliceMs)) platform::sleepMs(1);
        }
    }

    // Protocol engine state (a ProtocolState).
    int state() const
    {
        std::lock_guard<std::mutex> l(eng_);
        return proto_->state();
    }

    // One DSP-thread iteration (tests drive it by hand): waits at most about waitMs for
    // packets. Returns true if a block was processed (published or dropped).
    bool streamOnce(std::uint32_t waitMs = 1000);
    // Packets read from the transport but not yet processed (one read chunk = 384); tests.
    std::size_t queuedPackets() const { return queued_; }

    Stats stats() const
    {
        std::lock_guard<std::mutex> l(eng_);
        return stats_;
    }
    // Test hook (protocol tests): every pending change has been confirmed by the hardware,
    // as when the stream ran between two API calls.
    void debugConfirmAllPending()
    {
        std::lock_guard<std::mutex> l(eng_);
        proto_->confirmAllPending();
        for (Arm &a : arms_) a.pending = false;
        proc_.armGain(kNeverToggle, 0, false, false);
        proc_.armRf(kNeverToggle, 0.0, false);
        proc_.armFs(kNeverToggle, proc_.confirmedFsHz(), false);
    }
    const Proto &protocol() const { return *proto_; }

protected:
    struct Snapshot {
        sdrplay_api_DevParamsT dev{};
        sdrplay_api_RxChannelParamsT rx{};
    };
    struct PendingEvent {
        sdrplay_api_EventT id;
        sdrplay_api_TunerSelectT tuner;
        sdrplay_api_EventParamsT params;
    };
    struct PendingRecord {
        rsp1b::ErrorRecord rec;
        std::uint64_t timeUs;
    };
    struct Arm {
        bool pending = false;
        bool toggle = false;
        std::int32_t grValue = 0;
        double value = 0.0;
    };

    // Protocol::Io of the device: control transfers, stream actions and the stream-side
    // bookkeeping callbacks. IoBase is rsp1b::Io, or a model's Io extension (rspduo::Io).
    template <class IoBase> class DeviceIoT : public UsbControlIoT<IoBase> {
    public:
        explicit DeviceIoT(CommonRspDevice &d) : UsbControlIoT<IoBase>(d.ctx_.usb, d.ctx_.log), d_(d) {}

        int stream(rsp1b::StreamOp op) override
        {
            StreamTransport *s = d_.ctx_.stream;
            switch (op) {
            case rsp1b::StreamOp::Init: {
                if (!s) return 1;
                d_.gen_.fetch_add(1);
                const int r = s->start(d_.proto_->fsState().alt);
                const std::uint32_t g = d_.gen_.fetch_add(1) + 1;
                if (!d_.inUpdate_) d_.flushGen_.store(g);
                return r == 0 ? 0 : 1;
            }
            case rsp1b::StreamOp::Uninit: {
                const std::uint32_t g = d_.gen_.fetch_add(1) + 1;
                if (!d_.inUpdate_) d_.flushGen_.store(g);
                if (s) s->stop();
                return 0;
            }
            case rsp1b::StreamOp::Flush:
                if (s) s->flush();
                return 0;
            case rsp1b::StreamOp::ResetDevice: return s ? s->resetDevice() : 1;
            }
            return 1;
        }

        int readFirstBlock(rsp1b::ToggleBits &t) override
        {
            // resyncToggles: a stream read of 384 packets; the toggles of the last packet
            // read are the device's current state.
            t = rsp1b::ToggleBits{};
            StreamTransport *s = d_.ctx_.stream;
            if (!s) return 1;
            std::lock_guard<std::mutex> rl(d_.readMtx_);
            buf_.resize(std::size_t(dsp::kStreamReadBytes));
            const std::size_t maxPk = dsp::kStreamReadBytes / dsp::kPacketBytes;
            // The stream read copies exactly 384 packets and polls for them kFirstBlockPolls
            // times with a 1 ms sleep (in practice ~2.4 s, sleeps overshoot): keep reading until
            // all of them are in, fail when the polls run out. Nothing is consumed on failure:
            // the packets stay for the packet reader (with an out-of-range Fs, after
            // resyncToggles fails the stream still delivers them).
            std::size_t got = 0;
            if (s->availablePackets() >= 0) {
                for (unsigned polls = 0; s->availablePackets() < static_cast<long>(maxPk); ++polls) {
                    if (polls >= kFirstBlockPolls) return 1;
                    platform::sleepMs(1);
                }
                while (got < maxPk) {
                    const int r = s->read(buf_.data() + got * dsp::kPacketBytes, maxPk - got, 0);
                    if (r <= 0) return 1;
                    got += static_cast<std::size_t>(r);
                }
            } else {
                // transport without a fill level (test doubles): blocking reads, first one bounded
                while (got < maxPk) {
                    const int r = s->read(buf_.data() + got * dsp::kPacketBytes, maxPk - got, kFirstBlockPolls);
                    if (r <= 0) return 1;
                    got += static_cast<std::size_t>(r);
                }
            }
            const int n = static_cast<int>(got);
            // The k packets still queued for the stream thread are replaced by the newest k
            // packets of this read (the queued count stays k).
            if (got == maxPk && d_.queued_ > 0) {
                const std::size_t k = d_.queued_ < maxPk ? d_.queued_ : maxPk;
                std::memcpy(d_.pkts_.data(), buf_.data() + (maxPk - k) * dsp::kPacketBytes, k * dsp::kPacketBytes);
                d_.queued_ = k;
                d_.qhead_ = 0;
            }
            const dsp::PacketHeader h = dsp::parsePacketHeader(buf_.data() + std::size_t(n - 1) * dsp::kPacketBytes);
            t.gr = h.grToggle != 0;
            t.rf = h.rfToggle != 0;
            t.fs = h.fsToggle != 0;
            return 0;
        }

        void sleepMs(unsigned ms) override
        {
            if (d_.apiLock_ && ms >= kPollSleepMs) {
                // a pending change is being polled: let the DSP thread run (and confirm it)
                d_.self().applyDspRequests();
                std::unique_lock<std::mutex> *l = d_.apiLock_;
                l->unlock();
                platform::sleepMs(ms);
                l->lock();
                return;
            }
            platform::sleepMs(ms);
        }

        void gainChange(const rsp1b::GainChangeEvent &e) override
        {
            sdrplay_api_EventParamsT p;
            std::memset(&p, 0, sizeof p);
            p.gainParams.gRdB = e.gRdB;
            p.gainParams.lnaGRdB = e.lnaGRdB;
            p.gainParams.currGain = e.currGain;
            d_.postEvent(sdrplay_api_GainChange, static_cast<sdrplay_api_TunerSelectT>(e.tuner), p);
        }

        void overloadCorrected(int tuner) override
        {
            sdrplay_api_EventParamsT p;
            std::memset(&p, 0, sizeof p);
            p.powerOverloadParams.powerOverloadChangeType = sdrplay_api_Overload_Corrected;
            d_.postEvent(sdrplay_api_PowerOverloadChange, static_cast<sdrplay_api_TunerSelectT>(tuner), p);
        }

        void error(const rsp1b::ErrorRecord &r) override { d_.record(r); }

        // `report` is the per-setter report flag (programGain's user flag; programRf/programSampleRate copy
        // the Update's request flag and clear it). The change is reported
        // in the packet that confirms it, also when that happens while the Update is still
        // programming (the packet reader checks the flag at the
        // confirmation; a later setter of the same kind overwrites the flag).
        void armed(rsp1b::Change c, bool toggle, bool report) override
        {
            if (!d_.self().trackArms()) return; // the protocol's own toggle bookkeeping (RSPduo Dual/Master)
            Arm &a = d_.arms_[static_cast<int>(c)];
            a.pending = true;
            a.toggle = toggle;
            const std::uint32_t t = toggle ? 1u : 0u;
            switch (c) {
            case rsp1b::Change::Gr: {
                // grChanged for user-originated programGain (Update path, AGC application
                // of a user request), never for autonomous AGC steps.
                const bool rep = d_.dspActive_ ? d_.agcReport_ : report;
                d_.agcGr_ = d_.self().protoAppliedGr();
                a.grValue = d_.agcGr_;
                d_.proc_.armGain(t, d_.agcGr_, true, rep);
                break;
            }
            case rsp1b::Change::Rf:
                a.value = d_.cur_.rx.tunerParams.rfFreq.rfHz;
                d_.proc_.armRf(t, a.value, report && !d_.dspActive_);
                break;
            case rsp1b::Change::Fs:
                a.value = d_.cur_.dev.fsFreq.fsHz;
                d_.proc_.armFs(t, a.value, report && !d_.dspActive_);
                break;
            }
        }

        void disarmed(rsp1b::Change c) override
        {
            d_.arms_[static_cast<int>(c)].pending = false;
            switch (c) {
            case rsp1b::Change::Gr:
                d_.proc_.armGain(kNeverToggle, 0, false, false);
                d_.self().gainWriteDropped(); // clears the AGC gain-wait latch (see gainWriteDropped)
                break;
            case rsp1b::Change::Rf: d_.proc_.armRf(kNeverToggle, 0.0, false); break;
            case rsp1b::Change::Fs: d_.proc_.armFs(kNeverToggle, d_.proc_.confirmedFsHz(), false); break;
            }
        }

        void syncDeferred() override { d_.proc_.armDeferredSync(); }

    protected:
        CommonRspDevice &d_;
        std::vector<std::uint8_t> buf_;
    };
    using DeviceIo = DeviceIoT<rsp1b::Io>;

    // DSP hooks of the per-block pipeline
    class Hooks final : public dsp::StreamHooks {
    public:
        explicit Hooks(CommonRspDevice &d) : d_(d) {}

        void powerEstimate(dsp::StreamProcessor &p, const dsp::BlockView &v) override
        {
            dsp::agc::OverloadContext ctx;
            ctx.hdr = d_.self().hdrActive(); // HDR overload variant
            ctx.agcEnable = p.params().agcEnable;
            ctx.lastAgcGr = d_.agc_.lastGr();
            dsp::agc::OverloadMsg m = dsp::agc::OverloadMsg::None;
            d_.lastPow_ = d_.pe_.estimate(v.xi, v.xq, v.n, v.ionly, ctx, &m);
            if (m != dsp::agc::OverloadMsg::None) {
                sdrplay_api_EventParamsT ep;
                std::memset(&ep, 0, sizeof ep);
                ep.powerOverloadParams.powerOverloadChangeType = m == dsp::agc::OverloadMsg::Detected
                                                                     ? sdrplay_api_Overload_Detected
                                                                     : sdrplay_api_Overload_Corrected;
                d_.postEvent(sdrplay_api_PowerOverloadChange, d_.self().overloadTuner(), ep);
            }
        }

        void agc(dsp::StreamProcessor &p, const dsp::BlockView &) override
        {
            if (!d_.agc_.enabled() || d_.agc_.waiting()) return;
            if (d_.self().hdrActive()) return; // HDR: no AGC at all
            const sdrplay_api_RxChannelParamsT *live = d_.self().liveChannel();
            dsp::agc::AgcGainState g;
            g.gRdB = d_.agcGr_;
            g.lnaState = live ? live->tunerParams.gain.LNAstate : d_.cur_.rx.tunerParams.gain.LNAstate;
            g.minGr = raw(live ? live->tunerParams.gain.minGr : d_.cur_.rx.tunerParams.gain.minGr);
            const dsp::agc::AgcDecision dec = d_.agc_.evaluate(d_.lastPow_, g);
            if (!dec.apply) return;
            d_.agcReport_ = dec.reportUser;
            const int rc = d_.self().protoAgcGain(dec.newGRdB);
            d_.agcReport_ = false;
            ++d_.stats_.agcWrites;
            d_.agc_.applied(rc, d_.self().protoAppliedGr(), g.lnaState);
            if (rc == 0) p.setAgcWaitGain(true);
        }

        void dcCalPeriod(dsp::StreamProcessor &) override
        {
            // AGC off and dcCal == 4 -> programGain again with unchanged values
            // (re-triggers the tuner DC calibration).
            if (d_.self().hdrActive()) return; // HDR: no dcCal = 4 re-trigger
            if (d_.cur_.rx.tunerParams.dcOffsetTuner.dcCal != 4) return;
            d_.agcReport_ = false;
            (void)d_.self().protoAgcGain(d_.agcGr_);
        }

        bool deferredSyncUpdate(dsp::StreamProcessor &) override { return d_.self().protoSampleNumSync(); }

        bool downConvert(dsp::StreamProcessor &, std::int16_t *xi, std::int16_t *xq, std::uint32_t n, bool reset) override
        {
            return d_.self().modelDownConvert(xi, xq, n, reset);
        }

        void gainConfirmed(dsp::StreamProcessor &, std::int32_t) override { confirm(rsp1b::Change::Gr); }
        void rfConfirmed(dsp::StreamProcessor &, double) override { confirm(rsp1b::Change::Rf); }
        void fsConfirmed(dsp::StreamProcessor &, double) override { confirm(rsp1b::Change::Fs); }

    private:
        void confirm(rsp1b::Change c)
        {
            // Packet reader: the header toggle of kind c equals the expected one. Only that kind
            // may match: the other bits are given their non-matching values.
            rsp1b::ToggleBits t;
            t.gr = !d_.arms_[static_cast<int>(rsp1b::Change::Gr)].toggle;
            t.rf = !d_.arms_[static_cast<int>(rsp1b::Change::Rf)].toggle;
            t.fs = !d_.arms_[static_cast<int>(rsp1b::Change::Fs)].toggle;
            Arm &a = d_.arms_[static_cast<int>(c)];
            switch (c) {
            case rsp1b::Change::Gr: t.gr = a.toggle; break;
            case rsp1b::Change::Rf: t.rf = a.toggle; break;
            case rsp1b::Change::Fs: t.fs = a.toggle; break;
            }
            (void)d_.proto_->onPacketToggles(t);
            a.pending = false;
        }

        CommonRspDevice &d_;
    };

    CommonRspDevice(const DeviceContext &ctx, std::uint8_t hwVer) : ctx_(ctx), hwVer_(hwVer)
    {
        hooks_.reset(new Hooks(*this));
        scratchI_.assign(dsp::kMaxBlockSamples, 0);
        scratchQ_.assign(dsp::kMaxBlockSamples, 0);
        // one read chunk (384 packets) on top of a partly consumed block
        pkts_.assign((std::size_t(dsp::kStreamReadBytes) / dsp::kPacketBytes + dsp::kMaxPacketsPerBlock) *
                         dsp::kPacketBytes,
                     0);
    }

    // Derived constructors: the device Io (DeviceIo, or a model extension of DeviceIoT), then
    // the engine built over it.
    void setIo(std::unique_ptr<rsp1b::Io> io) { io_ = std::move(io); }
    rsp1b::Io &io() { return *io_; }
    void setProtocol(std::unique_ptr<Proto> p) { proto_ = std::move(p); }

    Derived &self() { return static_cast<Derived &>(*this); }
    const Derived &self() const { return static_cast<const Derived &>(*this); }

    // ---- CRTP hooks (defaults; see the header comment)
    int protoUpdate(int tuner, std::uint32_t reason, std::uint32_t ext1) { return proto_->update(tuner, reason, ext1); }
    int protoAgcGain(int gRdB) { return proto_->applyAgcGain(gRdB); }
    int protoAppliedGr() const { return proto_->appliedGr(); }
    bool protoSampleNumSync() { return proto_->onPacketSampleNum(0) == 0; }
    sdrplay_api_RxChannelParamsT *liveChannel() const { return ctx_.params ? &ctx_.params->rxChannelA : nullptr; }
    int logTuner() const { return static_cast<int>(ctx_.tuner); }
    sdrplay_api_TunerSelectT overloadTuner() const { return sdrplay_api_Tuner_A; }
    int agcMode() const { return 0; }
    bool hdrActive() const { return false; }
    bool trackArms() const { return true; }
    // A gain write dropped without a confirmation (resyncToggles on a stream (re)start,
    // dropPendingUpdates with resetGainUpdate) also clears the AGC gain-wait latch, deliberately:
    // left set, it would keep the AGC and power estimation dead until the next Init. No-op
    // while the AGC is not waiting. Engine mutex held.
    void gainWriteDropped() { clearAgcWait(proc_, agc_); }
    static void clearAgcWait(dsp::StreamProcessor &p, dsp::agc::AgcChannel &a)
    {
        if (!p.agcWaitGain() && !a.waiting()) return;
        p.setAgcWaitGain(false);
        a.gainConfirmed();
    }
    void latchDspForUpdate(int) { latchDspParams(cur_); } // engine mutex held, before the engine's Update
    bool logRecordModel(const rsp1b::ErrorRecord &) { return false; }
    bool uninitIdle() const { return proto_->state() == kStateIdle; }
    bool uninitClearsDsp(int) const { return true; }
    void blockProcessed(const dsp::BlockInfo &) {}
    bool modelDownConvert(std::int16_t *, std::int16_t *, std::uint32_t, bool) { return false; }
    // Returns true when the iteration was handled outside the per-block pipeline (*result set).
    bool streamOther(std::uint32_t, bool *) { return false; }
    // True when streamOther() handles the iteration (it acquires its own output slots).
    bool streamsOther() const { return false; }

    // ---- shared machinery
    Snapshot latch() const
    {
        Snapshot s;
        if (ctx_.params) s.dev = ctx_.params->devParams;
        if (const sdrplay_api_RxChannelParamsT *c = self().liveChannel()) s.rx = *c;
        return s;
    }
    void latchDspParams(const Snapshot &s);
    void configureDspAtInit(const Snapshot &s);
    void applyDspRequests(); // engine mutex held
    void postEvent(sdrplay_api_EventT id, sdrplay_api_TunerSelectT tuner, const sdrplay_api_EventParamsT &p);
    void record(const rsp1b::ErrorRecord &r);
    void logRecord(const rsp1b::ErrorRecord &r)
    {
        if (!ctx_.log) return;
        if (self().logRecordModel(r)) return;
        logRecordDefault(*ctx_.log, r, self().logTuner());
    }
    void logUpdateFailure(int tuner, int rc);
    void flushDspOutputs(bool published);
    int agcSetPointMin() const
    {
        return dsp::agc::setPointMin(proto_->fsState().spp, hwVer_, self().agcMode(), raw(cur_.rx.tunerParams.ifType));
    }

    DeviceContext ctx_;
    std::uint8_t hwVer_;
    std::unique_ptr<rsp1b::Io> io_;
    std::unique_ptr<Proto> proto_;
    std::unique_ptr<Hooks> hooks_;

    mutable std::mutex eng_;                          // engine mutex (see header comment)
    std::unique_lock<std::mutex> *apiLock_ = nullptr; // set while an API call holds eng_
    std::mutex readMtx_;                              // one packet-ring reader at a time
    std::atomic<std::uint32_t> gen_{0};               // stream generation (streamInit/Uninit)
    // Generation of the last stream start/stop OUTSIDE an Update (Init/Uninit/release): queued
    // packets older than it are dropped. A restart inside an Update keeps the packet reader's chunk
    // (the chunk count is reset at Init only).
    std::atomic<std::uint32_t> flushGen_{0};
    bool inUpdate_ = false;                           // engine mutex
    bool dspReady_ = false;                           // DSP configured for the current Init
    bool dspActive_ = false;                          // the DSP thread holds eng_
    Snapshot cur_;                                    // params latched by the current/last API call

    dsp::StreamProcessor proc_;
    dsp::agc::AgcChannel agc_;
    dsp::agc::PowerEstimator pe_;
    Arm arms_[3]; // indexed by rsp1b::Change
    bool agcReport_ = false;
    int agcGr_ = 0;          // channel gRdB the AGC adds its step to
    double lastPow_ = 0.0;

    // DSP-thread outputs delivered after eng_ is released
    std::vector<PendingEvent> dspEvents_;
    std::vector<PendingRecord> dspRecords_;

    // packet queue of the DSP thread
    std::vector<std::uint8_t> pkts_;
    std::size_t queued_ = 0;
    // The queue is packets [qhead_, qhead_ + queued_) of pkts_: a block advances qhead_ instead of
    // moving the rest of the 384-packet chunk to the front. Compacted only when refilling, when
    // fewer than one block is left.
    std::size_t qhead_ = 0;
    std::uint8_t *queueData() { return pkts_.data() + qhead_ * dsp::kPacketBytes; }
    std::uint32_t queueGen_ = 0;
    std::vector<std::int16_t> scratchI_, scratchQ_;
    Stats stats_;
};

// ============================================================ implementation

dsp::agc::AgcParams agcParamsOf(const sdrplay_api_RxChannelParamsT &rx);

template <class D, class P> void CommonRspDevice<D, P>::postEvent(sdrplay_api_EventT id, sdrplay_api_TunerSelectT tuner,
                                                                 const sdrplay_api_EventParamsT &p)
{
    if (dspActive_) {
        dspEvents_.push_back(PendingEvent{id, tuner, p});
        return;
    }
    if (!ctx_.events) return;
    if (apiLock_) {
        // API thread inside Init/Update: a full event ring blocks the post (500 ms waits until
        // the client frees a slot) and with it the Update, but the stream must go
        // on meanwhile (callers see stream callbacks keep coming while the event callback sleeps),
        // so the engine mutex is not held across the wait.
        std::unique_lock<std::mutex> *l = apiLock_;
        l->unlock();
        ctx_.events->postEvent(id, tuner, p);
        l->lock();
        return;
    }
    ctx_.events->postEvent(id, tuner, p);
}

template <class D, class P> void CommonRspDevice<D, P>::record(const rsp1b::ErrorRecord &r)
{
    logRecord(r);
    if (r.logOnly) return; // a LOG line without an error record
    const std::uint64_t t = platform::realtimeMicros();
    if (dspActive_) {
        dspRecords_.push_back(PendingRecord{r, t});
        return;
    }
    // API thread: the command thread already holds the record mutex.
    if (ctx_.lastError) api::setLastError(ctx_.lastError, t, r.file, r.function, r.line, "%s", r.message.c_str());
}

template <class D, class P> void CommonRspDevice<D, P>::flushDspOutputs(bool published)
{
    std::vector<PendingRecord> recs;
    std::vector<PendingEvent> evs;
    {
        std::lock_guard<std::mutex> l(eng_);
        if (published) ++stats_.published; // after publish(): stats() orders a reader after the slot write
        recs.swap(dspRecords_);
        evs.swap(dspEvents_);
    }
    if (!recs.empty() && ctx_.lastError) {
        std::unique_lock<std::mutex> rl;
        if (ctx_.recordMutex) rl = std::unique_lock<std::mutex>(*ctx_.recordMutex);
        for (const PendingRecord &p : recs)
            api::setLastError(ctx_.lastError, p.timeUs, p.rec.file, p.rec.function, p.rec.line, "%s",
                              p.rec.message.c_str());
    }
    if (ctx_.events)
        for (const PendingEvent &e : evs) ctx_.events->postEvent(e.id, e.tuner, e.params);
}

template <class D, class P> void CommonRspDevice<D, P>::latchDspParams(const Snapshot &s)
{
    // every API call latches the whole channel
    dsp::ChannelParams &c = proc_.params();
    c.bwType = raw(s.rx.tunerParams.bwType);
    c.ifType = raw(s.rx.tunerParams.ifType);
    c.lnaState = s.rx.tunerParams.gain.LNAstate;
    c.dcEnable = s.rx.ctrlParams.dcOffset.DCenable != 0;
    c.iqEnable = s.rx.ctrlParams.dcOffset.IQenable != 0;
    c.decEnable = s.rx.ctrlParams.decimation.enable != 0;
    c.decFactor = s.rx.ctrlParams.decimation.decimationFactor;
    c.decWide = s.rx.ctrlParams.decimation.wideBandSignal;
    c.agcEnable = raw(s.rx.ctrlParams.agc.enable);
    c.adsbMode = raw(s.rx.ctrlParams.adsbMode);
    agc_.setParams(agcParamsOf(s.rx)); // the stream thread reads the live copy (even after a rejected configureAgc)
    agcGr_ = s.rx.tunerParams.gain.gRdB;
}

template <class D, class P> void CommonRspDevice<D, P>::configureDspAtInit(const Snapshot &s)
{
    // Channel init + initHardware's configureDcIq / updateDecimation /
    // configureAgc, then the first block has reset = 1.
    latchDspParams(s);
    const int applied = self().protoAppliedGr();
    agcGr_ = applied >= 0 ? applied : s.rx.tunerParams.gain.gRdB;
    const auto &fs = proto_->fsState();
    proc_.setSamplesPerPacket(static_cast<std::uint32_t>(fs.spp));
    proc_.setConfirmedFsHz(s.dev.fsFreq.fsHz);
    proc_.initChannel(fs.fsMHz);
    proc_.dcOffsetIqControl();
    proc_.updateDecimation();
    proc_.requestReset();
    agc_.channelInit();
    (void)agc_.control(agcSetPointMin()); // initHardware ignores the result
    pe_.reset();
    (void)proto_->takeDspRequests();       // already applied above
    if (ctx_.sink) ctx_.sink->restart();
    dspReady_ = true;
}

template <class D, class P> void CommonRspDevice<D, P>::applyDspRequests()
{
    const rsp1b::DspRequests r = proto_->takeDspRequests();
    const auto &fs = proto_->fsState();
    if (r.streamRestarted) {
        // Dev_Fs format change: the next block has reset = 1 and a forced fsChanged
        proc_.setSamplesPerPacket(static_cast<std::uint32_t>(fs.spp));
        proc_.setConfirmedFsHz(cur_.dev.fsFreq.fsHz);
        proc_.requestReset();
        proc_.forceFsChanged();
        agc_.streamRestart(); // (queued packets: flushGen_)
    }
    if (r.downConverterReinit) proc_.configureDownConverter(fs.fsMHz);
    if (r.decimationChanged) proc_.updateDecimation();
    if (r.dcIqReset) proc_.dcOffsetIqControl();
    if (r.agcChanged) (void)agc_.control(agcSetPointMin()); // protocol already validated the setpoint
    if (r.agcUserRequest && agc_.enabled()) agc_.requestApply();
    if (r.overloadAck) pe_.detector().ack();
}

template <class D, class P> sdrplay_api_ErrT CommonRspDevice<D, P>::init()
{
    std::unique_lock<std::mutex> l(eng_);
    const int tuner = self().logTuner();
    if (proto_->state() != kStateIdle) {
        // Init: "Not in uninitialised state" error record.
        (void)proto_->init(); // writes the record, returns AlreadyInitialised
        return sdrplay_api_AlreadyInitialised;
    }
    dspReady_ = false;
    cur_ = latch();
    apiLock_ = &l;
    const int rc = proto_->init();
    apiLock_ = nullptr;
    if (rc == 0) {
        self().configureDspAtInit(cur_);
    } else if (ctx_.log) {
        switch (proto_->initFailStage()) {
        case kInitParams:
            ctx_.log->log(log::Level::Error, __func__, "Tuner%d: device parameter check returned error %d", tuner, rc);
            break;
        case kInitChannelState:
            ctx_.log->log(log::Level::Error, __func__, "Tuner%d: state init returned error %d", tuner, rc);
            break;
        case kInitHardware:
            ctx_.log->log(log::Level::Error, __func__, "Tuner%d: hardware init returned error %d", tuner, rc);
            break;
        case kInitStart:
            ctx_.log->log(log::Level::Error, __func__, "Tuner%d: start returned error %d", tuner, rc);
            break;
        default: break;
        }
    }
    return static_cast<sdrplay_api_ErrT>(rc);
}

template <class D, class P> sdrplay_api_ErrT CommonRspDevice<D, P>::uninit()
{
    std::unique_lock<std::mutex> l(eng_);
    if (self().uninitIdle()) {
        if (ctx_.log) ctx_.log->log(log::Level::Error, __func__, "Already uninitialised");
        return sdrplay_api_NotInitialised;
    }
    apiLock_ = &l;
    const int rc = proto_->uninit();
    apiLock_ = nullptr;
    if (self().uninitClearsDsp(rc)) dspReady_ = false;
    return static_cast<sdrplay_api_ErrT>(rc);
}

template <class D, class P> void CommonRspDevice<D, P>::logUpdateFailure(int tuner, int rc)
{
    // Update: "Tuner%d: <step> err=%d" for the failing step (log::Level::Error)
    if (rc == 0 || !ctx_.log) return;
    const char *st = proto_->updateFailStep();
    if (!st) return;
    if (std::strcmp(st, kStepProgramGr) == 0) ctx_.log->log(log::Level::Error, __func__, "Tuner%d: gain programming failed", tuner);
    else if (std::strcmp(st, kStepDuoBiasTWrongTuner) == 0)
        ctx_.log->log(log::Level::Error, __func__, "Tuner%d: bias-T control: wrong tuner", tuner);
    else ctx_.log->log(log::Level::Error, __func__, "Tuner%d: %s err=%d", tuner, st, rc);
}

template <class D, class P>
sdrplay_api_ErrT CommonRspDevice<D, P>::update(sdrplay_api_TunerSelectT tuner, sdrplay_api_ReasonForUpdateT reason,
                                               sdrplay_api_ReasonForUpdateExtension1T ext1)
{
    std::unique_lock<std::mutex> l(eng_);
    if (proto_->state() == kStateIdle) {
        if (ctx_.log) ctx_.log->log(log::Level::Error, __func__, "Wrong state: Uninitialised");
        return sdrplay_api_NotInitialised;
    }
    const std::uint32_t rs = static_cast<std::uint32_t>(raw(reason));
    const std::uint32_t ex = static_cast<std::uint32_t>(raw(ext1));
    cur_ = latch();
    self().latchDspForUpdate(static_cast<int>(tuner));
    apiLock_ = &l;
    inUpdate_ = true;
    const int rc = self().protoUpdate(static_cast<int>(tuner), rs, ex);
    inUpdate_ = false;
    apiLock_ = nullptr;
    logUpdateFailure(static_cast<int>(tuner), rc);
    self().applyDspRequests();
    return static_cast<sdrplay_api_ErrT>(rc);
}

template <class D, class P>
sdrplay_api_ErrT CommonRspDevice<D, P>::internalUpdate(sdrplay_api_ReasonForInternalUpdateT reason)
{
    // InternalUpdate: no initialised check; reason 0 -> 0; otherwise
    // the Rsp1aRfTest / RspDuoRfTest reason handlers. An RSP1B rejects the RSPduo
    // expander request (kReqGpioDual1) -> HwError; the other single-tuner models are expected to reject
    // it the same way (not checked on hardware).
    std::unique_lock<std::mutex> l(eng_);
    const std::uint32_t rs = static_cast<std::uint32_t>(raw(reason));
    apiLock_ = &l;
    const int rc = proto_->internalUpdate(rs);
    apiLock_ = nullptr;
    if (rc != 0 && ctx_.log) {
        if (rs & sdrplay_api_InternalUpdate_Rsp1aRfTest)
            ctx_.log->log(log::Level::Error, __func__, "RF test mode (RSP1A) err=%d", rc);
        else if (rs & sdrplay_api_InternalUpdate_RspDuoRfTest)
            ctx_.log->log(log::Level::Error, __func__, "RF test mode (RSPduo) err=%d", rc);
    }
    return static_cast<sdrplay_api_ErrT>(rc);
}

template <class D, class P> void CommonRspDevice<D, P>::releaseHardware()
{
    // Before destruction: Uninit if initialised (no overload event), then SET_INTERFACE 0 if
    // the bridge is still open after a failed Init.
    std::unique_lock<std::mutex> l(eng_);
    apiLock_ = &l;
    proto_->release();
    apiLock_ = nullptr;
    dspReady_ = false;
}

template <class D, class P> bool CommonRspDevice<D, P>::streamOnce(std::uint32_t waitMs)
{
    StreamTransport *s = ctx_.stream;
    std::uint32_t n = 0, g = 0;
    bool otherPath = false;
    {
        std::lock_guard<std::mutex> l(eng_);
        if (!s || !dspReady_ || proto_->state() != kStateStreaming) {
            queued_ = 0;
            proc_.closeBlock();
            return false;
        }
        g = gen_.load();
        if (queueGen_ != g) {
            // packets of a previous stream: dropped after Init/Uninit, kept across an Update's
            // restart (the packet reader goes on with its chunk). The block itself starts
            // again (the restart fails the packet read: new block, reset = 1).
            if (static_cast<std::int32_t>(flushGen_.load() - queueGen_) > 0) queued_ = 0;
            proc_.closeBlock();
            queueGen_ = g;
        }
        otherPath = self().streamsOther();
        n = proc_.packetsPerBlock();
    }

    // Stream thread block start: (a) wait for an output slot
    // (10 x 10 ms, else processed and dropped), (b) the staged decimation becomes active and N
    // is latched, (c) N packet reads, which wait for the next 384-packet chunk. An Update during
    // (c) keeps this block's N and user decimation (the down-converter and spp are read live
    // later), an Update during (a) does not.
    StreamBlock blk;
    bool pub = false;
    if (!otherPath) {
        pub = ctx_.sink && ctx_.sink->acquire(blk, kSlotWaitMs);
        if (!pub) {
            blk.xi = scratchI_.data();
            blk.xq = scratchQ_.data();
            blk.xiReadable = scratchI_.size();
            blk.xqReadable = scratchQ_.size();
        }
        std::lock_guard<std::mutex> l(eng_);
        n = proc_.beginBlock(); // idempotent while the block still waits for its packets
    }
    if (n == 0 || n > dsp::kMaxPacketsPerBlock) n = kFallbackPacketsPerBlock;

    // 1. packets (outside the engine mutex: Update/Uninit must not wait for USB data)
    bool gone = false;
    {
        std::lock_guard<std::mutex> rl(readMtx_);
        const std::uint64_t t0 = platform::monotonicNanos();
        // The packet reader reads one whole 384-packet chunk, then
        // the blocks are cut from it, so blocks (and callbacks) come in 384-packet bursts. Read a
        // full chunk whenever the queue holds less than one block.
        const std::size_t target = queued_ < n ? queued_ + dsp::kStreamReadBytes / dsp::kPacketBytes : queued_;
        if (target > queued_ && qhead_ != 0) { // refill: move the < n leftover packets to the front
            if (queued_) std::memmove(pkts_.data(), queueData(), queued_ * dsp::kPacketBytes);
            qhead_ = 0;
        }
        while (queued_ < target && gen_.load() == g) {
            const std::uint64_t el = (platform::monotonicNanos() - t0) / 1000000u;
            if (el >= waitMs) break;
            const std::uint32_t slice = static_cast<std::uint32_t>(waitMs - el) < kReadSliceMs
                                            ? static_cast<std::uint32_t>(waitMs - el)
                                            : kReadSliceMs;
            const int r = s->read(queueData() + queued_ * dsp::kPacketBytes, target - queued_, slice);
            if (r < 0) {
                gone = true;
                break;
            }
            queued_ += static_cast<std::size_t>(r);
        }
    }
    if (gone || queued_ == 0) {
        // the block stays open (N and decimation latched); its slot is given back meanwhile
        if (pub) ctx_.sink->cancel(blk);
        if (gone) platform::sleepMs(10); // the transport reports DeviceRemoved
        return false;                    // nothing arrived: try again (the packet reader keeps waiting)
    }

    if (otherPath) {
        bool other = false;
        if (self().streamOther(g, &other)) return other;
    }

    // 2. one block under the engine mutex
    dsp::BlockInfo info;
    bool processed = false;
    {
        std::lock_guard<std::mutex> l(eng_);
        const std::uint32_t nNow = n; // latched at the block start (beginBlock)
        if (gen_.load() != g || !dspReady_ || proto_->state() != kStateStreaming) {
            if (gen_.load() == g || static_cast<std::int32_t>(flushGen_.load() - g) > 0 || !dspReady_) queued_ = 0;
            proc_.closeBlock();
            ++stats_.discarded;
        } else {
            dspActive_ = true;
            if (const sdrplay_api_RxChannelParamsT *live = self().liveChannel())
                proc_.params().adsbMode = raw(live->ctrlParams.adsbMode); // read live per block
            const double fs = proc_.confirmedFsHz();
            const int spp = static_cast<int>(proc_.samplesPerPacket());
            agc_.blockStart(fs, spp, static_cast<int>(nNow));
            const std::size_t use = queued_ < nNow ? queued_ : nNow;
            dsp::MemoryPacketSource src(queueData(), use);
            const dsp::BlockBuffers bufs{blk.xi, blk.xq, blk.xiReadable, blk.xqReadable};
            proc_.processBlock(src, bufs, info, hooks_.get());
            if (!proc_.agcWaitGain()) agc_.gainConfirmed();
            agc_.blockEnd(proc_.confirmedFsHz(), spp, static_cast<int>(nNow));
            self().blockProcessed(info);
            if (info.dcNotEnabled) {
                // DC correction requested but off: an error record (no LOG line) for every block
                rsp1b::ErrorRecord r;
                r.file = __FILE__;
                r.function = __func__;
                r.line = __LINE__;
                r.message = "DCoffset not enabled";
                r.logAs = rsp1b::LogAs::None;
                record(r);
            }
            dspActive_ = false;
            // keep packets beyond this block for the next one
            qhead_ += use;
            queued_ -= use;
            if (queued_ == 0) qhead_ = 0;
            processed = true;
            ++stats_.blocks;
            if (info.readError) ++stats_.readErrors;
            if (!pub) ++stats_.dropped; // (published: counted after publish(), below)
        }
    }
    if (processed && pub) {
        sdrplay_api_StreamCbParamsT p;
        p.firstSampleNum = info.firstSampleNum;
        p.grChanged = static_cast<int>(info.grChanged);
        p.rfChanged = static_cast<int>(info.rfChanged);
        p.fsChanged = static_cast<int>(info.fsChanged);
        p.numSamples = info.numSamples;
        ctx_.sink->publish(blk, p, info.reset);
    } else if (pub) {
        ctx_.sink->cancel(blk);
    }
    flushDspOutputs(processed && pub);
    if (processed && info.readError) platform::sleepMs(10); // skip one iteration, next block reset = 1
    return processed;
}

} // namespace common
} // namespace device
} // namespace sdrplay
