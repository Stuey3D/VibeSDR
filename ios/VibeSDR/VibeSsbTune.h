// ★ The TRANSCRIBE recogniser feed's C API, for Swift — the implementation is vibedsp/ssb_offset.cpp, compiled into
//   libvibelocalsdr_ios.a (modules/vibe-local-sdr/build_ios.sh) and, for Android, into libvibelocalsdr.so. ONE
//   implementation on both platforms: any rate → 16 kHz through an anti-aliased resampler, then SSB auto-tune
//   (the voice's own harmonics say how far an SSB dial is off; the recogniser's copy is shifted back — never what the
//   listener hears).
// ★★ A HAND-KEPT MIRROR of the extern "C" block at the bottom of vibedsp/ssb_offset.h (Swift cannot read that C++
//    header). Same names, same argument order — change both or neither.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void* vibe_rxfeed_create(void);
void  vibe_rxfeed_destroy(void* h);
/** sideband +1 USB, -1 LSB, 0 other. Any change of sideband or dial resets the estimate. Any thread. */
void  vibe_rxfeed_set_tuning(void* h, int sideband, double dialHz);
/** Mono float in at inRate → 16 kHz float out. Returns samples written (0 if outCap < n·16000/inRate + 8). */
int   vibe_rxfeed_process(void* h, const float* in, int n, int inRate, float* out, int outCap);
/** The correction as a DIAL change in Hz (USB +offset, LSB −offset); 0 = none. */
float vibe_rxfeed_dial_correction(void* h);
/** RMS of the last processed block. */
float vibe_rxfeed_rms(void* h);
/** [offsetHz, confidence, voicedSeconds, passes, estimates, corrections, audioShiftHz]. Returns 7. */
int   vibe_rxfeed_stats(void* h, float* out7);
#ifdef __cplusplus
}
#endif
