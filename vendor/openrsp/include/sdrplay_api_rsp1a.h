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
 * sdrplay_api_rsp1a.h - OpenRSP public API: RSP1A / RSP1B specifics.
 *
 * Written for OpenRSP. Interface-compatible with the SDRplay API 3.15
 * (same identifiers, values and binary layout); not SDRplay's text.
 */
#ifndef SDRPLAY_API_RSP1A_H
#define SDRPLAY_API_RSP1A_H

/* Number of LNA states: default bands, AM band, L band. */
#define RSPIA_NUM_LNA_STATES 10
#define RSPIA_NUM_LNA_STATES_AM 7
#define RSPIA_NUM_LNA_STATES_LBAND 9

/* Device-wide settings (defaults in brackets). */
typedef struct {
    unsigned char rfNotchEnable;          /* FM broadcast notch [0] */
    unsigned char rfDabNotchEnable;       /* DAB notch [0] */
} sdrplay_api_Rsp1aParamsT;

/* Per-tuner settings. */
typedef struct {
    unsigned char biasTEnable;            /* [0] */
} sdrplay_api_Rsp1aTunerParamsT;

#endif /* SDRPLAY_API_RSP1A_H */
