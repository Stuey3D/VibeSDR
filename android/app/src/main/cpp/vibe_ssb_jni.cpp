// ★ JNI for the TRANSCRIBE recogniser feed (V12) — vibedsp/ssb_offset.h's RecogniserFeed, the SAME
//   object iOS reaches through its C API: any rate → 16 kHz (anti-aliased), SSB auto-tune, PCM16 out.
//   Kotlin: VibeRecogniserFeed (VibeTranscriberModule.kt). Called on the tap's own background thread;
//   only setTuning comes from elsewhere, and that one just stores atomics.
#include <jni.h>
#include <cmath>
#include <vector>
#include "vibedsp/ssb_offset.h"

extern "C" JNIEXPORT jlong JNICALL
Java_com_vibesdr_app_VibeRecogniserFeed_nCreate(JNIEnv*, jclass) {
    return (jlong)(intptr_t)vibe_rxfeed_create();
}

extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeRecogniserFeed_nSetTuning(JNIEnv*, jclass, jlong h, jint sideband, jdouble dialHz) {
    vibe_rxfeed_set_tuning((void*)(intptr_t)h, sideband, dialHz);
}

/** Mono float at `rate` in → 16 kHz PCM16 little-endian bytes out. Returns the BYTES written (0 if `out` is short). */
extern "C" JNIEXPORT jint JNICALL
Java_com_vibesdr_app_VibeRecogniserFeed_nProcess(JNIEnv* env, jclass, jlong h, jfloatArray in, jint n, jint rate, jbyteArray out) {
    if (!h || n <= 0 || rate <= 0) return 0;
    thread_local std::vector<float> res;
    const int cap = vibedsp::RecogniserFeed::maxOut(n, rate);
    res.resize((size_t)cap);
    if (env->GetArrayLength(out) < cap * 2 || env->GetArrayLength(in) < n) return 0;
    jfloat* src = env->GetFloatArrayElements(in, nullptr);
    if (!src) return 0;
    const int m = vibe_rxfeed_process((void*)(intptr_t)h, src, n, rate, res.data(), cap);
    env->ReleaseFloatArrayElements(in, src, JNI_ABORT);
    jbyte* dst = (jbyte*)env->GetPrimitiveArrayCritical(out, nullptr);
    if (!dst) return 0;
    for (int i = 0; i < m; ++i) {
        float v = res[i];
        v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
        const int s = (int)std::lrint(v * 32767.0f);
        dst[2 * i] = (jbyte)(s & 0xff);
        dst[2 * i + 1] = (jbyte)((s >> 8) & 0xff);
    }
    env->ReleasePrimitiveArrayCritical(out, dst, 0);
    return m * 2;
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_vibesdr_app_VibeRecogniserFeed_nDialCorrection(JNIEnv*, jclass, jlong h) {
    return vibe_rxfeed_dial_correction((void*)(intptr_t)h);
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_vibesdr_app_VibeRecogniserFeed_nRms(JNIEnv*, jclass, jlong h) {
    return vibe_rxfeed_rms((void*)(intptr_t)h);
}

/** [offsetHz, confidence, voicedSeconds, passes, estimates, corrections, audioShiftHz] — for the debug log. */
extern "C" JNIEXPORT jint JNICALL
Java_com_vibesdr_app_VibeRecogniserFeed_nStats(JNIEnv* env, jclass, jlong h, jfloatArray out) {
    if (!h || env->GetArrayLength(out) < 7) return 0;
    float s[7];
    const int n = vibe_rxfeed_stats((void*)(intptr_t)h, s);
    env->SetFloatArrayRegion(out, 0, n, s);
    return n;
}
