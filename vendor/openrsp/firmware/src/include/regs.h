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

/* Peripheral addresses. */
#ifndef REGS_H
#define REGS_H

#include <8051.h>
#include <stdint.h>

#define XREG(a) (*(volatile __xdata uint8_t *)(a))

/* ---- MUSB device core, X:4000 */
#define MUSB_FADDR      XREG(0x4000)
#define MUSB_POWER      XREG(0x4001)
#define MUSB_INTRTX     XREG(0x4002)   /* read-to-clear, INTRTX_* */
#define MUSB_INTRTXE_L  XREG(0x4006)
#define MUSB_INTRTXE_H  XREG(0x4007)
#define MUSB_INTRRXE_L  XREG(0x4008)
#define MUSB_INTRRXE_H  XREG(0x4009)
#define MUSB_INTRUSB    XREG(0x400A)   /* read-to-clear, INTRUSB_* */
#define MUSB_INTRUSBE   XREG(0x400B)
#define MUSB_INDEX      XREG(0x400E)
#define MUSB_TESTMODE   XREG(0x400F)
#define MUSB_TXMAXP_L   XREG(0x4010)   /* EP1, Index 1 */
#define MUSB_TXMAXP_H   XREG(0x4011)
#define MUSB_CSR0       XREG(0x4012)   /* Index 0: CSR0; Index 1: TxCSR lo */
#define MUSB_TXCSR_L    XREG(0x4012)
#define MUSB_TXCSR_H    XREG(0x4013)
#define MUSB_COUNT0     XREG(0x4018)
#define MUSB_VPLEN      XREG(0x407B)

/* EP0 FIFO: four byte lanes at offsets 0x20-0x23 of the MUSB page; byte i of a packet goes
 * through lane i & 3. The lane address is the page with an 8-bit offset in the low byte. */
#define MUSB_PAGE       0x4000
#define MUSB_FIFO0_OFS  0x20
#define FIFO0_LANE(i)   XREG(MUSB_PAGE | (uint8_t)(MUSB_FIFO0_OFS | ((i) & 3)))

#define INTRTX_EP0       0x01

#define INTRUSB_SUSPEND  0x01
#define INTRUSB_RESUME   0x02
#define INTRUSB_RESET    0x04

#define POWER_CONNECT    0x60   /* SoftConn | HSEnab */
#define POWER_SUSPENDM   0x61
#define POWER_DISCONNECT 0x20
#define POWER_HSMODE     0x10

#define CSR0_RXPKTRDY     0x01
#define CSR0_TXPKTRDY     0x02
#define CSR0_SENTSTALL    0x04
#define CSR0_DATAEND      0x08
#define CSR0_SETUPEND     0x10
#define CSR0_SENDSTALL    0x20
#define CSR0_SVCRXPKTRDY  0x40
#define CSR0_SVCSETUPEND  0x80

/* EP0 CSR0 patterns */
#define CSR0_ACK   (CSR0_SVCRXPKTRDY | CSR0_DATAEND)
#define CSR0_STALL (CSR0_SVCRXPKTRDY | CSR0_SENDSTALL)
#define CSR0_LAST  (CSR0_TXPKTRDY | CSR0_DATAEND)

/* EP1 TxCSR lo */
#define TXCSR_TXPKTRDY    0x01
#define TXCSR_SENDSTALL   0x10
#define TXCSR_CLRDATATOG  0x40   /* also clears SendStall, written as a whole byte */

/* EP1 TxCSR hi */
#define TXCSRH_DMAREQENAB 0x10
#define TXCSRH_ISO        0x40

/* EP1 TxMaxP hi: multiplier field m (bits 15:11 of TxMaxP) and bits 10:8 of the max packet size */
#define TXMAXP_H(maxp, m) ((uint8_t)(((m) << 3) | ((maxp) >> 8)))

/* TestMode register values */
#define TESTMODE_SE0_NAK  0x01
#define TESTMODE_J        0x02
#define TESTMODE_K        0x04
#define TESTMODE_PACKET   0x08

/* ---- bridge register window, X:C000 */
#define BR_BASE    0xC000
#define BR_PORT0   XREG(0xC000)
#define BR_PORT1   XREG(0xC001)
#define BR_PORT2   XREG(0xC002)
#define BR_PORT3   XREG(0xC003)
#define BR_STATUS  XREG(0xC014)   /* BRST_* */
#define BR_STRAPS  XREG(0xC018)   /* STRAP_* */
#define BR_SPIRX   XREG(0xC01C)   /* byte clocked in during the last byte of an SPI frame */

#define BRST_TUNER_IDLE 0x01      /* tuner SPI (reg 9) idle */
#define BRST_SPI_READY  0x10      /* SPI engine ready */
#define BRST_XFER_DONE  0x20      /* transfer done */

#define STRAP_EE16  0x04          /* EEPROM uses 16-bit addressing */
#define STRAP_WAKE  0x08          /* rsp1 wake strap */

/* ---- packet buffers: four 1 KiB buffers from X:E000 */
#define PKTBUF_BASE  0xE000
#define PKTBUF_SIZE  0x400
#define PKTBUF_LAST(n) XREG(PKTBUF_BASE + (n) * PKTBUF_SIZE + PKTBUF_SIZE - 1)

/* ---- port 0 lines */
#define PKT_GO       P0_0   /* packetiser "go / next load" strobe, pulsed low then high */
#define STREAM_EN    P0_1   /* 1 = packetiser runs */
#define PKTBUF_GATE  P0_2   /* held low while the firmware writes the packet buffers */

/* ---- non-standard SFRs */
__sfr __at(0xC4) BOOTSEL;
__sfr __at(0xC5) SOFTRST;

#endif
