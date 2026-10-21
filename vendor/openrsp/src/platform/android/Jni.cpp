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

// JNI bindings of the Android embedding API (sdrplay_android.h) for the Java class
//   package org.openrspapi;  public final class RspNative { ... }
// The Java side handles the UsbManager permission, the fds and the broadcasts.
#include "sdrplay_android.h"
#include "AndroidShm.h"

#include <cerrno>
#include <jni.h>
#include <string>

namespace {
std::string str(JNIEnv *env, jstring s)
{
    if (!s) return std::string();
    const char *c = env->GetStringUTFChars(s, nullptr);
    if (!c) return std::string();
    std::string r(c);
    env->ReleaseStringUTFChars(s, c);
    return r;
}
} // namespace

extern "C" {

JNIEXPORT jint JNICALL Java_org_openrspapi_RspNative_serviceStart(JNIEnv *env, jclass, jstring firmwareDir)
{
    const std::string dir = str(env, firmwareDir);
    return sdrplay_android_service_start(dir.c_str());
}

JNIEXPORT jint JNICALL Java_org_openrspapi_RspNative_serviceStop(JNIEnv *, jclass)
{
    return sdrplay_android_service_stop();
}

JNIEXPORT jboolean JNICALL Java_org_openrspapi_RspNative_serviceRunning(JNIEnv *, jclass)
{
    return sdrplay_android_service_running() ? JNI_TRUE : JNI_FALSE;
}

// fd = UsbDeviceConnection.getFileDescriptor(), deviceName = UsbDevice.getDeviceName().
JNIEXPORT jint JNICALL Java_org_openrspapi_RspNative_usbAttach(JNIEnv *env, jclass, jint fd, jstring deviceName)
{
    const std::string name = str(env, deviceName);
    return sdrplay_android_usb_attach(fd, deviceName ? name.c_str() : nullptr);
}

JNIEXPORT jint JNICALL Java_org_openrspapi_RspNative_usbDetach(JNIEnv *env, jclass, jstring deviceName)
{
    const std::string name = str(env, deviceName);
    return sdrplay_android_usb_detach(name.c_str(), -1);
}

JNIEXPORT jint JNICALL Java_org_openrspapi_RspNative_usbCount(JNIEnv *, jclass)
{
    return sdrplay_android_usb_count();
}

// ---- service mode, OpenRSP side: the app's ApiService
// serves the objects of its embedded service to approved client apps.
namespace sh = sdrplay::platform::android_shm;

// A NEW fd for the named object of this process's registry (the caller hands it to
// ParcelFileDescriptor.adoptFd), or -errno (-ENOENT, -EINVAL: minSize beyond the object).
JNIEXPORT jint JNICALL Java_org_openrspapi_RspNative_openObjectFd(JNIEnv *env, jclass, jstring name, jlong minSize)
{
    if (!name || minSize < 0) return -EINVAL;
    int err = 0;
    const int fd = sh::registryOpen(str(env, name), static_cast<std::size_t>(minSize), &err);
    return fd >= 0 ? fd : -(err != 0 ? err : ENOENT);
}

JNIEXPORT jboolean JNICALL Java_org_openrspapi_RspNative_objectExists(JNIEnv *env, jclass, jstring name)
{
    return name && sh::registryExists(str(env, name)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL Java_org_openrspapi_RspNative_objectGeneration(JNIEnv *env, jclass, jstring name)
{
    return name ? static_cast<jlong>(sh::registryGeneration(str(env, name))) : 0;
}

JNIEXPORT jlong JNICALL Java_org_openrspapi_RspNative_maxGeneration(JNIEnv *env, jclass, jstring prefix)
{
    return prefix ? static_cast<jlong>(sh::registryMaxGeneration(str(env, prefix))) : 0;
}

// Releases every mutex held by dead client process `pid` (Binder death notification).
JNIEXPORT jint JNICALL Java_org_openrspapi_RspNative_recoverDeadOwner(JNIEnv *, jclass, jint pid)
{
    return sh::recoverDeadOwner(pid);
}

JNIEXPORT jint JNICALL Java_org_openrspapi_RspNative_layoutVersion(JNIEnv *, jclass)
{
    return static_cast<jint>(sh::kAndroidLayoutVersion);
}

// Read-only status snapshot for the OpenRSP Status panel (sdrplay_android_status_json).
JNIEXPORT jstring JNICALL Java_org_openrspapi_RspNative_statusJson(JNIEnv *env, jclass)
{
    std::string buf(8192, '\0');
    int need = sdrplay_android_status_json(&buf[0], static_cast<int>(buf.size()));
    if (need > static_cast<int>(buf.size())) {
        buf.assign(static_cast<std::size_t>(need) + 1024, '\0');
        sdrplay_android_status_json(&buf[0], static_cast<int>(buf.size()));
    }
    return env->NewStringUTF(buf.c_str());
}

} // extern "C"
