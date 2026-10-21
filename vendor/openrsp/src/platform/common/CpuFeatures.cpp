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

// platform/Cpu.h: the OS side of the CPU feature probes (here, so that src/core includes no
// OS headers).
#include "platform/Cpu.h"

#if defined(__arm__) && defined(__linux__) // includes Android
#include <sys/auxv.h>
#ifndef HWCAP_NEON
#include <asm/hwcap.h>
#endif
#endif

namespace sdrplay {
namespace platform {

bool cpuHasArmNeon()
{
#if defined(__aarch64__) || defined(_M_ARM64)
    return true; // Advanced SIMD is mandatory on AArch64
#elif defined(__arm__) && defined(__linux__)
    static const bool ok = (getauxval(AT_HWCAP) & HWCAP_NEON) != 0;
    return ok;
#elif defined(__ARM_NEON)
    return true;
#else
    return false;
#endif
}

} // namespace platform
} // namespace sdrplay
