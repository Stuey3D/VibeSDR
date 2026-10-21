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

// Android service mode, client side.
//
// org.openrsp.api.OpenRsp (Java) binds the OpenRSP app's ApiService and completes the HELLO
// handshake (approval) in Java, then hands the service IBinder to nativeAttach(). From then
// on every SharedMemory::open() / exists() of the client half of libsdrplay_api.so goes to
// the service over Binder (OPEN_OBJECT / EXISTS) instead of the in-process registry
// (AndroidShm.h RemoteResolver). After the bootstrap, client and service talk through the
// unchanged shared-memory protocol.
//
// libbinder_ndk.so exists from API 29 only, so it is dlopen()ed here: the library still
// loads on API 26-28, where embedded mode keeps working and attach reports
// UNSUPPORTED_API_LEVEL. The NDK binder declarations are repeated below (as function
// pointer types) because the NDK headers hide them below __ANDROID_API__ 29.
//
// Wire format of the hand-written interface "org.openrsp.api.IRspService" (no AIDL; the
// Java side is the OpenRSP app's ApiService):
//   2 OPEN_OBJECT  in: token, String name, long minSize
//                  out: int status (0 or errno), int hasFd, [ParcelFileDescriptor], long size
//   3 EXISTS       in: token, String name               out: int status, int exists
#include "sdrplay_android.h"
#include "AndroidShm.h"

#include <android/log.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <jni.h>
#include <mutex>
#include <string>
#include <unistd.h>

namespace {
using namespace sdrplay::platform;

const char *kTag = "sdrplay_api";
const char *kDescriptor = "org.openrsp.api.IRspService";
constexpr std::uint32_t kOpenObject = 2;
constexpr std::uint32_t kExists = 3;

// binder_status_t values (android/binder_status.h, hidden below API 29 like the rest).
constexpr std::int32_t kStatusDeadObject = -32;
constexpr std::int32_t kStatusInvalidOperation = -38;

// sdrplay_android_client_mode() results.
constexpr int kClientModeNone = 0;
constexpr int kClientModeEmbedded = 1;
constexpr int kClientModeService = 2;

// ---- NDK binder (libbinder_ndk.so, API 29+), resolved at run time
struct AIBinder;
struct AIBinder_Class;
struct AParcel;
struct AIBinder_DeathRecipient;
using binder_status_t = std::int32_t;

struct Ndk {
    void *lib = nullptr;
    AIBinder *(*fromJavaBinder)(JNIEnv *, jobject) = nullptr;
    AIBinder_Class *(*classDefine)(const char *, void *(*)(void *), void (*)(void *),
                                   binder_status_t (*)(AIBinder *, std::uint32_t, const AParcel *, AParcel *)) = nullptr;
    bool (*associateClass)(AIBinder *, const AIBinder_Class *) = nullptr;
    void (*incStrong)(AIBinder *) = nullptr;
    void (*decStrong)(AIBinder *) = nullptr;
    bool (*isAlive)(const AIBinder *) = nullptr;
    binder_status_t (*prepareTransaction)(AIBinder *, AParcel **) = nullptr;
    binder_status_t (*transact)(AIBinder *, std::uint32_t, AParcel **, AParcel **, std::uint32_t) = nullptr;
    void (*parcelDelete)(AParcel *) = nullptr;
    binder_status_t (*writeInt64)(AParcel *, std::int64_t) = nullptr;
    binder_status_t (*writeString)(AParcel *, const char *, std::int32_t) = nullptr;
    binder_status_t (*readInt32)(const AParcel *, std::int32_t *) = nullptr;
    binder_status_t (*readInt64)(const AParcel *, std::int64_t *) = nullptr;
    binder_status_t (*readParcelFileDescriptor)(const AParcel *, int *) = nullptr;
    AIBinder_DeathRecipient *(*deathRecipientNew)(void (*)(void *)) = nullptr;
    binder_status_t (*linkToDeath)(AIBinder *, AIBinder_DeathRecipient *, void *) = nullptr;
    binder_status_t (*unlinkToDeath)(AIBinder *, AIBinder_DeathRecipient *, void *) = nullptr;
    AIBinder_Class *cls = nullptr;
    AIBinder_DeathRecipient *recipient = nullptr;
};

void *dummyCreate(void *args) { return args; }
void dummyDestroy(void *) {}
binder_status_t dummyTransact(AIBinder *, std::uint32_t, const AParcel *, AParcel *) { return kStatusInvalidOperation; }

template <typename T> bool sym(void *lib, T &out, const char *name)
{
    out = reinterpret_cast<T>(dlsym(lib, name));
    return out != nullptr;
}

struct Session {
    std::mutex m;             // serialises attach/detach
    AIBinder *binder = nullptr;
    std::atomic<bool> alive{false};
    std::atomic<std::uint64_t> generation{0}; // bumped per attach (death cookies of old sessions are ignored)
};

Session &session()
{
    static Session *s = new Session(); // never destroyed
    return *s;
}

void onServiceDied(void *cookie)
{
    Session &s = session();
    if (reinterpret_cast<std::uintptr_t>(cookie) == s.generation.load()) {
        s.alive.store(false);
        __android_log_write(ANDROID_LOG_WARN, kTag, "OpenRSP service died: service-mode objects are gone");
    }
}

Ndk *ndk(std::string *why)
{
    static Ndk *n = nullptr;
    static std::once_flag once;
    static std::string failure;
    std::call_once(once, [] {
        auto *x = new Ndk();
        x->lib = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_LOCAL);
        if (!x->lib) {
            failure = "libbinder_ndk.so not available (API < 29)";
            return;
        }
        bool ok = sym(x->lib, x->fromJavaBinder, "AIBinder_fromJavaBinder") &&
                  sym(x->lib, x->classDefine, "AIBinder_Class_define") &&
                  sym(x->lib, x->associateClass, "AIBinder_associateClass") &&
                  sym(x->lib, x->incStrong, "AIBinder_incStrong") &&
                  sym(x->lib, x->decStrong, "AIBinder_decStrong") && sym(x->lib, x->isAlive, "AIBinder_isAlive") &&
                  sym(x->lib, x->prepareTransaction, "AIBinder_prepareTransaction") &&
                  sym(x->lib, x->transact, "AIBinder_transact") && sym(x->lib, x->parcelDelete, "AParcel_delete") &&
                  sym(x->lib, x->writeInt64, "AParcel_writeInt64") &&
                  sym(x->lib, x->writeString, "AParcel_writeString") &&
                  sym(x->lib, x->readInt32, "AParcel_readInt32") && sym(x->lib, x->readInt64, "AParcel_readInt64") &&
                  sym(x->lib, x->readParcelFileDescriptor, "AParcel_readParcelFileDescriptor") &&
                  sym(x->lib, x->deathRecipientNew, "AIBinder_DeathRecipient_new") &&
                  sym(x->lib, x->linkToDeath, "AIBinder_linkToDeath") &&
                  sym(x->lib, x->unlinkToDeath, "AIBinder_unlinkToDeath");
        if (!ok) {
            failure = "libbinder_ndk.so lacks a needed symbol";
            return;
        }
        x->cls = x->classDefine(kDescriptor, dummyCreate, dummyDestroy, dummyTransact);
        x->recipient = x->deathRecipientNew(onServiceDied);
        if (!x->cls || !x->recipient) {
            failure = "AIBinder_Class_define/DeathRecipient_new failed";
            return;
        }
        n = x;
    });
    if (!n && why) *why = failure;
    return n;
}

