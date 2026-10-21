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

#include "core/dsp/StageTimes.h"

#include <atomic>

namespace sdrplay {
namespace dsp {
namespace stages {

namespace {
std::atomic<std::uint32_t> g_armed{0};
std::atomic<std::uint64_t> g_ns[kCount];
std::atomic<std::uint64_t> g_blocks{0};
} // namespace

const char *key(Stage s)
{
    static const char *const k[kCount] = {"unpack", "dc", "agc", "downconvert", "decimate", "iq"};
    return s < kCount ? k[s] : "?";
}

Snapshot read()
{
    g_armed.store(kArmBlocks, std::memory_order_relaxed);
    Snapshot s;
    for (unsigned i = 0; i < kCount; ++i) s.ns[i] = g_ns[i].load(std::memory_order_relaxed);
    s.blocks = g_blocks.load(std::memory_order_relaxed);
    return s;
}

bool takeBlock()
{
    std::uint32_t a = g_armed.load(std::memory_order_relaxed);
    while (a != 0 && !g_armed.compare_exchange_weak(a, a - 1, std::memory_order_relaxed)) {
    }
    return a != 0;
}

void add(const std::uint64_t (&ns)[kCount])
{
    for (unsigned i = 0; i < kCount; ++i)
        if (ns[i]) g_ns[i].fetch_add(ns[i], std::memory_order_relaxed);
    g_blocks.fetch_add(1, std::memory_order_relaxed);
}

} // namespace stages
} // namespace dsp
} // namespace sdrplay
