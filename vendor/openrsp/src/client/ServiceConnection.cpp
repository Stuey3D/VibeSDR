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

// Client side of the global channel: the exported globals,
// Open/Close, LockDeviceApi/UnlockDeviceApi, the command round trips and the
// shared logging/error helpers.
#include "client/ServiceConnection.h"

#include "core/api/LastError.h"
#include "core/ipc/Commands.h"
#include "core/ipc/Names.h"
#include "core/log/Logger.h"
#include "core/util/Strings.h"
#include "platform/Clock.h"
#include "platform/LogSink.h"
#include "platform/SharedMemory.h"
#include "platform/Thread.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <new>
#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#endif
#include <vector>

// Exported data (ABI): the client state and the connect delay. Both are
// zero-initialised; no static constructors run for them.
ServiceState m_serviceState;
int serviceConnectDelayMs = 0;

namespace sdrplay {
namespace client {

namespace {
// Close was called and no Open since (the handle pointers are dangling).
bool g_closed = false;

// Ours: serialises Open/Close (which replace the logger and the handles)
// with the device threads' reads of them (threadLog, serviceGone), so the
// threads never read freed memory after Close.
std::mutex &sessionMutex()
{
    static std::mutex m;
    return m;
}

char *mut(const char *s) { return const_cast<char *>(s); }

std::atomic<platform::LogSink *> g_testSink{nullptr};

struct ForwardingSink : platform::LogSink {
    std::unique_ptr<platform::LogSink> system = platform::createSystemLogSink();
    void write(platform::LogPriority prio, const std::string &line) override
    {
        platform::LogSink *t = g_testSink.load();
        (t ? t : system.get())->write(prio, line);
    }
};

platform::LogSink &rawSink() { return logSink(); }

#if defined(__GNUC__)
__attribute__((format(printf, 4, 0)))
#endif
void vlogVia(sdrplay_api_log *l, sdrplay_api_DbgLvl_t lvl, const char *function, const char *fmt, std::va_list ap)
{
    (void)l->vlog(lvl, function, fmt, ap);
}

std::vector<void *> &retiredHandles()
{
    static auto *v = new std::vector<void *>; // never destroyed: stays reachable at exit
    return *v;
}

std::vector<sdrplay_api_log *> &retiredLoggers()
{
    static auto *v = new std::vector<sdrplay_api_log *>; // never destroyed: stays reachable at exit
    return *v;
}

// Worker thread ids per device and role. Not thread_local: TLS in a shared
// library would add a DT_NEEDED on ld-linux (__tls_get_addr), and the
// dependency list is part of the ABI.
struct Worker {
    const sdrplay_api_device *dev;
    WorkerRole role;
    std::int32_t tid;
};
std::vector<Worker> &workers()
{
    static auto *v = new std::vector<Worker>;
    return *v;
}

std::mutex &registryMutex()
{
    static std::mutex m;
    return m;
}
std::vector<const void *> &registry()
{
    static auto *r = new std::vector<const void *>; // never destroyed (see logSink)
    return *r;
}

#if defined(__unix__) || defined(__APPLE__)
// fork() from a multi-threaded app (test harnesses fork per case, apps may
// fork+exec): only the forking thread survives in the child, so one of these
// process-local mutexes held by another library thread (event/stream/
// heartbeat) at fork time would deadlock the child's next API call. The child
// re-initialises them; it is single-threaded then.
void reinitMutexesInChild()
{
    new (&sessionMutex()) std::mutex;
    new (&registryMutex()) std::mutex;
}
const int g_atforkRegistered = [] {
    sessionMutex();
    registryMutex();
    return pthread_atfork(nullptr, nullptr, reinitMutexesInChild);
}();
#endif
} // namespace

bool sessionClosed() { return g_closed; }

platform::LogSink &logSink()
{
    // Never destroyed: applications call the API from their own static destructors and
    // atexit handlers (SDRangel's SDRplayV3 plugin calls sdrplay_api_Close in its destructor,
    // run by __cxa_finalize after our library's), so the library must not depend
    // on destructors at all.
    static auto *sink = new ForwardingSink;
    return *sink;
}

void setLogSinkForTesting(platform::LogSink *sink) { g_testSink = sink; }

void resetSessionForTesting()
{
    if (m_serviceState.log) retiredLoggers().push_back(m_serviceState.log);
    if (!g_closed)
        for (void *h : {static_cast<void *>(m_serviceState.hShm), static_cast<void *>(m_serviceState.hComMtx),
                        static_cast<void *>(m_serviceState.hCmdSem), static_cast<void *>(m_serviceState.hRespSem)})
            if (h) retiredHandles().push_back(h);
    std::memset(static_cast<void *>(&m_serviceState), 0, sizeof m_serviceState);
    serviceConnectDelayMs = 0;
    g_closed = false;
    clearSelections();
}

void syslogRaw(const char *text)
{
    rawSink().write(platform::LogPriority::Info, "[" + std::to_string(platform::currentThreadId()) + "]: " + text);
}

ipc::ComShMem *com() { return static_cast<ipc::ComShMem *>(m_serviceState.shm); }

bool serviceGone()
{
    std::lock_guard<std::mutex> lk(sessionMutex());
    if (g_closed) return false; // the handles are dangling after Close
    void *mtx = m_serviceState.hComMtx;
    if (!mtx) return false; // no session to judge by
    if (!platform::SharedMemory::exists(ipc::globalName(ipc::kComShMemName))) return true;
    int r = sdrplay_SharedMutexLock(mtx, 0); // probe: trylock only, never waits
    if (r == kOsalOk) {
        sdrplay_SharedMutexUnlock(mtx);
        return false;
    }
    return r != kOsalTimeout; // busy = alive; anything else = destroyed
}

void apiLog(sdrplay_api_DbgLvl_t lvl, const char *function, const char *fmt, ...)
{
    sdrplay_api_log *l = m_serviceState.log;
    if (!l) return; // NULL logger: no line, no crash
    std::va_list ap;
    va_start(ap, fmt);
    vlogVia(l, lvl, function, fmt, ap);
    va_end(ap);
}

void threadLog(sdrplay_api_DbgLvl_t lvl, const char *function, const char *fmt, ...)
{
    std::lock_guard<std::mutex> lk(sessionMutex());
    sdrplay_api_log *l = m_serviceState.log;
    if (!l) return;
    std::va_list ap;
    va_start(ap, fmt);
    vlogVia(l, lvl, function, fmt, ap);
    va_end(ap);
}

void setClientError(const char *file, const char *function, int line, const char *fmt, ...)
{
    std::va_list ap;
    va_start(ap, fmt);
    api::setLastErrorV(&m_serviceState.lastErr, platform::realtimeMicros(), file, function, line, fmt, ap);
    va_end(ap);
}

void registerDevice(sdrplay_api_device *d)
{
    std::lock_guard<std::mutex> lk(registryMutex());
    registry().push_back(d);
}

void unregisterDevice(sdrplay_api_device *d)
{
    std::lock_guard<std::mutex> lk(registryMutex());
    auto &r = registry();
    r.erase(std::remove(r.begin(), r.end(), static_cast<const void *>(d)), r.end());
    auto &w = workers();
    w.erase(std::remove_if(w.begin(), w.end(), [d](const Worker &x) { return x.dev == d; }), w.end());
}

struct Parked {
    sdrplay_api_device *dev;
    void *selfThread; // the calling worker thread's handle, joined when reaped
};
std::vector<Parked> &parked()
{
    static auto *p = new std::vector<Parked>; // never destroyed (see logSink)
    return *p;
}

void setCurrentWorker(const sdrplay_api_device *d, WorkerRole role)
{
    const std::int32_t tid = platform::currentThreadId();
    std::lock_guard<std::mutex> lk(registryMutex());
    auto &w = workers();
    for (Worker &x : w)
        if (x.dev == d && x.role == role) {
            x.tid = tid;
            return;
        }
    w.push_back({d, role, tid});
}

bool isCurrentWorker(const sdrplay_api_device *d, WorkerRole role)
{
    const std::int32_t tid = platform::currentThreadId();
    std::lock_guard<std::mutex> lk(registryMutex());
    for (const Worker &x : workers())
        if (x.dev == d && x.role == role) return x.tid == tid;
    return false;
}

void parkSelfReleasedDevice(sdrplay_api_device *d, void *selfThread)
{
    std::lock_guard<std::mutex> lk(registryMutex());
    parked().push_back({d, selfThread});
}

void reapParkedDevices()
{
    std::vector<Parked> ready;
    {
        std::lock_guard<std::mutex> lk(registryMutex());
        auto &p = parked();
        for (auto it = p.begin(); it != p.end();) {
            if (it->dev->threadsAlive_.load() == 0) {
                ready.push_back(*it);
                it = p.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (Parked &x : ready) {
        if (x.selfThread) {
            sdrplay_ThreadJoin(x.selfThread, 0);
            sdrplay_ThreadDestroy(&x.selfThread);
        }
        delete x.dev; // leakedBySelfRelease_: no KillThreads, frees handles and mutex
    }
}

struct Selection {
    char serNo[sizeof(sdrplay_api_DeviceT::SerNo) + 1];
    sdrplay_api_TunerSelectT tuner;
    bool slave;
    sdrplay_api_device *dev;
};
std::vector<Selection> &selections()
{
    static auto *v = new std::vector<Selection>; // never destroyed (see logSink)
    return *v;
}

void noteSelection(const sdrplay_api_DeviceT &d, sdrplay_api_device *obj)
{
    Selection s{};
    std::memcpy(s.serNo, d.SerNo, sizeof d.SerNo); // the last byte stays 0
    s.tuner = d.tuner;
    s.slave = d.rspDuoMode == sdrplay_api_RspDuoMode_Slave;
    s.dev = obj;
    std::lock_guard<std::mutex> lk(registryMutex());
    selections().push_back(s);
}

sdrplay_api_device *takeSelection(const sdrplay_api_DeviceT &d)
{
    char serNo[sizeof d.SerNo + 1] = {};
    std::memcpy(serNo, d.SerNo, sizeof d.SerNo);
    const bool slave = d.rspDuoMode == sdrplay_api_RspDuoMode_Slave;
    std::lock_guard<std::mutex> lk(registryMutex());
    auto &v = selections();
    auto found = v.end();
    for (auto it = v.begin(); it != v.end(); ++it) {
        if (it->slave != slave || (slave && it->tuner != d.tuner) || util::strCaseCmp(it->serNo, serNo) != 0)
            continue;
        if (found == v.end() || it->dev == d.dev) found = it;
    }
    if (found == v.end()) return nullptr;
    sdrplay_api_device *obj = found->dev;
    v.erase(found);
    return obj;
}

void clearSelections()
{
    std::lock_guard<std::mutex> lk(registryMutex());
    selections().clear();
}

std::vector<const void *> &freedDevices()
{
    static auto *v = new std::vector<const void *>;
    return *v;
}

void noteFreedDevice(const void *d)
{
    std::lock_guard<std::mutex> lk(registryMutex());
    freedDevices().push_back(d);
}

bool isFreedDevice(const void *d)
{
    if (!d) return false;
    std::lock_guard<std::mutex> lk(registryMutex());
    if (std::find(registry().begin(), registry().end(), d) != registry().end()) return false; // reallocated
    auto &f = freedDevices();
    return std::find(f.begin(), f.end(), d) != f.end();
}

bool anyLiveDevice()
{
    std::lock_guard<std::mutex> lk(registryMutex());
    return !registry().empty();
}

bool isLiveDevice(const void *d)
{
    if (!d) return false;
    std::lock_guard<std::mutex> lk(registryMutex());
    auto &r = registry();
    return std::find(r.begin(), r.end(), d) != r.end();
}

// Frees the four global handles. `clearPointers` = false is Close: the
// freed handle pointers stay in m_serviceState (g_closed guards every later
// use).
static void closeHandles(bool clearPointers)
{
    void **h[] = {reinterpret_cast<void **>(&m_serviceState.hRespSem), reinterpret_cast<void **>(&m_serviceState.hCmdSem)};
    for (void **p : h)
        if (*p) sdrplay_SharedSemaphoreClose(p);
    if (m_serviceState.hComMtx) sdrplay_SharedMutexClose(reinterpret_cast<void **>(&m_serviceState.hComMtx));
    if (m_serviceState.hShm) sdrplay_SharedMemoryClose(reinterpret_cast<void **>(&m_serviceState.hShm), &m_serviceState.shm);
    if (clearPointers) {
        m_serviceState.hRespSem = m_serviceState.hCmdSem = m_serviceState.hComMtx = nullptr;
        m_serviceState.hShm = nullptr;
    }
}

static void lockedCloseHandles()
{
    std::lock_guard<std::mutex> lk(sessionMutex());
    closeHandles(true);
}

// Opens ComShMem; nullptr on success, else the error text. With a connect
// delay armed (serviceConnectDelayMs != 0) a failed open is retried every
// 10 ms, delay/10 times; the delay is cleared on success. The retries stop
// once the delay has counted down to <= 0, so a delay that is not a multiple
// of 10 or is negative cannot hang here.
static const char *openComShMem()
{
    const std::string shmName = ipc::globalName(ipc::kComShMemName);
    void **hShm = reinterpret_cast<void **>(&m_serviceState.hShm);
    auto tryOpen = [&] {
        return sdrplay_SharedMemoryOpen(hShm, mut(shmName.c_str()), ipc::kComShMemSize, &m_serviceState.shm) ==
               kOsalOk;
    };
    if (tryOpen()) return nullptr;
    if (serviceConnectDelayMs == 0) return "Could not open file mapping object";
    do {
        sdrplay_Sleep(ipc::kOpenRetrySliceMs);
        // Free the failed attempt's handle (no leak per retry).
        sdrplay_SharedMemoryClose(hShm, &m_serviceState.shm);
        if (tryOpen()) {
            serviceConnectDelayMs = 0;
            return nullptr;
        }
        serviceConnectDelayMs -= static_cast<int>(ipc::kOpenRetrySliceMs);
    } while (serviceConnectDelayMs > 0);
    return "Could not open file mapping object (delayed)";
}

sdrplay_api_ErrT open()
{
    syslogRaw("sdrplay_api_Open"); // always first, before the memset
    {
        std::lock_guard<std::mutex> lk(sessionMutex());
        // A second Open without Close closes the previous session's handles
        // and frees its logger, as Close does (the ComShMem mapping stays, so
        // N Opens leave N ComShMem mappings). Device threads read the logger
        // and the handles only under this mutex.
        if (!g_closed) closeHandles(false);
        delete m_serviceState.log;
        std::memset(static_cast<void *>(&m_serviceState), 0, sizeof m_serviceState);
        // Handle values: on an Open without a Close before it, a device selected next
        // gets a new handle (the freed device storage is not offered to it again).
        // After Close the handle is reused (Release, Close, Open, Select gives the
        // same handle back).
        if (!g_closed) dropFreedDeviceStorage();
        m_serviceState.log = new sdrplay_api_log; // level 3
        g_closed = false;
    }

    if (const char *msg = openComShMem()) {
        apiLog(sdrplay_api_DbgLvl_Error, "sdrplay_api_Open", "%s", msg);
        setClientError(__FILE__, "sdrplay_api_Open", __LINE__, "%s", msg);
        serviceConnectDelayMs = ipc::kServiceConnectDelayAfterFailureMs;
        return sdrplay_api_Fail;
    }
    // The failure paths close and NULL every handle, so a later Lock reports
    // "NULL ptr" instead of using a dangling handle.
    if (sdrplay_SharedMutexOpen(reinterpret_cast<void **>(&m_serviceState.hComMtx),
                                mut(ipc::globalName(ipc::kComMtxName).c_str())) != kOsalOk) {
        apiLog(sdrplay_api_DbgLvl_Error, "sdrplay_api_Open", "Could not create common mutex");
        setClientError(__FILE__, "sdrplay_api_Open", __LINE__, "Could not create common mutex");
        lockedCloseHandles();
        return sdrplay_api_Fail;
    }
    if (sdrplay_SharedSemaphoreOpen(reinterpret_cast<void **>(&m_serviceState.hCmdSem),
                                    mut(ipc::globalName(ipc::kCmdSemaName).c_str()), 0, 1) != kOsalOk) {
        apiLog(sdrplay_api_DbgLvl_Error, "sdrplay_api_Open", "Could not create command semaphore");
        setClientError(__FILE__, "sdrplay_api_Open", __LINE__, "Could not create command semaphore");
        lockedCloseHandles();
        return sdrplay_api_Fail;
    }
    if (sdrplay_SharedSemaphoreOpen(reinterpret_cast<void **>(&m_serviceState.hRespSem),
                                    mut(ipc::globalName(ipc::kRespSemaName).c_str()), 0, 1) != kOsalOk) {
        apiLog(sdrplay_api_DbgLvl_Error, "sdrplay_api_Open", "Could not create response semaphore");
        setClientError(__FILE__, "sdrplay_api_Open", __LINE__, "Could not create response semaphore");
        lockedCloseHandles();
        return sdrplay_api_Fail;
    }
    return sdrplay_api_Success;
}

sdrplay_api_ErrT close()
{
    syslogRaw("sdrplay_api_Close");
    reapParkedDevices();
    // A second Close, a Close without Open and a Close after an Open that
    // failed return an error (the handles are already freed or were never
    // opened, so closing them again would be a double free or a NULL
    // dereference).
    if (g_closed || !m_serviceState.hRespSem) {
        setClientError(__FILE__, "sdrplay_api_Close", __LINE__, "%s", g_closed ? "Already closed" : "Not open");
        return sdrplay_api_Fail;
    }
    // Does not release a selected device or clear the selected flag.
    std::lock_guard<std::mutex> lk(sessionMutex());
    closeHandles(false); // handle pointers left dangling
    delete m_serviceState.log;
    m_serviceState.log = nullptr;
    g_closed = true;
    return sdrplay_api_Success;
}

sdrplay_api_ErrT lockApi()
{
    // After Close the handle is dangling: report "NULL ptr" as without a session.
    if (g_closed || !m_serviceState.hComMtx) {
        setClientError(__FILE__, "sdrplay_api_LockDeviceApi", __LINE__, "NULL ptr"); // no log
        return sdrplay_api_Fail;
    }
    for (;;) {
        int r = sdrplay_SharedMutexLock(m_serviceState.hComMtx, ipc::kComMtxLockSliceMs);
        if (r == kOsalOk) return sdrplay_api_Success;
        if (r != kOsalTimeout) {
            // The service destroyed the mutex (stopped or restarted): report
            // it rather than retry forever.
            setClientError(__FILE__, "sdrplay_api_LockDeviceApi", __LINE__, "Service not responding");
            return sdrplay_api_ServiceNotResponding;
        }
        apiLog(sdrplay_api_DbgLvl_Verbose, "sdrplay_api_LockDeviceApi", "Waiting for the API lock %d", r);
    }
}

sdrplay_api_ErrT unlockApi()
{
    if (g_closed || !m_serviceState.hComMtx) { // after Close the handle is dangling
        setClientError(__FILE__, "sdrplay_api_UnlockDeviceApi", __LINE__, "NULL ptr");
        return sdrplay_api_Fail;
    }
    sdrplay_SharedMutexUnlock(m_serviceState.hComMtx); // result ignored, EPERM swallowed
    return sdrplay_api_Success;
}

// Every never-destroyed function-local static above is created at load time, never lazily
// from a library thread: a fork() while another thread is inside its first-use
// initialisation leaves the __cxa_guard "in progress" in the child, which then blocks forever
// on its next API call.
// Creating them allocates a few empty containers and nothing else (no syscalls, no threads).
namespace {
const int g_eagerStatics = [] {
    registry();
    parked();
    freedDevices();
    selections();
    workers();
    retiredHandles();
    retiredLoggers();
    logSink();
    (void)ipc::objectNamePrefix();
    return 0;
}();
} // namespace

} // namespace client
} // namespace sdrplay

// ------------------------------------------------- exported C++ helpers (ABI)
// Callers hold the API lock (LockDeviceApi).
sdrplay_api_ErrT SendCommandAndWaitForResponse()
{
    using namespace sdrplay;
    if (sdrplay_SharedSemaphoreWait(m_serviceState.hRespSem, 0) == kOsalOk)
        client::syslogRaw("sdrplay_api: stale service response cleared"); // ungated
    long prev = 0;
    if (sdrplay_SharedSemaphoreRelease(m_serviceState.hCmdSem, 1, &prev) == kOsalOk) // result ignored
        m_serviceState.prevCount = prev;
    if (sdrplay_SharedSemaphoreWait(m_serviceState.hRespSem, ipc::kGlobalResponseTimeoutMs) == kOsalOk)
        return sdrplay_api_Success;
    int cmd = client::com() ? static_cast<int>(client::com()->cmd) : 0;
    client::apiLog(sdrplay_api_DbgLvl_Warning, __func__,
                   "response to command %d timed out", cmd);
    client::setClientError(__FILE__, __func__, __LINE__, "response to command %d timed out", cmd);
    return sdrplay_api_Fail;
}

// GetDevices only: no stale-response drain, 20 s, no SetLastError.
sdrplay_api_ErrT SendCommandAndWaitForResponseLong()
{
    using namespace sdrplay;
    long prev = 0;
    if (sdrplay_SharedSemaphoreRelease(m_serviceState.hCmdSem, 1, &prev) == kOsalOk) m_serviceState.prevCount = prev;
    if (sdrplay_SharedSemaphoreWait(m_serviceState.hRespSem, ipc::kGetDevicesResponseTimeoutMs) == kOsalOk)
        return sdrplay_api_Success;
    int cmd = client::com() ? static_cast<int>(client::com()->cmd) : 0;
    client::apiLog(sdrplay_api_DbgLvl_Warning, __func__,
                   "AwaitResponseLong for command %d timed out", cmd);
    return sdrplay_api_Fail;
}

unsigned long long sdrplay_api_GetTimeInMicroSecs() { return sdrplay::platform::realtimeMicros(); }

void sdrplay_api_SetLastError(sdrplay_api_TimedErrorInfoT *err, char *file, const char *function, int line, char *fmt,
                              ...)
{
    std::va_list ap;
    va_start(ap, fmt);
    sdrplay::api::setLastErrorV(err, sdrplay::platform::realtimeMicros(), file, function, line, fmt, ap);
    va_end(ap);
}
