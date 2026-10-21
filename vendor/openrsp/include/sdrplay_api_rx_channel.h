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
 * sdrplay_api_rx_channel.h - OpenRSP public API: one receive channel.
 *
 * Written for OpenRSP. Interface-compatible with the SDRplay API 3.15
 * (same identifiers, values and binary layout); not SDRplay's text.
 */
#ifndef SDRPLAY_API_RX_CHANNEL_H
#define SDRPLAY_API_RX_CHANNEL_H

#include "sdrplay_api_tuner.h"
#include "sdrplay_api_control.h"
#include "sdrplay_api_rsp1a.h"
#include "sdrplay_api_rsp2.h"
#include "sdrplay_api_rspDuo.h"
#include "sdrplay_api_rspDx.h"

/* Everything that can be set per tuner; only the block for the open
 * device model is used. */
typedef struct {
    sdrplay_api_TunerParamsT tunerParams;
    sdrplay_api_ControlParamsT ctrlParams;
    sdrplay_api_Rsp1aTunerParamsT rsp1aTunerParams;
    sdrplay_api_Rsp2TunerParamsT rsp2TunerParams;
    sdrplay_api_RspDuoTunerParamsT rspDuoTunerParams;
    sdrplay_api_RspDxTunerParamsT rspDxTunerParams;
} sdrplay_api_RxChannelParamsT;

#endif /* SDRPLAY_API_RX_CHANNEL_H */
