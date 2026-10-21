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
 * sdrplay_api_rspDuo.h - OpenRSP public API: RSPduo specifics.
 *
 * Written for OpenRSP. Interface-compatible with the SDRplay API 3.15
 * (same identifiers, values and binary layout); not SDRplay's text.
 */
/* The guard spelling (mixed case) is part of the compatible interface. */
#ifndef SDRPLAY_API_RSPduo_H
#define SDRPLAY_API_RSPduo_H

/* Number of LNA states: default bands, Hi-Z port, AM band, L band. */
#define RSPDUO_NUM_LNA_STATES 10
#define RSPDUO_NUM_LNA_STATES_AMPORT 5
#define RSPDUO_NUM_LNA_STATES_AM 7
#define RSPDUO_NUM_LNA_STATES_LBAND 9

/* Operating mode; values are bit flags. */
typedef enum {
    sdrplay_api_RspDuoMode_Unknown = 0,
    sdrplay_api_RspDuoMode_Single_Tuner = 1,
    sdrplay_api_RspDuoMode_Dual_Tuner = 2,
    sdrplay_api_RspDuoMode_Master = 4,
    sdrplay_api_RspDuoMode_Slave = 8
} sdrplay_api_RspDuoModeT;

/* Note: port 1 (Hi-Z) is value 1, port 2 is value 0. */
typedef enum {
    sdrplay_api_RspDuo_AMPORT_1 = 1,
    sdrplay_api_RspDuo_AMPORT_2 = 0
} sdrplay_api_RspDuo_AmPortSelectT;

/* Device-wide settings (defaults in brackets). */
typedef struct {
    int extRefOutputEn;                           /* [0] */
} sdrplay_api_RspDuoParamsT;

/* Flags a slave application uses to clear pending-update state. */
typedef struct {
    unsigned char resetGainUpdate;                /* [0] */
    unsigned char resetRfUpdate;                  /* [0] */
} sdrplay_api_RspDuo_ResetSlaveFlagsT;

/* Per-tuner settings. */
typedef struct {
    unsigned char biasTEnable;                        /* [0] */
    sdrplay_api_RspDuo_AmPortSelectT tuner1AmPortSel; /* [sdrplay_api_RspDuo_AMPORT_2] */
    unsigned char tuner1AmNotchEnable;                /* [0] */
    unsigned char rfNotchEnable;                      /* [0] */
    unsigned char rfDabNotchEnable;                   /* [0] */
    sdrplay_api_RspDuo_ResetSlaveFlagsT resetSlaveFlags;
} sdrplay_api_RspDuoTunerParamsT;

#endif /* SDRPLAY_API_RSPduo_H */
