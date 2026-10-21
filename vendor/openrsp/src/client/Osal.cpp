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

// The exported OS abstraction layer (sdrplay_Shared*, sdrplay_Semaphore*,
// sdrplay_Mutex*, sdrplay_Event*, sdrplay_Thread*, sdrplay_Sleep), implemented
// over the portable platform layer. They are exported with default visibility and
// our client calls them through their exported symbols, so an application can
// interpose them like any other export.
//
// SharedMutexLock returns kOsalError on a broken mutex (the service is gone)
// rather than retrying for ever.
#include "client/ClientAbi.h"

#include "core/util/Event.h"
#include "platform/InterProcessMutex.h"
#include "platform/InterProcessSemaphore.h"
#include "platform/SharedMemory.h"
#include "platform/Thread.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using sdrplay::platform::InterProcessMutex;
using sdrplay::platform::InterProcessSemaphore;
using sdrplay::platform::SharedMemory;

namespace {
constexpr std::size_t kNameCopySize = 256;

char *copyName(const char *name)
{
    char *n = static_cast<char *>(std::calloc(1, kNameCopySize));
    if (n && name) std::snprintf(n, kNameCopySize, "%s", name);
    return n;
}

std::uint32_t clampTimeout(unsigned long timeoutMs)
{
    return static_cast<std::uint32_t>(timeoutMs > 0xffffffffUL ? 0xffffffffUL : timeoutMs);
}

// perror() text of a failed open (observable on stderr; kept for compatibility).
void reportOpenError(int err)
{
    std::fprintf(stderr, "shm_open: %s\n", std::strerror(err));
}

InterProcessSemaphore *semOf(void *h)
{
    auto *o = static_cast<OsalObjHandle *>(h);
    return (o && o->kind == kOsalSemaphore) ? static_cast<InterProcessSemaphore *>(o->impl) : nullptr;
}
InterProcessMutex *mtxOf(void *h)
{
    auto *o = static_cast<OsalObjHandle *>(h);
    return (o && o->kind == kOsalMutex) ? static_cast<InterProcessMutex *>(o->impl) : nullptr;
}

OsalObjHandle *wrapSem(std::unique_ptr<InterProcessSemaphore> s, const char *name)
{
    auto *h = static_cast<OsalObjHandle *>(std::calloc(1, sizeof(OsalObjHandle)));
    if (!h) return nullptr;
    h->map = s->mappedObject();
    h->fd = s->nativeHandle();
    h->name = copyName(name);
    h->kind = kOsalSemaphore;
    h->impl = s.release();
    return h;
}
OsalObjHandle *wrapMutex(std::unique_ptr<InterProcessMutex> m, const char *name)
{
    auto *h = static_cast<OsalObjHandle *>(std::calloc(1, sizeof(OsalObjHandle)));
    if (!h) return nullptr;
    h->map = m->mappedObject();
    h->fd = m->nativeHandle();
    h->name = copyName(name);
    h->kind = kOsalMutex;
    h->impl = m.release();
    return h;
}

// A failed *Open leaves a calloc'd handle in *h with fd = -1 and no mapping:
// callers see a non-NULL handle.
void *failedObjHandle()
{
    auto *h = static_cast<OsalObjHandle *>(std::calloc(1, sizeof(OsalObjHandle)));
    if (h) h->fd = -1;
    return h;
}
void *failedShmHandle()
{
    auto *h = static_cast<OsalShmHandle *>(std::calloc(1, sizeof(OsalShmHandle)));
    if (h) h->fd = -1;
    return h;
}

// Close/Destroy free the handle but do NOT set *h to NULL: callers that keep
// the pointer must not use it again.
void freeObj(void **h)
{
    if (!h || !*h) return;
    auto *o = static_cast<OsalObjHandle *>(*h);
    if (o->kind == kOsalSemaphore) {
        auto *s = static_cast<InterProcessSemaphore *>(o->impl);
        delete s; // creator: destroy in place + unlink; opener: unmap
    } else if (o->kind == kOsalMutex) {
        auto *m = static_cast<InterProcessMutex *>(o->impl);
        delete m;
    }
    std::free(o->name);
    std::free(o);
}
} // namespace

// ------------------------------------------------------------- semaphores
int sdrplay_SemaphoreCreateInternal(void **sem, long initial, long max, int shared)
{
    (void)shared; // TODO: the "shared" flag should select pshared attrs for in-shm objects
    if (!sem) return kOsalError;
    auto s = InterProcessSemaphore::createLocal(initial, max);
    if (!s) return kOsalError;
    *sem = wrapSem(std::move(s), nullptr);
    return *sem ? kOsalOk : kOsalError;
}
int sdrplay_SemaphoreDestroyInternal(void **sem, int) { freeObj(sem); return kOsalOk; }
int sdrplay_SemaphoreCreate(void **sem, long initial, long max) { return sdrplay_SemaphoreCreateInternal(sem, initial, max, 0); }
int sdrplay_SemaphoreDestroy(void **sem) { return sdrplay_SemaphoreDestroyInternal(sem, 0); }

