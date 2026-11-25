// Fake librtlsdr for the host RTL-TCP server test — see fake/rtl-sdr.h for the two radios.
#include "rtl-sdr.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

struct rtlsdr_dev {
    bool rtl = true;
    std::atomic<uint32_t> rate{2048000};
    std::atomic<uint32_t> freq{100000000};
    std::atomic<bool> cancel{false};
};

extern "C" {
int rtlsdr_open_sys_dev(rtlsdr_dev_t** out, intptr_t) {
    auto* d = new rtlsdr_dev();
    const char* r = std::getenv("FAKE_RADIO");
    d->rtl = !(r && std::strcmp(r, "airspy") == 0);
    *out = d;
    return 0;   // ★ real librtlsdr returns 0 for a non-RTL device too
}
int rtlsdr_close(rtlsdr_dev_t* d) { delete d; return 0; }
int rtlsdr_set_sample_rate(rtlsdr_dev_t* d, uint32_t r) { d->rate = r; return 0; }
uint32_t rtlsdr_get_sample_rate(rtlsdr_dev_t* d) { return d->rate; }
int rtlsdr_set_center_freq(rtlsdr_dev_t* d, uint32_t f) { d->freq = f; return 0; }
uint32_t rtlsdr_get_center_freq(rtlsdr_dev_t* d) { return d->freq; }
int rtlsdr_set_freq_correction(rtlsdr_dev_t*, int) { return 0; }
int rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t*, int) { return 0; }
int rtlsdr_set_tuner_gain(rtlsdr_dev_t*, int) { return 0; }
int rtlsdr_get_tuner_gains(rtlsdr_dev_t* d, int* g) {
    if (!d->rtl) return 0;
    if (g) for (int i = 0; i < 29; i++) g[i] = i * 17;
    return 29;
}
enum rtlsdr_tuner rtlsdr_get_tuner_type(rtlsdr_dev_t* d) { return d->rtl ? RTLSDR_TUNER_R820T : RTLSDR_TUNER_UNKNOWN; }
int rtlsdr_set_agc_mode(rtlsdr_dev_t*, int) { return 0; }
int rtlsdr_set_direct_sampling(rtlsdr_dev_t*, int) { return 0; }
int rtlsdr_set_offset_tuning(rtlsdr_dev_t*, int) { return 0; }
int rtlsdr_set_bias_tee(rtlsdr_dev_t*, int) { return 0; }
int rtlsdr_reset_buffer(rtlsdr_dev_t*) { return 0; }
int rtlsdr_read_async(rtlsdr_dev_t* d, rtlsdr_read_async_cb_t cb, void* ctx, uint32_t, uint32_t bufLen) {
    std::vector<unsigned char> buf(bufLen ? bufLen : 262144);
    uint8_t ph = 0;
    while (!d->cancel.load()) {
        if (!d->rtl) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }   // nothing ever arrives
        // A tone around 127.5 — real u8 IQ sits there; the client checks the mean.
        for (size_t i = 0; i < buf.size(); i++) buf[i] = (uint8_t)(127 + ((ph++ & 0x3f) - 32));
        cb(buf.data(), (uint32_t)buf.size(), ctx);
        const double secs = (double)buf.size() / (2.0 * d->rate.load());
        std::this_thread::sleep_for(std::chrono::microseconds((long)(secs * 1e6)));
    }
    return 0;
}
int rtlsdr_cancel_async(rtlsdr_dev_t* d) { d->cancel = true; return 0; }
}
