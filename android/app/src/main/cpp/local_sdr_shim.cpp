// VibeSDR V5 — local-SDR shim implementation (NATIVE-ONLY, GPL-free).
//
// Pipeline (SDR++ Brown / FFTW / VOLK all removed as of V5):
//   RTL-SDR (USB fd) / RTL-TCP → u8 IQ → cf32 → vibedsp::RxPipeline
//     ├─ fftshifted FFT dB row → SPEC full-uint8 frames → /ws/user-spectrum
//     └─ DDC → demod (AM/SSB/CW/NFM/WFM stereo+RDS) → 48k float PCM
//                → /ws/audio  (WFM stereo, others mono)
//
// The on-device DSP is the clean-room `vibedsp` engine; the localhost HTTP/
// WebSocket server + RTL-TCP client use the clean-room `net_shim`. A minimal
// server (one thread per connection) speaks the UberSDR contract so the VibeSDR
// client connects unchanged. Control (zoom/tune/mode/bandwidth/set_rate/ping/
// reset) arrives as JSON text frames. librtlsdr (USB driver + HW controls) is
// the only remaining native dependency.

#include "local_sdr_shim.h"

// Android builds the USB/librtlsdr local-hardware path; iOS builds only the
// RTL-TCP path (no USB host SDR on iOS). The USB code stays compiled on iOS via a
// no-op rtl-sdr stub (start() is never invoked there), so the shared DSP/net/
// RTL-TCP code below needs no per-call-site #ifdefs.
#ifdef __ANDROID__
  #include <android/log.h>
  #include <rtl-sdr.h>
#else
  #include "rtl_sdr_stub.h"   // iOS: no-op rtlsdr_* decls so the USB path compiles
#endif
#include <unistd.h>
// Thread naming + audio priority are Android/Linux-only (Darwin/iOS has no
// <sys/prctl.h> / PR_SET_NAME). Guard so the shared shim still compiles for the
// iOS prebuilt lib, where these are no-ops. `vibeAudioThread` = name + real
// URGENT_AUDIO priority (nice -19) for the DSP/audio thread; `vibeThreadName` =
// name only, so a spinning thread is identifiable in `top -H` / systrace instead
// of showing as the inherited RN "mqt_v_native".
#if defined(__ANDROID__)
  #include <sys/resource.h>   // setpriority
  #include <sys/prctl.h>      // PR_SET_NAME
  static inline void vibeAudioThread(const char* name) {
      prctl(PR_SET_NAME, name);
      setpriority(PRIO_PROCESS, 0, -19); // = Process.THREAD_PRIORITY_URGENT_AUDIO
  }
  static inline void vibeThreadName(const char* name) { prctl(PR_SET_NAME, name); }
#else
  static inline void vibeAudioThread(const char*) {}
  static inline void vibeThreadName(const char*) {}
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <map>
#include <mutex>
#include <random>
#include <unordered_map>
#include <string>
#include <thread>
#include <vector>

#if defined(__aarch64__)
  #include <arm_neon.h>             // NEON u8->f32 IQ conversion
#endif

#include "vibedsp/vibedsp.h"        // V5 clean-room GPL-free DSP engine (RxPipeline)
#include "net_shim.h"
#include "spyserver/spyserver_client.h"               // V5 clean-room GPL-free TCP socket wrapper
#include "decoders/fsk_decoder.h"   // RTTY/NAVTEX (audio-extension decoder)
#include "decoders/wefax_decoder.h" // WEFAX (audio-extension decoder)
#include "decoders/ft8_decoder.h"   // FT8/FT4 → digital spots
#include "decoders/sstv_decoder.h"  // SSTV (audio-extension image decoder)
#include "decoders/audio_nr.h"      // self-contained spectral-subtraction audio NR
#include "decoders/auto_notch.h"    // NLMS automatic notch (adaptive line enhancer)
#include "vibe_web_page.h"          // GENERATED: the web client served from GET /

#define LOG_TAG "VibeLocalSDR"
#ifdef __ANDROID__
  #define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
  #define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
  #include <cstdio>
  #define LOGI(...) do { fprintf(stderr, "[" LOG_TAG "] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
  #define LOGE(...) do { fprintf(stderr, "[" LOG_TAG " E] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#endif

namespace vibe {
namespace {

// V5: the on-device DSP is now the clean-room GPL-free engine. These local
// aliases replace the old SDR++ dsp:: sample types so the decoder/audio feed
// code below is unchanged (it only ever touched .l / .r).
using cf32     = vibedsp::cf32;       // interleaved IQ sample (std::complex<float>)
using stereo_t = vibedsp::stereo;     // { float l, r; }
// Cap on samples copied out of one IQ buffer (was SDR++'s STREAM_BUFFER_SIZE).
constexpr int STREAM_BUFFER_SIZE = 1000000;

// Convert `nF` interleaved u8 I/Q bytes to floats: f = (b - 127.4)/128. Runs at
// the full IQ rate (2.4 MHz) for every mode, so NEON it on AArch64.
static inline void convU8ToF32(const uint8_t* in, float* out, int nF) {
#if defined(__aarch64__)
    const float32x4_t bias = vdupq_n_f32(127.4f), inv = vdupq_n_f32(1.0f / 128.0f);
    int i = 0;
    for (; i + 16 <= nF; i += 16) {
        const uint8x16_t b = vld1q_u8(in + i);
        const uint16x8_t lo = vmovl_u8(vget_low_u8(b)), hi = vmovl_u8(vget_high_u8(b));
        const float32x4_t f0 = vcvtq_f32_u32(vmovl_u16(vget_low_u16(lo)));
        const float32x4_t f1 = vcvtq_f32_u32(vmovl_u16(vget_high_u16(lo)));
        const float32x4_t f2 = vcvtq_f32_u32(vmovl_u16(vget_low_u16(hi)));
        const float32x4_t f3 = vcvtq_f32_u32(vmovl_u16(vget_high_u16(hi)));
        vst1q_f32(out + i,      vmulq_f32(vsubq_f32(f0, bias), inv));
        vst1q_f32(out + i + 4,  vmulq_f32(vsubq_f32(f1, bias), inv));
        vst1q_f32(out + i + 8,  vmulq_f32(vsubq_f32(f2, bias), inv));
        vst1q_f32(out + i + 12, vmulq_f32(vsubq_f32(f3, bias), inv));
    }
    for (; i < nF; ++i) out[i] = ((float)in[i] - 127.4f) * (1.0f / 128.0f);
#else
    for (int i = 0; i < nF; ++i) out[i] = ((float)in[i] - 127.4f) * (1.0f / 128.0f);
#endif
}

// ── SHA1 + base64 (WebSocket handshake) ─────────────────────────────────────
struct Sha1 {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    static uint32_t rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }
    void hash(const uint8_t* msg, size_t len, uint8_t out[20]) {
        std::vector<uint8_t> data(msg, msg + len);
        uint64_t ml = (uint64_t)len * 8;
        data.push_back(0x80);
        while (data.size() % 64 != 56) data.push_back(0x00);
        for (int i = 7; i >= 0; i--) data.push_back((uint8_t)(ml >> (i * 8)));
        for (size_t off = 0; off < data.size(); off += 64) {
            uint32_t w[80];
            for (int i = 0; i < 16; i++)
                w[i] = (data[off+i*4]<<24)|(data[off+i*4+1]<<16)|(data[off+i*4+2]<<8)|data[off+i*4+3];
            for (int i = 16; i < 80; i++) w[i] = rol(w[i-3]^w[i-8]^w[i-14]^w[i-16], 1);
            uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
            for (int i = 0; i < 80; i++) {
                uint32_t f, k;
                if (i<20){f=(b&c)|(~b&d);k=0x5A827999;}
                else if(i<40){f=b^c^d;k=0x6ED9EBA1;}
                else if(i<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;}
                else{f=b^c^d;k=0xCA62C1D6;}
                uint32_t t=rol(a,5)+f+e+k+w[i]; e=d;d=c;c=rol(b,30);b=a;a=t;
            }
            h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;
        }
        for (int i=0;i<5;i++){out[i*4]=(uint8_t)(h[i]>>24);out[i*4+1]=(uint8_t)(h[i]>>16);
                              out[i*4+2]=(uint8_t)(h[i]>>8);out[i*4+3]=(uint8_t)h[i];}
    }
};
std::string base64(const uint8_t* in, size_t len) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = in[i] << 16;
        if (i+1 < len) n |= in[i+1] << 8;
        if (i+2 < len) n |= in[i+2];
        out.push_back(t[(n>>18)&63]); out.push_back(t[(n>>12)&63]);
        out.push_back(i+1<len ? t[(n>>6)&63] : '=');
        out.push_back(i+2<len ? t[n&63] : '=');
    }
    return out;
}
// ── SHA256 + HMAC-SHA256 + hex (VibeServer PIN challenge-response) ───────────
struct Sha256 {
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    static uint32_t ror(uint32_t v,int b){return (v>>b)|(v<<(32-b));}
    void hash(const uint8_t* msg, size_t len, uint8_t out[32]) {
        static const uint32_t k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        std::vector<uint8_t> data(msg, msg + len);
        uint64_t ml = (uint64_t)len * 8;
        data.push_back(0x80);
        while (data.size() % 64 != 56) data.push_back(0x00);
        for (int i = 7; i >= 0; i--) data.push_back((uint8_t)(ml >> (i * 8)));
        for (size_t off = 0; off < data.size(); off += 64) {
            uint32_t w[64];
            for (int i=0;i<16;i++)
                w[i]=(data[off+i*4]<<24)|(data[off+i*4+1]<<16)|(data[off+i*4+2]<<8)|data[off+i*4+3];
            for (int i=16;i<64;i++){
                uint32_t s0=ror(w[i-15],7)^ror(w[i-15],18)^(w[i-15]>>3);
                uint32_t s1=ror(w[i-2],17)^ror(w[i-2],19)^(w[i-2]>>10);
                w[i]=w[i-16]+s0+w[i-7]+s1;
            }
            uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
            for (int i=0;i<64;i++){
                uint32_t S1=ror(e,6)^ror(e,11)^ror(e,25);
                uint32_t ch=(e&f)^(~e&g);
                uint32_t t1=hh+S1+ch+k[i]+w[i];
                uint32_t S0=ror(a,2)^ror(a,13)^ror(a,22);
                uint32_t maj=(a&b)^(a&c)^(b&c);
                uint32_t t2=S0+maj;
                hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
            }
            h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
        }
        for (int i=0;i<8;i++){out[i*4]=(uint8_t)(h[i]>>24);out[i*4+1]=(uint8_t)(h[i]>>16);
                              out[i*4+2]=(uint8_t)(h[i]>>8);out[i*4+3]=(uint8_t)h[i];}
    }
};
static void hmacSha256(const uint8_t* key, size_t klen,
                       const uint8_t* msg, size_t mlen, uint8_t out[32]) {
    uint8_t kb[64] = {0};
    if (klen > 64) { Sha256().hash(key, klen, kb); }        // block-size fold
    else memcpy(kb, key, klen);
    uint8_t ipad[64], opad[64];
    for (int i=0;i<64;i++){ ipad[i]=kb[i]^0x36; opad[i]=kb[i]^0x5c; }
    uint8_t inner[32];
    { std::vector<uint8_t> b(ipad, ipad+64); b.insert(b.end(), msg, msg+mlen);
      Sha256().hash(b.data(), b.size(), inner); }
    std::vector<uint8_t> b(opad, opad+64); b.insert(b.end(), inner, inner+32);
    Sha256().hash(b.data(), b.size(), out);
}
static std::string toHex(const uint8_t* in, size_t len) {
    static const char* d = "0123456789abcdef";
    std::string s; s.reserve(len*2);
    for (size_t i=0;i<len;i++){ s.push_back(d[in[i]>>4]); s.push_back(d[in[i]&15]); }
    return s;
}
// Constant-time hex-string compare (both lowercased, equal length assumed short).
static bool ctEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    volatile unsigned char acc = 0;
    for (size_t i=0;i<a.size();i++) acc |= (unsigned char)(a[i]^b[i]);
    return acc == 0;
}

std::string jsonStr(const std::string& s, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    auto p = s.find(pat); if (p == std::string::npos) return "";
    p = s.find(':', p); if (p == std::string::npos) return "";
    p = s.find('"', p); if (p == std::string::npos) return "";
    auto q = s.find('"', p+1); if (q == std::string::npos) return "";
    return s.substr(p+1, q-p-1);
}
bool jsonNum(const std::string& s, const char* key, double& out) {
    std::string pat = std::string("\"") + key + "\"";
    auto p = s.find(pat); if (p == std::string::npos) return false;
    p = s.find(':', p); if (p == std::string::npos) return false;
    out = strtod(s.c_str() + p + 1, nullptr);
    return true;
}

constexpr double AUDIO_SR = 48000.0;
// FFT averaging: the front end runs FFT_AVG× the emit rate and we block-average
// that many independent FFTs per emitted frame. Cuts per-frame variance so the
// spectrum/waterfall doesn't shimmer (UberSDR/SDR++ average similarly).
constexpr int FFT_AVG = 4;
// Bins actually sent to the client (= waterfall texture width). Kept GPU-safe
// (a 32768-wide texture exceeds mobile GPU max texture size → blank waterfall).
// The internal FFT is finer (fftSizeForRate); we downsample/crop to this.
constexpr int OUT_BINS = 4096;

// Per-mode demod parameters.
struct ModeParams {
    enum Kind { AM, SSB_USB, SSB_LSB, CW, NFM, WFM } kind;
    double ifRate;
    double bandwidth;
    int channels;
};
// Pick the FFT size for a given sample rate to hold ~constant Hz/bin so detail
// stays uniform and scales with bandwidth. ~75 Hz/bin → fine enough that
// zoomed-in views (crop-zoom stretches existing bins, it doesn't add resolution)
// still have plenty of bins. Smallest power-of-2 >= rate/75, clamped [4096, 32768].
/**
 * USB transfer size for a given sample rate.
 *
 * librtlsdr's default (buf_len = 0 → 262144 bytes = 131072 samples) is a fixed
 * SAMPLE COUNT, so the TIME each buffer represents scales inversely with the
 * rate: ~55 ms at 2.4 MSPS but ~136 ms at 0.96 MSPS. At the low rates the IQ
 * arrives in big infrequent lumps — the DSP thread starves between them, the
 * audio breaks up and the waterfall lurches. (Exactly the observed threshold:
 * 2.4 and 1.8 fine, 1.2 and 0.96 broken.)
 *
 * Size it by TIME instead — ~32 ms per buffer at any rate. libusb requires a
 * multiple of 512; clamp to librtlsdr's sane range.
 */
static uint32_t rtlBufLenForRate(double rate) {
    double bytes = rate * 2.0 * 0.032;             // 2 bytes/sample (I+Q u8), 32 ms
    uint32_t len = (uint32_t)(bytes / 512.0 + 0.5) * 512;
    if (len < 16384)  len = 16384;
    if (len > 262144) len = 262144;
    return len;
}

/**
 * The rate an RTL2832 will ACTUALLY run at when asked for `rate`.
 *
 * The device derives its rate from a 28.8 MHz clock through a fractional divider,
 * so it quantises the request. On USB we can read the truth back
 * (rtlsdr_get_sample_rate), but over rtl_tcp there is no way to ask — the
 * protocol has no reply channel. We used to just assume we got what we asked for,
 * which meant the DSP resampled against the WRONG rate: audio came out
 * pitch-shifted (the "chipmunks"). Reproduce librtlsdr's own arithmetic instead.
 */
static double rtlActualRate(double rate) {
    if (rate <= 0) return rate;
    const double xtal = 28800000.0;
    const double two22 = 4194304.0;                       // 1 << 22
    uint32_t ratio = (uint32_t)(xtal * two22 / rate);
    ratio &= 0x0ffffffcu;                                 // librtlsdr masks the low bits
    if (!ratio) return rate;
    const double real = xtal * two22 / (double)ratio;
    return real;
}

int fftSizeForRate(double rate) {
    double want = rate / 75.0;
    int s = 4096;
    while (s < (int)want && s < 32768) s *= 2;
    return s;
}

ModeParams paramsFor(const std::string& mode) {
    if (mode == "usb")            return {ModeParams::SSB_USB, 24000, 2700, 1};
    if (mode == "lsb")            return {ModeParams::SSB_LSB, 24000, 2700, 1};
    if (mode == "am" || mode == "sam") return {ModeParams::AM, 15000, 10000, 1};
    if (mode == "cwu" || mode == "cwl" || mode == "cw") return {ModeParams::CW, 8000, 1200, 1};
    if (mode == "wfm")            return {ModeParams::WFM, 250000, 200000, 2};  // NB: ifRate field is unused/dead
    /* nfm / fm */                return {ModeParams::NFM, 50000, 12500, 1};
}

// Map the shim's mode kind onto the V5 engine's demod mode.
vibedsp::RxPipeline::Mode rxModeFor(ModeParams::Kind k) {
    using M = vibedsp::RxPipeline::Mode;
    switch (k) {
        case ModeParams::AM:       return M::AM;
        case ModeParams::SSB_USB:  return M::SSB_USB;
        case ModeParams::SSB_LSB:  return M::SSB_LSB;
        case ModeParams::CW:       return M::CW;
        case ModeParams::WFM:      return M::WFM;
        case ModeParams::NFM:      default: return M::NFM;
    }
}

} // namespace

