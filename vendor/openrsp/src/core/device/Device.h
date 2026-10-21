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

// Device model abstraction (service side). One instance per selected device.
//
// A model owns the hardware programming for one RSP family. It only talks to
// the device through platform::UsbDevice (pure virtual), so every model is
// unit-testable against a fake that records control transfers, so the exact
// control-transfer sequence of each operation can be checked.
//
// Parameters live in the shared command map (ipc::CmdMap): the client app writes
// them in place; the service reads them at Init/Update and writes outputs back
// (gainVals, AGC gRdB).
#pragma once

#include "core/api/ApiTypes.h"
#include "core/device/Models.h"
#include "core/ipc/Layouts.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace sdrplay {
namespace platform {
class UsbDevice;
}
namespace log {
class Logger;
}

namespace device {

// Service-side event producer (implemented by the DeviceChannel event ring).
class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void postEvent(sdrplay_api_EventT id, sdrplay_api_TunerSelectT tuner,
                           const sdrplay_api_EventParamsT &params) = 0;
};

// ---------------------------------------------------------------- streaming seams
// The stream path is split in three stages: USB transport (libusb async
// transfers into a packet ring) -> DSP thread (model: packet reader +
// per-block pipeline + AGC/overload) -> output
// ring (shm data slots consumed by the client's stream thread). The model only
// sees these two interfaces; the service implements them (transport:
// src/service/stream, output ring: DeviceChannel), the tests fake them.

// Stream start / stop, bulk flush and the packet ring reader.
class StreamTransport {
public:
    virtual ~StreamTransport() = default;
    // Submit the transfers for the interface's current alt
    // setting (1/2 = isochronous geometries, 3 = bulk). The model
    // has already issued SET_INTERFACE through UsbDevice::setAltSetting. A new
    // start discards anything left in the packet ring. 0 = ok.
    virtual int start(std::uint8_t alt) = 0;
    // Cancel and drain the transfers. Wakes a blocked read().
    virtual void stop() = 0;
    // Bulk flush: up to 32 x 1024-byte bulk reads with a 100 ms timeout.
    virtual void flush() {}
    // Device reset recovery when the device reports function address 2 (not seen in practice). 0 = ok.
    virtual int resetDevice() { return 1; }
    // Copies up to maxPackets whole 1024-byte device packets (bulk already
    // aligned) to dst. Waits at most timeoutMs for the first packet, then
    // returns what is available without waiting further. Returns the number of
    // packets copied (0 = timeout or stopped) or < 0 when the device is gone.
    virtual int read(std::uint8_t *dst, std::size_t maxPackets, std::uint32_t timeoutMs) = 0;
    // Whole packets waiting to be read without blocking; -1 if the transport cannot tell (then
    // callers fall back to read()). A stream read copies only once all requested bytes are there.
    virtual long availablePackets() { return -1; }
};

// The output ring (shm data slots).
struct StreamBlock {
    std::int16_t *xi = nullptr;      // slot xiA (capacity >= 16128 samples)
    std::int16_t *xq = nullptr;      // slot xqA
    std::size_t xiReadable = 0;      // samples xi can hold (its capacity); the DSP reads at most this many
    std::size_t xqReadable = 0;
    std::uint32_t slot = 0;          // sink's own bookkeeping
    // RSPduo Dual: tuner B in the same slot (its xiB / xqB arrays); null if the sink has none.
    std::int16_t *xiB = nullptr;
    std::int16_t *xqB = nullptr;
    std::size_t xiBReadable = 0;
    std::size_t xqBReadable = 0;
};

class StreamSink {
public:
    virtual ~StreamSink() = default;
    // Init: the next published block is the first of a new stream (bufIdx 1).
    virtual void restart() {}
    // Wait up to timeoutMs (the service uses 10 x 10 ms) for a free slot. false: the
    // block is processed into a scratch buffer and not published (dropped).
    virtual bool acquire(StreamBlock &b, std::uint32_t timeoutMs) = 0;
    // Publish an acquired slot: StreamCbParamsT for tuner A and the reset flag.
    virtual void publish(const StreamBlock &b, const sdrplay_api_StreamCbParamsT &p, std::uint32_t reset) = 0;
    // RSPduo Dual: publish tuner A and tuner B (the slot's paramsB) with one reset flag.
    virtual void publishDual(const StreamBlock &b, const sdrplay_api_StreamCbParamsT &pA,
                             const sdrplay_api_StreamCbParamsT &pB, std::uint32_t reset)
    {
        (void)pB;
        publish(b, pA, reset);
    }
    // Give back an acquired slot without publishing (the stream restarted meanwhile).
    virtual void cancel(const StreamBlock &b) { (void)b; }
};

