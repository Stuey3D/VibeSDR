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

/* INT1 = MUSB interrupt. All USB work runs here. */
#include "fw_config.h"
#include "regs.h"
#include "bridge.h"
#include "usb.h"

__bit usb_fullspeed;          /* high speed from boot until the first bus reset says otherwise */
static __bit resume_pending;

static void usb_resume(void)
{
    /* flag first, then the two shadows unchanged with EA = 0, then reconnect */
    resume_pending = 0;
    EA = 0;
    bridge_write_d(adcpll_shadow);
    bridge_write_d(gpio_shadow);
    EA = 1;
    MUSB_POWER = POWER_CONNECT;
}

static void usb_suspend(void)
{
#if FW_SUSPEND_WAKE_GPIO
    /* reg 8 = gpio_shadow with GPIO bit 10 = remote_wakeup_en */
    uint8_t b2 = gpio_shadow[2] & ~GPIO_HI_BIT10;
    if (remote_wakeup_en)
        b2 |= GPIO_HI_BIT10;
    EA = 0;
    BR_PORT0 = gpio_shadow[0];
    BR_PORT1 = gpio_shadow[1];
    BR_PORT2 = b2;
    BR_PORT3 = gpio_shadow[3];
#else
    EA = 0;
#endif
    /* reg 3 = adcpll_shadow with ADCPLL_RUN cleared */
    BR_PORT0 = adcpll_shadow[0];
    BR_PORT1 = adcpll_shadow[1] & ~ADCPLL_RUN;
    BR_PORT2 = adcpll_shadow[2];
    BR_PORT3 = adcpll_shadow[3];
    EA = 1;
    resume_pending = 1;
    MUSB_POWER = POWER_SUSPENDM;
}

void isr_usb(void) __interrupt(2)
{
    uint8_t iu = MUSB_INTRUSB;                  /* both read-to-clear, read once */
    uint8_t it = MUSB_INTRTX;

    if ((iu & INTRUSB_RESUME) || resume_pending)
        usb_resume();

    if (iu & INTRUSB_RESET) {                   /* bus reset: nothing else is reset */
        MUSB_INTRTXE_L = INTRTX_EP0;
        MUSB_INTRTXE_H = 0x00;
        MUSB_INTRRXE_L = 0x00;
        MUSB_INTRRXE_H = 0x00;
        MUSB_VPLEN = 0x00;
        usb_fullspeed = !(MUSB_POWER & POWER_HSMODE);
    }

    if ((it & INTRTX_EP0) && ep0_service())
        return;                                 /* reboot / test mode: straight out */

    if (iu & INTRUSB_SUSPEND)                   /* applied once per ISR call */
        usb_suspend();

    /* INT1 is edge-triggered: serve EP0 events that arrived meanwhile */
    while (MUSB_INTRTX & INTRTX_EP0)
        if (ep0_service())
            return;
}
