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

// Compiling STUB of the platform layer for OSes not implemented yet (Windows,
// macOS, Android). Every IPC primitive fails cleanly; nothing here is used on
// Linux except the `sdrplay_platform_stub_check` build target that proves this
// file still compiles against the interfaces.
#include "platform/InterProcessMutex.h"
#include "platform/InterProcessSemaphore.h"
#include "platform/LogSink.h"
#include "platform/ServiceHost.h"
#include "platform/SharedMemory.h"
#include "platform/Thread.h"

#include <cstdio>

namespace sdrplay {
namespace platform {

// ---------------------------------------------------------------- SharedMemory
struct SharedMemory::Impl {
    std::string name;
};
SharedMemory::SharedMemory(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SharedMemory::~SharedMemory() = default;
std::unique_ptr<SharedMemory> SharedMemory::create(const std::string &, std::size_t, int *osError)
{
    if (osError) *osError = -1;
    return nullptr;
}
std::unique_ptr<SharedMemory> SharedMemory::open(const std::string &, std::size_t, int *osError)
{
    if (osError) *osError = -1;
    return nullptr;
}
bool SharedMemory::unlink(const std::string &) { return false; }
bool SharedMemory::exists(const std::string &) { return false; }
void *SharedMemory::data() const { return nullptr; }
std::size_t SharedMemory::size() const { return 0; }
const std::string &SharedMemory::name() const { return impl_->name; }
bool SharedMemory::isCreator() const { return false; }
void SharedMemory::setUnlinkOnDestroy(bool) {}
void SharedMemory::setUnmapOnDestroy(bool) {}
int SharedMemory::nativeHandle() const { return -1; }

// ------------------------------------------------------- InterProcessSemaphore
struct InterProcessSemaphore::Impl {
    std::string name;
};
InterProcessSemaphore::InterProcessSemaphore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
InterProcessSemaphore::~InterProcessSemaphore() = default;
std::unique_ptr<InterProcessSemaphore> InterProcessSemaphore::create(const std::string &, std::int64_t,
                                                                     std::int64_t, int *osError)
{
    if (osError) *osError = -1;
    return nullptr;
}
std::unique_ptr<InterProcessSemaphore> InterProcessSemaphore::open(const std::string &, int *osError)
{
    if (osError) *osError = -1;
    return nullptr;
}
std::unique_ptr<InterProcessSemaphore> InterProcessSemaphore::createLocal(std::int64_t, std::int64_t)
{
    return nullptr;
}
InterProcessSemaphore::WaitResult InterProcessSemaphore::wait(std::uint32_t) { return Error; }
int InterProcessSemaphore::release(std::int64_t, std::int64_t *) { return 1; }
std::int64_t InterProcessSemaphore::count() const { return 0; }
std::int64_t InterProcessSemaphore::maximum() const { return 0; }
const std::string &InterProcessSemaphore::name() const { return impl_->name; }
bool InterProcessSemaphore::isCreator() const { return false; }
void *InterProcessSemaphore::mappedObject() const { return nullptr; }
int InterProcessSemaphore::nativeHandle() const { return -1; }

// ----------------------------------------------------------- InterProcessMutex
struct InterProcessMutex::Impl {
    std::string name;
};
InterProcessMutex::InterProcessMutex(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
InterProcessMutex::~InterProcessMutex() = default;
std::unique_ptr<InterProcessMutex> InterProcessMutex::create(const std::string &, int *osError)
{
    if (osError) *osError = -1;
    return nullptr;
}
std::unique_ptr<InterProcessMutex> InterProcessMutex::open(const std::string &, int *osError)
{
    if (osError) *osError = -1;
    return nullptr;
}
std::unique_ptr<InterProcessMutex> InterProcessMutex::createLocal() { return nullptr; }
InterProcessMutex::LockResult InterProcessMutex::lock(std::uint32_t) { return Error; }
int InterProcessMutex::unlock() { return 1; }
const std::string &InterProcessMutex::name() const { return impl_->name; }
bool InterProcessMutex::isCreator() const { return false; }
void *InterProcessMutex::mappedObject() const { return nullptr; }
int InterProcessMutex::nativeHandle() const { return -1; }

// ----------------------------------------------------------------- misc
void setCurrentThreadNameOs(const char *) {}
std::int32_t currentThreadId() { return 0; }
std::int32_t currentProcessId() { return 0; }
bool setCurrentThreadPriority(ThreadPriority) { return false; }

namespace {
class StderrSink final : public LogSink {
public:
    void write(LogPriority prio, const std::string &line) override
    {
        std::fprintf(stderr, "<%d>%s\n", static_cast<int>(prio), line.c_str());
    }
};
class NeverStop final : public StopSignal {
public:
    bool stopRequested() const override { return false; }
};
} // namespace
std::unique_ptr<LogSink> createSystemLogSink() { return std::make_unique<StderrSink>(); }
// TODO(windows: GetModuleFileNameW; macos: _NSGetExecutablePath)
std::string executableDirectory() { return std::string(); }

int runServiceHost(const std::function<int(const StopSignal &)> &body)
{
    NeverStop s;
    return body(s);
}

} // namespace platform
} // namespace sdrplay
