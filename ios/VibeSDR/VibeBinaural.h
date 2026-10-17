// ★ BINAURAL TUNING's C API, for Swift — the implementation is vibedsp/binaural.cpp, compiled into
//   libvibelocalsdr_ios.a (modules/vibe-local-sdr/build_ios.sh) and, for Android, into libvibelocalsdr.so. ONE
//   implementation on both platforms: the far-ear spatialiser (level, delay, head shadow), the WFM stereo
//   hand-off, and the SSB voice-offset source on its own worker thread.
// ★★ A HAND-KEPT MIRROR of the extern "C" block at the bottom of vibedsp/binaural.h (Swift cannot read that C++
//    header). Same names, same argument order — change both or neither.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void* vibe_binaural_create(void);
void  vibe_binaural_destroy(void* h);
void  vibe_binaural_set_enabled(void* h, int on);
int   vibe_binaural_enabled(void* h);
/** source: 0 none, 1 given (set_offset), 2 SSB USB, 3 SSB LSB. 0 pan inside ±deadHz, full at ±fullHz. Any thread. */
void  vibe_binaural_set_source(void* h, int source, float deadHz, float fullHz);
/** Signed Hz, + = signal ABOVE the dial (lean right). Any thread. */
void  vibe_binaural_set_offset(void* h, float hz, int valid);
/** 0 mono / not a stereo mode, 1 stereo locked, 2 unknown (measure the audio). Any thread. */
void  vibe_binaural_set_stereo(void* h, int hint);
void  vibe_binaural_set_dial(void* h, double hz);
/** Audio thread. Planar stereo float, in place. */
void  vibe_binaural_process_planar(void* h, float* L, float* R, int n, int rate);
/** Audio thread. int16 interleaved (inCh 1 or 2) → int16 stereo interleaved. Returns frames. */
int   vibe_binaural_process_i16(void* h, const int16_t* in, int inCh, int frames, int rate, int16_t* outStereo);
/** [enabled, source, offsetHz, offsetValid, pan, stereoWeight, ssbEstimates, ssbAccepted, ringDrops]. */
int   vibe_binaural_stats(void* h, float* out, int cap);
#ifdef __cplusplus
}
#endif
