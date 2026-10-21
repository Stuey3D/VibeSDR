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

/* Build options per variant.
 *
 * Select exactly one of FW_VARIANT_COMMON / FW_VARIANT_RSP1 / FW_VARIANT_RSPDUO (the Makefile
 * passes it). FW_DEV=1 makes a dev build: FW_BCD_DEVICE in the dev range (FW_BCD_DEV_BASE plus the
 * release's low digit) and no VREQ_EEPROM_WRITE.
 */
#ifndef FW_CONFIG_H
#define FW_CONFIG_H

/* Reg 8 is the bridge's GPIO output word. GPIO_DEFAULT_OUT is the set of outputs every variant
 * drives high at boot (data bits 7, 9, 13 and 14); the firmware never changes them itself, and
 * the GPIO_HI_* bits it does drive (bridge.h) are all low in it. The boot levels, the RSPduo's
 * below included, show on the GPIO pins at power-up (logic analyser). */
#define GPIO_DEFAULT_OUT  0x006280UL

/* reg 8 GPIO outputs at boot: GPIO_DEFAULT_OUT; the RSPduo also holds GPIO bit 11 (first
 * front-end path, inactive) and bit 15 high. Used where bridge.h is included. */
#define FW_REG8_BOOT_BASE GPIO_DEFAULT_OUT

/* bcdDevice of dev builds: FW_BCD_DEV_BASE plus the release's low digit. FW_BCD_ROM_LOADER
 * is the value the ROM loader enumerates with, which an image never uses. */
#define FW_BCD_DEV_BASE   0x03F0
#define FW_BCD_ROM_LOADER 0x0200

#define FW_FE_NONE   0   /* no front-end request (VREQ_FE, VREQ_FE_A, VREQ_FE_B unsupported) */
#define FW_FE_SINGLE 1   /* VREQ_FE: GPIO bit 11, active low */
#define FW_FE_DUAL   2   /* VREQ_FE_A: GPIO bit 11 low; VREQ_FE_B: GPIO bit 10 high */

#if defined(FW_VARIANT_COMMON) + defined(FW_VARIANT_RSP1) + defined(FW_VARIANT_RSPDUO) != 1
#error "define exactly one of FW_VARIANT_COMMON, FW_VARIANT_RSP1, FW_VARIANT_RSPDUO"
#endif

#if defined(FW_VARIANT_COMMON)
#define FW_BCD_RELEASE        0x0301
#define FW_REG8_BOOT          FW_REG8_BOOT_BASE
#define FW_FE                 FW_FE_SINGLE
#define FW_RSP1_STRAP3_PROBE  0
#elif defined(FW_VARIANT_RSP1)
#define FW_BCD_RELEASE        0x0302
#define FW_REG8_BOOT          FW_REG8_BOOT_BASE
#define FW_FE                 FW_FE_NONE
#define FW_RSP1_STRAP3_PROBE  1
#else /* FW_VARIANT_RSPDUO */
#define FW_BCD_RELEASE        0x0303
#define FW_REG8_BOOT          (FW_REG8_BOOT_BASE | GPIO_HI(GPIO_HI_BIT11 | GPIO_HI_BIT15))
#define FW_FE                 FW_FE_DUAL
#define FW_RSP1_STRAP3_PROBE  0
#endif

#ifndef FW_DEV
#define FW_DEV 0
#endif

#if FW_DEV
#define FW_BCD_DEVICE   (FW_BCD_DEV_BASE | (FW_BCD_RELEASE & 0x0F))
#define FW_EEPROM_WRITE 0
#else
#define FW_BCD_DEVICE   FW_BCD_RELEASE
#ifndef FW_EEPROM_WRITE
#define FW_EEPROM_WRITE 1
#endif
#endif

/* reg 8 bit 10 is written on suspend unless bit 10 belongs to the second front-end path. */
#define FW_SUSPEND_WAKE_GPIO (FW_FE != FW_FE_DUAL)

/* ---- combinations that make no sense */
#if FW_DEV && FW_EEPROM_WRITE
#error "dev builds never carry VREQ_EEPROM_WRITE"
#endif
#if FW_RSP1_STRAP3_PROBE && FW_FE != FW_FE_NONE
#error "the strap-3 probe exists only on the rsp1 image"
#endif
#if (FW_BCD_DEVICE & 0xFF00) != 0x0300 || FW_BCD_DEVICE == FW_BCD_ROM_LOADER
#error "bcdDevice must be 0x03xx and never FW_BCD_ROM_LOADER"
#endif

#endif