int sdrplay_SemaphoreWait(void *sem, unsigned long timeoutMs)
{
    auto *s = semOf(sem);
    if (!s) return kOsalError;
    return static_cast<int>(s->wait(clampTimeout(timeoutMs)));
}
int sdrplay_SemaphoreRelease(void *sem, long count, long *prev)
{
    auto *s = semOf(sem);
    if (!s) return kOsalError;
    if (count <= 0) { // nothing released, *prev written, returns kOsalOk
        if (prev) *prev = static_cast<long>(s->count());
        return kOsalOk;
    }
    std::int64_t p = 0;
    int rc = s->release(count, &p);
    if (rc == 0 && prev) *prev = static_cast<long>(p);
    return rc;
}

int sdrplay_SharedSemaphoreCreate(void **sem, char *name, long initial, long max, void *)
{
    if (!sem || !name) return kOsalError;
    auto s = InterProcessSemaphore::create(name, initial, max);
    if (!s) return kOsalError;
    *sem = wrapSem(std::move(s), name);
    return *sem ? kOsalOk : kOsalError;
}
int sdrplay_SharedSemaphoreOpen(void **sem, char *name, long, long)
{
    if (!sem || !name) return kOsalError;
    int err = 0;
    auto s = InterProcessSemaphore::open(name, &err);
    if (!s) {
        reportOpenError(err);
        *sem = failedObjHandle();
        return kOsalError;
    }
    *sem = wrapSem(std::move(s), name);
    return *sem ? kOsalOk : kOsalError;
}
int sdrplay_SharedSemaphoreClose(void **sem) { freeObj(sem); return kOsalOk; }
int sdrplay_SharedSemaphoreDestroy(void **sem) { freeObj(sem); return kOsalOk; }
int sdrplay_SharedSemaphoreWait(void *sem, unsigned long timeoutMs) { return sdrplay_SemaphoreWait(sem, timeoutMs); }
int sdrplay_SharedSemaphoreRelease(void *sem, long count, long *prev) { return sdrplay_SemaphoreRelease(sem, count, prev); }

// ---------------------------------------------------------------- mutexes
int sdrplay_SharedMutexCreate(void **mtx, char *name, void *)
{
    if (!mtx || !name) return kOsalError;
    auto m = InterProcessMutex::create(name);
    if (!m) return kOsalError;
    *mtx = wrapMutex(std::move(m), name);
    return *mtx ? kOsalOk : kOsalError;
}
int sdrplay_SharedMutexOpen(void **mtx, char *name)
{
    if (!mtx || !name) return kOsalError;
    int err = 0;
    auto m = InterProcessMutex::open(name, &err);
    if (!m) {
        reportOpenError(err);
        *mtx = failedObjHandle();
        return kOsalError;
    }
    *mtx = wrapMutex(std::move(m), name);
    return *mtx ? kOsalOk : kOsalError;
}
int sdrplay_SharedMutexClose(void **mtx) { freeObj(mtx); return kOsalOk; }
int sdrplay_SharedMutexDestroy(void **mtx) { freeObj(mtx); return kOsalOk; }
int sdrplay_SharedMutexLock(void *mtx, unsigned long timeoutMs)
{
    auto *m = mtxOf(mtx);
    if (!m) return kOsalNoObject;
    return static_cast<int>(m->lock(clampTimeout(timeoutMs)));
}
int sdrplay_SharedMutexUnlock(void *mtx)
{
    auto *m = mtxOf(mtx);
    if (!m) return kOsalNoObject;
    return m->unlock();
}

int sdrplay_MutexCreate(void **mtx)
{
    if (!mtx) return kOsalError;
    auto m = InterProcessMutex::createLocal();
    if (!m) return kOsalError;
    *mtx = wrapMutex(std::move(m), nullptr);
    return *mtx ? kOsalOk : kOsalError;
}
int sdrplay_MutexDestroy(void **mtx) { freeObj(mtx); return kOsalOk; }
int sdrplay_MutexLock(void *mtx, unsigned long timeoutMs)
{
    auto *m = mtxOf(mtx);
    if (!m) return kOsalError;
    return static_cast<int>(m->lock(clampTimeout(timeoutMs)));
}
int sdrplay_MutexUnlock(void *mtx)
{
    auto *m = mtxOf(mtx);
    if (!m) return kOsalError;
    return m->unlock();
}

