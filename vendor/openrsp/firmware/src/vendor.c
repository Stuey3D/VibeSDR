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

/* Vendor requests. Dispatch on bRequest only. */
#include "fw_config.h"
#include "regs.h"
#include "bridge.h"
#include "eeprom.h"
#include "stream.h"
#include "usb.h"

/* The last tuner r3 word (sync bits included), re-sent before every r2. The valid flag
 * covers r2 before any r3 since boot: then only r2 is sent. The order shows on the tuner's
 * serial bus (logic analyser). */
#define TUNER_R2 2
#define TUNER_R3 3

static __data uint8_t tuner_r3_word[4];
static __bit tuner_r3_valid;

void vreq_reboot(void)
{
    BOOTSEL = SU_VALL & 1;
    reboot_pending = 1;                         /* disconnect + soft reset after the status stage */
    ep0_ack();
}

static void copy4(__data uint8_t *dst, __data uint8_t *src)
{
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[3];
}

/* WREG: the word is wValue.lo, wValue.hi, wIndex.lo, wIndex.hi, i.e. setup bytes 2..5 (wValue =
 * bits 15:0, wIndex = bits 31:16, as msi2500_ctrl_msg in the Linux msi2500 driver sends it),
 * written as they are apart from the cases below (matched on the register without the sync
 * field). */
static void vreq_wreg(void)
{
    __data uint8_t *w = &setup.b[2];
    uint8_t r = SU_VALL & BR_REG_MASK;

    if (r == BR_REG_TUNER) {
        uint8_t n = SU_VALH & TUNER_REG_MASK;
        if (n == TUNER_R3) {
            copy4(tuner_r3_word, w);
            tuner_r3_valid = 1;
        } else {
            while (!(BR_STATUS & BRST_TUNER_IDLE))  /* EA still on */
                ;
            EA = 0;
            if (n == TUNER_R2 && tuner_r3_valid) {
                bridge_write_d(tuner_r3_word);
                while (!(BR_STATUS & BRST_XFER_DONE))
                    ;
                while (!(BR_STATUS & BRST_TUNER_IDLE))
                    ;
            }
            bridge_write_d(w);
            EA = 1;
        }
    } else {
        if (r == BR_REG_CTRL)
            SU_VALH |= BR_CTRL_FORCE;
        EA = 0;
        bridge_write_d(w);
        EA = 1;
        if (r == BR_REG_ADCPLL || r == BR_REG_GPIO) {   /* shadow without the sync bits */
            __data uint8_t *sh = r == BR_REG_ADCPLL ? adcpll_shadow : gpio_shadow;
            copy4(sh, w);
            sh[0] = r;
        } else if (r == BR_REG_PKTFMT && (SU_VALH & PKTFMT_384_MASK) == PKTFMT_384) {
            /* the 384-sample format: the last byte of each packet buffer = PKTBUF_TRAILER */
            PKTBUF_GATE = 0;
            PKTBUF_LAST(0) = PKTBUF_TRAILER;
            PKTBUF_LAST(1) = PKTBUF_TRAILER;
            PKTBUF_LAST(2) = PKTBUF_TRAILER;
            PKTBUF_LAST(3) = PKTBUF_TRAILER;
            PKTBUF_GATE = 1;
        }
    }
    ep0_ack();
}

#if FW_FE != FW_FE_NONE
/* reg 8 = gpio_shadow with the path's GPIO bit at its active level, SPI engine on, the
 * expander frame, SPI engine off, then the bit forced to the inactive level. The
 * shadow itself is not changed. `mask` is the bit in data[15:8] (C002); `active` = mask for an
 * active-high line, 0 for active-low. */
static void vreq_fe(uint8_t mask, uint8_t active)
{
    copy4(brtmp, gpio_shadow);
    brtmp[2] = (brtmp[2] & ~mask) | active;
    bridge_write_d(brtmp);
    bridge_write(BRW(BR_REG_SPIMODE, SPI_ON));
    fe_frame(SU_IDXL, SU_VAL);
    bridge_write(BRW(BR_REG_SPIMODE, SPI_OFF));
    brtmp[2] ^= mask;
    bridge_write_d(brtmp);
    ep0_ack();
}
#endif

void vendor_request(void)
{
    switch (SU_REQ) {
    case VREQ_REBOOT:
        vreq_reboot();
        break;
    case VREQ_WREG:
        vreq_wreg();
        break;
    case VREQ_RDREG: {                          /* wIndex = byte offset; no range check, so the
                                                 * host can read any XDATA from C000 up */
        __xdata uint8_t *p = (__xdata uint8_t *)(BR_BASE + SU_IDX);
        ep0_small[0] = p[0];
        ep0_small[1] = p[1];
        ep0_small[2] = p[2];
        ep0_small[3] = p[3];
        ep0_in_short(4);
        break;
    }
    case VREQ_START:                            /* ignored at full speed */
        if (!usb_fullspeed)
            stream_start();
        ep0_ack();
        break;
    case VREQ_XDATA_WRITE:                      /* wValue = XDATA address */
        if (!SU_LEN) {                          /* wLength 0: ACK at once */
            ep0_ack();
            break;
        }
        ep0_ptr = (__xdata uint8_t *)SU_VAL;
        ep0_out_start(SU_LEN);
        break;
    case VREQ_STOP:                             /* the next INT0 stops */
        if (!usb_fullspeed)
            stop_req = 1;
        ep0_ack();
        break;
    case VREQ_EEPROM_READ:                      /* SPI engine on in ep0_in_start */
        ep0_src = EP0_SRC_EEPROM;
        ep0_eeaddr = SU_VAL;
        ep0_in_start(SU_LEN);
        break;
#if FW_EEPROM_WRITE
    case VREQ_EEPROM_WRITE:                     /* wValue = EEPROM address */
        if (!SU_LEN) {                          /* wLength 0: ACK, no bridge write, no state */
            ep0_ack();
            break;
        }
        bridge_write(BRW(BR_REG_SPIMODE, SPI_ON));
        ep0_src = EP0_SRC_EEWRITE;
        ep0_eeaddr = SU_VAL;
        ep0_out_start(SU_LEN);
        break;
#endif
    case VREQ_FNADDR:                           /* the true USB address */
        ep0_small[0] = MUSB_FADDR;
        ep0_in_short(1);
        break;
#if FW_FE == FW_FE_SINGLE
    case VREQ_FE:                               /* GPIO bit 11, active low */
        vreq_fe(GPIO_HI_BIT11, 0);
        break;
#elif FW_FE == FW_FE_DUAL
    case VREQ_FE_A:                             /* GPIO bit 11, active low */
        vreq_fe(GPIO_HI_BIT11, 0);
        break;
    case VREQ_FE_B:                             /* GPIO bit 10, active high */
        vreq_fe(GPIO_HI_BIT10, GPIO_HI_BIT10);
        break;
#endif
    default:                                    /* no answer: the host's request times out */
        break;
    }
}
