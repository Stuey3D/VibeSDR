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
 * sdrplay_api_rspDx.h - OpenRSP public API: RSPdx / RSPdx-R2 specifics.
 *
 * Written for OpenRSP. Interface-compatible with the SDRplay API 3.15
 * (same identifiers, values and binary layout); not SDRplay's text.
 */
#ifndef SDRPLAY_API_RSPDX_H
#define SDRPLAY_API_RSPDX_H

#include "sdrplay_api_tuner.h"

/* Number of LNA states per band / input path. */
#define RSPDX_NUM_LNA_STATES 28               /* general case */
#define RSPDX_NUM_LNA_STATES_AMPORT2_0_12 19  /* antenna C (port 2), 0-12 MHz */
#define RSPDX_NUM_LNA_STATES_AMPORT2_12_50 20 /* antenna C (port 2), 12-50 MHz */
#define RSPDX_NUM_LNA_STATES_AMPORT2_50_60 25 /* antenna C (port 2), 50-60 MHz */
#define RSPDX_NUM_LNA_STATES_VHF_BAND3 27     /* VHF and band III */
#define RSPDX_NUM_LNA_STATES_420MHZ 21        /* 420 MHz band */
#define RSPDX_NUM_LNA_STATES_LBAND 19         /* L band */
#define RSPDX_NUM_LNA_STATES_DX 22            /* DX path */

typedef enum {
    sdrplay_api_RspDx_ANTENNA_A = 0,
    sdrplay_api_RspDx_ANTENNA_B = 1,
    sdrplay_api_RspDx_ANTENNA_C = 2
} sdrplay_api_RspDx_AntennaSelectT;

/* IF bandwidth used in HDR mode (kHz: 200, 500, 1200, 1700). */
typedef enum {
    sdrplay_api_RspDx_HDRMODE_BW_0_200 = 0,
    sdrplay_api_RspDx_HDRMODE_BW_0_500 = 1,
    sdrplay_api_RspDx_HDRMODE_BW_1_200 = 2,
    sdrplay_api_RspDx_HDRMODE_BW_1_700 = 3
} sdrplay_api_RspDx_HdrModeBwT;

/* Device-wide settings (defaults in brackets). */
typedef struct {
    unsigned char hdrEnable;                      /* [0] */
    unsigned char biasTEnable;                    /* [0] */
    sdrplay_api_RspDx_AntennaSelectT antennaSel;  /* [sdrplay_api_RspDx_ANTENNA_A] */
    unsigned char rfNotchEnable;                  /* [0] */
    unsigned char rfDabNotchEnable;               /* [0] */
} sdrplay_api_RspDxParamsT;

/* Per-tuner settings. */
typedef struct {
    sdrplay_api_RspDx_HdrModeBwT hdrBw;           /* [sdrplay_api_RspDx_HDRMODE_BW_1_700] */
} sdrplay_api_RspDxTunerParamsT;

#endif /* SDRPLAY_API_RSPDX_H */
