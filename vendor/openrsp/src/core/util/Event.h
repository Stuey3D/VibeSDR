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

// Portable manual/auto-reset event (Windows-style event semantics; also used
// as the service stop event). Pure C++17.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace sdrplay {
namespace util {

class Event {
public:
    explicit Event(bool manualReset = true) : manual_(manualReset) {}
    void signal()
    {
        std::lock_guard<std::mutex> lk(m_);
        set_ = true;
        cv_.notify_all();
    }
    void reset()
    {
        std::lock_guard<std::mutex> lk(m_);
        set_ = false;
    }
    bool isSet() const
    {
        std::lock_guard<std::mutex> lk(m_);
        return set_;
    }
    // true if signalled within timeoutMs (auto-reset events consume the signal).
    bool wait(std::uint32_t timeoutMs)
    {
        std::unique_lock<std::mutex> lk(m_);
        bool ok = cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [this] { return set_; });
        if (ok && !manual_) set_ = false;
        return ok;
    }

private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool set_ = false;
    bool manual_;
};

} // namespace util
} // namespace sdrplay
