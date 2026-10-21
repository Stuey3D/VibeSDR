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

// rsp_info - list attached SDRplay RSPs (rsp_info --help for the options).
#include "RspInfo.h"
#include "UsbScan.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {
void usage(FILE *f)
{
    std::fprintf(f,
                 "usage: rsp_info [--json] [-v]\n"
                 "  Lists every attached SDRplay RSP: model, hwVer, VID:PID, bus/port, bcdDevice,\n"
                 "  state (ROM loader / running), serial and whether another program or driver\n"
                 "  holds it. Read-only: never claims an interface, never writes to the device,\n"
                 "  never uploads firmware; safe while a service streams.\n"
                 "  --json            machine-readable output\n"
                 "  -v, --verbose     more detail (in-use detail, serial source, firmware image)\n"
                 "exit status: 0 device(s) listed, 1 none found, 2 error\n");
}
} // namespace

int main(int argc, char **argv)
{
    bool json = false, verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--json") json = true;
        else if (a == "-v" || a == "--verbose") verbose = true;
        else if (a == "-h" || a == "--help") {
            usage(stdout);
            return 0;
        } else {
            std::fprintf(stderr, "rsp_info: unknown option '%s'\n", a.c_str());
            usage(stderr);
            return 2;
        }
    }

    std::vector<rspinfo::RawDevice> devs;
    std::string err;
    if (!rspinfo::scanDevices(devs, &err)) {
        std::fprintf(stderr, "rsp_info: %s\n", err.c_str());
        return 2;
    }
    const std::string out = json ? rspinfo::formatJson(devs, verbose) : rspinfo::formatText(devs, verbose);
    std::fputs(out.c_str(), stdout);
    return devs.empty() ? 1 : 0;
}
