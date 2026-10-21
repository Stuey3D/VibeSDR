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

// Platform interface: named, recursive, robust mutex shared between processes
// (e.g. "Glbl\sdrSrvComMtx").
//
// lock(timeoutMs): try; if the previous owner died, mark consistent and UNLOCK
// (the lock is released, not taken) then retry; while busy
// sleep 10 ms and subtract 10 ms; budget exhausted -> Timeout(2).
// Any other OS error (e.g. the service destroyed the object in place) returns
// Error(1) instead of spinning forever.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace sdrplay {
namespace platform {

class InterProcessMutex {
public:
    enum LockResult : int { Ok = 0, Error = 1, Timeout = 2 };

    static std::unique_ptr<InterProcessMutex> create(const std::string &name, int *osError = nullptr);
    static std::unique_ptr<InterProcessMutex> open(const std::string &name, int *osError = nullptr);
    // Process-local recursive mutex with the same lock(timeout) semantics.
    static std::unique_ptr<InterProcessMutex> createLocal();

    ~InterProcessMutex();
    InterProcessMutex(const InterProcessMutex &) = delete;
    InterProcessMutex &operator=(const InterProcessMutex &) = delete;

    LockResult lock(std::uint32_t timeoutMs);
    int unlock(); // 0 ok, 1 error

    const std::string &name() const;
    bool isCreator() const;
    void *mappedObject() const;
    int nativeHandle() const;

    struct Impl;

private:
    explicit InterProcessMutex(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace platform
} // namespace sdrplay
