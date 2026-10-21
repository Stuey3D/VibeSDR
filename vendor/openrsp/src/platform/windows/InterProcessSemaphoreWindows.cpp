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

// Windows InterProcessSemaphore: native named semaphores.
//
// Service: CreateSemaphoreA(&nullDaclSa, init, max, "Global\...").
// Client: OpenSemaphoreA, not create-or-open: the portable open() does not know the creator's
// init/max, and create-or-open would leave orphan objects with a wrong maximum (Data/Event need
// 8/8) when the service is absent. A correctly working application cannot tell the difference: with
// no service the client already fails at the map open, before it reaches the semaphores.
//
// Semantics vs the portable contract (InterProcessSemaphore.h):
//   release over max -> ReleaseSemaphore fails with ERROR_TOO_MANY_POSTS, nothing signalled: same.
//   wait             -> WaitForSingleObject(h, timeout); on Windows the wait is one
//                       call, not in 10 ms slices (that slicing is a POSIX emulation detail).
//   create on an existing name (a client kept a handle across a service restart): the kernel
//                       returns the old object with its old count; it is drained and refilled to
//                       `initial`. Its maximum cannot change.
//   destroy          -> CloseHandle; the object dies with its last handle (no unlink).
#include "platform/InterProcessSemaphore.h"
#include "platform/windows/WinCommon.h"

#include <climits>

#ifndef SEMAPHORE_QUERY_STATE
#define SEMAPHORE_QUERY_STATE 0x0001
#endif

namespace sdrplay {
namespace platform {

struct InterProcessSemaphore::Impl {
    HANDLE h = nullptr;
    std::string name;
    bool creator = false;
    std::int64_t localMax = 0; // fallback when NtQuerySemaphore is unavailable
};

InterProcessSemaphore::InterProcessSemaphore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

static std::unique_ptr<InterProcessSemaphore::Impl> makeImpl(HANDLE h, const std::string &name, bool creator,
                                                             std::int64_t maximum)
{
    auto impl = std::make_unique<InterProcessSemaphore::Impl>();
    impl->h = h;
    impl->name = name;
    impl->creator = creator;
    impl->localMax = maximum;
    return impl;
}

static bool validCounts(std::int64_t initial, std::int64_t maximum)
{
    return maximum >= 1 && maximum <= LONG_MAX && initial >= 0 && initial <= maximum;
}

std::unique_ptr<InterProcessSemaphore> InterProcessSemaphore::create(const std::string &name, std::int64_t initial,
                                                                     std::int64_t maximum, int *osError)
{
    if (!validCounts(initial, maximum)) {
        if (osError) *osError = ERROR_INVALID_PARAMETER;
        return nullptr;
    }
    const std::string os = win::objectName(name);
    HANDLE h = CreateSemaphoreA(win::nullDaclAttributes(), static_cast<LONG>(initial), static_cast<LONG>(maximum),
                                os.c_str());
    if (!h) {
        if (osError) *osError = static_cast<int>(GetLastError());
        return nullptr;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Stale object: drain, then refill to the initial count (bounded by its max).
        while (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
        }
        if (initial > 0) ReleaseSemaphore(h, static_cast<LONG>(initial), nullptr);
    }
    return std::unique_ptr<InterProcessSemaphore>(new InterProcessSemaphore(makeImpl(h, name, true, maximum)));
}

std::unique_ptr<InterProcessSemaphore> InterProcessSemaphore::open(const std::string &name, int *osError)
{
    const std::string os = win::objectName(name);
    HANDLE h = OpenSemaphoreA(SYNCHRONIZE | SEMAPHORE_MODIFY_STATE | SEMAPHORE_QUERY_STATE, FALSE, os.c_str());
    if (!h && GetLastError() == ERROR_ACCESS_DENIED) // a creator with a stricter DACL: no count queries
        h = OpenSemaphoreA(SYNCHRONIZE | SEMAPHORE_MODIFY_STATE, FALSE, os.c_str());
    if (!h) {
        if (osError) *osError = static_cast<int>(GetLastError());
        return nullptr;
    }
    long cur = 0, max = 0;
    if (!win::querySemaphore(h, &cur, &max)) max = 0;
    return std::unique_ptr<InterProcessSemaphore>(new InterProcessSemaphore(makeImpl(h, name, false, max)));
}

std::unique_ptr<InterProcessSemaphore> InterProcessSemaphore::createLocal(std::int64_t initial, std::int64_t maximum)
{
    // Process-local semaphore: an unnamed CreateSemaphoreA.
    if (!validCounts(initial, maximum)) return nullptr;
    HANDLE h = CreateSemaphoreA(nullptr, static_cast<LONG>(initial), static_cast<LONG>(maximum), nullptr);
    if (!h) return nullptr;
    return std::unique_ptr<InterProcessSemaphore>(new InterProcessSemaphore(makeImpl(h, std::string(), true, maximum)));
}

InterProcessSemaphore::~InterProcessSemaphore()
{
    if (impl_ && impl_->h) CloseHandle(impl_->h);
}

InterProcessSemaphore::WaitResult InterProcessSemaphore::wait(std::uint32_t timeoutMs)
{
    switch (WaitForSingleObject(impl_->h, timeoutMs)) {
    case WAIT_OBJECT_0: return Ok;
    case WAIT_TIMEOUT: return Timeout;
    default: return Error;
    }
}

int InterProcessSemaphore::release(std::int64_t n, std::int64_t *previous)
{
    if (n < 0 || n > LONG_MAX) return 1;
    if (n == 0) { // ReleaseSemaphore rejects 0; nothing to signal
        if (previous) *previous = count();
        return 0;
    }
    LONG prev = 0;
    if (!ReleaseSemaphore(impl_->h, static_cast<LONG>(n), &prev)) return 1; // ERROR_TOO_MANY_POSTS: no signal
    if (previous) *previous = prev;
    return 0;
}

std::int64_t InterProcessSemaphore::count() const
{
    long cur = 0;
    return win::querySemaphore(impl_->h, &cur, nullptr) ? cur : 0;
}

std::int64_t InterProcessSemaphore::maximum() const
{
    long max = 0;
    return win::querySemaphore(impl_->h, nullptr, &max) ? max : impl_->localMax;
}

const std::string &InterProcessSemaphore::name() const { return impl_->name; }
bool InterProcessSemaphore::isCreator() const { return impl_->creator; }
// Nothing is mapped on Windows: the object is identified by its kernel handle.
void *InterProcessSemaphore::mappedObject() const { return impl_->h; }
int InterProcessSemaphore::nativeHandle() const { return win::handleToInt(impl_->h); }

} // namespace platform
} // namespace sdrplay
