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

// Platform interface: CPU feature queries that need the OS (the kernel's hardware
// capability bits). Instruction-level probes (x86 cpuid) stay in the core.
#pragma once

namespace sdrplay {
namespace platform {

// True if the CPU executes Advanced SIMD (NEON). aarch64: always. 32-bit ARM Linux /
// Android: the kernel's AT_HWCAP HWCAP_NEON bit (getauxval). Other 32-bit ARM OSes:
// true when the build itself assumes NEON (__ARM_NEON). Non-ARM: false.
bool cpuHasArmNeon();

} // namespace platform
} // namespace sdrplay
