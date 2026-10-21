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

/* OpenRSP bridge firmware: the boot sequence and the idle loop. */
#include "fw_config.h"
#include "regs.h"
#include "bridge.h"
#include "eeprom.h"
#include "desc.h"
#include "usb.h"

void fw_delay(void);   /* delay.asm: bridge settling delay */

/* Vector 0x1B (timer 1): never enabled (ET1 stays 0); it holds RETI so a stray interrupt is
 * harmless (fwcheck "vectors"). */
void isr_unused_t1(void) __interrupt(3) __naked
{
    __asm
        reti
    __endasm;
}

void main(void)
{
    /* crt0 left P0 = 0xFF: the stream enable is high until it is cleared below */
    bridge_write(BRW(BR_REG_CTRL, BR_CTRL_BOOT));
    fw_delay();                                 /* settle before the next bridge access */
    P0 = 0xFF;
    P2 = 0xFF;                                  /* PDATA page no longer used */
    STREAM_EN = 0;
    MUSB_INDEX = 0;
    ee_addr16 = (BR_STRAPS & STRAP_EE16) != 0;  /* sampled once: the strap changes later */

#if FW_RSP1_STRAP3_PROBE
    {   /* GPIO boot value with the wake-strap probe */
        __bit wake_lo = (BR_STRAPS & STRAP_WAKE) == 0;
        if (wake_lo) {
            bridge_write(BRW(BR_REG_GPIO, FW_REG8_WAKE_PROBE));
            fw_delay();
        }
        bridge_write(BRW(BR_REG_GPIO, FW_REG8_BOOT));   /* gpio_shadow starts with this value */
        if (wake_lo) {
            fw_delay();
            if (BR_STRAPS & STRAP_WAKE)
                remote_wakeup_en = 1;
        }
        if (remote_wakeup_en)
            desc_cfg_hs[DESC_CFG_ATTRIBUTES] = CFG_ATTR_REMOTE_WAKEUP;
    }
#else
    bridge_write(BRW(BR_REG_GPIO, FW_REG8_BOOT));       /* gpio_shadow starts with this value */
#endif

    boot_record();
    bridge_write(BRW(BR_REG_SPIMODE, SPI_OFF));

    /* INT1 (USB): edge-triggered, stale edge dropped, low priority */
    IT1 = 1;
    IE1 = 0;
    PX1 = 0;
    EX1 = 1;
    PT1 = 0;
    EA = 1;
    MUSB_INTRUSBE = INTRUSB_SUSPEND | INTRUSB_RESUME | INTRUSB_RESET;
    MUSB_POWER = POWER_CONNECT;                 /* D+ pull-up on: the device appears on the bus */

    for (;;)                                    /* all work is done in the ISRs */
        PCON = 1;                               /* idle until the next interrupt */
}
