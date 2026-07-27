// VibeSDR V4 — local-SDR shim JNI.
//
// Stage 1 proved the SDR++ Brown core + RTL-SDR driver build/link into the APK.
// Stage 2 opens an RTL-SDR over a USB file descriptor handed down from Kotlin
// (which owns the Android USB permission flow) and probes it via librtlsdr's
// rtlsdr_open_sys_dev(fd). Later stages add the real localhost UberSDR shim
// (IQ → FFT/SPEC → Opus audio).

#include <jni.h>
#include <android/log.h>
#include <string>
#include <thread>
#include <vector>
#include <rtl-sdr.h>
#include "local_sdr_shim.h"
#include "rtl_tcp_server.h"

#define LOG_TAG "VibeLocalSDR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static const char* tunerName(enum rtlsdr_tuner t) {
    switch (t) {
        case RTLSDR_TUNER_E4000:  return "E4000";
        case RTLSDR_TUNER_FC0012: return "FC0012";
        case RTLSDR_TUNER_FC0013: return "FC0013";
        case RTLSDR_TUNER_FC2580: return "FC2580";
        case RTLSDR_TUNER_R820T:  return "R820T";
        case RTLSDR_TUNER_R828D:  return "R828D";
        default:                  return "unknown";
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeHello(JNIEnv* env, jobject /*thiz*/) {
    LOGI("native shim loaded (SDR++ Brown core + librtlsdr linked)");
    return env->NewStringUTF("VibeSDR local-SDR shim: SDR++ Brown core + rtl_sdr linked");
}

// Open an RTL-SDR from a USB fd (owned by the Kotlin UsbDeviceConnection) and
// return a human-readable description. Returns a string starting with "ERROR:"
// on failure. The fd stays owned by Kotlin; we close only the rtlsdr handle.
extern "C" JNIEXPORT jstring JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeProbeRtl(JNIEnv* env, jobject /*thiz*/,
                                                 jint fd, jint vid, jint pid) {
    LOGI("probing RTL-SDR: fd=%d vid=0x%04x pid=0x%04x", fd, vid, pid);

    rtlsdr_dev_t* dev = nullptr;
    int ret = rtlsdr_open_sys_dev(&dev, (intptr_t)fd);
    if (ret != 0 || dev == nullptr) {
        LOGE("rtlsdr_open_sys_dev failed: %d", ret);
        std::string err = "ERROR: rtlsdr_open_sys_dev failed (" + std::to_string(ret) + ")";
        return env->NewStringUTF(err.c_str());
    }

    enum rtlsdr_tuner tuner = rtlsdr_get_tuner_type(dev);

    char manufact[256] = {0}, product[256] = {0}, serial[256] = {0};
    rtlsdr_get_usb_strings(dev, manufact, product, serial);

    std::string desc = std::string("RTL-SDR opened: ")
        + (manufact[0] ? manufact : "?") + " "
        + (product[0]  ? product  : "?")
        + " [sn:" + (serial[0] ? serial : "?") + "]"
        + " tuner=" + tunerName(tuner);
    LOGI("%s", desc.c_str());

    rtlsdr_close(dev);
    return env->NewStringUTF(desc.c_str());
}

// Start the local-SDR spectrum pipeline + localhost UberSDR server.
// Returns the bound TCP port (>0), or -1 on failure (check logcat).
extern "C" JNIEXPORT jint JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeStartSpectrum(
        JNIEnv* env, jobject /*thiz*/, jint fd, jint vid, jint pid,
        jdouble centerFreq, jdouble sampleRate, jint gainTenthDb,
        jint fftSize, jdouble fftRate, jstring mode) {
    const char* modeC = mode ? env->GetStringUTFChars(mode, nullptr) : "";
    std::string modeS = modeC ? modeC : "";
    if (mode && modeC) env->ReleaseStringUTFChars(mode, modeC);
    std::string err;
    int port = vibe::LocalSdrShim::instance().start(
        fd, vid, pid, centerFreq, sampleRate, gainTenthDb, fftSize, fftRate, modeS, err);
    if (port < 0) LOGE("startSpectrum failed: %s", err.c_str());
    return port;
}

// RTL-TCP: IQ from an rtl_tcp server (host:port) instead of a USB fd. Same return
// contract as nativeStartSpectrum (bound localhost port, or -1).
extern "C" JNIEXPORT jint JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeStartTcp(
        JNIEnv* env, jobject /*thiz*/, jstring host, jint port,
        jdouble centerFreq, jdouble sampleRate, jint gainTenthDb,
        jint fftSize, jdouble fftRate, jstring mode) {
    const char* hostC = host ? env->GetStringUTFChars(host, nullptr) : "";
    std::string hostS = hostC ? hostC : "";
    if (host && hostC) env->ReleaseStringUTFChars(host, hostC);
    const char* modeC = mode ? env->GetStringUTFChars(mode, nullptr) : "";
    std::string modeS = modeC ? modeC : "";
    if (mode && modeC) env->ReleaseStringUTFChars(mode, modeC);
    std::string err;
    int bound = vibe::LocalSdrShim::instance().startTcp(
        hostS, port, centerFreq, sampleRate, gainTenthDb, fftSize, fftRate, modeS, err);
    if (bound < 0) LOGE("startTcp failed: %s", err.c_str());
    return bound;
}

// SpyServer: IQ from a SpyServer-compatible server. Same return contract as
// nativeStartTcp (bound localhost port, or -1).
extern "C" JNIEXPORT jint JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeStartSpyServer(
        JNIEnv* env, jobject /*thiz*/, jstring host, jint port,
        jdouble centerFreq, jdouble sampleRate, jint gainTenthDb,
        jint fftSize, jdouble fftRate, jstring mode) {
    const char* hostC = host ? env->GetStringUTFChars(host, nullptr) : "";
    std::string hostS = hostC ? hostC : "";
    if (host && hostC) env->ReleaseStringUTFChars(host, hostC);
    const char* modeC = mode ? env->GetStringUTFChars(mode, nullptr) : "";
    std::string modeS = modeC ? modeC : "";
    if (mode && modeC) env->ReleaseStringUTFChars(mode, modeC);
    std::string err;
    int bound = vibe::LocalSdrShim::instance().startSpyServer(
        hostS, port, centerFreq, sampleRate, gainTenthDb, fftSize, fftRate, modeS, err);
    if (bound < 0) LOGE("startSpyServer failed: %s", err.c_str());
    return bound;
}

// VibeServer: bind the shim's WS server to the LAN. Must be called BEFORE
// nativeStartSpectrum. Off by default — it exposes a tuning-control channel.
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetServeOnLan(JNIEnv*, jobject, jboolean on) {
    vibe::LocalSdrShim::setServeOnLan(on);
}

// VibeServer PIN. Empty secret = open access (no PIN). Call BEFORE start().
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetVibeServerAuth(JNIEnv* env, jobject, jstring secret) {
    const char* s = secret ? env->GetStringUTFChars(secret, nullptr) : nullptr;
    vibe::LocalSdrShim::setVibeServerAuth(s ? s : "");
    if (secret && s) env->ReleaseStringUTFChars(secret, s);
}

// ★ ADMIN PASSWORD — a second secret, gating CONTROL rather than ACCESS. The PIN decides who
// may LISTEN; this decides who may touch bias-T, direct sampling and calibration. Independent
// of the PIN on purpose: a public receiver can be open to every listener and still refuse a
// visitor putting DC on the feedline. Empty = nothing is protected.
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetVibeServerAdminSecret(JNIEnv* env, jobject, jstring secret) {
    const char* s = secret ? env->GetStringUTFChars(secret, nullptr) : nullptr;
    vibe::LocalSdrShim::setVibeServerAdminSecret(s ? s : "");
    if (secret && s) env->ReleaseStringUTFChars(secret, s);
}

// Uncompressed audio policy: 0 = off, 1 = listener's choice, 2 = compatibility fallback only.
// ★ Loopback is outside this setting entirely — it rations the owner's UPLINK, and 127.0.0.1
// does not touch it.
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetVibeServerUncompressedAudio(JNIEnv*, jobject, jint mode) {
    vibe::LocalSdrShim::setVibeServerUncompressedAudio((int)mode);
}

// Per-listener time limit, minutes. 0 = unlimited. Loopback and admin sessions exempt.
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetVibeServerSessionLimit(JNIEnv*, jobject, jint minutes) {
    vibe::LocalSdrShim::setVibeServerSessionLimit((int)minutes);
}

// VibeServer compatibility limits. <=0 = no cap / server default. BEFORE start().
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetVibeServerLimits(JNIEnv*, jobject,
                                                            jdouble maxBwHz, jdouble maxFftRate) {
    vibe::LocalSdrShim::setVibeServerLimits(maxBwHz, maxFftRate);
}

extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetVibeServerCompressAudio(JNIEnv*, jobject, jboolean on) {
    vibe::LocalSdrShim::setVibeServerCompressAudio(on);
}

extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetVibeServerWebEnabled(JNIEnv*, jobject, jboolean on) {
    vibe::LocalSdrShim::setVibeServerWebEnabled(on);
}

extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetVibeServerLockedRate(JNIEnv*, jobject, jdouble rate) {
    vibe::LocalSdrShim::setVibeServerLockedRate(rate);
}

// Learned RDS station bookmarks. The shim LEARNS them (it is the only place that sees
// both the tuned frequency and the decoded name); the app PERSISTS them.
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetBookmarksJson(JNIEnv* env, jobject, jstring json) {
    const char* c = env->GetStringUTFChars(json, nullptr);
    vibe::LocalSdrShim::setBookmarksJson(c ? c : "");
    if (c) env->ReleaseStringUTFChars(json, c);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeGetBookmarksJson(JNIEnv* env, jobject) {
    return env->NewStringUTF(vibe::LocalSdrShim::getBookmarksJson().c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeClearBookmarks(JNIEnv*, jobject) {
    vibe::LocalSdrShim::clearBookmarks();
}

// The shim OWNS the bookmarks, so it persists them itself. The app's JS could not: it is
// backgrounded whenever the server is actually serving, and its timers are suspended
// there, so an import lived in memory and died at the next restart.
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetBookmarksPath(JNIEnv* env, jobject, jstring path) {
    const char* c = env->GetStringUTFChars(path, nullptr);
    vibe::LocalSdrShim::setBookmarksPath(c ? c : "");
    if (c) env->ReleaseStringUTFChars(path, c);
}

// mDNS hostname responder: "vibesdr.local" in a browser. NsdManager publishes a SERVICE
// (what the app's Discovered list uses) but cannot publish a hostname A record, which is
// what resolving a name in a browser actually needs.
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeStartMdns(JNIEnv* env, jobject, jstring host, jstring ip) {
    const char* h = env->GetStringUTFChars(host, nullptr);
    const char* i = env->GetStringUTFChars(ip, nullptr);
    vibe::LocalSdrShim::startMdns(h ? h : "", i ? i : "");
    if (h) env->ReleaseStringUTFChars(host, h);
    if (i) env->ReleaseStringUTFChars(ip, i);
}

extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeStopMdns(JNIEnv*, jobject) {
    vibe::LocalSdrShim::stopMdns();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeMdnsHostname(JNIEnv* env, jobject) {
    return env->NewStringUTF(vibe::LocalSdrShim::mdnsHostname().c_str());
}

// Station list (JSON array) for the web client's search, served at GET /stations.
// The APP supplies it because it already downloads + caches EiBi — and a browser
// cannot fetch eibispace.de itself (no CORS headers), unlike React Native.
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetStationsJson(JNIEnv* env, jobject, jstring json) {
    const char* s = json ? env->GetStringUTFChars(json, nullptr) : nullptr;
    vibe::LocalSdrShim::setStationsJson(s ? s : "");
    if (json && s) env->ReleaseStringUTFChars(json, s);
}

// The RECEIVER's coarse location, served at GET /location.
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetLocationJson(JNIEnv* env, jobject, jstring json) {
    const char* s = json ? env->GetStringUTFChars(json, nullptr) : nullptr;
    vibe::LocalSdrShim::setLocationJson(s ? s : "");
    if (json && s) env->ReleaseStringUTFChars(json, s);
}

extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeStopSpectrum(JNIEnv* /*env*/, jobject /*thiz*/) {
    // Tear down on a detached thread so the JS/bridge caller never blocks if the
    // teardown is slow (RTL cancel + thread joins) — the app must not lock up
    // when leaving a local session. stop() is serialised internally (g_lifecycle).
    std::thread([]{ vibe::LocalSdrShim::instance().stop(); }).detach();
}

extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetGain(JNIEnv*, jobject, jint g) {
    vibe::LocalSdrShim::instance().setGain(g);
}
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetPpm(JNIEnv*, jobject, jint ppm) {
    vibe::LocalSdrShim::instance().setPpm(ppm);
}
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetBiasTee(JNIEnv*, jobject, jboolean on) {
    vibe::LocalSdrShim::instance().setBiasTee(on);
}
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetAgc(JNIEnv*, jobject, jboolean on) {
    vibe::LocalSdrShim::instance().setAgc(on);
}
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetDirectSampling(JNIEnv*, jobject, jint mode) {
    vibe::LocalSdrShim::instance().setDirectSampling(mode);
}
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetSampleRate(JNIEnv*, jobject, jdouble rate) {
    vibe::LocalSdrShim::instance().setSampleRate(rate);
}
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetDeemphasis(JNIEnv*, jobject, jdouble tau) {
    vibe::LocalSdrShim::instance().setDeemphasis(tau);
}
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetSquelch(JNIEnv*, jobject, jboolean on, jfloat db) {
    vibe::LocalSdrShim::instance().setSquelch(on, db);
}
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetNR(JNIEnv*, jobject, jboolean on) {
    vibe::LocalSdrShim::instance().setNR(on);
}
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetNrStrength(JNIEnv*, jobject, jfloat s) {
    vibe::LocalSdrShim::instance().setNrStrength(s);
}
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetNotch(JNIEnv*, jobject, jboolean on) {
    vibe::LocalSdrShim::instance().setNotch(on);
}
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetStereoEnabled(JNIEnv*, jobject, jboolean on) {
    vibe::LocalSdrShim::instance().setStereoEnabled(on);
}
extern "C" JNIEXPORT jfloat JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeGetNrCpu(JNIEnv*, jobject) {
    return vibe::LocalSdrShim::instance().getNrCpu();
}
// ── Decoder-only sidecar (Kiwi/OWRX): decode the backend's audio natively ────
extern "C" JNIEXPORT jint JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeStartDecoderService(JNIEnv* env, jobject) {
    std::string err;
    int port = vibe::LocalSdrShim::instance().startDecoderService(err);
    if (port < 0) LOGE("startDecoderService: %s", err.c_str());
    return port;
}
// PCM is base64-encoded int16 LE (same form JS already builds for pushExternalPcm).
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeFeedDecoderPcm(JNIEnv* env, jobject, jstring b64, jint rate) {
    if (!b64) return;
    const char* s = env->GetStringUTFChars(b64, nullptr);
    if (!s) return;
    // Inline base64 decode (RFC 4648).
    auto dval = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> bytes;
    int val = 0, bits = 0;
    for (const char* q = s; *q; q++) {
        int d = dval(*q);
        if (d < 0) continue;
        val = (val << 6) | d; bits += 6;
        if (bits >= 8) { bits -= 8; bytes.push_back((uint8_t)((val >> bits) & 0xFF)); }
    }
    (void)val;
    env->ReleaseStringUTFChars(b64, s);
    int n = (int)(bytes.size() / 2);
    if (n < 2) return;
    vibe::LocalSdrShim::instance().feedDecoderPcm((const int16_t*)bytes.data(), n, (int)rate);
}

extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetDecoderFreq(JNIEnv*, jobject, jdouble hz) {
    vibe::LocalSdrShim::instance().setDecoderFreq((double)hz);
}

// ── RTL-TCP SERVER (share this device's USB dongle over the network) ─────────
extern "C" JNIEXPORT jint JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeStartServer(
        JNIEnv* /*env*/, jobject, jint fd, jint vid, jint pid,
        jdouble sampleRate, jdouble centerFreq, jint gainTenthDb,
        jint port, jdouble overrideRate) {
    std::string err;
    int bound = vibe::RtlTcpServer::instance().start(
        fd, vid, pid, (uint32_t)sampleRate, (uint32_t)centerFreq, gainTenthDb,
        port, (uint32_t)overrideRate, err);
    if (bound < 0) LOGE("startServer failed: %s", err.c_str());
    return bound;
}

extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeStopServer(JNIEnv*, jobject) {
    // Detached like nativeStopSpectrum so the JS/bridge caller never blocks on the
    // teardown (socket closes + thread joins).
    std::thread([]{ vibe::RtlTcpServer::instance().stop(); }).detach();
}

extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeSetServerSampleRate(JNIEnv*, jobject, jdouble rate) {
    vibe::RtlTcpServer::instance().setSampleRateOverride((uint32_t)rate);
}

// Returns a small JSON status string for the UI + notification.
extern "C" JNIEXPORT jstring JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeGetServerStatus(JNIEnv* env, jobject) {
    auto s = vibe::RtlTcpServer::instance().getStatus();
    std::string j = "{";
    j += "\"running\":"       + std::string(s.running ? "true" : "false");
    j += ",\"client\":"       + std::string(s.clientConnected ? "true" : "false");
    j += ",\"clientAddr\":\"" + s.clientAddr + "\"";
    j += ",\"sampleRate\":"   + std::to_string(s.sampleRate);
    j += ",\"overrideRate\":" + std::to_string(s.overrideRate);
    j += ",\"droppedBytes\":" + std::to_string(s.droppedBytes);
    j += ",\"port\":"         + std::to_string(s.port);
    j += "}";
    return env->NewStringUTF(j.c_str());
}

// VibeServer live status: client presence + SEPARATE spectrum/audio byte rates.
extern "C" JNIEXPORT jstring JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeGetVibeServerStatus(JNIEnv* env, jobject) {
    auto s = vibe::LocalSdrShim::instance().getVibeServerStatus();
    std::string j = "{";
    j += "\"running\":"          + std::string(s.running ? "true" : "false");
    j += ",\"client\":"          + std::string(s.clientConnected ? "true" : "false");
    j += ",\"clientAddr\":\""    + s.clientAddr + "\"";
    j += ",\"specBytesPerSec\":" + std::to_string((long long)(s.specBytesPerSec + 0.5));
    j += ",\"audioBytesPerSec\":"+ std::to_string((long long)(s.audioBytesPerSec + 0.5));
    j += ",\"compressed\":"      + std::string(s.compressed ? "true" : "false");
    j += ",\"pinEnabled\":"      + std::string(s.pinEnabled ? "true" : "false");
    j += ",\"fftRate\":"         + std::to_string((long long)(s.fftRate + 0.5));
    j += ",\"bandwidthHz\":"     + std::to_string((long long)(s.bandwidthHz + 0.5));
    j += ",\"sampleRate\":"      + std::to_string((long long)(s.sampleRate + 0.5));
    j += "}";
    return env->NewStringUTF(j.c_str());
}

// rtl_tcp CLIENT link health (jitter buffer). JSON, like the server status above.
extern "C" JNIEXPORT jstring JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeGetNetStatus(JNIEnv* env, jobject) {
    auto s = vibe::LocalSdrShim::instance().getNetStatus();
    std::string j = "{";
    j += "\"tcp\":"             + std::string(s.tcp ? "true" : "false");
    j += ",\"stalls\":"         + std::to_string(s.stalls);
    j += ",\"droppedSamples\":" + std::to_string(s.droppedSamples);
    j += ",\"bufferedMs\":"     + std::to_string(s.bufferedMs);
    j += ",\"spy\":"            + std::string(s.spy ? "true" : "false");
    j += ",\"canControl\":"     + std::string(s.canControl ? "true" : "false");
    j += ",\"closed\":"         + std::string(s.closed ? "true" : "false");
    j += "}";
    return env->NewStringUTF(j.c_str());
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeGetTunerGains(JNIEnv* env, jobject) {
    auto gains = vibe::LocalSdrShim::instance().getTunerGains();
    jintArray arr = env->NewIntArray((jsize)gains.size());
    if (arr && !gains.empty()) env->SetIntArrayRegion(arr, 0, (jsize)gains.size(), gains.data());
    return arr;
}

// ── Opus decode, for the LOCAL AUDIO PUMP ────────────────────────────────────
//
// ★★★ WHY THIS EXISTS. The audio frame header's format byte is 0=raw PCM, 1/2=IMA-ADPCM,
// 3=Opus. The Kotlin pump handled 1 and 2 and let EVERYTHING ELSE fall through to "treat the
// payload as int16 PCM" — which was true when the only other case was raw. Opus then arrived
// as a third case and was played as though its compressed bytes were samples: a loud buzz, on
// every local radio on Android, with a perfectly normal spectrum beside it (Stuart, 2026-07-27).
// ★★ THE SAME SHAPE AS THE "else means dongle" FAMILY: a two-case test whose `else` silently
// means "the other one" mis-handles the third case rather than rejecting it.
//
// ★ iOS decodes Opus in its own audio engine (pushExternalOpus); Android had NO decoder at all,
// even though libopus is already linked into this library for the ENCODER. So this is a decode
// entry point next to an encoder we already ship, not a new dependency.
//
// ★ The decoder is STATEFUL and must persist across packets — Opus carries prediction between
// frames, so a fresh decoder per packet would produce a click at every frame boundary. It is
// rebuilt only when the rate or channel count actually changes.
#ifdef VIBE_HAVE_OPUS
#include <opus/opus.h>
#include <mutex>
namespace {
std::mutex      g_opusDecMtx;
OpusDecoder*    g_opusDec      = nullptr;
int             g_opusDecRate  = 0;
int             g_opusDecCh    = 0;
}
extern "C" JNIEXPORT jshortArray JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeOpusDecode(JNIEnv* env, jobject,
                                                   jbyteArray packet, jint rate, jint channels) {
    if (!packet || rate <= 0 || (channels != 1 && channels != 2)) return nullptr;
    const jsize n = env->GetArrayLength(packet);
    if (n <= 0) return nullptr;
    std::vector<jbyte> buf((size_t)n);
    env->GetByteArrayRegion(packet, 0, n, buf.data());

    std::lock_guard<std::mutex> lk(g_opusDecMtx);
    if (!g_opusDec || g_opusDecRate != rate || g_opusDecCh != channels) {
        if (g_opusDec) opus_decoder_destroy(g_opusDec);
        int err = OPUS_OK;
        g_opusDec = opus_decoder_create(rate, channels, &err);
        if (!g_opusDec || err != OPUS_OK) {
            g_opusDec = nullptr;
            LOGE("opus_decoder_create failed: %d", err);
            return nullptr;
        }
        g_opusDecRate = rate; g_opusDecCh = channels;
    }
    // 120 ms at 48 kHz is the largest an Opus packet can decode to — size for the worst case
    // rather than for the frame size we happen to send, so a server-side change cannot
    // silently truncate audio here.
    const int maxSamples = rate / 1000 * 120;
    std::vector<opus_int16> pcm((size_t)maxSamples * channels);
    const int got = opus_decode(g_opusDec, (const unsigned char*)buf.data(), (opus_int32)n,
                                pcm.data(), maxSamples, 0);
    if (got <= 0) return nullptr;
    const jsize outLen = (jsize)got * channels;
    jshortArray out = env->NewShortArray(outLen);
    if (out) env->SetShortArrayRegion(out, 0, outLen, (const jshort*)pcm.data());
    return out;
}
#else
extern "C" JNIEXPORT jshortArray JNICALL
Java_com_vibesdr_app_VibeLocalSDR_nativeOpusDecode(JNIEnv*, jobject, jbyteArray, jint, jint) {
    return nullptr;   // no encoder in this build either — the client will not ask for Opus
}
#endif
