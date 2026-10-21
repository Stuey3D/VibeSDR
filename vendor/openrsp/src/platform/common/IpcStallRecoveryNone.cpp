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

// InterProcessSemaphore stall recovery for the platform layers that do not need it:
// Windows (kernel semaphores: a dead waiter cannot wedge a ReleaseSemaphore), Darwin and
// Android (kernel-mediated / in-process objects), the stub layer. Linux: the glibc
// condvar repair in src/platform/linux/InterProcessSemaphoreLinux.cpp.
#include "platform/InterProcessSemaphore.h"

namespace sdrplay {
namespace platform {

void InterProcessSemaphore::enableStallRecovery(std::uint32_t) {}
void InterProcessSemaphore::disableStallRecovery() {}
std::uint64_t InterProcessSemaphore::stallRecoveries() { return 0; }

} // namespace platform
} // namespace sdrplay
