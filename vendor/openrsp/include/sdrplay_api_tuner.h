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
 * sdrplay_api_tuner.h - OpenRSP public API: tuner parameters.
 *
 * Written for OpenRSP. Interface-compatible with the SDRplay API 3.15
 * (same identifiers, values and binary layout); not SDRplay's text.
 */
#ifndef SDRPLAY_API_TUNER_H
#define SDRPLAY_API_TUNER_H

/* Highest baseband (IF) gain reduction in dB. */
#define MAX_BB_GR (59)

/* ---- enumerations ------------------------------------------------------ */

/* IF filter bandwidth; the value is the bandwidth in kHz. */
typedef enum {
    sdrplay_api_BW_Undefined = 0,
    sdrplay_api_BW_0_200 = 200,
    sdrplay_api_BW_0_300 = 300,
    sdrplay_api_BW_0_600 = 600,
    sdrplay_api_BW_1_536 = 1536,
    sdrplay_api_BW_5_000 = 5000,
    sdrplay_api_BW_6_000 = 6000,
    sdrplay_api_BW_7_000 = 7000,
    sdrplay_api_BW_8_000 = 8000
} sdrplay_api_Bw_MHzT;

/* Intermediate frequency in kHz (0 = zero-IF). */
typedef enum {
    sdrplay_api_IF_Undefined = -1,
    sdrplay_api_IF_Zero = 0,
    sdrplay_api_IF_0_450 = 450,
    sdrplay_api_IF_1_620 = 1620,
    sdrplay_api_IF_2_048 = 2048
} sdrplay_api_If_kHzT;

/* First LO selection for the up-converter bands. */
typedef enum {
    sdrplay_api_LO_Undefined = 0,
    sdrplay_api_LO_Auto = 1,
    sdrplay_api_LO_120MHz = 2,
    sdrplay_api_LO_144MHz = 3,
    sdrplay_api_LO_168MHz = 4
} sdrplay_api_LoModeT;

/* Lower limit applied to the requested gain reduction. */
typedef enum {
    sdrplay_api_EXTENDED_MIN_GR = 0,
    sdrplay_api_NORMAL_MIN_GR = 20
} sdrplay_api_MinGainReductionT;

/* Which tuner(s) a call refers to. */
typedef enum {
    sdrplay_api_Tuner_Neither = 0,
    sdrplay_api_Tuner_A = 1,
    sdrplay_api_Tuner_B = 2,
    sdrplay_api_Tuner_Both = 3
} sdrplay_api_TunerSelectT;

/* ---- structures -------------------------------------------------------- */

/* Overall system gain in dB, reported back by the library. */
typedef struct {
    float curr;
    float max;
    float min;
} sdrplay_api_GainValuesT;

typedef struct {
    int gRdB;                             /* IF gain reduction, dB (50) */
    unsigned char LNAstate;               /* LNA state index (0) */
    unsigned char syncUpdate;             /* apply on sync event (0) */
    sdrplay_api_MinGainReductionT minGr;  /* (sdrplay_api_NORMAL_MIN_GR) */
    sdrplay_api_GainValuesT gainVals;     /* output only */
} sdrplay_api_GainT;

typedef struct {
    double rfHz;                          /* tuned frequency, Hz (200 MHz) */
    unsigned char syncUpdate;             /* (0) */
} sdrplay_api_RfFreqT;

/* Tuner DC-offset tracking; times are in units of 72*3/24e6 s. */
typedef struct {
    unsigned char dcCal;                  /* calibration mode (3 = periodic) */
    unsigned char speedUp;                /* (0) */
    int trackTime;                        /* (1) */
    int refreshRateTime;                  /* (2048) */
} sdrplay_api_DcOffsetTunerT;

typedef struct {
    sdrplay_api_Bw_MHzT bwType;           /* (sdrplay_api_BW_0_200) */
    sdrplay_api_If_kHzT ifType;           /* (sdrplay_api_IF_Zero) */
    sdrplay_api_LoModeT loMode;           /* (sdrplay_api_LO_Auto) */
    sdrplay_api_GainT gain;
    sdrplay_api_RfFreqT rfFreq;
    sdrplay_api_DcOffsetTunerT dcOffsetTuner;
} sdrplay_api_TunerParamsT;

#endif /* SDRPLAY_API_TUNER_H */