struct DeviceContext {
    platform::UsbDevice *usb = nullptr; // opened + interface claimed
    ipc::CmdMap *params = nullptr;      // shared parameter storage
    EventSink *events = nullptr;
    log::Logger *log = nullptr;         // device logger (log prefix "devIdx<N>: rsp")
    // Service-device error record (the CmdMap's error record, returned by device
    // cmd 14). May be null in tests.
    sdrplay_api_TimedErrorInfoT *lastError = nullptr;
    sdrplay_api_TunerSelectT tuner = sdrplay_api_Tuner_A; // from the select request (DeviceT.tuner)
    StreamTransport *stream = nullptr; // null: Init fails at stream start (HwError)
    StreamSink *sink = nullptr;        // null: blocks are processed and dropped
    // Held by the command thread while it serves device GetLastError (cmd 14);
    // the DSP thread takes it to write the error record. May be null in tests.
    std::mutex *recordMutex = nullptr;
    // RSPduo (hwVer 3) master select: DeviceT.rspDuoMode / rspDuoSampleFreq of the request and
    // the slave parameter map (<P>_sCmdMap, always created for hwVer 3). Ignored by other models.
    int rspDuoMode = 0;
    double rspDuoSampleFreq = 0.0;
    ipc::CmdMap *slaveParams = nullptr;
    StreamSink *slaveSink = nullptr; // <P>_sData (RSPduo Master: the slave channel's blocks)
};

class Device {
public:
    virtual ~Device() = default;

    virtual const ModelInfo &model() const = 0;

    // Fill the default parameters into a freshly zeroed CmdMap. The defaults
    // are model-independent: see core/device/DefaultParams.h.
    virtual void writeDefaults(ipc::CmdMap &map) = 0;

    // Device commands. Return the result written to CmdMap.result.
    virtual sdrplay_api_ErrT init() = 0;
    virtual sdrplay_api_ErrT uninit() = 0;
    virtual sdrplay_api_ErrT update(sdrplay_api_TunerSelectT tuner, sdrplay_api_ReasonForUpdateT reason,
                                    sdrplay_api_ReasonForUpdateExtension1T ext1) = 0;
    virtual sdrplay_api_ErrT internalUpdate(sdrplay_api_ReasonForInternalUpdateT reason) = 0;

    // Lock and uninit if the device is
    // initialised, before the object is destroyed (release, heartbeat loss,
    // service stop). Called with the API mutex held.
    virtual void releaseHardware() {}

    // RSPduo master commands: cmd 10 SwapTuner (stores the tuner
    // and re-wires the channels) and cmd 11 ChangeSampleRate (stores fs). Other models
    // only store the value (the service always answers 0).
    virtual void rspDuoSwapTuner(int tuner) { (void)tuner; }
    virtual void rspDuoChangeSampleRate(double fsHz) { (void)fsHz; }

    // RSPduo slave side (the service's slave command thread and slave
    // select/release/heartbeat paths). Other models: no slave (Fail / no-ops).
    virtual sdrplay_api_ErrT rspDuoInitSlave() { return sdrplay_api_Fail; }   // slave cmd 8
    virtual sdrplay_api_ErrT rspDuoUninitSlave() { return sdrplay_api_Fail; } // slave cmd 9
    virtual sdrplay_api_ErrT rspDuoUpdateSlave(sdrplay_api_TunerSelectT tuner, sdrplay_api_ReasonForUpdateT reason,
                                               sdrplay_api_ReasonForUpdateExtension1T ext1) // slave cmd 12
    {
        (void)tuner;
        (void)reason;
        (void)ext1;
        return sdrplay_api_Fail;
    }
    virtual void rspDuoSlaveAttached() {}
    virtual void rspDuoSlaveDetached(bool dllDisappeared) { (void)dllDisappeared; }
    virtual void rspDuoMasterDisappeared() {}
    virtual bool rspDuoSlaveInitialised() const { return false; }
    // The master's sample rate offered to a slave by GetDevices.
    virtual double rspDuoMasterSampleFreq() const { return 0.0; }

    // Body of the device's stream thread (created with the device, idle until
    // Init, lives until the device object is destroyed): returns when `kill`
    // becomes true. The default idles.
    virtual void runStream(const std::atomic<bool> &kill);
};

// Factory: nullptr for models not implemented (see ModelInfo::supported).
std::unique_ptr<Device> createDevice(std::uint8_t hwVer, const DeviceContext &ctx);

} // namespace device
} // namespace sdrplay
