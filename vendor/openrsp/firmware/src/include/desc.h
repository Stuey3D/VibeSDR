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

/* USB descriptors. */
#ifndef DESC_H
#define DESC_H

#include <stdint.h>

#define DESC_DEVICE_LEN 18
#define DESC_CFG_HS_LEN 66
#define DESC_CFG_FS_LEN 18
#define DESC_QUAL_LEN   10
#define DESC_STR0_LEN   4
#define DESC_STR1_LEN   22
#define TEST_PACKET_LEN 53

/* Field offsets */
#define DESC_BLENGTH          0     /* any descriptor */
#define DESC_BTYPE            1     /* any descriptor */
#define DESC_CFG_TOTAL_LEN    2     /* configuration: wTotalLength lo (both lengths < 256) */
#define DESC_CFG_ATTRIBUTES   7     /* configuration: bmAttributes */
#define DESC_DEV_ID           8     /* device: idVendor, idProduct (4 bytes) */
#define DESC_STR1_SERIAL      2     /* string 1: first UTF-16LE serial character */

#define CFG_ATTR_REMOTE_WAKEUP 0xA0 /* bmAttributes: bus powered, remote wakeup */

/* Patched at run time, so they live in XDATA (initialised by crt0 from XINIT). */
extern __xdata uint8_t desc_device[DESC_DEVICE_LEN];   /* idVendor/idProduct from the boot record */
extern __xdata uint8_t desc_cfg_hs[DESC_CFG_HS_LEN];   /* type 2 or 7; rsp1 wake attribute */
extern __xdata uint8_t desc_cfg_fs[DESC_CFG_FS_LEN];   /* type 2 or 7 */
extern __xdata uint8_t desc_qual[DESC_QUAL_LEN];
extern __xdata uint8_t desc_str0[DESC_STR0_LEN];
extern __xdata uint8_t desc_str1[DESC_STR1_LEN];       /* serial, low bytes from the boot record */
extern __code uint8_t test_packet[TEST_PACKET_LEN];

#endif
