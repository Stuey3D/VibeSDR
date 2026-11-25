// Host stand-in for librtlsdr's <rtl-sdr.h> — only the surface rtl_tcp_server.cpp calls.
// The behaviour behind it is fake_rtlsdr.cpp, which plays one of two radios (FAKE_RADIO):
//   rtl    — an RTL2832U + R820T: opens, reports tuner 5 / 29 gains, streams u8 IQ at the set rate
//   airspy — any NON-RTL USB radio (Airspy HF+, Airspy R2, HackRF) behind librtlsdr: this is what
//            osmocom rtl-sdr 2.0.3 + our open_sys_dev patch really does with one — the open
//            SUCCEEDS (the dummy write fails -> "Resetting device", no tuner found -> direct
//            sampling, return 0, librtlsdr.c:1534/1634), tuner type 0, no gains, and
//            rtlsdr_read_async posts bulk reads on 0x81 that the radio never fills.
#pragma once
#include <cstdint>
extern "C" {
typedef struct rtlsdr_dev rtlsdr_dev_t;
typedef void (*rtlsdr_read_async_cb_t)(unsigned char* buf, uint32_t len, void* ctx);
enum rtlsdr_tuner { RTLSDR_TUNER_UNKNOWN = 0, RTLSDR_TUNER_E4000, RTLSDR_TUNER_FC0012, RTLSDR_TUNER_FC0013,
                    RTLSDR_TUNER_FC2580, RTLSDR_TUNER_R820T, RTLSDR_TUNER_R828D };
int      rtlsdr_open_sys_dev(rtlsdr_dev_t** dev, intptr_t fd);
int      rtlsdr_close(rtlsdr_dev_t*);
int      rtlsdr_set_sample_rate(rtlsdr_dev_t*, uint32_t);
uint32_t rtlsdr_get_sample_rate(rtlsdr_dev_t*);
int      rtlsdr_set_center_freq(rtlsdr_dev_t*, uint32_t);
uint32_t rtlsdr_get_center_freq(rtlsdr_dev_t*);
int      rtlsdr_set_freq_correction(rtlsdr_dev_t*, int);
int      rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t*, int);
int      rtlsdr_set_tuner_gain(rtlsdr_dev_t*, int);
int      rtlsdr_get_tuner_gains(rtlsdr_dev_t*, int*);
enum rtlsdr_tuner rtlsdr_get_tuner_type(rtlsdr_dev_t*);
int      rtlsdr_set_agc_mode(rtlsdr_dev_t*, int);
int      rtlsdr_set_direct_sampling(rtlsdr_dev_t*, int);
int      rtlsdr_set_offset_tuning(rtlsdr_dev_t*, int);
int      rtlsdr_set_bias_tee(rtlsdr_dev_t*, int);
int      rtlsdr_reset_buffer(rtlsdr_dev_t*);
int      rtlsdr_read_async(rtlsdr_dev_t*, rtlsdr_read_async_cb_t, void*, uint32_t, uint32_t);
int      rtlsdr_cancel_async(rtlsdr_dev_t*);
}
