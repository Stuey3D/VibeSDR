// VibeSDR — no-op librtlsdr stub for the iOS build of the local-SDR shim.
//
// iOS has no USB host SDR, so local_sdr_shim.cpp's USB path (rtlsdr_*) is never
// invoked there — but it must still COMPILE and LINK. On Android the shim includes
// the real <rtl-sdr.h> (from the android-sdr-kit); on every other platform it
// includes this stub, which declares the rtlsdr_* surface the shim references as
// inline no-ops. Only the RTL-TCP path runs on iOS (no librtlsdr at all).
#pragma once
#include <cstdint>

extern "C" {

typedef struct rtlsdr_dev rtlsdr_dev_t;
typedef void (*rtlsdr_read_async_cb_t)(unsigned char* buf, uint32_t len, void* ctx);

static inline int      rtlsdr_open_sys_dev(rtlsdr_dev_t**, int)        { return -1; }
static inline int      rtlsdr_close(rtlsdr_dev_t*)                     { return 0; }
static inline int      rtlsdr_set_sample_rate(rtlsdr_dev_t*, uint32_t) { return 0; }
static inline uint32_t rtlsdr_get_sample_rate(rtlsdr_dev_t*)           { return 0; }
static inline int      rtlsdr_set_center_freq(rtlsdr_dev_t*, uint32_t) { return 0; }
static inline int      rtlsdr_set_freq_correction(rtlsdr_dev_t*, int)  { return 0; }
static inline int      rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t*, int)  { return 0; }
static inline int      rtlsdr_set_tuner_gain(rtlsdr_dev_t*, int)       { return 0; }
static inline int      rtlsdr_get_tuner_gains(rtlsdr_dev_t*, int*)     { return 0; }
static inline int      rtlsdr_set_bias_tee(rtlsdr_dev_t*, int)         { return 0; }
static inline int      rtlsdr_set_agc_mode(rtlsdr_dev_t*, int)         { return 0; }
static inline int      rtlsdr_set_direct_sampling(rtlsdr_dev_t*, int)  { return 0; }
static inline int      rtlsdr_reset_buffer(rtlsdr_dev_t*)              { return 0; }
static inline int      rtlsdr_read_async(rtlsdr_dev_t*, rtlsdr_read_async_cb_t, void*, uint32_t, uint32_t) { return 0; }
static inline int      rtlsdr_cancel_async(rtlsdr_dev_t*)              { return 0; }
// iOS has no USB SDR — the enumeration/open path reports "no devices" so the shim uses RTL-TCP only.
static inline uint32_t    rtlsdr_get_device_count(void)                       { return 0; }
static inline int         rtlsdr_open(rtlsdr_dev_t**, uint32_t)               { return -1; }
static inline const char* rtlsdr_get_device_name(uint32_t)                    { return ""; }
static inline int         rtlsdr_get_device_usb_strings(uint32_t, char*, char*, char*) { return -1; }

} // extern "C"
