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

// sdrplay_api_device: the client-side device object returned in DeviceT.dev.
#include "client/ClientAbi.h"
#include "client/ServiceConnection.h"

#include "core/api/LastError.h"
#include "core/ipc/Commands.h"
#include "core/ipc/Layouts.h"
#include "platform/Clock.h"
#include "platform/Thread.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

using namespace sdrplay;

namespace {
ipc::CmdMap *cmdOf(const sdrplay_api_device *d) { return static_cast<ipc::CmdMap *>(d->cmd_.map); }

unsigned long long ptrValue(const void *p) { return reinterpret_cast<std::uintptr_t>(p); }

const char *const kStrm = "stream thread failed to exit";
const char *const kEvent = "EventHandler failed to exit";
const char *const kHb = "heartbeat thread failed to exit";
// Passed to sdrplay_ThreadJoin, which blocks until the thread exits regardless.
constexpr unsigned long kThreadJoinTimeoutMs = 2000;
} // namespace

// Everything zeroed; the prefix is the 32-byte name root
// from the SelectDevice reply.
sdrplay_api_device::sdrplay_api_device(char hwVer, char *prefix, sdrplay_api_TunerSelectT tuner,
                                       sdrplay_api_RspDuoModeT rspDuoMode, sdrplay_api_log *log)
    : hwVer_(static_cast<unsigned char>(hwVer)), prefix_{}, rspDuoMode_(rspDuoMode), tuner_(tuner), log_(log)
{
    if (prefix) {
        std::memcpy(prefix_, prefix, sizeof prefix_ - 1);
        prefix_[sizeof prefix_ - 1] = '\0';
    }
    client::registerDevice(this);
}

// SelectDeviceCB: copies the three callbacks and the context before the
// threads exist.
sdrplay_api_device::sdrplay_api_device(char hwVer, char *prefix, sdrplay_api_TunerSelectT tuner,
                                       sdrplay_api_RspDuoModeT rspDuoMode, sdrplay_api_log *log,
                                       sdrplay_api_CallbackFnsT *callbackFns, void *cbContext)
    : sdrplay_api_device(hwVer, prefix, tuner, rspDuoMode, log)
{
    // sdrplay_api_SelectDeviceCB rejects NULL callbacks before this point.
    if (!callbackFns) return;
    streamACb_ = callbackFns->StreamACbFn;
    streamBCb_ = callbackFns->StreamBCbFn;
    eventCb_ = callbackFns->EventCbFn;
    cbContext_ = cbContext;
}

// Stops the threads (unless a release already did), releases the shared
// objects and the device mutex.
sdrplay_api_device::~sdrplay_api_device()
{
    // KillThreads (and its "Setting killThreads" log line) runs once per release: skipped
    // here when destroyDevice already ran it.
    if (!leakedBySelfRelease_ && !killedByRelease_) {
        configured_ = false;
        KillThreads();
    }
    DeleteSharedResources();
    if (mutex_) sdrplay_MutexDestroy(&mutex_);
    client::unregisterDevice(this);
}

void sdrplay_api_device::setError(const char *file, const char *function, int line, const char *fmt, ...)
{
    std::va_list ap;
    va_start(ap, fmt);
    api::setLastErrorV(&lastError_, platform::realtimeMicros(), file, function, line, fmt, ap);
    va_end(ap);
}

void sdrplay_api_device::onServiceLost(const char *why)
{
    // Report DeviceRemoved once and fail fast instead of leaving the threads
    // spinning on a dead channel.
    if (serviceLost_.exchange(true)) return;
    client::threadLog(sdrplay_api_DbgLvl_Error, "device", "service lost (%s) - device removed", why);
    removedPending_ = true;
}

sdrplay_api_DeviceParamsT *sdrplay_api_device::GetDeviceParams() { return configured_ ? &params_ : nullptr; }

sdrplay_api_InternalDeviceParamsT *sdrplay_api_device::GetInternalDeviceParams()
{
    return configured_ ? &internal_ : nullptr;
}

