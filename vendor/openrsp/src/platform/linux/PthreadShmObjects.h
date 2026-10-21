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

// Layouts of the Linux process-shared objects of the IPC. These are a
// LINUX-PLATFORM implementation detail: the peers place raw glibc pthread
// objects in shared memory, so both must use the same glibc pthread ABI (true
// for any glibc on the same architecture).
//
//   semaphore: { mutex (pshared, robust); cond (pshared, CLOCK_REALTIME); count; max }
//   mutex:     one pthread_mutex_t (pshared, robust, recursive)
//
// count/max are C `long`. We spell that as a pointer-sized fixed-width type.
// TODO(arm64/armhf): unverified there (glibc aarch64 pthread_mutex_t is 48
// bytes, armhf 24 bytes, armhf long is 32-bit), so the semaphore object would
// be 112 bytes on arm64 and 80 on armhf.
#pragma once

#include <cstddef>
#include <cstdint>
#include <pthread.h>
#include <type_traits>

namespace sdrplay {
namespace platform {
namespace linux_shm {

using NativeLong = std::conditional<sizeof(void *) == 8, std::int64_t, std::int32_t>::type;

struct SemaphoreObject {
    pthread_mutex_t mutex; // pshared + robust (NOT recursive)
    pthread_cond_t cond;   // pshared, default clock (CLOCK_REALTIME; deadlines from gettimeofday)
    NativeLong count;
    NativeLong max;
};

using MutexObject = pthread_mutex_t; // pshared + robust + recursive

// Layout check (x86-64 glibc).
#if defined(__x86_64__) && defined(__GLIBC__)
static_assert(sizeof(pthread_mutex_t) == 40, "glibc x86-64 pthread_mutex_t");
static_assert(sizeof(pthread_cond_t) == 48, "glibc x86-64 pthread_cond_t");
static_assert(offsetof(SemaphoreObject, mutex) == 0, "Linux semaphore object layout");
static_assert(offsetof(SemaphoreObject, cond) == 40, "Linux semaphore object layout");
static_assert(offsetof(SemaphoreObject, count) == 88, "Linux semaphore object layout");
static_assert(offsetof(SemaphoreObject, max) == 96, "Linux semaphore object layout");
static_assert(sizeof(SemaphoreObject) == 104, "Linux semaphore object layout");
static_assert(sizeof(MutexObject) == 40, "Linux mutex object layout");
#endif

// glibc __kind values of these objects (for tests / diagnostics), built from glibc's
// mutex kind bits.
constexpr int kGlibcMutexKindPshared = 0x80;
constexpr int kGlibcMutexKindRobust = 0x10;
constexpr int kGlibcMutexKindRecursive = PTHREAD_MUTEX_RECURSIVE_NP;
constexpr int kExpectedSemMutexKind = kGlibcMutexKindPshared | kGlibcMutexKindRobust;
constexpr int kExpectedComMutexKind = kExpectedSemMutexKind | kGlibcMutexKindRecursive;

} // namespace linux_shm
} // namespace platform
} // namespace sdrplay