// ── IMA-ADPCM encoder (VibeServer compressed audio, ~4:1) ───────────────────
// Standard IMA/DVI tables. Each WS audio frame is self-contained: its 4-byte
// preamble carries the seed predictor+index, so the client decodes it statelessly
// (robust to a client that joins mid-stream). State still carries frame-to-frame
// on the server so quality doesn't reset each frame.
namespace {
static const int kAdpcmStep[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,
    88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,
    544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,
    2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,
    10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
static const int kAdpcmIndex[16] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};

struct AdpcmState { int predictor = 0; int index = 0; };

static inline int adpcmEncodeSample(int sample, AdpcmState& st) {
    int step = kAdpcmStep[st.index];
    int diff = sample - st.predictor;
    int code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    int vpdiff = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { code |= 2; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { code |= 1;             vpdiff += step; }
    if (code & 8) st.predictor -= vpdiff; else st.predictor += vpdiff;
    if (st.predictor > 32767) st.predictor = 32767;
    else if (st.predictor < -32768) st.predictor = -32768;
    st.index += kAdpcmIndex[code & 15];
    if (st.index < 0) st.index = 0; else if (st.index > 88) st.index = 88;
    return code & 15;
}

// Append one channel block: [pred int16 LE][index u8][0 pad] + ceil(n/2) nibble
// bytes (low nibble = first sample). Seeds the preamble from st's CURRENT value,
// then advances st across the block.
static void adpcmEncodeBlock(std::vector<uint8_t>& out, const int16_t* pcm, int n, AdpcmState& st) {
    out.push_back((uint8_t)(st.predictor & 0xff));
    out.push_back((uint8_t)((st.predictor >> 8) & 0xff));
    out.push_back((uint8_t)st.index);
    out.push_back(0);
    for (int i = 0; i < n; i += 2) {
        int lo = adpcmEncodeSample(pcm[i], st);
        int hi = (i + 1 < n) ? adpcmEncodeSample(pcm[i + 1], st) : 0;
        out.push_back((uint8_t)(lo | (hi << 4)));
    }
}
} // namespace

// ── VibeServer server-side state (declared here so Impl members can see it) ──
// LAN bind flag + PIN secret + compatibility limits + audio-codec toggle. The
// public setter methods that write these live after the Impl struct.
static std::atomic<bool>   g_serveOnLan{false};
static std::mutex          g_vsMtx;
static std::string         g_vsSecret;                 // empty = no PIN (open)
static std::atomic<double> g_vsMaxBandwidth{0.0};      // <=0 = no cap
static std::atomic<double> g_vsMaxFftRate{0.0};        // <=0 = server default (20 fps)
// Serve the browser client at GET /? Off = app-only, so a stranger who finds the
// address in a browser gets nothing. The WS endpoints stay up (the app uses them);
// only the human-facing page is withheld.
static std::atomic<bool>   g_vsWebEnabled{true};
// Pinned capture rate (Hz). 0 = client-controlled. When pinned, a client's
// {"type":"sampleRate"} is IGNORED and the rate is advertised as locked, so the
// client can hide a control it isn't allowed to use.
static std::atomic<double> g_vsLockedRate{0.0};

// Station list (EiBi + anything else the app has) served at GET /stations for the
// web client's search. Supplied BY THE APP — it already downloads and caches EiBi,
// and a browser can't fetch eibispace.de itself (no CORS headers). Same model as
// UberSDR: the server presents the stations, the client just renders them.
static std::mutex  g_stationsMtx;
static std::string g_stationsJson;

// ── Learned station bookmarks (RDS) ──────────────────────────────────────────
//
// When a WFM station announces its name over RDS we remember it against the
// frequency it was heard on, so the search bar fills itself in with the stations
// this receiver can ACTUALLY hear. Learned here in the shim, because this is the
// only place that sees both the tuned frequency AND the decoded name — when
// VibeServer is serving, the app isn't tuned to anything; the client is.
//
// Three rules keep the list honest, and each exists for a concrete failure:
//
//   1. CONFIRM before trusting. A marginal signal happily emits one garbled PS
//      ("H?a?t F"), so a name must be seen kConfirmHits times before it is written.
//   2. REPLACE on a new name. Move from Northampton to Manchester and 96.6 carries
//      somebody else — the new name overwrites the old.
//   3. EXPIRE when unheard. That is the case rule 2 CANNOT catch: 96.6 in Manchester
//      might carry NOTHING, so nothing ever contradicts the old bookmark and it would
//      sit on top of static forever. Anything unheard for kExpirySecs is dropped.
//
struct LearnedBm {
    std::string name;            // RDS programme-service name, trimmed
    int         pi = -1;         // RDS PI code (station identity)
    long long   hz = 0;          // the EXACT frequency — the map key is only a rounding
    std::string mode = "wfm";    // RDS learning is FM-only, but an IMPORT carries any mode
    long long   lastHeard = 0;   // unix seconds — drives expiry
    bool        manual = false;  // saved by hand: never expires
};
/**
 * A station we've seen but don't trust yet.
 *
 * The PS is EIGHT characters, sent over and over, and the corruption is RANDOM PER
 * REPETITION — the errors follow the fading, so they land in different places each
 * time. That is exploitable: tally which character turned up at each of the eight
 * positions, and the TRUE character wins every position on sheer frequency.
 * "H%art", "He%rt" and "H**r%" all vote for "Heart".
 *
 * The payoff is that we can reconstruct a name NO SINGLE REPETITION EVER DELIVERED
 * CLEANLY — and it needs no internet, which is the whole point for a receiver on an
 * aerial in a shed.
 */
struct PendingBm {
    int         pi = -1;         // station IDENTITY — the thing we lock on to
    long long   piSince = 0;     // when this PI first appeared
    int         samples = 0;     // PS repetitions tallied
    // votes[position][character] — 8 chars of PS, printable ASCII only.
    unsigned short votes[8][128] = {};
    std::string lastBest;        // last reconstruction, to spot when it settles
    long long   settledSince = 0;
};
static std::mutex g_bmMtx;
static std::map<long long, LearnedBm> g_bookmarks;    // key: Hz (rounded)
/**
 * Where to persist. The shim OWNS these bookmarks, so the shim must SAVE them.
 *
 * They were previously written by the app's JavaScript on a timer — but while the
 * server is serving, the app is BACKGROUNDED, and JS timers there are throttled or
 * suspended outright. So the save frequently never ran at all: an import of 145
 * bookmarks appeared in the list, lived only in memory, and vanished the instant the
 * server restarted. Shortening the timer would not have helped; the timer was the bug.
 * Writing from here, on every change, takes the JS runtime out of the path entirely.
 */
static std::string g_bmPath;
static std::map<long long, PendingBm> g_bmPending;    // awaiting confirmation

// THE PI CODE IS THE STATION'S IDENTITY — the name is only its label.
//
// PI travels in block A of EVERY RDS group, is 16 bits, and is error-protected. The PS
// name is assembled 2 characters at a time across FOUR separate 0A groups, so it is
// both slower to arrive and far more fragile — which is exactly why it garbles. Keying
// on the name would mean treating the flimsy thing as the source of truth.
//
// So we lock on to the PI first, then trust a name only while the PI holds steady:
//
//   * PI CHANGES  -> a different station is on this frequency. That is the Manchester
//                    case, and it is detected IMMEDIATELY, long before a legible name
//                    could ever assemble.
//   * PI STEADY, NAME CHANGING -> same station, corrupted text ("H%art", "He%rt",
//                    "H**r%"). The errors follow the fading, so a garbled name is
//                    garbled DIFFERENTLY each time and can never hold still.
//   * PI STEADY, NAME STEADY for kStableSecs -> believe it.
//
// Tuned against simulated corruption (scripts checked 5-60% character error):
// a 1/2 majority with 6 samples confidently produced "H%art F*" — a corrupt character
// only needs 3 of 6 votes to win. A 2/3 majority over >=10 samples reconstructs
// correctly through 30% corruption and REFUSES TO GUESS beyond ~45%, which is the
// behaviour we want: a wrong name in the list is worse than no name.
static const int       kMinSamples  = 10;                 // PS repetitions before voting
static const long long kPiLockSecs  = 5;                  // PI must be steady this long
static const long long kStableSecs  = 15;                 // reconstruction must hold
static const long long kExpirySecs  = 30LL * 24 * 3600;   // 30 days unheard

/**
 * Round so a few Hz of VFO drift can't create a second entry for one station.
 *
 * 1 kHz, NOT 10 kHz. 10 kHz was chosen for FM, where stations sit on a 100 kHz grid —
 * but shortwave is a 5 kHz grid and medium wave a 9 kHz one, so two genuinely different
 * stations rounded into the same bucket and the second silently destroyed the first.
 * Importing a real UberSDR bookmark list lost 25 entries this way: CB10 (27.075) and
 * CB11 (27.085) both landed on 27.080.
 */
static long long bmKey(double hz) { return (long long)(llround(hz / 1000.0) * 1000LL); }

static void bmSaveLocked();          // defined below; callers hold g_bmMtx

static std::string bmTrim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string bmEsc(const std::string& n) {
    std::string e;
    for (char c : n) { if (c == '"' || c == '\\') e += '\\'; e += c; }
    return e;
}

/** Called from the RDS PS callback. */
static void bmLearn(double hz, int pi, const std::string& psRaw) {
    const std::string ps = bmTrim(psRaw);
    if (hz <= 0 || pi <= 0) return;          // no PI = not locked on to anything
    const long long key = bmKey(hz);
    const long long now = (long long)time(nullptr);

    std::lock_guard<std::mutex> lk(g_bmMtx);
    auto it = g_bookmarks.find(key);

    // Already know this station (same PI) — just keep it alive. Note we do NOT require
    // the name to match: the PI says it's the same station even if this particular PS
    // arrived corrupted, and refusing to refresh on a garbled repetition would let a
    // perfectly audible station quietly expire.
    if (it != g_bookmarks.end() && it->second.pi == pi) {
        it->second.lastHeard = now;
        g_bmPending.erase(key);
        return;
    }

    auto& p = g_bmPending[key];

    // Lock on to the IDENTITY first. A new PI means a different station is here —
    // this is the "moved to Manchester" case, and the PI reveals it immediately.
    if (p.pi != pi) {
        // A different station: every vote we've gathered belongs to the OLD one and
        // is now worse than useless. Wipe the tallies, don't just reset the counter.
        p = PendingBm{};
        p.pi = pi; p.piSince = now;
        // A station change invalidates whatever was bookmarked here. Drop it now
        // rather than let a stale name sit on top of a different broadcaster for
        // however long it takes the new one's text to assemble.
        if (it != g_bookmarks.end() && !it->second.manual) g_bookmarks.erase(it);
        return;
    }
    if (now - p.piSince < kPiLockSecs) return;   // identity not settled yet
    if (ps.empty()) return;                      // locked on, but no text yet

    // Tally this repetition, character by character.
    //
    // NB positions BEYOND the name's length vote for a SPACE, they don't abstain.
    // The PS is eight characters on the air but the engine hands it to us trimmed, so
    // "Heart" arrives as five. Letting positions 5-7 abstain meant they could never
    // reach a majority and the name was NEVER accepted — a station called "Heart"
    // was unlearnable, while an 8-character one worked. Voting a space makes the
    // LENGTH a majority decision too, and the trim below turns it back into "Heart".
    for (int i = 0; i < 8; ++i) {
        unsigned char c = (i < (int)ps.size()) ? (unsigned char)ps[i] : (unsigned char)' ';
        if (c >= 32 && c < 127) p.votes[i][c]++;
    }
    p.samples++;
    if (p.samples < kMinSamples) return;

    // Reconstruct: the winner at each position. A position with no clear winner
    // (under half the samples) is still in dispute — we simply have not heard enough.
    std::string best;
    for (int i = 0; i < 8; ++i) {
        int topC = 0, topN = 0;
        for (int c = 32; c < 127; ++c) {
            if (p.votes[i][c] > topN) { topN = p.votes[i][c]; topC = c; }
        }
        if (topN * 3 < p.samples * 2) return;    // needs a 2/3 majority to win
        best += (char)topC;
    }
    best = bmTrim(best);
    if (best.empty()) return;

    // The reconstruction must then SETTLE — stop changing as more votes arrive.
    if (best != p.lastBest) { p.lastBest = best; p.settledSince = now; return; }
    if (now - p.settledSince < kStableSecs) return;

    LearnedBm b;
    b.name = best; b.pi = pi; b.lastHeard = now;
    b.manual = (it != g_bookmarks.end()) ? it->second.manual : false;
    g_bookmarks[key] = b;
    g_bmPending.erase(key);
    // PERSIST. This was the one mutator that didn't — so a station you sat on for
    // 20 seconds and genuinely learned lived in memory only, and died with the
    // process. bmSaveLocked()'s own comment says it is "called on EVERY change".
    bmSaveLocked();
}

/**
 * Manual "save to server" / import. Never expires.
 *
 * The MODE matters. RDS learning is FM-only so "wfm" is right for a LEARNED station, but
 * an imported list is full of AM, USB, CW and fax — defaulting those to WFM makes every
 * one of them unlistenable. And the EXACT hz is kept: the map key is a rounding, used to
 * group a station despite VFO drift, and emitting it as the frequency put bookmarks up
 * to 500 Hz out — far enough that the VTS (which matches within 99 Hz) never saw them.
 */
static void bmAddManual(double hz, const std::string& name, const std::string& mode) {
    const std::string n = bmTrim(name);
    if (n.empty() || hz <= 0) return;
    std::lock_guard<std::mutex> lk(g_bmMtx);
    LearnedBm b;
    b.name = n;
    b.pi = -1;
    b.hz = (long long)llround(hz);
    b.mode = mode.empty() ? "am" : mode;
    b.lastHeard = (long long)time(nullptr);
    b.manual = true;
    g_bookmarks[bmKey(hz)] = b;
    bmSaveLocked();
}

/** Wipe the lot. An auto-learning list needs a way to be emptied — a wrong or unwanted
 *  station would otherwise sit there for its full 30-day expiry, and a MANUAL entry
 *  never expires at all. */
static void bmClear() {
    std::lock_guard<std::mutex> lk(g_bmMtx);
    g_bookmarks.clear();
    g_bmPending.clear();
    bmSaveLocked();
}

static void bmRemove(double hz) {
    std::lock_guard<std::mutex> lk(g_bmMtx);
    g_bookmarks.erase(bmKey(hz));
    bmSaveLocked();
}

/** Drop anything unheard for kExpirySecs. Manual entries are exempt. */
static void bmPrune() {
    const long long cutoff = (long long)time(nullptr) - kExpirySecs;
    for (auto it = g_bookmarks.begin(); it != g_bookmarks.end(); ) {
        if (!it->second.manual && it->second.lastHeard < cutoff) it = g_bookmarks.erase(it);
        else ++it;
    }
}

/** Restore the saved list at start-up. The APP owns persistence — the shim has no
 *  storage of its own, and this mirrors how the station list already works. */
static void bmLoadJson(const std::string& json) {
    std::lock_guard<std::mutex> lk(g_bmMtx);
    g_bookmarks.clear();
    // Deliberately minimal parsing: this is our OWN serialisation coming back, not
    // arbitrary input, so scan for the fields rather than pull in a JSON library.
    size_t p = 0;
    while ((p = json.find("\"frequency\":", p)) != std::string::npos) {
        long long freq = atoll(json.c_str() + p + 12);
        size_t np  = json.find("\"name\":\"", p);
        size_t lp  = json.find("\"lastHeard\":", p);
        size_t mp  = json.find("\"manual\":", p);
        size_t mdp = json.find("\"mode\":\"", p);
        size_t pip = json.find("\"pi\":", p);
        if (np == std::string::npos || lp == std::string::npos) break;
        size_t ns = np + 8, ne = json.find('"', ns);
        LearnedBm b;
        b.name = json.substr(ns, ne - ns);
        b.pi = (pip != std::string::npos) ? atoi(json.c_str() + pip + 5) : -1;
        b.lastHeard = atoll(json.c_str() + lp + 12);
        b.manual = (mp != std::string::npos) && json.compare(mp + 9, 4, "true") == 0;
        b.hz = freq;                              // the saved value IS the exact frequency
        if (mdp != std::string::npos) {
            size_t ms = mdp + 8, me = json.find('"', ms);
            if (me != std::string::npos) b.mode = json.substr(ms, me - ms);
        }
        if (freq > 0 && !b.name.empty()) g_bookmarks[bmKey((double)freq)] = b;
        p = ne;
    }
    bmPrune();     // a long gap since the last run may have aged some out
    // PERSIST. Without this, a list handed in from the app (an import) replaced the
    // in-memory set but never reached the file, so it evaporated at the next restart —
    // exactly the bug we just finished fixing for the web client's import path.
    bmSaveLocked();
}

/** Serialise WITHOUT taking the lock — callers that already hold it use this. */
static std::string bmJsonLocked() {
    bmPrune();
    std::string j = "[";
    bool first = true;
    for (auto& kv : g_bookmarks) {
        if (!first) j += ",";
        first = false;
        // The EXACT frequency, never the rounded key. The key only groups a station
        // despite VFO drift; emitting it as the frequency shifted every bookmark by
        // up to 500 Hz, and the VTS only recognises a station within 99 Hz — so a
        // bookmark like 17.428100 MHz simply never showed up when tuned to it.
        j += "{\"frequency\":" + std::to_string(kv.second.hz ? kv.second.hz : kv.first)
           + ",\"name\":\"" + bmEsc(kv.second.name) + "\""
           + ",\"pi\":" + std::to_string(kv.second.pi)
           + ",\"lastHeard\":" + std::to_string(kv.second.lastHeard)
           + ",\"manual\":" + (kv.second.manual ? "true" : "false")
           + ",\"mode\":\"" + bmEsc(kv.second.mode) + "\""
           + ",\"source\":\"server\"}";
    }
    return j + "]";
}

static std::string bmJson() {
    std::lock_guard<std::mutex> lk(g_bmMtx);
    return bmJsonLocked();
}

/** Write the list out. Called on EVERY change — a bookmark the user saved and then
 *  lost to a restart is worse than one that was never saved. Caller holds g_bmMtx. */
static void bmSaveLocked() {
    if (g_bmPath.empty()) return;
    const std::string body = bmJsonLocked();
    // Write-and-rename, so a kill mid-write can't leave a truncated file behind and
    // destroy the whole list.
    const std::string tmp = g_bmPath + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    rename(tmp.c_str(), g_bmPath.c_str());
}

// RECEIVER location, served at GET /location. It is the SERVER's location, not the
// client's — a VibeServer might be left at a relative's house, or (once public) be
// listened to from anywhere in the world. Distances, map centring and the ITU
// REGION all have to be computed from where the ANTENNA is; using the viewer's
// position would give nonsense distances and, worse, the wrong region's band edges
// (80m is 3.5-3.8 in R1 but 3.5-4.0 in R2).
static std::mutex  g_locMtx;
static std::string g_locJson;
static std::atomic<bool>   g_vsCompressAudio{true};

// Nonce ledger (single-use, 30 s TTL) + per-IP failure backoff. Small maps: a
// single-client server, so lock contention is trivial.
namespace {
struct VsAuth {
    std::mutex mtx;
    std::mt19937_64 rng{ (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count() };
    std::unordered_map<std::string, int64_t> issued;    // nonce hex -> issue ms
    struct Fail { int count = 0; int64_t until = 0; };  // lockout epoch ms
    std::unordered_map<std::string, Fail> fails;

    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    // A nonce is a REUSABLE session credential (not single-use): the client
    // presents the same nonce+token on BOTH the spectrum and audio WebSockets and
    // on reconnects. Valid for 1 hour, then it must be re-fetched. HMAC still
    // proves PIN knowledge without the secret crossing the wire; the (bounded)
    // replay window is acceptable on a LAN — confidentiality is the VPN's job.
    void prune(int64_t now) {                            // drop nonces >1 h old
        for (auto it = issued.begin(); it != issued.end();)
            it = (now - it->second > 3600000) ? issued.erase(it) : std::next(it);
    }
    std::string issue() {
        std::lock_guard<std::mutex> lk(mtx);
        int64_t now = nowMs(); prune(now);
        uint8_t raw[16];
        uint64_t a = rng(), b = rng();
        memcpy(raw, &a, 8); memcpy(raw + 8, &b, 8);
        std::string hex = toHex(raw, 16);
        issued[hex] = now;
        return hex;
    }
    bool blocked(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = fails.find(ip);
        return it != fails.end() && nowMs() < it->second.until;
    }
    /** Seconds left on this IP's lockout, 0 if none. The CLIENT needs this: a WebSocket
     *  error carries no status code, so without being told, a locked-out browser reports
     *  "wrong PIN" and the user retypes a perfectly correct one forever. */
    int blockedFor(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = fails.find(ip);
        if (it == fails.end()) return 0;
        int64_t left = it->second.until - nowMs();
        return left > 0 ? (int)((left + 999) / 1000) : 0;
    }
    void recordFail(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mtx);
        auto& f = fails[ip];
        f.count++;
        if (f.count >= 3) {                              // backoff: 2s,4s,8s… ≤60s
            int64_t wait = (int64_t)2000 << std::min(f.count - 3, 5);
            f.until = nowMs() + std::min(wait, (int64_t)60000);
        }
    }
    void recordOk(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mtx); fails.erase(ip);
    }
    // Consume the nonce (single-use) and confirm HMAC(secret, nonce)==token.
    bool verify(const std::string& secret, const std::string& nonce, const std::string& token) {
        std::lock_guard<std::mutex> lk(mtx);
        int64_t now = nowMs(); prune(now);
        auto it = issued.find(nonce);
        if (it == issued.end()) return false;            // unknown/expired
        // NB: reusable within TTL (shared by both WS + reconnects) — do NOT erase.
        uint8_t mac[32];
        hmacSha256((const uint8_t*)secret.data(), secret.size(),
                   (const uint8_t*)nonce.data(), nonce.size(), mac);
        return ctEqual(toHex(mac, 32), token);
    }
};
VsAuth g_vsAuthState;

// Extract a query-string value (?a=1&key=val) from a full HTTP request line.
/** Percent-decode a query value. queryParam() returns it RAW, so a station name
 *  ("Heart FM") arrives as "Heart%20FM" and would be stored with the escape in it. */
std::string urlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') { out += ' '; }
        else if (in[i] == '%' && i + 2 < in.size() &&
                 isxdigit((unsigned char)in[i+1]) && isxdigit((unsigned char)in[i+2])) {
            out += (char)strtol(in.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else out += in[i];
    }
    return out;
}

std::string queryParam(const std::string& reqLine, const char* key) {
    auto q = reqLine.find('?'); if (q == std::string::npos) return "";
    auto sp = reqLine.find(' ', q);
    std::string qs = reqLine.substr(q + 1, sp == std::string::npos ? std::string::npos : sp - q - 1);
    std::string pat = std::string(key) + "=";
    size_t p = 0;
    while (p < qs.size()) {
        size_t amp = qs.find('&', p);
        std::string kv = qs.substr(p, amp == std::string::npos ? std::string::npos : amp - p);
        if (kv.rfind(pat, 0) == 0) return kv.substr(pat.size());
        if (amp == std::string::npos) break;
        p = amp + 1;
    }
    return "";
}
} // namespace

// ── Impl ────────────────────────────────────────────────────────────────────
struct LocalSdrShim::Impl {
    bool decoderOnly = false;             // sidecar mode: decoders only, no RTL
    std::vector<float> pcmResid;          // upsample carry (fractional sample pos)
    double pcmAcc = 0.0;
    // device / params
    rtlsdr_dev_t* dev = nullptr;
    // Our OWN dup() of the USB fd. rtlsdr_open_sys_dev → libusb_wrap_sys_device
    // does NOT take ownership of the fd, so if we used Kotlin's fd directly the
    // detached teardown thread (rtlsdr_cancel_async / joins / rtlsdr_close) would
    // race Kotlin's UsbDeviceConnection.close() on that same fd → use-after-free
    // SIGSEGV. dup() gives us an independent fd to the same open file description:
    // Kotlin closing its copy can't pull the rug from libusb, and we close ours
    // after rtlsdr_close on the teardown thread.
    int usbFd = -1;
    // RTL-TCP source (rtl_tcp protocol over the network, no USB/librtlsdr — so it
    // works on iOS too). When tcpSock is set, the IQ comes from this socket and the
    // hardware setters send rtl_tcp commands instead of calling rtlsdr_*.
    std::shared_ptr<net::Socket> tcpSock;
    std::atomic<bool> tcpRunning{false};

    // SpyServer client path. Mutually exclusive with tcpSock/dev — the shim drives
    // exactly one IQ source. IQ arrives as u8 (we negotiate FORMAT_UINT8), which is
    // byte-identical to what the USB and rtl_tcp paths feed enqueueIq().
    std::unique_ptr<spyserver::SpyServerClient> spy;
    bool useSpy() const { return (bool)spy; }
    std::vector<int> spyGains;             // device gain table (tenths dB)
    int lastGainTenthDb = -1;              // re-applied across a stream restart

    // Wide-waterfall geometry. The server's FFT stream spans maximumBandwidth and
    // is centred on SETTING_FFT_FREQUENCY, INDEPENDENTLY of the narrow IQ we
    // demodulate (verified on the wire — see spyserver/PROTOCOL_NOTES.md). That
    // split is the entire point: 2 MHz of waterfall for ~30 KB/s while the IQ
    // stays narrow enough to stream over cellular.
    double spyFftSpan = 0.0;               // 0 = not a SpyServer session
    uint32_t spyIqFormat = 1;              // FORMAT_UINT8 / FORMAT_INT16, per device resolution
    std::atomic<bool> spyClosed{false};    // server hung up (session limit / tuner taken)
    // Noise floor of our own FFT (engine dBFS) and of the server's frames (its own
    // dB scale). The server's dB is NOT dBFS: raw*range/255 - range put a typical
    // RTL noise floor near -78 dB where the engine reports about -100, so feeding
    // it straight to the waterfall saturated everything above the floor and made
    // the level jump at the zoom handover. Align the two on the floor they share.
    std::atomic<float> iqFloorDb{-100.0f};
    std::atomic<float> spyDbOffset{0.0f};   // smoothed correction, engine dB - server dB
    bool spyOffsetPrimed = false;

    // The server's FFT frames are painted on their OWN thread. emitServerFft()
    // builds a frame and sendWs()s it to the WebView — a blocking socket write. Run
    // that on the IQ reader (which also blocks on jitter-buffer backpressure) and any
    // hesitation in the WebView stops us draining the TCP socket, the server's queue
    // overflows, and the audio stutters. The IQ path must never wait on the display.
    //
    // Latest-frame-wins: a waterfall row we failed to draw in time is worthless, so
    // an undrawn frame is replaced rather than queued.
    std::atomic<bool> spyFftRunning{false};   // NOT tcpRunning: that toggles on a
                                             // decimation restart, which would kill
                                             // the painter permanently.
    std::vector<uint8_t> spyFftPending;
    bool spyFftHasFrame = false;
    std::mutex spyFftMtx;
    std::condition_variable spyFftCv;
    std::thread spyFftThread;

