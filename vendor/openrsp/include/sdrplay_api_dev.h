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
 * sdrplay_api_dev.h - OpenRSP public API: device-wide parameters.
 *
 * Written for OpenRSP. Interface-compatible with the SDRplay API 3.15
 * (same identifiers, values and binary layout); not SDRplay's text.
 */
#ifndef SDRPLAY_API_DEV_H
#define SDRPLAY_API_DEV_H

#include "sdrplay_api_rsp1a.h"
#include "sdrplay_api_rsp2.h"
#include "sdrplay_api_rspDuo.h"
#include "sdrplay_api_rspDx.h"

/* USB transfer type for the sample stream. */
typedef enum {
    sdrplay_api_ISOCH = 0,
    sdrplay_api_BULK = 1
} sdrplay_api_TransferModeT;

/* ---- structures (defaults in brackets) --------------------------------- */

typedef struct {
    double fsHz;                          /* ADC sample rate, Hz [2e6] */
    unsigned char syncUpdate;             /* [0] */
    unsigned char reCal;                  /* [0] */
} sdrplay_api_FsFreqT;

/* Sample number / period at which synchronous updates take effect. */
typedef struct {
    unsigned int sampleNum;               /* [0] */
    unsigned int period;                  /* [0] */
} sdrplay_api_SyncUpdateT;

typedef struct {
    unsigned char resetGainUpdate;        /* [0] */
    unsigned char resetRfUpdate;          /* [0] */
    unsigned char resetFsUpdate;          /* [0] */
} sdrplay_api_ResetFlagsT;

/* Settings shared by both tuners; only the block for the open device
 * model is used. */
typedef struct {
    double ppm;                           /* reference correction [0.0] */
    sdrplay_api_FsFreqT fsFreq;
    sdrplay_api_SyncUpdateT syncUpdate;
    sdrplay_api_ResetFlagsT resetFlags;
    sdrplay_api_TransferModeT mode;       /* [sdrplay_api_ISOCH] */
    unsigned int samplesPerPkt;           /* output only */
    sdrplay_api_Rsp1aParamsT rsp1aParams;
    sdrplay_api_Rsp2ParamsT rsp2Params;
    sdrplay_api_RspDuoParamsT rspDuoParams;
    sdrplay_api_RspDxParamsT rspDxParams;
} sdrplay_api_DevParamsT;

#endif /* SDRPLAY_API_DEV_H */
