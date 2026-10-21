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

#include "core/ipc/Names.h"

#include <cstdio>

namespace sdrplay {
namespace ipc {

namespace {
std::string &prefixStorage()
{
    static auto *p = new std::string; // never destroyed: used from apps' static destructors via Close
    return *p;
}
} // namespace

void setObjectNamePrefix(const std::string &prefix) { prefixStorage() = prefix; }
const std::string &objectNamePrefix() { return prefixStorage(); }

std::string globalName(const char *base) { return prefixStorage() + base; }

std::string devicePrefix(unsigned slot)
{
    char buf[kDevicePrefixBufLen];
    std::snprintf(buf, sizeof buf, SDRPLAY_IPC_NS "sdrSrvDv%02u", slot % 100u);
    return prefixStorage() + buf;
}

bool fitsServiceNameLimit(const std::string &name)
{
    return name.size() <= kServiceMaxNameLen + prefixStorage().size();
}

std::string deviceObjectName(const std::string &prefix, Side side, Channel ch, Kind kind)
{
    static const char *const chan[] = {"Hrtbt", "Cmd", "Data", "Event"};
    static const char *const kinds[] = {"Sem", "RSem", "Map"};
    std::string n = prefix;
    n += '_';
    n += (side == Side::Master) ? 'm' : 's';
    n += chan[static_cast<int>(ch)];
    n += kinds[static_cast<int>(kind)];
    return n;
}

} // namespace ipc
} // namespace sdrplay