    void spyFftLoop() {
        vibeThreadName("vibe-spy-fft");
        std::vector<uint8_t> frame;
        while (spyFftRunning.load()) {
            {
                std::unique_lock<std::mutex> lk(spyFftMtx);
                spyFftCv.wait_for(lk, std::chrono::milliseconds(200),
                                  [this]{ return spyFftHasFrame || !spyFftRunning.load(); });
                if (!spyFftRunning.load()) break;
                if (!spyFftHasFrame) continue;
                frame.swap(spyFftPending);
                spyFftHasFrame = false;
            }
            emitServerFft(frame.data(), (int)frame.size());
        }
    }

    // Called on the IQ reader thread: copy and hand off. Never blocks.
    void queueServerFft(const uint8_t* bins, int n) {
        if (n <= 0) return;
        {
            std::lock_guard<std::mutex> lk(spyFftMtx);
            spyFftPending.assign(bins, bins + n);
            spyFftHasFrame = true;
        }
        spyFftCv.notify_one();
    }
    std::atomic<double> spyFftCenter{0.0};
    int spyDecim = 0;
    uint32_t spyDbRange = 140;

    // The span the CLIENT thinks it is looking at. Everywhere except SpyServer the
    // display span is just the IQ rate; there it is the server's FFT span.
    double displaySpan() const { return spyFftSpan > 0.0 ? spyFftSpan : sampleRate; }

    int tcpTunerType = 0;
    std::vector<int> tcpGains;            // tuner gains (tenths dB) from the header
    // rtl_tcp 5-byte command: [code][param big-endian u32].
    void sendTcpCmd(uint8_t code, uint32_t param) {
        auto s = tcpSock; if (!s) return;
        uint8_t c[5] = { code, (uint8_t)(param >> 24), (uint8_t)(param >> 16),
                         (uint8_t)(param >> 8), (uint8_t)param };
        s->send(c, 5);
    }
    bool useTcp() const { return (bool)tcpSock; }
    double sampleRate = 2400000.0;
    int    fftSize    = 1024;
    double fftRate    = 20.0;
    std::atomic<double> rtlCenter{100000000.0}; // RTL tuned (dongle) centre — the DC of the capture
    // Last hardware retune caused by a view move (ms). See the zoom handler: a
    // PLL relock per pan message breaks the audio.
    long long lastDongleMoveMs = 0;
    std::atomic<double> viewCenter{100000000.0};// DISPLAY centre — may sit off the dongle centre so
                                                // the user can pan the view across the captured band
                                                // while a station stays tuned (RF-centre marker = dongle).
    std::atomic<double> audioFreq{100000000.0}; // demod dial frequency (VFO)
    std::atomic<int>    rateDivisor{1};
    std::atomic<double> zoomFactor{1.0}; // spectrum zoom: FFT-crop factor (>=1)
    std::string mode = "nfm";
    double demodOffset = 0.0;             // VFO offset for the mode (SSB = ±bw/2)
    // Recursive: setSampleRate holds it across its full teardown+rebuild and then
    // calls buildAudio() (which re-locks). Serialises EVERY audio-chain rebuild
    // (WS handleControl mode/tune AND the JS-driven HW setters) so they can't race
    // the shared resamp/demod members → dsp registerInput() double-init abort.
    std::recursive_mutex modeMtx;
    // Squelch — keys off the pre-AGC tuned-channel power from the FFT (post-demod
    // audio is AGC-flattened, so its level can't gate). channelDb is the peak
    // dB in the demod passband, updated in onFFT.
    std::atomic<bool>  squelchOn{false};
    std::atomic<float> squelchDb{-50.0f};
    std::atomic<double> vfoBwHz{12000.0};   // current demod bandwidth
    std::atomic<float>  channelDb{-200.0f};  // peak passband power (dB), pre-AGC
    // Audio noise reduction (self-contained spectral subtraction). Mono only;
    // off by default. No external resources → can't fail to init.
    std::atomic<bool>  nrOn{false};
    std::atomic<float> nrCpuPct{0.0f};      // rolling CPU% (NR time / wall time)
    std::mutex         nrMtx;
    AudioNR*           nrEng = nullptr;
    double nrBusyNs = 0.0, nrWallNs = 0.0;  // CPU% accumulators

    // Auto notch (NLMS adaptive line enhancer). Mono only; off by default.
    // Listening-path only — applied AFTER the decoder taps so tone decoders
    // (FT8/RTTY/CW) still see un-notched audio.
    std::atomic<bool>  notchOn{false};
    std::mutex         notchMtx;
    AutoNotch*         notchEng = nullptr;

    // V5 engine: the IQ -> {spectrum, audio} chain (replaces SDR++ IQFrontEnd +
    // VFO + demod graph). Fed raw IQ from the USB/TCP worker; calls back with a
    // fftshifted dB row (-> onSpectrum) and float PCM (-> onAudioPcm). Touch only
    // under modeMtx — feed() runs rebuildAudio() inline, so setTune from the WS/HW
    // threads must be serialised against the IQ worker.
    vibedsp::RxPipeline rx;
    std::vector<float> fftAccum;    // running sum for FFT averaging (fftshifted)
    int accumCount = 0;
    std::thread rtlThread;

    // IQ producer/consumer. CRITICAL: rtlsdr_read_async's callback runs on
    // libusb's event-handling thread, so it must return fast — running the heavy
    // DSP (rx.feed: FFT + WFM/RDS demod) inline there starves libusb and corrupts
    // its locks (HW control transfers then stall for seconds and SIGABRT on a
    // "destroyed mutex"). So the USB/TCP reader only CONVERTS + ENQUEUES IQ here;
    // a dedicated dspThread drains the queue and runs rx.feed off the libusb path
    // (mirrors how SDR++ ran the DSP on its own threads).
    std::deque<std::vector<cf32>> iqQueue;
    std::mutex iqMtx;
    std::condition_variable iqCv;
    std::condition_variable iqSpaceCv;          // TCP reader waits here when full
    std::atomic<bool> dspRunning{false};
    std::thread dspThread;
    static constexpr size_t IQ_QUEUE_MAX = 8;   // USB: drop oldest beyond this (overrun)

    // ── Network jitter buffer (TCP path only) ────────────────────────────────
    // USB IQ arrives on a hardware clock and the queue idles near empty, so 8
    // chunks is plenty. NETWORK IQ arrives in bursts around WiFi stalls, and the
    // DSP thread is NOT paced by the audio sink (audio is pushed non-blocking to
    // the WebView over the localhost WS) — it drains as fast as the CPU allows and
    // then blocks on iqCv. So the stream's timing comes straight from the socket:
    // a 200 ms radio stall punches a 200 ms hole in the audio.
    //
    // Fix: prefill a standing backlog before the DSP starts draining. Because the
    // DSP can only consume what arrives (it blocks when empty), that backlog then
    // PERSISTS as a delay line — a stall eats the backlog instead of the audio,
    // and the recovery burst refills it. Costs `prefill` of latency, buys `prefill`
    // of stall tolerance.
    //
    // Sized in samples, not chunks: TCP recv() returns whatever is available, so
    // chunk counts wouldn't pin the latency. cf32 = 8 bytes/sample, so at 2.4 MSPS
    // 250 ms is ~4.8 MB and the 2x cap ~9.6 MB.
    size_t iqQueuedSamples = 0;
    size_t iqPrefillSamples = 0;                // 0 = no prefill (USB path)
    size_t iqMaxSamples     = 0;                // 0 = unused (USB path)
    bool   iqPrefilled      = false;            // set once the backlog is built
    std::atomic<uint64_t> iqDroppedSamples{0};  // client-side overruns (was silent)

    // Network stalls: the socket delivered NOTHING for longer than this. Measured on
    // the reader thread, which is the only place that sees the network directly.
    //
    // Do NOT measure this as "iqQueue went empty" — the DSP drains faster than real
    // time and parks on iqCv, so an empty queue is the normal resting state and
    // would read as a permanent stall.
    static constexpr int64_t kStallMs = 120;    // ~2 WiFi beacon intervals
    std::atomic<uint64_t> netStalls{0};

    // Offset tuning: the RTL is physically tuned HW_OFFSET_HZ ABOVE the logical
    // centre (rtlCenter) so the zero-IF DC spike never lands on the channel —
    // on-carrier AM otherwise breaks up. This is purely internal: the client
    // protocol (rtlCenter as the display centre) is unchanged; we compensate in
    // the VFO offset and shift the displayed FFT crop back by the same amount.
    static constexpr double HW_OFFSET_HZ = 15000.0;

    // Physical DC of the FFT = rtlCenter + HW_OFFSET_HZ, so the VFO (at audioFreq)
    // sits HW_OFFSET_HZ below DC.
    double vfoOffsetNow() { return audioFreq.load() - rtlCenter.load() - HW_OFFSET_HZ + demodOffset; }

    // Margin keeping the VFO inside the usable capture: above the 50 kHz auto-
    // retune threshold AND clear of the RTL anti-alias rolloff (~10%). MUST
    // match the JS client (UberSDRClient panSpan / rfCenter derivation).
    double viewDongleMargin() { return std::max(sampleRate * 0.10, 60000.0); }

    // Dongle (RTL) centre for a requested DISPLAY centre.
    //
    // MINIMAL MOVEMENT: the dongle STAYS PUT unless it actually has to move. It
    // moves only when the VFO would fall out of the usable capture, or when the
    // requested view window would run off the edge of the captured band.
    //
    // The old rule was `clamp(view, vfo±lim)` — i.e. the dongle FOLLOWED the view
    // exactly whenever the view sat within the limit. That made every pan a
    // physical RTL retune (PLL relock) plus an FFT recentre, so panning steered
    // the hardware instead of sliding a window over spectrum we already had. It
    // felt like dragging through treacle, and the RF centre chased the pan.
    //
    // `viewSpan` is the width of the crop being displayed; pass 0 when unknown.
    double dongleForView(double view, double viewSpan) {
        const double lim = sampleRate / 2.0 - viewDongleMargin();
        const double vfo = audioFreq.load();
        const double cur = rtlCenter.load();

        // The VFO must stay inside the usable capture.
        double lo = vfo - lim;
        double hi = vfo + lim;

        // ...and so must the whole visible window, or we'd be showing the band
        // edge / rolloff. (Half-span, less the same anti-alias margin.)
        if (viewSpan > 0.0) {
            const double halfRoom = std::max(0.0,
                sampleRate / 2.0 - viewSpan / 2.0 - viewDongleMargin() * 0.5);
            lo = std::max(lo, view - halfRoom);
            hi = std::min(hi, view + halfRoom);
        }

        // A span wider than the capture can't satisfy both — keep the VFO captured.
        if (lo > hi) return std::min(vfo + lim, std::max(vfo - lim, view));

        // Still legal where we are? Then DON'T MOVE: no retune, no PLL relock, and
        // the pan is a pure crop of spectrum we already have.
        if (cur >= lo && cur <= hi) return cur;

        // We DO have to move. Move DECISIVELY: recentre the dongle on the view
        // (clamped so the VFO stays captured) rather than shuffling it the minimum
        // distance to make this one pan legal.
        //
        // The minimum move is a trap: it leaves the view pinned to the capture
        // edge, so the NEXT pan step needs another retune, and the next — at ~30
        // pan messages a second that's a continuous PLL relock storm. It starves
        // the DSP thread (audio drops out) and the waterfall crawls. Recentring
        // buys a whole capture's worth of headroom, so the following hundred pan
        // steps are free crops again.
        return std::min(hi, std::max(lo, view));
    }

    // Tune the radio to (logical centre + HW_OFFSET_HZ).
    void tuneHw(double logicalCenter) {
        uint32_t hz = (uint32_t)llround(logicalCenter + HW_OFFSET_HZ);
        if (useSpy()) {
            spy->setIqFrequency(hz);
            // The centres are independent, but not UNBOUNDED: the device only covers
            // its own span, so an IQ centre far from the FFT centre forces the server
            // to retune the hardware — silently dragging the FFT with it while we
            // still believe it sits where we parked it, which mislabels every
            // frequency on the waterfall. Keep the FFT with the IQ, recentring only
            // past 40% of the span so the display stays still during ordinary tuning.
            const double fc = spyFftCenter.load();
            if (spyFftSpan > 0.0 && std::fabs((double)hz - fc) > spyFftSpan * 0.4) {
                spy->setFftFrequency(hz);
                spyFftCenter.store((double)hz);
                viewCenter.store((double)hz);   // keep the display over visible spectrum
                // Copy the socket out, THEN send. tuneHw() runs under modeMtx, and
                // holding clientMtx across sendConfig() (which takes sendMtx) would
                // invert the lock order the spectrum path uses.
                std::shared_ptr<net::Socket> sc;
                { std::lock_guard<std::mutex> lk(clientMtx); sc = specClient; }
                if (sc) sendConfig(sc);
            }
        }
        else if (useTcp()) sendTcpCmd(0x01, hz);
        else if (dev) rtlsdr_set_center_freq(dev, hz);
    }

    // audio chain config (the engine itself lives in `rx`). buildAudio() maps the
    // mode string -> these + rx.setTune(); retune/setBandwidth re-issue setTune
    // with the cached mode/bw so a dial move doesn't need a full mode rebuild.
    vibedsp::RxPipeline::Mode rxMode = vibedsp::RxPipeline::Mode::NFM;
    double rxBwHz = 12500.0;
    std::atomic<int> audioChannels{1};
    std::atomic<float> audioGain{1.0f};   // per-mode output trim (AM is hotter)
    double deempTau = 50e-6;   // FM de-emphasis time constant (0 = off); 50us EU / 75us US

    // WFM RDS (fed by the engine's rdsPs/rdsText callbacks) + stereo-pilot lock.
    std::mutex rdsMtx;
    std::string rdsPsName, rdsText;
    int rdsPi = -1;
    int rdsEcc = 0;                          // RDS Extended Country Code (0 = none)
    std::atomic<bool> stereoDetected{false};
    // VibeServer ADPCM encoder state. M = (L+R)/2 stays continuous across
    // mono<->stereo transitions (mono also feeds it (L+R)/2), so the mid channel
    // never resets; S = (L-R)/2 is only sent when a stereo pilot is locked.
    AdpcmState adpcmM, adpcmS;
    // VibeServer wire-byte counters (cumulative), split spectrum vs audio, for the
    // sharing screen's live "what the server is pushing" readout. The rate is
    // computed as a delta between successive getVibeServerStatus() polls.
    std::atomic<uint64_t> vsSpecBytes{0}, vsAudioBytes{0};
    std::mutex vsRateMtx;
    uint64_t vsRateLastSpec = 0, vsRateLastAudio = 0;
    int64_t  vsRateLastMs = 0;
    std::atomic<float> spectrumSnr{0.0f};   // peak−floor (dB), centre vs edges

    // Audio-extension decoder (RTTY etc.) on /ws/dxcluster — fed the demod audio.
    std::mutex decoderMtx;
    FskDecoder* decoder = nullptr;
    WefaxDecoder* wefax = nullptr;          // active image decoder (WEFAX), or null
    // FT8/FT4 digital-spots decoders (independent of the text/image decoders).
    std::mutex spotsMtx;
    Ft8Decoder* ft8 = nullptr;
    Ft8Decoder* ft4 = nullptr;
    bool spotsActive = false;
    int  spotDecim = 0;                      // 48k→12k decimation counter
    float spotAcc = 0.0f;                    // box-average accumulator
    // SSTV image decoder (audio-extension). Runs a video-decode thread, so all
    // dxClient sends are serialised through dxSendMtx.
    SstvDecoder* sstv = nullptr;
    int  sstvDecim = 0; float sstvAcc = 0.0f;
    std::mutex dxSendMtx;
    std::shared_ptr<net::Socket> dxClient;
    std::string decTextBuf;                 // decoded chars awaiting flush (UTF-8)
    std::mutex decBufMtx;

    // server
    std::shared_ptr<net::Listener> listener;
    std::thread acceptThread;
    std::vector<std::thread> connThreads;
    std::mutex connMtx;
    std::atomic<bool> serverRunning{false};
    int port = 0;

    // clients
    std::mutex clientMtx;
    std::shared_ptr<net::Socket> specClient;
    std::shared_ptr<net::Socket> audioClient;
    std::atomic<uint64_t> frameCounter{0};

    std::mutex sendMtx; // serialises all WS writes (both directions are split, sends here)

