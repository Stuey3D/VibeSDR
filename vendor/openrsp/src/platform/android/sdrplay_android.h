// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the OpenRSP authors
//
// This file is part of OpenRSP.
//
// OpenRSP is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, either version 3
// of the License, or (at your option) any later version.
//
// OpenRSP is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with OpenRSP. If
// not, see <https://www.gnu.org/licenses/>.

/* Android embedding API of the open RSP API (C, callable from JNI or native apps).
 *
 * Android has no SDRplay daemon: the service core runs inside the app process and
 * the standard sdrplay_api_* C API (sdrplay_api.h) talks to it exactly as it talks to
 * sdrplay_apiService elsewhere. Apps cannot enumerate /dev/bus/usb, so USB devices
 * are INJECTED: the app gets permission through android.hardware.usb.UsbManager,
 * opens a UsbDeviceConnection and hands its file descriptor over. Hotplug comes from
 * the app's USB_DEVICE_ATTACHED / USB_DEVICE_DETACHED broadcasts, not from udev.
 *
 * Typical sequence:
 *   sdrplay_android_service_start(firmwareDir);        // once, before sdrplay_api_Open
 *   sdrplay_android_usb_attach(fd, "/dev/bus/usb/001/004");  // per granted device
 *   ... sdrplay_api_Open / GetDevices / SelectDevice / Init ...
 *   sdrplay_android_usb_detach("/dev/bus/usb/001/004", -1);  // on USB_DEVICE_DETACHED
 *   sdrplay_android_service_stop();                     // after sdrplay_api_Close
 *
 * Thread safety: every function may be called from any thread.
 */
#ifndef SDRPLAY_ANDROID_H
#define SDRPLAY_ANDROID_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define SDRPLAY_ANDROID_API __attribute__((visibility("default")))
#else
#define SDRPLAY_ANDROID_API
#endif

/* Registers a USB device opened by the app. `fd` is
 * UsbDeviceConnection.getFileDescriptor(). The library keeps its own dup() of it (and
 * one more per handle it opens), so the kernel device file stays open until the device
 * is detached AND every handle is closed, whatever the app does with its connection.
 * `deviceName` is UsbDevice.getDeviceName()
 * ("/dev/bus/usb/BBB/DDD", gives bus number and address; may be NULL).
 * Re-attaching the same deviceName replaces the old entry (the RSP re-enumerates after
 * its firmware upload, and the app then gets a NEW device and a new fd).
 * Returns 0, or a negative errno (-EBADF: fd unreadable / not a USB device). */
SDRPLAY_ANDROID_API int sdrplay_android_usb_attach(int fd, const char *deviceName);

/* Unregisters a device (by deviceName, or by the fd passed to attach when deviceName is NULL). Handles the
 * service still holds report LIBUSB_ERROR_NO_DEVICE from then on. 0, or -ENOENT. */
SDRPLAY_ANDROID_API int sdrplay_android_usb_detach(const char *deviceName, int fd);

/* Number of registered devices. */
SDRPLAY_ANDROID_API int sdrplay_android_usb_count(void);

/* Starts the embedded service (the sdrplay_apiService worker) on its own thread.
 * firmwareDir: directory holding, in own/, our firmware
 * images + manifest.sha256 (firmware/own; the default flavour is "own"), e.g.
 * copied to Context.getFilesDir()/firmware.
 * Returns 0 (also when already running), -1 if the service objects could not be
 * created (e.g. an object of the same name exists in this process) or the process is
 * attached to the OpenRSP service (sdrplay_android_client_mode() == 2). */
SDRPLAY_ANDROID_API int sdrplay_android_service_start(const char *firmwareDir);

/* Stops the embedded service and waits for it (devices released, objects unlinked).
 * Returns 0, or -1 if it was not running. */
SDRPLAY_ANDROID_API int sdrplay_android_service_stop(void);

/* 1 while the embedded service runs, else 0. */
SDRPLAY_ANDROID_API int sdrplay_android_service_running(void);

/* Which service the sdrplay_api_* client of this process talks to:
 * 0 none, 1 embedded (sdrplay_android_service_start in this process),
 * 2 service mode (attached to the OpenRSP app with org.openrsp.api.OpenRsp.attach).
 * sdrplay_android_service_start returns -1 while attached. */
SDRPLAY_ANDROID_API int sdrplay_android_client_mode(void);

/* OpenRSP Status panel (Android only, read-only): a JSON snapshot of the embedded service of
 * THIS process: {"running", "tableBusy", "numDevices", "devices":[{slot, serial, hwVer, pid,
 * rev, valid, tuner, rspDuoMode, bus, port, selected, prefix, params:{fsHz, decimation, ifType,
 * bwType, rfHz, lnaState, gRdB, agc, biasT, notches, ...}, stream:{bufIdx, firstSampleNum,
 * numSamples}}]}. "params" is the device's shared CmdMap (what the owning client set; it is
 * only applied by sdrplay_api_Init/Update), "stream" the newest data-ring slot header. Nothing
 * is written and no IPC message is sent. Writes at most size-1 bytes + NUL into buf and returns
 * the full length + 1 (call again with a bigger buffer if it is larger than size). */
SDRPLAY_ANDROID_API int sdrplay_android_status_json(char *buf, int size);

#ifdef __cplusplus
}
#endif

#endif /* SDRPLAY_ANDROID_H */
