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

/* EP0 state machine, CSR0 patterns and the IN data stage. */
#include "fw_config.h"
#include "regs.h"
#include "bridge.h"
#include "eeprom.h"
#include "desc.h"
#include "usb.h"

__data setup_t setup;
__data uint8_t ep0_state;
__data uint8_t ep0_src;
__xdata uint8_t * __data ep0_ptr;
__data uint16_t ep0_eeaddr;
__data uint8_t ep0_small[4];
static __data uint16_t ep0_rem;     /* IN bytes still to send */
static __bit ep0_zlp;               /* reply shorter than wLength: end with a short packet */

__bit faddr_pending;
__data uint8_t faddr_value;
__bit reboot_pending;
__bit testmode_pending;
__data uint8_t testmode_value;

void ep0_ack(void)
{
    MUSB_CSR0 = CSR0_ACK;
}

void ep0_stall(void)
{
    MUSB_CSR0 = CSR0_STALL;
}

void ep1_txcsr(uint8_t v)
{
    MUSB_INDEX = 1;
    MUSB_TXCSR_L = v;
    MUSB_INDEX = 0;
}

/* One IN packet of <= 64 bytes. The packet that completes the reply carries DataEnd; a reply
 * that is shorter than wLength and a multiple of 64 ends with a zero-length packet. */
static void ep0_in_packet(void)
{
    uint8_t n = EP0_MAXP;
    uint8_t i;

    if (ep0_rem < EP0_MAXP)
        n = (uint8_t)ep0_rem;
    ep0_rem -= n;
    if (ep0_src == EP0_SRC_EEPROM) {
        for (i = 0; i < n; i++)
            FIFO0_LANE(i) = eeprom_read_byte(ep0_eeaddr++);
        if (!ep0_rem)
            bridge_write(BRW(BR_REG_SPIMODE, SPI_OFF));      /* after the last byte */
    } else {
        __xdata uint8_t *src = ep0_ptr;
        for (i = 0; i < n; i++)
            FIFO0_LANE(i) = *src++;
        ep0_ptr = src;
    }
    if (n < EP0_MAXP || (!ep0_rem && !ep0_zlp)) {
        ep0_state = EP0_IDLE;
        MUSB_CSR0 = CSR0_LAST;
        return;
    }
    MUSB_CSR0 = CSR0_TXPKTRDY;
}

/* Start an IN data stage of min(len, wLength) bytes (a reply longer than wLength is
 * truncated). wLength 0 gives CSR0 = ServicedRxPktRdy, then CSR0_LAST. An EEPROM read turns
 * the SPI engine on after the ServicedRxPktRdy write, never before it. */
void ep0_in_start(uint16_t len)
{
    ep0_zlp = len < SU_LEN;
    ep0_rem = ep0_zlp ? len : SU_LEN;
    MUSB_CSR0 = CSR0_SVCRXPKTRDY;
    if (ep0_src == EP0_SRC_EEPROM)
        bridge_write(BRW(BR_REG_SPIMODE, SPI_ON));
    ep0_state = EP0_IN;
    ep0_in_packet();
}

/* Fixed-size reply of 1-4 bytes from ep0_small[] (GET_STATUS/CONFIGURATION/INTERFACE, RDREG,
 * FNADDR): one packet, truncated to wLength, CSR0 = ServicedRxPktRdy then CSR0_LAST. */
void ep0_in_short(uint8_t len)
{
    if (SU_LEN < len)
        len = (uint8_t)SU_LEN;
    MUSB_CSR0 = CSR0_SVCRXPKTRDY;
    if (len) {
        FIFO0_LANE(0) = ep0_small[0];
        if (len > 1) {
            FIFO0_LANE(1) = ep0_small[1];
            if (len > 2) {
                FIFO0_LANE(2) = ep0_small[2];
                if (len > 3)
                    FIFO0_LANE(3) = ep0_small[3];
            }
        }
    }
    MUSB_CSR0 = CSR0_LAST;
}

