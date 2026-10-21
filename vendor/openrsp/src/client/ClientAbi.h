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

// Every symbol libsdrplay_api.so.3 exports, with the API 3.15 prototypes.
//
// The C functions are declared by the API header. The C++ functions/classes
// below are ABI too: their C++ symbol names are part of the 3.15 export list.
// Do not change any parameter type, constness or class/type name here - the
// exported symbol names would change. Class member LAYOUT is ours (objects are only
// created inside the library).
//
// `long`/`unsigned long` appear in some signatures because the exported C++
// symbol names encode the parameter types; they never appear in shared-memory
// layouts.
#pragma once

#include "core/api/ApiTypes.h"

#include <atomic>
#include <cstdarg>
#include <climits>
#include <cstdint>
#include <memory>

namespace sdrplay {
namespace log {
class Logger;
}
} // namespace sdrplay

// --------------------------------------------------------------- OSAL handles
// Results of the OS abstraction layer functions (sdrplay_Semaphore*, sdrplay_Mutex*,
// sdrplay_Shared*, sdrplay_Event*, sdrplay_Thread*).
enum OsalResult : int {
    kOsalOk = 0,
    kOsalError = 1,
    kOsalTimeout = 2,
    kOsalNoObject = -1, // SharedMutexLock/Unlock on a handle without a mutex
};

// OSAL handles: callers read the leading `map` field. `name` is a zeroed 256-byte
// copy of the object name; `impl` points to the platform object.
enum OsalObjKind : int { kOsalSemaphore = 1, kOsalMutex = 2 };
struct OsalShmHandle {
    void *map;
    std::uint32_t size;
    int fd;
    char *name;
    void *impl; // sdrplay::platform::SharedMemory*
};
struct OsalObjHandle {
    void *map;
    int fd;
    char *name;
    void *impl; // platform::InterProcessSemaphore* / InterProcessMutex* (kind below)
    int kind;   // OsalObjKind
};

// One request/response triple (Sem, RSem, Map) of the device channel set.
// The type name is ABI: it is part of the exported InitCommObject/CreateCommObject/
// DeleteCommObject symbol names.
struct srvComm_t {
    void *shm;  // OsalShmHandle* (NULL for Hrtbt)
    void *map;  // mapping (NULL for Hrtbt)
    void *sem;  // OsalObjHandle* "<root>_<x>Sem"  (client -> service)
    void *rsem; // OsalObjHandle* "<root>_<x>RSem" (service -> client)
};

// ------------------------------------------------------------------- classes
class sdrplay_api_log {
public:
    sdrplay_api_log();
    ~sdrplay_api_log();
    void log_enable(sdrplay_api_DbgLvl_t lvl);
    sdrplay_api_DbgLvl_t get_log_enable();
    int log(sdrplay_api_DbgLvl_t lvl, const char *function, char *fmt, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 4, 5))) // attributes are not part of the symbol name
#endif
        ;

    // Non-exported helpers: the gate, the gated writer and the line writer.
    bool passes(sdrplay_api_DbgLvl_t msgLvl) const;
    // The body of log(): 0 when the gate drops the message or it was written,
    // -1 for a level outside 1..4 (passes the gate, not written).
    int vlog(sdrplay_api_DbgLvl_t msgLvl, const char *function, const char *fmt, std::va_list ap)
#if defined(__GNUC__)
        __attribute__((format(printf, 4, 0)))
#endif
        ;
    void vwrite(sdrplay_api_DbgLvl_t msgLvl, const char *function, const char *fmt, std::va_list ap)
#if defined(__GNUC__)
        __attribute__((format(printf, 4, 0)))
#endif
        ;

private:
    // Applications may read the debug level through the exported m_serviceState.log,
    // so the level is the first (and only) member. Atomic because device threads
    // read it while the app changes it.
    std::atomic<int> level_;
};

