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

// Per-device request/response triples (Sem, RSem, Map). The client only opens
// the objects the service created at SelectDevice.
#include "client/ClientAbi.h"
#include "client/ServiceConnection.h"

#include <cstdio>
#include <cstring>

using namespace sdrplay;

void InitCommObject(srvComm_t *c)
{
    if (c) std::memset(static_cast<void *>(c), 0, sizeof *c);
}

// prefix "Glbl\sdrSrvDv00", channel "mCmd" etc.; mapSize 0 = no map (Hrtbt).
// Returns 0 on success, 1 on failure. Partial objects are left for the caller
// to delete.
int CreateCommObject(sdrplay_api_device *dev, char *prefix, char *channel, srvComm_t *c, int mapSize,
                     sdrplay_api_log *log)
{
    (void)log; // we log through m_serviceState.log (the same object)
    if (!prefix || !channel || !c) return 1;
    InitCommObject(c);
    char name[64];
    std::snprintf(name, sizeof name, "%s_%sSem", prefix, channel);
    if (sdrplay_SharedSemaphoreOpen(&c->sem, name, 0, 1) != kOsalOk) {
        client::apiLog(sdrplay_api_DbgLvl_Error, __func__, "Could not create %s->cSem %s", channel, name);
        if (dev) dev->setError(__FILE__, __func__, __LINE__, "Could not create %s->cSem %s", channel, name);
        return 1;
    }
    std::snprintf(name, sizeof name, "%s_%sRSem", prefix, channel);
    if (sdrplay_SharedSemaphoreOpen(&c->rsem, name, 0, 1) != kOsalOk) {
        client::apiLog(sdrplay_api_DbgLvl_Error, __func__, "Could not create %s->rSem %s", channel, name);
        if (dev) dev->setError(__FILE__, __func__, __LINE__, "Could not create %s->rSem %s", channel, name);
        return 1;
    }
    if (mapSize > 0) {
        std::snprintf(name, sizeof name, "%s_%sMap", prefix, channel);
        if (sdrplay_SharedMemoryOpen(&c->shm, name, static_cast<unsigned>(mapSize), &c->map) != kOsalOk) {
            // "open" in the log, "create" in the record (both texts are what callers see)
            client::apiLog(sdrplay_api_DbgLvl_Error, __func__, "Could not open %s->map %s", channel, name);
            if (dev) dev->setError(__FILE__, __func__, __LINE__, "Could not create %s->map %s", channel, name);
            return 1;
        }
    }
    return 0;
}

// NULL handles of objects that were never opened are skipped.
// The maps stay mapped, because applications may still hold DeviceParamsT
// pointers into them and expect them to stay readable; only the handles are
// released.
void DeleteCommObject(srvComm_t *c)
{
    if (!c) return;
    if (c->sem) sdrplay_SharedSemaphoreClose(&c->sem);
    if (c->rsem) sdrplay_SharedSemaphoreClose(&c->rsem);
    if (c->shm) sdrplay_SharedMemoryClose(&c->shm, &c->map); // keeps the mapping
    InitCommObject(c);
}
