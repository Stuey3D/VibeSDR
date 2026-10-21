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

// Platform interface: named counting semaphore shared between processes.
//
// This is NOT an OS semaphore: on Linux the IPC "semaphores" are a
// shared-memory object {pshared robust mutex; pshared condvar; count; max}.
// That layout is a Linux-platform detail and lives in src/platform/linux/. The SEMANTICS below are portable and binding:
//
//   release(n, &prev): if count + n > max -> return 1 WITHOUT signalling;
//                      else prev = count; count += n (one signal per unit); return 0
//   wait(timeoutMs):   count >= 1 -> count--, return Ok(0)
//                      timeoutMs == 0 -> poll, return Timeout(2)
//                      otherwise wait in 10 ms slices until count >= 1 or the
//                      budget is used up -> Timeout(2); OS error -> Error(1)
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace sdrplay {
namespace platform {

class InterProcessSemaphore {
public:
    // Return codes of a semaphore wait.
    enum WaitResult : int { Ok = 0, Error = 1, Timeout = 2 };

    // Service side: (re)create the named object (stale object discarded) with
    // the given initial count and maximum.
    static std::unique_ptr<InterProcessSemaphore> create(const std::string &name, std::int64_t initial,
                                                         std::int64_t maximum, int *osError = nullptr);
    // Client side: open an existing object; init/max are those set by the creator.
    static std::unique_ptr<InterProcessSemaphore> open(const std::string &name, int *osError = nullptr);
    // Process-local (unnamed, not shared) semaphore with the same semantics.
    static std::unique_ptr<InterProcessSemaphore> createLocal(std::int64_t initial, std::int64_t maximum);

    // Creator: destroys the object in place and unlinks the name.
    // Opener: just unmaps.
    ~InterProcessSemaphore();

    InterProcessSemaphore(const InterProcessSemaphore &) = delete;
    InterProcessSemaphore &operator=(const InterProcessSemaphore &) = delete;

    WaitResult wait(std::uint32_t timeoutMs);
    // Returns 0 on success, 1 if count + n would exceed max (nothing signalled) or on error.
    int release(std::int64_t n, std::int64_t *previous = nullptr);

    // ---- peer-death robustness ----
    // The Linux objects put a glibc pshared condvar in shared memory, and glibc
    // condvars (< 2.41) are not robust: a peer killed while it waits leaves a
    // group reference behind, after which pthread_cond_signal on that object
    // blocks forever (and with it every later waiter that times out). Once
    // enabled (process-wide, idempotent), a guard thread watches every named-
    // object operation of THIS process; one that is stuck for `stallMs` gets the
    // condvar's leaked group references dropped (and, after 3 x stallMs, a
    // dead peer's internal condvar lock released), so it completes. Nothing
    // changes in the object layout or in the protocol, and a healthy run never
    // triggers it (every legitimate wait is a 10 ms slice). The service enables
    // it; the client library does not. No-op where not needed or not possible
    // (Windows kernel semaphores are robust; Darwin, Android: in-process or
    // kernel-backed objects; non-glibc Linux).
    static constexpr std::uint32_t kDefaultStallMs = 300;
    static void enableStallRecovery(std::uint32_t stallMs = kDefaultStallMs);
    static void disableStallRecovery(); // tests: stops and joins the guard thread
    // Number of repairs done so far in this process (diagnostics, tests).
    static std::uint64_t stallRecoveries();

    // Unlocked snapshots for diagnostics and tests.
    std::int64_t count() const;
    std::int64_t maximum() const;

    const std::string &name() const;
    bool isCreator() const;
    // Base address of the mapped shared object.
    void *mappedObject() const;
    int nativeHandle() const;

    struct Impl;

private:
    explicit InterProcessSemaphore(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace platform
} // namespace sdrplay
