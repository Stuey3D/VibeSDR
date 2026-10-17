// ★ JNI for V12 BINAURAL TUNING — vibedsp/binaural.h's BinauralTuning, the SAME object iOS reaches through its C API.
//   Kotlin: VibeBinaural (VibeBinaural.kt). The setters only store atomics (any thread); the process calls run on
//   the AudioTrack writer threads (vibesdr-ext, vibesdr-decode) and never block.
#include <jni.h>
#include <cstdint>
#include "vibedsp/binaural.h"

static inline void* H(jlong h) { return (void*)(intptr_t)h; }

extern "C" JNIEXPORT jlong JNICALL
Java_com_vibesdr_app_VibeBinaural_nCreate(JNIEnv*, jclass) { return (jlong)(intptr_t)vibe_binaural_create(); }

extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeBinaural_nSetEnabled(JNIEnv*, jclass, jlong h, jboolean on) { vibe_binaural_set_enabled(H(h), on ? 1 : 0); }

extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeBinaural_nSetSource(JNIEnv*, jclass, jlong h, jint src, jfloat dead, jfloat full) {
    vibe_binaural_set_source(H(h), src, dead, full);
}

extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeBinaural_nSetOffset(JNIEnv*, jclass, jlong h, jfloat hz, jboolean valid) {
    vibe_binaural_set_offset(H(h), hz, valid ? 1 : 0);
}

extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeBinaural_nSetStereo(JNIEnv*, jclass, jlong h, jint hint) { vibe_binaural_set_stereo(H(h), hint); }

extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeBinaural_nSetDial(JNIEnv*, jclass, jlong h, jdouble hz) { vibe_binaural_set_dial(H(h), hz); }

/** int16 interleaved (inCh 1|2) → int16 STEREO interleaved. Returns frames written (0 if `out` is short). */
extern "C" JNIEXPORT jint JNICALL
Java_com_vibesdr_app_VibeBinaural_nProcessShorts(JNIEnv* env, jclass, jlong h, jshortArray in, jint inCh, jint frames,
                                                 jint rate, jshortArray out) {
    if (!h || frames <= 0 || (inCh != 1 && inCh != 2)) return 0;
    if (env->GetArrayLength(in) < frames * inCh || env->GetArrayLength(out) < frames * 2) return 0;
    // ★ Critical regions: no copy on the audio thread. Nothing inside calls back into the JVM or waits.
    auto* src = (jshort*)env->GetPrimitiveArrayCritical(in, nullptr);
    if (!src) return 0;
    auto* dst = (jshort*)env->GetPrimitiveArrayCritical(out, nullptr);
    if (!dst) { env->ReleasePrimitiveArrayCritical(in, src, JNI_ABORT); return 0; }
    const int n = vibe_binaural_process_i16(H(h), (const int16_t*)src, inCh, frames, rate, (int16_t*)dst);
    env->ReleasePrimitiveArrayCritical(out, dst, 0);
    env->ReleasePrimitiveArrayCritical(in, src, JNI_ABORT);
    return n;
}

/** The same on little-endian PCM16 BYTES (the decode thread's MediaCodec output). Returns BYTES written. */
extern "C" JNIEXPORT jint JNICALL
Java_com_vibesdr_app_VibeBinaural_nProcessBytes(JNIEnv* env, jclass, jlong h, jbyteArray in, jint inCh, jint frames,
                                                jint rate, jbyteArray out) {
    if (!h || frames <= 0 || (inCh != 1 && inCh != 2)) return 0;
    if (env->GetArrayLength(in) < frames * inCh * 2 || env->GetArrayLength(out) < frames * 4) return 0;
    auto* src = (jbyte*)env->GetPrimitiveArrayCritical(in, nullptr);
    if (!src) return 0;
    auto* dst = (jbyte*)env->GetPrimitiveArrayCritical(out, nullptr);
    if (!dst) { env->ReleasePrimitiveArrayCritical(in, src, JNI_ABORT); return 0; }
    // Android is little-endian on every ABI we ship, so the bytes ARE int16 in place.
    const int n = vibe_binaural_process_i16(H(h), (const int16_t*)src, inCh, frames, rate, (int16_t*)dst);
    env->ReleasePrimitiveArrayCritical(out, dst, 0);
    env->ReleasePrimitiveArrayCritical(in, src, JNI_ABORT);
    return n * 4;
}
