; SPDX-License-Identifier: GPL-3.0-or-later
; Copyright (C) 2026 the OpenRSP authors
;
; This file is part of OpenRSP.
;
; OpenRSP is free software: you can redistribute it and/or modify it under the terms of the
; GNU General Public License as published by the Free Software Foundation, either version 3
; of the License, or (at your option) any later version.
;
; OpenRSP is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
; even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
; GNU General Public License for more details.
;
; You should have received a copy of the GNU General Public License along with OpenRSP. If
; not, see <https://www.gnu.org/licenses/>.

; INT0: packetiser "FIFO loaded", high priority, enabled by START.
;
; One call per load: EP1 TxCSR = TxPktRdy with the real MUSB Index saved and restored
; (the interrupted code may be in the middle of an EP0 access). Then either the
; next "go" strobe (P0.0 low -> high) or, with a STOP pending, disarm: EX0 = 0, P0.1 = 0,
; stop_req = 0 and no strobe.
;
; It may pre-empt the USB ISR anywhere EA = 1, so it touches only Index (restored), EP1 TxCSR,
; P0.0/P0.1, EX0 and stop_req. No register bank is used: A, DPL and DPH are pushed,
; and nothing here changes CY/AC/OV (PSW.P follows A, which is restored).
; Running path: 37 machine cycles from the vector LJMP target to RETI inclusive; the
; budget is 58 (up to 24 000 loads per second on alt 1).
	.module isr_int0
	.globl	_isr_int0
	.globl	_stop_req

MUSB_INDEX	= 0x400E		; MUSB Index register (XDATA)
MUSB_INDEX_LO	= 0x0E			; its address low byte
MUSB_TXCSR_LO	= 0x12			; low byte of X:4012, EP1 TxCSR lo while Index = 1
P0_0		= 0x80			; bit: packetiser "go" strobe
P0_1		= 0x81			; bit: stream enable
EX0		= 0xA8			; bit: INT0 enable

	.area	CSEG (CODE)
_isr_int0:
	push	acc
	push	dpl
	push	dph
	mov	dptr,#MUSB_INDEX
	movx	a,@dptr
	push	acc			; the interrupted code's Index
	mov	a,#0x01
	movx	@dptr,a			; Index = 1 (EP1)
	mov	dpl,#MUSB_TXCSR_LO
	movx	@dptr,a			; TxPktRdy (bit 0: the 1 already in A)
	mov	dpl,#MUSB_INDEX_LO
	pop	acc
	movx	@dptr,a			; Index restored
	jb	_stop_req,00001$
	clr	P0_0			; next load
	setb	P0_0
	pop	dph
	pop	dpl
	pop	acc
	reti
00001$:
	clr	EX0			; STOP: disarm, no strobe
	clr	P0_1
	clr	_stop_req
	pop	dph
	pop	dpl
	pop	acc
	reti
