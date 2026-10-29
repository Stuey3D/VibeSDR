// vibe_xaac.h — xHE-AAC (MPEG-D USAC) decoded IN PROCESS by libxaac (Ittiam / the Android Open Source Project,
// Apache-2.0), for DRM's xHE-AAC stations. V12, 2026-10-29 (docs/v12/LIBXAAC.md).
//
// ★★★ WHY libxaac AND NOT THE FFmpeg IT REPLACES. FFmpeg 8.1's USAC decoder (the bundled one, V12 until now) produced
//     garbage — peaks of 10^4–10^6 full scale, or NaN — on EVERY USAC stream with SBR we gave it, plain FD+SBR included,
//     could not parse MPEG Surround 2-1-2 and refused PVC and inter-TES outright; it does not decode the ACELP/TCX speech
//     core at all. DRM's xHE-AAC runs at 12–24 kbit/s, which is SBR (and often the speech core) by construction. libxaac
//     decodes all of it: ISO's conformance streams to ±1 LSB, and SBR / eSBR / ACELP / MPS streams to within float
//     rounding of macOS's own decoder (docs/v12/LIBXAAC.md "Correctness").
// ★★ IN PROCESS, ONE ACCESS UNIT AT A TIME. No child process, no pipe, no container: the AudioSpecificConfig (drm_audio.h
//    usacFromDrm) is given once, then each access unit. Its output is 16-bit PCM at the stream's own rate (mono or stereo);
//    this wrapper widens it to stereo float and brings it to 48 kHz with the DAB+ path's L/M upsampler (vibe_lm_upsampler.h)
//    — DRM xHE-AAC rates include 19.2, 32 and 38.4 kHz.
// ★★ 16-BIT OUTPUT, ON PURPOSE. libxaac offers 24-bit, but on its AAC (non-USAC) path it silently writes 16-bit while still
//    reporting 24 (measured: DAB+ came out two-thirds of its length). 16 bits is what every route reports truthfully.
// ★ A FRAME THAT FAILS ITS CRC OR THE DECODER IS CONCEALED, NOT PLAYED: the last good frame again, fading out; the next
//   good one fades in (the DrmFaad posture).
// ★ Licence: Apache-2.0, compatible with VibeServer's GPL-3.0; LICENSE + NOTICE ship as LICENSE.libxaac / NOTICE.libxaac.
//   Server builds only (VIBE_HAVE_LIBXAAC). The iPhone/iPad app never links it (it decodes nothing; AudioToolbox exists).
#pragma once
#if defined(VIBE_HAVE_LIBXAAC)
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "vibe_lm_upsampler.h"
#ifndef VIBE_LIBXAAC_VERSION
#define VIBE_LIBXAAC_VERSION "?"
#endif

extern "C" {
// ★ The library's one entry point (decoder/ixheaacd_api.c). The command numbers below are its published API
//   (README_dec.md; decoder/ixheaacd_apicmd_standards.h, ixheaacd_aac_config.h, ixheaacd_memory_standards.h) —
//   restated here so the server includes no library header (the deps prefix carries none).
int ixheaacd_dec_api(void* obj, int cmd, int idx, void* value);
}

namespace vibe {

class XaacDecoder {
public:
    XaacDecoder() = default;
    ~XaacDecoder() { close(); }
    XaacDecoder(const XaacDecoder&) = delete;
    XaacDecoder& operator=(const XaacDecoder&) = delete;

    /** "libxaac 0.1.14" — the release the deps build pinned (CMake passes VIBE_LIBXAAC_VERSION from the deps prefix). */
    static std::string version() { return "libxaac " VIBE_LIBXAAC_VERSION; }   // ★ ONE literal: build-deb.sh finds it in the binary

