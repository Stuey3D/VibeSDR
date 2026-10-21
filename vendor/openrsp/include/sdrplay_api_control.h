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
 * sdrplay_api_control.h - OpenRSP public API: per-channel DSP controls.
 *
 * Written for OpenRSP. Interface-compatible with the SDRplay API 3.15
 * (same identifiers, values and binary layout); not SDRplay's text.
 */
#ifndef SDRPLAY_API_CONTROL_H
#define SDRPLAY_API_CONTROL_H

/* ---- enumerations ------------------------------------------------------ */

/* AGC loop: off, one of the fixed loop rates, or user-defined timing. */
typedef enum {
    sdrplay_api_AGC_DISABLE = 0,
    sdrplay_api_AGC_100HZ = 1,
    sdrplay_api_AGC_50HZ = 2,
    sdrplay_api_AGC_5HZ = 3,
    sdrplay_api_AGC_CTRL_EN = 4
} sdrplay_api_AgcControlT;

/* ADS-B sample-path options. */
typedef enum {
    sdrplay_api_ADSB_DECIMATION = 0,
    sdrplay_api_ADSB_NO_DECIMATION_LOWPASS = 1,
    sdrplay_api_ADSB_NO_DECIMATION_BANDPASS_2MHZ = 2,
    sdrplay_api_ADSB_NO_DECIMATION_BANDPASS_3MHZ = 3
} sdrplay_api_AdsbModeT;

/* ---- structures (defaults in brackets) --------------------------------- */

typedef struct {
    unsigned char DCenable;               /* DC removal [1] */
    unsigned char IQenable;               /* IQ balance correction [1] */
} sdrplay_api_DcOffsetT;

typedef struct {
    unsigned char enable;                 /* [0] */
    unsigned char decimationFactor;       /* 1,2,4,...,32 [1] */
    unsigned char wideBandSignal;         /* half-band filters only [0] */
} sdrplay_api_DecimationT;

typedef struct {
    sdrplay_api_AgcControlT enable;       /* [sdrplay_api_AGC_50HZ] */
    int setPoint_dBfs;                    /* [-60] */
    unsigned short attack_ms;             /* [0] */
    unsigned short decay_ms;              /* [0] */
    unsigned short decay_delay_ms;        /* [0] */
    unsigned short decay_threshold_dB;    /* [0] */
    int syncUpdate;                       /* [0] */
} sdrplay_api_AgcT;

typedef struct {
    sdrplay_api_DcOffsetT dcOffset;
    sdrplay_api_DecimationT decimation;
    sdrplay_api_AgcT agc;
    sdrplay_api_AdsbModeT adsbMode;       /* [sdrplay_api_ADSB_DECIMATION] */
} sdrplay_api_ControlParamsT;

#endif /* SDRPLAY_API_CONTROL_H */
