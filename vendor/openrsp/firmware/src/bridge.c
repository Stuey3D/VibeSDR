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

/* Bridge write port and the register shadows. */
#include "fw_config.h"
#include "regs.h"
#include "bridge.h"

__data uint8_t adcpll_shadow[4] = { BR_REG_ADCPLL, (uint8_t)FW_REG3_BOOT, (uint8_t)(FW_REG3_BOOT >> 8),
                                    (uint8_t)(FW_REG3_BOOT >> 16) };
__data uint8_t gpio_shadow[4] = { BR_REG_GPIO, (uint8_t)FW_REG8_BOOT, (uint8_t)(FW_REG8_BOOT >> 8),
                                  (uint8_t)(FW_REG8_BOOT >> 16) };
__data uint8_t brtmp[4];

/* word in DPL (regbyte), DPH, B, A (SDCC's 32-bit argument registers) */
void bridge_write(uint32_t word) __naked
{
    (void)word;
    __asm
        mov  r7,a
        mov  r6,b
        mov  r5,dph
        mov  a,dpl
        mov  dptr,#0xC000
        movx @dptr,a
        inc  dptr
        mov  a,r5
        movx @dptr,a
        inc  dptr
        mov  a,r6
        movx @dptr,a
        inc  dptr
        mov  a,r7
        movx @dptr,a
        ret
    __endasm;
}

/* IRAM address in DPL */
void bridge_write_d(__data uint8_t *w) __naked
{
    (void)w;
    __asm
        mov  r0,dpl
        mov  dptr,#0xC000
        mov  a,@r0
        movx @dptr,a
        inc  r0
        inc  dptr
        mov  a,@r0
        movx @dptr,a
        inc  r0
        inc  dptr
        mov  a,@r0
        movx @dptr,a
        inc  r0
        inc  dptr
        mov  a,@r0
        movx @dptr,a
        ret
    __endasm;
}