// ---------------------------------------------------------- shared memory
int sdrplay_SharedMemoryCreate(void **shm, char *name, unsigned int size, void **ptr, void *)
{
    if (!shm || !name) return kOsalError;
    auto s = SharedMemory::create(name, size);
    if (!s) return kOsalError;
    auto *h = static_cast<OsalShmHandle *>(std::calloc(1, sizeof(OsalShmHandle)));
    if (!h) return kOsalError;
    h->map = s->data();
    h->size = size;
    h->fd = s->nativeHandle();
    h->name = copyName(name);
    h->impl = s.release();
    *shm = h;
    if (ptr) *ptr = h->map;
    return kOsalOk;
}
int sdrplay_SharedMemoryOpen(void **shm, char *name, unsigned int size, void **ptr)
{
    if (!shm || !name) return kOsalError;
    int err = 0;
    auto s = SharedMemory::open(name, size, &err);
    if (!s) {
        reportOpenError(err);
        *shm = failedShmHandle();
        return kOsalError;
    }
    auto *h = static_cast<OsalShmHandle *>(std::calloc(1, sizeof(OsalShmHandle)));
    if (!h) return kOsalError;
    h->map = s->data();
    h->size = size;
    h->fd = s->nativeHandle();
    h->name = copyName(name);
    h->impl = s.release();
    *shm = h;
    if (ptr) *ptr = h->map;
    return kOsalOk;
}
// Close never unmaps (pointers into the mapping, e.g. DeviceParamsT pointers,
// stay readable after Close); it
// releases only the fd and the handle, and *ptr is not cleared. Destroy unmaps
// and clears *ptr. Neither clears *shm.
static int freeShm(void **shm, void **ptr, bool unmap)
{
    if (!shm || !*shm) return kOsalOk;
    auto *h = static_cast<OsalShmHandle *>(*shm);
    auto *sm = static_cast<SharedMemory *>(h->impl); // NULL for a failed open
    if (sm && !unmap) sm->setUnmapOnDestroy(false);
    delete sm;
    std::free(h->name);
    std::free(h);
    if (unmap && ptr) *ptr = nullptr;
    return kOsalOk;
}
int sdrplay_SharedMemoryClose(void **shm, void **ptr) { return freeShm(shm, ptr, false); }
int sdrplay_SharedMemoryDestroy(void **shm, void **ptr) { return freeShm(shm, ptr, true); }

// ----------------------------------------------------------------- events
int sdrplay_EventCreate(void **ev)
{
    if (!ev) return kOsalError;
    *ev = new sdrplay::util::Event(true);
    return kOsalOk;
}
int sdrplay_EventDestroy(void **ev)
{
    if (!ev || !*ev) return kOsalOk;
    delete static_cast<sdrplay::util::Event *>(*ev);
    *ev = nullptr;
    return kOsalOk;
}
// Waits in 10 ms slices, so even ms == 0 waits one slice.
int sdrplay_EventWait(void *ev, unsigned long timeoutMs)
{
    if (!ev) return kOsalError;
    std::uint32_t t = clampTimeout(timeoutMs);
    if (t < 10) t = 10;
    return static_cast<sdrplay::util::Event *>(ev)->wait(t) ? kOsalOk : kOsalTimeout;
}
int sdrplay_EventSignal(void *ev)
{
    if (!ev) return kOsalError;
    static_cast<sdrplay::util::Event *>(ev)->signal();
    return kOsalOk;
}
int sdrplay_EventReset(void *ev)
{
    if (!ev) return kOsalError;
    static_cast<sdrplay::util::Event *>(ev)->reset();
    return kOsalOk;
}

// ---------------------------------------------------------------- threads
int sdrplay_ThreadCreate(void **thr, void *fn, void *arg)
{
    if (!thr || !fn) return kOsalError;
    using Fn = void *(*)(void *);
    Fn f = reinterpret_cast<Fn>(fn);
    // No name: workers inherit the caller's comm name.
    auto t = sdrplay::platform::Thread::start([f, arg] { f(arg); }, nullptr);
    *thr = t.release();
    return *thr ? kOsalOk : kOsalError;
}
int sdrplay_ThreadDestroy(void **thr)
{
    if (!thr || !*thr) return kOsalOk;
    delete static_cast<sdrplay::platform::Thread *>(*thr); // joins
    *thr = nullptr;
    return kOsalOk;
}
// NULL -> kOsalOk; the timeout is ignored (blocks until the thread exits). The
// handle stays valid until sdrplay_ThreadDestroy so that Join followed by
// Destroy is not a double free.
int sdrplay_ThreadJoin(void *thr, unsigned long timeoutMs)
{
    (void)timeoutMs;
    if (!thr) return kOsalOk;
    return static_cast<sdrplay::platform::Thread *>(thr)->join(UINT32_MAX) ? kOsalOk : kOsalError;
}

// Sleeps the full delay (in slices when it exceeds the platform call's range).
void sdrplay_Sleep(unsigned long ms)
{
    unsigned long long left = ms;
    while (left > UINT32_MAX) {
        sdrplay::platform::sleepMs(UINT32_MAX);
        left -= UINT32_MAX;
    }
    sdrplay::platform::sleepMs(static_cast<std::uint32_t>(left));
}
