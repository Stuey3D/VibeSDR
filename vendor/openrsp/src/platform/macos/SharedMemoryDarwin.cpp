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

// macOS SharedMemory: POSIX shm_open.
//
// Create pattern: shm_unlink(name); umask(0); shm_open(O_RDWR|O_CREAT, 0666);
// umask(old); ftruncate; mmap. Darwin specifics:
//  - no /dev/shm: objects are kernel objects that live until shm_unlink and
//    the last close/munmap (or reboot);
//  - ftruncate is legal ONCE per object (on the create path only);
//  - sizes are rounded up to the page size (16 KiB on Apple Silicon);
//  - names are limited to 31 characters (DarwinShmName.h);
//  - fchmod does not apply to shm descriptors, so the 0666 mode comes from the
//    umask(0) window. umask is process-global: the window is kept as
//    short as possible and serialised against our own creates.
#include "platform/SharedMemory.h"
#include "DarwinShmName.h"

#include <cerrno>
#include <fcntl.h>
#include <mutex>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sdrplay {
namespace platform {

using darwin_shm::kernelShmName;

struct SharedMemory::Impl {
    std::string name;       // as requested (what callers / logs see)
    std::string kernelName; // what the kernel sees (== name except long unit-test names)
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
    if (impl_->creator && impl_->unlinkOnDestroy) shm_unlink(impl_->kernelName.c_str());
}

namespace {
std::unique_ptr<SharedMemory::Impl> mapFd(int fd, const std::string &name, const std::string &kname,
                                          std::size_t size, int *osError)
{
    void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        if (osError) *osError = errno;
        close(fd);
        return nullptr;
    }
    auto impl = std::make_unique<SharedMemory::Impl>();
    impl->name = name;
    impl->kernelName = kname;
    impl->map = p;
    impl->size = size;
    impl->fd = fd;
    return impl;
}

std::mutex &umaskMutex()
{
    static std::mutex m;
    return m;
}
} // namespace

std::unique_ptr<SharedMemory> SharedMemory::create(const std::string &name, std::size_t size, int *osError)
{
    if (size == 0 || name.empty()) {
        if (osError) *osError = EINVAL;
        return nullptr;
    }
    const std::string kname = kernelShmName(name);
    shm_unlink(kname.c_str()); // always discard a stale object
    int fd;
    {
        std::lock_guard<std::mutex> lk(umaskMutex());
        const mode_t old = umask(0); // mode 0666 independent of the umask
        fd = shm_open(kname.c_str(), O_RDWR | O_CREAT, 0666);
        const int e = errno;
        umask(old);
        errno = e;
    }
    if (fd < 0) {
        if (osError) *osError = errno;
        return nullptr;
    }
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) { // once only on Darwin
        if (osError) *osError = errno;
        close(fd);
        shm_unlink(kname.c_str());
        return nullptr;
    }
    auto impl = mapFd(fd, name, kname, size, osError);
    if (!impl) {
        shm_unlink(kname.c_str());
        return nullptr;
    }
    impl->creator = true;
    impl->unlinkOnDestroy = true;
    return std::unique_ptr<SharedMemory>(new SharedMemory(std::move(impl)));
}

std::unique_ptr<SharedMemory> SharedMemory::open(const std::string &name, std::size_t size, int *osError)
{
    if (size == 0 || name.empty()) {
        if (osError) *osError = EINVAL;
        return nullptr;
    }
    const std::string kname = kernelShmName(name);
    int fd = shm_open(kname.c_str(), O_RDWR, 0666);
    if (fd < 0) {
        if (osError) *osError = errno;
        return nullptr;
    }
    // Refuse to map beyond the object. Darwin reports the page-rounded size,
    // so only requests beyond the last page are caught (within the page the
    // mapping is valid memory, no SIGBUS).
    struct stat st {};
    if (fstat(fd, &st) != 0 || static_cast<std::size_t>(st.st_size) < size) {
        if (osError) *osError = EINVAL;
        close(fd);
        return nullptr;
    }
    auto impl = mapFd(fd, name, kname, size, osError);
    if (!impl) return nullptr;
    return std::unique_ptr<SharedMemory>(new SharedMemory(std::move(impl)));
}

bool SharedMemory::unlink(const std::string &name) { return shm_unlink(kernelShmName(name).c_str()) == 0; }

bool SharedMemory::exists(const std::string &name)
{
    int fd = shm_open(kernelShmName(name).c_str(), O_RDONLY, 0);
    if (fd < 0) return errno == EACCES; // exists but not ours to read
    close(fd);
    return true;
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