    // ── Spectrum callback (Stage 3) ────────────────────────────────────────
    // The V5 engine hands us a fftshifted dB row (bin 0 = -fs/2, bins/2 = DC),
    // one per FFT, at FFT_AVG× the emit rate. We block-average FFT_AVG of them to
    // kill shimmer, then crop/zoom to OUT_BINS and key squelch/SNR — all in the
    // fftshifted layout (DC at bins/2), unlike the old raw-order IQFrontEnd path.
    static void specCb(void* ctx, const float* db, int bins) { ((Impl*)ctx)->onSpectrum(db, bins); }
    void onSpectrum(const float* db, int bins) {
        if ((int)fftAccum.size() != bins) { fftAccum.assign(bins, 0.0f); accumCount = 0; }
        for (int i = 0; i < bins; i++) fftAccum[i] += db[i];
        if (++accumCount < FFT_AVG) return;
        float inv = 1.0f / (float)accumCount;
        // Averaged dB at a signed bin offset from DC (DC = bins/2 in fftshifted).
        auto dbAt = [&](int sOff) -> float {
            int idx = bins / 2 + sOff;
            if (idx < 0) idx = 0; else if (idx >= bins) idx = bins - 1;
            return fftAccum[idx] * inv;
        };

        uint64_t n = frameCounter.fetch_add(1);
        int div = rateDivisor.load();
        bool emit = !(div > 1 && (n % (uint64_t)div) != 0);
        std::shared_ptr<net::Socket> sock;
        { std::lock_guard<std::mutex> lk(clientMtx); sock = specClient; }

        // Hybrid waterfall: the IQ FFT only covers `sampleRate` of spectrum. When the
        // user is zoomed out past that (SpyServer only, where displaySpan is wider),
        // the server's FFT stream paints the frame instead — see emitServerFft().
        // Zoomed in, we win: our own FFT of the narrow IQ has far finer bins than
        // the server's ~977 Hz.
        const double shownHz = displaySpan() / zoomFactor.load();
        // ONLY on SpyServer, where displaySpan() is the server's wider FFT span, can
        // the view exceed what our IQ covers. On USB and rtl_tcp displaySpan() IS
        // sampleRate, so at zoom 1.0 this compared sampleRate > 0.95*sampleRate and
        // suppressed every frame — a blank waterfall on the paths that have no
        // server FFT to fall back on.
        if (emit && useSpy() && shownHz > sampleRate * 0.95) emit = false;

        if (emit && sock && sock->isOpen()) {
            // Emit a FIXED OUT_BINS bins (GPU-safe waterfall texture width — a
            // 32768-wide texture exceeds mobile GPU limits and the waterfall
            // silently fails). Map the fine internal FFT (bins) to the output,
            // applying zoom: each output bin covers `step` source bins; peak-hold
            // when downsampling (don't drop narrow carriers).
            double zoom = zoomFactor.load();
            const int outBins = OUT_BINS;
            // Source bins per output bin. Written in terms of the DISPLAY span so it
            // stays correct when that is decoupled from the IQ rate (SpyServer).
            // Reduces to bins/(zoom*outBins) whenever displaySpan == sampleRate.
            const double srcBinHz = sampleRate / (double)bins;
            const double step = (shownHz / (double)outBins) / srcBinHz;  // src bins / out bin
            std::vector<uint8_t> frame(22 + outBins);
            frame[0]='S';frame[1]='P';frame[2]='E';frame[3]='C';frame[4]=0x01;frame[5]=0x03;
            uint64_t ts = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            std::memcpy(&frame[6], &ts, 8);
            // Offset tuning: the physical DC sits HW_OFFSET_HZ above rtlCenter, so
            // shift the crop down by that many source bins to keep the display
            // centred on the logical centre (rtlCenter) — the DC spike then draws
            // HW_OFFSET_HZ off-centre, harmlessly outside the channel.
            const double hwOffsetBin = HW_OFFSET_HZ * (double)bins / sampleRate;
            // The display centre is viewCenter, which may sit off the dongle
            // centre (rtlCenter) — shift the crop by their difference so the user
            // can pan the view across the captured band while the dongle (and the
            // tuned VFO) stay put. dbAt clamps past the capture edge → floor.
            const double viewOffsetBin = (viewCenter.load() - rtlCenter.load()) * (double)bins / sampleRate;
            uint64_t f = (uint64_t)llround(viewCenter.load());   // display centre = view centre
            std::memcpy(&frame[14], &f, 8);
            for (int i = 0; i < outBins; i++) {
                int signedOut = (i <= outBins / 2) ? i : i - outBins;
                double center = signedOut * step - hwOffsetBin + viewOffsetBin;  // signed src offset from DC
                int lo = (int)std::floor(center - step / 2.0);
                int hi = (int)std::ceil(center + step / 2.0);
                if (hi <= lo) hi = lo + 1;
                float best = -1e9f;
                for (int s = lo; s < hi; s++) {
                    float val = dbAt(s);                    // averaged dB
                    if (val > best) best = val;             // peak-hold
                }
                int v = (int)lround(best + 256.0);
                frame[22+i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
            }
            // Station presence (full-span, zoom-independent): a station is a broad
            // hump at centre, static is flat. Compare centre band (±100 kHz, less
            // the ±3 kHz DC spike) to the band edges.
            {
                double binHz = sampleRate / (double)bins;
                int half = std::min(bins / 4, (int)(100000.0 / binHz));
                int skip = std::min(half - 1, std::max(1, (int)(3000.0 / binHz)));
                double cSum = 0; int cN = 0;
                for (int i = skip; i <= half; i++)           { cSum += dbAt(i); cSum += dbAt(-i); cN += 2; }
                double eSum = 0; int eN = 0;
                for (int i = 0; i <= half / 2; i++)          { eSum += dbAt(-(bins/2) + i); eSum += dbAt((bins/2 - 1) - i); eN += 2; }
                spectrumSnr.store((cN && eN) ? (float)(cSum/cN - eSum/eN) : 0.0f);
                // Band-edge average = our own noise floor, in the engine's dBFS.
                // emitServerFft() aligns the server's differently-scaled dB onto
                // this, so the two waterfall sources agree and the colour map is
                // fed the values it was designed for.
                if (eN) iqFloorDb.store((float)(eSum / eN));
            }
            sendWs(sock, 0x2, frame.data(), frame.size());
            vsSpecBytes.fetch_add(frame.size(), std::memory_order_relaxed);
            if (n % 10 == 0) sendFmMeta(sock);   // RDS + stereo ~1/sec
        }
        // Tuned-channel power for squelch (peak dB in the demod passband).
        {
            double binHz = sampleRate / (double)bins;
            int cbin = (int)llround(vfoOffsetNow() / binHz);
            int hw = std::max(1, (int)(vfoBwHz.load() / 2.0 / binHz));
            float peak = -1e9f;
            for (int o = -hw; o <= hw; o++) {
                float v = dbAt(cbin + o);
                if (v > peak) peak = v;
            }
            channelDb.store(peak);
        }
        std::fill(fftAccum.begin(), fftAccum.end(), 0.0f);
        accumCount = 0;
    }

    // Re-negotiate the IQ decimation for the current mode (SpyServer only).
    //
    // Decimation is chosen from the demod bandwidth, so a mode change can demand a
    // different rate — NFM at stage 5 gives 75 kHz of IQ, which cannot carry WFM's
    // 200 kHz. The protocol has no in-place decimation change (stock clients always
    // stop, resend every setting, and restart), so that is what we do. Brief audio
    // gap, exactly as SDR#/SDR++ exhibit.
    //
    // Caller must NOT hold modeMtx: stopDspThread() joins the DSP thread, which
    // takes modeMtx per buffer, so holding it here would deadlock (the same trap
    // setSampleRate() documents). Returns true if the stream was restarted.
    bool spyRetuneDecimation() {
        if (!useSpy()) return false;
        const auto& info = spy->deviceInfo();
        const auto mp = paramsFor(mode);
        const double needHz = std::max(mp.bandwidth * 1.6, 48000.0);
        const uint32_t maxStage = std::max(info.decimationStageCount, info.minimumIQDecimation);
        int decim = (int)info.minimumIQDecimation;      // never below the server's floor
        for (uint32_t st = info.minimumIQDecimation; st <= maxStage; ++st) {
            const double r = (double)info.maximumSampleRate / (double)(1u << st);
            if (r < needHz) break;
            decim = (int)st;
        }
        if (decim == spyDecim) return false;

        const double newRate = (double)info.maximumSampleRate / (double)(1u << decim);
        LOGI("SpyServer mode=%s -> decim %d..%d (%.0f -> %.0f S/s)",
             mode.c_str(), spyDecim, decim, sampleRate, newRate);

        // Quiesce the DSP before the rate changes underneath it, exactly as
        // setSampleRate() does for the USB path.
        tcpRunning.store(false);
        stopDspThread();

        spyDecim   = decim;
        sampleRate = newRate;
        fftSize    = fftSizeForRate(sampleRate);
        iqPrefillSamples = (size_t)(sampleRate * 0.25);
        iqMaxSamples     = iqPrefillSamples * 2;

        const uint32_t iqHz  = (uint32_t)llround(rtlCenter.load() + HW_OFFSET_HZ);
        const uint32_t fftHz = (uint32_t)llround(spyFftCenter.load());
        const uint32_t gainIdx = lastGainTenthDb < 0
            ? (uint32_t)(spyGains.size() / 2)
            : spyserver::SpyServerClient::gainIndexForTenthDb(spyGains, lastGainTenthDb);
        spy->startStream(spyserver::STREAM_MODE_IQ | spyserver::STREAM_MODE_FFT,
                         spyIqFormat, (uint32_t)decim, iqHz, gainIdx, 2048, fftHz);

        rx.stop();
        startEngine();
        startDspThread();
        tcpRunning.store(true);
        { std::lock_guard<std::mutex> lk(clientMtx); if (specClient) sendConfig(specClient); }
        return true;
    }

    // Paint the waterfall from the SERVER's FFT stream (SpyServer wide view).
    // Mirrors onSpectrum's frame format exactly — same SPEC header, same OUT_BINS,
    // same peak-hold downsample — but reads u8 dB bins spanning spyFftSpan around
    // spyFftCenter instead of our own FFT of the IQ.
    //
    // Skipped whenever the view fits inside the IQ window: there onSpectrum's own
    // FFT is far finer (36 Hz vs ~977 Hz bins), so the zoom drum stays smooth.
    void emitServerFft(const uint8_t* bins, int n) {
        if (n <= 1 || spyFftSpan <= 0.0) return;
        const double shownHz = displaySpan() / zoomFactor.load();
        if (shownHz <= sampleRate * 0.95) return;      // zoomed in: IQ FFT owns it

        std::shared_ptr<net::Socket> sock;
        { std::lock_guard<std::mutex> lk(clientMtx); sock = specClient; }
        if (!sock || !sock->isOpen()) return;
        uint64_t frameNo = frameCounter.fetch_add(1);
        int div = rateDivisor.load();
        if (div > 1 && (frameNo % (uint64_t)div) != 0) return;

        const int outBins = OUT_BINS;
        const double srcBinHz = spyFftSpan / (double)n;
        const double step = (shownHz / (double)outBins) / srcBinHz;   // src bins / out bin
        // Signed source-bin offset of the display centre from the FFT centre.
        const double viewOffsetBin = (viewCenter.load() - spyFftCenter.load()) / srcBinHz;

        // u8 -> the server's dB (linear over [-dbRange, 0]).
        const double dbPerCount = (double)spyDbRange / 255.0;
        auto rawDb = [&](int idx) -> double {
            return bins[idx] * dbPerCount - (double)spyDbRange;
        };

        // Calibrate: the server's noise floor, in ITS dB, vs ours in engine dBFS.
        // Median is the floor here — most bins are noise, and it ignores carriers.
        {
            std::vector<uint8_t> sorted(bins, bins + n);
            std::nth_element(sorted.begin(), sorted.begin() + n / 2, sorted.end());
            const double serverFloor = sorted[n / 2] * dbPerCount - (double)spyDbRange;
            const double want = (double)iqFloorDb.load() - serverFloor;
            if (!spyOffsetPrimed) { spyDbOffset.store((float)want); spyOffsetPrimed = true; }
            else spyDbOffset.store((float)(spyDbOffset.load() * 0.95 + want * 0.05));  // slow
        }
        const double off = (double)spyDbOffset.load();

        auto dbAt = [&](int sOff) -> float {
            int idx = n / 2 + sOff;
            if (idx < 0) idx = 0; else if (idx >= n) idx = n - 1;
            return (float)(rawDb(idx) + off);
        };

        std::vector<uint8_t> frame(22 + outBins);
        frame[0]='S';frame[1]='P';frame[2]='E';frame[3]='C';frame[4]=0x01;frame[5]=0x03;
        uint64_t ts = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::memcpy(&frame[6], &ts, 8);
        uint64_t f = (uint64_t)llround(viewCenter.load());
        std::memcpy(&frame[14], &f, 8);
        for (int i = 0; i < outBins; i++) {
            int signedOut = (i <= outBins / 2) ? i : i - outBins;
            double center = signedOut * step + viewOffsetBin;
            int lo = (int)std::floor(center - step / 2.0);
            int hi = (int)std::ceil(center + step / 2.0);
            if (hi <= lo) hi = lo + 1;
            float best = -1e9f;
            for (int sB = lo; sB < hi; sB++) { float v = dbAt(sB); if (v > best) best = v; }
            int v = (int)lround(best + 256.0);
            frame[22+i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
        sendWs(sock, 0x2, frame.data(), frame.size());
        vsSpecBytes.fetch_add(frame.size(), std::memory_order_relaxed);
        if (frameNo % 10 == 0) sendFmMeta(sock);
    }

    // ── Audio callback (Stage 4) ───────────────────────────────────────────
    // The V5 engine delivers float PCM (mono, or interleaved L/R for WFM stereo)
    // at exactly AUDIO_SR. We re-pack it into the {l,r} buffer the existing
    // decoder / NR / notch / squelch / PCM-send body already works on.
    std::vector<stereo_t> audioPack;        // engine PCM -> {l,r} for onAudio()
    static void audioCb(void* ctx, const float* pcm, int frames, int channels, int /*outRate*/) {
        ((Impl*)ctx)->onEnginePcm(pcm, frames, channels);
    }
    void onEnginePcm(const float* pcm, int frames, int channels) {
        if (frames <= 0) return;
        audioPack.resize((size_t)frames);
        if (channels == 2) {
            for (int i = 0; i < frames; i++) { audioPack[i].l = pcm[2*i]; audioPack[i].r = pcm[2*i+1]; }
        } else {
            for (int i = 0; i < frames; i++) { audioPack[i].l = pcm[i]; audioPack[i].r = pcm[i]; }
        }
        onAudio(audioPack.data(), frames, channels);
    }
    // RDS programme-service name / RadioText / stereo-pilot lock from the engine.
    static void rdsPsCb(void* ctx, uint16_t pi, const char* ps8) {
        Impl* t = (Impl*)ctx;
        {
            std::lock_guard<std::mutex> lk(t->rdsMtx);
            t->rdsPi = pi; t->rdsPsName = ps8 ? ps8 : "";
        }
        // Learn the station against the frequency it was heard on. audioFreq is the
        // VFO — the thing actually being listened to — not the dongle centre.
        if (ps8) bmLearn(t->audioFreq.load(), (int)pi, ps8);
    }
    static void rdsTextCb(void* ctx, const char* rt64) {
        Impl* t = (Impl*)ctx; std::lock_guard<std::mutex> lk(t->rdsMtx);
        t->rdsText = rt64 ? rt64 : "";
    }
    static void rdsEccCb(void* ctx, uint8_t ecc) {
        Impl* t = (Impl*)ctx; std::lock_guard<std::mutex> lk(t->rdsMtx);
        t->rdsEcc = ecc;
    }
    static void stereoCb(void* ctx, bool locked) { ((Impl*)ctx)->stereoDetected.store(locked); }

    // Frame + send one block of interleaved int16 PCM to the audio client. On the
    // VibeServer path with compression on it emits IMA-ADPCM (~4:1): mono as one
    // block (format 1), stereo as M/S two-block (format 2, dropping S when the
    // pilot is unlocked). Otherwise raw int16 exactly as UberSDR (format 0) — the
    // loopback local-listen path always takes this branch.
    void sendAudioPcm(const std::shared_ptr<net::Socket>& sock, const int16_t* pcm, int count, int ch) {
        if (count <= 0) return;
        bool compress = g_serveOnLan.load() && g_vsCompressAudio.load();
        if (!compress) {
            // header: [0]=channels, [1]=0 (raw), [2..5]=sampleRate u32 LE, int16 PCM
            std::vector<uint8_t> frame(6 + (size_t)count * ch * 2);
            frame[0] = (uint8_t)ch; frame[1] = 0;
            uint32_t sr = (uint32_t)AUDIO_SR; std::memcpy(&frame[2], &sr, 4);
            std::memcpy(frame.data() + 6, pcm, (size_t)count * ch * 2);
            sendWs(sock, 0x2, frame.data(), frame.size());
            vsAudioBytes.fetch_add(frame.size(), std::memory_order_relaxed);
            return;
        }
        bool stereo = (ch == 2 && stereoDetected.load());
        // header: [0]=out channels, [1]=format (1 mono / 2 M/S), [2..5]=rate,
        // [6..7]=u16 sample count per channel.
        std::vector<uint8_t> frame; frame.reserve(8 + count);
        frame.push_back(stereo ? 2 : 1);
        frame.push_back(stereo ? 2 : 1);
        uint32_t sr = (uint32_t)AUDIO_SR;
        frame.push_back((uint8_t)(sr & 0xff)); frame.push_back((uint8_t)((sr >> 8) & 0xff));
        frame.push_back((uint8_t)((sr >> 16) & 0xff)); frame.push_back((uint8_t)((sr >> 24) & 0xff));
        frame.push_back((uint8_t)(count & 0xff)); frame.push_back((uint8_t)((count >> 8) & 0xff));
        std::vector<int16_t> mid((size_t)count), side;
        if (ch == 2) {
            for (int i = 0; i < count; i++) mid[i] = (int16_t)((pcm[2*i] + pcm[2*i+1]) >> 1);
        } else {
            std::memcpy(mid.data(), pcm, (size_t)count * 2);
        }
        adpcmEncodeBlock(frame, mid.data(), count, adpcmM);
        if (stereo) {
            side.resize((size_t)count);
            for (int i = 0; i < count; i++) side[i] = (int16_t)((pcm[2*i] - pcm[2*i+1]) >> 1);
            adpcmEncodeBlock(frame, side.data(), count, adpcmS);
        }
        sendWs(sock, 0x2, frame.data(), frame.size());
        vsAudioBytes.fetch_add(frame.size(), std::memory_order_relaxed);
    }

    void onAudio(stereo_t* data, int count, int ch) {
        if (count <= 0) return;
        // Feed the audio-extension decoder (mono int16) — runs even with no audio
        // WS client. The decoder's onChar/onState push frames to the dxcluster WS.
        feedDecoder(data, count);
        feedSpots(data, count);

        // Squelch: mute the audio when the tuned-channel power (pre-AGC, from the
        // FFT) is below threshold. Applied AFTER the decoders so they see raw audio.
        if (squelchOn.load() && channelDb.load() < squelchDb.load()) {
            for (int i = 0; i < count; i++) { data[i].l = 0.0f; data[i].r = 0.0f; }
        }

        std::shared_ptr<net::Socket> sock;
        { std::lock_guard<std::mutex> lk(clientMtx); sock = audioClient; }
        if (!sock || !sock->isOpen()) return;

        // Auto notch (mono listening path, opt-in): removes steady tones before NR.
        if (notchOn.load() && ch == 1) {
            std::lock_guard<std::mutex> lk(notchMtx);
            if (!notchEng) notchEng = new AutoNotch();
            std::vector<float> mono((size_t)count);
            for (int i = 0; i < count; i++) mono[i] = data[i].l;
            notchEng->process(mono.data(), count);
            for (int i = 0; i < count; i++) data[i].l = mono[i];
        }

        // Audio NR (mono only, opt-in). Spectral subtraction with STFT latency —
        // output count differs from input, so the NR branch sends its own frame.
        if (nrOn.load() && ch == 1) {
            std::vector<float> nrOut;
            {
                std::lock_guard<std::mutex> lk(nrMtx);
                if (!nrEng) nrEng = new AudioNR();
                std::vector<float> mono((size_t)count);
                for (int i = 0; i < count; i++) mono[i] = data[i].l;
                auto t0 = std::chrono::steady_clock::now();
                nrEng->process(mono.data(), count, nrOut);
                auto t1 = std::chrono::steady_clock::now();
                nrBusyNs += std::chrono::duration<double, std::nano>(t1 - t0).count();
                nrWallNs += (double)count / AUDIO_SR * 1e9;
                if (nrWallNs > 5e8) { nrCpuPct.store((float)(nrBusyNs / nrWallNs * 100.0)); nrBusyNs = nrWallNs = 0.0; }
            }
            if (nrOut.empty()) return;          // still filling the first STFT frame
            int n2 = (int)nrOut.size();
            std::vector<int16_t> pcm0((size_t)n2);
            for (int i = 0; i < n2; i++) {
                int s = (int)lround(nrOut[i] * 32767.0f);
                pcm0[i] = (int16_t)(s < -32768 ? -32768 : (s > 32767 ? 32767 : s));
            }
            sendAudioPcm(sock, pcm0.data(), n2, 1);
            return;
        }

        const float g = audioGain.load();
        auto cvt = [g](float v) -> int16_t {
            int s = (int)lround(v * g * 32767.0f);
            return (int16_t)(s < -32768 ? -32768 : (s > 32767 ? 32767 : s));
        };
        std::vector<int16_t> pcm((size_t)count * ch);
        if (ch == 2) {
            // Stereo lock comes from the engine's pilot-PLL callback (stereoCb),
            // so we just pack the decoded L/R here.
            for (int i = 0; i < count; i++) { pcm[i*2] = cvt(data[i].l); pcm[i*2+1] = cvt(data[i].r); }
        } else {
            for (int i = 0; i < count; i++) pcm[i] = cvt(data[i].l);
        }
        sendAudioPcm(sock, pcm.data(), count, ch);
    }

    // ── Audio-extension decoder (RTTY) ─────────────────────────────────────
    void feedDecoder(stereo_t* data, int count) {
        std::lock_guard<std::mutex> lk(decoderMtx);
        if (!decoder && !wefax && !sstv) return;
        // SSTV runs at 12 kHz — decimate 48k→12k (box-average 4) and feed.
        if (sstv) {
            std::vector<int16_t> dec; dec.reserve((size_t)count/4 + 1);
            for (int i = 0; i < count; i++) {
                sstvAcc += data[i].l;
                if (++sstvDecim >= 4) {
                    int s = (int)lround(sstvAcc / 4.0f * 32767.0f);
                    dec.push_back((int16_t)(s < -32768 ? -32768 : (s > 32767 ? 32767 : s)));
                    sstvDecim = 0; sstvAcc = 0.0f;
                }
            }
            if (!dec.empty()) sstv->process(dec.data(), (int)dec.size());
            return;
        }
        std::vector<int16_t> mono((size_t)count);
        for (int i = 0; i < count; i++) {
            int s = (int)lround(data[i].l * 32767.0f);
            mono[i] = (int16_t)(s < -32768 ? -32768 : (s > 32767 ? 32767 : s));
        }
        if (wefax) { wefax->process(mono.data(), count); return; }
        decoder->process(mono.data(), count);
        // Flush any decoded text to the dxcluster client.
        std::string text;
        { std::lock_guard<std::mutex> bl(decBufMtx); if (!decTextBuf.empty()) { text.swap(decTextBuf); } }
        if (!text.empty()) {
            std::shared_ptr<net::Socket> dx;
            { std::lock_guard<std::mutex> lk2(clientMtx); dx = dxClient; }
            if (dx && dx->isOpen()) {
                std::vector<uint8_t> msg(13 + text.size());
                msg[0] = 0x01;
                uint64_t ts = (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                for (int i = 0; i < 8; i++) msg[1 + i] = (uint8_t)(ts >> ((7 - i) * 8));   // big-endian
                uint32_t len = (uint32_t)text.size();
                msg[9] = (uint8_t)(len >> 24); msg[10] = (uint8_t)(len >> 16);
                msg[11] = (uint8_t)(len >> 8); msg[12] = (uint8_t)len;
                std::memcpy(msg.data() + 13, text.data(), text.size());
                sendWs(dx, 0x2, msg.data(), msg.size());
            }
        }
    }
    void sendDecoderState(int st) {
        std::shared_ptr<net::Socket> dx;
        { std::lock_guard<std::mutex> lk2(clientMtx); dx = dxClient; }
        if (dx && dx->isOpen()) { uint8_t m[2] = { 0x03, (uint8_t)st }; sendWs(dx, 0x2, m, 2); }
    }

    // ── FT8/FT4 digital spots ──────────────────────────────────────────────
    static const char* bandFor(double hz) {
        double m = hz / 1e6;
        if (m >= 1.8  && m < 2.0)   return "160m";
        if (m >= 3.5  && m < 4.0)   return "80m";
        if (m >= 5.3  && m < 5.5)   return "60m";
        if (m >= 7.0  && m < 7.3)   return "40m";
        if (m >= 10.1 && m < 10.15) return "30m";
        if (m >= 14.0 && m < 14.35) return "20m";
        if (m >= 18.0 && m < 18.2)  return "17m";
        if (m >= 21.0 && m < 21.45) return "15m";
        if (m >= 24.8 && m < 25.0)  return "12m";
        if (m >= 28.0 && m < 29.7)  return "10m";
        if (m >= 50.0 && m < 54.0)  return "6m";
        return "";
    }
    void emitSpot(bool isFt4, const std::string& callTo, const std::string& callDe,
                  const std::string& grid, int snr, float audioHz) {
        (void)callTo;
        std::shared_ptr<net::Socket> dx;
        { std::lock_guard<std::mutex> lk2(clientMtx); dx = dxClient; }
        if (!dx || !dx->isOpen()) return;
        double rfHz = audioFreq.load() + audioHz;     // dial (USB) + audio offset
        uint64_t ts = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        char buf[384];
        int n = snprintf(buf, sizeof(buf),
            "{\"type\":\"digital_spot\",\"data\":{\"mode\":\"%s\",\"callsign\":\"%s\","
            "\"snr\":%d,\"frequency\":%.0f,\"band\":\"%s\",\"grid\":\"%s\",\"timestamp\":%llu}}",
            isFt4 ? "FT4" : "FT8", callDe.c_str(), snr, rfHz, bandFor(rfHz),
            grid.c_str(), (unsigned long long)ts);
        if (n > 0) sendText(dx, std::string(buf, (size_t)n));
    }
    void startSpots() {
        std::lock_guard<std::mutex> lk(spotsMtx);
        if (spotsActive) return;
        delete ft8; delete ft4;
        ft8 = new Ft8Decoder(12000, false);
        ft4 = new Ft8Decoder(12000, true);
        ft8->onSpot = [this](const std::string& to, const std::string& de, const std::string& g, int s, float f) { emitSpot(false, to, de, g, s, f); };
        ft4->onSpot = [this](const std::string& to, const std::string& de, const std::string& g, int s, float f) { emitSpot(true,  to, de, g, s, f); };
        spotDecim = 0; spotAcc = 0.0f;
        spotsActive = true;
        LOGI("digital spots (FT8/FT4) started");
    }
    void stopSpots() {
        std::lock_guard<std::mutex> lk(spotsMtx);
        spotsActive = false;
        delete ft8; ft8 = nullptr;
        delete ft4; ft4 = nullptr;
    }
    void feedSpots(stereo_t* data, int count) {
        std::lock_guard<std::mutex> lk(spotsMtx);
        if (!spotsActive) return;
        // Decimate 48k→12k by box-averaging 4 samples (mono).
        std::vector<int16_t> dec;
        dec.reserve((size_t)count / 4 + 1);
        for (int i = 0; i < count; i++) {
            spotAcc += data[i].l;
            if (++spotDecim >= 4) {
                int s = (int)lround(spotAcc / 4.0f * 32767.0f);
                dec.push_back((int16_t)(s < -32768 ? -32768 : (s > 32767 ? 32767 : s)));
                spotDecim = 0; spotAcc = 0.0f;
            }
        }
        if (dec.empty()) return;
        if (ft8) ft8->process(dec.data(), (int)dec.size());
        if (ft4) ft4->process(dec.data(), (int)dec.size());
    }

    // ── Demod chain (re)build ──────────────────────────────────────────────
    // With the V5 engine there are no per-block dsp objects to destroy — the
    // engine reconfigures itself on the next feed() after setTune(). teardownAudio
    // just resets the derived UI state; the engine retains no audio across modes.
    void teardownAudio() {
        std::lock_guard<std::mutex> lk(rdsMtx);
        rdsPsName.clear(); rdsText.clear(); rdsPi = -1; rdsEcc = 0;
        stereoDetected.store(false);
    }

    void buildAudio() {
        std::lock_guard<std::recursive_mutex> lk(modeMtx);
        teardownAudio();
        ModeParams mp = paramsFor(mode);
        audioChannels.store(mp.channels);
        // AM envelope-detected audio (DSB full-carrier) lands ~2x hotter than the
        // SSB/FM paths, so trim it to match level with the rest.
        audioGain.store(mp.kind == ModeParams::AM ? 0.5f : 1.0f);
        // CW: tune a beat-note offset off the carrier so the engine's real-part
        // (SSB) detector produces an audible tone. The USB-side SSB demod passes
        // 0..+bw (it mixes down by bw/2 then low-passes at bw/2), so the carrier
        // must appear at a POSITIVE audio frequency. The channel is tuned to
        // (dial + demodOffset), so a carrier on the dial lands at baseband
        // -demodOffset — meaning we need a NEGATIVE offset to push it up into the
        // passband. Use -bw/2 so the carrier sits at +bw/2 (here +600 Hz with the
        // 1200 Hz CW bandwidth): the CENTRE of the passband, with symmetric ±bw/2
        // filtering around the tone. (A positive offset put the carrier at a
        // negative audio freq the USB filter rejected — silent on-signal, audible
        // only when tuned below the carrier.) A true narrow band-pass around the
        // pitch is a future engine refinement.
        demodOffset = (mp.kind == ModeParams::CW) ? -mp.bandwidth * 0.5 : 0.0;

        rxMode = rxModeFor(mp.kind);
        // Clamp to what the IQ can actually carry. Most public SpyServers cap the
        // streamed IQ rate (some as low as 12 kS/s), so a mode's nominal bandwidth
        // can exceed the whole capture — WFM's 200 kHz on a 12 kS/s stream. Asking
        // the engine for a filter wider than its input is meaningless.
        rxBwHz = std::min(mp.bandwidth, sampleRate * 0.8);
        vfoBwHz.store(mp.bandwidth);
        rx.setTune(vfoOffsetNow(), rxMode, rxBwHz);
        LOGI("audio chain: mode=%s bw=%.0f ch=%d", mode.c_str(), mp.bandwidth, mp.channels);
    }

    // (Re)start the V5 engine at the current sampleRate/fftSize. Wires the
    // spectrum/audio/RDS/stereo callbacks. The engine emits FFT frames at FFT_AVG×
    // the target rate; onSpectrum block-averages FFT_AVG of them.
    void startEngine() {
        vibedsp::RxPipeline::Callbacks cb;
        cb.ctx      = this;
        cb.spectrum = &Impl::specCb;
        cb.audio    = &Impl::audioCb;
        cb.rdsPs    = &Impl::rdsPsCb;
        cb.rdsText  = &Impl::rdsTextCb;
        cb.rdsEcc   = &Impl::rdsEccCb;
        cb.stereo   = &Impl::stereoCb;
        fftAccum.assign(fftSize, 0.0f); accumCount = 0;
        rx.start(sampleRate, fftSize, fftRate * FFT_AVG, (int)AUDIO_SR, cb);
    }

    // FM RDS + stereo status → client (reuses the OWRX metadata display).
    static std::string jsonEscape(const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(c); }
            else if ((unsigned char)c >= 0x20) o.push_back(c);
        }
        return o;
    }
    void sendFmMeta(const std::shared_ptr<net::Socket>& sock) {
        std::string ps, rt; int pi = -1, ecc = 0;
        bool wfm = (mode == "wfm");
        if (wfm) {
            std::lock_guard<std::mutex> lk(rdsMtx);
            ps = rdsPsName; rt = rdsText; pi = rdsPi; ecc = rdsEcc;
        }
        // trim trailing spaces RDS pads with
        auto trim = [](std::string s){ size_t e = s.find_last_not_of(" \t\r\n"); return e==std::string::npos?std::string():s.substr(0,e+1); };
        ps = trim(ps); rt = trim(rt);
        const bool st = wfm && stereoDetected.load();
        // Only send when something actually CHANGED — re-sending identical RDS each
        // second re-triggers the client's notification marquee (text "repopulates"
        // and flickers). Change-detect ps/rt/pi/ecc/stereo and skip otherwise.
        if (ps == lastSentPs_ && rt == lastSentRt_ && pi == lastSentPi_ && ecc == lastSentEcc_ && st == lastSentStereo_) return;
        lastSentPs_ = ps; lastSentRt_ = rt; lastSentPi_ = pi; lastSentEcc_ = ecc; lastSentStereo_ = st;
        char buf[512];
        snprintf(buf, sizeof buf,
            "{\"type\":\"rds\",\"stereo\":%s,\"ps\":\"%s\",\"radiotext\":\"%s\",\"pi\":%d,\"ecc\":%d}",
            st ? "true" : "false",
            jsonEscape(ps).c_str(), jsonEscape(rt).c_str(), pi, ecc);
        sendText(sock, buf);
    }
    // Last RDS values pushed to the client (change-detect to avoid marquee re-trigger).
    std::string lastSentPs_, lastSentRt_; int lastSentPi_ = -2; int lastSentEcc_ = -1; bool lastSentStereo_ = false;

    // retune the demod (and RTL centre if the offset would fall outside span)
    void retune(double freq) {
        // Hold modeMtx across the WHOLE placement (rtlCenter/viewCenter store +
        // tuneHw + rx.setTune), not just rx.setTune. retune() runs on the audio-WS
        // thread while the "zoom" handler runs on the spectrum-WS thread, and BOTH
        // call tuneHw() (an rtl-sdr USB control transfer that is NOT thread-safe).
        // With the placement outside the lock the two threads raced: the tuner PLL
        // landed at a corrupted centre → the station came up a few hundred kHz off
        // and one VFO nudge (a fresh single tune) fixed it. Serialising here means
        // whichever handler runs second re-reads the other's committed audioFreq/
        // rtlCenter and there is never a concurrent hardware tune.
        std::lock_guard<std::recursive_mutex> lk(modeMtx);
        audioFreq.store(freq);
        // Guard band before we recentre the capture. The fixed 50 kHz was fine at
        // 2.4 MSPS but goes NEGATIVE once decimation makes the IQ narrow (SpyServer),
        // which would retune on every tiny nudge.
        double limit = sampleRate / 2.0 - std::min(50000.0, sampleRate * 0.15);
        if (std::fabs(freq - rtlCenter.load()) > limit) {
            // The VFO has tuned outside the captured window — recentre the capture
            // on it so we don't end up showing dead air.
            rtlCenter.store(freq);
            // On SpyServer the display is the server's WIDE FFT, which the narrow IQ
            // window slides underneath. Moving viewCenter here would yank the
            // waterfall sideways on every recentre; the view only follows when the
            // VFO leaves the FFT span entirely (handled in the zoom/pan path).
            if (!useSpy()) viewCenter.store(freq);
            tuneHw(freq);
            // USB/rtl_tcp path: the capture just recentred, but tuneHw only pushes
            // a fresh config to the spectrum client on the SpyServer path. A LOCAL
            // client self-tracks the centre natively; a REMOTE VibeServer client
            // cannot, so without this its waterfall keeps the stale centre and stops
            // following once the VFO leaves the captured band. Tell it authoritatively.
            if (!useSpy()) {
                std::shared_ptr<net::Socket> sc;
                { std::lock_guard<std::mutex> lk(clientMtx); sc = specClient; }
                if (sc) sendConfig(sc);
            }
        }
        rx.setTune(vfoOffsetNow(), rxMode, rxBwHz);
        // New frequency -> drop the cached RDS so a different station doesn't keep
        // showing the previous one's PS/RadioText until its own RDS re-syncs.
        { std::lock_guard<std::mutex> rl(rdsMtx); rdsPsName.clear(); rdsText.clear(); rdsPi = -1; }
        stereoDetected.store(false);
    }

    // ── WebSocket framing ──────────────────────────────────────────────────
    void sendWs(const std::shared_ptr<net::Socket>& sock, uint8_t opcode,
                const uint8_t* payload, size_t len) {
        std::vector<uint8_t> hdr;
        hdr.push_back(0x80 | opcode);
        if (len < 126) hdr.push_back((uint8_t)len);
        else if (len < 65536) { hdr.push_back(126); hdr.push_back((uint8_t)(len>>8)); hdr.push_back((uint8_t)len); }
        else { hdr.push_back(127); for (int i=7;i>=0;i--) hdr.push_back((uint8_t)(len>>(i*8))); }
        std::lock_guard<std::mutex> lk(sendMtx);
        if (!sock->isOpen()) return;
        sock->send(hdr.data(), hdr.size());
        if (len) sock->send(payload, len);
    }
    void sendText(const std::shared_ptr<net::Socket>& sock, const std::string& s) {
        sendWs(sock, 0x1, (const uint8_t*)s.data(), s.size());
    }
    void sendConfig(const std::shared_ptr<net::Socket>& sock) {
        // NB: displaySpan(), not sampleRate. On SpyServer the waterfall is the
        // server's wide FFT while the IQ is narrow, so the client's zoom/pan model
        // must be built on the span it can actually SEE.
        const double span = displaySpan();
        double effective = span / zoomFactor.load();                  // zoom-aware span
        double binBw = effective / (double)OUT_BINS;                  // we emit OUT_BINS bins
        char buf[320];
        // maxBandwidth = full (unzoomed) device span — the client caps zoom-out
        // to this so you can't zoom out past the actual RTL bandwidth.
        snprintf(buf, sizeof buf,
            "{\"type\":\"config\",\"centerFreq\":%lld,\"binCount\":%d,"
            "\"binBandwidth\":%.6f,\"totalBandwidth\":%.1f,\"maxBandwidth\":%.1f}",
            (long long)llround(viewCenter.load()), OUT_BINS, binBw, effective, span);
        sendText(sock, buf);
    }

    // Waterfall zoom: set the FFT-crop factor to match the requested span
    // (binBandwidth*fftSize). Pure display-side crop in onFFT — no IQ
    // decimation, no IQFrontEnd reconfig (which would touch the uninitialised
    // headless core), no effect on audio. Capped so the crop keeps >= 16 bins.
    void setSpan(double binBw) {
        if (binBw <= 0) return;
        // The client sees OUT_BINS bins, so its requested span = binBw*OUT_BINS;
        // zoom = full span / requested span. (Using fftSize here made the reported
        // span 8x too wide → zoom snapped straight back out / wouldn't go deep.)
        double want = sampleRate / (binBw * (double)OUT_BINS);
        double maxZoom = (double)fftSize / 16.0;
        if (want < 1.0) want = 1.0;
        if (want > maxZoom) want = maxZoom;
        zoomFactor.store(want);
    }

    static bool recvN(const std::shared_ptr<net::Socket>& s, uint8_t* buf, size_t n) {
        size_t got = 0;
        while (got < n) { int r = s->recv(buf+got, n-got, true, net::NO_TIMEOUT); if (r <= 0) return false; got += (size_t)r; }
        return true;
    }
    int recvWs(const std::shared_ptr<net::Socket>& s, std::string& out) {
        uint8_t h[2]; if (!recvN(s, h, 2)) return -1;
        int opcode = h[0] & 0x0F; bool masked = h[1] & 0x80; uint64_t len = h[1] & 0x7F;
        if (len == 126) { uint8_t e[2]; if(!recvN(s,e,2)) return -1; len=(e[0]<<8)|e[1]; }
        else if (len == 127) { uint8_t e[8]; if(!recvN(s,e,8)) return -1; len=0; for(int i=0;i<8;i++) len=(len<<8)|e[i]; }
        uint8_t mask[4]={0,0,0,0}; if (masked && !recvN(s,mask,4)) return -1;
        out.resize((size_t)len);
        if (len && !recvN(s,(uint8_t*)out.data(),(size_t)len)) return -1;
        if (masked) for (size_t i=0;i<out.size();i++) out[i] ^= mask[i&3];
        return opcode;
    }

    void handleControl(const std::shared_ptr<net::Socket>& sock, const std::string& msg) {
        std::string type = jsonStr(msg, "type");
        double v;
        if (type == "ping") { sendText(sock, "{\"type\":\"pong\"}"); return; }
        if (type == "set_rate") { if (jsonNum(msg,"divisor",v)) rateDivisor.store(std::max(1,(int)llround(v))); return; }
        if (type == "reset") { zoomFactor.store(1.0); sendConfig(sock); return; }
        if (type == "zoom") { // spectrum view-centre move (+ span via binBandwidth)
            if (jsonNum(msg,"frequency",v) && v > 0) {
                // The requested frequency is the DISPLAY centre. Park the dongle
                // so the VFO stays captured (follow the view, then lock), and let
                // the crop offset (viewCenter − rtlCenter, applied in onSpectrum)
                // carry the view on past the dongle once it's locked. Retune the
                // RTL only when the dongle actually has to move (no per-pan clicks).
                // modeMtx serialises this against retune() on the audio-WS thread —
                // both call the non-thread-safe tuneHw(); racing them corrupted the
                // tuner PLL (off-tune-until-nudged bug). Under the lock, dongleForView
                // reads a consistent audioFreq and there is one hardware tune at a time.
                std::lock_guard<std::recursive_mutex> lk(modeMtx);
                viewCenter.store(v);
                double bb = 0.0;
                double viewSpan = jsonNum(msg, "binBandwidth", bb) && bb > 0
                    ? bb * (double)OUT_BINS
                    : displaySpan() / zoomFactor.load();
                double dongle = dongleForView(v, viewSpan);
                bool moved = std::fabs(dongle - rtlCenter.load()) > 1.0;
                // Cooldown: a hardware retune is a PLL relock, and doing it on every
                // pan message audibly breaks the audio. The crop keeps the display
                // honest in between, so skipping a retune costs nothing visible.
                auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (moved && nowMs - lastDongleMoveMs >= 120) {
                    lastDongleMoveMs = nowMs;
                    rtlCenter.store(dongle);
                    tuneHw(dongle);
                    rx.setTune(vfoOffsetNow(), rxMode, rxBwHz);
                }
            }
            double bb;
            if (jsonNum(msg,"binBandwidth",bb) && bb > 0) setSpan(bb);
            sendConfig(sock);
            return;
        }
        if (type == "tune") {
            std::string m = jsonStr(msg, "mode");
            bool rebuilt = false;
            if (!m.empty() && m != mode) { mode = m; buildAudio(); rebuilt = true; }
            if (jsonNum(msg, "frequency", v) && v > 0) retune(v);
            double lo, hi;
            if (!rebuilt && jsonNum(msg,"bandwidthLow",lo) && jsonNum(msg,"bandwidthHigh",hi)) setBandwidth(hi - lo);
            return;
        }
        if (type == "mode") {
            std::string m = jsonStr(msg, "mode");
            // Decimation is derived from the mode's bandwidth, so re-negotiate it
            // BEFORE rebuilding the audio chain (it may change sampleRate/fftSize).
            if (!m.empty() && m != mode) { mode = m; spyRetuneDecimation(); buildAudio(); }
            return;
        }
        if (type == "bandwidth") {
            double lo, hi, bw;
            if (jsonNum(msg,"bandwidthLow",lo) && jsonNum(msg,"bandwidthHigh",hi)) setBandwidth(hi - lo);
            else if (jsonNum(msg,"bandwidth",bw)) setBandwidth(bw);
            return;
        }
        // ── Hardware controls (VibeServer: the client drives the radio) ──────
        // The serving phone exposes no HW UI; a remote client sends these and the
        // server applies them via the same setters the on-device JS path uses.
        if (type == "gain") {
            if (msg.find("\"auto\":true") != std::string::npos) LocalSdrShim::instance().setGain(-1);
            else if (jsonNum(msg,"value",v)) LocalSdrShim::instance().setGain((int)v);
            return;
        }
        if (type == "biasT") {
            LocalSdrShim::instance().setBiasTee(msg.find("\"on\":true") != std::string::npos); return;
        }
        if (type == "agc") {
            LocalSdrShim::instance().setAgc(msg.find("\"on\":true") != std::string::npos); return;
        }
        if (type == "ppm") { if (jsonNum(msg,"value",v)) LocalSdrShim::instance().setPpm((int)v); return; }
        // Capture sample rate = the spectrum span the server sends. A remote client
        // can widen/narrow it (e.g. drop the rate to ease a struggling link) without
        // touching the server. setSampleRate restarts the IQ stream and pushes a
        // fresh config, so the client's waterfall span updates itself.
        // A PINNED rate ignores the client outright. The client is also TOLD it is
        // pinned (hwinfo.lockedRate) and hides its picker — this check is the
        // enforcement, not the UI: an old client, or a hand-rolled one, must not be
        // able to widen a span the host deliberately narrowed to save CPU.
        if (type == "sampleRate") {
            if (g_serveOnLan.load() && g_vsLockedRate.load() > 0) return;
            if (jsonNum(msg,"value",v) && v > 0) LocalSdrShim::instance().setSampleRate(v);
            return;
        }
        if (type == "directSampling") {
            if (jsonNum(msg,"value",v)) LocalSdrShim::instance().setDirectSampling((int)v); return;
        }
        // ── Audio DSP (squelch / NR / notch / de-emphasis / stereo) ───────────
        // These engines already run server-side; they were only reachable from
        // the on-device JS via JNI, so a REMOTE client (web or phone) had no way
        // to touch them. Exposing them here gives both remote clients the same
        // audio controls the local app has — and keeps the DSP server-side, so
        // the client stays a thin renderer (no duplicate DSP to drift).
        if (type == "squelch") {
            // db <= -100 means "off", matching the app's own convention.
            if (jsonNum(msg, "db", v))
                LocalSdrShim::instance().setSquelch(v > -100.0, (float)v);
            return;
        }
        if (type == "nr") {
            LocalSdrShim::instance().setNR(msg.find("\"on\":true") != std::string::npos);
            if (jsonNum(msg, "strength", v))
                LocalSdrShim::instance().setNrStrength((float)std::max(0.0, std::min(1.0, v)));
            return;
        }
        if (type == "notch") {
            LocalSdrShim::instance().setNotch(msg.find("\"on\":true") != std::string::npos); return;
        }
        if (type == "deemph") {
            // tau in SECONDS (0 = off, 50e-6 or 75e-6).
            if (jsonNum(msg, "tau", v)) LocalSdrShim::instance().setDeemphasis(v);
            return;
        }
        if (type == "stereo") {
            LocalSdrShim::instance().setStereoEnabled(msg.find("\"on\":true") != std::string::npos); return;
        }
        // Live spectrum frame rate — the client throttles this when the user goes
        // idle, so an unattended (solar/battery) server stops burning CPU and
        // Wi-Fi on a waterfall nobody is looking at. Audio keeps running.
        if (type == "fftRate") {
            if (jsonNum(msg, "value", v) && v > 0) LocalSdrShim::instance().setFftRate(v);
            return;
        }
    }

    // VibeServer: advertise the tuner's supported gains to the client so its gain
    // slider has real dB steps (it can't query the remote device natively).
    void sendHwInfo(const std::shared_ptr<net::Socket>& sock) {
        std::vector<int> gains = LocalSdrShim::instance().getTunerGains();
        std::string j = "{\"type\":\"hwinfo\",\"gains\":[";
        for (size_t i = 0; i < gains.size(); i++) { if (i) j += ','; j += std::to_string(gains[i]); }
        // Capture sample rates this server offers (= the spectrum spans the client
        // may pick). These are the rates built into THIS server, so the client's
        // picker aligns with the server rather than a generic RTL-TCP list.
        // NO 3.2 MSPS. The RTL2832U will happily ACCEPT the rate and then fail to
        // sustain it: above ~2.56 MSPS the USB transfers can't keep up, so it drops
        // samples and runs hot doing it. It looked like the biggest number in the
        // list — it was the first entry — so it was the one a curious user reached
        // for, and the dropped samples then read as a bad receiver rather than a bad
        // setting. 2.56 is the real ceiling; offer that instead.
        j += "],\"rates\":[2560000,2400000,1800000,1200000,960000]";
        // A pinned rate is advertised so the client can HIDE its rate picker and say
        // who set it, rather than offering a control whose every use is silently
        // dropped. 0 = client-controlled (the default).
        { double lr = g_serveOnLan.load() ? g_vsLockedRate.load() : 0.0;
          j += ",\"lockedRate\":" + std::to_string((long long)(lr > 0 ? lr : 0)); }
        j += "}";
        sendText(sock, j);
    }

    void setBandwidth(double bw) {
        if (bw <= 0) return;
        // VibeServer bandwidth cap: a serving host may limit demod bandwidth to
        // save CPU / wire data; the client's wider request is clamped server-side.
        { double cap = g_vsMaxBandwidth.load();
          if (g_serveOnLan.load() && cap > 0 && bw > cap) bw = cap; }
        std::lock_guard<std::recursive_mutex> lk(modeMtx);
        rxBwHz = std::min(bw, sampleRate * 0.8);
        vfoBwHz.store(rxBwHz);
        // CW: ignore the client's narrow passband override (cwu/cwl send ±200 Hz =
        // 400 Hz wide). With the USB demod the carrier must sit at a POSITIVE audio
        // freq inside the 0..bw passband; a 400 Hz filter forces the beat note down
        // to ~200 Hz, which a phone speaker barely reproduces (it sounded silent).
        // Keep the mode's fixed CW filter (buildAudio's 1200 Hz) and -bw/2 beat-note
        // offset so the tone stays a clear ~600 Hz, centred, audible on-signal.
        if (rxMode == vibedsp::RxPipeline::Mode::CW) {
            rxBwHz = std::min(paramsFor(mode).bandwidth, sampleRate * 0.8);  // restore CW filter width
            vfoBwHz.store(rxBwHz);
            demodOffset = -rxBwHz * 0.5;
            rx.setTune(vfoOffsetNow(), rxMode, rxBwHz);
            return;
        }
        rx.setTune(vfoOffsetNow(), rxMode, rxBwHz);
    }

    // ── HTTP/WS server ─────────────────────────────────────────────────────
    void acceptLoop() {
        vibeThreadName("vibe-accept");
        while (serverRunning.load()) {
            std::shared_ptr<net::Socket> sock;
            try { sock = listener->accept(nullptr, 500); } catch (...) { sock = nullptr; }
            if (!sock) continue;
            std::lock_guard<std::mutex> lk(connMtx);
            connThreads.emplace_back([this, sock]{ handleConnection(sock); });
        }
    }

    // Returns true if this WS may upgrade: no PIN set, or a valid single-use
    // HMAC token in the query string. On failure sends 401, records backoff.
    bool vsAuthOk(const std::shared_ptr<net::Socket>& sock, const std::string& reqLine) {
        std::string secret; { std::lock_guard<std::mutex> lk(g_vsMtx); secret = g_vsSecret; }
        if (secret.empty()) return true;                 // open access
        std::string ip = sock->peerAddress();
        if (g_vsAuthState.blocked(ip)) {
            sock->sendstr("HTTP/1.1 429 Too Many Requests\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
            LOGI("VibeServer auth: %s locked out (backoff)", ip.c_str());
            return false;
        }
        std::string nonce = queryParam(reqLine, "vs_nonce");
        std::string token = queryParam(reqLine, "vs_auth");
        if (!nonce.empty() && !token.empty() &&
            g_vsAuthState.verify(secret, nonce, token)) {
            g_vsAuthState.recordOk(ip);
            return true;
        }
        g_vsAuthState.recordFail(ip);
        sock->sendstr("HTTP/1.1 401 Unauthorized\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
        LOGI("VibeServer auth: %s rejected", ip.c_str());
        return false;
    }

    void handleConnection(std::shared_ptr<net::Socket> sock) {
        vibeThreadName("vibe-conn");
        std::string reqLine, line, wsKey;
        if (sock->recvline(reqLine, 8192, 5000) <= 0) { sock->close(); return; }
        while (sock->recvline(line, 8192, 5000) > 0) {
            if (line.empty() || line == "\r") break;
            if (line.size() > 18) {
                std::string lk = line.substr(0, 18);
                for (auto& c : lk) c = (char)tolower(c);
                if (lk == "sec-websocket-key:") {
                    auto vv = line.substr(18);
                    size_t a = vv.find_first_not_of(" \t");
                    size_t b = vv.find_last_not_of(" \t\r\n");
                    if (a != std::string::npos) wsKey = vv.substr(a, b - a + 1);
                }
            }
        }
        bool wsSpec  = reqLine.find("/ws/user-spectrum") != std::string::npos;
        bool wsAudio = reqLine.find("/ws/audio") != std::string::npos;
        bool wsDx    = reqLine.find("/ws/dxcluster") != std::string::npos;

        // VibeServer PIN pre-flight: the client fetches a nonce here, computes
        // HMAC(pin, nonce), then opens the WS with ?vs_nonce=&vs_auth=. When no
        // PIN is set we say so (required:false) and everything behaves as UberSDR.
        if (reqLine.find("/vibeserver/auth") != std::string::npos) {
            std::string secret; { std::lock_guard<std::mutex> lk(g_vsMtx); secret = g_vsSecret; }
            std::string body;
            if (secret.empty()) body = "{\"required\":false}";
            else {
                // Report a lockout HERE. The WS upgrade answers 429, but a WebSocket
                // error gives the browser no status code at all — so a locked-out client
                // could only guess, and it guessed "wrong PIN", leaving the user retyping
                // a perfectly correct one until the backoff expired.
                int wait = g_vsAuthState.blockedFor(sock->peerAddress());
                body = "{\"required\":true,\"nonce\":\"" + g_vsAuthState.issue() + "\""
                     + ",\"lockedFor\":" + std::to_string(wait) + "}";
            }
            sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                          "Access-Control-Allow-Origin: *\r\nConnection: close\r\nContent-Length: "
                          + std::to_string(body.size()) + "\r\n\r\n" + body);
            sock->close();
            return;
        }

        if (wsDx && !wsKey.empty()) {
            // PIN-gate this like the other sockets. It was open: anyone who could
            // reach the port could attach decoders and start the FT8 engine without
            // the PIN — burning the host's CPU on an unattended (solar) server.
            if (!vsAuthOk(sock, reqLine)) { sock->close(); return; }
            acceptDxcluster(sock, wsKey);
        } else if ((wsSpec || wsAudio) && !wsKey.empty()) {
            if (!vsAuthOk(sock, reqLine)) { sock->close(); return; }
            acceptWs(sock, wsKey, wsAudio);
        } else if (reqLine.find("/connection") != std::string::npos) {
            std::string body = "{\"allowed\":true}";
            sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                          "Access-Control-Allow-Origin: *\r\nConnection: close\r\nContent-Length: "
                          + std::to_string(body.size()) + "\r\n\r\n" + body);
            sock->close();
        } else if (reqLine.rfind("GET /location", 0) == 0) {
            // The RECEIVER's coarse position (or a city the host picked). Clients
            // use it for spot distances, map centring and the ITU region — all of
            // which are properties of the ANTENNA, not the listener.
            std::string body;
            { std::lock_guard<std::mutex> lk(g_locMtx); body = g_locJson; }
            if (body.empty()) body = "{}";
            sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: "
                          + std::to_string(body.size()) + "\r\n\r\n" + body);
            sock->close();
        } else if (reqLine.rfind("GET /stations", 0) == 0) {
            // Station list for the web client's search: the EiBi schedule the APP
            // already downloaded and cached, handed to the shim when Server mode
            // starts (setStationsJson).
            //
            // The browser can't fetch EiBi itself — eibispace.de sends no
            // Access-Control-Allow-Origin, and unlike React Native a browser
            // enforces CORS. Serving the app's cached copy from here is same-origin,
            // so it just works, AND it needs no internet at query time (the
            // allotment case). Empty until the app supplies it.
            std::string body;
            { std::lock_guard<std::mutex> lk(g_stationsMtx); body = g_stationsJson; }
            if (body.empty()) body = "[]";
            sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: "
                          + std::to_string(body.size()) + "\r\n\r\n" + body);
            sock->close();
        } else if (reqLine.rfind("GET /favicon", 0) == 0) {
            // A REAL file, not the data: URI in the page. Safari refuses data: URI
            // favicons outright and silently shows its own default arrow instead, so
            // the icon has to come from a URL. ~1 KB, compiled in beside the page.
            std::string body((const char*)kVibeFavicon, kVibeFaviconLen);
            sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Cache-Control: max-age=86400\r\nConnection: close\r\nContent-Length: "
                          + std::to_string(body.size()) + "\r\n\r\n" + body);
            sock->close();
        } else if (reqLine.rfind("GET /bookmarks", 0) == 0) {
            // Stations this receiver has actually HEARD, learned from RDS, plus any
            // saved by hand. Expired entries are pruned on the way out (see bmPrune).
            std::string body = bmJson();
            sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: "
                          + std::to_string(body.size()) + "\r\n\r\n" + body);
            sock->close();
        } else if (reqLine.rfind("POST /bookmarks", 0) == 0 ||
                   reqLine.rfind("DELETE /bookmarks", 0) == 0) {
            // WRITE path — "save to server" / "remove from server".
            //
            // Gated on the SAME PIN that guards the stream today. When public servers
            // arrive this becomes the admin credential instead: the gate moves, the
            // shape does not, and the client already hides its write buttons unless
            // this call would succeed.
            if (!vsAuthOk(sock, reqLine)) return;        // vsAuthOk already sent 401

            const bool remove = (reqLine.rfind("DELETE", 0) == 0);
            double hz = atof(queryParam(reqLine, "frequency").c_str());
            std::string name = urlDecode(queryParam(reqLine, "name"));
            if (hz <= 0) {
                sock->sendstr("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
                sock->close();
                return;
            }
            if (remove) bmRemove(hz);
            else bmAddManual(hz, name, urlDecode(queryParam(reqLine, "mode")));

            std::string body = bmJson();
            sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: "
                          + std::to_string(body.size()) + "\r\n\r\n" + body);
            sock->close();
        } else if (reqLine.rfind("GET / ", 0) == 0 || reqLine.rfind("GET /index.htm", 0) == 0) {
            // VibeServer web client. Compiled in (vibe_web_page.h) because a phone
            // has nowhere to serve files FROM — one self-contained page, no assets,
            // no second request. Matched on the request-line PREFIX, not a substring:
            // every other route here is a substring test, and a bare "/" would match
            // all of them.
            if (!g_vsWebEnabled.load()) {
                // Host turned the web client off: app-only. Say so in plain words —
                // a bare 404 reads as "wrong address" and sends people hunting.
                static const std::string kOff =
                    "<!doctype html><meta charset=utf-8>"
                    "<title>VibeSDR</title>"
                    "<body style=\"background:#080601;color:#ffb833;font:16px ui-monospace,monospace;"
                    "display:flex;align-items:center;justify-content:center;height:100vh;margin:0;text-align:center\">"
                    "<div><h1 style=\"letter-spacing:6px\">VibeSDR</h1>"
                    "<p>This server does not serve the web client.<br>Connect with the VibeSDR app.</p></div>";
                sock->sendstr("HTTP/1.1 403 Forbidden\r\nContent-Type: text/html; charset=utf-8\r\n"
                              "Connection: close\r\nContent-Length: "
                              + std::to_string(kOff.size()) + "\r\n\r\n" + kOff);
                sock->close();
                return;
            }
            static const std::string kPage(kVibeWebPage);
            sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: "
                          + std::to_string(kPage.size()) + "\r\n\r\n" + kPage);
            sock->close();
        } else {
            sock->sendstr("HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
            sock->close();
        }
    }

    void acceptWs(std::shared_ptr<net::Socket> sock, const std::string& wsKey, bool isAudio) {
        std::string acc = wsKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        uint8_t digest[20]; Sha1().hash((const uint8_t*)acc.data(), acc.size(), digest);
        sock->sendstr("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: " + base64(digest, 20) + "\r\n\r\n");

        if (isAudio) { std::lock_guard<std::mutex> lk(clientMtx); audioClient = sock; LOGI("audio WS connected"); }
        else { std::lock_guard<std::mutex> lk(clientMtx); specClient = sock; sendConfig(sock); sendHwInfo(sock); LOGI("spectrum WS connected"); }

        while (serverRunning.load() && sock->isOpen()) {
            std::string payload;
            int op = recvWs(sock, payload);
            if (op < 0 || op == 0x8) break;
            if (op == 0x9) { sendWs(sock, 0xA, (const uint8_t*)payload.data(), payload.size()); continue; }
            if (op == 0x1) handleControl(sock, payload);
        }
        { std::lock_guard<std::mutex> lk(clientMtx);
          if (specClient == sock) specClient = nullptr;
          if (audioClient == sock) audioClient = nullptr; }
        sock->close();
        LOGI("%s WS disconnected", isAudio ? "audio" : "spectrum");
    }

    void startDecoder(const std::string& msg) {
        std::string ext = jsonStr(msg, "extension_name");
        if (ext == "wefax") { startWefax(msg); return; }
        if (ext == "sstv")  { startSstv(msg);  return; }
        bool navtex = (ext == "navtex");
        if (ext != "fsk" && !navtex) return;   // RTTY / NAVTEX
        double cf, sh, baud; bool inv = msg.find("\"inverted\":true") != std::string::npos;
        if (!jsonNum(msg, "center_frequency", cf)) cf = navtex ? 500.0 : 1000.0;
        if (!jsonNum(msg, "shift", sh)) sh = navtex ? 170.0 : 170.0;
        if (!jsonNum(msg, "baud_rate", baud)) baud = navtex ? 100.0 : 45.45;
        std::string enc = jsonStr(msg, "encoding"); if (enc.empty()) enc = navtex ? "CCIR476" : "ITA2";
        std::string framing = jsonStr(msg, "framing"); if (framing.empty()) framing = navtex ? "4/7" : "5N1.5";
        std::lock_guard<std::mutex> lk(decoderMtx);
        delete decoder;
        decoder = new FskDecoder(48000, cf, sh, baud, framing, enc, inv);
        decoder->onChar = [this](char32_t ch) {
            std::lock_guard<std::mutex> bl(decBufMtx);
            // RTTY/ITA2 is ASCII; encode minimally as UTF-8.
            if (ch < 0x80) decTextBuf.push_back((char)ch);
            else if (ch < 0x800) { decTextBuf.push_back((char)(0xC0|(ch>>6))); decTextBuf.push_back((char)(0x80|(ch&0x3F))); }
        };
        decoder->onState = [this](int st) { sendDecoderState(st); };
        LOGI("decoder attached: fsk cf=%.0f shift=%.0f baud=%.2f enc=%s", cf, sh, baud, enc.c_str());
    }
    void startWefax(const std::string& msg) {
        WefaxDecoder::Config cfg;
        double v;
        if (jsonNum(msg, "lpm", v))         cfg.lpm        = (int)v;
        if (jsonNum(msg, "image_width", v)) cfg.imageWidth = (int)v;
        if (jsonNum(msg, "carrier", v))     cfg.carrier    = v;
        if (jsonNum(msg, "deviation", v))   cfg.deviation  = v;
        if (jsonNum(msg, "bandwidth", v))   cfg.bandwidth  = (int)v;
        cfg.usePhasing = msg.find("\"use_phasing\":false") == std::string::npos;
        cfg.autoStop   = msg.find("\"auto_stop\":true")    != std::string::npos;
        cfg.autoStart  = msg.find("\"auto_start\":true")   != std::string::npos;
        std::lock_guard<std::mutex> lk(decoderMtx);
        delete decoder; decoder = nullptr;
        delete wefax;
        wefax = new WefaxDecoder(48000, cfg);
        wefax->onLine = [this](uint32_t ln, uint32_t w, const uint8_t* px) {
            std::shared_ptr<net::Socket> dx;
            { std::lock_guard<std::mutex> lk2(clientMtx); dx = dxClient; }
            if (!dx || !dx->isOpen()) return;
            std::vector<uint8_t> m(9 + w);
            m[0] = 0x01;
            m[1] = (uint8_t)(ln >> 24); m[2] = (uint8_t)(ln >> 16); m[3] = (uint8_t)(ln >> 8); m[4] = (uint8_t)ln;
            m[5] = (uint8_t)(w >> 24);  m[6] = (uint8_t)(w >> 16);  m[7] = (uint8_t)(w >> 8);  m[8] = (uint8_t)w;
            std::memcpy(m.data() + 9, px, w);
            sendWs(dx, 0x2, m.data(), m.size());
        };
        wefax->onStart = [this]() {
            std::shared_ptr<net::Socket> dx;
            { std::lock_guard<std::mutex> lk2(clientMtx); dx = dxClient; }
            if (dx && dx->isOpen()) { uint8_t b = 0x02; sendWs(dx, 0x2, &b, 1); }
        };
        wefax->onStop = [this]() {
            std::shared_ptr<net::Socket> dx;
            { std::lock_guard<std::mutex> lk2(clientMtx); dx = dxClient; }
            if (dx && dx->isOpen()) { uint8_t b = 0x03; sendWs(dx, 0x2, &b, 1); }
        };
        LOGI("decoder attached: wefax lpm=%d width=%d carrier=%.0f", cfg.lpm, cfg.imageWidth, cfg.carrier);
    }
    // ── SSTV ───────────────────────────────────────────────────────────────
    void dxSend(const uint8_t* d, size_t n) {
        std::shared_ptr<net::Socket> dx;
        { std::lock_guard<std::mutex> lk2(clientMtx); dx = dxClient; }
        if (!dx || !dx->isOpen()) return;
        std::lock_guard<std::mutex> sl(dxSendMtx);
        sendWs(dx, 0x2, d, n);
    }
    static void put32(std::vector<uint8_t>& v, uint32_t x) {
        v.push_back((uint8_t)(x>>24)); v.push_back((uint8_t)(x>>16));
        v.push_back((uint8_t)(x>>8));  v.push_back((uint8_t)x);
    }
    void startSstv(const std::string& msg) {
        (void)msg;
        std::lock_guard<std::mutex> lk(decoderMtx);
        delete decoder; decoder = nullptr;
        delete wefax;   wefax = nullptr;
        delete sstv;
        sstv = new SstvDecoder(12000);
        sstvDecim = 0; sstvAcc = 0.0f;
        sstv->onImageStart = [this](int w, int h) {
            std::vector<uint8_t> m; m.push_back(0x07); put32(m,(uint32_t)w); put32(m,(uint32_t)h);
            dxSend(m.data(), m.size());
        };
        sstv->onLine = [this](int y, int w, const uint8_t* rgb) {
            std::vector<uint8_t> m; m.reserve(9 + (size_t)w*3);
            m.push_back(0x01); put32(m,(uint32_t)y); put32(m,(uint32_t)w);
            m.insert(m.end(), rgb, rgb + (size_t)w*3);
            dxSend(m.data(), m.size());
        };
        sstv->onMode = [this](uint8_t, const std::string& name) {
            std::vector<uint8_t> m; m.push_back(0x02);
            m.push_back((uint8_t)(name.size()>>8)); m.push_back((uint8_t)name.size());
            m.insert(m.end(), name.begin(), name.end());
            dxSend(m.data(), m.size());
        };
        sstv->onStatus = [this](const std::string& s) {
            std::vector<uint8_t> m; m.push_back(0x03); m.push_back(0x00);
            m.push_back((uint8_t)(s.size()>>8)); m.push_back((uint8_t)s.size());
            m.insert(m.end(), s.begin(), s.end());
            dxSend(m.data(), m.size());
        };
        sstv->onSync = [this]() { uint8_t b = 0x04; dxSend(&b, 1); };
        sstv->onComplete = [this]() { std::vector<uint8_t> m; m.push_back(0x05); put32(m,0); dxSend(m.data(), m.size()); };
        sstv->onRedrawStart = [this]() { uint8_t b = 0x08; dxSend(&b, 1); };
        LOGI("decoder attached: sstv");
    }
    void stopDecoder() {
        std::lock_guard<std::mutex> lk(decoderMtx);
        delete decoder; decoder = nullptr;
        delete wefax;   wefax = nullptr;
        delete sstv;    sstv = nullptr;
        { std::lock_guard<std::mutex> bl(decBufMtx); decTextBuf.clear(); }
    }

    void acceptDxcluster(std::shared_ptr<net::Socket> sock, const std::string& wsKey) {
        std::string acc = wsKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        uint8_t digest[20]; Sha1().hash((const uint8_t*)acc.data(), acc.size(), digest);
        sock->sendstr("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: " + base64(digest, 20) + "\r\n\r\n");
        { std::lock_guard<std::mutex> lk(clientMtx); dxClient = sock; }
        LOGI("dxcluster (decoder) WS connected");
        while (serverRunning.load() && sock->isOpen()) {
            std::string payload;
            int op = recvWs(sock, payload);
            if (op < 0 || op == 0x8) break;
            if (op == 0x9) { sendWs(sock, 0xA, (const uint8_t*)payload.data(), payload.size()); continue; }
            if (op != 0x1) continue;
            std::string type = jsonStr(payload, "type");
            if (type == "audio_extension_attach") {
                startDecoder(payload);
                sendText(sock, "{\"type\":\"audio_extension_attached\"}");
            } else if (type == "audio_extension_detach") {
                stopDecoder();
                sendText(sock, "{\"type\":\"audio_extension_detached\"}");
            } else if (type == "subscribe_digital_spots") {
                startSpots();    // local FT8/FT4 decoder feeds digital_spot frames
            } else if (type == "unsubscribe_digital_spots") {
                stopSpots();
            }
            // chat / cw-spot / subscribe_chat messages are ignored (no server here).
        }
        stopDecoder();
        stopSpots();
        { std::lock_guard<std::mutex> lk(clientMtx); if (dxClient == sock) dxClient = nullptr; }
        sock->close();
        LOGI("dxcluster WS disconnected");
    }

    // ── IQ producer (runs on the libusb/socket reader thread) ───────────────
    // Convert `sampCount` interleaved u8 I/Q samples to cf32 and ENQUEUE for the
    // dspThread. Must stay cheap (no DSP, no modeMtx) so the libusb callback
    // returns promptly. Drops the oldest buffer on overrun to bound latency.
    // `blockIfFull`: TCP reader may wait for space (surplus stays in the kernel's
    // receive buffer, where u8 IQ is 4x denser than cf32). The USB callback must
    // NEVER block — blocking libusb's handler stalls the whole device — so it
    // keeps the drop-oldest behaviour.
    void enqueueIq(const uint8_t* buf, int sampCount, bool blockIfFull = false) {
        if (sampCount <= 0) return;
        if (sampCount > STREAM_BUFFER_SIZE) sampCount = STREAM_BUFFER_SIZE;
        std::vector<cf32> v((size_t)sampCount);
        convU8ToF32(buf, reinterpret_cast<float*>(v.data()), sampCount * 2);  // NEON
        {
            std::unique_lock<std::mutex> lk(iqMtx);
            if (iqMaxSamples > 0) {
                if (blockIfFull) {
                    iqSpaceCv.wait(lk, [this]{
                        return iqQueuedSamples < iqMaxSamples || !dspRunning.load();
                    });
                    if (!dspRunning.load()) return;
                } else if (iqQueuedSamples >= iqMaxSamples) {
                    dropOldestLocked();
                }
            } else if (iqQueue.size() >= IQ_QUEUE_MAX) {
                dropOldestLocked();                       // USB: bounded by chunk count
            }
            iqQueuedSamples += v.size();
            iqQueue.push_back(std::move(v));
        }
        iqCv.notify_one();
    }

    // int16 IQ (16-bit devices: Airspy et al). Same queue, same backpressure; only
    // the sample conversion differs. Public SpyServers are not all 8-bit RTL-SDRs,
    // and feeding a 16-bit device's stream through the u8 path would be garbage.
    void enqueueIqInt16(const int16_t* buf, int sampCount, bool blockIfFull) {
        if (sampCount <= 0) return;
        if (sampCount > STREAM_BUFFER_SIZE) sampCount = STREAM_BUFFER_SIZE;
        std::vector<cf32> v((size_t)sampCount);
        constexpr float kInv = 1.0f / 32768.0f;
        for (int i = 0; i < sampCount; i++)
            v[i] = cf32(buf[2*i] * kInv, buf[2*i + 1] * kInv);
        {
            std::unique_lock<std::mutex> lk(iqMtx);
            if (iqMaxSamples > 0) {
                if (blockIfFull) {
                    iqSpaceCv.wait(lk, [this]{
                        return iqQueuedSamples < iqMaxSamples || !dspRunning.load();
                    });
                    if (!dspRunning.load()) return;
                } else if (iqQueuedSamples >= iqMaxSamples) {
                    dropOldestLocked();
                }
            }
            iqQueuedSamples += v.size();
            iqQueue.push_back(std::move(v));
        }
        iqCv.notify_one();
    }

    // Caller holds iqMtx. Counts what it discards — this used to be silent, which
    // is why the server could report a healthy link while the client broke up.
    void dropOldestLocked() {
        if (iqQueue.empty()) return;
        size_t n = iqQueue.front().size();
        iqQueuedSamples -= n;
        iqQueue.pop_front();
        iqDroppedSamples.fetch_add(n, std::memory_order_relaxed);
    }

    // ── DSP consumer (dedicated thread, OFF the libusb path) ────────────────
    // Drains the IQ queue and runs the engine. modeMtx serialises rx.feed against
    // setTune / buildAudio / setSampleRate (feed runs rebuildAudio inline).
    void dspLoop() {
        // This thread runs the whole demod chain (WFM stereo MPX + RDS + FIR
        // filters) and must keep up in real time or the audio it produces
        // underruns → thin/sibilant/"low-bandwidth" sound. It is spawned from the
        // React native-modules (v_native) JNI thread, so WITHOUT this it inherits
        // that thread's name + default scheduling — under the New Architecture
        // that leaves it losing CPU to the Fabric/worklets/JS threads on weak
        // (e.g. Moto G35 / Unisoc) cores. Pin it to real audio priority so the
        // scheduler treats it like the AudioTrack callback (v5/old-arch behaviour).
        vibeAudioThread("vibe-dsp");
        while (dspRunning.load()) {
            std::vector<cf32> buf;
            {
                std::unique_lock<std::mutex> lk(iqMtx);
                // One-shot prefill at stream start. The DSP is NOT paced by this
                // queue — it drains faster than real time and parks on iqCv, so an
                // empty queue here is normal, not starvation. What the prefill buys
                // is 250 ms of extra audio pushed downstream into the WebView's
                // audio buffer, which DOES run on a real-time clock; that buffer is
                // where the jitter actually gets absorbed.
                if (!iqPrefilled && iqPrefillSamples > 0) {
                    iqCv.wait(lk, [this]{
                        return iqQueuedSamples >= iqPrefillSamples || !dspRunning.load();
                    });
                    if (!dspRunning.load()) break;
                    iqPrefilled = true;
                }
                iqCv.wait(lk, [this]{ return !iqQueue.empty() || !dspRunning.load(); });
                if (!dspRunning.load()) break;
                buf = std::move(iqQueue.front());
                iqQueue.pop_front();
                iqQueuedSamples -= buf.size();
            }
            iqSpaceCv.notify_one();
            std::lock_guard<std::recursive_mutex> mlk(modeMtx);
            rx.feed(buf.data(), (int)buf.size());
        }
    }

    void startDspThread() {
        dspRunning.store(true);
        dspThread = std::thread([this]{ dspLoop(); });
    }
    void stopDspThread() {
        dspRunning.store(false);
        iqCv.notify_all();
        iqSpaceCv.notify_all();      // release a TCP reader parked on backpressure
        if (dspThread.joinable()) dspThread.join();
        std::lock_guard<std::mutex> lk(iqMtx);
        iqQueue.clear();
        iqQueuedSamples = 0;
        iqPrefilled = false;
    }

    static void asyncHandler(unsigned char* buf, uint32_t len, void* ctx) {
        ((Impl*)ctx)->enqueueIq(buf, (int)(len / 2));
    }

    // SpyServer read loop: the client owns framing; we just forward IQ into the
    // same queue the USB/rtl_tcp paths feed. Blocks with backpressure exactly like
    // tcpReadLoop, so the 250 ms jitter buffer applies here too.
    void spyReadLoop() {
        vibeThreadName("vibe-spy");
        auto lastData = std::chrono::steady_clock::now();
        spy->run(tcpRunning,
            [&](const uint8_t* data, size_t bytes, uint32_t fmt) {
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastData).count()
                        >= kStallMs)
                    netStalls.fetch_add(1, std::memory_order_relaxed);
                lastData = now;
                // Format is negotiated from the device's ADC resolution, so honour
                // whichever the server is actually sending.
                if (fmt == spyserver::FORMAT_UINT8)
                    enqueueIq(data, (int)(bytes / 2), /*blockIfFull=*/true);
                else if (fmt == spyserver::FORMAT_INT16)
                    enqueueIqInt16((const int16_t*)data, (int)(bytes / 4), /*blockIfFull=*/true);
            },
            [&](const uint8_t* bins, size_t count) { queueServerFft(bins, (int)count); });
        // run() only returns when the link dies or we asked it to stop. If we did
        // not ask, the SERVER hung up: session time limit, or another client took
        // the tuner. Say which rather than reporting a generic connection loss.
        if (tcpRunning.load()) {
            spyClosed.store(true);
            LOGI("SpyServer closed the connection (session limit, or the tuner was taken)");
        }
    }

    // RTL-TCP read loop: pull u8 I/Q from the socket in ~32 KB chunks and enqueue.
    // Reads what's available (low latency) and carries a stray odd byte so I/Q
    // pairs never misalign across reads.
    void tcpReadLoop() {
        vibeThreadName("vibe-tcp");
        const int CHUNK = 32768;                 // bytes (16384 IQ samples)
        std::vector<uint8_t> buf(CHUNK + 1);
        int carry = 0;                            // 0/1 leftover byte from last read
        auto lastData = std::chrono::steady_clock::now();
        while (tcpRunning.load()) {
            auto s = tcpSock; if (!s) break;
            int got = s->recv(buf.data() + carry, CHUNK, false, 5000);
            if (got <= 0) { if (!tcpRunning.load()) break; continue; }
            auto now = std::chrono::steady_clock::now();
            auto gapMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now - lastData).count();
            if (gapMs >= kStallMs) netStalls.fetch_add(1, std::memory_order_relaxed);
            lastData = now;
            int total = carry + got;
            enqueueIq(buf.data(), total / 2, /*blockIfFull=*/true);
            carry = total & 1;                    // keep the trailing half-sample byte
            if (carry) buf[0] = buf[total - 1];
        }
    }
};