class sdrplay_api_device {
public:
    sdrplay_api_device(char hwVer, char *prefix, sdrplay_api_TunerSelectT tuner, sdrplay_api_RspDuoModeT rspDuoMode,
                       sdrplay_api_log *log);
    sdrplay_api_device(char hwVer, char *prefix, sdrplay_api_TunerSelectT tuner, sdrplay_api_RspDuoModeT rspDuoMode,
                       sdrplay_api_log *log, sdrplay_api_CallbackFnsT *callbackFns, void *cbContext);
    ~sdrplay_api_device();
    // A SelectDevice after a ReleaseDevice returns the same DeviceT.dev as the released
    // device (the handle value repeats): the freed object's storage is
    // kept in a one-slot cache and reused.
    static void *operator new(std::size_t n);
    static void operator delete(void *p) noexcept;

    sdrplay_api_DeviceParamsT *GetDeviceParams();
    sdrplay_api_InternalDeviceParamsT *GetInternalDeviceParams();
    sdrplay_api_ErrT SendDeviceCommandAndWaitForResponse();
    sdrplay_api_ErrT DevGetLastError();
    sdrplay_api_ErrT Init(sdrplay_api_CallbackFnsT *callbackFns, void *cbContext);
    sdrplay_api_ErrT Uninit();
    sdrplay_api_ErrT Update(sdrplay_api_TunerSelectT tuner, sdrplay_api_ReasonForUpdateT reason,
                            sdrplay_api_ReasonForUpdateExtension1T reasonExt1);
    sdrplay_api_ErrT SwapTuner(sdrplay_api_TunerSelectT tuner);
    sdrplay_api_ErrT SwapRspDuoActiveTuner(sdrplay_api_TunerSelectT *tuner,
                                           sdrplay_api_RspDuo_AmPortSelectT tuner1AmPortSel);
    sdrplay_api_ErrT ChangeSampleRate(double fsHz);
    sdrplay_api_ErrT SwapRspDuoDualTunerModeSampleRate(double *currentSampleRate, double newSampleRate);
    sdrplay_api_ErrT InternalUpdate(sdrplay_api_ReasonForInternalUpdateT reason);
    void KillThreads();
    int CreateThreads(unsigned char heartbeatDisabled);
    int mutexLock(const char *who);
    int mutexUnlock(const char *who);
    sdrplay_api_ErrT DebugEnable(sdrplay_api_DbgLvl_t lvl);
    int CreateSharedResources(unsigned char heartbeatDisabled);
    void DeleteSharedResources();

    // Non-exported helpers (hidden by cmake/libsdrplay_api.map "local: *").
    void setError(const char *file, const char *function, int line, const char *fmt, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 5, 6)))
