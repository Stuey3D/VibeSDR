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

// Windows InterProcessMutex: native named mutex.
//
// Service: CreateMutexA(&nullDaclSa, FALSE, "Global\sdrSrvComMtx"). Client: OpenMutexA (open
// only, as for the semaphores). Process-local mutexes are unnamed CreateMutexA(NULL, FALSE, NULL).
//
// Win32 mutexes are recursive and thread-owned, like the Linux PTHREAD_MUTEX_RECURSIVE.
// lock(): WAIT_ABANDONED (the owner died) leaves the caller owning the mutex; lock() RELEASES
// it and retries within the remaining budget (the Windows twin of EOWNERDEAD -> consistent +
// unlock).
#include "platform/InterProcessMutex.h"
#include "platform/windows/WinCommon.h"

namespace sdrplay {
namespace platform {

struct InterProcessMutex::Impl {
    HANDLE h = nullptr;
    std::string name;
    bool creator = false;
};

InterProcessMutex::InterProcessMutex(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

static std::unique_ptr<InterProcessMutex::Impl> makeImpl(HANDLE h, const std::string &name, bool creator)
{
    auto impl = std::make_unique<InterProcessMutex::Impl>();
    impl->h = h;
    impl->name = name;
    impl->creator = creator;
    return impl;
}

std::unique_ptr<InterProcessMutex> InterProcessMutex::create(const std::string &name, int *osError)
{
    const std::string os = win::objectName(name);
    HANDLE h = CreateMutexA(win::nullDaclAttributes(), FALSE, os.c_str());
    if (!h) {
        if (osError) *osError = static_cast<int>(GetLastError());
        return nullptr;
    }
    // ERROR_ALREADY_EXISTS: a client kept the old mutex alive; it is unowned or abandoned, both
    // of which lock() handles, so it is used as is.
    return std::unique_ptr<InterProcessMutex>(new InterProcessMutex(makeImpl(h, name, true)));
}

std::unique_ptr<InterProcessMutex> InterProcessMutex::open(const std::string &name, int *osError)
{
    const std::string os = win::objectName(name);
    HANDLE h = OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, os.c_str());
    if (!h) {
        if (osError) *osError = static_cast<int>(GetLastError());
        return nullptr;
    }
    return std::unique_ptr<InterProcessMutex>(new InterProcessMutex(makeImpl(h, name, false)));
}

std::unique_ptr<InterProcessMutex> InterProcessMutex::createLocal()
{
    HANDLE h = CreateMutexA(nullptr, FALSE, nullptr);
    if (!h) return nullptr;
    return std::unique_ptr<InterProcessMutex>(new InterProcessMutex(makeImpl(h, std::string(), true)));
}

InterProcessMutex::~InterProcessMutex()
{
    if (impl_ && impl_->h) CloseHandle(impl_->h);
}

InterProcessMutex::LockResult InterProcessMutex::lock(std::uint32_t timeoutMs)
{
    const ULONGLONG start = GetTickCount64();
    DWORD budget = timeoutMs;
    for (;;) {
        const DWORD r = WaitForSingleObject(impl_->h, budget);
        if (r == WAIT_OBJECT_0) return Ok;
        if (r == WAIT_TIMEOUT) return Timeout;
        if (r != WAIT_ABANDONED) return Error;
        // Owner died: we own it now; release and retry with what is left.
        ReleaseMutex(impl_->h);
        if (timeoutMs == INFINITE) continue;
        const ULONGLONG used = GetTickCount64() - start;
        budget = used >= timeoutMs ? 0 : static_cast<DWORD>(timeoutMs - used);
    }
}

int InterProcessMutex::unlock() { return ReleaseMutex(impl_->h) ? 0 : 1; }

const std::string &InterProcessMutex::name() const { return impl_->name; }
bool InterProcessMutex::isCreator() const { return impl_->creator; }
void *InterProcessMutex::mappedObject() const { return impl_->h; }
int InterProcessMutex::nativeHandle() const { return win::handleToInt(impl_->h); }

} // namespace platform
} // namespace sdrplay
