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

/*
 * sdrplay_api_rsp2.h - OpenRSP public API: RSP2 specifics.
 *
 * Written for OpenRSP. Interface-compatible with the SDRplay API 3.15
 * (same identifiers, values and binary layout); not SDRplay's text.
 */
#ifndef SDRPLAY_API_RSP2_H
#define SDRPLAY_API_RSP2_H

/* Number of LNA states: default bands, Hi-Z (AM) port, 420 MHz band. */
#define RSPII_NUM_LNA_STATES 9
#define RSPII_NUM_LNA_STATES_AMPORT 5
#define RSPII_NUM_LNA_STATES_420MHZ 6

typedef enum {
    sdrplay_api_Rsp2_ANTENNA_A = 5,
    sdrplay_api_Rsp2_ANTENNA_B = 6
} sdrplay_api_Rsp2_AntennaSelectT;

/* Note: port 1 (Hi-Z) is value 1, port 2 is value 0. */
typedef enum {
    sdrplay_api_Rsp2_AMPORT_1 = 1,
    sdrplay_api_Rsp2_AMPORT_2 = 0
} sdrplay_api_Rsp2_AmPortSelectT;

/* Device-wide settings (defaults in brackets). */
typedef struct {
    unsigned char extRefOutputEn;                 /* [0] */
} sdrplay_api_Rsp2ParamsT;

/* Per-tuner settings. */
typedef struct {
    unsigned char biasTEnable;                    /* [0] */
    sdrplay_api_Rsp2_AmPortSelectT amPortSel;     /* [sdrplay_api_Rsp2_AMPORT_2] */
    sdrplay_api_Rsp2_AntennaSelectT antennaSel;   /* [sdrplay_api_Rsp2_ANTENNA_A] */
    unsigned char rfNotchEnable;                  /* [0] */
} sdrplay_api_Rsp2TunerParamsT;

#endif /* SDRPLAY_API_RSP2_H */
