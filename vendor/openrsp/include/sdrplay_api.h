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
 * sdrplay_api.h - OpenRSP public API, main header.
 *
 * Written for OpenRSP. Interface-compatible with the SDRplay API 3.15:
 * identifiers, constant values, structure layouts and function signatures
 * are the same, so code built against the SDRplay headers builds unchanged
 * against these and the binaries are interchangeable. The text of this file
 * is OpenRSP's own.
 *
 * Applications include only this file.
 */
#ifndef SDRPLAY_API_H
#define SDRPLAY_API_H

#include "sdrplay_api_dev.h"
#include "sdrplay_api_rx_channel.h"
#include "sdrplay_api_callback.h"

/* Opaque device handle: the Windows HANDLE with MSVC, void * with GCC/Clang
 * (MinGW included). */
#if defined(_M_X64) || defined(_M_IX86) || defined(_M_ARM64)
#include "windows.h"
#elif defined(__GNUC__)
typedef void *HANDLE;
#endif

/* Linkage qualifier on every API function. Pre-define it to override;
 * define STATIC_LIB for a static build. MSVC imports from the DLL. */
#ifndef _SDRPLAY_DLL_QUALIFIER
#if !defined(STATIC_LIB) && (defined(_M_X64) || defined(_M_IX86) || defined(_M_ARM64))
#define _SDRPLAY_DLL_QUALIFIER __declspec(dllimport)
#elif defined(STATIC_LIB) || defined(__GNUC__)
#define _SDRPLAY_DLL_QUALIFIER
#endif
#endif

/* Version these headers describe. Compare with sdrplay_api_ApiVersion(). */
#define SDRPLAY_API_VERSION (float)(3.15)

/* ---- limits ------------------------------------------------------------ */
#define SDRPLAY_MAX_DEVICES (16)
#define SDRPLAY_MAX_TUNERS_PER_DEVICE (2)
#define SDRPLAY_MAX_SER_NO_LEN (64)
#define SDRPLAY_MAX_ROOT_NM_LEN (32)

/* ---- hardware IDs (sdrplay_api_DeviceT.hwVer) -------------------------- */
#define SDRPLAY_RSP1_ID (1)
#define SDRPLAY_RSP1A_ID (255)
#define SDRPLAY_RSP2_ID (2)
#define SDRPLAY_RSPduo_ID (3)
#define SDRPLAY_RSPdx_ID (4)
#define SDRPLAY_RSP1B_ID (6)
#define SDRPLAY_RSPdxR2_ID (7)

/* ---- status codes ------------------------------------------------------ */
typedef enum {
    sdrplay_api_Success = 0,
    sdrplay_api_Fail = 1,
    sdrplay_api_InvalidParam = 2,
    sdrplay_api_OutOfRange = 3,
    sdrplay_api_GainUpdateError = 4,
    sdrplay_api_RfUpdateError = 5,
    sdrplay_api_FsUpdateError = 6,
    sdrplay_api_HwError = 7,
    sdrplay_api_AliasingError = 8,
    sdrplay_api_AlreadyInitialised = 9,
    sdrplay_api_NotInitialised = 10,
    sdrplay_api_NotEnabled = 11,
    sdrplay_api_HwVerError = 12,
    sdrplay_api_OutOfMemError = 13,
    sdrplay_api_ServiceNotResponding = 14,
    sdrplay_api_StartPending = 15,
    sdrplay_api_StopPending = 16,
    sdrplay_api_InvalidMode = 17,
    sdrplay_api_FailedVerification1 = 18,
    sdrplay_api_FailedVerification2 = 19,
    sdrplay_api_FailedVerification3 = 20,
    sdrplay_api_FailedVerification4 = 21,
    sdrplay_api_FailedVerification5 = 22,
    sdrplay_api_FailedVerification6 = 23,
    sdrplay_api_InvalidServiceVersion = 24
} sdrplay_api_ErrT;

/* ---- sdrplay_api_Update() reason bits ---------------------------------- */
/* Bits below sdrplay_api_Update_Tuner_Gr may only be used by the master
 * (or single-tuner) application; bits from it upwards by master and slave.
 * The value 0x80000000 makes this enum unsigned. */
