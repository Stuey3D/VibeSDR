/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 the OpenRSP authors
 *
 * This file is part of OpenRSP.
 *
 * OpenRSP is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 *
 * OpenRSP is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with OpenRSP. If
 * not, see <https://www.gnu.org/licenses/>.
 */

/* EP1 / packetiser control. */
#ifndef STREAM_H
#define STREAM_H

#include <stdint.h>

extern volatile __bit stop_req;    /* STOP pending; consumed by the INT0 ISR */

void stream_disarm(void);          /* SET_INTERFACE, before any EP1 access */
void stream_set_alt(uint8_t alt);  /* EP1 programming for alt 0-3 */
void stream_start(void);           /* high speed only; the caller checks */

#endif