    /** Open for one stream: its AudioSpecificConfig (object type 42 for xHE-AAC). False if the library refuses it. */
    bool open(const std::vector<uint8_t>& asc) {
        close();
        unsigned sz = 0;
        if (ixheaacd_dec_api(nullptr, kGetApiSize, 0, &sz) != 0 || !sz) return false;
        api_ = alloc_(sz, 8);
        if (!api_ || ixheaacd_dec_api(api_, kInit, kPreConfig, nullptr) != 0) return fail_();
        unsigned v = 1;  ixheaacd_dec_api(api_, kSetConfig, kParamMp4Flag, &v);      // raw access units, config given apart
        v = 16;          ixheaacd_dec_api(api_, kSetConfig, kParamPcmWdsz, &v);       // ★ see the header: 16 is honest
        v = 1;           ixheaacd_dec_api(api_, kSetConfig, kParamEsbr, &v);          // eSBR tools on (harmonic, PVC, inter-TES)
        if (ixheaacd_dec_api(api_, kGetMemtabsSize, 0, &sz) != 0) return fail_();
        void* mt = alloc_(sz, 8);
        if (!mt || ixheaacd_dec_api(api_, kSetMemtabsPtr, 0, mt) != 0) return fail_();
        if (ixheaacd_dec_api(api_, kInit, kPostConfig, nullptr) != 0) return fail_();
        unsigned n = 0; ixheaacd_dec_api(api_, kGetNMemtabs, 0, &n);
        for (unsigned i = 0; i < n; ++i) {
            unsigned s = 0, a = 0, t = 0;
            ixheaacd_dec_api(api_, kGetMemInfoSize, int(i), &s);
            ixheaacd_dec_api(api_, kGetMemInfoAlign, int(i), &a);
            ixheaacd_dec_api(api_, kGetMemInfoType, int(i), &t);
            void* p = alloc_(s, a ? a : 8);
            if (!p || ixheaacd_dec_api(api_, kSetMemPtr, int(i), p) != 0) return fail_();
            if (t == kMemInput) { in_ = static_cast<uint8_t*>(p); inSize_ = int(s); }
            if (t == kMemOutput) out_ = static_cast<uint8_t*>(p);
        }
        if (!in_ || !out_ || asc.empty() || int(asc.size()) > inSize_) return fail_();
        std::memcpy(in_, asc.data(), asc.size());
        int nb = int(asc.size()); ixheaacd_dec_api(api_, kSetInputBytes, 0, &nb);
        if (fatal_(ixheaacd_dec_api(api_, kInit, kInitProcess, nullptr))) return fail_();
        unsigned done = 0; ixheaacd_dec_api(api_, kInit, kInitDoneQuery, &done);
        initDone_ = done != 0;      // ★ plain AAC needs its first frame too (implicit SBR); USAC is done from the config
        open_ = true; last_.clear(); fadeIn_ = false; fadeGain_ = 1.0f; rate_ = 0;
        return true;
    }
    bool ok() const { return open_; }

