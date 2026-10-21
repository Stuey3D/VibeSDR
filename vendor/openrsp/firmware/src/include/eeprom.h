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

/* SPI engine: EEPROM and front-end expander frames. */
#ifndef EEPROM_H
#define EEPROM_H

#include <stdint.h>

/* 25xx SPI EEPROM opcodes and status */
#define EE_OP_WRITE 0x02
#define EE_OP_READ  0x03
#define EE_OP_RDSR  0x05
#define EE_OP_WREN  0x06
#define EE_A8_SHIFT 3                  /* 8-bit addressing: address bit 8 in opcode bit 3 */
#define EE_SR_WIP   0x01               /* write in progress */

/* Boot record at byte 0: signature, then idVendor/idProduct, then the serial digits. The reads
 * show on the EEPROM's SPI bus (logic analyser), and the record can be read from the EEPROM
 * directly. */
#define EE_BOOT_SIG       0
#define EE_BOOT_SIG_ID    0xB4         /* identity record */
#define EE_BOOT_ID        1            /* idVendor, idProduct: bytes 1..4 */
#define EE_BOOT_ID_LAST   4
#define EE_BOOT_SERIAL    7            /* serial digits: bytes 7..16 */
#define EE_BOOT_SERIAL_LAST 16

extern __bit ee_addr16;            /* STRAP_EE16, read once at boot */

void spi_wait(void);               /* BRST_XFER_DONE, then BRST_SPI_READY */
uint8_t eeprom_read_byte(uint16_t addr);
void eeprom_write_byte(uint16_t addr, uint8_t d);   /* FW_EEPROM_WRITE builds only */
void fe_frame(uint8_t op, uint16_t regval);         /* FW_FE != NONE builds only */

void boot_record(void);            /* EEPROM boot record at byte 0 */

#endif
