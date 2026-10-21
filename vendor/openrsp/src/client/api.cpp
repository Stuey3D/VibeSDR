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

// The exported C API of libsdrplay_api.so.3 (prototypes from the API header;
// undocumented ones in ClientAbi.h). Behaviour, validation order, return
// codes, log texts and error-record lines are compatible with SDRplay API 3.15.
#include <atomic>
#include "client/ClientAbi.h"
#include "client/ServiceConnection.h"

#include "core/ipc/Commands.h"

#include <cstring>
#include <vector>
#include <initializer_list>

using namespace sdrplay;

namespace {
// The device behind a HANDLE, or nullptr when the handle is NULL. A handle
// that is not one of our live device objects (freed by ReleaseDevice, or
// garbage) is treated like a NULL handle instead of touching freed memory.
sdrplay_api_device *devOf(HANDLE h)
{
    if (!h || !client::isLiveDevice(h)) return nullptr;
    return static_cast<sdrplay_api_device *>(h);
}

// Shared shape of the per-device wrappers:
// NULL dev -> the nullLog text logged as an error + a global error record (null device) ->
// NotInitialised; then the device mutex around the call, with an error record when locking
// or unlocking it fails.
struct Wrapper {
    const char *fn;
    const char *nullLog = "Device NOT initialised";
    const char *lockLog = "Failed to lock mutex";
    const char *unlockLog = "Failed to unlock mutex";
};

sdrplay_api_ErrT nullDev(const Wrapper &w)
{
    client::apiLog(sdrplay_api_DbgLvl_Error, w.fn, "%s", w.nullLog);
    client::setClientError(__FILE__, w.fn, __LINE__, "Dev NULL ptr");
    return sdrplay_api_NotInitialised;
}

// A NULL out-pointer -> InvalidParam with a global record on the wrapper's
// NULL line (checked, not dereferenced).
sdrplay_api_ErrT nullOut(const Wrapper &w)
{
    client::apiLog(sdrplay_api_DbgLvl_Error, w.fn, "NULL ptr");
    client::setClientError(__FILE__, w.fn, __LINE__, "NULL ptr");
    return sdrplay_api_InvalidParam;
}

template <typename F> sdrplay_api_ErrT locked(const Wrapper &w, sdrplay_api_device *d, F &&body)
{
    if (d->mutexLock(w.fn) != 0) {
        client::apiLog(sdrplay_api_DbgLvl_Error, w.fn, "%s", w.lockLog);
        client::setClientError(__FILE__, w.fn, __LINE__, "Failed to lock mutex");
        return sdrplay_api_Fail;
    }
    sdrplay_api_ErrT r = body();
    if (d->mutexUnlock(w.fn) != 0) {
        client::apiLog(sdrplay_api_DbgLvl_Error, w.fn, "%s", w.unlockLog);
        client::setClientError(__FILE__, w.fn, __LINE__, "Failed to unlock mutex");
        return sdrplay_api_Fail;
    }
    return r;
}

// Releases our device object. ReleaseDevice from one of the device's own
// callbacks cannot join the calling thread; the object is
// then left to that thread (leaked) instead of being freed under it.
void destroyDevice(sdrplay_api_device *d)
{
    d->configured_ = false;
    d->KillThreads();
    if (d->leakedBySelfRelease_) return; // parked by KillThreads, reaped later
    d->killedByRelease_ = true;           // the destructor must not run (and log) it again
    client::noteFreedDevice(d); // the address only (still registered until the destructor runs)
    delete d;
}

sdrplay_api_ErrT selectImpl(sdrplay_api_DeviceT *device, bool withCb, sdrplay_api_CallbackFnsT *cb, void *ctx,
                            const char *fn)
{
    client::reapParkedDevices();
    if (m_serviceState.deviceSelected) { // checked before the NULL check and the lock
        client::apiLog(sdrplay_api_DbgLvl_Error, fn, "Device already selected");
        client::setClientError(__FILE__, fn, __LINE__, "Device already selected");
        return sdrplay_api_Fail;
    }
    if (!device) {
        client::apiLog(sdrplay_api_DbgLvl_Error, fn, "NULL ptr");
        client::setClientError(__FILE__, fn, __LINE__, "Device NULL ptr");
        return sdrplay_api_InvalidParam;
    }
    if (withCb && !cb) { // rejected before the service selects the device
        client::apiLog(sdrplay_api_DbgLvl_Error, fn, "NULL ptr");
        client::setClientError(__FILE__, fn, __LINE__, "callbackFns NULL ptr");
        return sdrplay_api_InvalidParam;
    }
    sdrplay_api_ErrT lr = client::lockApi();
    if (lr != sdrplay_api_Success) {
        client::apiLog(sdrplay_api_DbgLvl_Error, fn, "Failed to lock device"); // also before Open
        return client::lockFailure(lr);
    }
    ipc::ComShMem *c = client::com();
    c->cmd = static_cast<std::uint32_t>(ipc::GlobalCmd::SelectDevice);
    std::memcpy(&c->u.selectDevice.device, device, sizeof *device); // the whole DeviceT
    c->u.selectDevice.device.dev = nullptr;                          // the handle stays in this process
    c->u.selectDevice.heartbeatDisabled = m_serviceState.hbDisabled;
    if (SendCommandAndWaitForResponse() != sdrplay_api_Success) {
        client::unlockApi();
        return sdrplay_api_ServiceNotResponding;
    }
    if (c->result != sdrplay_api_Success) { // service code passed through, no client log or record
        auto err = static_cast<sdrplay_api_ErrT>(c->result);
        client::unlockApi();
        return err;
    }
    char prefix[sizeof(ipc::SelectDevicePayload::prefix)];
    std::memcpy(prefix, c->u.selectDevice.prefix, sizeof prefix);
    prefix[sizeof prefix - 1] = '\0';
    // hwVer/tuner/rspDuoMode come from the CALLER's struct.
    auto *dev = withCb ? new sdrplay_api_device(static_cast<char>(device->hwVer), prefix, device->tuner,
                                                device->rspDuoMode, m_serviceState.log, cb, ctx)
                       : new sdrplay_api_device(static_cast<char>(device->hwVer), prefix, device->tuner,
                                                device->rspDuoMode, m_serviceState.log);
    device->dev = dev; // set before the resources exist
    if (dev->CreateSharedResources(m_serviceState.hbDisabled) != 0) {
        // The half-built object is destroyed, DeviceT.dev is NULLed and the service
        // releases the device again; the selected flag stays 0. The device record
        // (e.g. the CreateCommObject failure) becomes the client-global record.
        const sdrplay_api_TimedErrorInfoT devErr = dev->lastError_;
        delete dev; // the destructor copes with any subset of the objects and threads
        device->dev = nullptr;
        c->cmd = static_cast<std::uint32_t>(ipc::GlobalCmd::ReleaseDevice);
        std::memcpy(&c->u.releaseDevice.device, device, sizeof *device);
        SendCommandAndWaitForResponse(); // a timeout is logged there; the select still fails
        if (devErr.timeUs != 0) m_serviceState.lastErr = devErr;
        client::unlockApi();
        client::apiLog(sdrplay_api_DbgLvl_Error, fn, "shared resource creation failed");
        return sdrplay_api_Fail;
    }
    m_serviceState.deviceSelected = 1;
    client::noteSelection(*device, dev);
    dev->DebugEnable(static_cast<sdrplay_api_DbgLvl_t>(m_serviceState.log->get_log_enable())); // result ignored
    auto err = static_cast<sdrplay_api_ErrT>(c->result);
    client::unlockApi();
    return err;
}

// GlobalCmd::GetLastError: fetch the service-global record into m_serviceState.serviceErr.
// Returns false on a timeout (record not updated). Caller holds the lock.
bool fetchServiceError()
{
    ipc::ComShMem *c = client::com();
    c->cmd = static_cast<std::uint32_t>(ipc::GlobalCmd::GetLastError);
    if (SendCommandAndWaitForResponse() != sdrplay_api_Success) return false;
    std::memcpy(static_cast<void *>(&m_serviceState.serviceErr), &c->u.getLastError.error,
                sizeof m_serviceState.serviceErr);
    return true;
}
} // namespace

