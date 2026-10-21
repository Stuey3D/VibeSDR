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

/* SPI engine frames: EEPROM byte read/write and the front-end expander frame. */
#include "fw_config.h"
#include "regs.h"
#include "bridge.h"
#include "eeprom.h"

__bit ee_addr16;

void spi_wait(void)
{
    /* two separate polls, in this order, no timeout */
    while (!(BR_STATUS & BRST_XFER_DONE))
        ;
    while (!(BR_STATUS & BRST_SPI_READY))
        ;
}

uint8_t eeprom_read_byte(uint16_t addr)
{
    uint8_t hi = addr >> 8;
    if (ee_addr16) {
        /* frame READ Ahi Alo 00 */
        bridge_write(BRW(BR_REG_SPIMODE, SPI_FRAME(4)));
        bridge_write(BRW(BR_REG_SPITX34, (uint32_t)addr << 8)); /* C002 = A lo, C003 = A hi */
        bridge_write(BRW(BR_REG_SPITX12, (EE_OP_READ << 8) | hi));
    } else {
        /* frame READ|A8 Alo 00 */
        bridge_write(BRW(BR_REG_SPIMODE, SPI_FRAME(3)));
        bridge_write(BRW(BR_REG_SPITX12, ((uint16_t)(EE_OP_READ | ((hi & 1) << EE_A8_SHIFT)) << 8)
                                         | (uint8_t)addr));
    }
    spi_wait();
    return BR_SPIRX;
}

#if FW_EEPROM_WRITE
/* One byte: WREN frame, write frame, then RDSR until WIP clears; no limit. */
void eeprom_write_byte(uint16_t addr, uint8_t d)
{
    uint8_t hi = addr >> 8;

    bridge_write(BRW(BR_REG_SPIMODE, SPI_FRAME(1)));        /* WREN */
    bridge_write(BRW(BR_REG_SPITX12, EE_OP_WREN << 8));
    spi_wait();
    if (ee_addr16) {
        /* frame WRITE Ahi Alo D */
        bridge_write(BRW(BR_REG_SPIMODE, SPI_FRAME(4)));
        bridge_write(BRW(BR_REG_SPITX34, (uint32_t)addr << 8 | d));
        bridge_write(BRW(BR_REG_SPITX12, (EE_OP_WRITE << 8) | hi));
    } else {
        /* frame WRITE|A8 Alo D */
        bridge_write(BRW(BR_REG_SPIMODE, SPI_FRAME(3)));
        bridge_write(BRW(BR_REG_SPITX34, (uint16_t)d << 8));
        bridge_write(BRW(BR_REG_SPITX12, ((uint16_t)(EE_OP_WRITE | ((hi & 1) << EE_A8_SHIFT)) << 8)
                                         | (uint8_t)addr));
    }
    spi_wait();
    do {                                                    /* RDSR xx */
        bridge_write(BRW(BR_REG_SPIMODE, SPI_FRAME(2)));
        bridge_write(BRW(BR_REG_SPITX12, EE_OP_RDSR << 8));
        spi_wait();
    } while (BR_SPIRX & EE_SR_WIP);
}
#endif

#if FW_FE != FW_FE_NONE
/* Expander write frame `op reg val` (MCP23S17 style): op = wIndex.lo, reg/val = wValue hi/lo */
void fe_frame(uint8_t op, uint16_t regval)
{
    bridge_write(BRW(BR_REG_SPIMODE, SPI_FRAME(3)));
    bridge_write(BRW(BR_REG_SPITX34, (uint32_t)regval << 8));
    bridge_write(BRW(BR_REG_SPITX12, (uint16_t)op << 8 | (regval >> 8)));
    spi_wait();
}
#endif
