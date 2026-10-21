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

; Settling delay: after the reg 5 boot write (and around the rsp1 wake-strap probe) the bridge
; is not accessed again for at least about 10 000 machine cycles. The CPU clock is not known, so
; the delay is counted in cycles and written in assembler.
; Here: 41 x (1 + 127 x 2 + 2) + 2 (mov) + 2 (lcall) + 2 (ret) = 10 543 machine cycles,
; the same for every variant. Clobbers r6, r7 (bank 0, caller-saved under SDCC).
	.module delay
	.globl	_fw_delay

	.area	CSEG (CODE)
_fw_delay:
	mov	r6,#41
00001$:
	mov	r7,#127
00002$:
	djnz	r7,00002$
	djnz	r6,00001$
	ret
