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
 * sdrplay_api_callback.h - OpenRSP public API: stream and event callbacks.
 *
 * Written for OpenRSP. Interface-compatible with the SDRplay API 3.15
 * (same identifiers, values and binary layout); not SDRplay's text.
 *
 * Like the header it is compatible with, this file is not self-contained:
 * it uses sdrplay_api_TunerSelectT, so include "sdrplay_api.h" instead.
 */
#ifndef SDRPLAY_API_CALLBACK_H
#define SDRPLAY_API_CALLBACK_H

/* ---- event identifiers ------------------------------------------------- */

typedef enum {
    sdrplay_api_Overload_Detected = 0,
    sdrplay_api_Overload_Corrected = 1
} sdrplay_api_PowerOverloadCbEventIdT;

typedef enum {
    sdrplay_api_MasterInitialised = 0,
    sdrplay_api_SlaveAttached = 1,
    sdrplay_api_SlaveDetached = 2,
    sdrplay_api_SlaveInitialised = 3,
    sdrplay_api_SlaveUninitialised = 4,
    sdrplay_api_MasterDllDisappeared = 5,
    sdrplay_api_SlaveDllDisappeared = 6
} sdrplay_api_RspDuoModeCbEventIdT;

typedef enum {
    sdrplay_api_GainChange = 0,
    sdrplay_api_PowerOverloadChange = 1,
    sdrplay_api_DeviceRemoved = 2,
    sdrplay_api_RspDuoModeChange = 3,
    sdrplay_api_DeviceFailure = 4
} sdrplay_api_EventT;

/* ---- event payloads ---------------------------------------------------- */

typedef struct {
    unsigned int gRdB;
    unsigned int lnaGRdB;
    double currGain;
} sdrplay_api_GainCbParamT;

typedef struct {
    sdrplay_api_PowerOverloadCbEventIdT powerOverloadChangeType;
} sdrplay_api_PowerOverloadCbParamT;

typedef struct {
    sdrplay_api_RspDuoModeCbEventIdT modeChangeType;
} sdrplay_api_RspDuoModeCbParamT;

/* The member that is valid depends on the sdrplay_api_EventT delivered. */
typedef union {
    sdrplay_api_GainCbParamT gainParams;
    sdrplay_api_PowerOverloadCbParamT powerOverloadParams;
    sdrplay_api_RspDuoModeCbParamT rspDuoModeParams;
} sdrplay_api_EventParamsT;

/* ---- stream callback --------------------------------------------------- */

/* Per-packet information passed with every block of samples. */
typedef struct {
    unsigned int firstSampleNum;
    int grChanged;
    int rfChanged;
    int fsChanged;
    unsigned int numSamples;
} sdrplay_api_StreamCbParamsT;

typedef void (*sdrplay_api_StreamCallback_t)(short *xi, short *xq,
        sdrplay_api_StreamCbParamsT *params, unsigned int numSamples,
        unsigned int reset, void *cbContext);

typedef void (*sdrplay_api_EventCallback_t)(sdrplay_api_EventT eventId,
        sdrplay_api_TunerSelectT tuner, sdrplay_api_EventParamsT *params,
        void *cbContext);

/* Handed to sdrplay_api_Init(); StreamBCbFn is used in dual-tuner mode. */
typedef struct {
    sdrplay_api_StreamCallback_t StreamACbFn;
    sdrplay_api_StreamCallback_t StreamBCbFn;
    sdrplay_api_EventCallback_t EventCbFn;
} sdrplay_api_CallbackFnsT;

#endif /* SDRPLAY_API_CALLBACK_H */
