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

// Linux InterProcessSemaphore: the shared-memory semaphore object of the IPC
// (layout in PthreadShmObjects.h).
#include "platform/InterProcessSemaphore.h"
#include "platform/SharedMemory.h"
#include "PthreadShmObjects.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

// ThreadSanitizer cannot see that two mappings of the same named object are
// one semaphore (the pthread objects live at different addresses in each
// mapping), so in-process tests that pair a service and a client over the
// real objects would report false races. Under TSan only, each named object
// also gets a process-wide token keyed by its name for release/acquire
// annotations. No effect on normal builds.
#if defined(__SANITIZE_THREAD__)
#define SDRPLAY_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define SDRPLAY_TSAN 1
#endif
#endif
#if defined(SDRPLAY_TSAN)
#include <map>
#include <mutex>
extern "C" void __tsan_acquire(void *addr);
extern "C" void __tsan_release(void *addr);
namespace {
void *tsanToken(const std::string &name)
{
    static std::mutex m;
    static std::map<std::string, char> tokens; // never erased: stable addresses
    std::lock_guard<std::mutex> lk(m);
    return &tokens[name];
}
} // namespace
#define SDRPLAY_TSAN_RELEASE(impl) __tsan_release(tsanToken((impl)->name))
#define SDRPLAY_TSAN_ACQUIRE(impl) __tsan_acquire(tsanToken((impl)->name))
#else
#define SDRPLAY_TSAN_RELEASE(impl) ((void)0)
#define SDRPLAY_TSAN_ACQUIRE(impl) ((void)0)
#endif