// ── Public API ───────────────────────────────────────────────────────────────
// Serialises start()/stop() so concurrent app-teardown calls can't double-free.
static std::mutex g_lifecycle;

// VibeServer LAN-bind opt-in (g_serveOnLan is declared above the Impl struct so
// its members can read it). A separate act rather than a start() parameter: it
// exposes a tuning-control channel, so it must never be a defaulted argument.
void LocalSdrShim::setServeOnLan(bool on) { g_serveOnLan.store(on); }

// mDNS hostname responder — "vibesdr.local". NsdManager publishes a SERVICE, which is
// what the app's Discovered list uses, but a browser resolving a hostname needs an A
// record and NsdManager cannot publish one. See mdns_responder.cpp.
// NB: this file is ALREADY inside `namespace vibe`, so these are declared bare —
// wrapping them in `namespace vibe { }` here would nest to vibe::vibe and fail to link.
void mdnsStart(const std::string& host, const std::string& ipv4);
void mdnsStop();
std::string mdnsHost();

void LocalSdrShim::startMdns(const std::string& host, const std::string& ipv4) {
    mdnsStart(host, ipv4);
}
void LocalSdrShim::stopMdns() { mdnsStop(); }
std::string LocalSdrShim::mdnsHostname() { return mdnsHost(); }
bool LocalSdrShim::serveOnLan() { return g_serveOnLan.load(); }
static const char* bindHost() { return g_serveOnLan.load() ? "0.0.0.0" : "127.0.0.1"; }