void ep0_in_xdata(__xdata uint8_t *src, uint16_t len)
{
    ep0_ptr = src;
    ep0_in_start(len);
}

/* OUT data stage (XDATA write, EEPROM write): ServicedRxPktRdy now and per packet, CSR0_ACK
 * on the packet that completes wLength. */
void ep0_out_start(uint16_t len)
{
    ep0_rem = len;
    ep0_state = EP0_OUT;
    MUSB_CSR0 = CSR0_SVCRXPKTRDY;
}

static void ep0_out_packet(void)
{
    uint8_t n = MUSB_COUNT0;
    uint8_t i;

#if FW_EEPROM_WRITE
    if (ep0_src == EP0_SRC_EEWRITE) {
        for (i = 0; i < n; i++)
            eeprom_write_byte(ep0_eeaddr++, FIFO0_LANE(i));
    } else
#endif
    {
        __xdata uint8_t *dst = ep0_ptr;
        for (i = 0; i < n; i++)
            *dst++ = FIFO0_LANE(i);
        ep0_ptr = dst;
    }
    if (ep0_rem > n) {
        ep0_rem -= n;
        MUSB_CSR0 = CSR0_SVCRXPKTRDY;
        return;
    }
#if FW_EEPROM_WRITE
    if (ep0_src == EP0_SRC_EEWRITE)
        bridge_write(BRW(BR_REG_SPIMODE, SPI_OFF));
#endif
    ep0_state = EP0_IDLE;
    MUSB_CSR0 = CSR0_ACK;
}

static void ep0_setup(void)
{
    /* FIFO0 byte lanes 0-3, twice */
    setup.b[0] = FIFO0_LANE(0);
    setup.b[1] = FIFO0_LANE(1);
    setup.b[2] = FIFO0_LANE(2);
    setup.b[3] = FIFO0_LANE(3);
    setup.b[4] = FIFO0_LANE(0);
    setup.b[5] = FIFO0_LANE(1);
    setup.b[6] = FIFO0_LANE(2);
    setup.b[7] = FIFO0_LANE(3);
    ep0_src = EP0_SRC_XDATA;
    if (SU_REQ < VREQ_FIRST)
        std_request();
    else
        vendor_request();
}

static void ep0_test_mode(void)
{
    if (testmode_value == TESTMODE_PACKET) {
        uint8_t i;
        for (i = 0; i < TEST_PACKET_LEN; i++)
            FIFO0_LANE(i) = test_packet[i];
        MUSB_CSR0 = CSR0_TXPKTRDY;
    }
    MUSB_TESTMODE = testmode_value;
}

uint8_t ep0_service(void)
{
    uint8_t csr;

    /* actions deferred until the status stage of their own request is done */
    if (faddr_pending) {
        faddr_pending = 0;
        MUSB_FADDR = faddr_value;
    }
    if (reboot_pending) {
        reboot_pending = 0;
        MUSB_POWER = POWER_DISCONNECT;
        SOFTRST = 1;
        return 1;
    }
    if (testmode_pending) {
        testmode_pending = 0;
        ep0_test_mode();
        return 1;
    }

    csr = MUSB_CSR0;
    if (csr & CSR0_SENTSTALL) {                 /* writing 0 clears SentStall */
        MUSB_CSR0 = 0x00;
        ep0_state = EP0_IDLE;
        ep0_src = EP0_SRC_XDATA;
    }
    if (csr & CSR0_SETUPEND) {
        MUSB_CSR0 = CSR0_SVCSETUPEND;
        ep0_state = EP0_IDLE;
        ep0_src = EP0_SRC_XDATA;
    }

    if (ep0_state == EP0_IN) {                  /* next IN packet once the FIFO is free */
        if (!(csr & CSR0_TXPKTRDY))
            ep0_in_packet();
    } else if (ep0_state == EP0_OUT) {
        if (csr & CSR0_RXPKTRDY)
            ep0_out_packet();
    } else if (csr & CSR0_RXPKTRDY) {           /* idle: a new SETUP packet */
        ep0_setup();
    }
    return 0;
}
