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

// Android InterProcessSemaphore, layout v2 (AndroidShm.h): a lock-free counting
// semaphore on a shared futex, with the portable semantics of InterProcessSemaphore.h /
// the Linux objects: release fails WITHOUT signalling past max, wait(0) polls, other
// waits run in 10 ms slices and each expired slice costs 10 ms of budget (the Linux
// arithmetic). Nothing is ever locked, so a process killed inside wait() or release()
// cannot wedge the others (the glibc condvar hang of the Linux objects cannot happen
// here). A destroyed named object reports Error(1).
#include "platform/InterProcessSemaphore.h"
#include "platform/SharedMemory.h"
#include "AndroidFutex.h"
#include "AndroidShm.h"

#include <cerrno>
#include <climits>
#include <new>

namespace sdrplay {
namespace platform {

using android_shm::SemaphoreObject;

struct InterProcessSemaphore::Impl {
    std::unique_ptr<SharedMemory> shm;      // named objects
    std::unique_ptr<SemaphoreObject> local; // createLocal()
    SemaphoreObject *obj = nullptr;
    std::string name;
    bool creator = false;
};

InterProcessSemaphore::InterProcessSemaphore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

static void initObject(SemaphoreObject *o, std::int64_t initial, std::int64_t maximum)
{
    new (&o->state) std::atomic<std::uint32_t>(0);
    new (&o->count) std::atomic<std::int64_t>(initial);
    new (&o->seq) std::atomic<std::uint32_t>(0);
    new (&o->waiters) std::atomic<std::uint32_t>(0);
    o->max = maximum;
    o->magic = android_shm::kSemMagic;
    o->state.store(android_shm::kStateAlive, std::memory_order_release);
}

static bool alive(const SemaphoreObject *o)
{
    return o->magic == android_shm::kSemMagic &&
           o->state.load(std::memory_order_acquire) == android_shm::kStateAlive;
}

std::unique_ptr<InterProcessSemaphore> InterProcessSemaphore::create(const std::string &name, std::int64_t initial,
                                                                     std::int64_t maximum, int *osError)
{
    auto shm = SharedMemory::create(name, sizeof(SemaphoreObject), osError);
    if (!shm) return nullptr;
    auto *o = static_cast<SemaphoreObject *>(shm->data());
    initObject(o, initial, maximum);
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
    auto *o = static_cast<SemaphoreObject *>(shm->data());
    if (o->magic != android_shm::kSemMagic) { // a different kind of object (or a v1 layout)
        if (osError) *osError = EINVAL;
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->obj = o;
    impl->shm = std::move(shm);
    impl->name = name;
    return std::unique_ptr<InterProcessSemaphore>(new InterProcessSemaphore(std::move(impl)));
}

std::unique_ptr<InterProcessSemaphore> InterProcessSemaphore::createLocal(std::int64_t initial, std::int64_t maximum)
{
    auto impl = std::make_unique<Impl>();
    impl->local = std::make_unique<SemaphoreObject>();
    initObject(impl->local.get(), initial, maximum);
    impl->obj = impl->local.get();
    impl->creator = true;
    return std::unique_ptr<InterProcessSemaphore>(new InterProcessSemaphore(std::move(impl)));
}

InterProcessSemaphore::~InterProcessSemaphore()
{
    if (!impl_ || !impl_->obj || !impl_->creator || impl_->local) return; // opener: SharedMemory dtor unmaps
    // Named: openers must see an error, not a hang.
    SemaphoreObject *o = impl_->obj;
    o->state.store(android_shm::kStateDestroyed, std::memory_order_release);
    o->seq.fetch_add(1, std::memory_order_acq_rel);
    android_shm::futexWakeAll(&o->seq);
}

static bool tryTake(SemaphoreObject *o)
{
    std::int64_t c = o->count.load(std::memory_order_acquire);
    while (c >= 1)
        if (o->count.compare_exchange_weak(c, c - 1, std::memory_order_acq_rel, std::memory_order_acquire))
            return true;
    return false;
}

InterProcessSemaphore::WaitResult InterProcessSemaphore::wait(std::uint32_t timeoutMs)
{
    SemaphoreObject *o = impl_->obj;
    if (!alive(o)) return Error;
    if (tryTake(o)) return Ok;
    if (timeoutMs == 0) return Timeout;
    std::int64_t remaining = timeoutMs;
    for (;;) {
        // Read seq BEFORE re-checking the count: a release in between bumps seq, so the
        // futex wait below returns at once instead of missing the wake-up.
        const std::uint32_t s = o->seq.load(std::memory_order_acquire);
        if (!alive(o)) return Error;
        if (tryTake(o)) return Ok;
        o->waiters.fetch_add(1, std::memory_order_acq_rel);
        const int rc = android_shm::futexWaitMs(&o->seq, s, 10);
        o->waiters.fetch_sub(1, std::memory_order_acq_rel);
        if (!alive(o)) return Error;
        if (tryTake(o)) return Ok;
        if (rc == ETIMEDOUT) {
            remaining -= 10;
            if (remaining <= 0) return Timeout;
        }
    }
}

int InterProcessSemaphore::release(std::int64_t n, std::int64_t *previous)
{
    SemaphoreObject *o = impl_->obj;
    if (n < 0 || !alive(o)) return 1;
    std::int64_t c = o->count.load(std::memory_order_acquire);
    do {
        if (c + n > o->max) return 1; // fail WITHOUT signalling
    } while (!o->count.compare_exchange_weak(c, c + n, std::memory_order_acq_rel, std::memory_order_acquire));
    if (previous) *previous = c;
    o->seq.fetch_add(1, std::memory_order_acq_rel);
    if (n > 0 && o->waiters.load(std::memory_order_acquire) != 0)
        android_shm::futexWake(&o->seq, n > INT_MAX ? INT_MAX : static_cast<int>(n));
    return 0;
}

std::int64_t InterProcessSemaphore::count() const { return impl_->obj->count.load(std::memory_order_acquire); }
std::int64_t InterProcessSemaphore::maximum() const { return impl_->obj->max; }
const std::string &InterProcessSemaphore::name() const { return impl_->name; }
bool InterProcessSemaphore::isCreator() const { return impl_->creator; }
void *InterProcessSemaphore::mappedObject() const { return impl_->obj; }
int InterProcessSemaphore::nativeHandle() const { return impl_->shm ? impl_->shm->nativeHandle() : -1; }

} // namespace platform
} // namespace sdrplay
