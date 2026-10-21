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

// Linux SharedMemory: POSIX shm_open under /dev/shm.
//
// Create: discard any stale object, create /dev/shm/<name> with mode 0666
// whatever the umask, ftruncate, mmap. The mode is set with fchmod instead of a
// process-global umask(0) around shm_open, which is racy in a multi-threaded process.
// Names contain a literal backslash ("Glbl\sdrSrv...") and no leading '/';
// glibc maps them to /dev/shm/<name> unchanged.
#include "platform/SharedMemory.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sdrplay {
namespace platform {

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
    if (impl_->creator && impl_->unlinkOnDestroy) shm_unlink(impl_->name.c_str());
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
    // Always discard a stale object. If it exists and cannot be removed (owned
    // by another user in the sticky /dev/shm, i.e. a live service running as root), fail instead of
    // opening it and re-initialising another service's mutexes and semaphores in place. The
    // installed (root) service never takes this path.
    if (shm_unlink(name.c_str()) != 0 && (errno == EPERM || errno == EACCES)) {
        if (osError) *osError = errno;
        return nullptr;
    }
    int fd = shm_open(name.c_str(), O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        if (osError) *osError = errno;
        return nullptr;
    }
    (void)fchmod(fd, 0666); // mode 0666 regardless of umask
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        if (osError) *osError = errno;
        close(fd);
        shm_unlink(name.c_str());
        return nullptr;
    }
    auto impl = mapFd(fd, name, size, osError);
    if (!impl) {
        shm_unlink(name.c_str());
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
    int fd = shm_open(name.c_str(), O_RDWR, 0666);
    if (fd < 0) {
        if (osError) *osError = errno;
        return nullptr;
    }
    // Refuse to map beyond the object (SIGBUS on access otherwise).
    struct stat st {};
    if (fstat(fd, &st) != 0 || static_cast<std::size_t>(st.st_size) < size) {
        if (osError) *osError = EINVAL;
        close(fd);
        return nullptr;
    }
    auto impl = mapFd(fd, name, size, osError);
    if (!impl) return nullptr;
    return std::unique_ptr<SharedMemory>(new SharedMemory(std::move(impl)));
}

bool SharedMemory::unlink(const std::string &name) { return shm_unlink(name.c_str()) == 0; }

bool SharedMemory::exists(const std::string &name)
{
    int fd = shm_open(name.c_str(), O_RDONLY, 0);
    if (fd < 0) return false;
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
