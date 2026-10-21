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

// Platform interface: named shared memory ("file mapping object").
//
// Portable header: no OS includes. Implementations live in src/platform/<os>/.
// Linux: POSIX shm under /dev/shm, mode 0666, the creator discards any stale
// object first.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace sdrplay {
namespace platform {

class SharedMemory {
public:
    // Service-side create: unlink any stale object of that name,
    // create it fresh with mode 0666 (independent of the process umask), size
    // it and map it read/write. A fresh object is zero-filled.
    // Returns nullptr on failure; *osError (if given) receives errno/GetLastError.
    static std::unique_ptr<SharedMemory> create(const std::string &name, std::size_t size,
                                                int *osError = nullptr);

    // Client-side open of an existing object (never creates). Maps `size` bytes.
    static std::unique_ptr<SharedMemory> open(const std::string &name, std::size_t size,
                                              int *osError = nullptr);

    // Removes the name (existing mappings stay valid). Returns false if it did not exist.
    static bool unlink(const std::string &name);

    // True if an object of that name currently exists.
    static bool exists(const std::string &name);

    // Unmaps and closes. If this object was created by create() and
    // setUnlinkOnDestroy(true) (the default for creators), the name is unlinked too.
    ~SharedMemory();

    SharedMemory(const SharedMemory &) = delete;
    SharedMemory &operator=(const SharedMemory &) = delete;

    void *data() const;
    std::size_t size() const;
    const std::string &name() const;
    bool isCreator() const;
    void setUnlinkOnDestroy(bool on);
    // false: the destructor closes the OS handle but keeps the mapping for the
    // rest of the process lifetime. The client never unmaps its maps:
    // applications may keep using DeviceParamsT pointers after ReleaseDevice.
    // Default true.
    void setUnmapOnDestroy(bool on);

    // OS handle (fd on POSIX, -1 where not applicable), for the exported
    // sdrplay_SharedMemoryOpen/Close handle ABI.
    int nativeHandle() const;

    struct Impl;

private:
    explicit SharedMemory(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace platform
} // namespace sdrplay