// VibeServer server-side config setter definitions (state declared above Impl).
void LocalSdrShim::setVibeServerAuth(const std::string& secret) {
    std::lock_guard<std::mutex> lk(g_vsMtx); g_vsSecret = secret;
}
void LocalSdrShim::setVibeServerLimits(double maxBandwidthHz, double maxFftRate) {
    g_vsMaxBandwidth.store(maxBandwidthHz); g_vsMaxFftRate.store(maxFftRate);
}
void LocalSdrShim::setVibeServerCompressAudio(bool on) { g_vsCompressAudio.store(on); }
void LocalSdrShim::setVibeServerWebEnabled(bool on) { g_vsWebEnabled.store(on); }
void LocalSdrShim::setVibeServerLockedRate(double rate) { g_vsLockedRate.store(rate > 0 ? rate : 0.0); }
void LocalSdrShim::setBookmarksJson(const std::string& json) { bmLoadJson(json); }
void LocalSdrShim::clearBookmarks() { bmClear(); }

/** Give the shim a file to own. It loads immediately and saves on every change. */
void LocalSdrShim::setBookmarksPath(const std::string& path) {
    std::string body;
    {
        std::lock_guard<std::mutex> lk(g_bmMtx);
        g_bmPath = path;
    }
    if (path.empty()) return;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;                       // nothing saved yet — that's fine
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) body.append(buf, n);
    fclose(f);
    if (!body.empty()) bmLoadJson(body);
}
std::string LocalSdrShim::getBookmarksJson() { return bmJson(); }
void LocalSdrShim::setStationsJson(const std::string& json) {
    std::lock_guard<std::mutex> lk(g_stationsMtx);
    g_stationsJson = json;
    LOGI("stations list set (%zu bytes)", json.size());
}
void LocalSdrShim::setLocationJson(const std::string& json) {
    std::lock_guard<std::mutex> lk(g_locMtx);
    g_locJson = json;
    LOGI("receiver location set (%zu bytes)", json.size());
}

