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

// AVX-512 kernel table: deliberately not provided. No AVX-512 hardware or emulator is
// available to prove a variant bit-exact on recorded streams, and every table must be
// proven bit-exact against the scalar reference before it is used. The stream path's
// blocks are short, so 512-bit lanes would add little over AVX2. SDRPLAY_FORCE_SIMD=avx512 falls back to avx2 (Kernels.cpp).
#include "core/dsp/Kernels.h"

namespace sdrplay {
namespace dsp {
namespace simd {
const Kernels *avx512Kernels() { return nullptr; }
} // namespace simd
} // namespace dsp
} // namespace sdrplay
