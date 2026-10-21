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

/* USB ISR, EP0 engine and request handlers. */
#ifndef USB_H
#define USB_H

#include <stdint.h>

/* ISR prototypes: they must be visible in main.c so that SDCC emits the vectors. */
void isr_int0(void) __interrupt(0);       /* isr_int0.asm */
void isr_usb(void) __interrupt(2);        /* usb_isr.c */
void isr_unused_t1(void) __interrupt(3);  /* main.c: RETI only, keeps vector 0x1B valid */

typedef union {
    uint8_t b[8];
    struct {
        uint8_t bmRequestType;
        uint8_t bRequest;
        uint16_t wValue;
        uint16_t wIndex;
        uint16_t wLength;
    } w;
} setup_t;

extern __data setup_t setup;
#define SU_RT    setup.w.bmRequestType
#define SU_REQ   setup.w.bRequest
#define SU_VAL   setup.w.wValue
#define SU_VALL  setup.b[2]
#define SU_VALH  setup.b[3]
#define SU_IDX   setup.w.wIndex
#define SU_IDXL  setup.b[4]
#define SU_IDXH  setup.b[5]
#define SU_LEN   setup.w.wLength

/* Standard requests (bRequest) */
#define USB_REQ_GET_STATUS        0x00
#define USB_REQ_CLEAR_FEATURE     0x01
#define USB_REQ_SET_FEATURE       0x03
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_DESCRIPTOR    0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_GET_INTERFACE     0x0A
#define USB_REQ_SET_INTERFACE     0x0B
#define USB_REQ_SYNCH_FRAME       0x0C

/* bmRequestType values the standard requests check */
#define USB_RT_DEVICE_OUT   0x00
#define USB_RT_ENDPOINT_OUT 0x02
#define USB_RT_DEVICE_IN    0x80
#define USB_RT_ENDPOINT_IN  0x82

/* Descriptor types (wValue.hi of GET_DESCRIPTOR) */
#define USB_DT_DEVICE       1
#define USB_DT_CONFIG       2
#define USB_DT_STRING       3
#define USB_DT_QUALIFIER    6
#define USB_DT_OTHER_SPEED  7

/* Feature selectors and their GET_STATUS bits */
#define USB_FEAT_ENDPOINT_HALT  0
#define USB_FEAT_REMOTE_WAKEUP  1
#define USB_FEAT_TEST_MODE      2
#define USB_STATUS_HALTED       0x01
#define USB_STATUS_REMOTE_WAKEUP 0x02

/* TEST_MODE selectors (wIndex.hi) */
#define USB_TEST_J          1
#define USB_TEST_K          2
#define USB_TEST_SE0_NAK    3
#define USB_TEST_PACKET     4

#define EP1_IN_ADDR         0x0081
#define EP0_MAXP            64
#define ALT_BULK            3       /* the alt setting with the bulk endpoint */

/* Vendor requests: every bRequest from VREQ_FIRST up. 0x41, 0x43, 0x45 and 0x48 are the requests
 * the Linux kernel's msi2500 driver sends (CMD_WREG, CMD_START_STREAMING, CMD_STOP_STREAMING,
 * CMD_READ_UNKNOWN). */
#define VREQ_FIRST          0x40
#define VREQ_REBOOT         0x40
#define VREQ_WREG           0x41
#define VREQ_RDREG          0x42
#define VREQ_START          0x43
#define VREQ_XDATA_WRITE    0x44
#define VREQ_STOP           0x45
#define VREQ_EEPROM_READ    0x46
#define VREQ_EEPROM_WRITE   0x47
#define VREQ_FNADDR         0x48
#define VREQ_FE             0x49    /* FW_FE_SINGLE */
#define VREQ_FE_A           0x4A    /* FW_FE_DUAL, first path */
#define VREQ_FE_B           0x4B    /* FW_FE_DUAL, second path */

#define EP0_IDLE 0
#define EP0_IN   1
#define EP0_OUT  2
extern __data uint8_t ep0_state;

/* Data-stage source of an IN transfer. Chosen by every SETUP and forgotten on SetupEnd /
 * SentStall, so nothing survives an aborted data stage. */
#define EP0_SRC_XDATA   0      /* IN: descriptor / XDATA; OUT: VREQ_XDATA_WRITE */
#define EP0_SRC_EEPROM  1      /* IN: VREQ_EEPROM_READ */
#define EP0_SRC_EEWRITE 2      /* OUT: VREQ_EEPROM_WRITE */
extern __data uint8_t ep0_src;
extern __xdata uint8_t * __data ep0_ptr;   /* XDATA source/destination */
extern __data uint16_t ep0_eeaddr;        /* EEPROM address (EP0_SRC_EEPROM) */

extern __bit usb_fullspeed;               /* NOT Power.HSMode at the last bus reset */

/* pending actions applied after the status stage */
extern __bit faddr_pending;
extern __data uint8_t faddr_value;
extern __bit reboot_pending;
extern __bit testmode_pending;
extern __data uint8_t testmode_value;

extern __xdata uint8_t cur_config;
extern __xdata uint8_t cur_alt;
extern __bit ep1_halted;
extern __bit remote_wakeup_en;

/* ep0.c */
uint8_t ep0_service(void);                         /* 1 = leave the ISR now (reboot/test mode) */
void ep0_ack(void);
void ep0_stall(void);
void ep0_in_xdata(__xdata uint8_t *src, uint16_t len);   /* start an IN data stage */
void ep0_in_start(uint16_t len);                   /* source already set up */
void ep0_in_short(uint8_t len);                    /* 1-4 bytes from ep0_small[] */
void ep0_out_start(uint16_t len);                  /* OUT data stage of len > 0 bytes */
extern __data uint8_t ep0_small[4];
void ep1_txcsr(uint8_t v);                         /* Index 1, TxCSR lo = v, Index 0 */

/* std_req.c / vendor.c */
void std_request(void);
void vendor_request(void);

#endif
