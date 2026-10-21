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

// Android IPC object layouts and the process-wide name registry.
//
// Android has no daemon. The service core runs inside an app
// process: either the client's own process (embedded mode, EmbeddedService.cpp) or the
// OpenRSP app (service mode), and the client reaches it through the
// same named objects as on Linux. Bionic has no shm_open and no /dev/shm, so a "named shared
// memory object" here is an ASharedMemory (ashmem/memfd) region kept in a process-wide
// registry {name -> fd}. Semantics follow the Linux shm objects (create discards a stale
// object, open never creates, unlink drops the name while mappings stay valid). Because
// the registry holds file descriptors, a fork()ed child (tests) inherits it and maps the
// SAME memory. In service mode a client process has no registry of its own: a
// RemoteResolver (set by ServiceClient.cpp after the Binder bootstrap) turns a name into
// an fd received from the OpenRSP process.
//
// The byte layouts are OURS (there is no existing Android layout to match). Layout
// version 2 (kAndroidLayoutVersion) replaces the bionic pthread objects of version 1 with
// raw futex words, so that nothing a dead process held stays locked:
//   mutex v2     {u32 magic "AMX2"; atomic u32 state; atomic u32 ownerTid; u32 ownerPid;
//                 u32 recursion; atomic u32 waiters}                              24 bytes
//   semaphore v2 {u32 magic "ASM2"; atomic u32 state; atomic i64 count; i64 max;
//                 atomic u32 seq; atomic u32 waiters}                             32 bytes
// The futexes are SHARED (no FUTEX_PRIVATE_FLAG): the kernel keys them on the backing
// shmem page, which every process mapping the object sees.
// A destroyed named object is marked kStateDestroyed (and its waiters are woken); every
// operation on it then returns Error(1), the same client-visible result as the Linux
// EINVAL path (a heartbeat response error).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace sdrplay {
namespace platform {
namespace android_shm {

constexpr std::uint32_t kAndroidLayoutVersion = 2;
constexpr std::uint32_t kMutexMagic = 0x32584d41u; // "AMX2"
constexpr std::uint32_t kSemMagic = 0x324d5341u;   // "ASM2"
constexpr std::uint32_t kStateAlive = 1;
constexpr std::uint32_t kStateDestroyed = 2;

struct MutexObject {
    std::uint32_t magic;
    std::atomic<std::uint32_t> state;
    std::atomic<std::uint32_t> ownerTid; // 0 = free; the futex word
    std::uint32_t ownerPid;              // process of ownerTid (written by the owner after the CAS)
    std::uint32_t recursion;             // extra lock() calls by the owner (recursive mutex)
    std::atomic<std::uint32_t> waiters;  // > 0: unlock must FUTEX_WAKE
};

struct SemaphoreObject {
    std::uint32_t magic;
    std::atomic<std::uint32_t> state;
    std::atomic<std::int64_t> count;
    std::int64_t max;
    std::atomic<std::uint32_t> seq;     // bumped by every release/destroy; the futex word
    std::atomic<std::uint32_t> waiters; // > 0: release must FUTEX_WAKE
};

static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "futex words must be lock-free");
static_assert(std::atomic<std::int64_t>::is_always_lock_free, "the semaphore count must be lock-free");
static_assert(sizeof(MutexObject) == 24 && offsetof(MutexObject, ownerTid) == 8, "android mutex v2 layout");
static_assert(sizeof(SemaphoreObject) == 32 && offsetof(SemaphoreObject, count) == 8 &&
                  offsetof(SemaphoreObject, seq) == 24,
              "android semaphore v2 layout");

// Registry operations (SharedMemoryAndroid.cpp). All take the registry lock. They always
// act on THIS process's registry, also while a RemoteResolver is set (the OpenRSP service
// side uses them to serve its objects to clients).
// Returns a NEW fd (dup) for an existing object, -1 with *err set otherwise.
int registryOpen(const std::string &name, std::size_t minSize, int *err);
// Discards any stale entry, creates a zero-filled region of `size` bytes, registers it
// and returns a NEW fd (dup) for the caller; -1 with *err on failure.
int registryCreate(const std::string &name, std::size_t size, int *err);
bool registryUnlink(const std::string &name);
bool registryExists(const std::string &name);
std::size_t registryCount();
// Creation generation of a registered object (a process-wide counter, starting at 1,
// incremented by every registryCreate), 0 if there is no such object.
std::uint64_t registryGeneration(const std::string &name);
// Highest generation among the registered objects whose name starts with `prefix`, 0 if none.
std::uint64_t registryMaxGeneration(const std::string &prefix);

// Dead-owner recovery (the equivalent of the Linux robust mutex's EOWNERDEAD path):
// every registered mutex object owned by process `pid` is RELEASED and its
// waiters are woken. The OpenRSP service calls it when Binder reports a client dead.
// Returns the number of mutexes released.
int recoverDeadOwner(int pid);

// Service mode, client side. While a resolver is set,
// SharedMemory::open() asks it for the fd of a name, SharedMemory::exists() asks it whether
// the name exists, and SharedMemory::create() fails with EACCES (clients never create
// objects). With no resolver (embedded mode, the OpenRSP process itself) nothing changes.
struct RemoteResolver {
    void *ctx;
    // A NEW fd for the object (the caller owns it), or -1 with *err (ENOENT, EPERM, EINVAL, EPIPE).
    int (*open)(void *ctx, const std::string &name, std::size_t minSize, int *err);
    bool (*exists)(void *ctx, const std::string &name);
};
// nullptr clears it. The resolver must stay valid until it is cleared.
void setRemoteResolver(const RemoteResolver *resolver);
bool remoteResolverSet();

} // namespace android_shm
} // namespace platform
} // namespace sdrplay