typedef enum {
    sdrplay_api_Update_None = 0x00000000,

    sdrplay_api_Update_Dev_Fs = 0x00000001,
    sdrplay_api_Update_Dev_Ppm = 0x00000002,
    sdrplay_api_Update_Dev_SyncUpdate = 0x00000004,
    sdrplay_api_Update_Dev_ResetFlags = 0x00000008,

    sdrplay_api_Update_Rsp1a_BiasTControl = 0x00000010,
    sdrplay_api_Update_Rsp1a_RfNotchControl = 0x00000020,
    sdrplay_api_Update_Rsp1a_RfDabNotchControl = 0x00000040,

    sdrplay_api_Update_Rsp2_BiasTControl = 0x00000080,
    sdrplay_api_Update_Rsp2_AmPortSelect = 0x00000100,
    sdrplay_api_Update_Rsp2_AntennaControl = 0x00000200,
    sdrplay_api_Update_Rsp2_RfNotchControl = 0x00000400,
    sdrplay_api_Update_Rsp2_ExtRefControl = 0x00000800,

    sdrplay_api_Update_RspDuo_ExtRefControl = 0x00001000,

    sdrplay_api_Update_Master_Spare_1 = 0x00002000,
    sdrplay_api_Update_Master_Spare_2 = 0x00004000,

    sdrplay_api_Update_Tuner_Gr = 0x00008000,
    sdrplay_api_Update_Tuner_GrLimits = 0x00010000,
    sdrplay_api_Update_Tuner_Frf = 0x00020000,
    sdrplay_api_Update_Tuner_BwType = 0x00040000,
    sdrplay_api_Update_Tuner_IfType = 0x00080000,
    sdrplay_api_Update_Tuner_DcOffset = 0x00100000,
    sdrplay_api_Update_Tuner_LoMode = 0x00200000,

    sdrplay_api_Update_Ctrl_DCoffsetIQimbalance = 0x00400000,
    sdrplay_api_Update_Ctrl_Decimation = 0x00800000,
    sdrplay_api_Update_Ctrl_Agc = 0x01000000,
    sdrplay_api_Update_Ctrl_AdsbMode = 0x02000000,
    sdrplay_api_Update_Ctrl_OverloadMsgAck = 0x04000000,

    sdrplay_api_Update_RspDuo_BiasTControl = 0x08000000,
    sdrplay_api_Update_RspDuo_AmPortSelect = 0x10000000,
    sdrplay_api_Update_RspDuo_Tuner1AmNotchControl = 0x20000000,
    sdrplay_api_Update_RspDuo_RfNotchControl = 0x40000000,
    sdrplay_api_Update_RspDuo_RfDabNotchControl = 0x80000000
} sdrplay_api_ReasonForUpdateT;

/* Second word of update reasons (all master-only so far). */
typedef enum {
    sdrplay_api_Update_Ext1_None = 0x00000000,

    sdrplay_api_Update_RspDx_HdrEnable = 0x00000001,
    sdrplay_api_Update_RspDx_BiasTControl = 0x00000002,
    sdrplay_api_Update_RspDx_AntennaControl = 0x00000004,
    sdrplay_api_Update_RspDx_RfNotchControl = 0x00000008,
    sdrplay_api_Update_RspDx_RfDabNotchControl = 0x00000010,
    sdrplay_api_Update_RspDx_HdrBw = 0x00000020,
    sdrplay_api_Update_RspDuo_ResetSlaveFlags = 0x00000040
} sdrplay_api_ReasonForUpdateExtension1T;

/* Debug output verbosity for sdrplay_api_DebugEnable(). */
typedef enum {
    sdrplay_api_DbgLvl_Disable = 0,
    sdrplay_api_DbgLvl_Verbose = 1,
    sdrplay_api_DbgLvl_Warning = 2,
    sdrplay_api_DbgLvl_Error = 3,
    sdrplay_api_DbgLvl_Message = 4
} sdrplay_api_DbgLvl_t;

/* ---- structures -------------------------------------------------------- */

/* One entry of the sdrplay_api_GetDevices() list. */
typedef struct {
    char SerNo[SDRPLAY_MAX_SER_NO_LEN];   /* NUL-terminated serial number */
    unsigned char hwVer;                  /* one of the SDRPLAY_*_ID values */
    sdrplay_api_TunerSelectT tuner;       /* RSPduo: tuner(s) to use */
    sdrplay_api_RspDuoModeT rspDuoMode;   /* RSPduo: available / chosen mode */
    unsigned char valid;
    double rspDuoSampleFreq;              /* RSPduo master/slave sample rate */
    HANDLE dev;                           /* filled in by SelectDevice */
} sdrplay_api_DeviceT;