    /** One access unit (crcOk = the transport's CRC passed) → 48 kHz interleaved stereo appended to `out`. */
    void decode(const uint8_t* au, size_t n, bool crcOk, std::vector<float>& out) {
        if (decodeOne_(au, n, crcOk) && rate_ > 0) up_.process(st_.data(), st_.size() / 2, out, mono_);
    }
    /** The same, at the STREAM's own rate (rateHz()) — what test-drm-xaac compares with the conformance references. */
    void decodeNative(const uint8_t* au, size_t n, bool crcOk, std::vector<float>& out) {
        if (decodeOne_(au, n, crcOk)) out.insert(out.end(), st_.begin(), st_.end());
    }
private:
    /** Decode (or conceal) one unit into st_ — interleaved stereo at the stream's rate. False: nothing to emit. */
    bool decodeOne_(const uint8_t* au, size_t n, bool crcOk) {
        if (!open_) return false;
        std::vector<float>& st = st_; st.clear();
        bool good = crcOk && n > 0 && int(n) <= inSize_;
        if (good && !initDone_) {   // the first frame finishes the init (and is then decoded as usual)
            std::memcpy(in_, au, n); int nb = int(n); ixheaacd_dec_api(api_, kSetInputBytes, 0, &nb);
            ixheaacd_dec_api(api_, kInit, kInitProcess, nullptr);
            unsigned done = 0; ixheaacd_dec_api(api_, kInit, kInitDoneQuery, &done); initDone_ = done != 0;
            if (!initDone_) good = false;
        }
        if (good) {
            std::memcpy(in_, au, n); int nb = int(n); ixheaacd_dec_api(api_, kSetInputBytes, 0, &nb);
            const int e = ixheaacd_dec_api(api_, kExecute, kDoExecute, nullptr);
            int ob = 0; ixheaacd_dec_api(api_, kGetOutputBytes, 0, &ob);
            if (fatal_(e) || ob <= 0) good = false;
            else {
                int ch = 0, rate = 0;
                ixheaacd_dec_api(api_, kGetConfig, kParamNumChannels, &ch);
                ixheaacd_dec_api(api_, kGetConfig, kParamSampFreq, &rate);
                if (ch < 1 || rate <= 0) good = false;
                else {
                    const int16_t* s = reinterpret_cast<const int16_t*>(out_);
                    const size_t frames = size_t(ob) / (2u * size_t(ch));
                    st.resize(frames * 2);
                    for (size_t i = 0; i < frames; ++i) {
                        st[2 * i] = float(s[i * size_t(ch)]) * (1.0f / 32768.0f);
                        st[2 * i + 1] = float(s[i * size_t(ch) + (ch > 1 ? 1 : 0)]) * (1.0f / 32768.0f);
                    }
                    if (rate != rate_) { rate_ = rate; up_.configure(rate, 48000); }
                    mono_ = ch == 1;
                }
            }
        }
        if (!good) {
            ++bad_;
            if (last_.empty()) return false;      // nothing to conceal with yet (and no clock to keep)
            st = last_;                            // ★ the last good frame again, fading out
            const size_t f = st.size() / 2;
            for (size_t i = 0; i < f; ++i) { const float g = fadeGain_ * float(f - i) / float(f); st[2 * i] *= g; st[2 * i + 1] *= g; }
            fadeGain_ = 0.0f; fadeIn_ = true;
        } else {
            ++good_;
            last_ = st; fadeGain_ = 1.0f;
            if (fadeIn_) {                         // after a concealed frame: in again over ~10 ms
                const size_t m = std::min(st.size() / 2, size_t(std::max(1, rate_ / 100)));
                for (size_t i = 0; i < m; ++i) { const float g = float(i) / float(m); st[2 * i] *= g; st[2 * i + 1] *= g; }
                fadeIn_ = false;
            }
        }
        return !st.empty();
    }
public:
    void close() {
        for (void* p : blocks_) std::free(p);
        blocks_.clear(); api_ = nullptr; in_ = out_ = nullptr; inSize_ = 0; open_ = false; initDone_ = false;
    }
    int64_t good() const { return good_; }
    int64_t bad() const { return bad_; }
    int rateHz() const { return rate_; }

private:
    // ★ libxaac's API numbers (see the extern "C" note above)
    enum : int {
        kGetApiSize = 0x0002, kInit = 0x0003, kSetConfig = 0x0004, kGetConfig = 0x0005, kGetMemtabsSize = 0x0006,
        kSetMemtabsPtr = 0x0007, kGetNMemtabs = 0x0008, kExecute = 0x0009, kSetInputBytes = 0x000C,
        kGetOutputBytes = 0x000D, kGetMemInfoSize = 0x0011, kGetMemInfoAlign = 0x0012, kGetMemInfoType = 0x0013,
        kSetMemPtr = 0x0016,
        kPreConfig = 0x0100, kPostConfig = 0x0200, kInitProcess = 0x0300, kInitDoneQuery = 0x0400, kDoExecute = 0x0100,
        kParamPcmWdsz = 0x0000, kParamSampFreq = 0x0001, kParamNumChannels = 0x0002, kParamMp4Flag = 0x000C,
        kParamEsbr = 0x0028, kMemInput = 0x02, kMemOutput = 0x03,
    };
    static bool fatal_(int e) { return (unsigned(e) & 0x80000000u) != 0; }
    void* alloc_(size_t n, size_t align) {
        if (align < sizeof(void*)) align = sizeof(void*);
        void* p = nullptr;
        if (posix_memalign(&p, align, n ? n : 1) != 0) return nullptr;
        std::memset(p, 0, n);
        blocks_.push_back(p);
        return p;
    }
    bool fail_() { close(); return false; }

    void* api_ = nullptr;
    std::vector<void*> blocks_;
    uint8_t* in_ = nullptr; uint8_t* out_ = nullptr; int inSize_ = 0;
    bool open_ = false, initDone_ = false, mono_ = false, fadeIn_ = false;
    float fadeGain_ = 1.0f;
    int rate_ = 0;
    int64_t good_ = 0, bad_ = 0;
    std::vector<float> st_, last_;
    vibedab::DabUpsampler up_;
};

}  // namespace vibe
#endif