// One transaction: prepare (writes the interface token), fill, transact, hand the reply to
// `read`. Returns false when the service is unreachable.
template <typename Fill, typename Read> bool call(std::uint32_t code, Fill fill, Read read)
{
    Ndk *n = ndk(nullptr);
    Session &s = session();
    if (!n || !s.alive.load()) return false;
    AIBinder *b;
    {
        // Own a reference for the whole call: a concurrent detach (OpenRsp.detach on another
        // thread) drops the session's reference and must not free the binder under us.
        std::lock_guard<std::mutex> lk(s.m);
        b = s.binder;
        if (b) n->incStrong(b);
    }
    if (!b) return false;
    struct Ref {
        Ndk *n;
        AIBinder *b;
        ~Ref() { n->decStrong(b); }
    } ref{n, b};
    AParcel *in = nullptr, *out = nullptr;
    if (n->prepareTransaction(b, &in) != 0 || !in) return false;
    if (!fill(n, in)) {
        n->parcelDelete(in);
        return false;
    }
    const binder_status_t st = n->transact(b, code, &in, &out, 0); // takes `in`
    if (st != 0 || !out) {
        if (out) n->parcelDelete(out);
        if (st == kStatusDeadObject) {
            std::lock_guard<std::mutex> lk(s.m);
            if (s.binder == b) s.alive.store(false); // not a session attached since
        }
        return false;
    }
    const bool ok = read(n, out);
    n->parcelDelete(out);
    return ok;
}

int remoteOpen(void *, const std::string &name, std::size_t minSize, int *err)
{
    std::int32_t status = EPIPE, hasFd = 0;
    int fd = -1;
    const bool ok = call(
        kOpenObject,
        [&](Ndk *n, AParcel *in) {
            return n->writeString(in, name.c_str(), static_cast<std::int32_t>(name.size())) == 0 &&
                   n->writeInt64(in, static_cast<std::int64_t>(minSize)) == 0;
        },
        [&](Ndk *n, AParcel *out) {
            if (n->readInt32(out, &status) != 0 || n->readInt32(out, &hasFd) != 0) return false;
            if (hasFd && n->readParcelFileDescriptor(out, &fd) != 0) return false;
            std::int64_t size = 0;
            n->readInt64(out, &size);
            return true;
        });
    if (!ok) {
        if (fd >= 0) close(fd);
        if (err) *err = EPIPE;
        return -1;
    }
    if (status != 0 || fd < 0) {
        if (fd >= 0) close(fd);
        if (err) *err = status != 0 ? status : ENOENT;
        return -1;
    }
    return fd;
}