namespace sdrplay {
namespace platform {

using linux_shm::SemaphoreObject;

// ------------------------------------------------------------------ stall recovery
//
// glibc's pshared condvar (< 2.41) is not robust against the death of
// a waiter. A waiter blocked in pthread_cond_(timed)wait holds a reference on its group
// (__g_refs[g]); when it is SIGKILLed that reference is never dropped. The next
// pthread_cond_signal that has to close the dead waiter's group
// (__condvar_quiesce_and_switch_g1) then waits for __g_refs[g1] to reach 0 FOREVER, with
// the condvar's internal lock and (in release()) the object mutex held; every later
// waiter that times out blocks on that internal lock too. Reproduced on glibc 2.39 with a
// raw object of this layout: it takes 2-3 kill/signal rounds.
//
// The layout and protocol are fixed (other peers share these objects), so the
// repair works on the condvar words in place. Every operation on a NAMED object that may
// block in glibc (mutex lock, cond signal, one 10 ms timed-wait slice) registers itself
// while it runs. A guard thread looks at the registered operations every stallMs/4; one
// running for >= stallMs (a healthy one takes microseconds, or one 10 ms slice) gets:
//   1. the references of the group being closed (g1 = (wseq & 1) ^ 1) dropped to 0 and a
//      futex wake on them: the stuck quiesce loop re-reads 0 and completes. A live waiter
//      of that group would have dropped its reference within its 10 ms slice (glibc
//      decrements before taking the internal lock on timeout), so the remaining ones are
//      dead. If one was not (a stopped process), its later decrement underflows the word
//      and the next quiesce of that group stalls and is repaired the same way.
//   2. if no reference is left and the operation is still stuck after 3 x stallMs, the
//      condvar's internal lock (__g1_orig_size bits 0-1) is released and its waiters woken:
//      a peer died while holding it (e.g. killed inside its own stuck signal).
// The words are those of glibc's struct __pthread_cond_s (bits/thread-shared-types.h),
// identical on every glibc architecture since 2.25. Signals that the dead waiters' group
// absorbs are not lost for live waiters: they re-check the count after every 10 ms slice.
namespace {

#if defined(__GLIBC__)
constexpr bool kCondRepairSupported = true;
static_assert(sizeof(pthread_cond_t) == 48, "glibc >= 2.25 pthread_cond_t");
#else
constexpr bool kCondRepairSupported = false;
#endif
// 32-bit word indices into pthread_cond_t (glibc struct __pthread_cond_s).
constexpr int kCondWseqLo = 0;     // __wseq low word: bit 0 = index of G2
constexpr int kCondGRefs = 4;      // __g_refs[2]: (refs << 1) | wake-request
constexpr int kCondG1OrigSize = 8; // __g1_orig_size: bits 0-1 = internal lock

using StallClock = std::chrono::steady_clock;

struct PendingOp {
    linux_shm::SemaphoreObject *obj = nullptr;
    StallClock::time_point t0;
    StallClock::time_point lastRepair;
    PendingOp *prev = nullptr, *next = nullptr;
};

// Constant-initialised (no guard variable): the client library never enables the repair and
// must not create function-local statics lazily from its threads (fork safety, see
// src/client/ServiceConnection.cpp "g_eagerStatics"), so the registry below is only
// touched once this flag has been set by enableStallRecovery().
std::atomic<bool> g_stallEnabled{false};
std::atomic<bool> g_stallEverEnabled{false};

struct StallRegistry {
    std::mutex m; // guards the list, the thread state; held during a repair
    PendingOp head;
    std::uint32_t stallMs = InterProcessSemaphore::kDefaultStallMs;
    bool stop = false;
    std::condition_variable cv;
    std::thread guard;
    std::atomic<std::uint64_t> repairs{0};
    StallRegistry() { head.prev = head.next = &head; }
};

StallRegistry &stallRegistry()
{
    static auto *r = new StallRegistry; // never destroyed (ops may run during static destruction)
    return *r;
}

void futexWakeAll(unsigned *word)
{
    // pshared condvar: shared (non-private) futexes.
    syscall(SYS_futex, word, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
}

// Called with the registry mutex held: the object stays mapped (its operation is
// registered and deregistering needs the same mutex).
bool repairCondvar(linux_shm::SemaphoreObject *o, StallClock::duration age, std::uint32_t stallMs)
{
    if (!kCondRepairSupported) return false;
    auto *w = reinterpret_cast<unsigned *>(&o->cond);
    const unsigned g1 = (__atomic_load_n(&w[kCondWseqLo], __ATOMIC_ACQUIRE) & 1u) ^ 1u;
    unsigned *refs = &w[kCondGRefs + g1];
    if ((__atomic_load_n(refs, __ATOMIC_ACQUIRE) >> 1) != 0) {
        __atomic_store_n(refs, 0u, __ATOMIC_RELEASE);
        futexWakeAll(refs);
        return true;
    }
    if (age >= std::chrono::milliseconds(3u * stallMs) && (__atomic_load_n(&w[kCondG1OrigSize], __ATOMIC_ACQUIRE) & 3u)) {
        __atomic_fetch_and(&w[kCondG1OrigSize], ~3u, __ATOMIC_RELEASE);
        futexWakeAll(&w[kCondG1OrigSize]);
        return true;
    }
    return false;
}

void guardThreadBody()
{
    StallRegistry &r = stallRegistry();
#if defined(__GLIBC__)
    pthread_setname_np(pthread_self(), "ipcStallGuard");
#endif
    std::unique_lock<std::mutex> lk(r.m);
    while (!r.stop) {
        const auto stall = std::chrono::milliseconds(r.stallMs);
        r.cv.wait_for(lk, std::chrono::milliseconds(r.stallMs / 4 > 10 ? r.stallMs / 4 : 10));
        if (r.stop) break;
        const auto now = StallClock::now();
        for (PendingOp *op = r.head.next; op != &r.head; op = op->next) {
            if (now - op->t0 < stall || now - op->lastRepair < stall) continue;
            op->lastRepair = now;
            if (repairCondvar(op->obj, now - op->t0, r.stallMs)) r.repairs.fetch_add(1);
        }
    }
}

// Registers one possibly-blocking glibc call on a named object for its duration.
class StallWatch {
public:
    StallWatch(linux_shm::SemaphoreObject *o, bool named)
    {
        if (!named || !g_stallEnabled.load(std::memory_order_acquire)) return;
        StallRegistry &r = stallRegistry();
        op_.obj = o;
        op_.t0 = op_.lastRepair = StallClock::now();
        std::lock_guard<std::mutex> lk(r.m);
        op_.next = r.head.next;
        op_.prev = &r.head;
        r.head.next->prev = &op_;
        r.head.next = &op_;
        linked_ = true;
    }
    ~StallWatch()
    {
        if (!linked_) return;
        std::lock_guard<std::mutex> lk(stallRegistry().m);
        op_.prev->next = op_.next;
        op_.next->prev = op_.prev;
    }
    StallWatch(const StallWatch &) = delete;
    StallWatch &operator=(const StallWatch &) = delete;

private:
    PendingOp op_;
    bool linked_ = false;
};

} // namespace

void InterProcessSemaphore::enableStallRecovery(std::uint32_t stallMs)
{
    StallRegistry &r = stallRegistry();
    std::lock_guard<std::mutex> lk(r.m);
    r.stallMs = stallMs < 20 ? 20 : stallMs;
    if (!r.guard.joinable()) {
        r.stop = false;
        r.guard = std::thread(guardThreadBody);
    }
    g_stallEverEnabled = true;
    g_stallEnabled = true;
}

void InterProcessSemaphore::disableStallRecovery()
{
    if (!g_stallEverEnabled.load()) return;
    StallRegistry &r = stallRegistry();
    std::thread t;
    {
        std::lock_guard<std::mutex> lk(r.m);
        g_stallEnabled = false;
        r.stop = true;
        r.cv.notify_all();
        t = std::move(r.guard);
    }
    if (t.joinable()) t.join();
}

std::uint64_t InterProcessSemaphore::stallRecoveries()
{
    return g_stallEverEnabled.load() ? stallRegistry().repairs.load() : 0;
}

// ------------------------------------------------------------------ objects
struct InterProcessSemaphore::Impl {
    std::unique_ptr<SharedMemory> shm;          // named objects
    std::unique_ptr<SemaphoreObject> local;     // createLocal()
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
    if (pshared) {
        ok = ok && pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED) == 0;
        ok = ok && pthread_mutexattr_setrobust(&ma, PTHREAD_MUTEX_ROBUST) == 0;
    }
    ok = ok && pthread_mutex_init(&o->mutex, &ma) == 0;
    pthread_mutexattr_destroy(&ma);
    if (!ok) return false;
    if (pthread_condattr_init(&ca) != 0) return false;
    if (pshared) ok = pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED) == 0;
    ok = ok && pthread_cond_init(&o->cond, &ca) == 0;
    pthread_condattr_destroy(&ca);
    if (!ok) {
        pthread_mutex_destroy(&o->mutex);
        return false;
    }
    o->count = static_cast<linux_shm::NativeLong>(initial);
    o->max = static_cast<linux_shm::NativeLong>(maximum);
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
    // The condvar is zeroed first: references left by waiters that died would otherwise make
    // pthread_cond_destroy wait for them forever. Destroying the mutex makes clients still
    // mapping the unlinked object get EINVAL (a heartbeat response error); the SharedMemory destructor
    // then unmaps and unlinks it.
    std::memset(static_cast<void *>(&impl_->obj->cond), 0, sizeof(pthread_cond_t));
    pthread_cond_destroy(&impl_->obj->cond);
    pthread_mutex_destroy(&impl_->obj->mutex);
}

// Locks the object mutex, recovering a mutex whose owner died while holding it.
// (A live owner stuck in a wedged pthread_cond_signal is freed by the stall repair.)
static int lockObject(SemaphoreObject *o, bool named)
{
    // An uncontended lock cannot stall, so the fast path skips the StallWatch: no clock read
    // (a syscall on hosts whose clocksource is not vDSO-capable, e.g. HPET) and no registry lock.
    int rc = pthread_mutex_trylock(&o->mutex);
    if (rc == EBUSY) {
        StallWatch watch(o, named);
        rc = pthread_mutex_lock(&o->mutex);
    }
    if (rc == EOWNERDEAD) {
        pthread_mutex_consistent(&o->mutex);
        rc = 0;
    }
    return rc;
}

InterProcessSemaphore::WaitResult InterProcessSemaphore::wait(std::uint32_t timeoutMs)
{
    SemaphoreObject *o = impl_->obj;
    const bool named = impl_->shm != nullptr;
    if (lockObject(o, named) != 0) return Error;
    if (o->count >= 1) {
        --o->count;
        pthread_mutex_unlock(&o->mutex);
        SDRPLAY_TSAN_ACQUIRE(impl_);
        return Ok;
    }
    if (timeoutMs == 0) {
        pthread_mutex_unlock(&o->mutex);
        return Timeout;
    }
    // 10 ms slices against CLOCK_REALTIME deadlines: the first is
    // gettimeofday()+10 ms and each ETIMEDOUT advances it by exactly 10 ms and
    // costs 10 ms of budget. The total is therefore ceil(timeoutMs/10)*10 ms
    // with no drift; the 1000 ms stream-thread wait phase is visible in the
    // ReleaseDevice duration.
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
        int rc;
        {
            StallWatch watch(o, named);
            rc = pthread_cond_timedwait(&o->cond, &o->mutex, &ts);
        }
        if (rc == EOWNERDEAD) {
            pthread_mutex_consistent(&o->mutex);
            rc = 0;
        }
        // We re-check the count after every wake-up (also after ETIMEDOUT) so a
        // unit released exactly at a slice boundary is not lost.
        if ((rc == 0 || rc == ETIMEDOUT) && o->count >= 1) {
            --o->count;
            pthread_mutex_unlock(&o->mutex);
            SDRPLAY_TSAN_ACQUIRE(impl_);
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
    const bool named = impl_->shm != nullptr;
    if (n < 0 || lockObject(o, named) != 0) return 1;
    if (o->count + n > o->max) { // fail WITHOUT signalling
        pthread_mutex_unlock(&o->mutex);
        return 1;
    }
    if (previous) *previous = o->count;
    SDRPLAY_TSAN_RELEASE(impl_);
    for (std::int64_t i = 0; i < n; ++i) {
        ++o->count;
        StallWatch watch(o, named);
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
