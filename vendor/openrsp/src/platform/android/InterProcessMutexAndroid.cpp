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

// Android InterProcessMutex, layout v2 (AndroidShm.h): a recursive futex mutex in our
// own shm object, with the InterProcessMutex lock(timeout) contract: one try, then 10 ms waits while
// busy, budget exhausted -> Timeout(2), timeout 0 -> a single try.
//
// Dead holders (the Linux ROBUST mutex's EOWNERDEAD path, which ends with the lock
// released): a waiter whose 10 ms slice expires checks whether the owning thread still
// exists (kill(tid, 0) == ESRCH) and, if not, takes the lock over. In service mode the
// OpenRSP app also calls android_shm::recoverDeadOwner(pid) as soon as Binder reports a
// client dead. Bionic pthread mutexes cannot do either (no robust mutexes, no unlock by a
// non-owner), which is why v1 left a dead holder's lock held forever.
#include "platform/InterProcessMutex.h"
#include "platform/SharedMemory.h"
#include "platform/Clock.h"
#include "AndroidFutex.h"
#include "AndroidShm.h"

#include <cerrno>
#include <new>
#include <unistd.h>

namespace sdrplay {
namespace platform {

using android_shm::MutexObject;

struct InterProcessMutex::Impl {
    std::unique_ptr<SharedMemory> shm;
    std::unique_ptr<MutexObject> local;
    MutexObject *obj = nullptr;
    std::string name;
    bool creator = false;
};

InterProcessMutex::InterProcessMutex(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

static void initMutex(MutexObject *m)
{
    new (&m->state) std::atomic<std::uint32_t>(0);
    new (&m->ownerTid) std::atomic<std::uint32_t>(0);
    new (&m->waiters) std::atomic<std::uint32_t>(0);
    m->ownerPid = 0;
    m->recursion = 0;
    m->magic = android_shm::kMutexMagic;
    m->state.store(android_shm::kStateAlive, std::memory_order_release);
}

static bool alive(const MutexObject *m)
{
    return m->magic == android_shm::kMutexMagic &&
           m->state.load(std::memory_order_acquire) == android_shm::kStateAlive;
}

static std::uint32_t selfTid() { return static_cast<std::uint32_t>(gettid()); }

std::unique_ptr<InterProcessMutex> InterProcessMutex::create(const std::string &name, int *osError)
{
    auto shm = SharedMemory::create(name, sizeof(MutexObject), osError);
    if (!shm) return nullptr;
    auto *m = static_cast<MutexObject *>(shm->data());
    initMutex(m);
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
    auto *m = static_cast<MutexObject *>(shm->data());
    if (m->magic != android_shm::kMutexMagic) { // another kind of object, or a v1 layout
        if (osError) *osError = EINVAL;
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->obj = m;
    impl->shm = std::move(shm);
    impl->name = name;
    return std::unique_ptr<InterProcessMutex>(new InterProcessMutex(std::move(impl)));
}

std::unique_ptr<InterProcessMutex> InterProcessMutex::createLocal()
{
    auto impl = std::make_unique<Impl>();
    impl->local = std::make_unique<MutexObject>();
    initMutex(impl->local.get());
    impl->obj = impl->local.get();
    impl->creator = true;
    return std::unique_ptr<InterProcessMutex>(new InterProcessMutex(std::move(impl)));
}

InterProcessMutex::~InterProcessMutex()
{
    if (!impl_ || !impl_->obj || !impl_->creator || impl_->local) return;
    // Named: openers must see Error, not a hang. Mark it dead and wake every waiter.
    impl_->obj->state.store(android_shm::kStateDestroyed, std::memory_order_release);
    android_shm::futexWakeAll(&impl_->obj->ownerTid);
}

// One acquisition attempt. True when the caller now holds the mutex.
static bool tryAcquire(MutexObject *m, std::uint32_t me)
{
    std::uint32_t expected = 0;
    if (m->ownerTid.compare_exchange_strong(expected, me, std::memory_order_acquire, std::memory_order_relaxed)) {
        m->ownerPid = static_cast<std::uint32_t>(getpid());
        m->recursion = 0;
        return true;
    }
    if (expected == me) { // recursive lock by the owner
        ++m->recursion;
        return true;
    }
    return false;
}

// The owner thread is gone (killed process, or a thread that exited holding it): release it.
static void releaseIfOwnerDead(MutexObject *m)
{
    std::uint32_t owner = m->ownerTid.load(std::memory_order_acquire);
    if (owner == 0 || !android_shm::tidIsDead(owner)) return;
    if (m->ownerTid.compare_exchange_strong(owner, 0u, std::memory_order_acq_rel)) {
        m->recursion = 0;
        m->ownerPid = 0;
        android_shm::futexWakeAll(&m->ownerTid);
    }
}

InterProcessMutex::LockResult InterProcessMutex::lock(std::uint32_t timeoutMs)
{
    MutexObject *m = impl_->obj;
    if (!alive(m)) return Error; // destroyed -> Error, not a spin
    const std::uint32_t me = selfTid();
    if (tryAcquire(m, me)) {
        if (alive(m)) return Ok;
        unlock();
        return Error;
    }
    if (timeoutMs == 0) return Timeout; // busy with timeout 0
    const std::uint64_t deadline = monotonicNanos() + static_cast<std::uint64_t>(timeoutMs) * 1000000u;
    for (;;) {
        const std::uint32_t owner = m->ownerTid.load(std::memory_order_acquire);
        if (owner != 0) {
            m->waiters.fetch_add(1, std::memory_order_acq_rel);
            const int rc = android_shm::futexWaitMs(&m->ownerTid, owner, 10);
            m->waiters.fetch_sub(1, std::memory_order_acq_rel);
            if (rc == ETIMEDOUT) releaseIfOwnerDead(m);
        }
        if (!alive(m)) return Error;
        if (tryAcquire(m, me)) {
            if (alive(m)) return Ok;
            unlock();
            return Error;
        }
        if (monotonicNanos() >= deadline) return Timeout;
    }
}

int InterProcessMutex::unlock()
{
    MutexObject *m = impl_->obj;
    if (m->ownerTid.load(std::memory_order_acquire) != selfTid()) return 1; // not the owner (EPERM)
    if (m->recursion > 0) {
        --m->recursion;
        return 0;
    }
    m->ownerPid = 0;
    m->ownerTid.store(0, std::memory_order_release);
    if (m->waiters.load(std::memory_order_acquire) != 0) android_shm::futexWake(&m->ownerTid, 1);
    return 0;
}

const std::string &InterProcessMutex::name() const { return impl_->name; }
bool InterProcessMutex::isCreator() const { return impl_->creator; }
void *InterProcessMutex::mappedObject() const { return impl_->obj; }
int InterProcessMutex::nativeHandle() const { return impl_->shm ? impl_->shm->nativeHandle() : -1; }

} // namespace platform
} // namespace sdrplay
