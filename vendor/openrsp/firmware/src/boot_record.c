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

/* EEPROM boot record at byte 0. Only the identity record is handled; any other
 * signature keeps the defaults. */
#include "fw_config.h"
#include "regs.h"
#include "eeprom.h"
#include "desc.h"

void boot_record(void)
{
    uint8_t i;
    uint8_t sig = eeprom_read_byte(EE_BOOT_SIG);

    if (sig == EE_BOOT_SIG_ID) {
        /* idVendor, idProduct */
        for (i = EE_BOOT_ID; i <= EE_BOOT_ID_LAST; i++)
            desc_device[DESC_DEV_ID + (i - EE_BOOT_ID)] = eeprom_read_byte(i);
        /* serial: the low byte of each UTF-16LE character */
        for (i = EE_BOOT_SERIAL; i <= EE_BOOT_SERIAL_LAST; i++)
            desc_str1[DESC_STR1_SERIAL + 2 * (i - EE_BOOT_SERIAL)] = eeprom_read_byte(i);
    }
}