int sdrplay_api_device::mutexLock(const char *who)
{
    if (!mutex_) {
        setError(__FILE__, __func__, __LINE__, "mutex not locked %s mtx=%llx", who, 0ull);
        return 1;
    }
    for (;;) {
        int r = sdrplay_MutexLock(mutex_, ipc::kDeviceMutexSliceMs);
        if (r == kOsalOk) return 0;
        client::threadLog(sdrplay_api_DbgLvl_Verbose, __func__, "Waiting for the device mutex");
        if (r != kOsalTimeout) {
            setError(__FILE__, __func__, __LINE__, "mutex not locked %s mtx=%llx err != TIMEOUT", who,
                     ptrValue(mutex_));
            return 1;
        }
    }
}

int sdrplay_api_device::mutexUnlock(const char *who)
{
    if (!mutex_) {
        setError(__FILE__, __func__, __LINE__, "Invalid mtx %s mtx=%llx", who, 0ull);
        return 1;
    }
    return sdrplay_MutexUnlock(mutex_) != kOsalOk ? 1 : 0;
}

int sdrplay_api_device::CreateSharedResources(unsigned char heartbeatDisabled)
{
    if (configured_) return sdrplay_api_AlreadyInitialised;
    const bool slave = rspDuoMode_ == sdrplay_api_RspDuoMode_Slave; // "s" prefix
    char names[4][8];
    const char *const base[4] = {"Hrtbt", "Cmd", "Data", "Event"};
    for (int i = 0; i < 4; ++i) std::snprintf(names[i], sizeof names[i], "%s%s", slave ? "s" : "m", base[i]);
    srvComm_t *const comm[4] = {&hrtbt_, &cmd_, &data_, &event_};
    const int sizes[4] = {0, static_cast<int>(ipc::kCmdMapSize), static_cast<int>(ipc::kDataMapSize),
                          static_cast<int>(ipc::kEventMapSize)};
    for (int i = 0; i < 4; ++i) {
        if (CreateCommObject(this, prefix_, names[i], comm[i], sizes[i], log_) != 0) {
            client::apiLog(sdrplay_api_DbgLvl_Error, __func__, "Could not create %s", names[i]);
            // Always clean up and fail; DeleteCommObject skips the handles of
            // comm objects not reached yet.
            DeleteSharedResources();
            return 1;
        }
    }
    ipc::CmdMap *m = cmdOf(this);
    cmdBuf_ = m;
    params_.devParams = &m->devParams;
    params_.rxChannelA = &m->rxChannelA;
    params_.rxChannelB = &m->rxChannelB;
    internal_.rfTest = &m->internal.params;
    // NULLing rules: which parameter pointers the app sees for this model/mode.
    if (hwVer_ == SDRPLAY_RSPduo_ID) {
        if (rspDuoMode_ == sdrplay_api_RspDuoMode_Slave) params_.devParams = nullptr;
        if (tuner_ == sdrplay_api_Tuner_A)
            params_.rxChannelB = nullptr;
        else if (tuner_ == sdrplay_api_Tuner_B)
            params_.rxChannelA = nullptr;
    } else {
        params_.rxChannelB = nullptr;
    }
    if (CreateThreads(heartbeatDisabled) != 0) {
        client::apiLog(sdrplay_api_DbgLvl_Error, __func__, "failed to create threads");
        return 1;
    }
    configured_ = true;
    return 0;
}

// Maps stay mapped (see DeleteCommObject).
void sdrplay_api_device::DeleteSharedResources()
{
    DeleteCommObject(&hrtbt_);
    DeleteCommObject(&cmd_);
    DeleteCommObject(&data_);
    DeleteCommObject(&event_);
}

// Creates the device mutex, then the stream, event and (unless disabled)
// heartbeat threads.
int sdrplay_api_device::CreateThreads(unsigned char heartbeatDisabled)
{
    if (sdrplay_MutexCreate(&mutex_) != kOsalOk) {
        mutex_ = nullptr;
        client::apiLog(sdrplay_api_DbgLvl_Error, __func__, "Could not create mtx");
        setError(__FILE__, __func__, __LINE__, "Could not create mtx"); // KillThreads not called
        return 1;
    }
    killThreads_ = false;
    struct Spec {
        void **handle;
        void *(*fn)(void *);
        const char *msg;
    };
    const Spec specs[] = {
        {&strmThread_, &strmHandlerThread, "Could not create stream thread"},
        {&eventThread_, &eventHandlerThread, "Could not create event thread"},
        {&hbThread_, &heartBeatThread, "Could not create heartbeat thread"},
    };
    for (const Spec &spec : specs) {
        if (spec.handle == &hbThread_ && heartbeatDisabled) break; // no heartbeat thread
        if (sdrplay_ThreadCreate(spec.handle, reinterpret_cast<void *>(spec.fn), this) != kOsalOk) {
            *spec.handle = nullptr;
            client::apiLog(sdrplay_api_DbgLvl_Error, __func__, "%s", spec.msg);
            setError(__FILE__, __func__, __LINE__, "%s", spec.msg);
            KillThreads();
            return 1;
        }
    }
    return 0;
}