bool remoteExists(void *, const std::string &name)
{
    std::int32_t status = -1, exists = 0;
    const bool ok = call(
        kExists,
        [&](Ndk *n, AParcel *in) {
            return n->writeString(in, name.c_str(), static_cast<std::int32_t>(name.size())) == 0;
        },
        [&](Ndk *n, AParcel *out) { return n->readInt32(out, &status) == 0 && n->readInt32(out, &exists) == 0; });
    return ok && status == 0 && exists != 0; // a dead service: false (serviceGone() -> true)
}

const android_shm::RemoteResolver kResolver = {nullptr, remoteOpen, remoteExists};

void detachLocked(Session &s, Ndk *n)
{
    android_shm::setRemoteResolver(nullptr);
    s.alive.store(false);
    if (s.binder && n) {
        n->unlinkToDeath(s.binder, n->recipient, reinterpret_cast<void *>(static_cast<std::uintptr_t>(s.generation.load())));
        n->decStrong(s.binder);
    }
    s.binder = nullptr;
}
} // namespace

// Status codes shared with org.openrsp.api.OpenRsp (Java).
enum : int {
    kAttachOk = 0,
    kAttachUnsupportedApiLevel = -2,
    kAttachEmbeddedRunning = 4, // SERVICE_START_FAILED: this process runs the embedded service
    kAttachNativeError = -4,
};

extern "C" SDRPLAY_ANDROID_API int sdrplay_android_client_mode(void)
{
    if (android_shm::remoteResolverSet()) return kClientModeService;
    return sdrplay_android_service_running() ? kClientModeEmbedded : kClientModeNone;
}

extern "C" JNIEXPORT jint JNICALL Java_org_openrsp_api_OpenRsp_nativeAttach(JNIEnv *env, jclass, jobject binder)
{
    std::string why;
    Ndk *n = ndk(&why);
    if (!n) {
        __android_log_write(ANDROID_LOG_WARN, kTag, ("OpenRSP attach: " + why).c_str());
        return kAttachUnsupportedApiLevel;
    }
    if (sdrplay_android_service_running()) {
        __android_log_write(ANDROID_LOG_ERROR, kTag, "OpenRSP attach: this process runs the embedded service");
        return kAttachEmbeddedRunning;
    }
    Session &s = session();
    std::lock_guard<std::mutex> lk(s.m);
    detachLocked(s, n);
    AIBinder *b = binder ? n->fromJavaBinder(env, binder) : nullptr;
    if (!b) return kAttachNativeError;
    if (!n->associateClass(b, n->cls)) {
        __android_log_write(ANDROID_LOG_ERROR, kTag, "OpenRSP attach: binder is not an org.openrsp.api.IRspService");
        n->decStrong(b);
        return kAttachNativeError;
    }
    const std::uint64_t gen = s.generation.fetch_add(1) + 1;
    if (n->linkToDeath(b, n->recipient, reinterpret_cast<void *>(static_cast<std::uintptr_t>(gen))) != 0 ||
        !n->isAlive(b)) {
        n->decStrong(b);
        return kAttachNativeError;
    }
    s.binder = b;
    s.alive.store(true);
    android_shm::setRemoteResolver(&kResolver);
    __android_log_write(ANDROID_LOG_INFO, kTag, "OpenRSP attach: service mode");
    return kAttachOk;
}

extern "C" JNIEXPORT void JNICALL Java_org_openrsp_api_OpenRsp_nativeDetach(JNIEnv *, jclass)
{
    Session &s = session();
    std::lock_guard<std::mutex> lk(s.m);
    detachLocked(s, ndk(nullptr));
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_openrsp_api_OpenRsp_nativeServiceAlive(JNIEnv *, jclass)
{
    return android_shm::remoteResolverSet() && session().alive.load() ? JNI_TRUE : JNI_FALSE;
}

// Test hook (OpenRsp.debugOpenObject): OPEN_OBJECT for `name` through the attached service;
// returns 0 or the errno the service answered (EPERM before HELLO, ENOENT, ...).
extern "C" JNIEXPORT jint JNICALL Java_org_openrsp_api_OpenRsp_nativeProbeObject(JNIEnv *env, jclass, jstring jname)
{
    if (!jname) return EINVAL;
    const char *c = env->GetStringUTFChars(jname, nullptr);
    if (!c) return ENOMEM;
    const std::string name(c);
    env->ReleaseStringUTFChars(jname, c);
    int err = 0;
    const int fd = remoteOpen(nullptr, name, 1, &err);
    if (fd >= 0) {
        close(fd);
        return 0;
    }
    return err;
}
