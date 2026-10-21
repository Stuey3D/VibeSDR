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

// macOS InterProcessSemaphore: the shared-memory semaphore object of the IPC on
// macOS (layout in DarwinShmObjects.h). Algorithms are the Linux ones: 10 ms
// pthread_cond_timedwait slices with deadlines from gettimeofday, timeout -> 2,
// error -> 1, release fails with 1 WITHOUT signalling when count + n > max.
#include "platform/InterProcessSemaphore.h"
#include "platform/SharedMemory.h"
#include "DarwinShmObjects.h"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <sys/time.h>

namespace sdrplay {
namespace platform {

using darwin_shm::SemaphoreObject;

struct InterProcessSemaphore::Impl {
    std::unique_ptr<SharedMemory> shm;      // named objects
    std::unique_ptr<SemaphoreObject> local; // createLocal()
    SemaphoreObject *obj = nullptr;
    std::string name;
    bool creator = false;
};

InterProcessSemaphore::InterProcessSemaphore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

static bool initObject(SemaphoreObject *o, std::int64_t initial, std::int64_t maximum, bool pshared)
{
    pthread_mutexattr_t ma;
    pthread_condattr_t ca;
    if (pthread_mutexattr_init(&ma) != 0) return false;
    bool ok = true;
    if (pshared) ok = pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED) == 0;
    ok = ok && pthread_mutex_init(&o->mutex, &ma) == 0;
    pthread_mutexattr_destroy(&ma);
    if (!ok) return false;
    if (pthread_condattr_init(&ca) != 0) {
        pthread_mutex_destroy(&o->mutex);
        return false;
    }
    if (pshared) ok = pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED) == 0;
    ok = ok && pthread_cond_init(&o->cond, &ca) == 0;
    pthread_condattr_destroy(&ca);
    if (!ok) {
        pthread_mutex_destroy(&o->mutex);
        return false;
    }
    o->count = initial;
    o->max = maximum;
    return true;
}

std::unique_ptr<InterProcessSemaphore> InterProcessSemaphore::create(const std::string &name, std::int64_t initial,
                                                                     std::int64_t maximum, int *osError)
{
    auto shm = SharedMemory::create(name, sizeof(SemaphoreObject), osError);
    if (!shm) return nullptr;
    auto *o = static_cast<SemaphoreObject *>(shm->data());
    if (!initObject(o, initial, maximum, true)) {
        if (osError) *osError = EINVAL;
        return nullptr; // shm destructor unlinks
    }
    auto impl = std::make_unique<Impl>();
    impl->obj = o;
    impl->shm = std::move(shm);
    impl->name = name;
    impl->creator = true;
    return std::unique_ptr<InterProcessSemaphore>(new InterProcessSemaphore(std::move(impl)));
}

std::unique_ptr<InterProcessSemaphore> InterProcessSemaphore::open(const std::string &name, int *osError)
{
    auto shm = SharedMemory::open(name, sizeof(SemaphoreObject), osError);
    if (!shm) return nullptr;
    auto impl = std::make_unique<Impl>();
    impl->obj = static_cast<SemaphoreObject *>(shm->data());
    impl->shm = std::move(shm);
    impl->name = name;
    return std::unique_ptr<InterProcessSemaphore>(new InterProcessSemaphore(std::move(impl)));
}

std::unique_ptr<InterProcessSemaphore> InterProcessSemaphore::createLocal(std::int64_t initial, std::int64_t maximum)
{
    auto impl = std::make_unique<Impl>();
    impl->local = std::make_unique<SemaphoreObject>();
    std::memset(static_cast<void *>(impl->local.get()), 0, sizeof(SemaphoreObject));
    if (!initObject(impl->local.get(), initial, maximum, false)) return nullptr;
    impl->obj = impl->local.get();
    impl->creator = true;
    return std::unique_ptr<InterProcessSemaphore>(new InterProcessSemaphore(std::move(impl)));
}

InterProcessSemaphore::~InterProcessSemaphore()
{
    if (!impl_ || !impl_->obj || !impl_->creator) return; // opener: SharedMemory dtor unmaps
    // The condvar is zeroed first so that waiters that died cannot keep it busy at destroy.
    // Destroying the mutex makes clients still mapping the unlinked object get EINVAL
    // (a heartbeat response error); the SharedMemory destructor then unmaps and unlinks it.
    std::memset(static_cast<void *>(&impl_->obj->cond), 0, sizeof(pthread_cond_t));
    pthread_cond_destroy(&impl_->obj->cond);
    pthread_mutex_destroy(&impl_->obj->mutex);
}

InterProcessSemaphore::WaitResult InterProcessSemaphore::wait(std::uint32_t timeoutMs)
{
    SemaphoreObject *o = impl_->obj;
    if (pthread_mutex_lock(&o->mutex) != 0) return Error;
    if (o->count >= 1) {
        --o->count;
        pthread_mutex_unlock(&o->mutex);
        return Ok;
    }
    if (timeoutMs == 0) {
        pthread_mutex_unlock(&o->mutex);
        return Timeout;
    }
    // 10 ms slices against gettimeofday deadlines (each ETIMEDOUT advances the
    // deadline by exactly 10 ms and costs 10 ms of budget), as on Linux.
    std::int64_t remaining = timeoutMs;
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct timespec ts;
    ts.tv_sec = tv.tv_sec;
    long nsec = static_cast<long>(tv.tv_usec) * 1000L;
    for (;;) {
        nsec += 10L * 1000000L;
        ts.tv_sec += nsec / 1000000000L;
        nsec %= 1000000000L;
        ts.tv_nsec = nsec;
        const int rc = pthread_cond_timedwait(&o->cond, &o->mutex, &ts);
        // Re-check after every wake-up (also after ETIMEDOUT) so a unit released
        // exactly at a slice boundary is not lost.
        if ((rc == 0 || rc == ETIMEDOUT) && o->count >= 1) {
            --o->count;
            pthread_mutex_unlock(&o->mutex);
            return Ok;
        }
        if (rc == ETIMEDOUT) {
            remaining -= 10;
            if (remaining <= 0) {
                pthread_mutex_unlock(&o->mutex);
                return Timeout;
            }
        } else if (rc != 0) {
            pthread_mutex_unlock(&o->mutex);
            return Error;
        }
    }
}

int InterProcessSemaphore::release(std::int64_t n, std::int64_t *previous)
{
    SemaphoreObject *o = impl_->obj;
    if (n < 0 || pthread_mutex_lock(&o->mutex) != 0) return 1;
    if (o->count + n > o->max) { // fail WITHOUT signalling
        pthread_mutex_unlock(&o->mutex);
        return 1;
    }
    if (previous) *previous = o->count;
    for (std::int64_t i = 0; i < n; ++i) {
        ++o->count;
        pthread_cond_signal(&o->cond);
    }
    pthread_mutex_unlock(&o->mutex);
    return 0;
}

std::int64_t InterProcessSemaphore::count() const { return impl_->obj->count; }
std::int64_t InterProcessSemaphore::maximum() const { return impl_->obj->max; }
const std::string &InterProcessSemaphore::name() const { return impl_->name; }
bool InterProcessSemaphore::isCreator() const { return impl_->creator; }
void *InterProcessSemaphore::mappedObject() const { return impl_->obj; }
int InterProcessSemaphore::nativeHandle() const { return impl_->shm ? impl_->shm->nativeHandle() : -1; }

} // namespace platform
} // namespace sdrplay