LocalSdrShim& LocalSdrShim::instance() { static LocalSdrShim inst; return inst; }

int LocalSdrShim::start(int fd, int vid, int pid,
                        double centerFreq, double sampleRate, int gainTenthDb,
                        int fftSize, double fftRate, const std::string& mode, std::string& err) {
    std::lock_guard<std::mutex> life(g_lifecycle);
    // Recover from a stale shim left by a dirty exit (app swiped away while the
    // foreground service kept the process — and the shim — alive). Without this
    // the new connect got "already running" and wedged on the next launch.
    if (p) { LOGI("stale shim found on start — tearing down"); stopLocked(); }
    auto* impl = new Impl();
    impl->sampleRate = sampleRate;
    impl->fftSize = fftSize;
    impl->fftRate = fftRate;
    // VibeServer waterfall frame-rate throttle: a serving host can cap fps (Full
    // 20 / Half 10 / Quarter 5) to save CPU and wire data. The client interpolates
    // the waterfall, so a lower rate still scrolls smoothly.
    if (g_serveOnLan.load()) {
        double mr = g_vsMaxFftRate.load();
        if (mr > 0 && mr < impl->fftRate) impl->fftRate = mr;
    }
    impl->rtlCenter.store(centerFreq);
    impl->viewCenter.store(centerFreq);
    impl->audioFreq.store(centerFreq);
    impl->mode = mode.empty() ? "nfm" : mode;

    impl->usbFd = dup(fd);
    if (impl->usbFd < 0) { err = "dup(usb fd) failed"; delete impl; return -1; }
    int ret = rtlsdr_open_sys_dev(&impl->dev, (intptr_t)impl->usbFd);
    if (ret != 0 || !impl->dev) { err = "rtlsdr_open_sys_dev failed: " + std::to_string(ret); ::close(impl->usbFd); delete impl; return -1; }
    rtlsdr_set_sample_rate(impl->dev, (uint32_t)sampleRate);
    // Offset tuning: physically tune HW_OFFSET_HZ above the logical centre.
    impl->tuneHw(centerFreq);
    if (gainTenthDb < 0) rtlsdr_set_tuner_gain_mode(impl->dev, 0);
    else { rtlsdr_set_tuner_gain_mode(impl->dev, 1); rtlsdr_set_tuner_gain(impl->dev, gainTenthDb); }
    rtlsdr_reset_buffer(impl->dev);
    // Use the ACTUAL rate the RTL rounded to (keeps the waterfall calibrated).
    uint32_t actualSr = rtlsdr_get_sample_rate(impl->dev);
    if (actualSr > 0) impl->sampleRate = (double)actualSr;
    // FFT size auto-scales with the rate for uniform Hz/bin (matches UberSDR).
    impl->fftSize = fftSizeForRate(impl->sampleRate);

    impl->startEngine();
    impl->buildAudio();

    int chosen = -1;
    for (int port = 48000; port < 48050; port++) {
        try { impl->listener = net::listen(bindHost(), port); chosen = port; break; }
        catch (...) { impl->listener = nullptr; }
    }
    if (!impl->listener) {
        err = "could not bind localhost port";
        impl->teardownAudio(); impl->rx.stop(); rtlsdr_close(impl->dev); ::close(impl->usbFd); delete impl; return -1;
    }
    impl->port = chosen;
    impl->serverRunning.store(true);
    impl->acceptThread = std::thread([impl]{ impl->acceptLoop(); });

    impl->startDspThread();
    {
        const uint32_t bufLen = rtlBufLenForRate(impl->sampleRate);
        impl->rtlThread = std::thread([impl, bufLen]{
            vibeThreadName("vibe-rtl");
            rtlsdr_read_async(impl->dev, &Impl::asyncHandler, impl, 0, bufLen);
        });
    }

    p = impl;
    LOGI("local SDR started: center=%.0f rate=%.0f fft=%d mode=%s port=%d",
         centerFreq, sampleRate, fftSize, impl->mode.c_str(), chosen);
    return chosen;
}

// Standard R820T/R828D tuner gains (tenths of dB) — rtl_tcp's header gives only a
// gain COUNT, not the values, so we expose this well-known table for the slider.
static const int kR820tGains[] = {
    0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254, 280,
    297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496
};

int LocalSdrShim::startTcp(const std::string& host, int port,
                           double centerFreq, double sampleRate, int gainTenthDb,
                           int fftSize, double fftRate, const std::string& mode, std::string& err) {
    std::lock_guard<std::mutex> life(g_lifecycle);
    if (p) { LOGI("stale shim found on TCP start — tearing down"); stopLocked(); }
    auto* impl = new Impl();
    impl->sampleRate = sampleRate;
    impl->fftSize = fftSize;
    impl->fftRate = fftRate;
    impl->rtlCenter.store(centerFreq);
    impl->viewCenter.store(centerFreq);
    impl->audioFreq.store(centerFreq);
    impl->mode = mode.empty() ? "nfm" : mode;

    // Connect to the rtl_tcp server and read its 12-byte header.
    try { impl->tcpSock = net::connect(host, port); }
    catch (...) { impl->tcpSock = nullptr; }
    if (!impl->tcpSock) { err = "could not connect to rtl_tcp " + host + ":" + std::to_string(port); delete impl; return -1; }
    // 1 MB receive buffer: absorbs WiFi stalls on the receiving side so the IQ
    // stream doesn't gap when the radio naps. Kernel may clamp; not fatal.
    impl->tcpSock->setRecvBufferSize(1024 * 1024);

    // Network jitter buffer: 250 ms of standing backlog, capped at 500 ms. Enough
    // to ride out a WiFi power-save / retry stall; small enough that retuning still
    // feels responsive. Only the TCP path sets these (USB leaves them at 0).
    impl->iqPrefillSamples = (size_t)(sampleRate * 0.25);
    impl->iqMaxSamples     = impl->iqPrefillSamples * 2;
    uint8_t hdr[12];
    if (impl->tcpSock->recv(hdr, 12, true, 8000) != 12 || memcmp(hdr, "RTL0", 4) != 0) {
        err = "bad rtl_tcp header (not an rtl_tcp server?)"; impl->tcpSock->close(); impl->tcpSock = nullptr; delete impl; return -1;
    }
    impl->tcpTunerType = (hdr[4] << 24) | (hdr[5] << 16) | (hdr[6] << 8) | hdr[7];
    impl->tcpGains.assign(kR820tGains, kR820tGains + (sizeof(kR820tGains) / sizeof(int)));

    // Initial config via rtl_tcp commands (0x02 rate, 0x01 freq, 0x03/0x04 gain).
    impl->sendTcpCmd(0x02, (uint32_t)sampleRate);
    impl->tuneHw(impl->rtlCenter.load());   // offset tuning (HW_OFFSET_HZ above centre)
    if (gainTenthDb < 0) { impl->sendTcpCmd(0x03, 0); }                       // auto
    else { impl->sendTcpCmd(0x03, 1); impl->sendTcpCmd(0x04, (uint32_t)gainTenthDb); }

    impl->fftSize = fftSizeForRate(impl->sampleRate);

    impl->startEngine();
    impl->buildAudio();

    int chosen = -1;
    for (int p2 = 48000; p2 < 48050; p2++) {
        try { impl->listener = net::listen("127.0.0.1", p2); chosen = p2; break; }
        catch (...) { impl->listener = nullptr; }
    }
    if (!impl->listener) {
        err = "could not bind localhost port";
        impl->teardownAudio(); impl->rx.stop();
        impl->tcpSock->close(); impl->tcpSock = nullptr; delete impl; return -1;
    }
    impl->port = chosen;
    impl->serverRunning.store(true);
    impl->acceptThread = std::thread([impl]{ impl->acceptLoop(); });

    impl->startDspThread();
    impl->tcpRunning.store(true);
    impl->rtlThread = std::thread([impl]{ impl->tcpReadLoop(); });

    p = impl;
    LOGI("RTL-TCP started: %s:%d center=%.0f rate=%.0f tuner=%d port=%d",
         host.c_str(), port, centerFreq, sampleRate, impl->tcpTunerType, chosen);
    return chosen;
}

