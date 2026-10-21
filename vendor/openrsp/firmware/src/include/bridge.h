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

/* Bridge register writes and the reg 3 / reg 8 shadows. */
#ifndef BRIDGE_H
#define BRIDGE_H

#include <stdint.h>

/* A bridge word is (data24 << 8) | regbyte; it goes out as C000..C003 = bytes 0..3. Bits 4:0
 * of the regbyte are the register, bits 7:5 a sync field passed through unchanged. The host sends
 * the same word with vendor request 0x41, as the Linux kernel's msi2500 driver does
 * (drivers/media/usb/msi2500/msi2500.c: register address in bits 7:0); the words show on the USB
 * bus in those requests. */
#define BRW(reg, data) ((((uint32_t)(data)) << 8) | (uint8_t)(reg))
#define BR_REG_MASK    0x1F

/* Bridge registers the firmware writes or looks at. Registers 3 (the ADC's fractional-N
 * synthesiser), 7 (packet format) and 9 (tuner SPI) are described in the Linux msi2500 driver,
 * which also writes registers 5 and 8. */
#define BR_REG_ADCPLL  0x03      /* ADC PLL; shadowed, bits 1:0 cleared while suspended */
#define BR_REG_CTRL    0x05      /* boot value BR_CTRL_BOOT; relayed writes get BR_CTRL_FORCE */
#define BR_REG_PKTFMT  0x07      /* packet format */
#define BR_REG_GPIO    0x08      /* GPIO outputs; shadowed */
#define BR_REG_TUNER   0x09      /* tuner SPI word */
#define BR_REG_SPIMODE 0x0B      /* SPI engine mode, SPI_* */
#define BR_REG_SPITX34 0x0C      /* SPI frame bytes 3-4: data[15:8], data[7:0] */
#define BR_REG_SPITX12 0x0D      /* SPI frame bytes 1-2: data[15:8], data[7:0]; starts the frame */

/* reg 3 data bits 1:0, cleared while suspended */
#define ADCPLL_RUN     0x03

#define BR_CTRL_FORCE  0x08      /* data bit 3 */
#define BR_CTRL_BOOT   0x00000C  /* reg 5 value written at boot: data bits 3:2 set */

/* reg 8 GPIO bits in data[15:8] (C002) */
#define GPIO_HI_BIT10  0x04      /* remote-wakeup line; RSPduo: second front-end path */
#define GPIO_HI_BIT11  0x08      /* front-end path select, active low */
#define GPIO_HI_BIT15  0x80
#define GPIO_HI(bits)  ((bits) * 0x100UL)       /* data[15:8] bits as a reg 8 data word */

/* reg 7, data[7:0]: bits 5:4 = 2 selects the 384-sample format (the Linux msi2500 driver's
 * 384 format writes reg 7 data 0x00A5) */
#define PKTFMT_384_MASK 0x30
#define PKTFMT_384      0x20

/* Packet trailer marker: the 384-sample payload ends at byte 999, and byte 1023 of each packet
 * buffer carries this value. The host does not read it; it shows in the stream on the USB bus. */
#define PKTBUF_TRAILER  0xFA

/* reg 9, data[7:0] bits 3:0: the tuner register the word is for (4-bit address and 20-bit value,
 * as in the Linux kernel's msi001 tuner driver, drivers/media/tuners/msi001.c) */
#define TUNER_REG_MASK  0x0F

/* SPI engine modes: off, or a frame of n = 1..4 bytes */
#define SPI_OFF        0
#define SPI_FRAME(n)   (3 + (n))
#define SPI_ON         SPI_FRAME(1)

/* Reg 3 (ADC PLL) data bits above ADCPLL_RUN held from boot until the host first writes reg 3.
 * The firmware does not interpret these fields; the host programs its own PLL setting. Unlike
 * the host's reg 3 words, this value never crosses USB. */
#define ADCPLL_DEFAULT_CFG 0x018904UL

/* The reg 3 value held in the shadow from boot until the host first writes reg 3 */
#define FW_REG3_BOOT   (ADCPLL_DEFAULT_CFG | ADCPLL_RUN)

/* rsp1 wake-strap probe: FW_REG8_BOOT with GPIO bit 10 also high, driven briefly first. The
 * pulse shows on the pin; the STRAP_WAKE read-back after it is inferred. */
#define FW_REG8_WAKE_PROBE (FW_REG8_BOOT | GPIO_HI(GPIO_HI_BIT10))

/* Both unguarded: the caller holds EA = 0 where the write must not be interrupted. Both write
 * all four bytes, in the order C000, C001, C002, C003. */
void bridge_write(uint32_t word);              /* a constant or computed word */
void bridge_write_d(__data uint8_t *w);        /* 4 bytes in IRAM: regbyte, data[7:0..23:16] */

/* The shadows as the 4 bytes that go to the port (regbyte without the sync field first). IRAM,
 * because the suspend/resume rewrites run with EA = 0 and must stay short. */
extern __data uint8_t adcpll_shadow[4];   /* initial BR_REG_ADCPLL + FW_REG3_BOOT */
extern __data uint8_t gpio_shadow[4];     /* initial BR_REG_GPIO + FW_REG8_BOOT */

/* Scratch word for a modified shadow (front-end requests). INT1 context only. */
extern __data uint8_t brtmp[4];

#endif
