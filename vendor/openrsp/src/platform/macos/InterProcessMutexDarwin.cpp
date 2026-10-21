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

// macOS InterProcessMutex: a pshared + recursive Darwin pthread_mutex_t that is the
// whole shared-memory object (layout in DarwinShmObjects.h). Not robust: Darwin has no robust mutexes, so a client
// that dies holding Glbl\sdrSrvComMtx leaves it locked until the service
// recreates it.
#include "platform/InterProcessMutex.h"
#include "platform/SharedMemory.h"
#include "platform/Thread.h"
#include "DarwinShmObjects.h"

#include <cerrno>
#include <cstring>

namespace sdrplay {
namespace platform {

using darwin_shm::MutexObject;

struct InterProcessMutex::Impl {
    std::unique_ptr<SharedMemory> shm;
    std::unique_ptr<MutexObject> local;
    MutexObject *obj = nullptr;
    std::string name;
    bool creator = false;
};

InterProcessMutex::InterProcessMutex(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

static bool initMutex(MutexObject *m, bool pshared)
{
    pthread_mutexattr_t ma;
    if (pthread_mutexattr_init(&ma) != 0) return false;
    // Recursive, pshared for the named mutex; Darwin has no robust mutexes.
    bool ok = true;
    if (pshared) ok = pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED) == 0;
    ok = ok && pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE) == 0;
    ok = ok && pthread_mutex_init(m, &ma) == 0;
    pthread_mutexattr_destroy(&ma);
    return ok;
}

std::unique_ptr<InterProcessMutex> InterProcessMutex::create(const std::string &name, int *osError)
{
    auto shm = SharedMemory::create(name, sizeof(MutexObject), osError);
    if (!shm) return nullptr;
    auto *m = static_cast<MutexObject *>(shm->data());
    if (!initMutex(m, true)) {
        if (osError) *osError = EINVAL;
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->obj = m;
    impl->shm = std::move(shm);
    impl->name = name;
    impl->creator = true;
    return std::unique_ptr<InterProcessMutex>(new InterProcessMutex(std::move(impl)));
}

std::unique_ptr<InterProcessMutex> InterProcessMutex::open(const std::string &name, int *osError)
{
    auto shm = SharedMemory::open(name, sizeof(MutexObject), osError);
    if (!shm) return nullptr;
    auto impl = std::make_unique<Impl>();
    impl->obj = static_cast<MutexObject *>(shm->data());
    impl->shm = std::move(shm);
    impl->name = name;
    return std::unique_ptr<InterProcessMutex>(new InterProcessMutex(std::move(impl)));
}

std::unique_ptr<InterProcessMutex> InterProcessMutex::createLocal()
{
    auto impl = std::make_unique<Impl>();
    impl->local = std::make_unique<MutexObject>();
    std::memset(static_cast<void *>(impl->local.get()), 0, sizeof(MutexObject));
    if (!initMutex(impl->local.get(), false)) return nullptr;
    impl->obj = impl->local.get();
    impl->creator = true;
    return std::unique_ptr<InterProcessMutex>(new InterProcessMutex(std::move(impl)));
}

InterProcessMutex::~InterProcessMutex()
{
    if (impl_ && impl_->obj && impl_->creator) pthread_mutex_destroy(impl_->obj);
}

InterProcessMutex::LockResult InterProcessMutex::lock(std::uint32_t timeoutMs)
{
    // trylock; while busy, a further try after each 10 ms slice until the budget is used up.
    // There is no owner-dead path (Darwin has no robust mutexes). As on Linux, any other
    // error (e.g. EINVAL on a mutex the service destroyed) returns Error instead of spinning
    // forever.
    std::int64_t remaining = timeoutMs;
    for (;;) {
        const int rc = pthread_mutex_trylock(impl_->obj);
        if (rc == 0) return Ok;
        if (rc != EBUSY) return Error;
        if (remaining < 1) return Timeout;
        remaining -= 10;
        sleepMs(10);
    }
}

int InterProcessMutex::unlock() { return pthread_mutex_unlock(impl_->obj) == 0 ? 0 : 1; }

const std::string &InterProcessMutex::name() const { return impl_->name; }
bool InterProcessMutex::isCreator() const { return impl_->creator; }
void *InterProcessMutex::mappedObject() const { return impl_->obj; }
int InterProcessMutex::nativeHandle() const { return impl_->shm ? impl_->shm->nativeHandle() : -1; }

} // namespace platform
} // namespace sdrplay