// Joins the stream, event and heartbeat threads in that order (each join blocks
// until the thread exits), then destroys the device mutex.
void sdrplay_api_device::KillThreads()
{
    client::threadLog(sdrplay_api_DbgLvl_Verbose, __func__, "Setting killThreads");
    killThreads_ = true;
    bool joinedSelf = false;
    void *selfHandle = nullptr;
    auto join = [&](void *&h, client::WorkerRole role, const char *failMsg) {
        if (!h) return;
        if (client::isCurrentWorker(this, role)) {
            // ReleaseDevice called from one of our own callbacks: pthread_join
            // would fail with EDEADLK, and freeing the object under the
            // running thread would crash it. Leave the thread to finish on
            // its own (see ReleaseDevice).
            client::threadLog(sdrplay_api_DbgLvl_Warning, __func__, "%s", failMsg);
            joinedSelf = true;
            selfHandle = h; // joined later by client::reapParkedDevices()
            h = nullptr;
            return;
        }
        if (sdrplay_ThreadJoin(h, kThreadJoinTimeoutMs) != kOsalOk)
            client::threadLog(sdrplay_api_DbgLvl_Warning, __func__, "%s", failMsg);
        sdrplay_ThreadDestroy(&h);
        h = nullptr;
    };
    join(strmThread_, client::kStreamWorker, kStrm);
    join(eventThread_, client::kEventWorker, kEvent);
    join(hbThread_, client::kHeartbeatWorker, kHb);
    if (joinedSelf) {
        // Keep the mutex and handles: the calling callback thread still uses
        // the object. Freed by a later API call once that thread has left.
        leakedBySelfRelease_ = true;
        client::unregisterDevice(this);
        client::parkSelfReleasedDevice(this, selfHandle);
        return;
    }
    // NULL the mutex pointer, so a later mutexLock reports an error instead
    // of touching freed memory.
    if (mutex_) sdrplay_MutexDestroy(&mutex_);
    mutex_ = nullptr;
}

// Signals mCmdSem and waits up to 5000 ms for mCmdRSem. No stale-response
// drain, no global lock.
sdrplay_api_ErrT sdrplay_api_device::SendDeviceCommandAndWaitForResponse()
{
    if (!serviceLost_) {
        long prev = 0;
        sdrplay_SharedSemaphoreRelease(cmd_.sem, 1, &prev); // result ignored
        const int r = sdrplay_SharedSemaphoreWait(cmd_.rsem, ipc::kDeviceResponseTimeoutMs);
        if (r == kOsalOk) return sdrplay_api_Success;
        // A broken semaphore with a broken global channel means the service
        // is gone.
        if (r == kOsalError && client::serviceGone()) onServiceLost("device command semaphore broken");
    }
    ipc::CmdMap *m = cmdOf(this);
    const int id = m ? static_cast<int>(m->cmd) : 0; // read after the wait
    client::threadLog(sdrplay_api_DbgLvl_Error, __func__,
                      "response to device command %d timed out - suspect Service problem", id);
    setError(__FILE__, __func__, __LINE__,
             "response to device command %d timed out - suspect Service problem", id);
    return sdrplay_api_Fail;
}

