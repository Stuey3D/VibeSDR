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

// Layouts of the macOS process-shared objects of the IPC.
// macOS-platform implementation detail: the peers place raw Darwin libpthread
// objects in POSIX shm, so both must use the Darwin pthread ABI (identical LP64
// layout on arm64 and x86_64: pthread_mutex_t 64 B, pthread_cond_t 48 B).
//
//   semaphore: { mutex (pshared, default type, not robust); cond (pshared); count; max }
//   mutex:     one pthread_mutex_t (pshared, recursive, not robust: no API for it)
//
// These must NOT be confused with the Linux layouts
// (src/platform/linux/PthreadShmObjects.h): mixing them is wire-incompatible.
#pragma once

#include <cstddef>
#include <cstdint>
#include <pthread.h>

namespace sdrplay {
namespace platform {
namespace darwin_shm {

struct SemaphoreObject {
    pthread_mutex_t mutex; // pshared, PTHREAD_MUTEX_DEFAULT
    pthread_cond_t cond;   // pshared; deadlines from gettimeofday (CLOCK_REALTIME)
    std::int64_t count;    // C `long` (LP64 on both slices)
    std::int64_t max;
};

using MutexObject = pthread_mutex_t; // pshared + recursive

constexpr std::size_t kSemSize = 128;
constexpr std::size_t kMutexSize = 64;

// Layout check.
static_assert(sizeof(void *) == 8, "macOS builds are LP64 only (arm64, x86_64)");
static_assert(sizeof(long) == 8, "count/max are C long in the 3.15 ABI");
static_assert(sizeof(pthread_mutex_t) == 64, "Darwin pthread_mutex_t");
static_assert(sizeof(pthread_cond_t) == 48, "Darwin pthread_cond_t");
static_assert(offsetof(SemaphoreObject, mutex) == 0, "macOS semaphore object layout");
static_assert(offsetof(SemaphoreObject, cond) == 64, "macOS semaphore object layout");
static_assert(offsetof(SemaphoreObject, count) == 112, "macOS semaphore object layout");
static_assert(offsetof(SemaphoreObject, max) == 120, "macOS semaphore object layout");
static_assert(sizeof(SemaphoreObject) == kSemSize, "macOS semaphore object layout");
static_assert(sizeof(MutexObject) == kMutexSize, "macOS mutex object layout");

// PSHMNAMLEN (sys/posix_shm_internal.h): longest shm name the kernel accepts.
constexpr std::size_t kMaxShmNameLen = 31;

} // namespace darwin_shm
} // namespace platform
} // namespace sdrplay