/* Returned by sdrplay_api_GetDeviceParams(); rxChannelB is set only for
 * the RSPduo in dual-tuner mode. */
typedef struct {
    sdrplay_api_DevParamsT *devParams;
    sdrplay_api_RxChannelParamsT *rxChannelA;
    sdrplay_api_RxChannelParamsT *rxChannelB;
} sdrplay_api_DeviceParamsT;

/* Details of the most recent error. */
typedef struct {
    char file[256];
    char function[256];
    int line;
    char message[1024];
} sdrplay_api_ErrorInfoT;

/* ---- function pointer types (for run-time loading) --------------------- */

typedef sdrplay_api_ErrT (*sdrplay_api_Open_t)(void);
typedef sdrplay_api_ErrT (*sdrplay_api_Close_t)(void);
typedef sdrplay_api_ErrT (*sdrplay_api_ApiVersion_t)(float *apiVer);
typedef sdrplay_api_ErrT (*sdrplay_api_LockDeviceApi_t)(void);
typedef sdrplay_api_ErrT (*sdrplay_api_UnlockDeviceApi_t)(void);
typedef sdrplay_api_ErrT (*sdrplay_api_GetDevices_t)(sdrplay_api_DeviceT *devices,
        unsigned int *numDevs, unsigned int maxDevs);
typedef sdrplay_api_ErrT (*sdrplay_api_SelectDevice_t)(sdrplay_api_DeviceT *device);
typedef sdrplay_api_ErrT (*sdrplay_api_ReleaseDevice_t)(sdrplay_api_DeviceT *device);
typedef const char *(*sdrplay_api_GetErrorString_t)(sdrplay_api_ErrT err);
typedef sdrplay_api_ErrorInfoT *(*sdrplay_api_GetLastError_t)(sdrplay_api_DeviceT *device);
typedef sdrplay_api_ErrorInfoT *(*sdrplay_api_GetLastErrorByType_t)(sdrplay_api_DeviceT *device,
        int type, unsigned long long *time);
typedef sdrplay_api_ErrT (*sdrplay_api_DisableHeartbeat_t)(void);

typedef sdrplay_api_ErrT (*sdrplay_api_DebugEnable_t)(HANDLE dev, sdrplay_api_DbgLvl_t dbgLvl);
typedef sdrplay_api_ErrT (*sdrplay_api_GetDeviceParams_t)(HANDLE dev,
        sdrplay_api_DeviceParamsT **deviceParams);
typedef sdrplay_api_ErrT (*sdrplay_api_Init_t)(HANDLE dev,
        sdrplay_api_CallbackFnsT *callbackFns, void *cbContext);
typedef sdrplay_api_ErrT (*sdrplay_api_Uninit_t)(HANDLE dev);
typedef sdrplay_api_ErrT (*sdrplay_api_Update_t)(HANDLE dev, sdrplay_api_TunerSelectT tuner,
        sdrplay_api_ReasonForUpdateT reasonForUpdate,
        sdrplay_api_ReasonForUpdateExtension1T reasonForUpdateExt1);
typedef sdrplay_api_ErrT (*sdrplay_api_SwapRspDuoActiveTuner_t)(HANDLE dev,
        sdrplay_api_TunerSelectT *tuner, sdrplay_api_RspDuo_AmPortSelectT tuner1AmPortSel);
/* Note: unlike the function itself, this pointer type has no HANDLE
 * parameter. Kept as-is for source compatibility. */
typedef sdrplay_api_ErrT (*sdrplay_api_SwapRspDuoDualTunerModeSampleRate_t)(
        double *currentSampleRate, double newSampleRate);
typedef sdrplay_api_ErrT (*sdrplay_api_SwapRspDuoMode_t)(sdrplay_api_DeviceT *currDevice,
        sdrplay_api_DeviceParamsT **deviceParams, sdrplay_api_RspDuoModeT rspDuoMode,
        double sampleRate, sdrplay_api_TunerSelectT tuner, sdrplay_api_Bw_MHzT bwType,
        sdrplay_api_If_kHzT ifType, sdrplay_api_RspDuo_AmPortSelectT tuner1AmPortSel);