#endif
        ;
    void *cmdMap() const { return cmd_.map; }
    // The service died or was restarted (we report this; callers would otherwise hang).
    // Marks the device lost and queues one DeviceRemoved event (delivered on
    // the event thread). Idempotent.
    void onServiceLost(const char *why);
    bool serviceLost() const { return serviceLost_.load(); }

    // ---- members ----
    unsigned char hwVer_;
    char prefix_[32];                 // object-name root from the SelectDevice reply
    void *cmdBuf_ = nullptr;          // device command block (= mCmdMap base)
    void *strmThread_ = nullptr;
    void *eventThread_ = nullptr;
    void *hbThread_ = nullptr;        // NULL with heartbeat disabled
    bool configured_ = false;         // set by CreateSharedResources
    bool initialised_ = false;        // set by Init, cleared by Uninit
    std::atomic<bool> killThreads_{false};
    std::atomic<bool> serviceLost_{false};    // service died or restarted
    std::atomic<bool> removedPending_{false}; // DeviceRemoved queued for the event thread
    bool leakedBySelfRelease_ = false;        // released from its own callback thread
    bool killedByRelease_ = false;            // destroyDevice already ran KillThreads
    void *mutex_ = nullptr;           // process-private recursive robust mutex
    srvComm_t hrtbt_{}, cmd_{}, data_{}, event_{};
    sdrplay_api_DeviceParamsT params_{};           // returned by GetDeviceParams
    sdrplay_api_InternalDeviceParamsT internal_{}; // returned by GetInternalDeviceParams
    sdrplay_api_TimedErrorInfoT lastError_{};      // device-local record (GetLastErrorByType 1)
    sdrplay_api_TimedErrorInfoT serviceError_{};   // copy of the service device record (GetLastErrorByType 3)
    sdrplay_api_RspDuoModeT rspDuoMode_;           // from the caller's DeviceT
    sdrplay_api_TunerSelectT tuner_;               // from the caller's DeviceT
    // Callbacks and context. The threads read them unlocked on every
    // buffer/event; atomics make that race-free.
    std::atomic<sdrplay_api_StreamCallback_t> streamACb_{nullptr};
    std::atomic<sdrplay_api_StreamCallback_t> streamBCb_{nullptr};
    std::atomic<sdrplay_api_EventCallback_t> eventCb_{nullptr};
    std::atomic<void *> cbContext_{nullptr};
    unsigned int bufIdxExpected_ = 0; // not reset by Init/Uninit
    std::atomic<int> threadsAlive_{0}; // workers still using *this
    sdrplay_api_log *log_;            // we log through m_serviceState.log
};

// ------------------------------------------------- client global state (data)
// Exported as `m_serviceState`, zeroed by sdrplay_api_Open. Its size is ABI
// (3176 bytes on LP64, 3144 on ILP32): applications linked against the library
// carry copy relocations sized to it.
struct ServiceState {
    OsalShmHandle *hShm;     // Glbl\sdrSrvComShMem
    OsalObjHandle *hCmdSem;  // Glbl\sdrSrvCmdSema
    OsalObjHandle *hRespSem; // Glbl\sdrSrvRespSema
    OsalObjHandle *hComMtx;  // Glbl\sdrSrvComMtx (NULL = not open)
    void *shm;               // ipc::ComShMem mapping
    // `long`, not int64: it receives the `long *` out-parameter of
    // sdrplay_SharedSemaphoreRelease.
    long prevCount;
    sdrplay_api_log *log;
    std::uint8_t hbDisabled;
    sdrplay_api_TimedErrorInfoT lastErr;    // client-global record
    sdrplay_api_TimedErrorInfoT serviceErr; // copy of the service-global record (GlobalCmd::GetLastError)
    std::uint8_t deviceSelected;
};

// ------------------------------------------------------------- layout checks
// The OSAL handles are read by callers; the exported m_serviceState has a fixed
// size (see ServiceState).
#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu
static_assert(offsetof(OsalShmHandle, size) == 8, "OsalShmHandle layout");
static_assert(offsetof(OsalShmHandle, fd) == 12, "OsalShmHandle layout");
static_assert(offsetof(OsalShmHandle, name) == 16, "OsalShmHandle layout");
static_assert(offsetof(OsalObjHandle, fd) == 8, "OsalObjHandle layout");
static_assert(offsetof(OsalObjHandle, name) == 16, "OsalObjHandle layout");
static_assert(sizeof(srvComm_t) == 32, "srvComm_t layout");
#endif
#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu && LONG_MAX == 0x7FFFFFFFFFFFFFFFL
static_assert(sizeof(ServiceState) == 3176, "ServiceState size");
#elif UINTPTR_MAX == 0xFFFFFFFFu
static_assert(sizeof(ServiceState) == 3144, "ServiceState size (ILP32)");
#endif

extern ServiceState m_serviceState;
extern int serviceConnectDelayMs;

// ------------------------------------------- C++-linkage free functions (ABI)
sdrplay_api_ErrT sdrplay_api_GetInternalParams(void *dev, sdrplay_api_InternalDeviceParamsT **params);
unsigned long long sdrplay_api_GetTimeInMicroSecs();
void sdrplay_api_SetLastError(sdrplay_api_TimedErrorInfoT *err, char *file, const char *function, int line,
                              char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 5, 6)))
