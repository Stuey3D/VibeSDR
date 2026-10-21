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

/* EP1 programming on SET_INTERFACE, START/STOP. The per-load work is the INT0 ISR
 * in isr_int0.asm. */
#include "fw_config.h"
#include "regs.h"
#include "stream.h"

volatile __bit stop_req;

/* TxMaxP lo, TxMaxP hi, TxCSR hi per alt setting. Alt 1's 3 x 1024 bytes is the wMaxPacketSize
 * 0x1400 listed in the Linux kernel's msi2500 driver. */
static __code uint8_t ep1_cfg[4][3] = {
    { 0x00, 0x00, 0x00 },                                        /* alt 0: no endpoint */
    { 0x00, TXMAXP_H(1024, 2), TXCSRH_ISO | TXCSRH_DMAREQENAB }, /* alt 1: iso 3 x 1024 */
    { 0x00, TXMAXP_H(1024, 0), TXCSRH_ISO | TXCSRH_DMAREQENAB }, /* alt 2: iso 1024 */
    { 0x00, TXMAXP_H(512, 7),  TXCSRH_DMAREQENAB },              /* alt 3: bulk 512 */
};

/* SET_INTERFACE disarms a live stream (or a pending STOP) before it touches EP1 at all,
 * i.e. before the EP1 un-halt and the EP1 programming below. */
void stream_disarm(void)
{
    if (EX0) {
        EX0 = 0;
        IE0 = 0;
        STREAM_EN = 0;
        stop_req = 0;
    }
}

void stream_set_alt(uint8_t alt)
{
    /* TxCSR lo is not written; EA stays on */
    MUSB_INDEX = 1;
    MUSB_TXMAXP_L = ep1_cfg[alt][0];
    MUSB_TXMAXP_H = ep1_cfg[alt][1];
    MUSB_TXCSR_H = ep1_cfg[alt][2];
    MUSB_INDEX = 0;
}

/* START at high speed: arm INT0 (edge, high priority, stale edge dropped), enable the
 * packetiser and give it the first "go". Reg 3/7, the alt setting and EP1 are not touched. */
void stream_start(void)
{
    IT0 = 1;
    IE0 = 0;
    PX0 = 1;
    EX0 = 1;
    stop_req = 0;
    STREAM_EN = 1;
    PKT_GO = 0;
    PKT_GO = 1;
}