/* ---- functions --------------------------------------------------------- */

#ifdef __cplusplus
extern "C" {
#endif

/* Library / service session. */
_SDRPLAY_DLL_QUALIFIER sdrplay_api_ErrT sdrplay_api_Open(void);
_SDRPLAY_DLL_QUALIFIER sdrplay_api_ErrT sdrplay_api_Close(void);
_SDRPLAY_DLL_QUALIFIER sdrplay_api_ErrT sdrplay_api_ApiVersion(float *apiVer);
_SDRPLAY_DLL_QUALIFIER sdrplay_api_ErrT sdrplay_api_LockDeviceApi(void);
_SDRPLAY_DLL_QUALIFIER sdrplay_api_ErrT sdrplay_api_UnlockDeviceApi(void);
_SDRPLAY_DLL_QUALIFIER sdrplay_api_ErrT sdrplay_api_GetDevices(sdrplay_api_DeviceT *devices,
        unsigned int *numDevs, unsigned int maxDevs);
_SDRPLAY_DLL_QUALIFIER sdrplay_api_ErrT sdrplay_api_SelectDevice(sdrplay_api_DeviceT *device);
_SDRPLAY_DLL_QUALIFIER sdrplay_api_ErrT sdrplay_api_ReleaseDevice(sdrplay_api_DeviceT *device);
_SDRPLAY_DLL_QUALIFIER const char *sdrplay_api_GetErrorString(sdrplay_api_ErrT err);
_SDRPLAY_DLL_QUALIFIER sdrplay_api_ErrorInfoT *sdrplay_api_GetLastError(sdrplay_api_DeviceT *device);
_SDRPLAY_DLL_QUALIFIER sdrplay_api_ErrorInfoT *sdrplay_api_GetLastErrorByType(
        sdrplay_api_DeviceT *device, int type, unsigned long long *time);
/* Only effective when called before sdrplay_api_SelectDevice(). */
_SDRPLAY_DLL_QUALIFIER sdrplay_api_ErrT sdrplay_api_DisableHeartbeat(void);

/* Per-device calls. */
_SDRPLAY_DLL_QUALIFIER sdrplay_api_ErrT sdrplay_api_DebugEnable(HANDLE dev, sdrplay_api_DbgLvl_t enable);
_SDRPLAY_DLL_QUALIFIER sdrplay_api_ErrT sdrplay_api_GetDeviceParams(HANDLE dev,
        sdrplay_api_DeviceParamsT **deviceParams);
_SDRPLAY_DLL_QUALIFIER sdrplay_api_ErrT sdrplay_api_Init(HANDLE dev,
        sdrplay_api_CallbackFnsT *callbackFns, void *cbContext);
_SDRPLAY_DLL_QUALIFIER sdrplay_api_ErrT sdrplay_api_Uninit(HANDLE dev);
_SDRPLAY_DLL_QUALIFIER sdrplay_api_ErrT sdrplay_api_Update(HANDLE dev, sdrplay_api_TunerSelectT tuner,
        sdrplay_api_ReasonForUpdateT reasonForUpdate,
        sdrplay_api_ReasonForUpdateExtension1T reasonForUpdateExt1);
_SDRPLAY_DLL_QUALIFIER sdrplay_api_ErrT sdrplay_api_SwapRspDuoActiveTuner(HANDLE dev,
        sdrplay_api_TunerSelectT *currentTuner, sdrplay_api_RspDuo_AmPortSelectT tuner1AmPortSel);
_SDRPLAY_DLL_QUALIFIER sdrplay_api_ErrT sdrplay_api_SwapRspDuoDualTunerModeSampleRate(HANDLE dev,
        double *currentSampleRate, double newSampleRate);
_SDRPLAY_DLL_QUALIFIER sdrplay_api_ErrT sdrplay_api_SwapRspDuoMode(sdrplay_api_DeviceT *currDevice,
        sdrplay_api_DeviceParamsT **deviceParams, sdrplay_api_RspDuoModeT rspDuoMode,
        double sampleRate, sdrplay_api_TunerSelectT tuner, sdrplay_api_Bw_MHzT bwType,
        sdrplay_api_If_kHzT ifType, sdrplay_api_RspDuo_AmPortSelectT tuner1AmPortSel);

#ifdef __cplusplus
}
#endif

#endif /* SDRPLAY_API_H */
