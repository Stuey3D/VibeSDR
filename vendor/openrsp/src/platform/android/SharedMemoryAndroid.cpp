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

// Android SharedMemory: ASharedMemory regions in a process-wide name registry
// (AndroidShm.h). Same contract as the Linux implementation (SharedMemory.h,
// SharedMemoryLinux.cpp): create() discards a stale object and returns a zero-filled
// mapping; open() never creates and refuses sizes beyond the object; unlink() drops
// the name while existing mappings stay valid. There is no file mode on Android: in
// embedded mode the objects never leave the app's process tree, and in service mode
// the OpenRSP app hands fds only to clients the user approved.
// While a RemoteResolver is set (service-mode client), open()/exists() go to the
// OpenRSP service over Binder and create() is refused (EACCES).
#include "platform/SharedMemory.h"
#include "AndroidFutex.h"
#include "AndroidShm.h"

#include <android/sharedmem.h>
#include <cerrno>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

namespace sdrplay {
namespace platform {

namespace android_shm {
namespace {
struct Entry {
    int fd;                  // registry-owned fd
    std::uint64_t generation; // registryGeneration()
};
struct Registry {
    std::mutex m;
    std::map<std::string, Entry> fds; // name -> entry
    std::uint64_t nextGeneration = 1;
};
Registry &registry()
{
    static Registry *r = new Registry(); // never destroyed: used from static destructors too
    return *r;
}
int dupFd(int fd, int *err)
{
    int d = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (d < 0 && err) *err = errno;
    return d;
}
} // namespace

int registryOpen(const std::string &name, std::size_t minSize, int *err)
{
    Registry &r = registry();
    std::lock_guard<std::mutex> lk(r.m);
    auto it = r.fds.find(name);
    if (it == r.fds.end()) {
        if (err) *err = ENOENT;
        return -1;
    }
    if (ASharedMemory_getSize(it->second.fd) < minSize) { // never map beyond the object (SIGBUS)
        if (err) *err = EINVAL;
        return -1;
    }
    return dupFd(it->second.fd, err);
}

int registryCreate(const std::string &name, std::size_t size, int *err)
{
    // The kernel-side name is only a debugging label (/proc/<pid>/maps); the
    // IPC names contain a backslash, so a fixed label is used instead.
    int fd = ASharedMemory_create("sdrplay_ipc", size);
    if (fd < 0) {
        if (err) *err = errno != 0 ? errno : ENOMEM;
        return -1;
    }
    int mine = dupFd(fd, err);
    if (mine < 0) {
        close(fd);
        return -1;
    }
    Registry &r = registry();
    std::lock_guard<std::mutex> lk(r.m);
    auto it = r.fds.find(name);
    const Entry e{fd, r.nextGeneration++};
    if (it != r.fds.end()) { // stale object discarded; old mappings stay valid
        close(it->second.fd);
        it->second = e;
    } else {
        r.fds.emplace(name, e);
    }
    return mine;
}

bool registryUnlink(const std::string &name)
{
    Registry &r = registry();
    std::lock_guard<std::mutex> lk(r.m);
    auto it = r.fds.find(name);
    if (it == r.fds.end()) return false;
    close(it->second.fd);
    r.fds.erase(it);
    return true;
}

bool registryExists(const std::string &name)
{
    Registry &r = registry();
    std::lock_guard<std::mutex> lk(r.m);
    return r.fds.count(name) != 0;
}

std::size_t registryCount()
{
    Registry &r = registry();
    std::lock_guard<std::mutex> lk(r.m);
    return r.fds.size();
}

std::uint64_t registryGeneration(const std::string &name)
{
    Registry &r = registry();
    std::lock_guard<std::mutex> lk(r.m);
    auto it = r.fds.find(name);
    return it == r.fds.end() ? 0 : it->second.generation;
}

std::uint64_t registryMaxGeneration(const std::string &prefix)
{
    Registry &r = registry();
    std::lock_guard<std::mutex> lk(r.m);
    std::uint64_t g = 0;
    for (auto it = r.fds.lower_bound(prefix); it != r.fds.end() && it->first.compare(0, prefix.size(), prefix) == 0;
         ++it)
        if (it->second.generation > g) g = it->second.generation;
    return g;
}

int recoverDeadOwner(int pid)
{
    if (pid <= 0) return 0;
    Registry &r = registry();
    std::lock_guard<std::mutex> lk(r.m);
    int released = 0;
    for (auto &kv : r.fds) {
        const int fd = kv.second.fd;
        if (ASharedMemory_getSize(fd) != sizeof(MutexObject)) continue;
        void *p = mmap(nullptr, sizeof(MutexObject), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) continue;
        auto *m = static_cast<MutexObject *>(p);
        std::uint32_t tid = m->ownerTid.load(std::memory_order_acquire);
        // ownerPid is written right after the CAS; a holder killed in between is caught by
        // the tid check (that thread no longer exists).
        if (m->magic == kMutexMagic && tid != 0 &&
            (m->ownerPid == static_cast<std::uint32_t>(pid) || (m->ownerPid == 0 && tidIsDead(tid)))) {
            m->recursion = 0;
            m->ownerPid = 0;
            if (m->ownerTid.compare_exchange_strong(tid, 0u, std::memory_order_acq_rel)) {
                futexWakeAll(&m->ownerTid);
                ++released;
            }
        }
        munmap(p, sizeof(MutexObject));
    }
    return released;
}

namespace {
std::atomic<const RemoteResolver *> g_resolver{nullptr};
}

void setRemoteResolver(const RemoteResolver *resolver) { g_resolver.store(resolver, std::memory_order_release); }
bool remoteResolverSet() { return g_resolver.load(std::memory_order_acquire) != nullptr; }

} // namespace android_shm

struct SharedMemory::Impl {
    std::string name;
    void *map = nullptr;
    std::size_t size = 0;
    int fd = -1;
    bool creator = false;
    bool unlinkOnDestroy = false;
    bool unmapOnDestroy = true;
};

SharedMemory::SharedMemory(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

SharedMemory::~SharedMemory()
{
    if (!impl_) return;
    if (impl_->map && impl_->unmapOnDestroy) munmap(impl_->map, impl_->size);
    if (impl_->fd >= 0) close(impl_->fd);
    // Like shm_unlink(name): removes whatever object carries the name now.
    if (impl_->creator && impl_->unlinkOnDestroy) android_shm::registryUnlink(impl_->name);
}

static std::unique_ptr<SharedMemory::Impl> mapFd(int fd, const std::string &name, std::size_t size, int *osError)
{
    void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        if (osError) *osError = errno;
        close(fd);
        return nullptr;
    }
    auto impl = std::make_unique<SharedMemory::Impl>();
    impl->name = name;
    impl->map = p;
    impl->size = size;
    impl->fd = fd;
    return impl;
}

std::unique_ptr<SharedMemory> SharedMemory::create(const std::string &name, std::size_t size, int *osError)
{
    if (size == 0) {
        if (osError) *osError = EINVAL;
        return nullptr;
    }
    if (android_shm::remoteResolverSet()) { // service-mode client: never creates
        if (osError) *osError = EACCES;
        return nullptr;
    }
    int fd = android_shm::registryCreate(name, size, osError);
    if (fd < 0) return nullptr;
    auto impl = mapFd(fd, name, size, osError);
    if (!impl) {
        android_shm::registryUnlink(name);
        return nullptr;
    }
    impl->creator = true;
    impl->unlinkOnDestroy = true;
    return std::unique_ptr<SharedMemory>(new SharedMemory(std::move(impl)));
}

std::unique_ptr<SharedMemory> SharedMemory::open(const std::string &name, std::size_t size, int *osError)
{
    if (size == 0) {
        if (osError) *osError = EINVAL;
        return nullptr;
    }
    int fd;
    if (const android_shm::RemoteResolver *r = android_shm::g_resolver.load(std::memory_order_acquire)) {
        int err = 0;
        fd = r->open(r->ctx, name, size, &err);
        if (fd < 0 && osError) *osError = err != 0 ? err : ENOENT;
    } else {
        fd = android_shm::registryOpen(name, size, osError);
    }
    if (fd < 0) return nullptr;
    auto impl = mapFd(fd, name, size, osError);
    if (!impl) return nullptr;
    return std::unique_ptr<SharedMemory>(new SharedMemory(std::move(impl)));
}

bool SharedMemory::unlink(const std::string &name)
{
    if (android_shm::remoteResolverSet()) return false; // not ours to remove
    return android_shm::registryUnlink(name);
}

bool SharedMemory::exists(const std::string &name)
{
    if (const android_shm::RemoteResolver *r = android_shm::g_resolver.load(std::memory_order_acquire))
        return r->exists(r->ctx, name);
    return android_shm::registryExists(name);
}

void *SharedMemory::data() const { return impl_->map; }
std::size_t SharedMemory::size() const { return impl_->size; }
const std::string &SharedMemory::name() const { return impl_->name; }
bool SharedMemory::isCreator() const { return impl_->creator; }
void SharedMemory::setUnlinkOnDestroy(bool on) { impl_->unlinkOnDestroy = on; }
void SharedMemory::setUnmapOnDestroy(bool on) { impl_->unmapOnDestroy = on; }
int SharedMemory::nativeHandle() const { return impl_->fd; }

} // namespace platform
} // namespace sdrplay