int LocalSdrShim::startSpyServer(const std::string& host, int port,
                                double centerFreq, double sampleRate, int gainTenthDb,
                                int fftSize, double fftRate, const std::string& mode,
                                std::string& err) {
    std::lock_guard<std::mutex> life(g_lifecycle);
    if (p) { LOGI("stale shim found on SpyServer start — tearing down"); stopLocked(); }
    auto* impl = new Impl();
    impl->fftSize = fftSize;
    impl->fftRate = fftRate;
    impl->rtlCenter.store(centerFreq);
    impl->viewCenter.store(centerFreq);
    impl->audioFreq.store(centerFreq);
    impl->mode = mode.empty() ? "nfm" : mode;

    impl->spy = std::make_unique<spyserver::SpyServerClient>();
    if (!impl->spy->connect(host, port, "VibeSDR", err)) { delete impl; return -1; }

    const auto& info = impl->spy->deviceInfo();
    if (info.maximumSampleRate == 0) {
        err = "SpyServer reported no sample rate"; impl->spy->close(); delete impl; return -1;
    }

    // Pick the DEEPEST decimation whose rate still comfortably carries the mode's
    // bandwidth. This is where the bandwidth win lives, and it must be derived from
    // the server's own DEVICE_INFO — public servers are not all RTL-SDRs. An Airspy
    // One runs at 10 MSPS, so a hardcoded "decimation 0" would pull 20 MB/s from a
    // stranger's uplink.
    // 1.6x the demod bandwidth covers the filter skirts and leaves the VFO room to
    // move before retune() has to recentre the IQ window. The 48 kHz floor keeps the
    // audio chain fed on narrow modes (SSB/CW would otherwise pick an absurd stage).
    const auto mp = paramsFor(impl->mode);
    const double needHz = std::max(mp.bandwidth * 1.6, 48000.0);
    // START at minimumIQDecimation, never 0. An Airspy One advertises min stage 5
    // (and 6 MSPS, 12-bit): starting at 0 and only raising the stage when a rate
    // satisfies the bandwidth leaves decim=0 whenever even the minimum stage is too
    // narrow — which for WFM it is. That would pull 24 MB/s off a stranger's server.
    // If no stage is wide enough, take the minimum: it's the widest we're allowed.
    const uint32_t maxStage = std::max(info.decimationStageCount, info.minimumIQDecimation);
    int decim = (int)info.minimumIQDecimation;
    for (uint32_t st = info.minimumIQDecimation; st <= maxStage; ++st) {
        const double r = (double)info.maximumSampleRate / (double)(1u << st);
        if (r < needHz) break;
        decim = (int)st;
    }
    impl->spyDecim   = decim;
    impl->sampleRate = (double)info.maximumSampleRate / (double)(1u << decim);
    impl->fftSize    = fftSizeForRate(impl->sampleRate);

    // Wide waterfall geometry, straight from the server.
    impl->spyFftSpan = info.maximumBandwidth > 0 ? (double)info.maximumBandwidth
                                                 : (double)info.maximumSampleRate;
    impl->spyFftCenter.store(centerFreq);
    impl->spyDbRange = 140;

    // SpyServer sends a bare gain INDEX and never the dB values, so the client must
    // supply a table. Public servers are NOT all RTL-SDRs: an Airspy HF advertises
    // maximumGainIndex = 8, so the 29-entry R820T table would have us sending
    // indices the server has to clamp or reject. Only use it when the device really
    // is an RTL-SDR AND the index count matches; otherwise synthesise a monotonic
    // table of the right length so the slider stays usable and in range.
    const size_t nGains = (size_t)info.maximumGainIndex + 1;
    const size_t rtlGains = sizeof(kR820tGains) / sizeof(int);
    if (info.deviceType == spyserver::DEVICE_RTLSDR && nGains >= rtlGains) {
        impl->spyGains.assign(kR820tGains, kR820tGains + rtlGains);
    } else {
        impl->spyGains.resize(nGains);
        for (size_t i = 0; i < nGains; ++i)                 // evenly spread 0..49.6 dB
            impl->spyGains[i] = (int)llround(496.0 * (double)i / (double)(nGains > 1 ? nGains - 1 : 1));
    }

    // Match the wire format to the device's ADC. uint8 is lossless on an 8-bit
    // RTL-SDR and half the bytes; on a 16-bit Airspy it would discard 8 bits of a
    // considerably better receiver. forcedIQFormat != 0 means the server dictates.
    uint32_t iqFormat = spyserver::FORMAT_UINT8;
    if (info.forcedIQFormat != 0)      iqFormat = info.forcedIQFormat;
    else if (info.resolution > 8)      iqFormat = spyserver::FORMAT_INT16;
    impl->spyIqFormat = iqFormat;
    impl->lastGainTenthDb = gainTenthDb;
    const uint32_t gainIdx = gainTenthDb < 0
        ? (uint32_t)(impl->spyGains.size() / 2)     // no AGC in the protocol: mid-scale
        : spyserver::SpyServerClient::gainIndexForTenthDb(impl->spyGains, gainTenthDb);

    // Same jitter buffer as the rtl_tcp path — decimation shrinks the stream but
    // does nothing about a WiFi stall.
    impl->iqPrefillSamples = (size_t)(impl->sampleRate * 0.25);
    impl->iqMaxSamples     = impl->iqPrefillSamples * 2;

    // IQ carries the offset-tuning shift the DSP expects; the FFT does not (its
    // bins are read straight against spyFftCenter).
    const uint32_t iqHz  = (uint32_t)llround(centerFreq + Impl::HW_OFFSET_HZ);
    const uint32_t fftHz = (uint32_t)llround(centerFreq);
    // 2048 bins over the span: ~977 Hz on a 2 MHz RTL server, ~30 KB/s at 15 fps.
    // Finer than this costs bandwidth for detail the waterfall can't show, and the
    // IQ FFT takes over anyway once you zoom in.
    constexpr uint32_t kFftPixels = 2048;
    if (!impl->spy->startStream(spyserver::STREAM_MODE_IQ | spyserver::STREAM_MODE_FFT,
                                iqFormat, (uint32_t)decim,
                                iqHz, gainIdx, kFftPixels, fftHz)) {
        err = "SpyServer refused the stream settings";
        impl->spy->close(); delete impl; return -1;
    }

    impl->startEngine();
    impl->buildAudio();

    int chosen = -1;
    for (int p2 = 48000; p2 < 48050; p2++) {
        try { impl->listener = net::listen("127.0.0.1", p2); chosen = p2; break; }
        catch (...) { impl->listener = nullptr; }
    }
    if (!impl->listener) {
        err = "could not bind localhost port";
        impl->teardownAudio(); impl->rx.stop();
        impl->spy->close(); delete impl; return -1;
    }
    impl->port = chosen;
    impl->serverRunning.store(true);
    impl->acceptThread = std::thread([impl]{ impl->acceptLoop(); });

    impl->startDspThread();
    impl->tcpRunning.store(true);                 // shared "network source alive" flag
    impl->rtlThread    = std::thread([impl]{ impl->spyReadLoop(); });
    impl->spyFftRunning.store(true);
    impl->spyFftThread = std::thread([impl]{ impl->spyFftLoop(); });

    p = impl;
    LOGI("SpyServer started: %s:%d center=%.0f decim=%d iqRate=%.0f fftSpan=%.0f "
         "control=%d port=%d",
         host.c_str(), port, centerFreq, decim, impl->sampleRate, impl->spyFftSpan,
         (int)impl->spy->canControl(), chosen);
    return chosen;
}

void LocalSdrShim::stop() {
    // Serialise with start()/stop(): app teardown fires stopSpectrum from several
    // Kotlin paths (unmount + invalidate), possibly concurrently — without this
    // two stops grab the same Impl and double-free it (the ~Impl crash on close).
    std::lock_guard<std::mutex> life(g_lifecycle);
    stopLocked();
}

void LocalSdrShim::stopLocked() {
    if (!p) return;
    Impl* impl = p; p = nullptr;

    impl->serverRunning.store(false);
    // Close the client sockets FIRST. The FFT/audio worker threads write to them
    // via sendWs; if a client has stopped reading (e.g. the app is tearing the
    // spectrum/audio WS down while switching to a network instance), that send
    // blocks — and teardownAudio()/frontend->stop() below would then hang joining
    // a worker stuck mid-write. Closing the sockets makes the blocked send/recv
    // fail so every thread can exit. (This was the "shim gets stuck" on
    // local→network — long-standing, just never hit until that path was used.)
    { std::lock_guard<std::mutex> lk(impl->clientMtx);
      if (impl->specClient) impl->specClient->close();
      if (impl->audioClient) impl->audioClient->close();
      if (impl->dxClient) impl->dxClient->close(); }

    // Stop the IQ source. USB: cancel the async read. RTL-TCP: clear the run flag
    // and close the socket so the blocked recv() returns and the read thread exits.
    if (impl->dev) rtlsdr_cancel_async(impl->dev);
    if (impl->useSpy()) {
        impl->tcpRunning.store(false);
        impl->spyFftRunning.store(false);
        impl->spyFftCv.notify_all();
        if (impl->spyFftThread.joinable()) impl->spyFftThread.join();
        impl->spy->close();
    }
    if (impl->useTcp()) { impl->tcpRunning.store(false); if (impl->tcpSock) impl->tcpSock->close(); }
    if (impl->rtlThread.joinable()) impl->rtlThread.join();
    // IQ source stopped -> stop the DSP consumer (drains/clears the queue) before
    // tearing the engine down, so no rx.feed runs against a destroyed engine.
    impl->stopDspThread();
    impl->teardownAudio();
    impl->rx.stop();

    impl->stopDecoder();
    impl->stopSpots();
    { std::lock_guard<std::mutex> lk(impl->nrMtx); delete impl->nrEng; impl->nrEng = nullptr; }
    { std::lock_guard<std::mutex> lk(impl->notchMtx); delete impl->notchEng; impl->notchEng = nullptr; }
    // NOTE: do NOT call listener->stop() here. acceptLoop polls accept() with a
    // 500ms timeout and checks serverRunning, so clearing it (above) exits the
    // loop on its own. net::Listener::stop() isn't idempotent (it closeSocket()s
    // unconditionally) and ~Listener calls stop() again on `delete impl` — an
    // explicit stop() here made that a DOUBLE close of the same fd, which after
    // the number was reused tripped fdsan → SIGABRT on teardown. Let ~Listener
    // close it exactly once.
    if (impl->acceptThread.joinable()) impl->acceptThread.join();
    { std::lock_guard<std::mutex> lk(impl->connMtx);
      for (auto& t : impl->connThreads) if (t.joinable()) t.join();
      impl->connThreads.clear(); }

    if (impl->dev) rtlsdr_close(impl->dev);
    impl->tcpSock = nullptr;             // RTL-TCP socket already closed above
    // Close our own dup last (rtlsdr_close/libusb don't own it). Kotlin's
    // UsbDeviceConnection.close() races us harmlessly now — it's a different fd.
    if (impl->usbFd >= 0) { ::close(impl->usbFd); impl->usbFd = -1; }
    delete impl;
    LOGI("local SDR stopped");
}

// ── Decoder-only sidecar (network backends) ───────────────────────────────────
int LocalSdrShim::startDecoderService(std::string& err) {
    std::lock_guard<std::mutex> life(g_lifecycle);
    if (p) { LOGI("stale shim found on decoder-service start — tearing down"); stopLocked(); }
    Impl* impl = new Impl();
    impl->decoderOnly = true;
    impl->sampleRate = 48000.0;            // decoders run at 48 kHz
    int chosen = -1;
    for (int port = 48050; port < 48100; port++) {   // above the local-SDR range
        try { impl->listener = net::listen("127.0.0.1", port); chosen = port; break; }
        catch (...) { impl->listener = nullptr; }
    }
    if (!impl->listener) { err = "could not bind localhost port"; delete impl; return -1; }
    impl->port = chosen;
    impl->serverRunning.store(true);
    impl->acceptThread = std::thread([impl]{ impl->acceptLoop(); });
    p = impl;
    LOGI("decoder service started: port=%d", chosen);
    return chosen;
}

void LocalSdrShim::feedDecoderPcm(const int16_t* pcm, int n, int rate) {
    if (!p || !p->decoderOnly || n < 2 || rate <= 0) return;
    // Upsample to the decoders' 48 kHz (linear interp), build a mono stereo_t
    // buffer (l=r) and feed the decoder + digital-spots paths.
    double ratio = 48000.0 / (double)rate;
    double srcStep = 1.0 / ratio;
    std::vector<stereo_t> buf;
    buf.reserve((size_t)(n * ratio) + 2);
    for (double s = 0; s < n - 1; s += srcStep) {
        int i = (int)s; double f = s - i;
        float v = (float)(((1.0 - f) * pcm[i] + f * pcm[i + 1]) / 32768.0);
        buf.push_back({ v, v });
    }
    if (buf.empty()) return;
    p->feedDecoder(buf.data(), (int)buf.size());
    p->feedSpots(buf.data(), (int)buf.size());
}

void LocalSdrShim::setDecoderFreq(double hz) {
    if (!p || hz <= 0) return;
    // Dial frequency for the sidecar: FT8 spots are emitted at audioFreq + offset,
    // so without this they'd land at the 100 MHz default (empty band, wrong freq).
    p->audioFreq.store(hz);
}

// ── Hardware controls ─────────────────────────────────────────────────────────
void LocalSdrShim::setGain(int gainTenthDb) {
    if (!p) return;
    if (p->useSpy()) {
        // No AGC in the protocol — "auto" has no wire representation, so mid-scale.
        p->lastGainTenthDb = gainTenthDb;
        uint32_t idx = gainTenthDb < 0
            ? (uint32_t)(p->spyGains.size() / 2)
            : spyserver::SpyServerClient::gainIndexForTenthDb(p->spyGains, gainTenthDb);
        const uint32_t maxIdx = p->spy->deviceInfo().maximumGainIndex;
        if (idx > maxIdx) idx = maxIdx;
        p->spy->setGainIndex(idx);
        LOGI("gain: index %u", idx);
        return;
    }
    if (p->useTcp()) {
        if (gainTenthDb < 0) p->sendTcpCmd(0x03, 0);
        else { p->sendTcpCmd(0x03, 1); p->sendTcpCmd(0x04, (uint32_t)gainTenthDb); }
        return;
    }
    if (!p->dev) return;
    if (gainTenthDb < 0) { rtlsdr_set_tuner_gain_mode(p->dev, 0); LOGI("gain: auto"); }
    else { rtlsdr_set_tuner_gain_mode(p->dev, 1); rtlsdr_set_tuner_gain(p->dev, gainTenthDb);
           LOGI("gain: %.1f dB", gainTenthDb / 10.0); }
}
void LocalSdrShim::setPpm(int ppm) {
    if (!p) return;
    if (p->useSpy()) return;   // no ppm setting in the SpyServer protocol

    if (p->useTcp()) { p->sendTcpCmd(0x05, (uint32_t)ppm); return; }
    if (!p->dev) return;
    rtlsdr_set_freq_correction(p->dev, ppm); LOGI("ppm: %d", ppm);
}
void LocalSdrShim::setBiasTee(bool on) {
    if (!p) return;
    if (p->useTcp()) { p->sendTcpCmd(0x0e, on ? 1 : 0); return; }
    if (!p->dev) return;
    rtlsdr_set_bias_tee(p->dev, on ? 1 : 0); LOGI("bias-tee: %d", on);
}
void LocalSdrShim::setAgc(bool on) {
    if (!p) return;
    if (p->useTcp()) { p->sendTcpCmd(0x08, on ? 1 : 0); return; }
    if (!p->dev) return;
    rtlsdr_set_agc_mode(p->dev, on ? 1 : 0); LOGI("agc: %d", on);
}
void LocalSdrShim::setDirectSampling(int mode) {
    if (!p) return;
    if (p->useTcp()) { p->sendTcpCmd(0x09, (uint32_t)mode); return; }
    if (!p->dev) return;
    rtlsdr_set_direct_sampling(p->dev, mode); LOGI("direct sampling: %d", mode);
}
void LocalSdrShim::setSquelch(bool on, float db) {
    if (!p) return;
    p->squelchOn.store(on); p->squelchDb.store(db);
    LOGI("squelch: %d @ %.1f dB", on, db);
}
void LocalSdrShim::setNR(bool on) {
    if (!p) return;
    p->nrOn.store(on);
    if (!on) { std::lock_guard<std::mutex> lk(p->nrMtx); if (p->nrEng) p->nrEng->reset(); }
    LOGI("audio NR: %d", on);
}
void LocalSdrShim::setNrStrength(float s) {
    if (!p) return;
    std::lock_guard<std::mutex> lk(p->nrMtx);
    if (!p->nrEng) p->nrEng = new AudioNR();
    p->nrEng->setStrength(s);
}
float LocalSdrShim::getNrCpu() { return p ? p->nrCpuPct.load() : 0.0f; }
void LocalSdrShim::setNotch(bool on) {
    if (!p) return;
    p->notchOn.store(on);
    if (!on) { std::lock_guard<std::mutex> lk(p->notchMtx); if (p->notchEng) p->notchEng->reset(); }
    LOGI("auto notch: %d", on);
}
void LocalSdrShim::setStereoEnabled(bool on) {
    if (!p) return;
    p->rx.setStereoEnabled(on);           // engine blends L-R out when off (-> mono)
    LOGI("stereo: %s", on ? "on" : "forced mono");
}
// Live spectrum frame rate. The power lever for a solar/battery-powered server:
// the engine skips the FFT work entirely (specStride_ = sampleRate/fps), and the
// spectrum bytes on the wire fall with it — so both the CPU and the Wi-Fi radio
// wind down. Audio is untouched, so a throttled server still sounds identical.
// (Contrast with the client's set_rate divisor, which only drops frames at SEND
// time: the FFTs are still computed, so it saves bandwidth and nothing else.)
void LocalSdrShim::setFftRate(double fps) {
    if (!p || fps <= 0) return;
    double mr = g_vsMaxFftRate.load();       // never exceed the server's own cap
    if (mr > 0 && fps > mr) fps = mr;
    p->fftRate = fps;
    // The engine runs at FFT_AVG× the EMIT rate — onSpectrum block-averages
    // FFT_AVG frames into each one it sends (see start(): rx.start(..., fftRate *
    // FFT_AVG, ...)). Pass the raw fps here and everything comes out 4× too slow.
    p->rx.setFftRate(fps * FFT_AVG);
    LOGI("fft rate: %.1f fps (engine %.1f)", fps, fps * FFT_AVG);
}
void LocalSdrShim::setSampleRate(double rate) {
    if (!p || rate <= 0) return;
    Impl* impl = p;
    const bool tcp = impl->useTcp();
    if (!tcp && !impl->dev) return;
    // Stop the IQ source + drain the DSP consumer BEFORE taking modeMtx (the
    // dspThread locks modeMtx per buffer, so holding it across the join would
    // deadlock). With both quiesced, the rtlsdr control transfer below runs on an
    // idle libusb and the engine rebuild has no concurrent rx.feed.
    if (tcp) { impl->tcpRunning.store(false); }
    else     { rtlsdr_cancel_async(impl->dev); }
    if (impl->rtlThread.joinable()) impl->rtlThread.join();
    impl->stopDspThread();
    std::lock_guard<std::recursive_mutex> lk(impl->modeMtx);
    uint32_t actual;
    if (tcp) {
        impl->sendTcpCmd(0x02, (uint32_t)rate);
        // rtl_tcp has no reply channel, so the server can't tell us what the device
        // really landed on. Compute it the way librtlsdr does — assuming we got
        // exactly what we asked for made the DSP resample against the wrong rate,
        // and the audio came out pitch-shifted.
        actual = (uint32_t)llround(rtlActualRate(rate));
    } else {
        rtlsdr_set_sample_rate(impl->dev, (uint32_t)rate);
        rtlsdr_reset_buffer(impl->dev);
        // The RTL rounds to a supported rate — use the ACTUAL for FFT/config or the
        // waterfall calibration drifts (signals land off their true freq).
        actual = rtlsdr_get_sample_rate(impl->dev);
    }
    impl->sampleRate = actual > 0 ? (double)actual : rate;
    impl->fftSize = fftSizeForRate(impl->sampleRate);

    impl->teardownAudio();
    impl->rx.stop();
    impl->startEngine();
    impl->buildAudio();
    { std::lock_guard<std::mutex> lk(impl->clientMtx); if (impl->specClient) impl->sendConfig(impl->specClient); }
    impl->startDspThread();
    if (tcp) { impl->tcpRunning.store(true); impl->rtlThread = std::thread([impl]{ impl->tcpReadLoop(); }); }
    else {
        const uint32_t bufLen = rtlBufLenForRate(impl->sampleRate);
        impl->rtlThread = std::thread([impl, bufLen]{
            vibeThreadName("vibe-rtl");
            rtlsdr_read_async(impl->dev, &Impl::asyncHandler, impl, 0, bufLen);
        });
    }
    LOGI("sample rate: %.0f (actual %u) fft=%d tcp=%d", rate, actual, impl->fftSize, tcp);
}
void LocalSdrShim::setDeemphasis(double tau) {
    std::lock_guard<std::mutex> life(g_lifecycle);
    if (!p) return;
    if (p->deempTau == tau) return;
    p->deempTau = tau;
    p->rx.setDeemphasis(tau);   // engine applies 0/50us/75us on the next rebuild
    LOGI("deemphasis: %.0f us", tau * 1e6);
}
LocalSdrShim::NetStatus LocalSdrShim::getNetStatus() {
    NetStatus s;
    if (!p || !(p->useTcp() || p->useSpy())) return s;   // USB path: nothing to report
    s.tcp = true;
    s.stalls         = p->netStalls.load(std::memory_order_relaxed);
    s.droppedSamples = p->iqDroppedSamples.load(std::memory_order_relaxed);
    if (p->useSpy()) {
        s.spy        = true;
        s.canControl = p->spy->canControl();     // refreshed from every CLIENT_SYNC
        s.closed     = p->spyClosed.load();
    }
    double rate = p->sampleRate > 0 ? p->sampleRate : 1.0;
    std::lock_guard<std::mutex> lk(p->iqMtx);
    s.bufferedMs = (uint32_t)(p->iqQueuedSamples * 1000.0 / rate);
    return s;
}

LocalSdrShim::VibeServerStatus LocalSdrShim::getVibeServerStatus() {
    VibeServerStatus s;
    s.compressed = g_vsCompressAudio.load();
    { std::lock_guard<std::mutex> lk(g_vsMtx); s.pinEnabled = !g_vsSecret.empty(); }
    if (!p) return s;
    s.running   = g_serveOnLan.load();
    s.fftRate   = p->fftRate;
    s.bandwidthHz = p->vfoBwHz.load();
    s.sampleRate  = p->sampleRate;
    { std::lock_guard<std::mutex> lk(p->clientMtx);
      auto sp = p->specClient, au = p->audioClient;
      s.clientConnected = (sp && sp->isOpen()) || (au && au->isOpen());
      if (au && au->isOpen()) s.clientAddr = au->peerAddress();
      else if (sp && sp->isOpen()) s.clientAddr = sp->peerAddress(); }
    // Byte rates = delta since the previous poll (the sharing screen polls ~1-2 s).
    uint64_t spec = p->vsSpecBytes.load(std::memory_order_relaxed);
    uint64_t aud  = p->vsAudioBytes.load(std::memory_order_relaxed);
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lk(p->vsRateMtx);
    if (p->vsRateLastMs > 0 && now > p->vsRateLastMs) {
        double dt = (now - p->vsRateLastMs) / 1000.0;
        s.specBytesPerSec  = (spec - p->vsRateLastSpec) / dt;
        s.audioBytesPerSec = (aud  - p->vsRateLastAudio) / dt;
    }
    p->vsRateLastSpec = spec; p->vsRateLastAudio = aud; p->vsRateLastMs = now;
    return s;
}

std::vector<int> LocalSdrShim::getTunerGains() {
    std::vector<int> out;
    if (!p) return out;
    // SpyServer transmits a bare gain INDEX and never the dB values, so the UI has
    // no table unless we supply one. Without this the gain slider has nothing to
    // offer and gain looks uncontrollable. (Stock clients just show a 0..29 dial.)
    if (p->useSpy()) return p->spyGains;
    if (p->useTcp()) return p->tcpGains;     // rtl_tcp header has no values → R820T table
    if (!p->dev) return out;
    int n = rtlsdr_get_tuner_gains(p->dev, nullptr);
    if (n <= 0) return out;
    out.resize(n);
    rtlsdr_get_tuner_gains(p->dev, out.data());
    return out;
}

bool LocalSdrShim::isRunning() const { return p != nullptr; }

} // namespace vibe
