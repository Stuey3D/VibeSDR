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

/* Standard requests. Dispatch on bRequest only. */
#include "fw_config.h"
#include "regs.h"
#include "desc.h"
#include "stream.h"
#include "usb.h"

__xdata uint8_t cur_config = 1;     /* 1 before any SET_CONFIGURATION (GET_CONFIGURATION right
                                     * after enumeration returns 1) */
__xdata uint8_t cur_alt;
__bit ep1_halted;
__bit remote_wakeup_en;

static void ep1_unhalt(void)
{
    if (ep1_halted) {
        ep1_txcsr(TXCSR_CLRDATATOG);
        ep1_halted = 0;
    }
}

/* ENDPOINT_HALT is only meaningful for EP 0x81 in the bulk alt setting, from an endpoint
 * recipient */
static __bit halt_target_ok(void)
{
    return cur_alt == ALT_BULK && SU_IDX == EP1_IN_ADDR && SU_RT == USB_RT_ENDPOINT_OUT;
}

static void get_status(void)
{
    uint8_t s = 0;
    if (ep1_halted && SU_IDX == EP1_IN_ADDR && SU_RT == USB_RT_ENDPOINT_IN)
        s = USB_STATUS_HALTED;
    else if (remote_wakeup_en && SU_IDX == 0 && SU_RT == USB_RT_DEVICE_IN)
        s = USB_STATUS_REMOTE_WAKEUP;
    ep0_small[0] = s;
    ep0_small[1] = 0;
    ep0_in_short(2);
}

static void clear_feature(void)
{
    if (SU_VAL == USB_FEAT_ENDPOINT_HALT && halt_target_ok()) {
        ep1_txcsr(TXCSR_CLRDATATOG);
        ep1_halted = 0;
    } else if (SU_VAL == USB_FEAT_REMOTE_WAKEUP && SU_IDX == 0 && SU_RT == USB_RT_DEVICE_OUT) {
        remote_wakeup_en = 0;
    } else {
        ep0_stall();
        return;
    }
    ep0_ack();
}

static void set_feature(void)
{
    if (SU_VAL == USB_FEAT_TEST_MODE) {         /* bmRequestType and wIndex.lo not checked: other
                                                 * values there still put the test signal on the
                                                 * USB lines */
        switch (SU_IDXH) {
        case USB_TEST_J:       testmode_value = TESTMODE_J; break;
        case USB_TEST_K:       testmode_value = TESTMODE_K; break;
        case USB_TEST_SE0_NAK: testmode_value = TESTMODE_SE0_NAK; break;
        case USB_TEST_PACKET:  testmode_value = TESTMODE_PACKET; break;
        default: ep0_stall(); return;
        }
        ep0_ack();
        testmode_pending = 1;
        return;
    }
    if (SU_VAL == USB_FEAT_ENDPOINT_HALT && halt_target_ok()) {
        ep1_txcsr(TXCSR_SENDSTALL);
        ep1_halted = 1;
    } else if (SU_VAL == USB_FEAT_REMOTE_WAKEUP && SU_IDX == 0 && SU_RT == USB_RT_DEVICE_OUT) {
        remote_wakeup_en = 1;
    } else {
        ep0_stall();
        return;
    }
    ep0_ack();
}

static void get_descriptor(void)
{
    __xdata uint8_t *d;
    uint8_t len;

    switch (SU_VALH) {
    case USB_DT_DEVICE:
        d = desc_device;
        len = DESC_DEVICE_LEN;
        break;
    case USB_DT_CONFIG:                         /* configuration of the current speed */
    case USB_DT_OTHER_SPEED:                    /* configuration of the other speed */
        if (SU_VALH == USB_DT_OTHER_SPEED)
            d = usb_fullspeed ? desc_cfg_hs : desc_cfg_fs;
        else
            d = usb_fullspeed ? desc_cfg_fs : desc_cfg_hs;
        d[DESC_BTYPE] = SU_VALH;
        len = d[DESC_CFG_TOTAL_LEN];
        break;
    case USB_DT_STRING:                         /* strings 0 and 1 only */
        if (SU_VALL == 0) {
            d = desc_str0;
        } else if (SU_VALL == 1) {
            d = desc_str1;
        } else {
            ep0_stall();
            return;
        }
        len = d[DESC_BLENGTH];
        break;
    case USB_DT_QUALIFIER:
        d = desc_qual;
        len = DESC_QUAL_LEN;
        break;
    default:
        ep0_stall();
        return;
    }
    ep0_in_xdata(d, len);
}

static void set_configuration(void)
{
    if (SU_VAL > 1) {
        ep0_stall();
        return;
    }
    cur_config = SU_VALL;
    ep1_unhalt();
    ep0_ack();
}

static void get_interface(void)                 /* interface 0 only */
{
    if (SU_IDX) {
        ep0_stall();
        return;
    }
    ep0_small[0] = cur_alt;
    ep0_in_short(1);
}

static void set_interface(void)                 /* check first */
{
    uint8_t alt = SU_VALL;
    if (SU_IDX || alt > (usb_fullspeed ? 0 : 3)) {
        ep0_stall();
        return;
    }
    stream_disarm();                            /* before any EP1 access */
    ep1_unhalt();
    cur_alt = alt;
    stream_set_alt(alt);
    ep0_ack();
}

void std_request(void)
{
    switch (SU_REQ) {
    case USB_REQ_GET_STATUS: get_status(); break;
    case USB_REQ_CLEAR_FEATURE: clear_feature(); break;
    case USB_REQ_SET_FEATURE: set_feature(); break;
    case USB_REQ_SET_ADDRESS:
        faddr_value = SU_VALL & 0x7F;           /* 7-bit address */
        faddr_pending = 1;
        ep0_ack();
        break;
    case USB_REQ_GET_DESCRIPTOR: get_descriptor(); break;
    case USB_REQ_GET_CONFIGURATION:
        ep0_small[0] = cur_config;
        ep0_in_short(1);
        break;
    case USB_REQ_SET_CONFIGURATION: set_configuration(); break;
    case USB_REQ_GET_INTERFACE: get_interface(); break;
    case USB_REQ_SET_INTERFACE: set_interface(); break;
    case USB_REQ_SET_DESCRIPTOR:
    case USB_REQ_SYNCH_FRAME:
        ep0_stall();
        break;
    default:                                    /* reserved codes up to 0x3F: no answer */
        break;
    }
}