#endif
    ;
sdrplay_api_ErrT SendCommandAndWaitForResponse();
sdrplay_api_ErrT SendCommandAndWaitForResponseLong();
void *eventHandlerThread(void *arg);
void *strmHandlerThread(void *arg);
void *heartBeatThread(void *arg);
void InitCommObject(srvComm_t *c);
int CreateCommObject(sdrplay_api_device *dev, char *prefix, char *channel, srvComm_t *c, int mapSize,
                     sdrplay_api_log *log);
void DeleteCommObject(srvComm_t *c);

// OS abstraction layer (Windows-style API over the platform layer). Results:
// OsalResult.
int sdrplay_SemaphoreCreateInternal(void **sem, long initial, long max, int shared);
int sdrplay_SemaphoreDestroyInternal(void **sem, int shared);
int sdrplay_SemaphoreCreate(void **sem, long initial, long max);
int sdrplay_SemaphoreDestroy(void **sem);
int sdrplay_SemaphoreWait(void *sem, unsigned long timeoutMs);
int sdrplay_SemaphoreRelease(void *sem, long count, long *prev);
int sdrplay_SharedSemaphoreCreate(void **sem, char *name, long initial, long max, void *sa);
int sdrplay_SharedSemaphoreOpen(void **sem, char *name, long initial, long max);
int sdrplay_SharedSemaphoreClose(void **sem);
int sdrplay_SharedSemaphoreDestroy(void **sem);
int sdrplay_SharedSemaphoreWait(void *sem, unsigned long timeoutMs);
int sdrplay_SharedSemaphoreRelease(void *sem, long count, long *prev);
int sdrplay_SharedMutexCreate(void **mtx, char *name, void *sa);
int sdrplay_SharedMutexOpen(void **mtx, char *name);
int sdrplay_SharedMutexClose(void **mtx);
int sdrplay_SharedMutexDestroy(void **mtx);
int sdrplay_SharedMutexLock(void *mtx, unsigned long timeoutMs);
int sdrplay_SharedMutexUnlock(void *mtx);
int sdrplay_SharedMemoryCreate(void **shm, char *name, unsigned int size, void **ptr, void *sa);
int sdrplay_SharedMemoryOpen(void **shm, char *name, unsigned int size, void **ptr);
int sdrplay_SharedMemoryClose(void **shm, void **ptr);
int sdrplay_SharedMemoryDestroy(void **shm, void **ptr);
int sdrplay_MutexCreate(void **mtx);
int sdrplay_MutexDestroy(void **mtx);
int sdrplay_MutexLock(void *mtx, unsigned long timeoutMs);
int sdrplay_MutexUnlock(void *mtx);
int sdrplay_EventCreate(void **ev);
int sdrplay_EventDestroy(void **ev);
int sdrplay_EventWait(void *ev, unsigned long timeoutMs);
int sdrplay_EventSignal(void *ev);
int sdrplay_EventReset(void *ev);
int sdrplay_ThreadCreate(void **thr, void *fn, void *arg);
int sdrplay_ThreadDestroy(void **thr);
int sdrplay_ThreadJoin(void *thr, unsigned long timeoutMs);
void sdrplay_Sleep(unsigned long ms);

// Undocumented C exports (not in the public header).
extern "C" {
sdrplay_api_ErrT sdrplay_api_SelectDeviceCB(sdrplay_api_DeviceT *device, sdrplay_api_CallbackFnsT *callbackFns,
                                            void *cbContext);
sdrplay_api_ErrT sdrplay_api_GetInternalDeviceParams(HANDLE dev, sdrplay_api_InternalDeviceParamsT **params);
sdrplay_api_ErrT sdrplay_api_InternalUpdate(HANDLE dev, sdrplay_api_ReasonForInternalUpdateT reason);
}