extern "C" {

sdrplay_api_ErrT sdrplay_api_Open(void) { return client::open(); }

sdrplay_api_ErrT sdrplay_api_Close(void) { return client::close(); }

// Lock, IPC, status, version, then the NULL check.
sdrplay_api_ErrT sdrplay_api_ApiVersion(float *apiVer)
{
    sdrplay_api_ErrT lr = client::lockApi();
    if (lr != sdrplay_api_Success) return client::lockFailure(lr);
    ipc::ComShMem *c = client::com();
    c->cmd = static_cast<std::uint32_t>(ipc::GlobalCmd::ApiVersion); // payload not cleared
    if (SendCommandAndWaitForResponse() != sdrplay_api_Success) {
        client::unlockApi();
        return sdrplay_api_ServiceNotResponding;
    }
    if (c->result != sdrplay_api_Success) {
        client::apiLog(sdrplay_api_DbgLvl_Error, "sdrplay_api_ApiVersion", "Couldn't retrieve service version");
        client::setClientError(__FILE__, "sdrplay_api_ApiVersion", __LINE__, "Couldn't retrieve service version");
        client::unlockApi();
        return sdrplay_api_Fail;
    }
    float remote = c->u.apiVersion.serviceIpcVersion;
    if (!(remote == ipc::kServiceIpcVersion)) { // bit-exact; NaN => mismatch
        client::apiLog(sdrplay_api_DbgLvl_Error, "sdrplay_api_ApiVersion",
                       "Remote API versions don't match (local=%.2f remote=%.2f)",
                       static_cast<double>(ipc::kClientApiVersion), static_cast<double>(remote));
        // the error record prints 1.02 as "local"
        client::setClientError(__FILE__, "sdrplay_api_ApiVersion", __LINE__, "Remote API versions don't match (local=%.2f remote=%.2f)",
                               static_cast<double>(ipc::kServiceIpcVersion), static_cast<double>(remote));
        client::unlockApi();
        return sdrplay_api_InvalidServiceVersion;
    }
    if (!apiVer) {
        client::unlockApi(); // Unlock BEFORE SetLastError
        client::setClientError(__FILE__, "sdrplay_api_ApiVersion", __LINE__, "NULL ptr");
        return sdrplay_api_InvalidParam;
    }
    *apiVer = ipc::kClientApiVersion;
    client::unlockApi();
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_LockDeviceApi(void) { return client::lockApi(); }

sdrplay_api_ErrT sdrplay_api_UnlockDeviceApi(void) { return client::unlockApi(); }

sdrplay_api_ErrT sdrplay_api_GetDevices(sdrplay_api_DeviceT *devices, unsigned int *numDevs, unsigned int maxDevs)
{
    static const char *const fn = "sdrplay_api_GetDevices";
    if (!devices || !numDevs) { // before the lock (also safe before Open)
        client::apiLog(sdrplay_api_DbgLvl_Error, fn, "NULL ptr");
        client::setClientError(__FILE__, fn, __LINE__, "NULL ptr");
        return sdrplay_api_InvalidParam;
    }
    sdrplay_api_ErrT lr = client::lockApi();
    if (lr != sdrplay_api_Success) return client::lockFailure(lr); // no log
    ipc::ComShMem *c = client::com();
    c->cmd = static_cast<std::uint32_t>(ipc::GlobalCmd::GetDevices);
    if (maxDevs <= ipc::kMaxGetDevices) {
        c->u.getDevices.maxDevs = maxDevs;
    } else {
        client::apiLog(sdrplay_api_DbgLvl_Error, fn, "Requested maxDevs (%d) too large for shared memory, requesting %d",
                       static_cast<int>(maxDevs), static_cast<int>(ipc::kMaxGetDevices)); // printed signed
        c->u.getDevices.maxDevs = ipc::kMaxGetDevices;
    }
    if (SendCommandAndWaitForResponseLong() != sdrplay_api_Success) {
        client::unlockApi();
        return sdrplay_api_ServiceNotResponding; // no record
    }
    int err = c->result;
    if (err == sdrplay_api_Success) {
        // numDevs is NOT clamped to maxDevs: n records are copied whatever
        // maxDevs was. A count that would run past the mapped page (e.g. a
        // stale reply read before the service answered) fails with an error
        // record instead of faulting in the copy.
        const unsigned n = c->u.getDevices.numDevs;
        constexpr std::size_t kMappedBytes = 4096; // ComShMem is mapped as one 4 KiB page
        constexpr unsigned kMaxInPage =
            (kMappedBytes - offsetof(ipc::ComShMem, u.getDevices.devices)) / sizeof(sdrplay_api_DeviceT);
        if (n > kMaxInPage) {
            client::unlockApi();
            client::apiLog(sdrplay_api_DbgLvl_Error, fn, "Invalid device count %u from the service", n);
            client::setClientError(__FILE__, fn, __LINE__, "Invalid device count %u", n);
            return sdrplay_api_Fail;
        }
        *numDevs = n;
        std::memcpy(devices, c->u.getDevices.devices, n * sizeof(sdrplay_api_DeviceT));
        err = c->result;
    }
    client::unlockApi();
    return static_cast<sdrplay_api_ErrT>(err);
}

sdrplay_api_ErrT sdrplay_api_SelectDeviceCB(sdrplay_api_DeviceT *device, sdrplay_api_CallbackFnsT *callbackFns,
                                            void *cbContext)
{
    return selectImpl(device, true, callbackFns, cbContext, "sdrplay_api_SelectDeviceCB");
}

sdrplay_api_ErrT sdrplay_api_SelectDevice(sdrplay_api_DeviceT *device)
{
    return selectImpl(device, false, nullptr, nullptr, "sdrplay_api_SelectDevice");
}

sdrplay_api_ErrT sdrplay_api_ReleaseDevice(sdrplay_api_DeviceT *device)
{
    static const char *const fn = "sdrplay_api_ReleaseDevice";
    client::reapParkedDevices();
    if (!device) {
        client::apiLog(sdrplay_api_DbgLvl_Error, fn, "NULL ptr");
        client::setClientError(__FILE__, fn, __LINE__, "Device NULL ptr");
        return sdrplay_api_InvalidParam;
    }
    sdrplay_api_ErrT lr = client::lockApi();
    if (lr != sdrplay_api_Success) return client::lockFailure(lr); // no log
    // Threads are joined while the global lock is held, before GlobalCmd::ReleaseDevice.
    // Contract: the caller's DeviceT.dev keeps its value. A second release of
    // the same struct fails with an already-released error (no double free)
    // and does not contact the service, so a device selected again through
    // another DeviceT stays selected.
    if (!devOf(device->dev) && client::isFreedDevice(device->dev)) {
        client::unlockApi();
        client::apiLog(sdrplay_api_DbgLvl_Error, fn, "Device already released");
        client::setClientError(__FILE__, fn, __LINE__, "Device already released");
        return sdrplay_api_Fail;
    }
    // Only a selection this process holds is released (client::takeSelection):
    // anything else fails with a not-selected error without contacting the
    // service. The held device object is destroyed whichever DeviceT copy names
    // the selection.
    sdrplay_api_device *held = client::takeSelection(*device);
    if (!held) {
        client::unlockApi();
        client::apiLog(sdrplay_api_DbgLvl_Error, fn, "Device not selected");
        client::setClientError(__FILE__, fn, __LINE__, "Device not selected");
        return sdrplay_api_Fail;
    }
    destroyDevice(held);
    ipc::ComShMem *c = client::com();
    c->cmd = static_cast<std::uint32_t>(ipc::GlobalCmd::ReleaseDevice);
    std::memcpy(&c->u.releaseDevice.device, device, sizeof *device);
    c->u.releaseDevice.device.dev = nullptr; // the handle stays in this process
    if (SendCommandAndWaitForResponse() != sdrplay_api_Success) {
        client::unlockApi();
        return sdrplay_api_ServiceNotResponding; // selected flag stays set
    }
    m_serviceState.deviceSelected = 0; // whatever the service returned
    auto err = static_cast<sdrplay_api_ErrT>(c->result);
    client::unlockApi();
    return err;
}

// Pure; 0..24 return the enum names, every other value UnknownError.
namespace {
constexpr const char *kErrorNames[] = {
    "sdrplay_api_Success",
    "sdrplay_api_Fail",
    "sdrplay_api_InvalidParam",
    "sdrplay_api_OutOfRange",
    "sdrplay_api_GainUpdateError",
    "sdrplay_api_RfUpdateError",
    "sdrplay_api_FsUpdateError",
    "sdrplay_api_HwError",
    "sdrplay_api_AliasingError",
    "sdrplay_api_AlreadyInitialised",
    "sdrplay_api_NotInitialised",
    "sdrplay_api_NotEnabled",
    "sdrplay_api_HwVerError",
    "sdrplay_api_OutOfMemError",
    "sdrplay_api_ServiceNotResponding",
    "sdrplay_api_StartPending",
    "sdrplay_api_StopPending",
    "sdrplay_api_InvalidMode",
    "sdrplay_api_FailedVerification1",
    "sdrplay_api_FailedVerification2",
    "sdrplay_api_FailedVerification3",
    "sdrplay_api_FailedVerification4",
    "sdrplay_api_FailedVerification5",
    "sdrplay_api_FailedVerification6",
    "sdrplay_api_InvalidServiceVersion",
};
constexpr int kNumErrorNames = static_cast<int>(sizeof kErrorNames / sizeof kErrorNames[0]);
static_assert(kNumErrorNames == sdrplay_api_InvalidServiceVersion + 1, "one name per sdrplay_api_ErrT value");
constexpr char kUnknownError[] = "sdrplay_api_UnknownError";
} // namespace

const char *sdrplay_api_GetErrorString(sdrplay_api_ErrT err)
{
    int e; // the raw int bits: values outside the C enum's range are valid here
    static_assert(sizeof e == sizeof err, "ErrT is passed as an int");
    std::memcpy(&e, &err, sizeof e);
    if (e >= 0 && e < kNumErrorNames) return kErrorNames[e];
    return kUnknownError;
}

// The newest of the local, service-global,
// device-local and device-service records; ties keep the earlier source.
sdrplay_api_ErrorInfoT *sdrplay_api_GetLastError(sdrplay_api_DeviceT *device)
{
    enum { kLocal, kSvc, kDevLocal } src = kLocal;
    unsigned long long t = m_serviceState.lastErr.timeUs;
    // After Close the handle is dangling: skip the fetch and return the local
    // record.
    if (m_serviceState.hComMtx && !client::sessionClosed()) {
        if (client::lockApi() != sdrplay_api_Success) return &m_serviceState.lastErr.info;
        if (!fetchServiceError()) {
            client::unlockApi();
            return &m_serviceState.lastErr.info; // the timeout record it just wrote
        }
        client::unlockApi();
        if (m_serviceState.serviceErr.timeUs > t) {
            src = kSvc;
            t = m_serviceState.serviceErr.timeUs;
        }
    }
    sdrplay_api_device *d = device ? devOf(device->dev) : nullptr;
    // A released handle is ignored like a NULL one.
    if (d) {
        if (t < d->lastError_.timeUs) { // compared BEFORE the DeviceCmd::GetLastError fetch
            src = kDevLocal;
            t = d->lastError_.timeUs;
        }
        d->DevGetLastError(); // result ignored
        if (t < d->serviceError_.timeUs) return &d->serviceError_.info;
    }
    if (t == 0) return nullptr;
    if (src == kSvc) return &m_serviceState.serviceErr.info;
    if (src == kDevLocal) return &d->lastError_.info;
    return &m_serviceState.lastErr.info;
}

// `type` selects the error record (not an sdrplay_api_ErrT value).
// A NULL `time` skips the write.
sdrplay_api_ErrorInfoT *sdrplay_api_GetLastErrorByType(sdrplay_api_DeviceT *device, int type, unsigned long long *time)
{
    enum ErrorRecord : int {
        kRecordLocal = 0,         // this library's last error
        kRecordDeviceLocal = 1,   // the device object's last error
        kRecordService = 2,       // the service's global last error
        kRecordDeviceService = 3, // the service's last error for this device
    };
    auto writeTime = [time](unsigned long long v) {
        if (time) *time = v;
    };
    switch (type) {
    case kRecordLocal:
        writeTime(m_serviceState.lastErr.timeUs);
        return m_serviceState.lastErr.timeUs ? &m_serviceState.lastErr.info : nullptr;
    case kRecordDeviceLocal:
    case kRecordDeviceService: {
        if (!device) return nullptr;
        sdrplay_api_device *d = devOf(device->dev);
        if (!d) {
            // A released handle returns NULL with t = 0; other handles leave *time alone.
            if (client::isFreedDevice(device->dev)) writeTime(0);
            return nullptr;
        }
        if (type == kRecordDeviceLocal) {
            writeTime(d->lastError_.timeUs);
            return d->lastError_.timeUs ? &d->lastError_.info : nullptr;
        }
        if (d->DevGetLastError() == sdrplay_api_Success) {
            writeTime(d->serviceError_.timeUs);
            return d->serviceError_.timeUs ? &d->serviceError_.info : nullptr;
        }
        return nullptr;
    }
    case kRecordService:
        if (m_serviceState.hComMtx && !client::sessionClosed() && client::lockApi() == sdrplay_api_Success) {
            const bool fetched = fetchServiceError();
            client::unlockApi();
            if (fetched) {
                writeTime(m_serviceState.serviceErr.timeUs);
                return m_serviceState.serviceErr.timeUs ? &m_serviceState.serviceErr.info : nullptr;
            }
        }
        return nullptr;
    default:
        return nullptr; // *time untouched
    }
}

sdrplay_api_ErrT sdrplay_api_DisableHeartbeat(void)
{
    sdrplay_api_ErrT lr = client::lockApi();
    if (lr != sdrplay_api_Success) return client::lockFailure(lr);
    m_serviceState.hbDisabled = 1;
    client::apiLog(sdrplay_api_DbgLvl_Verbose, "sdrplay_api_DisableHeartbeat", "sdrplay_api: Heartbeat Disabled");
    client::syslogRaw("sdrplay_api: WARNING - heartbeat disabled for this device; if the application does not "
                      "release the device, it may stay unusable until the service is restarted"); // ungated
    client::unlockApi();
    return sdrplay_api_Success;
}

// Returns only Success, Fail or ServiceNotResponding.
sdrplay_api_ErrT sdrplay_api_DebugEnable(HANDLE dev, sdrplay_api_DbgLvl_t enable)
{
    sdrplay_api_ErrT lr = client::lockApi();
    if (lr != sdrplay_api_Success) return client::lockFailure(lr);
    m_serviceState.log->log_enable(enable); // before the IPC, no range check
    ipc::ComShMem *c = client::com();
    c->cmd = static_cast<std::uint32_t>(ipc::GlobalCmd::DebugEnable);
    c->u.debugEnable.dbgLvl = static_cast<std::uint32_t>(enable);
    if (SendCommandAndWaitForResponse() != sdrplay_api_Success) {
        client::unlockApi();
        return sdrplay_api_ServiceNotResponding;
    }
    // dev->DebugEnable() is only called for live device objects of this
    // library (an unconfigured one returns NotInitialised). The result is ignored.
    if (c->result == sdrplay_api_Success)
        if (sdrplay_api_device *d = devOf(dev)) d->DebugEnable(enable);
    client::unlockApi();
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_GetDeviceParams(HANDLE dev, sdrplay_api_DeviceParamsT **deviceParams)
{
    static const Wrapper w{"sdrplay_api_GetDeviceParams"};
    sdrplay_api_device *d = devOf(dev);
    if (!d) return nullDev(w);
    return locked(w, d, [&] {
        if (!deviceParams) return nullOut(w);
        *deviceParams = d->GetDeviceParams();
        return sdrplay_api_Success;
    });
}

// Init checks callbackFns BEFORE dev.
sdrplay_api_ErrT sdrplay_api_Init(HANDLE dev, sdrplay_api_CallbackFnsT *callbackFns, void *cbContext)
{
    static const Wrapper w{"sdrplay_api_Init"};
    if (!callbackFns) {
        client::apiLog(sdrplay_api_DbgLvl_Error, w.fn, "NULL ptr"); // no record
        return sdrplay_api_InvalidParam;
    }
    sdrplay_api_device *d = devOf(dev);
    if (!d) return nullDev(w);
    // Does not take LockDeviceApi; holds the device mutex for the whole call.
    return locked(w, d, [&] { return d->Init(callbackFns, cbContext); });
}

sdrplay_api_ErrT sdrplay_api_Uninit(HANDLE dev)
{
    static const Wrapper w{"sdrplay_api_Uninit"};
    sdrplay_api_device *d = devOf(dev);
    if (!d) return nullDev(w);
    return locked(w, d, [&] { return d->Uninit(); });
}

sdrplay_api_ErrT sdrplay_api_Update(HANDLE dev, sdrplay_api_TunerSelectT tuner,
                                    sdrplay_api_ReasonForUpdateT reasonForUpdate,
                                    sdrplay_api_ReasonForUpdateExtension1T reasonForUpdateExt1)
{
    static const Wrapper w{"sdrplay_api_Update"};
    sdrplay_api_device *d = devOf(dev);
    if (!d) return nullDev(w);
    return locked(w, d, [&] { return d->Update(tuner, reasonForUpdate, reasonForUpdateExt1); });
}

sdrplay_api_ErrT sdrplay_api_SwapRspDuoActiveTuner(HANDLE dev, sdrplay_api_TunerSelectT *currentTuner,
                                                   sdrplay_api_RspDuo_AmPortSelectT tuner1AmPortSel)
{
    static const Wrapper w{"sdrplay_api_SwapRspDuoActiveTuner"};
    sdrplay_api_device *d = devOf(dev);
    if (!d) return nullDev(w);
    return locked(w, d, [&] { return d->SwapRspDuoActiveTuner(currentTuner, tuner1AmPortSel); });
}

sdrplay_api_ErrT sdrplay_api_SwapRspDuoDualTunerModeSampleRate(HANDLE dev, double *currentSampleRate,
                                                               double newSampleRate)
{
    static const Wrapper w{"sdrplay_api_SwapRspDuoDualTunerModeSampleRate"};
    sdrplay_api_device *d = devOf(dev);
    if (!d) return nullDev(w);
    return locked(w, d, [&] { return d->SwapRspDuoDualTunerModeSampleRate(currentSampleRate, newSampleRate); });
}

// Built from public API calls only; no SetLastError of its own; logs carry a
// trailing "\n".
sdrplay_api_ErrT sdrplay_api_SwapRspDuoMode(sdrplay_api_DeviceT *currDevice, sdrplay_api_DeviceParamsT **deviceParams,
                                            sdrplay_api_RspDuoModeT rspDuoMode, double sampleRate,
                                            sdrplay_api_TunerSelectT tuner, sdrplay_api_Bw_MHzT bwType,
                                            sdrplay_api_If_kHzT ifType, sdrplay_api_RspDuo_AmPortSelectT tuner1AmPortSel)
{
    static const char *const fn = "sdrplay_api_SwapRspDuoMode";
    if (!currDevice) {
        client::apiLog(sdrplay_api_DbgLvl_Error, fn, "NULL ptr");
        client::setClientError(__FILE__, fn, __LINE__, "currDevice NULL ptr");
        return sdrplay_api_InvalidParam;
    }
    sdrplay_api_device *dev = devOf(currDevice->dev);
    if (!dev) {
        client::apiLog(sdrplay_api_DbgLvl_Error, fn, "Device NOT initialised");
        return sdrplay_api_NotInitialised;
    }
    if (currDevice->hwVer != SDRPLAY_RSPduo_ID) {
        client::apiLog(sdrplay_api_DbgLvl_Error, fn, "Invalid hwVer=%d", static_cast<int>(currDevice->hwVer));
        return sdrplay_api_InvalidParam;
    }
    if (rspDuoMode == sdrplay_api_RspDuoMode_Slave) {
        client::apiLog(sdrplay_api_DbgLvl_Error, fn, "Can't change to slave mode");
        return sdrplay_api_InvalidParam;
    }
    if (rspDuoMode == sdrplay_api_RspDuoMode_Dual_Tuner && tuner != sdrplay_api_Tuner_Both) {
        client::apiLog(sdrplay_api_DbgLvl_Error, fn, "Can't change to dual tuner mode, for tuner=%d",
                       static_cast<int>(tuner));
        return sdrplay_api_InvalidParam;
    }
    if (!deviceParams) {
        client::apiLog(sdrplay_api_DbgLvl_Error, fn, "NULL ptr");
        client::setClientError(__FILE__, fn, __LINE__, "deviceParams NULL ptr");
        return sdrplay_api_InvalidParam;
    }
    sdrplay_api_DeviceParamsT *old = *deviceParams;
    bool wasInit = false;
    if (dev->initialised_) {
        for (;;) {
            sdrplay_api_ErrT r = sdrplay_api_Uninit(dev);
            if (r == sdrplay_api_Success) break;
            client::apiLog(sdrplay_api_DbgLvl_Error, fn, "sdrplay_api_Uninit failed %s\n", sdrplay_api_GetErrorString(r));
            if (r != sdrplay_api_StopPending) return r;
            sdrplay_Sleep(1000);
            client::apiLog(sdrplay_api_DbgLvl_Error, fn, "Waiting for slave to uninitialise\n");
        }
        wasInit = true;
    }
    sdrplay_api_CallbackFnsT cb{dev->streamACb_.load(), dev->streamBCb_.load(), dev->eventCb_.load()};
    void *ctxSlot = dev->cbContext_.load();
    sdrplay_api_RxChannelParamsT savedA{}, savedB{};
    sdrplay_api_DevParamsT savedD{};
    sdrplay_api_InternalRfTestT savedI{};
    const bool haveA = old && old->rxChannelA, haveB = old && old->rxChannelB, haveD = old && old->devParams;
    if (haveA) savedA = *old->rxChannelA;
    if (haveB) savedB = *old->rxChannelB;
    if (haveD) savedD = *old->devParams;
    const bool haveI = dev->internal_.rfTest != nullptr;
    if (haveI) savedI = *dev->internal_.rfTest;
    *deviceParams = nullptr;
    sdrplay_api_LockDeviceApi(); // return value ignored
    sdrplay_api_ErrT r = sdrplay_api_ReleaseDevice(currDevice);
    if (r != sdrplay_api_Success) {
        client::apiLog(sdrplay_api_DbgLvl_Error, fn, "sdrplay_api_ReleaseDevice failed %s\n",
                       sdrplay_api_GetErrorString(r));
        return sdrplay_api_Fail; // the API lock stays held
    }
    const sdrplay_api_TunerSelectT oldTuner = currDevice->tuner;
    currDevice->rspDuoSampleFreq = sampleRate;
    currDevice->tuner = tuner;
    currDevice->rspDuoMode = rspDuoMode;
    r = sdrplay_api_SelectDevice(currDevice);
    sdrplay_api_UnlockDeviceApi();
    if (r != sdrplay_api_Success) {
        client::apiLog(sdrplay_api_DbgLvl_Error, fn, "sdrplay_api_SelectDevice failed %s\n",
                       sdrplay_api_GetErrorString(r));
        return sdrplay_api_Fail;
    }
    r = sdrplay_api_GetDeviceParams(currDevice->dev, deviceParams);
    if (r != sdrplay_api_Success) {
        client::apiLog(sdrplay_api_DbgLvl_Error, fn, "sdrplay_api_GetDeviceParams failed %s\n",
                       sdrplay_api_GetErrorString(r));
        return sdrplay_api_Fail;
    }
    auto *nd = static_cast<sdrplay_api_device *>(currDevice->dev);
    if (haveI && nd->internal_.rfTest) *nd->internal_.rfTest = savedI;
    sdrplay_api_DeviceParamsT *p = *deviceParams;
    if (p->devParams && haveD) {
        const double keep = p->devParams->fsFreq.fsHz;
        *p->devParams = savedD;
        p->devParams->fsFreq.fsHz = keep;
    }
    sdrplay_api_RxChannelParamsT *nA = p->rxChannelA, *nB = p->rxChannelB;
    const sdrplay_api_If_kHzT ifA0 = nA ? nA->tunerParams.ifType : static_cast<sdrplay_api_If_kHzT>(-1);
    const sdrplay_api_If_kHzT ifB0 = nB ? nB->tunerParams.ifType : static_cast<sdrplay_api_If_kHzT>(-1);
    const sdrplay_api_TunerSelectT nt = currDevice->tuner;
    if (nt == sdrplay_api_Tuner_Neither) {
        client::apiLog(sdrplay_api_DbgLvl_Error, fn, "Invalid tuner sdrplay_api_Tuner_Neither\n");
        return sdrplay_api_InvalidParam;
    }
    auto restore = [](sdrplay_api_RxChannelParamsT *dst, bool have, const sdrplay_api_RxChannelParamsT &src) {
        if (dst && have) *dst = src;
    };
    const bool oldAorBoth = oldTuner == sdrplay_api_Tuner_A || oldTuner == sdrplay_api_Tuner_Both;
    if (nt == oldTuner) {
        restore(nA, haveA, savedA);
        restore(nB, haveB, savedB);
    } else if (nt == sdrplay_api_Tuner_A || nt == sdrplay_api_Tuner_Both) {
        if (oldAorBoth) {
            restore(nA, haveA, savedA);
        } else if (oldTuner == sdrplay_api_Tuner_B && nA) {
            restore(nA, haveB, savedB);
            nA->rspDuoTunerParams.tuner1AmPortSel = tuner1AmPortSel;
        }
    } else if (nt == sdrplay_api_Tuner_B) {
        if (oldAorBoth && nB)
            restore(nB, haveA, savedA);
        else if (oldTuner == sdrplay_api_Tuner_B && nB)
            restore(nB, haveB, savedB);
    }
    if (rspDuoMode == sdrplay_api_RspDuoMode_Single_Tuner) {
        if (p->devParams) p->devParams->fsFreq.fsHz = sampleRate;
        for (sdrplay_api_RxChannelParamsT *ch : {nA, nB})
            if (ch) {
                ch->tunerParams.bwType = bwType;
                ch->tunerParams.ifType = ifType;
            }
    } else {
        if (nA) {
            nA->tunerParams.bwType = bwType;
            nA->tunerParams.ifType = ifA0; // the ifType argument is ignored
        }
        if (nB) {
            nB->tunerParams.bwType = bwType;
            nB->tunerParams.ifType = ifB0;
        }
    }
    r = sdrplay_api_Success;
    if (wasInit) {
        // Pass the saved context itself as cbContext, not the address of a
        // local copy (that would hand the app a dangling pointer).
        r = sdrplay_api_Init(nd, &cb, ctxSlot);
        if (r != sdrplay_api_Success)
            client::apiLog(sdrplay_api_DbgLvl_Error, fn, "sdrplay_api_Init failed %s\n", sdrplay_api_GetErrorString(r));
    }
    return r;
}

sdrplay_api_ErrT sdrplay_api_GetInternalDeviceParams(HANDLE dev, sdrplay_api_InternalDeviceParamsT **params)
{
    static const Wrapper w{"sdrplay_api_GetInternalDeviceParams", "device NOT initialised",
                           "failed to lock mutex", "failed to unlock mutex"};
    sdrplay_api_device *d = devOf(dev);
    if (!d) return nullDev(w);
    return locked(w, d, [&] {
        if (!params) return nullOut(w);
        *params = d->GetInternalDeviceParams();
        return sdrplay_api_Success;
    });
}

// No initialised check.
sdrplay_api_ErrT sdrplay_api_InternalUpdate(HANDLE dev, sdrplay_api_ReasonForInternalUpdateT reason)
{
    static const Wrapper w{"sdrplay_api_InternalUpdate"};
    sdrplay_api_device *d = devOf(dev);
    if (!d) return nullDev(w);
    return locked(w, d, [&] { return d->InternalUpdate(reason); });
}

} // extern "C"

// C++-linkage twin of GetInternalDeviceParams; it keeps the Wrapper's default null-device
// log text (GetInternalDeviceParams spells its own in lower case).
sdrplay_api_ErrT sdrplay_api_GetInternalParams(void *dev, sdrplay_api_InternalDeviceParamsT **params)
{
    static const Wrapper w{"sdrplay_api_GetInternalParams"};
    sdrplay_api_device *d = devOf(dev);
    if (!d) return nullDev(w);
    return locked(w, d, [&] {
        if (!params) return nullOut(w);
        *params = d->GetInternalDeviceParams();
        return sdrplay_api_Success;
    });
}

// One-slot storage cache for sdrplay_api_device (ClientAbi.h: a device selected after a
// release gets the released handle value).
static std::atomic<void *> g_freedDeviceStorage{nullptr};

void *sdrplay_api_device::operator new(std::size_t n)
{
    if (n == sizeof(sdrplay_api_device))
        if (void *p = g_freedDeviceStorage.exchange(nullptr)) return p;
    return ::operator new(n);
}

void sdrplay_api_device::operator delete(void *p) noexcept
{
    if (!p) return;
    if (void *old = g_freedDeviceStorage.exchange(p)) ::operator delete(old);
}

namespace sdrplay {
namespace client {
void dropFreedDeviceStorage()
{
    // Kept allocated, so the next device gets a new handle value (see ServiceConnection open()).
    static auto *parkedStorage = new std::vector<void *>;
    if (void *p = g_freedDeviceStorage.exchange(nullptr)) parkedStorage->push_back(p);
}
} // namespace client
} // namespace sdrplay