sdrplay_api_ErrT sdrplay_api_device::DebugEnable(sdrplay_api_DbgLvl_t lvl)
{
    if (!configured_) return sdrplay_api_NotInitialised;
    if (mutexLock("DebugEnable") != 0) {
        client::threadLog(sdrplay_api_DbgLvl_Error, __func__, "failed to lock mutex");
        return sdrplay_api_Fail;
    }
    ipc::CmdMap *m = cmdOf(this);
    m->cmd = static_cast<std::uint32_t>(ipc::DeviceCmd::DebugEnable);
    m->dbgLvl = static_cast<std::uint32_t>(lvl);
    if (SendDeviceCommandAndWaitForResponse() != sdrplay_api_Success) {
        // Unlock before returning: a device mutex left locked here would make
        // a later per-device call from another thread block forever while
        // holding the global API mutex. This path only runs when the service
        // does not answer.
        mutexUnlock("DebugEnable");
        return sdrplay_api_ServiceNotResponding;
    }
    if (mutexUnlock("DebugEnable") != 0) {
        client::threadLog(sdrplay_api_DbgLvl_Error, __func__, "failed to unlock mutex");
        return sdrplay_api_Fail;
    }
    return static_cast<sdrplay_api_ErrT>(m->result);
}

// DeviceCmd::GetLastError without the device mutex; copies the service's per-device
// record (CmdMap::lastError) into the device copy (GetLastErrorByType 3).
sdrplay_api_ErrT sdrplay_api_device::DevGetLastError()
{
    ipc::CmdMap *m = cmdOf(this);
    if (!m) return sdrplay_api_Fail; // half-built device
    m->cmd = static_cast<std::uint32_t>(ipc::DeviceCmd::GetLastError);
    if (SendDeviceCommandAndWaitForResponse() != sdrplay_api_Success) return sdrplay_api_ServiceNotResponding;
    std::memcpy(static_cast<void *>(&serviceError_), &m->lastError, sizeof serviceError_);
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_device::Init(sdrplay_api_CallbackFnsT *callbackFns, void *cbContext)
{
    if (!configured_) {
        setError(__FILE__, "Init", __LINE__, "Unconfigured device");
        return sdrplay_api_NotInitialised;
    }
    if (initialised_) {
        setError(__FILE__, "Init", __LINE__, "Device already initialised"); // callbacks NOT replaced
        return sdrplay_api_AlreadyInitialised;
    }
    if (!callbackFns) { // the API wrapper checks first
        setError(__FILE__, "Init", __LINE__, "NULL ptr");
        return sdrplay_api_InvalidParam;
    }
    ipc::CmdMap *m = cmdOf(this);
    m->cmd = static_cast<std::uint32_t>(ipc::DeviceCmd::Init); // the only field written
    // Stored before the service replies and kept on failure.
    streamACb_ = callbackFns->StreamACbFn;
    streamBCb_ = callbackFns->StreamBCbFn;
    eventCb_ = callbackFns->EventCbFn;
    cbContext_ = cbContext;
    if (SendDeviceCommandAndWaitForResponse() != sdrplay_api_Success) return sdrplay_api_ServiceNotResponding;
    const int r = m->result;
    if (r == sdrplay_api_Success) {
        initialised_ = true;
        return sdrplay_api_Success;
    }
    if (r == sdrplay_api_StartPending) return sdrplay_api_StartPending; // RSPduo slave
    client::threadLog(sdrplay_api_DbgLvl_Error, __func__, "Failed to initialise device");
    return sdrplay_api_Fail; // every other service code collapses to Fail
}

sdrplay_api_ErrT sdrplay_api_device::Uninit()
{
    if (!initialised_) {
        setError(__FILE__, "Uninit", __LINE__, "Uninitialised device"); // no log, no IPC
        return sdrplay_api_NotInitialised;
    }
    ipc::CmdMap *m = cmdOf(this);
    m->cmd = static_cast<std::uint32_t>(ipc::DeviceCmd::Uninit);
    if (SendDeviceCommandAndWaitForResponse() != sdrplay_api_Success) return sdrplay_api_ServiceNotResponding;
    const int r = m->result;
    if (r != sdrplay_api_StopPending) initialised_ = false; // cleared even on a service error
    return static_cast<sdrplay_api_ErrT>(r);
}

// No client-side validation; only cmd/tuner/reason/ext1 are written.
sdrplay_api_ErrT sdrplay_api_device::Update(sdrplay_api_TunerSelectT tuner, sdrplay_api_ReasonForUpdateT reason,
                                            sdrplay_api_ReasonForUpdateExtension1T reasonExt1)
{
    if (!initialised_) {
        setError(__FILE__, "Update", __LINE__, "Uninitialised device");
        return sdrplay_api_NotInitialised;
    }
    ipc::CmdMap *m = cmdOf(this);
    m->cmd = static_cast<std::uint32_t>(ipc::DeviceCmd::Update);
    m->tuner = static_cast<std::uint32_t>(tuner);
    m->reason = static_cast<std::uint32_t>(reason);
    m->reasonExt1 = static_cast<std::uint32_t>(reasonExt1);
    if (SendDeviceCommandAndWaitForResponse() != sdrplay_api_Success) return sdrplay_api_ServiceNotResponding;
    return static_cast<sdrplay_api_ErrT>(m->result);
}

// No initialised check.
sdrplay_api_ErrT sdrplay_api_device::InternalUpdate(sdrplay_api_ReasonForInternalUpdateT reason)
{
    ipc::CmdMap *m = cmdOf(this);
    m->cmd = static_cast<std::uint32_t>(ipc::DeviceCmd::InternalUpdate);
    m->internalReason = static_cast<std::uint32_t>(reason);
    if (SendDeviceCommandAndWaitForResponse() != sdrplay_api_Success) return sdrplay_api_ServiceNotResponding;
    return static_cast<sdrplay_api_ErrT>(m->result);
}

// DeviceCmd::SwapTuner.
sdrplay_api_ErrT sdrplay_api_device::SwapTuner(sdrplay_api_TunerSelectT tuner)
{
    ipc::CmdMap *m = cmdOf(this);
    m->cmd = static_cast<std::uint32_t>(ipc::DeviceCmd::SwapTuner);
    m->tuner = static_cast<std::uint32_t>(tuner);
    if (SendDeviceCommandAndWaitForResponse() != sdrplay_api_Success) return sdrplay_api_ServiceNotResponding;
    return static_cast<sdrplay_api_ErrT>(m->result);
}

// 8.192 / 8 MHz are kept with IF 2.048 MHz; anything else
// becomes 6 MHz with IF 1.620 MHz, written into the app-visible shm.
sdrplay_api_ErrT sdrplay_api_device::ChangeSampleRate(double fsHz)
{
    ipc::CmdMap *m = cmdOf(this);
    m->cmd = static_cast<std::uint32_t>(ipc::DeviceCmd::ChangeSampleRate);
    if (fsHz == 8192000.0 || fsHz == 8000000.0) {
        m->devParams.fsFreq.fsHz = fsHz;
        m->rxChannelA.tunerParams.ifType = sdrplay_api_IF_2_048;
    } else {
        m->devParams.fsFreq.fsHz = 6000000.0;
        m->rxChannelA.tunerParams.ifType = sdrplay_api_IF_1_620;
    }
    if (SendDeviceCommandAndWaitForResponse() != sdrplay_api_Success) return sdrplay_api_ServiceNotResponding;
    return static_cast<sdrplay_api_ErrT>(m->result);
}

sdrplay_api_ErrT sdrplay_api_device::SwapRspDuoActiveTuner(sdrplay_api_TunerSelectT *tuner,
                                                           sdrplay_api_RspDuo_AmPortSelectT tuner1AmPortSel)
{
    static const char *const fn = "SwapRspDuoActiveTuner";
    if (!initialised_) return sdrplay_api_NotInitialised; // silent, *tuner not read
    if (!tuner) {
        setError(__FILE__, fn, __LINE__, "NULL ptr");
        return sdrplay_api_InvalidParam;
    }
    const int t = *tuner; // read before the model check
    if (!(hwVer_ == SDRPLAY_RSPduo_ID && t != sdrplay_api_Tuner_Both && t != sdrplay_api_Tuner_Neither &&
          rspDuoMode_ == sdrplay_api_RspDuoMode_Single_Tuner)) {
        client::threadLog(sdrplay_api_DbgLvl_Error, fn, "Invalid state (hwVer=%d, tuner=%d, rspDuoMode=%d)", hwVer_, t,
                          static_cast<int>(rspDuoMode_));
        setError(__FILE__, fn, __LINE__, "Invalid state (hwVer=%d, tuner=%d, rspDuoMode=%d)", hwVer_, t,
                 static_cast<int>(rspDuoMode_));
        return sdrplay_api_InvalidParam;
    }
    sdrplay_api_ErrT r = Uninit();
    if (r != sdrplay_api_Success) {
        client::threadLog(sdrplay_api_DbgLvl_Error, fn, "Uninit() failed %d",static_cast<int>(r));
        return r;
    }
    sdrplay_api_CallbackFnsT cb{streamACb_.load(), streamBCb_.load(), eventCb_.load()};
    void *ctx = cbContext_.load();
    ipc::CmdMap *m = cmdOf(this);
    sdrplay_api_TunerSelectT newT;
    if (*tuner == sdrplay_api_Tuner_A) {
        client::threadLog(sdrplay_api_DbgLvl_Verbose, fn, "Swapping Tuner1 -> Tuner2");
        m->rxChannelB = *params_.rxChannelA;
        params_.rxChannelB = &m->rxChannelB;
        params_.rxChannelA = nullptr;
        newT = sdrplay_api_Tuner_B;
    } else {
        client::threadLog(sdrplay_api_DbgLvl_Verbose, fn, "Swapping Tuner2 -> Tuner1");
        if (!params_.rxChannelB) { // the device stays uninitialised
            client::threadLog(sdrplay_api_DbgLvl_Error, fn, "No tuner B parameters to swap from");
            setError(__FILE__, fn, __LINE__, "No tuner B parameters to swap from");
            return sdrplay_api_Fail;
        }
        m->rxChannelA = *params_.rxChannelB;
        params_.rxChannelA = &m->rxChannelA;
        params_.rxChannelA->rspDuoTunerParams.tuner1AmPortSel = tuner1AmPortSel;
        params_.rxChannelB = nullptr;
        newT = sdrplay_api_Tuner_A;
    }
    *tuner = newT;
    tuner_ = newT;
    r = SwapTuner(newT);
    if (r != sdrplay_api_Success) {
        client::threadLog(sdrplay_api_DbgLvl_Error, fn, "tuner swap failed %d",static_cast<int>(r));
        return r;
    }
    r = Init(&cb, ctx);
    if (r != sdrplay_api_Success) client::threadLog(sdrplay_api_DbgLvl_Error, fn, "Init() failed %d",static_cast<int>(r));
    return r;
}

sdrplay_api_ErrT sdrplay_api_device::SwapRspDuoDualTunerModeSampleRate(double *currentSampleRate, double newSampleRate)
{
    static const char *const fn = "SwapRspDuoDualTunerModeSampleRate";
    if (!(hwVer_ == SDRPLAY_RSPduo_ID && rspDuoMode_ == sdrplay_api_RspDuoMode_Master)) {
        client::threadLog(sdrplay_api_DbgLvl_Error, fn, "Invalid state (hwVer=%d, rspDuoMode=%d)", hwVer_,
                          static_cast<int>(rspDuoMode_));
        setError(__FILE__, fn, __LINE__, "Invalid state (hwVer=%d, rspDuoMode=%d)", hwVer_, static_cast<int>(rspDuoMode_));
        return sdrplay_api_InvalidParam;
    }
    if (!currentSampleRate) { // checked before the swap
        setError(__FILE__, fn, __LINE__, "NULL ptr");
        return sdrplay_api_InvalidParam;
    }
    sdrplay_api_ErrT r = Uninit();
    if (r != sdrplay_api_Success) {
        client::threadLog(sdrplay_api_DbgLvl_Error, fn, "Uninit() failed %d",static_cast<int>(r));
        return r;
    }
    sdrplay_api_CallbackFnsT cb{streamACb_.load(), streamBCb_.load(), eventCb_.load()};
    void *ctx = cbContext_.load();
    r = ChangeSampleRate(newSampleRate);
    if (r != sdrplay_api_Success) {
        client::threadLog(sdrplay_api_DbgLvl_Error, fn, "sample-rate swap failed %d",static_cast<int>(r));
        return r;
    }
    *currentSampleRate = newSampleRate; // the requested value, even when coerced
    r = Init(&cb, ctx);
    if (r != sdrplay_api_Success) client::threadLog(sdrplay_api_DbgLvl_Error, fn, "Init() failed %d",static_cast<int>(r));
    return r;
}
