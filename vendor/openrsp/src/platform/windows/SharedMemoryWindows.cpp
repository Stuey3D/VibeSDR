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

// Windows SharedMemory: pagefile-backed named sections.
//
// Service: CreateFileMappingA(INVALID_HANDLE_VALUE, &nullDaclSa, PAGE_READWRITE, 0, size,
// "Global\...") + MapViewOfFile(FILE_MAP_ALL_ACCESS, size). Client:
// OpenFileMappingA(FILE_MAP_ALL_ACCESS) + MapViewOfFile(size). ANSI names.
//
// Lifetime differs from POSIX: there is no unlink; a section lives until its last handle and
// view are gone. create() of a name that still exists (a client kept a handle across a service
// restart) therefore gets the OLD section; we zero it so the creator always starts from a fresh,
// zero-filled object like on Linux. unlink() cannot remove anything (see below).
#include "platform/SharedMemory.h"
#include "platform/windows/WinCommon.h"

#include <cstring>

namespace sdrplay {
namespace platform {

struct SharedMemory::Impl {
    std::string name;
    HANDLE section = nullptr;
    void *map = nullptr;
    std::size_t size = 0;
    bool creator = false;
    bool unmapOnDestroy = true;
};

SharedMemory::SharedMemory(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

SharedMemory::~SharedMemory()
{
    if (!impl_) return;
    if (impl_->map && impl_->unmapOnDestroy) UnmapViewOfFile(impl_->map);
    if (impl_->section) CloseHandle(impl_->section);
}

static std::unique_ptr<SharedMemory::Impl> mapSection(HANDLE section, const std::string &name, std::size_t size,
                                                      int *osError)
{
    // Refuse to map beyond the section (access violation later otherwise). The exact section
    // size comes from NtQuerySection; MapViewOfFile itself also rejects views past the end.
    std::uint64_t have = 0;
    if (win::querySectionSize(section, &have) && have < size) {
        if (osError) *osError = ERROR_INVALID_PARAMETER;
        CloseHandle(section);
        return nullptr;
    }
    void *p = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!p) {
        if (osError) *osError = static_cast<int>(GetLastError());
        CloseHandle(section);
        return nullptr;
    }
    auto impl = std::make_unique<SharedMemory::Impl>();
    impl->name = name;
    impl->section = section;
    impl->map = p;
    impl->size = size;
    return impl;
}

std::unique_ptr<SharedMemory> SharedMemory::create(const std::string &name, std::size_t size, int *osError)
{
    if (size == 0) {
        if (osError) *osError = ERROR_INVALID_PARAMETER;
        return nullptr;
    }
    const std::string os = win::objectName(name);
    const std::uint64_t sz = size;
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, win::nullDaclAttributes(), PAGE_READWRITE,
                                  static_cast<DWORD>(sz >> 32), static_cast<DWORD>(sz & 0xffffffffu), os.c_str());
    if (!h) {
        if (osError) *osError = static_cast<int>(GetLastError());
        return nullptr;
    }
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    auto impl = mapSection(h, name, size, osError);
    if (!impl) return nullptr;
    if (existed) std::memset(impl->map, 0, size); // stale object re-initialised
    impl->creator = true;
    return std::unique_ptr<SharedMemory>(new SharedMemory(std::move(impl)));
}

std::unique_ptr<SharedMemory> SharedMemory::open(const std::string &name, std::size_t size, int *osError)
{
    if (size == 0) {
        if (osError) *osError = ERROR_INVALID_PARAMETER;
        return nullptr;
    }
    const std::string os = win::objectName(name);
    HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, os.c_str());
    if (!h) {
        if (osError) *osError = static_cast<int>(GetLastError());
        return nullptr;
    }
    auto impl = mapSection(h, name, size, osError);
    if (!impl) return nullptr;
    return std::unique_ptr<SharedMemory>(new SharedMemory(std::move(impl)));
}

// Windows has no unlink: the name disappears with the last handle. Returns whether an object of
// that name exists (i.e. what the POSIX call would have removed); nothing is removed.
bool SharedMemory::unlink(const std::string &name) { return exists(name); }

// Any named kernel object of that name counts, as on POSIX where the semaphores and the mutex are
// shm objects too: sections, semaphores and mutexes share one object namespace, and opening a
// name that belongs to another object type fails with ERROR_INVALID_HANDLE
// (STATUS_OBJECT_TYPE_MISMATCH).
bool SharedMemory::exists(const std::string &name)
{
    const std::string os = win::objectName(name);
    HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, os.c_str());
    if (h) {
        CloseHandle(h);
        return true;
    }
    return GetLastError() == ERROR_INVALID_HANDLE;
}

void *SharedMemory::data() const { return impl_->map; }
std::size_t SharedMemory::size() const { return impl_->size; }
const std::string &SharedMemory::name() const { return impl_->name; }
bool SharedMemory::isCreator() const { return impl_->creator; }
void SharedMemory::setUnlinkOnDestroy(bool) {} // nothing to unlink on Windows
void SharedMemory::setUnmapOnDestroy(bool on) { impl_->unmapOnDestroy = on; }
int SharedMemory::nativeHandle() const { return win::handleToInt(impl_->section); }

} // namespace platform
} // namespace sdrplay
